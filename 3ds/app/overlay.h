#pragma once
#include <3ds.h>
#include "frame.h"
#include "overlay_pixels.h"
typedef struct {
    NativeFrame frame;
    uint8_t *pixels;
    void *mapping;
    size_t size, mapped_size;
    uint32_t source;
    NativeOverlayPixels saved;
} NativeOverlayBuffer;
typedef struct {
    NativeCapture capture;
    uint32_t pid;
    NativeOverlayBuffer buffers[2];
    Result error;
} NativeOverlay;
// Return ACTIVE / WAITING / UNAVAILABLE. No game pause, input hooks or GPU register writes.
unsigned native_overlay_update(NativeOverlay *overlay, const char *label);
void native_overlay_close(NativeOverlay *overlay);
