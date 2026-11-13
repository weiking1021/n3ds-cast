// Partial updates: the rectangle planner, the rectangle encoder (every block must have exactly the coefficients it
// has in the full-frame JPEG, so the area decodes to the same pixels) and the cache's partial/whole-screen decision.
#include "capture_jpeg.h"
#include "rects.h"
#include "video.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- minimal baseline decoder to quantised zigzag coefficients ----
typedef struct {
    uint8_t bits[17];
    uint8_t vals[256];
    int present;
    int mincode[17], maxcode[17], valptr[17];
} DHT;
typedef struct {
    const uint8_t *d;
    size_t n, pos;
    uint32_t buf;
    int cnt;
} BitIn;
static int get_bit(BitIn *b) {
    if (!b->cnt) {
        assert(b->pos < b->n);
        b->buf = b->d[b->pos++];
        if (b->buf == 0xFF) {
            assert(b->pos < b->n);
            if (b->d[b->pos] == 0) {
                b->pos++;
            }
        }
        b->cnt = 8;
    }
    b->cnt--;
    return (b->buf >> b->cnt) & 1;
}
static void build(DHT *t) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k;
        t->mincode[l] = code;
        code += t->bits[l];
        k += t->bits[l];
        t->maxcode[l] = t->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
}
static int decode_sym(BitIn *b, const DHT *t) {
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | get_bit(b);
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l] && code >= t->mincode[l]) {
            return t->vals[t->valptr[l] + code - t->mincode[l]];
        }
    }
    assert(!"bad code");
    return 0;
}
static int extend(int v, int s) {
    return s == 0 ? 0 : (v < (1 << (s - 1)) ? v - (1 << s) + 1 : v);
}
static int recvb(BitIn *b, int s) {
    int v = 0;
    for (int i = 0; i < s; i++) {
        v = (v << 1) | get_bit(b);
    }
    return v;
}
static size_t decode(const uint8_t *j, size_t n, int *width, int *height, int16_t *out) {
    DHT t[4];
    memset(t, 0, sizeof(t));
    size_t p = 2;
    while (p + 4 <= n) {
        assert(j[p] == 0xFF);
        uint8_t m = j[p + 1];
        size_t len = (j[p + 2] << 8) | j[p + 3];
        if (m == 0xC0) {
            *height = (j[p + 5] << 8) | j[p + 6];
            *width = (j[p + 7] << 8) | j[p + 8];
        }
        if (m == 0xC4) {
            size_t q = p + 4;
            while (q < p + 2 + len) {
                int id = j[q++];
                int idx = (id & 0x10) ? (2 + (id & 1)) : (id & 1);
                DHT *d = &t[idx];
                d->bits[0] = 0;
                int total = 0;
                for (int l = 1; l <= 16; l++) {
                    d->bits[l] = j[q++];
                    total += d->bits[l];
                }
                for (int i = 0; i < total; i++) {
                    d->vals[i] = j[q++];
                }
                d->present = 1;
                build(d);
            }
        }
        if (m == 0xDA) {
            p += 2 + len;
            break;
        }
        p += 2 + len;
    }
    BitIn b = {j, n, p, 0, 0};
    int mcus = (*width / 16) * (*height / 16);
    int pd[3] = {0, 0, 0};
    size_t blocks = 0;
    for (int m = 0; m < mcus; m++) {
        for (int blk = 0; blk < 6; blk++) {
            int comp = blk < 4 ? 0 : blk - 3;
            const DHT *dc = &t[comp ? 1 : 0], *ac = &t[comp ? 3 : 2];
            int16_t *z = out + blocks * 64;
            memset(z, 0, 128);
            int s = decode_sym(&b, dc);
            int diff = extend(recvb(&b, s), s);
            pd[comp] += diff;
            z[0] = (int16_t)pd[comp];
            for (int k = 1; k < 64;) {
                int rs = decode_sym(&b, ac);
                int r = rs >> 4, sz = rs & 15;
                if (sz == 0) {
                    if (r == 15) {
                        k += 16;
                        continue;
                    }
                    break;
                }
                k += r;
                assert(k < 64);
                z[k] = (int16_t)extend(recvb(&b, sz), sz);
                k++;
            }
            blocks++;
        }
    }
    return blocks;
}

