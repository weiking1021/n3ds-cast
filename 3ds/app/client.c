#include "client.h"
#include <stdio.h>
#include <string.h>
#include "resident_image.h"
extern Result nativeSystemMemory(u32 *out, u32 address, u32 bytes, u32 operation, u32 permissions);

static Result system_resource_limit(Handle *out) {
    // Resource limit providers must outlive their users on the retail kernel.
    // Borrow PM's persistent SYSTEM category; never edit its limits or borrow
    // the launcher's APPLICATION limit, which disappears when it closes.
    u32 pids[80];
    s32 count = 0;
    Result rc = svcGetProcessList(&count, pids, 80);
    if (rc != 0) {
        return rc;
    }
    if (count > 80) {
        count = 80;
    }
    for (int i = 0; i < count; i++) {
        Handle process = 0, limit = 0;
        s64 title = 0;
        if (svcOpenProcess(&process, pids[i]) != 0) {
            continue;
        }
        rc = svcGetProcessInfo(&title, process, 0x10001);
        if (rc == 0 && ((uint64_t)title & ~0x20000000ULL) == 0x0004013000002E02ULL) {
            rc = svcGetResourceLimit(&limit, process);
            svcCloseHandle(process);
            if (rc != 0) {
                return rc;
            }
            ResourceLimitType names[] = {RESLIMIT_PRIORITY, RESLIMIT_COMMIT,       RESLIMIT_THREAD,
                                         RESLIMIT_EVENT,    RESLIMIT_SHAREDMEMORY, RESLIMIT_ADDRESSARBITER};
            s64 maximum[6] = {0}, current[6] = {0};
            rc = svcGetResourceLimitLimitValues(maximum, limit, names, 6);
            if (!rc) {
                rc = svcGetResourceLimitCurrentValues(current, limit, names, 6);
            }
            if (!rc &&
                (maximum[0] > 0x2E || maximum[1] - current[1] < NATIVE_MEMORY_LIMIT || maximum[2] - current[2] < 4 ||
                 maximum[3] - current[3] < 1 || maximum[4] - current[4] < 1 || maximum[5] - current[5] < 1)) {
                rc = (Result)-1;
            }
            if (rc != 0) {
                svcCloseHandle(limit);
                return rc;
            }
            *out = limit;
            return 0;
        }
        svcCloseHandle(process);
    }
    return (Result)-1;
}

Result native_request(Handle client, unsigned operation, const NativeStartConfig *start, NativeStatus *status) {
    if (operation < NATIVE_STATUS || operation > NATIVE_OSD_OFF || !status ||
        (operation == NATIVE_START && !native_start_valid(start))) {
        return (Result)0xD8E007ED;
    }
    u32 *buffer = getThreadCommandBuffer();
    buffer[0] = IPC_MakeHeader(operation, start ? sizeof(*start) / 4 : 0, 0);
    if (start) {
        memcpy(buffer + 1, start, sizeof(*start));
    }
    Result rc = svcSendSyncRequest(client);
    if (rc != 0) {
        return rc;
    }
    if (buffer[0] != IPC_MakeHeader(operation, 1 + sizeof(*status) / 4, 0)) {
        return (Result)0xD8E007ED;
    }
    rc = buffer[1];
    if (rc != 0) {
        return rc;
    }
    memcpy(status, buffer + 2, sizeof(*status));
    if (status->magic != NATIVE_MAGIC || status->version != NATIVE_VERSION) {
        return (Result)0xD8E007ED;
    }
    status->message[sizeof(status->message) - 1] = 0;
    return 0;
}
Result native_connect(Handle *client, NativeStatus *status) {
    *client = 0;
    Handle connection = 0;
    Result rc = svcConnectToPort(&connection, NATIVE_PORT);
    if (rc == 0) {
        *client = connection;
        rc = native_request(*client, NATIVE_STATUS, NULL, status);
        if (rc != 0) {
            svcCloseHandle(*client);
            *client = 0;
        }
    }
    return rc;
}
#define SYSTEM_REGION_START 0x08000000u // the SYSTEM memory range searched for room for the resident image
#define SYSTEM_REGION_END 0x10000000u
#define LUMA_SYSTEM_INFO_TYPE 0x10000 // svcGetSystemInfo type that only Luma's extended services answer
#define READY_ATTEMPTS 200            // the new process gets 200 x 10 ms to open its control port
#define READY_POLL_NS 10000000LL
#define RESIDENT_PRIORITY 0x30
#define RESIDENT_STACK_SIZE 0x10000
#define RESIDENT_AFFINITY_MASK 2 // the resident process runs on the system core (core 1)
#define RESIDENT_IDEAL_CORE 1

