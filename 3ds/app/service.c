#include "service_internal.h"

#define THREAD_STACK_SIZE 0x10000
#define START_WAIT_TRIES 25 // a new thread must report in within 25 x 10 ms
#define START_WAIT_NS 10000000LL
#define SUPERVISE_SLEEP_NS 20000000LL // how often the main loop looks at the request flags
#define MEMORY_FAILURE_SLEEP_NS 50000000LL
#define JOIN_TIMEOUT_NS 100000000LL // shutdown waits this long for each helper thread

// Independent process: no APT/NS IPC, applet registration, HID or display ownership.
extern char *fake_heap_start, *fake_heap_end;
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;
static Result srv_result;
void __appInit(void) {
    srv_result = srvInit();
}
void __appExit(void) {
    if (!srv_result) {
        srvExit();
    }
}
__attribute__((noreturn)) void __libctru_exit(int code) {
    (void)code;
    // This process was started by svcRun, not the Homebrew Launcher. Never
    // follow a 3dsx loader return address or release memory under a live thread.
    svcExitProcess();
}
void __system_allocateHeaps(void) {
    __ctru_heap_size = 6 * 1024 * 1024;
    __ctru_linear_heap_size = 0;
    __ctru_linear_heap = 0;
    Result rc = svcControlMemory(&__ctru_heap, 0x08000000, 0, __ctru_heap_size, MEMOP_ALLOC | MEMOP_REGION_SYSTEM,
                                 MEMPERM_READWRITE);
    if (rc != 0) {
        svcExitProcess();
    }
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + __ctru_heap_size;
    mappableInit(0x10000000, 0x14000000);
}

static Result keep_network_available(void) {
    Result rc = pmAppInit();
    if (rc != 0) {
        return rc;
    }
    // A separately created process is not an APT application. Explicitly keep
    // these standard modules loaded when the launcher's dependencies are freed.
    // PM's LaunchTitle clears AUTOLOADED for an already-running sysmodule.
    const uint64_t titles[] = {0x0004013000002E02ULL, 0x0004013000002F02ULL};
    for (unsigned i = 0; i < 2 && !rc; i++) {
        FS_ProgramInfo info = {0};
        info.programId = titles[i];
        info.mediaType = MEDIATYPE_NAND;
        rc = PMAPP_LaunchTitle(&info, 0);
    }
    pmAppExit();
    return rc;
}
static Thread create_worker(int core) {
    unsigned token = atomic_fetch_add(&worker_token, 1) + 1;
    atomic_store(&chosen_token, token);
    atomic_store(&worker_started, false);
    Thread thread = threadCreate(stream_worker, (void *)(uintptr_t)token, THREAD_STACK_SIZE, 0x2F, core, false);
    if (!thread) {
        return NULL;
    }
    for (unsigned i = 0; i < START_WAIT_TRIES && !atomic_load(&worker_started); i++) {
        svcSleepThread(START_WAIT_NS);
    }
    if (!atomic_load(&worker_started)) {
        // Never join/free a thread that has not run; bumping the token makes it
        // return immediately if its core ever schedules it.
        atomic_fetch_add(&worker_token, 1);
        return NULL;
    }
    return thread;
}
static Thread start_worker(unsigned preference) {
    // The sender always runs on core 1; the chosen core is for the encoder thread,
    // which the worker starts, verifies and (if core 2 is unusable) falls back from.
    atomic_store(&encoder_core_request, preference == NATIVE_CORE_1 ? 1 : 2);
    Thread thread = create_worker(1);
    if (!thread) {
        thread = create_worker(-1);
    }
    return thread;
}
static void stop_worker(Thread *worker) {
    if (!*worker) {
        return;
    }
    atomic_fetch_add(&worker_token, 1);
    threadJoin(*worker, U64_MAX);
    threadFree(*worker);
    *worker = NULL;
}

// Starts the audio sampler: highest priority of our threads, because it must catch every 4.9 ms DSP frame even while
// TLS/JPEG work saturates a core (it only does tiny reads and sleeps). It must not share a core with Wi-Fi/SOC
// interrupt work, which delayed polls past the DSP's frame, so core 2 (New 3DS) is preferred. Each thread is verified
// to run; otherwise the next priority (lower number = higher; the kernel grants only some), then the default core.
static Thread start_sampler(void) {
    static const int priorities[] = {0x18, 0x1C, 0x20, 0x24, 0x28, 0x2C};
    Thread sampler = NULL;
    for (int core = 2; core >= -1 && !sampler; core -= 3) {
        for (unsigned p = 0; p < sizeof(priorities) / sizeof(priorities[0]) && !sampler; p++) {
            unsigned gen = atomic_fetch_add(&audio_gen, 1) + 1;
            atomic_store(&audio_started, false);
            Thread thread =
                threadCreate(audio_worker, (void *)(uintptr_t)gen, THREAD_STACK_SIZE, priorities[p], core, false);
            if (!thread) {
                continue; // priority not permitted: try a lower one
            }
            for (unsigned i = 0; i < START_WAIT_TRIES && !atomic_load(&audio_started); i++) {
                svcSleepThread(START_WAIT_NS);
            }
            if (atomic_load(&audio_started)) {
                sampler = thread;
                break;
            }
            atomic_fetch_add(&audio_gen, 1); // never join/free a thread that has not run
            break;                           // this core does not run it: next core
        }
    }
    return sampler;
}

