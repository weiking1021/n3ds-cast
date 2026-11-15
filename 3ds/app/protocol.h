#pragma once
#include <stdint.h>
#include "config.h"

// Private local IPC ABI for the two components embedded in one .3dsx.
// The Cast receiver application ID is fixed when the app is packaged (end users never change it).
// Override when building: make APP_ID=XXXXXXXX (see native/Makefile).
#ifndef NATIVE_APP_ID
#define NATIVE_APP_ID "01B7A380"
#endif
#define NATIVE_PORT "n3dcast"
#define NATIVE_MAGIC 0x4E334443u
#define NATIVE_VERSION 32u
// jpeg_quality 0 = automatic: the encoder adapts it to the scene (see encoder_thread).
#define NATIVE_QUALITY_AUTO 0
#define NATIVE_MEMORY_LIMIT (8u * 1024 * 1024)
enum NativeCommand {
    NATIVE_STATUS = 1,
    NATIVE_START,
    NATIVE_STOP,
    NATIVE_EXIT,
    NATIVE_OSD_ON,
    NATIVE_OSD_OFF
};
enum NativeAudioState {
    NATIVE_AUDIO_OFF,
    NATIVE_AUDIO_WAIT,
    NATIVE_AUDIO_UNSUPPORTED,
    NATIVE_AUDIO_LIVE
};
enum NativeState {
    NATIVE_IDLE,
    NATIVE_CONNECTING,
    NATIVE_WAIT_GAME,
    NATIVE_STREAMING,
    NATIVE_ERROR
};
// Encoder thread placement. AUTO tries the New 3DS core 2, then falls back to core 1.
enum NativeCore {
    NATIVE_CORE_AUTO = 0,
    NATIVE_CORE_1 = 1,
    NATIVE_CORE_2 = 2,
    NATIVE_CORE_COUNT = 3
};
// GAME: only while a 3DS application process exists. SCREEN: whatever is displayed.
enum NativeSource {
    NATIVE_SOURCE_GAME = 0,
    NATIVE_SOURCE_SCREEN = 1,
    NATIVE_SOURCE_COUNT = 2
};
typedef struct {
    CastConfig cast;
    uint32_t cpu_core, capture_source;
} NativeStartConfig;
typedef struct {
    uint32_t magic, version, pid, state, frames, rejected;
    uint64_t title_id;
    char message[128];
    uint32_t fps, capture_ms, encode_ms, send_ms, audio_state, audio_packets, audio_dropped;
    uint32_t overlay_enabled, overlay_state, overlay_error;
    uint32_t worker_core, core_fallback; // core running the encoder (0=none); 1 if AUTO/core 2 fell back
    uint32_t quality;                    // JPEG quality currently in use (differs from the setting in automatic mode)
} NativeStatus;

static inline int native_config_valid(const CastConfig *cfg) {
    if (!cfg) {
        return 0;
    }
    unsigned i;
    for (i = 0; i < sizeof(cfg->tv_ip) && cfg->tv_ip[i]; i++)
        ;
    if (i == 0 || i == sizeof(cfg->tv_ip)) {
        return 0;
    }
    const char *ip = cfg->tv_ip;
    for (unsigned part = 0; part < 4; part++) {
        unsigned digits = 0, value = 0;
        const char *start = ip;
        while (*ip >= '0' && *ip <= '9') {
            value = value * 10 + (*ip++ - '0');
            if (++digits > 3) {
                return 0;
            }
        }
        if (!digits || value > 255 || (digits > 1 && *start == '0')) {
            return 0;
        }
        if (part < 3) {
            if (*ip++ != '.') {
                return 0;
            }
        } else if (*ip) {
            return 0;
        }
    }
    for (i = 0; i < sizeof(cfg->app_id) && cfg->app_id[i]; i++) {
        unsigned char c = (unsigned char)cfg->app_id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return i == 8 && cfg->layout_mode >= 0 && cfg->layout_mode < LAYOUT_COUNT && cfg->scale_filter >= 0 &&
           cfg->scale_filter < FILTER_COUNT && cfg->stretch_mode >= 0 && cfg->stretch_mode < STRETCH_COUNT &&
           (cfg->jpeg_quality == NATIVE_QUALITY_AUTO || (cfg->jpeg_quality >= 30 && cfg->jpeg_quality <= 100)) &&
           cfg->target_fps >= 5 && cfg->target_fps <= 60 && (cfg->show_osd == 0 || cfg->show_osd == 1) &&
           (cfg->audio_enabled == 0 || cfg->audio_enabled == 1) &&
           (!cfg->audio_enabled || cfg->audio_kbps == 32 || cfg->audio_kbps == 64 || cfg->audio_kbps == 128);
}
static inline int native_start_valid(const NativeStartConfig *start) {
    return start && native_config_valid(&start->cast) && start->cpu_core < NATIVE_CORE_COUNT &&
           start->capture_source < NATIVE_SOURCE_COUNT;
}