// Everything that has to be released again, whichever step of the launch fails.
typedef struct {
    u32 staging;
    Handle codeset, process, limits;
    bool running;
    const char *stage; // what was being done, for the error message
} Launch;

// Finds free SYSTEM memory big enough for the resident image. Never replaces an existing process or edits the
// foreground application's instructions.
static Result find_free_region(u32 *address) {
    u32 candidate = SYSTEM_REGION_START;
    while (candidate < SYSTEM_REGION_END) {
        MemInfo memory = {0};
        PageInfo page = {0};
        Result rc = svcQueryMemory(&memory, &page, candidate);
        if (rc != 0) {
            return rc;
        }
        uint64_t end = (uint64_t)memory.base_addr + memory.size;
        if (end <= candidate || end > 0x100000000ULL) {
            return (Result)-1;
        }
        if (memory.state == MEMSTATE_FREE && end - candidate >= NATIVE_IMAGE_SIZE &&
            (uint64_t)candidate + NATIVE_IMAGE_SIZE <= SYSTEM_REGION_END) {
            *address = candidate;
            return 0;
        }
        candidate = end > SYSTEM_REGION_END ? SYSTEM_REGION_END : (u32)end;
    }
    return (Result)-1;
}

// Copies the embedded image into SYSTEM memory and creates the (not yet started) process from it.
static Result create_process(Launch *launch) {
    launch->stage = "system memory allocation";
    u32 candidate = 0;
    Result rc = find_free_region(&candidate);
    if (rc != 0) {
        return rc;
    }
    u32 allocation = 0;
    rc = nativeSystemMemory(&allocation, candidate, NATIVE_IMAGE_SIZE, MEMOP_ALLOC | MEMOP_REGION_SYSTEM,
                            MEMPERM_READWRITE);
    if (rc != 0) {
        return rc;
    }
    launch->staging = allocation;
    memcpy((void *)launch->staging, native_image, NATIVE_IMAGE_SIZE);
    CodeSetHeader header = {0};
    memcpy(header.name, "n3d_cast", 8);
    header.program_id = 0;
    header.text_addr = NATIVE_TEXT_ADDRESS;
    header.text_size = header.text_size_total = NATIVE_TEXT_PAGES;
    header.ro_addr = NATIVE_RO_ADDRESS;
    header.ro_size = header.ro_size_total = NATIVE_RO_PAGES;
    header.rw_addr = NATIVE_RW_ADDRESS;
    header.rw_size = NATIVE_RW_INIT_PAGES;
    header.rw_size_total = NATIVE_RW_TOTAL_PAGES;
    launch->stage = "codeset creation";
    Handle created = 0;
    rc = svcCreateCodeSet(&created, &header, launch->staging, launch->staging + NATIVE_RO_OFFSET,
                          launch->staging + NATIVE_RW_OFFSET);
    if (rc != 0) {
        return rc;
    }
    launch->codeset = created;
    u32 capabilities[] = {
        0xFC000000 | (osGetKernelVersion() >> 16),
        0xFF81F000,
        0xFF81F600, // VRAM pixels writable for the bounded bottom-screen OSD.
        0xFF81EC00,
        0xFF81F000, // ARM11 I/O map used only for display register reads.
        0xFF91FF40,
        0xFF91FF80, // Read-only DSP DRAM; no service ownership or writes.
        0xFF002209, // SYSTEM memory region, debug permission.
        0xFE000100, // 256 handles, independently limited below.
        0xF0FFFFFF,
        0xF1FFFFFF,
        0xF2FFFFFF,
        0xF3FFFFFF,
        0xF4FFFFFF,
        0xF5FFFFFF,
        0xF6FFFFFF,
        0xF7FFFFFF,
    };
    launch->stage = "process creation";
    rc = svcCreateProcess(&created, launch->codeset, capabilities, sizeof(capabilities) / sizeof(capabilities[0]));
    if (rc != 0) {
        return rc;
    }
    launch->process = created;
    return 0;
}

