#pragma once
#include <stdint.h>
// Partial screen updates: a screen is cut into 16x16-pixel tiles (one JPEG MCU). The tiles that changed since the
// previous frame are covered by a few rectangles, each sent as its own small JPEG.
#define NATIVE_TILE 16
#define NATIVE_TILES_X_MAX 25 // 400 / 16
#define NATIVE_TILES_Y 15     // 240 / 16
#define NATIVE_MAX_RECTS 8
typedef struct {
    uint8_t x, y, w, h;
} NativeTileRect; // in tiles, y counted from the top of the screen
typedef struct {
    unsigned count, cost;
    NativeTileRect r[NATIVE_MAX_RECTS];
} NativePlan;
// dirty: tiles_y rows of tiles_x bytes (row-major, non-zero = changed). Estimated cost of one rectangle is
// header_cost + tiles * tile_cost (bytes). Cuts the changed area apart along fully unchanged rows/columns when that
// is cheaper than one bounding box, using at most NATIVE_MAX_RECTS rectangles. count 0 = nothing changed.
void native_plan_rects(const uint8_t *dirty, unsigned tiles_x, unsigned tiles_y, unsigned header_cost,
                       unsigned tile_cost, NativePlan *plan);
