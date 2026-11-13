#include "video.h"
#include "capture_jpeg.h"
#include <string.h>
#ifdef __3DS__
#include <3ds.h>
#define VIDEO_TICK() ((unsigned long long)svcGetSystemTick())
#else
#define VIDEO_TICK() 0ULL
#endif

static void bgr_pixel(const uint8_t *p, unsigned format, uint8_t *out) {
    unsigned v = p[0] | ((unsigned)p[1] << 8);
    switch (format) {
        case 0:
            out[0] = p[1];
            out[1] = p[2];
            out[2] = p[3];
            break;
        case 1:
            memcpy(out, p, 3);
            break;
        case 2:
            out[0] = ((v & 31) * 255 + 15) / 31;
            out[1] = (((v >> 5) & 63) * 255 + 31) / 63;
            out[2] = ((v >> 11) * 255 + 15) / 31;
            break;
        case 3:
            out[0] = (((v >> 1) & 31) * 255 + 15) / 31;
            out[1] = (((v >> 6) & 31) * 255 + 15) / 31;
            out[2] = ((v >> 11) * 255 + 15) / 31;
            break;
        case 4:
            out[0] = ((v >> 4) & 15) * 17;
            out[1] = ((v >> 8) & 15) * 17;
            out[2] = (v >> 12) * 17;
            break;
    }
}

// Size of a tile relative to quality 50 (x100), typical shape of 4:2:0 JPEGs of screen content; the learned
// k_tile fixes the scale, so only the shape matters here.
static unsigned size_factor(unsigned q) {
    static const unsigned f[] = {75, 87, 100, 115, 140, 190, 300, 800}; // 30,40,...,100
    if (q <= 30) {
        return f[0];
    }
    if (q >= 100) {
        return f[7];
    }
    unsigned i = (q - 30) / 10, rest = (q - 30) % 10;
    return f[i] + (f[i + 1] - f[i]) * rest / 10;
}
unsigned native_pick_quality(unsigned k_tile, unsigned tiles, unsigned budget, unsigned max_q) {
    if (max_q > 100) {
        max_q = 100;
    }
    for (unsigned q = 100; q > NATIVE_AUTO_FLOOR; q -= 10) {
        if (q > max_q) {
            continue;
        }
        if ((unsigned long long)tiles * k_tile * size_factor(q) / 100 <= budget) {
            return q;
        }
    }
    return NATIVE_AUTO_FLOOR;
}
static void learn(NativeVideoCache *c, unsigned bytes, unsigned tiles, unsigned q) {
    c->obs_bytes = bytes;
    c->chosen = q;
    if (!bytes || !tiles) {
        return;
    }
    unsigned obs = (unsigned)((unsigned long long)bytes * 100 / ((unsigned long long)tiles * size_factor(q)));
    if (obs < 5) {
        obs = 5;
    }
    if (obs > 400) {
        obs = 400;
    }
    c->k_tile = c->k_tile ? (c->k_tile * 3 + obs + 2) / 4 : obs;
}

// Whole screen from the cached picture: base64 becomes current.
static int encode_full(NativeVideoCache *c, unsigned width, unsigned quality, uint8_t *jpeg, size_t jpeg_size) {
    c->rect_count = 0;
    c->valid = false;
    unsigned long long tick1 = VIDEO_TICK();
    size_t bytes = capture_encode_3ds_bgr8_jpeg_420(c->bgr, width, 240, quality, jpeg, jpeg_size);
    unsigned long long tick2 = VIDEO_TICK();
    c->t_jpeg += tick2 - tick1;
    bool b64_ok = bytes && (!c->base64 || capture_base64_encode(jpeg, bytes, c->base64, NATIVE_B64_MAX));
    c->t_b64 += VIDEO_TICK() - tick2;
    if (!b64_ok) {
        return -1;
    }
    c->width = width;
    c->quality = quality;
    c->picture_q = quality;
    c->valid = true;
    memset(c->tile_q, quality, sizeof(c->tile_q));
    learn(c, (unsigned)bytes, (width / NATIVE_TILE) * NATIVE_TILES_Y, quality);
    c->last_full_ms = c->now_ms;
    c->full_stale = false;
    c->full_bytes = (unsigned)bytes;
    return 1;
}

