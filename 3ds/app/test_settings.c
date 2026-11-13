#define _GNU_SOURCE
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    char path[] = "/tmp/n3ds-native-settings-XXXXXX";
    assert(mkdtemp(path) && chdir(path) == 0 && mkdir("sdmc:", 0777) == 0);
    CastConfig cfg;
    native_load_settings(&cfg);
    assert(!strcmp(cfg.app_id, "01B7A380") && cfg.target_fps == 30 && cfg.jpeg_quality == 50);
    strcpy(cfg.tv_ip, "192.168.1.100");
    strcpy(cfg.app_id, "ABCD1234");
    cfg.jpeg_quality = 100;
    cfg.target_fps = 30;
    assert(native_load_show_timings());
    assert(config_save(&cfg, CONFIG_FILE_PATH));
    FILE *file = fopen(CONFIG_FILE_PATH, "r");
    assert(file);
    char before[4096] = {0};
    assert(fread(before, 1, sizeof(before) - 1, file) > 0);
    fclose(file);
    native_load_settings(&cfg);
    assert(!strcmp(cfg.tv_ip, "192.168.1.100") && !strcmp(cfg.app_id, "01B7A380") && cfg.target_fps == 30 &&
           cfg.jpeg_quality == 100);
    file = fopen(CONFIG_FILE_PATH, "r");
    assert(file);
    char after[4096] = {0};
    assert(fread(after, 1, sizeof(after) - 1, file) > 0);
    fclose(file);
    assert(!strcmp(before, after));
    strcpy(cfg.tv_ip, "10.0.0.8");
    cfg.jpeg_quality = 35;
    cfg.layout_mode = 4;
    cfg.scale_filter = 1;
    cfg.stretch_mode = 1;
    cfg.show_osd = 0;
    cfg.audio_enabled = 1;
    cfg.audio_kbps = 128;
    assert(native_save_settings(&cfg, 0, false, 2, 1));
    native_load_settings(&cfg);
    assert(!strcmp(cfg.tv_ip, "10.0.0.8") && cfg.jpeg_quality == 35 && cfg.layout_mode == 4 && cfg.scale_filter == 1 &&
           cfg.stretch_mode == 1 && !cfg.show_osd);
    assert(cfg.audio_enabled && cfg.audio_kbps == 128 && native_load_language() == 0 && !native_load_show_timings());
    assert(native_load_cpu_core() == 2 && native_load_capture_source() == 1);
    cfg.audio_enabled = 0;
    assert(native_save_settings(&cfg, 1, true, 0, 0));
    native_load_settings(&cfg);
    assert(!cfg.audio_enabled && native_load_language() == 1 && native_load_show_timings());
    assert(native_load_cpu_core() == 0 && native_load_capture_source() == 0);
    puts(
        "PASS: separate native.ini persistence, read-only legacy import, literal IP/App ID preserved, full display settings round trip");
}
