#include "frame.h"
#include "protocol.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int main(void) {
    for (unsigned width = 320; width <= 400; width += 80) {
        for (unsigned format = 0; format < 5; format++) {
            const unsigned bytes[] = {4, 3, 2, 2, 2};
            NativeFrame f = {
                .address = 0x18000000, .stride = 240 * bytes[format] + 32, .format = format, .width = width};
            size_t span = native_frame_span(&f), size = width * 240 * 3;
            unsigned char *raw = malloc(span), *before = malloc(span), *guard = malloc(size + 2);
            memset(raw, 0, span);
            memset(guard, 0xC7, size + 2);
            // A red pixel at upright top-left, blue at bottom-right. Test all five
            // formats independently, including alpha and stride padding.
            uint8_t *red = raw + 239 * bytes[format], *blue = raw + (width - 1) * f.stride;
            if (format == 0) {
                red[3] = 255;
                blue[1] = 255;
            } else if (format == 1) {
                red[2] = 255;
                blue[0] = 255;
            } else {
                unsigned rv = format == 4 ? 0xF00F : format == 3 ? 0xF801 : 0xF800;
                unsigned bv = format == 4 ? 0x00FF : format == 3 ? 0x003F : 0x001F;
                red[0] = rv;
                red[1] = rv >> 8;
                blue[0] = bv;
                blue[1] = bv >> 8;
            }
            memcpy(before, raw, span);
            assert(native_frame_rgb(&f, raw, span, guard + 1, size));
            assert(guard[0] == 0xC7 && guard[size + 1] == 0xC7);
            assert(guard[1] == 255 && guard[2] == 0 && guard[3] == 0);
            assert(guard[size - 2] == 0 && guard[size - 1] == 0 && guard[size] == 255);
            assert(!memcmp(before, raw, span));
            assert(!native_frame_rgb(&f, raw, span - 1, guard + 1, size));
            assert(!native_frame_rgb(&f, raw, span, guard + 1, size - 1));
            f.stride = 4097;
            assert(!native_frame_span(&f));
            f.stride = 240 * bytes[format] - 1;
            assert(!native_frame_span(&f));
            free(raw);
            free(before);
            free(guard);
        }
    }
    CastConfig cfg = {0};
    strcpy(cfg.tv_ip, "192.168.0.9");
    strcpy(cfg.app_id, "01B7A380");
    cfg.jpeg_quality = 50;
    cfg.target_fps = 30;
    assert(native_config_valid(&cfg));
    cfg.target_fps = 0;
    assert(!native_config_valid(&cfg));
    cfg.target_fps = 30;
    cfg.jpeg_quality = NATIVE_QUALITY_AUTO;
    assert(native_config_valid(&cfg));
    cfg.jpeg_quality = 20;
    assert(!native_config_valid(&cfg));
    cfg.jpeg_quality = 100;
    assert(native_config_valid(&cfg));
    cfg.jpeg_quality = 50;
    memset(cfg.tv_ip, 'x', sizeof(cfg.tv_ip));
    assert(!native_config_valid(&cfg));
    puts(
        "PASS: all 5 native game pixel formats, both screen widths, stride, orientation, colors, read-only inputs, output guards, malformed IPC config");
}
