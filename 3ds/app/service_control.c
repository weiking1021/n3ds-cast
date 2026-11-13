#include "service_internal.h"

void control_thread(void *unused) {
    (void)unused;
    Handle client = 0, reply = 0;
    while (!atomic_load(&quitting)) {
        Handle handles[] = {quit_event, server_port, client};
        s32 index = -1;
        u32 *cmd = getThreadCommandBuffer();
        if (!reply) {
            cmd[0] = 0xFFFF0000;
        }
        Result rc = svcReplyAndReceive(&index, handles, client ? 3 : 2, reply);
        reply = 0;
        if (rc == (Result)0xC920181A) {
            if (client) {
                svcCloseHandle(client);
            }
            client = 0;
            continue;
        }
        if (rc != 0) {
            break;
        }
        if (index == 0) {
            break;
        }
        if (index == 1) {
            if (client) {
                svcCloseHandle(client);
            }
            client = 0;
            Handle accepted = 0;
            if (svcAcceptSession(&accepted, server_port) == 0) {
                client = accepted;
            }
            continue;
        }
        if (index != 2) {
            continue;
        }
        unsigned operation = cmd[0] >> 16;
        Result result = 0;
        if (operation == NATIVE_START && cmd[0] == IPC_MakeHeader(NATIVE_START, sizeof(NativeStartConfig) / 4, 0)) {
            NativeStartConfig start;
            memcpy(&start, cmd + 1, sizeof(start));
            if (!native_start_valid(&start)) {
                result = (Result)0xD8E007ED;
            } else {
                LightLock_Lock(&state_lock);
                requested = start.cast;
                requested_core = start.cpu_core;
                requested_source = start.capture_source;
                atomic_store(&cfg_audio_enabled, start.cast.audio_enabled ? 1u : 0u);
                atomic_store(&cfg_audio_kbps, (unsigned)start.cast.audio_kbps);
                status.frames = 0;
                status.rejected = 0;
                status.title_id = 0;
                publish_title(0);
                status.audio_packets = status.audio_dropped = status.fps = 0;
                atomic_fetch_add(&generation, 1);
                atomic_store(&wanted, true);
                status.state = NATIVE_CONNECTING;
                snprintf(status.message, sizeof(status.message), "Preparing direct Google TV stream");
                LightLock_Unlock(&state_lock);
            }
        } else if (cmd[0] == IPC_MakeHeader(operation, 0, 0)) {
            if (operation == NATIVE_STOP) {
                atomic_store(&wanted, false);
                set_status(NATIVE_IDLE, "Stopping stream...");
            } else if (operation == NATIVE_EXIT) {
                atomic_store(&wanted, false);
                atomic_store(&quitting, true);
            } else if (operation == NATIVE_OSD_ON || operation == NATIVE_OSD_OFF) {
                LightLock_Lock(&state_lock);
                status.overlay_enabled = operation == NATIVE_OSD_ON;
                LightLock_Unlock(&state_lock);
            } else if (operation != NATIVE_STATUS) {
                result = (Result)0xD8E007ED;
            }
        } else {
            result = (Result)0xD8E007ED;
        }
        NativeStatus snapshot;
        LightLock_Lock(&state_lock);
        snapshot = status;
        LightLock_Unlock(&state_lock);
        cmd[0] = IPC_MakeHeader(operation, 1 + sizeof(snapshot) / 4, 0);
        cmd[1] = result;
        memcpy(cmd + 2, &snapshot, sizeof(snapshot));
        reply = client;
        if (atomic_load(&quitting)) {
            // Send the final reply without waiting for another request.
            Handle event = quit_event;
            s32 ignored;
            svcSignalEvent(event);
            svcReplyAndReceive(&ignored, &event, 1, reply);
            reply = 0;
        }
    }
    if (client) {
        svcCloseHandle(client);
    }
    atomic_store(&wanted, false);
    atomic_store(&quitting, true);
}
