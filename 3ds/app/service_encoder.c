#include "service_internal.h"

// ---- Encoder/sender pipeline -------------------------------------------------
// The encoder thread captures and JPEG-encodes frame N+1 while the stream worker
// (sole owner of the TLS client) is still transmitting frame N. Two slots give a
// pipeline depth of two; the encoder blocks (never drops) when both are in use,
// so the change-detection cache always matches what the TV has been sent.
uint8_t *raw, *jpeg;
NativeVideoCache video[2];
char *b64[2];
FrameSlot slots[2];
// Set by the sender once the receiver said it composes rectangles; the encoder then produces partial updates.
atomic_bool rects_mode = false;
atomic_uint stat_rect_frames = 0, stat_full_frames = 0;
atomic_uint stat_q_min = 1000, stat_q_max = 0,
            stat_q_now = 0; // JPEG quality used in the last second (for the TV OSD)
LightLock enc_lock;
CastConfig enc_cfg;
unsigned enc_source;
atomic_bool enc_enabled = false;
atomic_uint enc_epoch = 0;
atomic_uint stat_capture_ms = 0, stat_encode_ms = 0;
// The stream worker (TLS sender) runs on core 1 at higher priority; the encoder
// runs on the user's chosen core so the two genuinely overlap (core 2 on New 3DS).
atomic_int encoder_core_request = 1;
static atomic_uint encoder_gen = 0;
atomic_bool encoder_started = false, core2_failed = false;

#define SCREEN_HEIGHT 240
#define REFRESH_INTERVAL_MS 1000    // an unchanged screen is still re-sent this often
#define IDLE_SLEEP_NS 10000000LL    // streaming switched off: look again after 10 ms
#define NO_GAME_SLEEP_NS 20000000LL // no game running: look again after 20 ms
#define SLOT_WAIT_NS 1000000LL      // both slots busy / frame not due yet: look again after 1 ms
#define QUALITY_BACKOFF_MS 3000     // a screen that overflowed the buffer stays at the lower quality this long
#define MIN_BACKOFF_QUALITY 60
#define LATE_WINDOW_MS 400 // how often the lateness feedback is evaluated
#define BUDGET_MIN_PCT 12
#define BUDGET_MAX_PCT 100

// State of one encoder thread run.
typedef struct {
    NativeCapture capture;
    NativeScreenCapture screen;
    CastConfig cfg; // the configuration of the current epoch
    unsigned source, epoch;
    uint64_t next_frame, last_refresh, last_title, seq;
    // Automatic quality also watches the clock: a frame that starts later than it was due means the picture
    // cost more than the link and the CPU allow, so the byte budget shrinks (percent of the nominal budget);
    // frames that start on time let it grow back. The size prediction alone is only a guess.
    unsigned budget_pct, late_sum, late_frames;
    uint64_t late_window;
    int quality_cap[2]; // lowered quality of a screen whose picture did not fit, until quality_until
    uint64_t quality_until[2];
    bool open; // capture handles may be open
} EncoderRun;

static void note_rejected(void) {
    LightLock_Lock(&state_lock);
    status.rejected++;
    LightLock_Unlock(&state_lock);
}

static void publish_game_title(uint64_t title) {
    LightLock_Lock(&state_lock);
    status.title_id = title;
    publish_title(title);
    LightLock_Unlock(&state_lock);
}

static void invalidate_pictures(void) {
    video[0].valid = video[1].valid = false;
}

// Streaming was switched off: let go of the game and forget what was sent.
static void close_capture(EncoderRun *e) {
    if (e->open) {
        native_capture_close(&e->capture);
        native_capture_screen_close(&e->screen);
        invalidate_pictures();
        e->open = false;
        publish_game_title(0);
    }
    e->epoch = ~0u;
}

// A new connection (epoch) starts from the configuration the stream worker negotiated.
static void adopt_epoch(EncoderRun *e, unsigned epoch) {
    LightLock_Lock(&enc_lock);
    e->cfg = enc_cfg;
    e->source = enc_source;
    LightLock_Unlock(&enc_lock);
    e->epoch = epoch;
    invalidate_pictures();
    e->next_frame = e->last_refresh = 0;
    e->last_title = 0;
    e->budget_pct = BUDGET_MAX_PCT;
    e->late_sum = e->late_frames = 0;
    e->late_window = 0;
}

