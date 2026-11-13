// Tests the real control thread and request state (service_control.c, service_state.c) against a scripted client
// session: no game, network or GPU activity. test.sh compiles it with -D__3DS__ -Itest_stubs.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "service_internal.h"

#define PORT_HANDLE 2
#define EVENT_HANDLE 1
#define SESSION_HANDLE 3

static u32 buffer[128];
static unsigned sequence, reply_op;

// ---- the kernel calls the control thread makes ----
void LightLock_Init(LightLock *lock) {
    *lock = 0;
}
void LightLock_Lock(LightLock *lock) {
    assert(!*lock); // never locked twice
    *lock = 1;
}
int LightLock_TryLock(LightLock *lock) {
    if (*lock) {
        return 1;
    }
    *lock = 1;
    return 0;
}
void LightLock_Unlock(LightLock *lock) {
    assert(*lock);
    *lock = 0;
}
u32 *getThreadCommandBuffer(void) {
    return buffer;
}
Result svcAcceptSession(Handle *out, Handle port) {
    assert(port == PORT_HANDLE);
    *out = SESSION_HANDLE;
    return 0;
}
Result svcCloseHandle(Handle handle) {
    assert(handle == SESSION_HANDLE);
    return 0;
}
Result svcSignalEvent(Handle handle) {
    assert(handle == EVENT_HANDLE);
    return 0;
}
void svcSleepThread(s64 nanoseconds) {
    (void)nanoseconds;
}
// The scripted client: connect, a command of the wrong length, an unknown command, START, STATUS, STOP, the overlay
// on and off, EXIT. Every reply the thread sends is checked on the way.
Result svcReplyAndReceive(s32 *index, const Handle *handles, s32 count, Handle reply) {
    assert(handles[0] == EVENT_HANDLE);
    if (count == 1) {
        assert(reply == SESSION_HANDLE && atomic_load(&quitting));
        *index = 0;
        return 0;
    }
    if (reply) {
        assert(reply == SESSION_HANDLE && buffer[0] == IPC_MakeHeader(reply_op, 1 + sizeof(NativeStatus) / 4, 0));
        if (sequence == 2 || sequence == 3) {
            assert(buffer[1] != 0 && !atomic_load(&wanted));
        } else {
            assert(buffer[1] == 0);
        }
    }
    *index = sequence ? 2 : 1;
    switch (sequence++) {
        case 0:
            assert(count == 2); // connect
            break;
        case 1: // wrong length
            buffer[0] = IPC_MakeHeader(NATIVE_START, 0, 0);
            reply_op = NATIVE_START;
            break;
        case 2: // wrong command
            buffer[0] = IPC_MakeHeader(99, 0, 0);
            reply_op = 99;
            break;
        case 3: {
            NativeStartConfig start = {0};
            CastConfig *cfg = &start.cast;
            strcpy(cfg->tv_ip, "192.168.0.4");
            strcpy(cfg->app_id, "01B7A380");
            cfg->jpeg_quality = 50;
            cfg->target_fps = 15;
            cfg->audio_enabled = 1;
            cfg->audio_kbps = 64;
            start.cpu_core = 2;
            start.capture_source = 1;
            buffer[0] = IPC_MakeHeader(NATIVE_START, sizeof(start) / 4, 0);
            memcpy(buffer + 1, &start, sizeof(start));
            reply_op = NATIVE_START;
            break;
        }
        case 4:
            assert(atomic_load(&wanted) && atomic_load(&generation) == 1 && requested.audio_enabled &&
                   requested_core == 2 && requested_source == 1);
            buffer[0] = IPC_MakeHeader(NATIVE_STATUS, 0, 0);
            reply_op = NATIVE_STATUS;
            break;
        case 5:
            buffer[0] = IPC_MakeHeader(NATIVE_STOP, 0, 0);
            reply_op = NATIVE_STOP;
            break;
        case 6:
            assert(!atomic_load(&wanted) && status.state == NATIVE_IDLE);
            buffer[0] = IPC_MakeHeader(NATIVE_OSD_ON, 0, 0);
            reply_op = NATIVE_OSD_ON;
            break;
        case 7:
            assert(status.overlay_enabled);
            buffer[0] = IPC_MakeHeader(NATIVE_OSD_OFF, 0, 0);
            reply_op = NATIVE_OSD_OFF;
            break;
        case 8:
            assert(!status.overlay_enabled);
            buffer[0] = IPC_MakeHeader(NATIVE_EXIT, 0, 0);
            reply_op = NATIVE_EXIT;
            break;
        default:
            assert(0);
    }
    return 0;
}

int main(void) {
    quit_event = EVENT_HANDLE;
    server_port = PORT_HANDLE;
    LightLock_Init(&state_lock);
    status.magic = NATIVE_MAGIC;
    status.version = NATIVE_VERSION;
    control_thread(NULL);
    assert(sequence == 9 && atomic_load(&quitting) && !atomic_load(&wanted) && !state_lock);
    assert(!strcmp(requested.tv_ip, "192.168.0.4"));
    // Cancellation and failure reporting of the request state.
    atomic_store(&quitting, false);
    atomic_store(&wanted, true);
    atomic_store(&generation, 4);
    atomic_store(&active_generation, 4);
    assert(!cancelled(NULL));
    atomic_store(&generation, 5);
    assert(cancelled(NULL));
    request_failed(4, "old failure");
    assert(atomic_load(&wanted));
    atomic_store(&active_generation, 5);
    assert(!cancelled(NULL));
    request_failed(5, "new failure");
    assert(!atomic_load(&wanted) && status.state == NATIVE_ERROR && !strcmp(status.message, "new failure"));
    request_failed(5, "cancelled transfer");
    assert(status.state == NATIVE_IDLE);
    puts("PASS: actual resident IPC start/status/stop/shutdown, malformed command rejection, config snapshot locking, "
         "game audio config preserved, final reply and disconnect");
}