static bool allocate_buffers(void) {
    soc_buffer = memalign(0x1000, SOC_BUFFER_SIZE);
    raw = memalign(0x1000, RAW_BUFFER_SIZE);
    jpeg = malloc(NATIVE_JPEG_MAX);
    video[0].bgr = calloc(400 * 240, 3);
    video[1].bgr = calloc(400 * 240, 3);
    b64[0] = malloc(NATIVE_B64_MAX);
    b64[1] = malloc(NATIVE_B64_MAX);
    video[0].base64 = b64[0];
    video[1].base64 = b64[1];
    // Partial updates need this text pool per screen; without it the encoder just sends whole screens.
    video[0].rect_b64 = malloc(NATIVE_B64_MAX);
    video[1].rect_b64 = malloc(NATIVE_B64_MAX);
    bool buffers = soc_buffer && raw && video[0].bgr && video[1].bgr && jpeg && b64[0] && b64[1];
    for (unsigned s = 0; s < 2; s++) {
        for (unsigned i = 0; i < 2; i++) {
            slots[s].img[i] = malloc(NATIVE_B64_MAX);
            buffers = buffers && slots[s].img[i];
        }
    }
    return buffers;
}

static void free_buffers(void) {
    free(raw);
    free(video[0].bgr);
    free(video[1].bgr);
    free(jpeg);
    free(b64[0]);
    free(b64[1]);
    free(soc_buffer);
    for (unsigned s = 0; s < 2; s++) {
        for (unsigned i = 0; i < 2; i++) {
            free(slots[s].img[i]);
        }
    }
    free(video[0].rect_b64);
    free(video[1].rect_b64);
}

// Runs the stream worker whenever a stream is wanted, restarting it when the requested encoder core changes.
static void supervise_worker(bool buffers) {
    Thread worker = NULL;
    unsigned worker_preference = ~0u;
    while (!atomic_load(&quitting)) {
        if (!buffers) {
            svcSleepThread(MEMORY_FAILURE_SLEEP_NS);
            continue;
        }
        unsigned preference;
        LightLock_Lock(&state_lock);
        preference = requested_core;
        LightLock_Unlock(&state_lock);
        if (atomic_load(&wanted) && (!worker || preference != worker_preference)) {
            stop_worker(&worker);
            worker = start_worker(preference);
            worker_preference = preference;
            if (!worker) {
                request_failed(atomic_load(&generation), "Could not start encoder thread");
            }
        }
        svcSleepThread(SUPERVISE_SLEEP_NS);
    }
    stop_worker(&worker);
}

static void join_thread(Thread thread) {
    if (threadJoin(thread, JOIN_TIMEOUT_NS) == 0) {
        threadFree(thread);
    }
}

static void fail_startup(void) {
    atomic_store(&quitting, true);
    svcSignalEvent(quit_event);
}

int main(void) {
    if (srv_result != 0) {
        return 1;
    }
    // Complete dependency lifetime setup before publishing the ready port.
    if (keep_network_available() != 0) {
        return 1;
    }
    LightLock_Init(&state_lock);
    LightLock_Init(&audio_lock);
    LightLock_Init(&enc_lock);
    status.magic = NATIVE_MAGIC;
    status.version = NATIVE_VERSION;
    svcGetProcessId(&status.pid, CUR_PROCESS_HANDLE);
    set_status(NATIVE_IDLE, "Ready; no game hooks or plugins");
    Handle dummy = 0;
    if (svcCreateEvent(&quit_event, RESET_STICKY) != 0 || svcCreatePort(&server_port, &dummy, NATIVE_PORT, 1) != 0) {
        return 1;
    }
    svcCloseHandle(dummy);
    Thread controller = threadCreate(control_thread, NULL, THREAD_STACK_SIZE, 0x30, -1, false);
    if (!controller) {
        return 1;
    }
    Thread sampler = start_sampler();
    if (!sampler) {
        fail_startup();
        return 1;
    }
    Thread osd = threadCreate(overlay_worker, NULL, THREAD_STACK_SIZE, 0x2E, -1, false);
    if (!osd) {
        fail_startup();
        return 1;
    }
    bool buffers = allocate_buffers();
    if (!buffers) {
        set_status(NATIVE_ERROR, "Not enough background memory");
    }
    supervise_worker(buffers);
    if (ssl_ok) {
        sslcExit();
    }
    if (soc_ok) {
        socExit();
    }
    svcSignalEvent(quit_event);
    join_thread(controller);
    join_thread(sampler);
    join_thread(osd);
    svcCloseHandle(server_port);
    svcCloseHandle(quit_event);
    free_buffers();
    return 0;
}