static uint32_t rng = 987654;
static uint32_t rnd(void) {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}
static void picture(uint8_t *img,
                    int w) { // column-major, bottom-to-top BGR: smooth areas, flat blocks and a noisy patch
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < 240; y++) {
            uint8_t *p = img + ((size_t)x * 240 + y) * 3;
            p[0] = (uint8_t)(x * 2 + y);
            p[1] = (uint8_t)(y * 3);
            p[2] = (uint8_t)(((x >> 4) ^ (y >> 4)) * 29);
            if (x >= 96 && x < 176 && y >= 60 && y < 140) {
                p[0] = (uint8_t)rnd();
                p[1] = (uint8_t)rnd();
                p[2] = (uint8_t)rnd();
            }
        }
    }
}

// ---- planner ----
static void test_planner(void) {
    NativePlan p;
    // nothing changed
    uint8_t none[375] = {0};
    native_plan_rects(none, 25, 15, 700, 100, &p);
    assert(p.count == 0 && p.cost == 0);
    // one tile
    uint8_t one[375] = {0};
    one[7 * 25 + 12] = 1;
    native_plan_rects(one, 25, 15, 700, 100, &p);
    assert(p.count == 1 && p.r[0].x == 12 && p.r[0].y == 7 && p.r[0].w == 1 && p.r[0].h == 1 && p.cost == 800);
    // two far apart tiles: two rectangles beat one big box
    uint8_t two[375] = {0};
    two[0] = 1;
    two[14 * 25 + 24] = 1;
    native_plan_rects(two, 25, 15, 700, 100, &p);
    assert(p.count == 2 && p.cost == 2 * 800);
    // two neighbours: one box (a second header costs more than the gap)
    uint8_t near[375] = {0};
    near[5 * 25 + 5] = 1;
    near[5 * 25 + 7] = 1;
    native_plan_rects(near, 25, 15, 700, 100, &p);
    assert(p.count == 1 && p.r[0].x == 5 && p.r[0].w == 3 && p.r[0].h == 1);
    // everything changed: one rectangle covering all
    uint8_t all[375];
    memset(all, 1, sizeof(all));
    native_plan_rects(all, 25, 15, 700, 100, &p);
    assert(p.count == 1 && p.r[0].w == 25 && p.r[0].h == 15 && p.cost == 700 + 375 * 100);
    // 20x15 screen (bottom): bounds are respected
    uint8_t low[300] = {0};
    low[14 * 20 + 19] = 1;
    native_plan_rects(low, 20, 15, 700, 100, &p);
    assert(p.count == 1 && p.r[0].x == 19 && p.r[0].y == 14);
    // random maps: rectangles are in range, disjoint, cover every dirty tile, never exceed the limit or one box's cost
    for (int round = 0; round < 2000; round++) {
        uint8_t m[375];
        unsigned tx = (round & 1) ? 25 : 20, ty = 15;
        memset(m, 0, sizeof(m));
        unsigned dirty = 0;
        int shape = round % 4;
        for (unsigned y = 0; y < ty; y++) {
            for (unsigned x = 0; x < tx; x++) {
                int on = shape == 0   ? (rnd() % 40 == 0)
                         : shape == 1 ? (rnd() % 6 == 0)
                         : shape == 2 ? ((x < 4 && y < 4) || (x > tx - 5 && y > ty - 5) || (x == 10 && y > 4))
                                      : (rnd() % 2);
                m[y * tx + x] = (uint8_t)on;
                dirty += on;
            }
        }
        native_plan_rects(m, tx, ty, 700, 100, &p);
        assert(p.count <= NATIVE_MAX_RECTS);
        if (!dirty) {
            assert(p.count == 0);
            continue;
        }
        uint8_t cover[375] = {0};
        unsigned cost = 0;
        for (unsigned i = 0; i < p.count; i++) {
            NativeTileRect r = p.r[i];
            assert(r.w > 0 && r.h > 0 && r.x + r.w <= tx && r.y + r.h <= ty);
            cost += 700 + r.w * r.h * 100;
            for (unsigned y = r.y; y < r.y + r.h; y++) {
                for (unsigned x = r.x; x < r.x + r.w; x++) {
                    assert(!cover[y * tx + x]);
                    cover[y * tx + x] = 1;
                }
            }
        }
        assert(cost == p.cost);
        for (unsigned i = 0; i < tx * ty; i++) {
            if (m[i]) {
                assert(cover[i]);
            }
        }
        int x0 = 99, y0 = 99, x1 = -1, y1 = -1;
        for (unsigned y = 0; y < ty; y++) {
            for (unsigned x = 0; x < tx; x++) {
                if (m[y * tx + x]) {
                    if ((int)x < x0) {
                        x0 = x;
                    }
                    if ((int)x > x1) {
                        x1 = x;
                    }
                    if ((int)y < y0) {
                        y0 = y;
                    }
                    if ((int)y > y1) {
                        y1 = y;
                    }
                }
            }
        }
        assert(p.cost <= 700 + (unsigned)((x1 - x0 + 1) * (y1 - y0 + 1)) * 100);
    }
}

