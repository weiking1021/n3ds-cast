#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef struct {
    uint32_t address, stride, format, bank, width;
} NativeFrame;
size_t native_frame_span(const NativeFrame *frame);
bool native_frame_rgb(const NativeFrame *frame, const uint8_t *raw, size_t raw_size, uint8_t *rgb, size_t rgb_size);
#ifdef __3DS__
#include <3ds.h>
typedef struct {
    Handle game;
    uint32_t pid;
    uint64_t title_id;
    uint64_t next_search;
} NativeCapture;
bool native_capture_game(NativeCapture *capture);
bool native_capture_frame(NativeCapture *capture, bool top, NativeFrame *frame, uint8_t *raw, size_t capacity);
void native_capture_close(NativeCapture *capture);
// Continuous mode: read whatever each screen shows. The process owning an
// FCRAM framebuffer is located by physical-address match, per screen.
typedef struct {
    Handle owner[2];
    uint64_t title_id[2];
    uint64_t next_search[2];
} NativeScreenCapture;
bool native_capture_screen(NativeScreenCapture *capture, bool top, NativeFrame *frame, uint8_t *raw, size_t capacity);
void native_capture_screen_close(NativeScreenCapture *capture);
#endif
