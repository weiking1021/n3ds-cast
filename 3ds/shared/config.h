#ifndef N3DS_CAST_CONFIG_H
#define N3DS_CAST_CONFIG_H

#include <stdbool.h>

#define CONFIG_DIR_PATH "sdmc:/3ds/n3ds-cast"
#define CONFIG_FILE_PATH "sdmc:/3ds/n3ds-cast/config.ini"

typedef enum {
    LAYOUT_TOP_ONLY = 0,
    LAYOUT_BOTTOM_ONLY = 1,
    LAYOUT_VERTICAL = 2,
    LAYOUT_SIDE_BY_SIDE = 3,
    LAYOUT_PIP = 4,
    LAYOUT_COUNT = 5
} LayoutMode;

typedef enum {
    FILTER_PIXELATED = 0,
    FILTER_SMOOTH = 1,
    FILTER_COUNT = 2
} ScaleFilter;

typedef enum {
    STRETCH_KEEP_ASPECT = 0,
    STRETCH_FILL_SCREEN = 1,
    STRETCH_COUNT = 2
} StretchMode;

typedef struct {
    char tv_ip[64];
    char app_id[32];
    char lang[8]; // language tag the receiver page should show its texts in ("en", "zh-TW", "ja"); empty = none
    int layout_mode;
    int scale_filter;
    int stretch_mode;
    int jpeg_quality;
    int target_fps;
    int show_osd;
    int audio_enabled; // 0=off, 1=440 Hz test tone (not other games' audio)
    int audio_kbps;    // 32/64/128, mono IMA ADPCM at 8/16/32 kHz
} CastConfig;

void config_init_defaults(CastConfig *cfg);
bool config_load(CastConfig *cfg, const char *path);
bool config_save(const CastConfig *cfg, const char *path);

const char *config_layout_name(int mode);
const char *config_filter_name(int filter);
const char *config_stretch_name(int stretch);

#endif // N3DS_CAST_CONFIG_H
