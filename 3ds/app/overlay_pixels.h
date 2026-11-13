#pragma once
#include "frame.h"
#define NATIVE_OSD_X 4u
#define NATIVE_OSD_Y 222u
#define NATIVE_OSD_WIDTH 288u
#define NATIVE_OSD_HEIGHT 18u
#define NATIVE_OSD_PIXELS (NATIVE_OSD_WIDTH * NATIVE_OSD_HEIGHT)
typedef struct {
    uint8_t original[NATIVE_OSD_PIXELS * 4], painted[NATIVE_OSD_PIXELS * 4];
    bool valid;
} NativeOverlayPixels;
bool native_overlay_paint(NativeOverlayPixels *state, const NativeFrame *frame, uint8_t *pixels, size_t size,
                          const char *label);
bool native_overlay_restore(NativeOverlayPixels *state, const NativeFrame *frame, uint8_t *pixels, size_t size);
