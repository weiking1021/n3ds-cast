#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <stdatomic.h>
#include "client.h"
#include "ui.h"
#include "settings.h"
#include "i18n.h"
#include "ui_layout.h"
#define TR(s) native_tr(ui->language, s)

// Leave room in this launcher's resource limit for the small staging image.
u32 __ctru_heap_size = 8 * 1024 * 1024;
u32 __ctru_linear_heap_size = 8 * 1024 * 1024;

static atomic_bool scan_cancel = false, scan_done = false;
static NativeTV scan_results[NATIVE_TV_LIMIT];
static unsigned scan_count;
static NativeDiscoveryInfo scan_info;
static Thread scanner;
static void scan_worker(void *unused) {
    (void)unused;
    scan_count = native_discover_ex(scan_results, NATIVE_TV_LIMIT, &scan_cancel, &scan_info);
    atomic_store_explicit(&scan_done, true, memory_order_release);
}
static void begin_scan(NativeUI *ui, bool soc_ok) {
    if (scanner) {
        return;
    }
    if (!soc_ok) {
        snprintf(ui->notice, sizeof(ui->notice), TR("Network unavailable; check Wi-Fi"));
        return;
    }
    atomic_store(&scan_cancel, false);
    atomic_store(&scan_done, false);
    scanner = threadCreate(scan_worker, NULL, 0x10000, 0x32, -1, false);
    ui->scanning = scanner != NULL;
    snprintf(ui->notice, sizeof(ui->notice), "%s",
             scanner ? TR("Searching for Google Cast TVs...") : TR("Could not start network scan"));
}
static bool active(const NativeUI *ui) {
    return ui->status.state == NATIVE_CONNECTING || ui->status.state == NATIVE_WAIT_GAME ||
           ui->status.state == NATIVE_STREAMING;
}
static bool edit_text(const char *hint, char *value, size_t size) {
    SwkbdState keyboard;
    char result[64];
    snprintf(result, sizeof(result), "%s", value);
    swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, size - 1);
    swkbdSetInitialText(&keyboard, result);
    swkbdSetHintText(&keyboard, hint);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "Apply", true);
    if (swkbdInputText(&keyboard, result, sizeof(result)) != SWKBD_BUTTON_RIGHT || !result[0]) {
        return false;
    }
    snprintf(value, size, "%s", result);
    return true;
}
static void choose_tv(NativeUI *ui, unsigned selected) {
    if (active(ui) || selected >= ui->tv_count || selected >= NATIVE_TV_LIMIT) {
        return;
    }
    ui->selected_tv = selected;
    NativeTV chosen = ui->tvs[selected];
    snprintf(ui->cfg.tv_ip, sizeof(ui->cfg.tv_ip), "%s", chosen.ip);
    snprintf(ui->selected_name, sizeof(ui->selected_name), "%s", chosen.name);
    snprintf(ui->notice, sizeof(ui->notice), TR("TV selected. Y starts streaming; X saves."));
}
static void edit_setting(NativeUI *ui, int direction, bool press_a) {
    if (ui->selected_setting == 9) {
        ui->language = (ui->language + NATIVE_LANG_COUNT + direction) % NATIVE_LANG_COUNT;
        return;
    }
    if (active(ui)) {
        snprintf(ui->notice, sizeof(ui->notice), TR("Stop streaming before editing settings."));
        return;
    }
    switch (ui->selected_setting) {
        case 0:
            if (press_a && edit_text(TR("Google TV IPv4 address"), ui->cfg.tv_ip, sizeof(ui->cfg.tv_ip))) {
                ui->selected_name[0] = 0;
            }
            break;
        case 1:
            ui->cfg.layout_mode = (ui->cfg.layout_mode + direction + 5) % 5;
            break;
        case 2:
            ui->cfg.scale_filter ^= 1;
            break;
        case 3:
            ui->cfg.stretch_mode ^= 1;
            break;
        case 4: { // Auto, then 30..100 in steps of 5
            int index = ui->cfg.jpeg_quality == NATIVE_QUALITY_AUTO ? 0 : (ui->cfg.jpeg_quality - 30) / 5 + 1;
            index = (index + direction + 16) % 16;
            ui->cfg.jpeg_quality = index ? 30 + (index - 1) * 5 : NATIVE_QUALITY_AUTO;
            break;
        }
        case 5:
            ui->cfg.target_fps = 5 + ((ui->cfg.target_fps - 5) / 5 + direction + 12) % 12 * 5;
            break; // 5..60
        case 6:
            ui->cfg.show_osd ^= 1;
            break;
        case 7:
            ui->cfg.audio_enabled ^= 1;
            break;
        case 8: {
            int index = ui->cfg.audio_kbps == 32 ? 0 : ui->cfg.audio_kbps == 64 ? 1 : 2;
            ui->cfg.audio_kbps = 32 << ((index + direction + 3) % 3);
            break;
        }
        case 10:
            ui->cpu_core = (ui->cpu_core + direction + NATIVE_CORE_COUNT) % NATIVE_CORE_COUNT;
            break;
        case 11:
            ui->capture_source ^= 1;
            break;
        case 12: { // Auto, then 0.5 MB/s to 2 MB/s in steps of 0.25
            int index = ui->cfg.link_kbps == 0 ? 0 : (ui->cfg.link_kbps - 500 + 128) / 250 + 1;
            index = (index + direction + 8) % 8;
            ui->cfg.link_kbps = index ? 500 + (index - 1) * 250 : 0;
            break;
        }
    }
}
// ---- The app: one instance for the whole run ----------------------------------------------------------------
#define SOC_BUFFER_SIZE 0x100000
#define STATUS_INTERVAL_MS 250 // how often the background process is asked for its status
#define AUTO_RESCANS 2         // Wi-Fi often needs a moment right after the start: look again by itself this often
#define SCANNER_JOIN_NS 5000000000ULL
#define TAB_COUNT 3
#define TAB_CONTROL 0
#define TAB_DEVICES 1
#define TAB_SETTINGS 2
// Touch areas (pixels on the bottom screen) that are not covered by ui_layout.h.
#define TOUCH_CONTROL_LEFT 12
#define TOUCH_CONTROL_RIGHT 308
#define TOUCH_START_TOP 50 // the big start/stop button
#define TOUCH_START_BOTTOM 110
#define TOUCH_SCAN_TOP 120 // the search button
#define TOUCH_SCAN_BOTTOM 172
#define TOUCH_SELECTED_TV_TOP 180        // the selected-TV card: opens the Devices tab
#define TOUCH_SETTING_DECREASE_RIGHT 134 // touching a setting left of this x lowers its value, right of it raises it
#define TOUCH_SAVE_TOP 210
#define TOUCH_SAVE_LEFT 133
#define TOUCH_SAVE_RIGHT 229