// The changed tiles as a few rectangles, each its own small JPEG with its Base64 text in rect_b64.
// False (nothing kept) if any rectangle is too big or the text does not fit: the caller sends the whole screen.
static bool encode_rects(NativeVideoCache *c, unsigned quality, const NativePlan *plan, uint8_t *jpeg,
                         size_t jpeg_size) {
    unsigned long long tick1 = VIDEO_TICK();
    unsigned total_bytes = 0, total_tiles = 0;
    c->rect_count = 0;
    c->rect_used = 0;
    for (unsigned i = 0; i < plan->count; i++) {
        NativeTileRect t = plan->r[i];
        size_t bytes =
            capture_encode_3ds_bgr8_jpeg_420_rect(c->bgr, 240, t.x * NATIVE_TILE, t.y * NATIVE_TILE, t.w * NATIVE_TILE,
                                                  t.h * NATIVE_TILE, quality, jpeg, jpeg_size);
        size_t n = 0;
        if (bytes && bytes <= NATIVE_RECT_MAX_BYTES) {
            n = capture_base64_encode(jpeg, bytes, c->rect_b64 + c->rect_used, NATIVE_B64_MAX - c->rect_used);
        }
        if (!n) {
            c->rect_count = 0;
            c->t_jpeg += VIDEO_TICK() - tick1;
            return false;
        }
        c->rects[i] = (NativeOutRect){(uint16_t)(t.x * NATIVE_TILE),
                                      (uint16_t)(t.y * NATIVE_TILE),
                                      (uint16_t)(t.w * NATIVE_TILE),
                                      (uint16_t)(t.h * NATIVE_TILE),
                                      c->rect_used,
                                      (uint32_t)n};
        c->rect_used += (unsigned)n;
        c->rect_count = i + 1;
        total_bytes += (unsigned)bytes;
        total_tiles += t.w * t.h;
    }
    c->t_jpeg += VIDEO_TICK() - tick1;
    learn(c, total_bytes, total_tiles, quality);
    unsigned lowest = quality, tiles_x = c->width / NATIVE_TILE;
    for (unsigned i = 0; i < plan->count; i++) {
        for (unsigned ty = 0; ty < plan->r[i].h; ty++) {
            for (unsigned tx = 0; tx < plan->r[i].w; tx++) {
                c->tile_q[(plan->r[i].y + ty) * tiles_x + plan->r[i].x + tx] = (uint8_t)quality;
            }
        }
    }
    for (unsigned i = 0; i < tiles_x * NATIVE_TILES_Y; i++) {
        if (c->tile_q[i] < lowest) {
            lowest = c->tile_q[i];
        }
    }
    c->picture_q = lowest;
    return true;
}

