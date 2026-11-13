#include "frame.h"

size_t native_frame_span(const NativeFrame *f) {
    static const unsigned bytes[] = {4, 3, 2, 2, 2};
    if (!f || f->format > 4 || (f->width != 320 && f->width != 400)) {
        return 0;
    }
    unsigned column = 240 * bytes[f->format];
    if (f->stride < column || f->stride > 4096 || !f->address) {
        return 0;
    }
    return (f->width - 1) * (size_t)f->stride + column;
}
bool native_frame_rgb(const NativeFrame *f, const uint8_t *raw, size_t raw_size, uint8_t *rgb, size_t rgb_size) {
    size_t span = native_frame_span(f);
    if (!span || !raw || !rgb || raw_size < span || rgb_size < (size_t)f->width * 240 * 3) {
        return false;
    }
    static const unsigned bytes[] = {4, 3, 2, 2, 2};
    for (unsigned x = 0; x < f->width; x++) {
        for (unsigned y = 0; y < 240; y++) {
            const uint8_t *p = raw + x * f->stride + (239 - y) * bytes[f->format];
            uint8_t *out = rgb + (y * f->width + x) * 3;
            unsigned v = p[0] | ((unsigned)p[1] << 8);
            switch (f->format) {
                case 0:
                    out[0] = p[3];
                    out[1] = p[2];
                    out[2] = p[1];
                    break;
                case 1:
                    out[0] = p[2];
                    out[1] = p[1];
                    out[2] = p[0];
                    break;
                case 2:
                    out[0] = ((v >> 11) * 255 + 15) / 31;
                    out[1] = (((v >> 5) & 63) * 255 + 31) / 63;
                    out[2] = ((v & 31) * 255 + 15) / 31;
                    break;
                case 3:
                    out[0] = ((v >> 11) * 255 + 15) / 31;
                    out[1] = (((v >> 6) & 31) * 255 + 15) / 31;
                    out[2] = (((v >> 1) & 31) * 255 + 15) / 31;
                    break;
                case 4:
                    out[0] = (v >> 12) * 17;
                    out[1] = ((v >> 8) & 15) * 17;
                    out[2] = ((v >> 4) & 15) * 17;
                    break;
            }
        }
    }
    return true;
}