typedef struct {
    NativeUI ui;
    Handle resident; // control connection to the background process, 0 when there is none
    void *soc;       // memory handed to socInit
    bool soc_ok;
    bool last_show_timings;
    uint64_t next_status;
    unsigned auto_rescans;
} App;

static void app_init(App *app) {
    NativeUI *ui = &app->ui;
    native_load_settings(&ui->cfg);
    ui->language = native_load_language();
    ui->show_timings = false; // the RJT overlay is no longer offered
    ui->cpu_core = native_load_cpu_core();
    ui->capture_source = native_load_capture_source();
    ui->status.magic = NATIVE_MAGIC;
    ui->status.version = NATIVE_VERSION;
    snprintf(ui->status.message, sizeof(ui->status.message), "Find your TV, then start streaming");
    ui->connected = native_connect(&app->resident, &ui->status) == 0;
    app->soc = memalign(0x1000, SOC_BUFFER_SIZE);
    app->soc_ok = app->soc && socInit(app->soc, SOC_BUFFER_SIZE) == 0;
    if (app->resident) {
        native_request(app->resident, ui->show_timings ? NATIVE_OSD_ON : NATIVE_OSD_OFF, NULL, &ui->status);
    }
    app->last_show_timings = ui->show_timings;
    begin_scan(ui, app->soc_ok);
}

