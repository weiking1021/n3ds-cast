#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "i18n.h"
#include "font_data.h"
#include "ui_layout.h"
#include "version.h"
static C3D_RenderTarget *screens[2];
static C2D_TextBuf text_buffer, measure_buffer;
static C2D_Font ui_font;
static unsigned language;
static u32 background, card, ink, muted, accent;
static void rect(float x, float y, float w, float h, u32 color) {
    C2D_DrawRectSolid(x, y, 0, w, h, color);
}
static void panel(float x, float y, float w, float h, u32 color) {
    rect(x + 7, y, w - 14, h, color);
    rect(x, y + 7, w, h - 14, color);
    C2D_DrawCircleSolid(x + 7, y + 7, 0, 7, color);
    C2D_DrawCircleSolid(x + w - 7, y + 7, 0, 7, color);
    C2D_DrawCircleSolid(x + 7, y + h - 7, 0, 7, color);
    C2D_DrawCircleSolid(x + w - 7, y + h - 7, 0, 7, color);
}

static C2D_Font text_font(const char *value) {
    C2D_Font font = ui_font;
    for (const unsigned char *p = (const unsigned char *)value; *p;) {
        unsigned c = *p++;
        if (c >= 0xC0) {
            unsigned remaining = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
            c &= (1u << (6 - remaining)) - 1;
            while (remaining-- && (*p & 0xC0) == 0x80) {
                c = (c << 6) | (*p++ & 63);
            }
        }
        if (c > 127 && C2D_FontGlyphIndexFromCodePoint(font, c) == C2D_FontGetInfo(font)->alterCharIndex &&
            C2D_FontGlyphIndexFromCodePoint(NULL, c) != C2D_FontGetInfo(NULL)->alterCharIndex) {
            return NULL;
        }
    }
    return font;
}
static bool glyph_known(unsigned c) {
    return C2D_FontGlyphIndexFromCodePoint(ui_font, c) != C2D_FontGetInfo(ui_font)->alterCharIndex ||
           C2D_FontGlyphIndexFromCodePoint(NULL, c) != C2D_FontGetInfo(NULL)->alterCharIndex;
}
// Names come from the network (a TV can be called anything): a character that neither our font nor the system font
// has, and any broken UTF-8, is shown as '?' instead of a missing-glyph box. Returns value itself when nothing changes.
static const char *printable(const char *value, char *out, size_t size) {
    size_t at = 0;
    bool changed = false;
    const unsigned char *p = (const unsigned char *)value;
    while (*p && at + 1 < size) {
        const unsigned char *start = p;
        unsigned c = *p++;
        bool valid = c < 0xC0 ? c < 0x80 : true;
        if (c >= 0xC0) {
            unsigned remaining = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
            c &= (1u << (6 - remaining)) - 1;
            for (; remaining && valid; remaining--) {
                valid = (*p & 0xC0) == 0x80;
                if (valid) {
                    c = (c << 6) | (*p++ & 63);
                }
            }
        }
        if (c > 127 && (!valid || !glyph_known(c))) {
            out[at++] = '?';
            changed = true;
            continue;
        }
        size_t n = (size_t)(p - start);
        if (at + n + 1 > size) {
            break;
        }
        memcpy(out + at, start, n);
        at += n;
    }
    out[at] = 0;
    return changed ? out : value;
}
static float text_width(const char *value, float scale) {
    C2D_Text parsed;
    float width = 0;
    C2D_TextBufClear(measure_buffer);
    C2D_TextFontParse(&parsed, text_font(value), measure_buffer, value);
    C2D_TextGetDimensions(&parsed, scale, scale, &width, NULL);
    return width;
}
static size_t text_prefix(const char *value, float scale, float width) {
    size_t positions[256] = {0}, count = 0, at = 0;
    while (value[at] && count < 255) {
        at++;
        while (value[at] && ((unsigned char)value[at] & 0xC0) == 0x80) {
            at++;
        }
        if (at >= 256) {
            break;
        }
        positions[++count] = at;
    }
    size_t low = 0, high = count;
    while (low < high) {
        size_t mid = (low + high + 1) / 2;
        char prefix[256];
        memcpy(prefix, value, positions[mid]);
        prefix[positions[mid]] = 0;
        if (text_width(prefix, scale) <= width) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }
    return positions[low];
}
static void text(float x, float y, float scale, u32 color, const char *value, float width) {
    value = native_tr(language, value);
    char clean[260], shortened[260];
    value = printable(value, clean, sizeof(clean));
    // Keep the requested font size. Long values get an ellipsis, never tiny text.
    if (text_width(value, scale) > width) {
        size_t bytes = text_prefix(value, scale, width - text_width("...", scale));
        memcpy(shortened, value, bytes);
        memcpy(shortened + bytes, "...", 4);
        value = shortened;
    }
    C2D_Text parsed;
    C2D_TextFontParse(&parsed, text_font(value), text_buffer, value);
    C2D_TextOptimize(&parsed);
    C2D_DrawText(&parsed, C2D_WithColor, x, y, 0, scale, scale, color);
}
static void wrapped_text(float x, float y, float scale, u32 color, const char *value, float width, unsigned lines) {
    value = native_tr(language, value);
    for (unsigned line = 0; line < lines && *value; line++) {
        if (line + 1 == lines) {
            text(x, y + line * 25, scale, color, value, width);
            break;
        }
        size_t bytes = text_prefix(value, scale, width);
        if (!bytes) {
            break;
        }
        if (value[bytes]) {
            for (size_t i = bytes; i > bytes / 2; i--) {
                if (value[i - 1] == ' ') {
                    bytes = i - 1;
                    break;
                }
            }
        }
        char part[256];
        memcpy(part, value, bytes);
        part[bytes] = 0;
        text(x, y + line * 25, scale, color, part, width);
        value += bytes;
        while (*value == ' ') {
            value++;
        }
    }
}
static void page_buttons(unsigned page, unsigned pages, bool save) {
    panel(12, 210, 66, 28, card);
    text(34, 209, .85f, ink, "<", 30);
    panel(242, 210, 66, 28, card);
    text(264, 209, .85f, ink, ">", 30);
    char label[32];
    snprintf(label, sizeof(label), "%u/%u", page + 1, pages);
    text(save ? 85 : 139, 212, .70f, muted, label, save ? 42 : 64);
    if (save) {
        panel(133, 210, 96, 28, card);
        text(144, 212, .70f, accent, "Save [X]", 78);
    }
}

