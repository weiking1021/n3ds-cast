#pragma once
#include "frame.h"
#include "rects.h"
// One screen's JPEG may be this large (it used to be 48000, which silently dropped every frame of a busy scene at
// 75%+ quality). Larger-than-one-message frames are split into parts by the sender (Cast messages are limited to 64 KB).
#define NATIVE_JPEG_MAX 144000u // quality 100 on a busy scene needs ~100-140 KB
#define NATIVE_B64_MAX 192008u  // Base64 of NATIVE_JPEG_MAX plus the terminator
// Partial updates: a rectangle's JPEG must stay this small (its Base64 then fits one Cast message), else the whole
// screen is sent. A rectangle costs about this many bytes of header/overhead. Everything older than the keyframe
// interval is refreshed with a whole screen so a receiver that missed something (reload, reconnect) recovers.
#define NATIVE_RECT_MAX_BYTES 40000u
#define NATIVE_RECT_HEADER_COST 700u
#define NATIVE_KEYFRAME_MS 3000u
// Automatic quality: what the Wi-Fi link carries comfortably (bytes/s; measured ceiling ~0.79 MB/s), the quality
// floor, and a one-off budget (bytes per screen) for sharpening a picture that has stopped moving.
#define NATIVE_AUTO_LINK 700000u
#define NATIVE_AUTO_FLOOR 30u
#define NATIVE_AUTO_IDLE_BYTES 110000u
typedef struct {
    uint16_t x, y, w, h;
    uint32_t off, len;
} NativeOutRect; // pixels; Base64 text at rect_b64+off
typedef struct {
    uint8_t *bgr; // Caller owns 400*240*3 bytes for either display.
    char *base64; // Base64 text of the last JPEG (fallback path)
    unsigned long long cols_total,
        cols_same; // 16x16 tiles seen / unchanged vs the previous frame (changed frames only)
    unsigned width, quality;
    bool valid;
    unsigned long long t_convert, t_jpeg, t_b64; // accumulated ARM11 ticks (0 on host builds)
    // ---- partial updates (all optional: partial=false or rect_b64=NULL keeps whole-screen output only) ----
    bool partial;                  // the receiver understands rectangles
    uint64_t now_ms, last_full_ms; // caller sets now_ms before each call; time of the last whole-screen encode
    bool full_stale;               // rectangles were sent since: base64 no longer match the picture
    unsigned full_bytes;           // size of the last whole-screen JPEG (cost estimate per tile)
    char *rect_b64;                // NATIVE_B64_MAX bytes: Base64 text of the rectangles
    unsigned rect_count;           // >0: the last encode produced these rectangles instead of a whole screen
    unsigned rect_used;
    NativeOutRect rects[NATIVE_MAX_RECTS];
    uint8_t dirty[NATIVE_TILES_X_MAX * NATIVE_TILES_Y]; // changed tiles of the last frame, row-major from the top
    // ---- automatic quality: the quality is chosen per frame from the number of changed tiles ----
    bool auto_quality;    // on: the quality argument is only the ceiling; the cache picks what fits the budget
    unsigned budget;      // bytes the whole frame (both screens) may take; set by the caller each frame
    unsigned other_tiles; // changed tiles expected on the other screen (shares the budget); 0 if none
    unsigned chosen;      // quality used by the last encode
    unsigned k_tile;      // learned JPEG bytes per 16x16 tile at quality 50 (0 = not learned yet)
    unsigned last_dirty;  // changed tiles in the last frame (the other screen's other_tiles)
    unsigned picture_q;   // lowest quality anywhere in the picture the receiver shows
    unsigned obs_bytes;   // JPEG bytes of the last encode
    uint8_t tile_q[NATIVE_TILES_X_MAX *
                   NATIVE_TILES_Y]; // quality each tile of the shown picture was sent at (automatic mode)
    unsigned refine_cursor;         // where the next sweep for tiles that can be sharpened starts
} NativeVideoCache;
// The best quality (100 down to the floor in steps of 10, not above max_q) whose predicted size for these tiles
// fits the budget; k_tile is bytes per tile at quality 50.
unsigned native_pick_quality(unsigned k_tile, unsigned tiles, unsigned budget, unsigned max_q);
// Reuse the proven column-major BGR8 4:2:0 encoder. Zero means unchanged, 1 a whole screen (base64),
// 2 rectangles (rect_count, rects, rect_b64), -1 an error.
int native_video_encode(NativeVideoCache *cache, const NativeFrame *frame, const uint8_t *raw, size_t raw_size,
                        unsigned quality, uint8_t *jpeg, size_t jpeg_size);
// Re-encodes the current picture as a whole screen (after rectangles made base64 stale). 1 or -1.
int native_video_refresh(NativeVideoCache *cache, unsigned quality, uint8_t *jpeg, size_t jpeg_size);
