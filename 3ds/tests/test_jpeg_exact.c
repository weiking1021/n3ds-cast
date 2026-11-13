// New capture_jpeg.c must be byte-identical to the previous implementation (tests/ref_capture_jpeg.c).
#include "capture_jpeg.h"
#include "refcap_jpeg.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint32_t rng = 12345;
static uint32_t rnd(void) {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}
// column-major BGR8 like the 3DS framebuffer: width columns of 240 rows
static void make_image(uint8_t *img, int width, int kind) {
    for (int x = 0; x < width; x++) {
        for (int y = 0; y < 240; y++) {
            uint8_t *p = img + ((size_t)x * 240 + y) * 3;
            int r, g, b;
            switch (kind) {
                case 0:
                    r = rnd() & 255;
                    g = rnd() & 255;
                    b = rnd() & 255;
                    break; // noise
                case 1:
                    r = x * 255 / width;
                    g = y * 255 / 240;
                    b = (x + y) & 255;
                    break; // gradient
                case 2:
                    r = g = b = 128;
                    break; // flat
                case 3:
                    r = (x / 8 + y / 8) & 1 ? 250 : 10;
                    g = (x / 16) & 1 ? 200 : 20;
                    b = (y / 4) & 1 ? 90 : 30;
                    break; // checkers / text-like
                case 4: {
                    int bg = (y < 100) ? 200 : 60;
                    r = bg;
                    g = bg + 20;
                    b = 255 - bg;
                    if (((x * 7 + y * 3) % 53) < 4) {
                        r = g = b = 0;
                    }
                    if (x > 150 && x < 210 && y > 80 && y < 150) {
                        r = rnd() & 255;
                        g = rnd() & 255;
                        b = 40;
                    }
                } break; // sky/ground + sprite + noise patch
                default:
                    r = (x * 3) & 255;
                    g = (y * 5) & 255;
                    b = ((x ^ y) & 255);
                    break;
            }
            p[0] = (uint8_t)b;
            p[1] = (uint8_t)g;
            p[2] = (uint8_t)r;
        }
    }
}
static double now(void) {
    return (double)clock() / CLOCKS_PER_SEC;
}
int main(void) {
    static uint8_t img[400 * 240 * 3], a[60000], b[60000];
    unsigned cases = 0;
    double tn = 0, to = 0;
    for (int width = 320; width <= 400; width += 80) {
        for (int kind = 0; kind < 6; kind++) {
            make_image(img, width, kind);
            for (int q = 30; q <= 95; q += 13) {
                size_t na = capture_encode_3ds_bgr8_jpeg_420(img, width, 240, q, a, sizeof(a));
                size_t nb = refcap_encode_3ds_bgr8_jpeg_420(img, width, 240, q, b, sizeof(b));
                assert(na == nb);
                assert(na == 0 || !memcmp(a, b, na));
                cases++;
                if (na) { // capacity edge cases: exact fit, one short, much smaller (slow path + overflow accounting)
                    size_t caps[] = {na, na - 1, na + 5, na / 2, 100};
                    for (unsigned c = 0; c < 5; c++) {
                        memset(a, 0xAA, sizeof(a));
                        memset(b, 0xAA, sizeof(b));
                        size_t xa = capture_encode_3ds_bgr8_jpeg_420(img, width, 240, q, a, caps[c]);
                        size_t xb = refcap_encode_3ds_bgr8_jpeg_420(img, width, 240, q, b, caps[c]);
                        assert(xa == xb);
                        assert(!memcmp(a, b, caps[c]));
                        cases++;
                    }
                }
            }
            double t0 = now();
            for (int i = 0; i < 20; i++) {
                capture_encode_3ds_bgr8_jpeg_420(img, width, 240, 70, a, sizeof(a));
            }
            double t1 = now();
            for (int i = 0; i < 20; i++) {
                refcap_encode_3ds_bgr8_jpeg_420(img, width, 240, 70, b, sizeof(b));
            }
            double t2 = now();
            tn += t1 - t0;
            to += t2 - t1;
            printf("  width %d kind %d: new %.2f ms  old %.2f ms  (%zu bytes)\n", width, kind, (t1 - t0) * 50,
                   (t2 - t1) * 50, capture_encode_3ds_bgr8_jpeg_420(img, width, 240, 70, a, sizeof(a)));
        }
    }
    printf("PASS: %u byte-identical encodes incl. capacity edge cases; host speed new/old = %.2f\n", cases, tn / to);
    return 0;
}
