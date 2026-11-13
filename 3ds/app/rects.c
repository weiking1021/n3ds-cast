#include "rects.h"
#include <string.h>

typedef struct {
    const uint8_t *dirty;
    unsigned tx, ty, header, tile;
} Ctx;

static int at(const Ctx *c, int x, int y) {
    return c->dirty[y * c->tx + x] != 0;
}

// Best cover of the changed tiles inside [x0,x1) x [y0,y1): one bounding box, or two covers separated by an
// all-clear row or column (recursively, bounded by the rectangle limit).
static void solve(const Ctx *c, int x0, int y0, int x1, int y1, unsigned depth, NativePlan *out) {
    int bx0 = x1, by0 = y1, bx1 = -1, by1 = -1;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (at(c, x, y)) {
                if (x < bx0) {
                    bx0 = x;
                }
                if (x > bx1) {
                    bx1 = x;
                }
                if (y < by0) {
                    by0 = y;
                }
                if (y > by1) {
                    by1 = y;
                }
            }
        }
    }
    if (bx1 < 0) {
        out->count = 0;
        out->cost = 0;
        return;
    }
    NativePlan best;
    best.count = 1;
    best.r[0] = (NativeTileRect){(uint8_t)bx0, (uint8_t)by0, (uint8_t)(bx1 - bx0 + 1), (uint8_t)(by1 - by0 + 1)};
    best.cost = c->header + (unsigned)((bx1 - bx0 + 1) * (by1 - by0 + 1)) * c->tile;
    if (depth < 3) {
        for (int pass = 0; pass < 2; pass++) { // pass 0: cut at a clear column, pass 1: at a clear row
            int lo = pass ? by0 : bx0, hi = pass ? by1 : bx1;
            for (int cut = lo + 1; cut < hi; cut++) {
                int clear = 1;
                for (int k = pass ? bx0 : by0; k <= (pass ? bx1 : by1) && clear; k++) {
                    if (pass ? at(c, k, cut) : at(c, cut, k)) {
                        clear = 0;
                    }
                }
                if (!clear) {
                    continue;
                }
                int previous_clear = 1; // only the first line of a clear band: the rest cut the same way
                if (cut - 1 > lo) {
                    for (int k = pass ? bx0 : by0; k <= (pass ? bx1 : by1) && previous_clear; k++) {
                        if (pass ? at(c, k, cut - 1) : at(c, cut - 1, k)) {
                            previous_clear = 0;
                        }
                    }
                    if (previous_clear) {
                        continue;
                    }
                }
                NativePlan a, b;
                if (pass) {
                    solve(c, bx0, by0, bx1 + 1, cut, depth + 1, &a);
                    solve(c, bx0, cut, bx1 + 1, by1 + 1, depth + 1, &b);
                } else {
                    solve(c, bx0, by0, cut, by1 + 1, depth + 1, &a);
                    solve(c, cut, by0, bx1 + 1, by1 + 1, depth + 1, &b);
                }
                if (a.count + b.count > NATIVE_MAX_RECTS || a.cost + b.cost >= best.cost) {
                    continue;
                }
                best.count = 0;
                best.cost = a.cost + b.cost;
                for (unsigned i = 0; i < a.count; i++) {
                    best.r[best.count++] = a.r[i];
                }
                for (unsigned i = 0; i < b.count; i++) {
                    best.r[best.count++] = b.r[i];
                }
            }
        }
    }
    *out = best;
}

void native_plan_rects(const uint8_t *dirty, unsigned tiles_x, unsigned tiles_y, unsigned header_cost,
                       unsigned tile_cost, NativePlan *plan) {
    memset(plan, 0, sizeof(*plan));
    if (!dirty || !tiles_x || !tiles_y || tiles_x > NATIVE_TILES_X_MAX || tiles_y > NATIVE_TILES_Y) {
        return;
    }
    Ctx c = {dirty, tiles_x, tiles_y, header_cost, tile_cost};
    solve(&c, 0, 0, (int)tiles_x, (int)tiles_y, 0, plan);
}