// The title of what is being captured. False: no game to capture yet (status updated).
static bool find_title(EncoderRun *e, uint64_t *title) {
    if (e->source == NATIVE_SOURCE_SCREEN) {
        *title = e->screen.title_id[0] ? e->screen.title_id[0] : e->screen.title_id[1] ? e->screen.title_id[1] : 1;
        return true;
    }
    if (!native_capture_game(&e->capture)) {
        set_status(NATIVE_WAIT_GAME, "Waiting for an active 3DS game");
        publish_game_title(0);
        return false;
    }
    *title = e->capture.title_id;
    return true;
}

static int free_slot_index(void) {
    for (int i = 0; i < 2; i++) {
        if (atomic_load(&slots[i].state) == SLOT_FREE) {
            return i;
        }
    }
    return -1;
}

// Lateness feedback for the automatic quality: shrink the byte budget when frames start late, regrow it when on time.
static void track_lateness(EncoderRun *e, uint64_t now) {
    if (e->next_frame && now > e->next_frame) {
        e->late_sum += (unsigned)(now - e->next_frame);
    }
    e->late_frames++;
    if (!e->late_window) {
        e->late_window = now;
        return;
    }
    if (now - e->late_window < LATE_WINDOW_MS) {
        return;
    }
    unsigned interval = 1000 / e->cfg.target_fps, average = e->late_frames ? e->late_sum / e->late_frames : 0;
    if (average * 4 > interval) {
        e->budget_pct = e->budget_pct * 70 / 100; // more than a quarter interval behind
    } else if (average * 10 < interval) {
        e->budget_pct += e->budget_pct / 6 + 1; // comfortably on time
    }
    if (e->budget_pct < BUDGET_MIN_PCT) {
        e->budget_pct = BUDGET_MIN_PCT;
    }
    if (e->budget_pct > BUDGET_MAX_PCT) {
        e->budget_pct = BUDGET_MAX_PCT;
    }
    e->late_sum = e->late_frames = 0;
    e->late_window = now;
}

// JPEG bytes one frame may use: 9/10 of the link rate over the target FPS, as JPEG (Base64 adds a third: 3/4).
static unsigned frame_byte_budget(const EncoderRun *e) {
    return (unsigned)((uint64_t)NATIVE_AUTO_LINK * 9 / 10 * 3 / 4 / e->cfg.target_fps * e->budget_pct / 100);
}

// Automatic quality (setting 0): the cache picks the quality from how many tiles changed and this frame's byte budget.
// The first screen shares the budget with the other in proportion to the tiles each has to send; the second gets what
// is left.
static void set_auto_budget(const EncoderRun *e, unsigned screen) {
    unsigned budget = frame_byte_budget(e);
    if (screen == 0) {
        bool other_shown = e->cfg.layout_mode != LAYOUT_TOP_ONLY; // the bottom screen is encoded too
        video[0].budget = budget;
        video[0].other_tiles = other_shown ? video[1].last_dirty : 0;
    } else {
        unsigned used = video[0].obs_bytes, floor = budget / 4;
        video[1].budget = budget > used + floor ? budget - used : floor;
        video[1].other_tiles = 0;
    }
}

