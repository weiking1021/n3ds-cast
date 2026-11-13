#ifndef N3DS_CAPTURE_JPEG_H
#define N3DS_CAPTURE_JPEG_H

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
void capture_convert_3ds_bgr8_to_rgb24(const uint8_t *src_bgr8, uint8_t *dst_rgb24, int width, int height);

/**
 * Convert a rotated 3DS RGB565 framebuffer (240xW column-major, bottom-to-top)
 * into a standard upright RGB24 buffer (Wx240 row-major, top-to-bottom).
 */
void capture_convert_3ds_rgb565_to_rgb24(const uint8_t *src_rgb565, uint8_t *dst_rgb24, int width, int height);

/**
 * Fast single-pass YCbCr 4:2:0 JPEG encoder reading directly from a 3DS BGR8
 * column-major framebuffer (400x240 or 320x240, width & height multiples of 16).
 */
size_t capture_encode_3ds_bgr8_jpeg_420(const uint8_t *src_bgr8, int width, int height, int quality, uint8_t *out_jpeg,
                                        size_t out_capacity);

/**
 * Encodes the width x height rectangle at (x0, y0) (all multiples of 16) of a column-major BGR8 framebuffer whose
 * columns are column_rows pixels tall, as a standalone JPEG. Each 16x16 block gets the same quantised
 * coefficients as in the full-frame encoder, so the area decodes to the same pixels. Returns 0 on error.
 */
size_t capture_encode_3ds_bgr8_jpeg_420_rect(const uint8_t *src_bgr8, int column_rows, int x0, int y0, int width,
                                             int height, int quality, uint8_t *out_jpeg, size_t out_capacity);

/**
 * Compress an upright RGB24 image (width x height, 3 bytes per pixel)
 * into a standard baseline YCbCr 4:4:4 JPEG in memory (ideal for crisp UI text).
 * Returns number of bytes written to out_jpeg, or 0 on error.
 */
size_t capture_encode_jpeg_rgb24(const uint8_t *rgb24, int width, int height, int quality, uint8_t *out_jpeg,
                                 size_t out_capacity);

/**
 * Base64 encode binary buffer into null-terminated string.
 * Returns string length (excluding null terminator), or 0 on overflow.
 */
size_t capture_base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_capacity);

#endif // N3DS_CAPTURE_JPEG_H
