#include "settings.h"
#include "i18n.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *trim(char *p) {
    p += strspn(p, " \t\r\n");
    size_t n = strlen(p);
    while (n && strchr(" \t\r\n", p[n - 1])) {
        p[--n] = 0;
    }
    return p;
}
void native_load_settings(CastConfig *cfg) {
    config_init_defaults(cfg);
    snprintf(cfg->app_id, sizeof(cfg->app_id), "%s", NATIVE_APP_ID);
    cfg->jpeg_quality = NATIVE_QUALITY_AUTO;
    cfg->target_fps = 30;
    cfg->audio_enabled = 1;
    cfg->audio_kbps = 64;
    FILE *file = fopen(NATIVE_CONFIG_PATH, "r");
    if (!file) {
        file = fopen(CONFIG_FILE_PATH, "r"); // import only; never rewrite the old file
    }
    if (!file) {
        return;
    }
    struct Setting {
        const char *key;
        int *value;
        int minimum, maximum;
    };
    struct Setting fields[] = {{"layout_mode", &cfg->layout_mode, 0, 4},   {"scale_filter", &cfg->scale_filter, 0, 1},
                               {"stretch_mode", &cfg->stretch_mode, 0, 1}, {"jpeg_quality", &cfg->jpeg_quality, 0, 100},
                               {"target_fps", &cfg->target_fps, 5, 60},    {"show_osd", &cfg->show_osd, 0, 1}};
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *key = trim(line);
        if (*key == '#' || *key == ';') {
            continue;
        }
        char *value = strchr(key, '=');
        if (!value) {
            continue;
        }
        *value++ = 0;
        key = trim(key);
        value = trim(value);
        if (!strcmp(key, "tv_ip")) {
            snprintf(cfg->tv_ip, sizeof(cfg->tv_ip), "%.63s", value);
        } else if (!strcmp(key, "app_id")) {
            continue; // fixed at build time (NATIVE_APP_ID); old files may still carry one
        } else if (!strcmp(key, "audio_source")) {
            cfg->audio_enabled = !strcmp(value, "game");
        } else if (!strcmp(key, "audio_enabled")) {
            cfg->audio_enabled = atoi(value) ? 1 : 0;
        } else if (!strcmp(key, "audio_kbps")) {
            int rate = atoi(value);
            if (rate == 32 || rate == 64 || rate == 128) {
                cfg->audio_kbps = rate;
            }
        } else {
            char *end = NULL;
            long number = strtol(value, &end, 10);
            if (end == value || *end) {
                continue;
            }
            for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
                if (!strcmp(key, fields[i].key) && number >= fields[i].minimum && number <= fields[i].maximum) {
                    *fields[i].value = (int)number;
                }
            }
        }
    }
    fclose(file);
    if (cfg->jpeg_quality != NATIVE_QUALITY_AUTO && cfg->jpeg_quality < 30) {
        cfg->jpeg_quality = 50;
    }
}

unsigned native_load_language(void) {
    unsigned language = NATIVE_LANG_EN;
    FILE *file = fopen(NATIVE_CONFIG_PATH, "r");
    if (!file) {
        return language;
    }
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *key = trim(line), *value = strchr(key, '=');
        if (!value) {
            continue;
        }
        *value++ = 0;
        if (!strcmp(trim(key), "ui_language")) {
            value = trim(value);
            for (unsigned i = 0; i < NATIVE_LANG_COUNT; i++) {
                if (!strcmp(value, native_language_tag(i))) {
                    language = i;
                }
            }
        }
    }
    fclose(file);
    return language;
}
bool native_load_show_timings(void) {
    bool enabled = true;
    FILE *file = fopen(NATIVE_CONFIG_PATH, "r");
    if (!file) {
        return enabled;
    }
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *key = trim(line), *value = strchr(key, '=');
        if (!value) {
            continue;
        }
        *value++ = 0;
        if (!strcmp(trim(key), "show_timings")) {
            value = trim(value);
            if (!strcmp(value, "0")) {
                enabled = false;
            } else if (!strcmp(value, "1")) {
                enabled = true;
            }
        }
    }
    fclose(file);
    return enabled;
}
static bool read_value(const char *name, char *out, size_t size) {
    bool found = false;
    FILE *file = fopen(NATIVE_CONFIG_PATH, "r");
    if (!file) {
        return false;
    }
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *key = trim(line), *value = strchr(key, '=');
        if (!value) {
            continue;
        }
        *value++ = 0;
        if (!strcmp(trim(key), name)) {
            snprintf(out, size, "%s", trim(value));
            found = true;
        }
    }
    fclose(file);
    return found;
}
unsigned native_load_cpu_core(void) {
    char value[16];
    if (!read_value("cpu_core", value, sizeof(value))) {
        return 0;
    }
    return !strcmp(value, "1") ? 1 : !strcmp(value, "2") ? 2 : 0;
}
unsigned native_load_capture_source(void) {
    char value[16];
    if (!read_value("capture_source", value, sizeof(value))) {
        return 1; // the screen, whatever is displayed
    }
    return !strcmp(value, "game") ? 0 : 1;
}
bool native_save_settings(const CastConfig *cfg, unsigned language, bool show_timings, unsigned cpu_core,
                          unsigned capture_source) {
    const char *temporary = NATIVE_CONFIG_PATH ".tmp";
    if (!config_save(cfg, temporary)) {
        return false;
    }
    FILE *file = fopen(temporary, "a");
    if (!file) {
        return false;
    }
    bool ok = fprintf(file, "audio_source=%s\nui_language=%s\nshow_timings=%d\ncpu_core=%s\ncapture_source=%s\n",
                      cfg->audio_enabled ? "game" : "off", native_language_tag(language), show_timings ? 1 : 0,
                      cpu_core == 1   ? "1"
                      : cpu_core == 2 ? "2"
                                      : "auto",
                      capture_source == 1 ? "screen" : "game") > 0;
    if (fclose(file) != 0) {
        ok = false;
    }
    remove(NATIVE_CONFIG_PATH);
    if (rename(temporary, NATIVE_CONFIG_PATH) == 0) {
        return ok;
    }
    // Fallback: direct write to destination if rename fails (e.g. FAT32 quirks)
    if (!config_save(cfg, NATIVE_CONFIG_PATH)) {
        return false;
    }
    file = fopen(NATIVE_CONFIG_PATH, "a");
    if (!file) {
        return false;
    }
    ok = fprintf(file, "audio_source=%s\nui_language=%s\nshow_timings=%d\ncpu_core=%s\ncapture_source=%s\n",
                 cfg->audio_enabled ? "game" : "off", native_language_tag(language), show_timings ? 1 : 0,
                 cpu_core == 1   ? "1"
                 : cpu_core == 2 ? "2"
                                 : "auto",
                 capture_source == 1 ? "screen" : "game") > 0;
    if (fclose(file) != 0) {
        ok = false;
    }
    remove(temporary);
    return ok;
}