// Captures one screen and encodes it. A picture too big for the buffer (quality 100 on a busy scene) is not dropped:
// this screen steps down in quality for a few seconds, then tries the configured quality again.
// result: native_video_encode outcome (0 unchanged, 1 whole screen, 2 rectangles); use: this screen goes in the frame.
static void encode_screen(EncoderRun *e, unsigned i, bool rects, uint64_t now, bool *use, int *result,
                          unsigned *readable) {
    NativeFrame frame;
    uint64_t stage = osGetTime();
    bool ok = e->source == NATIVE_SOURCE_SCREEN
                  ? native_capture_screen(&e->screen, i == 0, &frame, raw, RAW_BUFFER_SIZE)
                  : native_capture_frame(&e->capture, i == 0, &frame, raw, RAW_BUFFER_SIZE);
    if (!ok) {
        note_rejected();
        return;
    }
    atomic_fetch_add(&stat_capture_ms, (unsigned)(osGetTime() - stage));
    (*readable)++;
    stage = osGetTime();
    video[i].partial = rects;
    video[i].now_ms = now;
    const bool automatic = e->cfg.jpeg_quality == NATIVE_QUALITY_AUTO;
    video[i].auto_quality = automatic;
    video[i].obs_bytes = 0;
    if (automatic) {
        set_auto_budget(e, i);
    }
    int quality = automatic ? 100 : e->cfg.jpeg_quality;
    if (now < e->quality_until[i] && quality > e->quality_cap[i]) {
        quality = e->quality_cap[i];
    }
    int encoded = native_video_encode(&video[i], &frame, raw, RAW_BUFFER_SIZE, quality, jpeg, NATIVE_JPEG_MAX);
    while (encoded < 0 && quality > MIN_BACKOFF_QUALITY) {
        quality = quality > 95 ? 95 : quality - 10;
        e->quality_cap[i] = quality;
        e->quality_until[i] = now + QUALITY_BACKOFF_MS;
        encoded = native_video_encode(&video[i], &frame, raw, RAW_BUFFER_SIZE, quality, jpeg, NATIVE_JPEG_MAX);
    }
    // Unchanged screen, refresh due, but partial updates made the cached whole screen out of date:
    if (encoded == 0 && now - e->last_refresh >= REFRESH_INTERVAL_MS && video[i].full_stale) {
        encoded = native_video_refresh(&video[i], quality, jpeg, NATIVE_JPEG_MAX);
    }
    atomic_fetch_add(&stat_encode_ms, (unsigned)(osGetTime() - stage));
    if (encoded == 2) {
        atomic_fetch_add(&stat_rect_frames, 1);
    } else if (encoded == 1) {
        atomic_fetch_add(&stat_full_frames, 1);
    }
    result[i] = encoded;
    if (encoded > 0 || (encoded == 0 && now - e->last_refresh >= REFRESH_INTERVAL_MS)) {
        use[i] = true;
    } else if (encoded < 0) {
        note_rejected();
    }
}

// Copies the encoded screens into a slot for the sender. Returns the Base64 bytes carried.
static unsigned fill_slot(FrameSlot *slot, bool rects, const bool *use, const int *result) {
    unsigned total_bytes = 0;
    slot->rect_mode = rects;
    slot->nrect[0] = slot->nrect[1] = 0;
    for (unsigned i = 0; i < 2; i++) {
        slot->has[i] = use[i];
        if (!use[i]) {
            continue;
        }
        if (rects && result[i] == 2) { // partial-update mode: rectangles
            memcpy(slot->img[i], video[i].rect_b64, video[i].rect_used);
            for (unsigned k = 0; k < video[i].rect_count; k++) {
                slot->rect[i][k] = video[i].rects[k];
            }
            slot->nrect[i] = video[i].rect_count;
            total_bytes += video[i].rect_used;
        } else if (rects) { // partial-update mode: the whole screen as one rectangle
            size_t n = strlen(video[i].base64);
            memcpy(slot->img[i], video[i].base64, n);
            slot->rect[i][0] = (NativeOutRect){0, 0, (uint16_t)video[i].width, SCREEN_HEIGHT, 0, (uint32_t)n};
            slot->nrect[i] = 1;
            total_bytes += (unsigned)n;
        } else {
            size_t n = strlen(video[i].base64);
            memcpy(slot->img[i], video[i].base64, n + 1);
            total_bytes += (unsigned)n;
        }
    }
    return total_bytes;
}

// The quality in use, for the app's status line and the TV's per-second min/max.
static void publish_quality(const EncoderRun *e, const bool *use) {
    uint32_t used = e->cfg.jpeg_quality != NATIVE_QUALITY_AUTO ? (uint32_t)e->cfg.jpeg_quality
                    : use[0] && video[0].chosen                ? video[0].chosen
                                                               : video[1].chosen;
    LightLock_Lock(&state_lock);
    status.quality = used;
    LightLock_Unlock(&state_lock);
    if (used) {
        atomic_store(&stat_q_now, used);
        unsigned lo = atomic_load(&stat_q_min), hi = atomic_load(&stat_q_max);
        if (used < lo) {
            atomic_store(&stat_q_min, used);
        }
        if (used > hi) {
            atomic_store(&stat_q_max, used);
        }
    }
}

