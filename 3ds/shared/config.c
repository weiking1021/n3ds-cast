#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

void config_init_defaults(CastConfig *cfg) {
    memset(cfg, 0, sizeof(CastConfig));
    strncpy(cfg->app_id, "4FD132A7", sizeof(cfg->app_id) - 1);
    cfg->layout_mode = LAYOUT_TOP_ONLY;
    cfg->scale_filter = FILTER_PIXELATED;
    cfg->stretch_mode = STRETCH_KEEP_ASPECT;
    cfg->jpeg_quality = 70;
    cfg->target_fps = 20;
    cfg->show_osd = 1;
    cfg->audio_enabled = 0;
    cfg->audio_kbps = 64;
}

static void trim_whitespace(char *str) {
    char *end;
    while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') {
        memmove(str, str + 1, strlen(str));
    }
    if (*str == 0) {
        return;
    }
    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
}

bool config_load(CastConfig *cfg, const char *path) {
    config_init_defaults(cfg);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }

    bool migrated = false;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        trim_whitespace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim_whitespace(key);
        trim_whitespace(val);

        if (strcmp(key, "tv_ip") == 0) {
            strncpy(cfg->tv_ip, val, sizeof(cfg->tv_ip) - 1);
        } else if (strcmp(key, "app_id") == 0) {
            if (strcmp(val, "ABCD1234") == 0) {
                strncpy(cfg->app_id, "4FD132A7", sizeof(cfg->app_id) - 1);
                migrated = true;
            } else {
                strncpy(cfg->app_id, val, sizeof(cfg->app_id) - 1);
            }
        } else if (strcmp(key, "layout_mode") == 0) {
            int v = atoi(val);
            if (v >= 0 && v < LAYOUT_COUNT) {
                cfg->layout_mode = v;
            }
        } else if (strcmp(key, "scale_filter") == 0) {
            int v = atoi(val);
            if (v >= 0 && v < FILTER_COUNT) {
                cfg->scale_filter = v;
            }
        } else if (strcmp(key, "stretch_mode") == 0) {
            int v = atoi(val);
            if (v >= 0 && v < STRETCH_COUNT) {
                cfg->stretch_mode = v;
            }
        } else if (strcmp(key, "jpeg_quality") == 0) {
            int v = atoi(val);
            if (v >= 30 && v <= 95) {
                cfg->jpeg_quality = v;
            }
        } else if (strcmp(key, "target_fps") == 0) {
            int v = atoi(val);
            if (v >= 5 && v <= 30) {
                cfg->target_fps = v;
            }
        } else if (strcmp(key, "show_osd") == 0) {
            cfg->show_osd = atoi(val) ? 1 : 0;
        } else if (strcmp(key, "audio_enabled") == 0) {
            cfg->audio_enabled = atoi(val) ? 1 : 0;
        } else if (strcmp(key, "audio_kbps") == 0) {
            int v = atoi(val);
            if (v == 32 || v == 64 || v == 128) {
                cfg->audio_kbps = v;
            }
        }
    }

    fclose(fp);
    if (migrated) {
        config_save(cfg, path);
    }
    return true;
}

bool config_save(const CastConfig *cfg, const char *path) {
    mkdir("sdmc:/3ds", 0777);
    mkdir(CONFIG_DIR_PATH, 0777);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        return false;
    }

    fprintf(fp, "# n3ds-cast 設定檔\n");
    fprintf(fp, "tv_ip=%s\n", cfg->tv_ip);
    fprintf(fp, "app_id=%s\n", cfg->app_id);
    fprintf(fp, "layout_mode=%d\n", cfg->layout_mode);
    fprintf(fp, "scale_filter=%d\n", cfg->scale_filter);
    fprintf(fp, "stretch_mode=%d\n", cfg->stretch_mode);
    fprintf(fp, "jpeg_quality=%d\n", cfg->jpeg_quality);
    fprintf(fp, "target_fps=%d\n", cfg->target_fps);
    fprintf(fp, "show_osd=%d\n", cfg->show_osd);
    fprintf(fp, "audio_enabled=%d\n", cfg->audio_enabled);
    fprintf(fp, "audio_kbps=%d\n", cfg->audio_kbps);

    fclose(fp);
    return true;
}

const char *config_layout_name(int mode) {
    switch (mode) {
        case LAYOUT_TOP_ONLY:
            return "Top Screen Only (400x240)";
        case LAYOUT_BOTTOM_ONLY:
            return "Bottom Screen Only (320x240)";
        case LAYOUT_VERTICAL:
            return "Dual Vertical (Top + Bottom)";
        case LAYOUT_SIDE_BY_SIDE:
            return "Dual Side-by-Side (16:9)";
        case LAYOUT_PIP:
            return "Picture-in-Picture (PiP)";
        default:
            return "Unknown";
    }
}

const char *config_filter_name(int filter) {
    switch (filter) {
        case FILTER_PIXELATED:
            return "Pixelated (Sharp Retro)";
        case FILTER_SMOOTH:
            return "Smooth (Bilinear)";
        default:
            return "Unknown";
    }
}

const char *config_stretch_name(int stretch) {
    switch (stretch) {
        case STRETCH_KEEP_ASPECT:
            return "Keep Aspect Ratio";
        case STRETCH_FILL_SCREEN:
            return "Stretch to 16:9 TV";
        default:
            return "Unknown";
    }
}
