#include "overlay_pixels.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    for (unsigned format = 0; format < 5; format++) {
        unsigned bpp = format == 0 ? 4 : format == 1 ? 3 : 2;
        NativeFrame f = {.address = 0x20001000, .width = 320, .format = format, .stride = 240 * bpp + 16};
        size_t size = native_frame_span(&f);
        uint8_t *allocation = malloc(size + 2), *p = allocation + 1, *original = malloc(size);
        memset(allocation, 0xA7, size + 2);
        memcpy(original, p, size);
        NativeOverlayPixels state = {0};
        assert(!native_overlay_paint(&state, &f, p, size - 1, "R1"));
        assert(native_overlay_paint(&state, &f, p, size, "R 13 J 19 T 15 24FPS"));
        assert(memcmp(original, p, size));
        for (unsigned x = 0; x < 320; x++) {
            for (unsigned byte = 0; byte < f.stride && x * f.stride + byte < size; byte++) {
                unsigned y = byte / bpp;
                bool inside = x >= NATIVE_OSD_X && x < NATIVE_OSD_X + NATIVE_OSD_WIDTH && y < 18;
                if (!inside) {
                    assert(p[x * f.stride + byte] == original[x * f.stride + byte]);
                }
            }
        }
        assert(native_overlay_paint(&state, &f, p, size, "R  2 J 20 T 10 28FPS"));
        size_t changed = NATIVE_OSD_X * f.stride;
        memset(p + changed, 0x63, bpp);
        assert(native_overlay_restore(&state, &f, p, size));
        memset(original + changed, 0x63, bpp);
        assert(!memcmp(original, p, size));
        assert(allocation[0] == 0xA7 && allocation[size + 1] == 0xA7);
        free(original);
        free(allocation);
    }
    puts("PASS: overlay five formats, padded strides, strip boundaries, repeated draws and conditional restore");
}
