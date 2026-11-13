#include "service_internal.h"

LightLock state_lock;
NativeStatus status;
CastConfig requested;
atomic_bool wanted = false, quitting = false;
// The audio sampler must never wait for a lock (a preempted holder made it miss DSP
// ticks): it reads these atomics and only TRY-locks shared structures.
atomic_uint cfg_audio_enabled = 0, cfg_audio_kbps = 64, game_hi = 0, game_lo = 0;
atomic_uint generation = 0;
atomic_uint active_generation = 0;
Handle quit_event, server_port;
uint32_t requested_core = NATIVE_CORE_AUTO, requested_source = NATIVE_SOURCE_GAME;
atomic_uint worker_token = 0, chosen_token = 0;
atomic_bool worker_started = false;
bool soc_ok = false, ssl_ok = false;
void *soc_buffer;
bool cancelled(void *unused) {
    (void)unused;
    return !atomic_load(&wanted) || atomic_load(&quitting) ||
           atomic_load(&generation) != atomic_load(&active_generation) ||
           atomic_load(&chosen_token) != atomic_load(&worker_token);
}
void set_status(unsigned state, const char *text) {
    LightLock_Lock(&state_lock);
    status.state = state;
    snprintf(status.message, sizeof(status.message), "%s", text);
    LightLock_Unlock(&state_lock);
}
void request_failed(unsigned expected, const char *message) {
    LightLock_Lock(&state_lock);
    if (expected == atomic_load(&generation)) {
        bool stopped = !atomic_load(&wanted) || atomic_load(&quitting);
        status.state = stopped ? NATIVE_IDLE : NATIVE_ERROR;
        snprintf(status.message, sizeof(status.message), "%s", stopped ? "Streaming stopped" : message);
        atomic_store(&wanted, false);
    }
    LightLock_Unlock(&state_lock);
}
// Transient network failure after a successful connection: keep `wanted` set and
// let the worker reconnect. Only an explicit STOP (or cancel) ends the stream.
atomic_uint stat_reconnects = 0; // connection losses that led to a reconnect (shown on the TV)
void stream_retry(unsigned expected, const char *message) {
    atomic_fetch_add(&stat_reconnects, 1);
    LightLock_Lock(&state_lock);
    if (expected == atomic_load(&generation) && atomic_load(&wanted) && !atomic_load(&quitting)) {
        status.state = NATIVE_CONNECTING;
        snprintf(status.message, sizeof(status.message), "Reconnecting: %.100s", message);
    }
    LightLock_Unlock(&state_lock);
    for (unsigned i = 0;
         i < 30 && atomic_load(&wanted) && !atomic_load(&quitting) && expected == atomic_load(&generation); i++) {
        svcSleepThread(50000000); // 1.5 s backoff
    }
}