// ---- rectangle encoder: same coefficients as in the full picture ----
static void test_rect_encoder(void) {
    static uint8_t img[400 * 240 * 3], full[200000], rect[200000];
    static int16_t cf[25 * 15 * 6 * 64], cr[25 * 15 * 6 * 64];
    for (int w = 320; w <= 400; w += 80) {
        for (int q = 30; q <= 95; q += 13) {
            picture(img, w);
            size_t nf = capture_encode_3ds_bgr8_jpeg_420(img, w, 240, q, full, sizeof(full));
            int fw = 0, fh = 0;
            size_t fb = decode(full, nf, &fw, &fh, cf);
            assert(fb == (size_t)(w / 16) * 15 * 6);
            int rects[][4] = {
                {0, 0, 16, 16}, {16, 32, 48, 64}, {w - 16, 224, 16, 16}, {0, 0, w, 240}, {32, 160, w - 64, 80}};
            for (unsigned k = 0; k < sizeof(rects) / sizeof(rects[0]); k++) {
                int x0 = rects[k][0], y0 = rects[k][1], rw = rects[k][2], rh = rects[k][3];
                size_t nr = capture_encode_3ds_bgr8_jpeg_420_rect(img, 240, x0, y0, rw, rh, q, rect, sizeof(rect));
                assert(nr > 0);
                int pw = 0, ph = 0;
                size_t rb = decode(rect, nr, &pw, &ph, cr);
                assert(pw == rw && ph == rh && rb == (size_t)(rw / 16) * (rh / 16) * 6);
                for (int my = 0; my < rh / 16; my++) {
                    for (int mx = 0; mx < rw / 16; mx++) {
                        size_t in_rect = ((size_t)my * (rw / 16) + mx) * 6,
                               in_full = ((size_t)((y0 / 16) + my) * (w / 16) + (x0 / 16) + mx) * 6;
                        assert(!memcmp(cr + in_rect * 64, cf + in_full * 64,
                                       6 * 64 *
                                           sizeof(int16_t))); // every block identical (DC is differential, but decoded)
                    }
                }
            }
        }
    }
    // the whole picture as a rectangle is the full encoder's output, byte for byte
    picture(img, 400);
    size_t nf = capture_encode_3ds_bgr8_jpeg_420(img, 400, 240, 50, full, sizeof(full));
    size_t nr = capture_encode_3ds_bgr8_jpeg_420_rect(img, 240, 0, 0, 400, 240, 50, rect, sizeof(rect));
    assert(nf == nr && !memcmp(full, rect, nf));
    // invalid rectangles are refused
    assert(!capture_encode_3ds_bgr8_jpeg_420_rect(img, 240, 8, 0, 16, 16, 50, rect, sizeof(rect)));
    assert(!capture_encode_3ds_bgr8_jpeg_420_rect(img, 240, 0, 0, 16, 17, 50, rect, sizeof(rect)));
    assert(!capture_encode_3ds_bgr8_jpeg_420_rect(img, 240, 0, 232, 16, 16, 50, rect, sizeof(rect)));
    assert(!capture_encode_3ds_bgr8_jpeg_420_rect(img, 240, 0, 0, 16, 16, 50, rect, 10));
}