int native_video_encode(NativeVideoCache *c, const NativeFrame *f, const uint8_t *raw, size_t raw_size,
                        unsigned quality, uint8_t *jpeg, size_t jpeg_size) {
    size_t span = f ? native_frame_span(f) : 0;
    if (!c || !c->bgr || !c->base64 || !raw || !span || span > raw_size || !jpeg || quality < 30 || quality > 100) {
        return -1;
    }
    unsigned long long tick0 = VIDEO_TICK();
    bool had = c->valid && c->width == f->width;
    const unsigned original_quality = quality;
    const bool automatic = c->auto_quality;
    if (automatic && !c->k_tile) {
        c->k_tile = 60;
    }
    bool full_needed = !had || (!automatic && c->quality != quality);
    unsigned tiles_x = f->width / NATIVE_TILE;
    bool tiled = tiles_x > 0 && tiles_x <= NATIVE_TILES_X_MAX && !(f->width & (NATIVE_TILE - 1));
    if (tiled) {
        memset(c->dirty, 0, tiles_x * NATIVE_TILES_Y);
    }
    unsigned dirty_count = 0;
    static const unsigned pixel_size[] = {4, 3, 2, 2, 2};
    // Compare with the previous picture tile by tile (16 rows of one column = 48 contiguous bytes) while copying
    // the new one in. The framebuffer is column-major and bottom-to-top, so screen rows 16t..16t+15 are the bytes
    // from (240-16(t+1))*3 on in each column.
    for (unsigned x = 0; x < f->width; x++) {
        const uint8_t *column = raw + x * f->stride;
        uint8_t *target = c->bgr + x * 720;
        uint8_t converted[720];
        const uint8_t *src = column;
        if (f->format != 1) {
            for (unsigned y = 0; y < 240; y++) {
                bgr_pixel(column + y * pixel_size[f->format], f->format, converted + y * 3);
            }
            src = converted;
        }
        if (had) {
            if (tiled) {
                for (unsigned t = 0; t < NATIVE_TILES_Y; t++) {
                    unsigned off = (240 - NATIVE_TILE * (t + 1)) * 3;
                    if (memcmp(target + off, src + off, NATIVE_TILE * 3) != 0) {
                        uint8_t *d = &c->dirty[t * tiles_x + (x >> 4)];
                        if (!*d) {
                            *d = 1;
                            dirty_count++;
                        }
                    }
                }
            } else if (memcmp(target, src, 720) != 0) {
                dirty_count++;
            }
        }
        memcpy(target, src, 720);
    }
    const unsigned tiles_all = tiles_x * NATIVE_TILES_Y;
    c->last_dirty = had ? dirty_count : tiles_all;
    if (!full_needed && dirty_count == 0) {
        c->t_convert += VIDEO_TICK() - tick0;
        // Automatic mode: a picture that has stopped moving is sharpened (once) if the budget allows better.
        if (automatic && c->valid && tiles_all) {
            unsigned better = native_pick_quality(c->k_tile, tiles_all, NATIVE_AUTO_IDLE_BYTES, quality);
            if (better >= c->picture_q + 10) {
                return encode_full(c, f->width, better, jpeg, jpeg_size);
            }
        }
        return 0;
    }
    if (had && tiled) {
        c->cols_total += tiles_x * NATIVE_TILES_Y;
        c->cols_same += tiles_x * NATIVE_TILES_Y - dirty_count;
    }
    c->t_convert += VIDEO_TICK() - tick0;

    // Automatic mode: the quality follows from how many tiles must be sent and the budget for this frame, which
    // is shared with the other screen in proportion to the tiles each has to send.
    unsigned budget_now = 0;
    if (automatic) {
        unsigned send = full_needed ? tiles_all : dirty_count;
        unsigned share = send + c->other_tiles;
        budget_now = share ? (unsigned)((unsigned long long)c->budget * send / share) : c->budget;
        quality = native_pick_quality(c->k_tile, send, budget_now, quality);
    }
    // Tiles that were sent at a lower quality earlier (the picture moved on, they did not) are sharpened with
    // whatever is left of the frame's budget, so low-quality remnants do not stay on screen next to a Q100 area.
    if (automatic && !full_needed && tiled && c->partial && c->rect_b64) {
        unsigned per_tile = (unsigned)((unsigned long long)c->k_tile * size_factor(quality) / 100);
        if (per_tile < 20) {
            per_tile = 20;
        }
        unsigned used = dirty_count * per_tile, spare_tiles = budget_now > used ? (budget_now - used) / per_tile : 0;
        for (unsigned n = 0; n < tiles_all && spare_tiles; n++) {
            unsigned i = (c->refine_cursor + n) % tiles_all;
            if (!c->dirty[i] && (unsigned)c->tile_q[i] + 10 <= quality) {
                c->dirty[i] = 1;
                dirty_count++;
                spare_tiles--;
                c->refine_cursor = i + 1;
            }
        }
    }
    // Only the changed tiles, when that is clearly cheaper and no keyframe is due.
    if (!full_needed && tiled && c->partial && c->rect_b64 && (automatic ? c->k_tile : c->full_bytes) && dirty_count &&
        c->now_ms - c->last_full_ms < NATIVE_KEYFRAME_MS) {
        unsigned tiles = tiles_all;
        unsigned tile_cost = automatic ? (unsigned)((unsigned long long)c->k_tile * size_factor(quality) * 12 / 1000)
                                       : c->full_bytes * 12u / 10u / tiles;
        if (tile_cost < 30u) {
            tile_cost = 30u;
        }
        NativePlan plan;
        native_plan_rects(c->dirty, tiles_x, NATIVE_TILES_Y, NATIVE_RECT_HEADER_COST, tile_cost, &plan);
        unsigned full_cost = NATIVE_RECT_HEADER_COST + tiles * tile_cost;
        if (plan.count && plan.cost * 100u < full_cost * 80u && encode_rects(c, quality, &plan, jpeg, jpeg_size)) {
            c->full_stale = true;
            return 2;
        }
    }
    if (automatic && !full_needed) {
        // A whole screen although few tiles changed (keyframe, or rectangles would not pay): it may take more than
        // this frame's share, the more so the more tiles are clean.
        unsigned clean = tiles_all - dirty_count, b = budget_now;
        if (NATIVE_AUTO_IDLE_BYTES > b) {
            b += (unsigned)((unsigned long long)(NATIVE_AUTO_IDLE_BYTES - b) * clean / tiles_all);
        }
        quality = native_pick_quality(c->k_tile, tiles_all, b, original_quality);
    }
    return encode_full(c, f->width, quality, jpeg, jpeg_size);
}

int native_video_refresh(NativeVideoCache *c, unsigned quality, uint8_t *jpeg, size_t jpeg_size) {
    if (!c || !c->bgr || !c->valid || !jpeg || quality < 30 || quality > 100) {
        return -1;
    }
    if (c->auto_quality) {
        quality = native_pick_quality(c->k_tile ? c->k_tile : 60, (c->width / NATIVE_TILE) * NATIVE_TILES_Y,
                                      NATIVE_AUTO_IDLE_BYTES, quality);
    }
    return encode_full(c, c->width, quality, jpeg, jpeg_size);
}
