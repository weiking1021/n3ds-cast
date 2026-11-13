#include "overlay_pixels.h"
#include <string.h>
// Compact geometric 5x7 glyphs; only the numeric RJT/FPS line needs a font.
static const uint8_t glyphs[][7] = {
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
    {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},   {14, 16, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},   {30, 17, 17, 30, 20, 18, 17}, // R
    {7, 2, 2, 2, 2, 18, 12},      {31, 4, 4, 4, 4, 4, 4},                                     // J,T
    {31, 16, 16, 30, 16, 16, 16}, {30, 17, 17, 30, 16, 16, 16}, {15, 16, 16, 14, 1, 1, 30}    // F,P,S
};
static unsigned glyph_row(char c, unsigned y) {
    const char *letters = "RJTFPS";
    if (c >= '0' && c <= '9') {
        return glyphs[c - '0'][y];
    }
    const char *found = strchr(letters, c);
    return found && c ? glyphs[10 + (found - letters)][y] : 0;
}
static unsigned pixel_bytes(const NativeFrame *f, size_t size) {
    static const unsigned bytes[] = {4, 3, 2, 2, 2};
    size_t span = f ? native_frame_span(f) : 0;
    return span && span <= size && f->width == 320 ? bytes[f->format] : 0;
}
bool native_overlay_paint(NativeOverlayPixels *s, const NativeFrame *f, uint8_t *pixels, size_t size,
                          const char *label) {
    unsigned bpp = pixel_bytes(f, size);
    if (!s || !pixels || !label || !bpp) {
        return false;
    }
    size_t length = strlen(label);
    if (length > 23) {
        length = 23;
    }
    for (unsigned x = 0; x < NATIVE_OSD_WIDTH; x++) {
        for (unsigned y = 0; y < NATIVE_OSD_HEIGHT; y++) {
            unsigned at = (x * NATIVE_OSD_HEIGHT + y) * 4;
            uint8_t *p = pixels + (NATIVE_OSD_X + x) * f->stride + (239 - NATIVE_OSD_Y - y) * bpp;
            if (!s->valid || memcmp(p, s->painted + at, bpp)) {
                memcpy(s->original + at, p, bpp);
            }
            bool lit = false;
            if (x >= 4 && y >= 2 && y < 16) {
                unsigned text_x = (x - 4) / 2, index = text_x / 6, column = text_x % 6;
                if (index < length && column < 5) {
                    lit = (glyph_row(label[index], (y - 2) / 2) & (16u >> column)) != 0;
                }
            }
            uint8_t color[4] = {0};
            if (lit) {
                memset(color, 255, bpp);
            } else if (f->format == 0) {
                color[0] = 255;
            } else if (f->format == 3) {
                color[0] = 1;
            } else if (f->format == 4) {
                color[0] = 15;
            }
            memcpy(p, color, bpp);
            memcpy(s->painted + at, color, bpp);
        }
    }
    s->valid = true;
    return true;
}
bool native_overlay_restore(NativeOverlayPixels *s, const NativeFrame *f, uint8_t *pixels, size_t size) {
    unsigned bpp = pixel_bytes(f, size);
    if (!s || !pixels || !bpp || !s->valid) {
        return false;
    }
    for (unsigned x = 0; x < NATIVE_OSD_WIDTH; x++) {
        for (unsigned y = 0; y < NATIVE_OSD_HEIGHT; y++) {
            unsigned at = (x * NATIVE_OSD_HEIGHT + y) * 4;
            uint8_t *p = pixels + (NATIVE_OSD_X + x) * f->stride + (239 - NATIVE_OSD_Y - y) * bpp;
            if (!memcmp(p, s->painted + at, bpp)) {
                memcpy(p, s->original + at, bpp);
            }
        }
    }
    s->valid = false;
    return true;
}