static void encoder_thread(void *argument) {
    unsigned gen = (unsigned)(uintptr_t)argument;
    if (gen != atomic_load(&encoder_gen)) {
        return; // started too late after a fallback
    }
    unsigned token = atomic_load(&worker_token);
    atomic_store(&encoder_started, true);
    EncoderRun e = {
        .source = NATIVE_SOURCE_GAME, .epoch = ~0u, .budget_pct = BUDGET_MAX_PCT, .quality_cap = {100, 100}};
    bool prev_rects = false;
    while (!atomic_load(&quitting) && token == atomic_load(&worker_token) && gen == atomic_load(&encoder_gen)) {
        if (!atomic_load(&enc_enabled)) {
            close_capture(&e);
            svcSleepThread(IDLE_SLEEP_NS);
            continue;
        }
        e.open = true;
        unsigned current_epoch = atomic_load(&enc_epoch);
        if (current_epoch != e.epoch) {
            adopt_epoch(&e, current_epoch);
        }
        uint64_t now = osGetTime();
        if (now < e.next_frame) {
            svcSleepThread(SLOT_WAIT_NS);
            continue;
        }
        uint64_t title = 0;
        if (!find_title(&e, &title)) {
            svcSleepThread(NO_GAME_SLEEP_NS);
            continue;
        }
        publish_game_title(title);
        if (e.last_title != title) {
            invalidate_pictures();
            e.last_title = title;
        }
        int free_slot = free_slot_index();
        if (free_slot < 0) {
            svcSleepThread(SLOT_WAIT_NS);
            continue;
        }
        track_lateness(&e, now);
        e.next_frame = now + 1000 / e.cfg.target_fps;
        bool use[2] = {false, false};
        int result[2] = {0, 0};
        unsigned readable = 0;
        bool rects = atomic_load(&rects_mode);
        if (rects != prev_rects) {
            invalidate_pictures(); // first frame in the new mode is a whole screen
            prev_rects = rects;
        }
        for (unsigned i = 0; i < 2; i++) {
            if ((i == 0 && e.cfg.layout_mode == LAYOUT_BOTTOM_ONLY) ||
                (i == 1 && e.cfg.layout_mode == LAYOUT_TOP_ONLY)) {
                continue;
            }
            encode_screen(&e, i, rects, now, use, result, &readable);
        }
        if (!use[0] && !use[1]) {
            if (!video[0].valid && !video[1].valid) {
                set_status(NATIVE_WAIT_GAME, readable ? "Frame exceeds capacity; lower JPEG quality"
                                             : e.source == NATIVE_SOURCE_SCREEN
                                                 ? "Waiting for a readable screen"
                                                 : "Waiting for a readable game framebuffer");
            }
            continue;
        }
        FrameSlot *slot = &slots[free_slot];
        slot->bytes = fill_slot(slot, rects, use, result);
        slot->epoch = e.epoch;
        slot->seq = ++e.seq;
        e.last_refresh = now;
        publish_quality(&e, use);
        atomic_store(&slot->state, SLOT_READY);
    }
    native_capture_close(&e.capture);
    native_capture_screen_close(&e.screen);
}

FrameSlot *take_ready_slot(unsigned epoch) {
    FrameSlot *best = NULL;
    for (int i = 0; i < 2; i++) {
        if (atomic_load(&slots[i].state) != SLOT_READY) {
            continue;
        }
        if (slots[i].epoch != epoch) {
            atomic_store(&slots[i].state, SLOT_FREE);
            continue;
        } // stale after reconnect
        if (!best || slots[i].seq < best->seq) {
            best = &slots[i];
        }
    }
    if (best) {
        atomic_store(&best->state, SLOT_SENDING);
    }
    return best;
}

Thread spawn_encoder(int core) {
    unsigned gen = atomic_fetch_add(&encoder_gen, 1) + 1;
    atomic_store(&encoder_started, false);
    Thread thread = threadCreate(encoder_thread, (void *)(uintptr_t)gen, 0x10000, 0x30, core, false);
    if (!thread) {
        return NULL;
    }
    for (unsigned i = 0; i < 25 && !atomic_load(&encoder_started); i++) {
        svcSleepThread(10000000); // 250 ms
    }
    if (!atomic_load(&encoder_started)) {
        // Never join/free a thread that has not run; the new generation makes it
        // return immediately if its core ever schedules it.
        atomic_fetch_add(&encoder_gen, 1);
        return NULL;
    }
    return thread;
}