bool native_ui_init(void) {
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        return false;
    }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) {
        C3D_Fini();
        return false;
    }
    C2D_Prepare();
    ui_font = C2D_FontLoadFromMem(native_font, sizeof(native_font));
    if (ui_font) {
        C2D_FontSetFilter(ui_font, GPU_NEAREST, GPU_NEAREST);
    }
    screens[0] = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    screens[1] = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    text_buffer = C2D_TextBufNew(4096);
    measure_buffer = C2D_TextBufNew(512);
    background = C2D_Color32(12, 19, 32, 255);
    card = C2D_Color32(23, 35, 53, 255);
    ink = C2D_Color32(236, 242, 251, 255);
    muted = C2D_Color32(139, 158, 183, 255);
    accent = C2D_Color32(61, 217, 180, 255);
    return screens[0] && screens[1] && text_buffer && measure_buffer && ui_font;
}

void native_ui_draw(const NativeUI *ui) {
    static char previous_notice[160];
    static uint64_t notice_until;
    language = ui->language;
    C2D_TextBufClear(text_buffer);
    if (strcmp(previous_notice, ui->notice)) {
        snprintf(previous_notice, sizeof(previous_notice), "%s", ui->notice);
        notice_until = osGetTime() + 6000;
    }
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(screens[0], background);
    C2D_SceneBegin(screens[0]);
    rect(18, 15, 5, 27, accent);
    text(33, 7, 1.20f, ink, "n3ds cast", 226);
    text(264, 19, .66f, muted, "START: exit", 122);
    text(264, 0, .66f, muted, "v" NATIVE_APP_VERSION, 122);
    panel(14, 52, 372, 94, card);
    bool live = ui->status.state == NATIVE_STREAMING;
    const char *state = live                                    ? "LIVE"
                        : ui->status.state == NATIVE_WAIT_GAME  ? "READY FOR GAME"
                        : ui->status.state == NATIVE_CONNECTING ? "CONNECTING"
                        : ui->status.state == NATIVE_ERROR      ? "CONNECTION ERROR"
                                                                : "READY";
    text(28, 56, .95f, live ? accent : ink, state, 344);
    const char *message = ui->notice[0] && osGetTime() < notice_until ? ui->notice : ui->status.message;
    wrapped_text(28, 91, .75f, muted, message, 344, 2);
    char details[160];
    snprintf(details, sizeof(details), native_tr(language, "%lu FPS / Q%d / target %d"), (unsigned long)ui->status.fps,
             ui->cfg.jpeg_quality ? ui->cfg.jpeg_quality : (int)ui->status.quality, ui->cfg.target_fps);
    text(18, 153, .72f, ink, details, 364);
    const char *audio[] = {"Audio: off", "Audio: waiting for game DSP", "Audio: DSP layout not identified",
                           "Audio: streaming game mix"};
    text(18, 180, .66f, muted, audio[ui->status.audio_state < 4 ? ui->status.audio_state : 0], 364);
    if (ui->status.worker_core) {
        char core[96];
        if (ui->status.core_fallback) {
            snprintf(core, sizeof(core), "%s", native_tr(language, "Encoder: core 1 (core 2 unavailable)"));
        } else {
            snprintf(core, sizeof(core), native_tr(language, "Encoder: core %lu"),
                     (unsigned long)ui->status.worker_core);
        }
        text(18, 205, .66f, muted, core, 364);
    }

    C2D_TargetClear(screens[1], background);
    C2D_SceneBegin(screens[1]);
    const char *tabs[] = {"CONTROL", "DEVICES", "SETTINGS"};
    for (unsigned i = 0; i < 3; i++) {
        text(12 + i * 104, 7, .80f, ui->tab == i ? accent : muted, tabs[i], 98);
        if (ui->tab == i) {
            rect(12 + i * 104, 39, 96, 3, accent);
        }
    }
    bool active = ui->status.state == NATIVE_STREAMING || ui->status.state == NATIVE_WAIT_GAME ||
                  ui->status.state == NATIVE_CONNECTING;
    if (ui->tab == 0) {
        panel(12, 50, 296, 60, active ? C2D_Color32(181, 65, 82, 255) : accent);
        text(28, 61, 1.10f, active ? ink : background, active ? "Stop [Y]" : "Start [Y]", 264);
        panel(12, 120, 296, 52, card);
        text(26, 131, .88f, ink, ui->scanning ? "Searching..." : "Find TV [SELECT]", 268);
        if (ui->show_timings) {
            panel(12, 180, 296, 58, card);
            snprintf(details, sizeof(details), "R%lu J%lu T%lu ms/f", (unsigned long)ui->status.capture_ms,
                     (unsigned long)ui->status.encode_ms, (unsigned long)ui->status.send_ms);
            text(22, 182, .78f, accent, details, 276);
            text(22, 211, .66f, muted, "R: capture  J: JPEG  T: send", 276);
        } else {
            text(16, 181, .78f, ink, ui->selected_name[0] ? ui->selected_name : "Selected TV", 288);
            text(16, 210, .72f, muted, ui->cfg.tv_ip, 288);
        }
    } else if (ui->tab == 1) {
        text(16, 47, .72f, muted, ui->scanning ? "Searching..." : "Select TV [A]", 288);
        if (!ui->tv_count) {
            wrapped_text(18, 93, .78f, ink, "No TV found. SELECT to scan again.", 284, 3);
        }
        unsigned first = (ui->selected_tv / NATIVE_TVS_PER_PAGE) * NATIVE_TVS_PER_PAGE;
        for (unsigned i = first; i < ui->tv_count && i < first + NATIVE_TVS_PER_PAGE; i++) {
            int y = 78 + (i - first) * 66;
            panel(12, y, 296, 58, ui->selected_tv == i ? C2D_Color32(32, 70, 78, 255) : card);
            text(24, y + 1, .80f, ink, ui->tvs[i].name, 272);
            text(24, y + 29, .72f, muted, ui->tvs[i].ip, 272);
        }
        if (ui->tv_count) {
            page_buttons(first / NATIVE_TVS_PER_PAGE, (ui->tv_count + NATIVE_TVS_PER_PAGE - 1) / NATIVE_TVS_PER_PAGE,
                         false);
        }
    } else {
        static const char *labels[] = {"TV IP",      "Screens", "Filter",     "Aspect",     "Quality",
                                       "Target FPS", "TV OSD",  "Game audio", "Audio rate", "Language",
                                       "CPU core",   "Capture", "Network"};
        static const char *layouts[] = {"Top", "Bottom", "Vertical", "Side by side", "PiP"};
        char values[NATIVE_SETTINGS_COUNT][64] = {0};
        snprintf(values[0], 64, "%s", ui->cfg.tv_ip);
        snprintf(values[1], 64, "%s", layouts[ui->cfg.layout_mode]);
        snprintf(values[2], 64, "%s", ui->cfg.scale_filter ? "Smooth" : "Pixelated");
        snprintf(values[3], 64, "%s", ui->cfg.stretch_mode ? "Stretch" : "Keep aspect");
        if (ui->cfg.jpeg_quality) {
            snprintf(values[4], 64, "%d", ui->cfg.jpeg_quality);
        } else {
            snprintf(values[4], 64, "Auto");
        }
        snprintf(values[5], 64, "%d", ui->cfg.target_fps);
        snprintf(values[6], 64, "%s", ui->cfg.show_osd ? "On" : "Off");
        snprintf(values[7], 64, "%s", ui->cfg.audio_enabled ? "Game mix" : "Off");
        snprintf(values[8], 64, "%d kbps", ui->cfg.audio_kbps);
        snprintf(values[9], 64, "%s", native_language_name(language));
        static const char *cores[] = {"Auto", "Core 1", "Core 2"};
        snprintf(values[10], 64, "%s", cores[ui->cpu_core < NATIVE_CORE_COUNT ? ui->cpu_core : 0]);
        snprintf(values[11], 64, "%s", ui->capture_source ? "Always" : "Games only");
        if (ui->cfg.link_kbps) {
            snprintf(values[12], 64, "%d.%02d MB/s", ui->cfg.link_kbps / 1000, ui->cfg.link_kbps % 1000 / 10);
        } else {
            snprintf(values[12], 64, "Auto");
        }
        unsigned first = (ui->selected_setting / NATIVE_SETTINGS_PER_PAGE) * NATIVE_SETTINGS_PER_PAGE;
        for (unsigned i = first; i < NATIVE_SETTINGS_COUNT && i < first + NATIVE_SETTINGS_PER_PAGE; i++) {
            unsigned y = 46 + (i - first) * 40;
            if (ui->selected_setting == i) {
                panel(12, y, 296, 36, card);
            }
            text(20, y + 5, .78f, ui->selected_setting == i ? accent : muted, labels[i], 106);
            text(134, y + 5, .78f, ink, values[i], 166);
        }
        page_buttons(first / NATIVE_SETTINGS_PER_PAGE,
                     (NATIVE_SETTINGS_COUNT + NATIVE_SETTINGS_PER_PAGE - 1) / NATIVE_SETTINGS_PER_PAGE, true);
    }
    C3D_FrameEnd(0);
}

void native_ui_exit(void) {
    if (text_buffer) {
        C2D_TextBufDelete(text_buffer);
    }
    if (measure_buffer) {
        C2D_TextBufDelete(measure_buffer);
    }
    if (ui_font) {
        C2D_FontFree(ui_font);
    }
    C2D_Fini();
    C3D_Fini();
}
