#ifndef N3DS_REFCAP_JPEG_H
#define N3DS_REFCAP_JPEG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TOP_SCREEN_WIDTH 400
#define TOP_SCREEN_HEIGHT 240
#define BOT_SCREEN_WIDTH 320
#define BOT_SCREEN_HEIGHT 240

/**
 * Convert a rotated 3DS BGR8 framebuffer (240xW column-major, bottom-to-top)
 * into a standard upright RGB24 buffer (Wx240 row-major, top-to-bottom).
 */
void refcap_convert_3ds_bgr8_to_rgb24(const uint8_t *src_bgr8, uint8_t *dst_rgb24, int width, int height);

/**
 * Convert a rotated 3DS RGB565 framebuffer (240xW column-major, bottom-to-top)
 * into a standard upright RGB24 buffer (Wx240 row-major, top-to-bottom).
 */
void refcap_convert_3ds_rgb565_to_rgb24(const uint8_t *src_rgb565, uint8_t *dst_rgb24, int width, int height);

/**
 * Fast single-pass YCbCr 4:2:0 JPEG encoder reading directly from a 3DS BGR8
 * column-major framebuffer (400x240 or 320x240, width & height multiples of 16).
 */
size_t refcap_encode_3ds_bgr8_jpeg_420(const uint8_t *src_bgr8, int width, int height, int quality, uint8_t *out_jpeg,
                                       size_t out_capacity);

/**
 * Compress an upright RGB24 image (width x height, 3 bytes per pixel)
 * into a standard baseline YCbCr 4:4:4 JPEG in memory (ideal for crisp UI text).
 * Returns number of bytes written to out_jpeg, or 0 on error.
 */
size_t refcap_encode_jpeg_rgb24(const uint8_t *rgb24, int width, int height, int quality, uint8_t *out_jpeg,
                                size_t out_capacity);

/**
 * Base64 encode binary buffer into null-terminated string.
 * Returns string length (excluding null terminator), or 0 on overflow.
 */
size_t refcap_base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_capacity);

#endif // N3DS_REFCAP_JPEG_H