// Resource budget and scheduling of the new process, then start it.
static Result configure_and_start(Launch *launch) {
    launch->stage = "SYSTEM resource budget";
    Handle created = 0;
    Result rc = system_resource_limit(&created);
    if (rc != 0) {
        return rc;
    }
    launch->limits = created;
    rc = svcSetProcessResourceLimits(launch->process, launch->limits);
    if (rc != 0) {
        return rc;
    }
    u8 affinity = RESIDENT_AFFINITY_MASK;
    launch->stage = "system core scheduling";
    rc = svcSetProcessAffinityMask(launch->process, &affinity, 2);
    if (rc != 0) {
        return rc;
    }
    rc = svcSetProcessIdealProcessor(launch->process, RESIDENT_IDEAL_CORE);
    if (rc != 0) {
        return rc;
    }
    StartupInfo startup = {.priority = RESIDENT_PRIORITY, .stack_size = RESIDENT_STACK_SIZE};
    launch->stage = "resident startup";
    rc = svcRun(launch->process, &startup);
    if (rc != 0) {
        return rc;
    }
    launch->running = true;
    return 0;
}

// Waits for the new process to open its control port (or to die).
static Result wait_until_ready(Launch *launch, Handle *client, NativeStatus *status) {
    launch->stage = "resident readiness";
    Result rc = 0;
    for (unsigned attempt = 0; attempt < READY_ATTEMPTS; attempt++) {
        rc = native_connect(client, status);
        if (rc == 0) {
            break;
        }
        if (svcWaitSynchronization(launch->process, 0) == 0) {
            break;
        }
        svcSleepThread(READY_POLL_NS);
    }
    return rc;
}

// Gives back everything the launch took. The process is only terminated when the launch failed.
static void release_launch(Launch *launch, Result rc) {
    if (rc != 0 && launch->process && launch->running) {
        svcTerminateProcess(launch->process);
    }
    if (launch->process) {
        svcCloseHandle(launch->process);
    }
    if (launch->limits) {
        svcCloseHandle(launch->limits);
    }
    if (launch->codeset) {
        svcCloseHandle(launch->codeset);
    }
    if (launch->staging) {
        u32 unused = 0;
        nativeSystemMemory(&unused, launch->staging, NATIVE_IMAGE_SIZE, MEMOP_FREE | MEMOP_REGION_SYSTEM, 0);
    }
}

Result native_launch(Handle *client, NativeStatus *status, char *error, size_t capacity) {
    Result rc = native_connect(client, status);
    if (rc == 0) {
        return 0;
    }
    if (R_SUMMARY(rc) != RS_NOTFOUND) {
        snprintf(error, capacity, "Control service incompatible; reboot console: %08lX", (unsigned long)rc);
        return rc;
    }
    s64 luma = 0;
    if (svcGetSystemInfo(&luma, LUMA_SYSTEM_INFO_TYPE, 0) != 0) {
        snprintf(error, capacity, "Luma extended services unavailable");
        return (Result)-1;
    }
    Launch launch = {.stage = "system memory allocation"};
    rc = create_process(&launch);
    if (rc == 0) {
        rc = configure_and_start(&launch);
    }
    if (rc == 0) {
        rc = wait_until_ready(&launch, client, status);
    }
    release_launch(&launch, rc);
    if (rc != 0) {
        snprintf(error, capacity, "%s: %08lX", launch.stage, (unsigned long)rc);
    }
    return rc;
}