static void drop_resident(App *app) {
    svcCloseHandle(app->resident);
    app->resident = 0;
    app->ui.connected = false;
}

// A finished TV search: take its results, pre-select a TV and tell the user what happened.
static void finish_scan(App *app) {
    NativeUI *ui = &app->ui;
    if (!scanner || !atomic_load_explicit(&scan_done, memory_order_acquire) || threadJoin(scanner, 0) != 0) {
        return;
    }
    threadFree(scanner);
    scanner = NULL;
    ui->scanning = false;
    ui->tv_count = scan_count;
    memcpy(ui->tvs, scan_results, sizeof(ui->tvs));
    if (ui->selected_tv >= ui->tv_count) {
        ui->selected_tv = 0;
    }
    unsigned selected = 0;
    for (unsigned i = 0; i < ui->tv_count; i++) {
        if (!strcmp(ui->cfg.tv_ip, ui->tvs[i].ip)) {
            selected = i;
        }
    }
    if (ui->tv_count == 1 || (ui->tv_count && !strcmp(ui->cfg.tv_ip, ui->tvs[selected].ip))) {
        choose_tv(ui, selected);
    }
    if (ui->tv_count) {
        snprintf(ui->notice, sizeof(ui->notice), native_tr(ui->language, "Found %u TV(s). Select in Devices."),
                 ui->tv_count);
    } else if (!scan_info.sockets) {
        snprintf(ui->notice, sizeof(ui->notice), native_tr(ui->language, "Network scan failed (%d). SELECT retries."),
                 scan_info.error);
    } else if (scan_info.valid_packets) {
        snprintf(ui->notice, sizeof(ui->notice), native_tr(ui->language, "No TV: %u replies. SELECT retries."),
                 scan_info.valid_packets);
    } else {
        snprintf(ui->notice, sizeof(ui->notice), "%s",
                 native_tr(ui->language, "No mDNS replies. Check same LAN / isolation."));
    }
    if (ui->tv_count > 1 && !active(ui)) {
        ui->tab = TAB_DEVICES;
    }
    if (!ui->tv_count && scan_info.sockets && app->auto_rescans < AUTO_RESCANS && app->soc_ok) {
        app->auto_rescans++;
        begin_scan(ui, app->soc_ok);
    } else if (ui->tv_count) {
        app->auto_rescans = 0;
    }
}

// A touch on the bottom screen. May ask for a start/stop (toggle) or a save.
static void handle_touch(App *app, bool *toggle, bool *save) {
    NativeUI *ui = &app->ui;
    touchPosition touch;
    hidTouchRead(&touch);
    int tab = native_ui_tab_at(touch.px, touch.py);
    int page = native_ui_page_at(touch.px, touch.py);
    if (tab >= 0) {
        ui->tab = tab;
    } else if (ui->tab == TAB_CONTROL && touch.px >= TOUCH_CONTROL_LEFT && touch.px < TOUCH_CONTROL_RIGHT) {
        if (touch.py >= TOUCH_START_TOP && touch.py < TOUCH_START_BOTTOM) {
            *toggle = true;
        }
        if (touch.py >= TOUCH_SCAN_TOP && touch.py < TOUCH_SCAN_BOTTOM) {
            begin_scan(ui, app->soc_ok);
        }
        if (touch.py >= TOUCH_SELECTED_TV_TOP) {
            ui->tab = TAB_DEVICES;
        }
    } else if (ui->tab == TAB_DEVICES) {
        int index = native_ui_device_at(ui->selected_tv, ui->tv_count, touch.px, touch.py);
        if (index >= 0) {
            choose_tv(ui, (unsigned)index);
        } else if (page) {
            ui->selected_tv = native_ui_device_page(ui->selected_tv, ui->tv_count, page);
        }
    } else if (ui->tab == TAB_SETTINGS) {
        int setting = native_ui_setting_at(ui->selected_setting, touch.px, touch.py);
        if (setting >= 0) {
            ui->selected_setting = setting;
            edit_setting(ui, touch.px < TOUCH_SETTING_DECREASE_RIGHT ? -1 : 1, true);
        } else if (page) {
            ui->selected_setting = native_ui_setting_page(ui->selected_setting, page);
        } else if (touch.py >= TOUCH_SAVE_TOP && touch.px >= TOUCH_SAVE_LEFT && touch.px < TOUCH_SAVE_RIGHT) {
            *save = true;
        }
    }
}

