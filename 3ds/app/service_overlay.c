#include "service_internal.h"

static NativeOverlay overlay;
void overlay_worker(void *unused) {
    (void)unused;
    while (!atomic_load(&quitting)) {
        NativeStatus snapshot;
        LightLock_Lock(&state_lock);
        snapshot = status;
        LightLock_Unlock(&state_lock);
        unsigned state = 1;
        if (atomic_load(&wanted) && snapshot.overlay_enabled) {
            char label[32];
            unsigned r = snapshot.capture_ms > 999 ? 999 : snapshot.capture_ms;
            unsigned j = snapshot.encode_ms > 999 ? 999 : snapshot.encode_ms;
            unsigned t = snapshot.send_ms > 999 ? 999 : snapshot.send_ms;
            unsigned fps = snapshot.fps > 99 ? 99 : snapshot.fps;
            snprintf(label, sizeof(label), "R%3u J%3u T%3u %2uFPS", r, j, t, fps);
            state = native_overlay_update(&overlay, label);
        } else if (overlay.capture.game || overlay.buffers[0].pixels || overlay.buffers[1].pixels) {
            native_overlay_close(&overlay);
        }
        LightLock_Lock(&state_lock);
        status.overlay_state = state;
        status.overlay_error = (uint32_t)overlay.error;
        LightLock_Unlock(&state_lock);
        svcSleepThread(snapshot.overlay_enabled && atomic_load(&wanted) ? 8000000 : 50000000);
    }
    native_overlay_close(&overlay);
}