// ---- cache: partial vs whole-screen decision ----
static void raw_from(const uint8_t *img, uint8_t *raw, int w) { // tightly packed BGR8 frame (stride 720)
    for (int x = 0; x < w; x++) {
        memcpy(raw + (size_t)x * 720, img + (size_t)x * 720, 720);
    }
}
static void test_cache(void) {
    static uint8_t img[400 * 240 * 3], raw[400 * 4096], jpeg[NATIVE_JPEG_MAX], bgr[400 * 240 * 3];
    static char b64[NATIVE_B64_MAX], pool[NATIVE_B64_MAX];
    NativeVideoCache c = {.bgr = bgr, .base64 = b64, .rect_b64 = pool, .partial = true};
    NativeFrame f = {.address = 1, .format = 1, .width = 400, .stride = 720};
    picture(img, 400);
    raw_from(img, raw, 400);
    assert(native_video_encode(&c, &f, raw, sizeof(raw), 70, jpeg, sizeof(jpeg)) == 1 && c.rect_count == 0 &&
           c.full_bytes > 1000);
    assert(native_video_encode(&c, &f, raw, sizeof(raw), 70, jpeg, sizeof(jpeg)) == 0); // unchanged
    // change one tile: only a rectangle for it
    for (int x = 48; x < 64; x++) {
        for (int y = 64; y < 80; y++) {
            uint8_t *p = img + ((size_t)x * 240 + (239 - y)) * 3;
            p[0] ^= 0x55;
            p[2] += 40;
        }
    }
    raw_from(img, raw, 400);
    c.now_ms = 100;
    assert(native_video_encode(&c, &f, raw, sizeof(raw), 70, jpeg, sizeof(jpeg)) == 2);
    assert(c.rect_count == 1 && c.rects[0].x == 48 && c.rects[0].y == 64 && c.rects[0].w == 16 && c.rects[0].h == 16 &&
           c.full_stale);
    assert(c.rects[0].len > 100 && c.rects[0].len < c.full_bytes);
    // the rectangle decodes to the same blocks as in a full encode of the changed picture
    {
        static uint8_t fullnew[200000], rj[200000];
        static int16_t cf[25 * 15 * 6 * 64], cr[6 * 64];
        size_t nf = capture_encode_3ds_bgr8_jpeg_420(img, 400, 240, 70, fullnew, sizeof(fullnew));
        int w = 0, h = 0;
        decode(fullnew, nf, &w, &h, cf);
        size_t rn = capture_encode_3ds_bgr8_jpeg_420_rect(img, 240, 48, 64, 16, 16, 70, rj, sizeof(rj));
        decode(rj, rn, &w, &h, cr);
        assert(!memcmp(cr, cf + ((size_t)(64 / 16) * 25 + (48 / 16)) * 6 * 64, 6 * 64 * sizeof(int16_t)));
    }
    // refresh: the cached whole screen is stale, native_video_refresh makes it current again
    assert(native_video_refresh(&c, 70, jpeg, sizeof(jpeg)) == 1 && !c.full_stale && c.rect_count == 0);
    // mostly changed: whole screen
    for (int x = 0; x < 400; x++) {
        for (int y = 0; y < 240; y++) {
            uint8_t *p = img + ((size_t)x * 240 + y) * 3;
            p[1] ^= 0x33;
        }
    }
    raw_from(img, raw, 400);
    assert(native_video_encode(&c, &f, raw, sizeof(raw), 70, jpeg, sizeof(jpeg)) == 1 && c.rect_count == 0);
    // a quality change is a whole screen
    assert(native_video_encode(&c, &f, raw, sizeof(raw), 75, jpeg, sizeof(jpeg)) == 1);
    // keyframe: after the interval even a one-tile change goes out as a whole screen
    for (int x = 0; x < 16; x++) {
        for (int y = 0; y < 16; y++) {
            uint8_t *p = img + ((size_t)x * 240 + (239 - y)) * 3;
            p[0] ^= 0x70;
        }
    }
    raw_from(img, raw, 400);
    c.now_ms = c.last_full_ms + NATIVE_KEYFRAME_MS;
    assert(native_video_encode(&c, &f, raw, sizeof(raw), 75, jpeg, sizeof(jpeg)) == 1);
    // partial output off (the default): always whole screens
    c.partial = false;
    for (int x = 100; x < 116; x++) {
        for (int y = 100; y < 116; y++) {
            uint8_t *p = img + ((size_t)x * 240 + (239 - y)) * 3;
            p[0] ^= 0x70;
        }
    }
    raw_from(img, raw, 400);
    assert(native_video_encode(&c, &f, raw, sizeof(raw), 75, jpeg, sizeof(jpeg)) == 1 && c.rect_count == 0);
    // the unchanged-tile statistic counts whole tiles
    assert(c.cols_total > 0 && c.cols_same < c.cols_total);
    // the bottom screen (320 wide) and another pixel format
    NativeVideoCache d = {.bgr = bgr, .base64 = b64, .rect_b64 = pool, .partial = true};
    NativeFrame g = {.address = 1, .format = 1, .width = 320, .stride = 720};
    picture(img, 320);
    raw_from(img, raw, 320);
    assert(native_video_encode(&d, &g, raw, sizeof(raw), 60, jpeg, sizeof(jpeg)) == 1);
    for (int x = 304; x < 320; x++) {
        for (int y = 224; y < 240; y++) {
            uint8_t *p = img + ((size_t)x * 240 + (239 - y)) * 3;
            p[1] ^= 0x60;
        }
    }
    raw_from(img, raw, 320);
    assert(native_video_encode(&d, &g, raw, sizeof(raw), 60, jpeg, sizeof(jpeg)) == 2 && d.rects[0].x == 304 &&
           d.rects[0].y == 224);
    { // RGB565 input is converted per pixel and compared as converted
        NativeVideoCache e = {.bgr = bgr, .base64 = b64, .rect_b64 = pool, .partial = true};
        NativeFrame h = {.address = 1, .format = 2, .width = 400, .stride = 480};
        static uint8_t r565[400 * 4096];
        for (int x = 0; x < 400; x++) {
            for (int y = 0; y < 240; y++) {
                uint16_t v = (uint16_t)((x * 7 + y) & 0xFFFF);
                memcpy(r565 + (size_t)x * 480 + y * 2, &v, 2);
            }
        }
        c.now_ms = 0;
        e.now_ms = 0;
        assert(native_video_encode(&e, &h, r565, sizeof(r565), 60, jpeg, sizeof(jpeg)) == 1);
        uint16_t v = 0xFFFF;
        memcpy(r565 + (size_t)100 * 480 + 100 * 2, &v, 2); // one pixel, in storage row 100 = screen row 139
        assert(native_video_encode(&e, &h, r565, sizeof(r565), 60, jpeg, sizeof(jpeg)) == 2);
        assert(e.rect_count == 1 && e.rects[0].x == 96 && e.rects[0].y == 128);
    }
}