// Buttons whose meaning depends on the current tab.
static void handle_tab_keys(NativeUI *ui, u32 keys, bool *toggle) {
    if (ui->tab == TAB_DEVICES && ui->tv_count) {
        if (keys & KEY_UP) {
            ui->selected_tv = (ui->selected_tv + ui->tv_count - 1) % ui->tv_count;
        }
        if (keys & KEY_DOWN) {
            ui->selected_tv = (ui->selected_tv + 1) % ui->tv_count;
        }
        if (keys & KEY_A) {
            choose_tv(ui, ui->selected_tv);
        }
        if (keys & KEY_LEFT) {
            ui->selected_tv = native_ui_device_page(ui->selected_tv, ui->tv_count, -1);
        }
        if (keys & KEY_RIGHT) {
            ui->selected_tv = native_ui_device_page(ui->selected_tv, ui->tv_count, 1);
        }
    } else if (ui->tab == TAB_SETTINGS) {
        if (keys & KEY_UP) {
            ui->selected_setting = (ui->selected_setting + NATIVE_SETTINGS_COUNT - 1) % NATIVE_SETTINGS_COUNT;
        }
        if (keys & KEY_DOWN) {
            ui->selected_setting = (ui->selected_setting + 1) % NATIVE_SETTINGS_COUNT;
        }
        if (keys & (KEY_LEFT | KEY_RIGHT | KEY_A)) {
            edit_setting(ui, keys & KEY_LEFT ? -1 : 1, (keys & KEY_A) != 0);
        }
    } else if (keys & KEY_A) {
        *toggle = true;
    }
}

// Keeps the background process' overlay switch in step with the setting.
static void sync_overlay(App *app) {
    NativeUI *ui = &app->ui;
    if (!app->resident || ui->show_timings == app->last_show_timings) {
        return;
    }
    Result rc = native_request(app->resident, ui->show_timings ? NATIVE_OSD_ON : NATIVE_OSD_OFF, NULL, &ui->status);
    if (rc != 0) {
        drop_resident(app);
    }
    app->last_show_timings = ui->show_timings;
}

static void save_settings(NativeUI *ui) {
    snprintf(ui->notice, sizeof(ui->notice), "%s",
             native_save_settings(&ui->cfg, ui->language, ui->show_timings, ui->cpu_core, ui->capture_source)
                 ? "Settings saved"
                 : "Could not save settings");
}

