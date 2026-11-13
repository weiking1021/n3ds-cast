#include "video.h"
#include "capture_jpeg.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    uint8_t *raw = malloc(400 * 4096), *rgb = malloc(400 * 240 * 3), *reference = malloc(400 * 720);
    static uint8_t jpeg[NATIVE_JPEG_MAX], expected[NATIVE_JPEG_MAX];
    static char encoded[NATIVE_B64_MAX], expected_b64[NATIVE_B64_MAX];
    NativeVideoCache cache = {.bgr = calloc(400 * 240, 3), .base64 = encoded};
    for (unsigned format = 0; format < 5; format++) {
        for (unsigned width = 320; width <= 400; width += 80) {
            unsigned pixel_size[] = {4, 3, 2, 2, 2};
            NativeFrame f = {.address = 1, .format = format, .width = width, .stride = 240 * pixel_size[format] + 8};
            for (size_t i = 0; i < 400 * 4096; i++) {
                raw[i] = (uint8_t)(i * 17 + (i / 23));
            }
            assert(native_frame_rgb(&f, raw, 400 * 4096, rgb, 400 * 240 * 3));
            for (unsigned x = 0; x < width; x++) {
                for (unsigned y = 0; y < 240; y++) {
                    const uint8_t *p = rgb + ((239 - y) * width + x) * 3;
                    uint8_t *bgr = reference + (x * 240 + y) * 3;
                    bgr[0] = p[2];
                    bgr[1] = p[1];
                    bgr[2] = p[0];
                }
            }
            cache.valid = false;
            assert(native_video_encode(&cache, &f, raw, 400 * 4096, 50, jpeg, sizeof(jpeg)) == 1);
            size_t bytes = capture_encode_3ds_bgr8_jpeg_420(reference, width, 240, 50, expected, sizeof(expected));
            assert(bytes && capture_base64_encode(expected, bytes, expected_b64, sizeof(expected_b64)));
            assert(!strcmp(encoded, expected_b64));
            assert(native_video_encode(&cache, &f, raw, 400 * 4096, 50, jpeg, sizeof(jpeg)) == 0);
            raw[240 * pixel_size[format]] = 123; // Padding must not invalidate visible pixels.
            assert(native_video_encode(&cache, &f, raw, 400 * 4096, 50, jpeg, sizeof(jpeg)) == 0);
            assert(native_video_encode(&cache, &f, raw, 400 * 4096, 55, jpeg, sizeof(jpeg)) == 1);
            assert(native_video_encode(&cache, &f, raw, 1, 55, jpeg, sizeof(jpeg)) == -1);
        }
    }
    NativeFrame tight = {.address = 1, .format = 1, .width = 400, .stride = 720};
    cache.valid = false;
    assert(native_video_encode(&cache, &tight, raw, 400 * 4096, 30, jpeg, sizeof(jpeg)) == 1);
    assert(native_video_encode(&cache, &tight, raw, 400 * 4096, 30, jpeg, sizeof(jpeg)) == 0);
    raw[0] ^= 1;
    assert(native_video_encode(&cache, &tight, raw, 400 * 4096, 30, jpeg, sizeof(jpeg)) == 1);
    cache.valid = false;
    assert(native_video_encode(&cache, &tight, raw, 400 * 4096, 30, jpeg, 8) == -1 && !cache.valid);
    free(cache.bgr);
    free(raw);
    free(rgb);
    free(reference);
    puts(
        "PASS: proven 4:2:0 encoder byte-identical across five formats, both widths, padded/tight stride, unchanged cache, quality changes and overflow recovery");
}