// ---- automatic quality: chosen per frame from the changed tiles and the budget ----
static void test_auto(void) {
    // the pick is monotonic: more tiles or a smaller budget never give a higher quality; ceiling and floor hold
    for (unsigned k = 20; k <= 200; k += 60) {
        for (unsigned tiles = 1; tiles <= 375; tiles += 37) {
            unsigned last = 101;
            for (unsigned budget = 500; budget <= 400000; budget += 997) {
                unsigned q = native_pick_quality(k, tiles, budget, 100);
                assert(q >= NATIVE_AUTO_FLOOR && q <= 100 && q % 10 == 0);
                if (budget > 500) {
                    assert(q >= last || last == 101);
                }
                last = q;
            }
            assert(native_pick_quality(k, tiles, 1u << 30, 100) == 100 &&
                   native_pick_quality(k, tiles, 1u << 30, 85) == 80);
            assert(native_pick_quality(k, tiles, 0, 100) == NATIVE_AUTO_FLOOR);
        }
    }
    static uint8_t img[400 * 240 * 3], raw[400 * 4096], jpeg[NATIVE_JPEG_MAX], bgr[400 * 240 * 3];
    static char b64[NATIVE_B64_MAX], pool[NATIVE_B64_MAX];
    NativeVideoCache c = {.bgr = bgr, .base64 = b64, .rect_b64 = pool, .partial = true, .auto_quality = true};
    NativeFrame f = {.address = 1, .format = 1, .width = 400, .stride = 720};
    picture(img, 400);
    raw_from(img, raw, 400);
    // first frame with a tiny budget: whole screen at a low quality
    c.budget = 8000;
    c.now_ms = 0;
    assert(native_video_encode(&c, &f, raw, sizeof(raw), 100, jpeg, sizeof(jpeg)) == 1);
    unsigned low = c.chosen;
    assert(low <= 60 && c.picture_q == low && c.k_tile > 0);
    // nothing moves: the budget for a one-off sharpening is large, the picture is re-encoded better, once
    int sharpened = native_video_encode(&c, &f, raw, sizeof(raw), 100, jpeg, sizeof(jpeg));
    assert(sharpened == 1 && c.chosen > low && c.picture_q == c.chosen);
    assert(native_video_encode(&c, &f, raw, sizeof(raw), 100, jpeg, sizeof(jpeg)) == 0 || c.chosen == c.picture_q);
    while (native_video_encode(&c, &f, raw, sizeof(raw), 100, jpeg, sizeof(jpeg)) == 1)
        ; // settles
    assert(c.picture_q >= 70);
    // one tile changes with a generous budget: a rectangle, at a high quality, no whole-screen re-encode
    for (int x = 48; x < 64; x++) {
        for (int y = 64; y < 80; y++) {
            uint8_t *p = img + ((size_t)x * 240 + (239 - y)) * 3;
            p[0] ^= 0x55;
            p[2] += 40;
        }
    }
    raw_from(img, raw, 400);
    c.now_ms = 100;
    c.budget = 60000;
    {
        int r = native_video_encode(&c, &f, raw, sizeof(raw), 100, jpeg,
                                    sizeof(jpeg)); // may also carry tiles being sharpened
        assert((r == 2 && c.rect_count >= 1) || r == 1);
        assert(c.chosen >= 80);
    }
    // the ceiling caps the pick
    for (int x = 48; x < 64; x++) {
        for (int y = 64; y < 80; y++) {
            uint8_t *p = img + ((size_t)x * 240 + (239 - y)) * 3;
            p[0] ^= 0x22;
        }
    }
    raw_from(img, raw, 400);
    c.now_ms = 200;
    assert(native_video_encode(&c, &f, raw, sizeof(raw), 70, jpeg, sizeof(jpeg)) > 0 && c.chosen <= 70);
    // a busy frame (everything changes) with a small budget goes out whole at a low quality, with the other
    // screen's share taken into account
    for (int x = 0; x < 400; x++) {
        for (int y = 0; y < 240; y++) {
            uint8_t *p = img + ((size_t)x * 240 + y) * 3;
            p[1] ^= 0x33;
        }
    }
    raw_from(img, raw, 400);
    c.now_ms = 300;
    c.budget = 12000;
    c.other_tiles = 300;
    assert(native_video_encode(&c, &f, raw, sizeof(raw), 100, jpeg, sizeof(jpeg)) == 1);
    assert(c.chosen <= 50 && c.picture_q == c.chosen);
    // low-quality remnants are sharpened with spare budget while one corner keeps changing: no whole screen needed
    {
        unsigned before = c.picture_q;
        c.other_tiles = 0;
        c.budget = 40000;
        for (int frame = 0; frame < 60 && c.picture_q < 80; frame++) {
            for (int x = 0; x < 16; x++) {
                for (int y = 0; y < 16; y++) {
                    uint8_t *p = img + ((size_t)x * 240 + (239 - y)) * 3;
                    p[0] ^= 0x11;
                }
            }
            raw_from(img, raw, 400);
            c.now_ms = 500 + frame * 20;
            assert(native_video_encode(&c, &f, raw, sizeof(raw), 100, jpeg, sizeof(jpeg)) == 2);
        }
        assert(c.picture_q >= 80 && c.picture_q > before);
    }
    // a quality that is not automatic keeps the old behaviour: a different quality is a whole screen
    c.auto_quality = false;
    for (int x = 0; x < 16; x++) {
        for (int y = 0; y < 16; y++) {
            uint8_t *p = img + ((size_t)x * 240 + (239 - y)) * 3;
            p[0] ^= 0x70;
        }
    }
    raw_from(img, raw, 400);
    c.now_ms = 400;
    assert(native_video_encode(&c, &f, raw, sizeof(raw), 100, jpeg, sizeof(jpeg)) == 1 && c.quality == 100);
}

int main(void) {
    test_auto();
    test_planner();
    test_rect_encoder();
    test_cache();
    puts(
        "PASS: partial updates: rectangle planner (2000 random maps), rectangle blocks identical to the full encode, cache partial/whole/keyframe decisions, automatic quality");
    return 0;
}