// Stops the running stream, or starts one (launching the background process first when it is not running).
// Returns false when the settings are invalid and the rest of this frame is to be skipped.
static bool toggle_stream(App *app) {
    NativeUI *ui = &app->ui;
    if (active(ui) && app->resident) {
        Result rc = native_request(app->resident, NATIVE_STOP, NULL, &ui->status);
        snprintf(ui->notice, sizeof(ui->notice),
                 rc ? native_tr(ui->language, "Stop request failed: %08lX") : "Stopping stream...", (unsigned long)rc);
        return true;
    }
    if (!native_config_valid(&ui->cfg)) {
        snprintf(ui->notice, sizeof(ui->notice), "Check IPv4 address and 8-digit hex App ID.");
        ui->tab = TAB_SETTINGS;
        native_ui_draw(ui);
        return false;
    }
    snprintf(ui->notice, sizeof(ui->notice), "Starting own background stream service...");
    native_ui_draw(ui);
    char error[160] = {0};
    if (!app->resident && native_launch(&app->resident, &ui->status, error, sizeof(error)) != 0) {
        snprintf(ui->notice, sizeof(ui->notice), "%s", error);
        return true;
    }
    Result rc = native_request(app->resident, ui->show_timings ? NATIVE_OSD_ON : NATIVE_OSD_OFF, NULL, &ui->status);
    NativeStartConfig start = {.cast = ui->cfg, .cpu_core = ui->cpu_core, .capture_source = ui->capture_source};
    snprintf(start.cast.lang, sizeof(start.cast.lang), "%s",
             native_language_tag(ui->language)); // the receiver page speaks it
    if (!rc) {
        rc = native_request(app->resident, NATIVE_START, &start, &ui->status);
    }
    ui->connected = rc == 0;
    snprintf(ui->notice, sizeof(ui->notice), "%s",
             rc ? "Start request failed" : "Wait for READY, then HOME > open your game.");
    if (!rc && !native_save_settings(&ui->cfg, ui->language, ui->show_timings, ui->cpu_core, ui->capture_source)) {
        snprintf(ui->notice, sizeof(ui->notice), "Started; could not save settings.");
    }
    return true;
}

static void poll_status(App *app) {
    NativeUI *ui = &app->ui;
    if (!app->resident || osGetTime() < app->next_status) {
        return;
    }
    Result rc = native_request(app->resident, NATIVE_STATUS, NULL, &ui->status);
    if (rc != 0) {
        drop_resident(app);
        ui->status.state = NATIVE_ERROR;
        snprintf(ui->status.message, sizeof(ui->status.message),
                 native_tr(ui->language, "Background service ended: %08lX"), (unsigned long)rc);
    }
    app->next_status = osGetTime() + STATUS_INTERVAL_MS;
}

// Releases everything the app holds. A discovery thread that does not stop is never freed: its stack and the SOC
// memory stay allocated, and the process is ended instead.
static void app_exit(App *app) {
    atomic_store(&scan_cancel, true);
    bool joined = !scanner || threadJoin(scanner, SCANNER_JOIN_NS) == 0;
    if (scanner && joined) {
        threadFree(scanner);
    }
    if (app->resident) {
        svcCloseHandle(app->resident);
    }
    if (!joined) {
        native_ui_exit();
        gfxExit();
        svcExitProcess();
    }
    if (app->soc_ok) {
        socExit();
    }
    free(app->soc);
    native_ui_exit();
    gfxExit();
}

int main(void) {
    gfxInitDefault();
    if (!native_ui_init()) {
        gfxExit();
        return 1;
    }
    App app = {0};
    app_init(&app);
    NativeUI *ui = &app.ui;
    while (aptMainLoop()) {
        hidScanInput();
        u32 keys = hidKeysDown();
        if (keys & KEY_START) {
            break; // Only closes this UI; independent service continues.
        }
        if (keys & KEY_R) {
            ui->tab = (ui->tab + 1) % TAB_COUNT;
        }
        if (keys & KEY_L) {
            ui->tab = (ui->tab + TAB_COUNT - 1) % TAB_COUNT;
        }
        if (keys & KEY_SELECT) {
            app.auto_rescans = 0;
            begin_scan(ui, app.soc_ok);
        }
        finish_scan(&app);
        bool toggle = (keys & KEY_Y) != 0;
        bool save = (keys & KEY_X) != 0;
        if ((keys & KEY_B) && active(ui)) {
            toggle = true;
        }
        if (keys & KEY_TOUCH) {
            handle_touch(&app, &toggle, &save);
        }
        handle_tab_keys(ui, keys, &toggle);
        sync_overlay(&app);
        if (save) {
            save_settings(ui);
        }
        if (toggle && !toggle_stream(&app)) {
            continue;
        }
        poll_status(&app);
        native_ui_draw(ui);
    }
    app_exit(&app);
    return 0;
}
