#include "service_internal.h"

// The stream worker owns the Cast client. Each pass of its loop (stream_worker) does, in this order: (re)connect when
// needed, poll the TV, forward audio packets, send the once-a-second debug panel, and send one finished picture.
// Every step that can break the link returns false so the loop starts a fresh pass.

#define TICKS_PER_MS 268112ULL // ARM11 system ticks per millisecond (268111856 Hz)
#define STATS_INTERVAL_MS 1000 // the TV debug panel and the app's FPS figures refresh once a second
#define AUDIO_PACKETS_PER_PASS 2
#define IDLE_SLEEP_NS 20000000LL // no stream wanted: poll the request flags this often
#define FRAME_WAIT_NS 1000000LL  // nothing to send yet: look at the slots again after 1 ms
#define STATS_PANEL_TITLE_SIZE 224
#define STATS_JSON_SIZE 900

// Counters of the last panel update, to turn the running totals into per-frame averages.
typedef struct {
    unsigned long long net, conv, jpeg, b64, write, bytes, cols_total[2], cols_same[2];
    unsigned frames;
} StatsWindow;

typedef struct {
    CastClient client;
    bool client_ok, ever_streamed;
    CastConfig active;     // the configuration this connection was made with
    unsigned applied;      // the request generation being served
    unsigned source;       // NATIVE_SOURCE_GAME or NATIVE_SOURCE_SCREEN
    uint64_t stats_since;  // start of the current FPS window
    uint64_t next_stats;   // when the next debug panel is due
    unsigned stats_frames; // frames sent in the current FPS window
    unsigned send_ms;      // time spent sending them
    uint64_t last_done;
    unsigned send_max, gap_max; // worst single send and longest gap between frames, per panel window
    uint64_t first_frame_ms;
    unsigned long all_frames; // session averages (a single photo of the panel is too noisy)
    unsigned long long sent_bytes;
    StatsWindow window;
} StreamSession;

// Starts the encoder thread on the requested core (2 on a New 3DS), falling back to core 1 and then to any core.
static Thread start_encoder(unsigned *core, bool *fallback) {
    Thread encoder = NULL;
    *core = 0;
    *fallback = false;
    if (atomic_load(&encoder_core_request) == 2) {
        if (!atomic_load(&core2_failed)) {
            encoder = spawn_encoder(2);
        }
        if (encoder) {
            *core = 2;
        } else {
            atomic_store(&core2_failed, true);
            *fallback = true;
        }
    }
    if (!encoder) {
        encoder = spawn_encoder(1);
        if (encoder) {
            *core = 1;
        }
    }
    if (!encoder) {
        encoder = spawn_encoder(-1);
        if (encoder) {
            *core = 1;
        }
    }
    return encoder;
}

static Result init_network(void) {
    Result rc = 0;
    if (!soc_ok) {
        rc = socInit(soc_buffer, SOC_BUFFER_SIZE);
        soc_ok = rc == 0;
    }
    if (!rc && !ssl_ok) {
        rc = sslcInit(0);
        ssl_ok = rc == 0;
    }
    return rc;
}

// Connects to the TV and launches the receiver for the current request. False: the pass is over (the failure has
// been reported or a retry scheduled).
static bool connect_session(StreamSession *s, unsigned current) {
    atomic_store(&enc_enabled, false);
    if (s->applied != current) {
        s->ever_streamed = false;
    }
    if (s->client_ok) {
        cast_client_disconnect(&s->client);
    }
    LightLock_Lock(&state_lock);
    s->active = requested;
    s->source = requested_source;
    LightLock_Unlock(&state_lock);
    s->applied = current;
    atomic_store(&rects_mode, false); // renegotiated per connection
    s->stats_since = osGetTime();
    s->stats_frames = s->send_ms = 0;
    atomic_store(&stat_capture_ms, 0);
    atomic_store(&stat_encode_ms, 0);
    atomic_store(&active_generation, s->applied);
    set_status(NATIVE_CONNECTING, s->ever_streamed ? "Reconnecting to Google TV" : "Connecting directly to Google TV");
    Result rc = init_network();
    if (rc) {
        char error[128];
        snprintf(error, sizeof(error), "Network initialization: %08lX", (unsigned long)rc);
        request_failed(s->applied, error);
        return false;
    }
    if (!s->client_ok) {
        s->client_ok = cast_client_init(&s->client);
        if (!s->client_ok) {
            request_failed(s->applied, s->client.status_msg);
            cast_client_cleanup(&s->client);
            return false;
        }
        s->client.cancelled = cancelled;
    }
    s->client.restart_app = !s->ever_streamed; // only a fresh start restarts the TV app, not an automatic reconnect
    if (!cast_client_connect_and_launch(&s->client, &s->active)) {
        // Never connected in this session: report the error (wrong IP, TV off).
        // Already streaming before: keep retrying until the user presses stop.
        if (s->ever_streamed) {
            stream_retry(s->applied, s->client.status_msg);
        } else {
            request_failed(s->applied, s->client.status_msg);
        }
        return false;
    }
    s->ever_streamed = true;
    LightLock_Lock(&enc_lock);
    enc_cfg = s->active;
    enc_source = s->source;
    LightLock_Unlock(&enc_lock);
    atomic_fetch_add(&enc_epoch, 1);
    atomic_store(&enc_enabled, true);
    set_status(NATIVE_WAIT_GAME, s->source == NATIVE_SOURCE_SCREEN ? "Ready. Streaming the current screen"
                                                                   : "Ready. Open a 3DS game from HOME Menu");
    return true;
}

// Forwards queued audio packets. False: the link broke (a retry is scheduled).
static bool send_audio(StreamSession *s) {
    uint64_t title;
    LightLock_Lock(&state_lock);
    title = status.title_id;
    LightLock_Unlock(&state_lock);
    AudioPacket packet;
    for (unsigned sent = 0; sent < AUDIO_PACKETS_PER_PASS && audio_pop(&packet, s->applied, title); sent++) {
        if (!cast_client_send_audio(&s->client, &packet)) {
            cast_client_disconnect(&s->client);
            stream_retry(s->applied, "Audio send failed");
            break;
        }
        LightLock_Lock(&state_lock);
        status.audio_packets++;
        LightLock_Unlock(&state_lock);
    }
    return s->client.state == CAST_STATE_STREAMING;
}

// A running total's growth since the last panel, in tenths of ms (ticks) per frame.
static unsigned long tenth_ms_per_frame(unsigned long long total, unsigned long long *previous, unsigned frames) {
    unsigned long value = (unsigned long)((total - *previous) * 10ULL / TICKS_PER_MS / frames);
    *previous = total;
    return value;
}

// The panel's free-text 'title' line carries scheduling diagnostics (no HTML change): dsp = DSP block rewrites seen,
// wr = TLS write time, g = poll-gap histogram (<=2/<=5/<=10/<=50/>50 ms) and max gap in ms, same = share of 3DS
// framebuffer columns identical to the previous frame (per screen, T = top, B = bottom), part a/b = frames of the last
// second sent as partial rectangles / as whole screens, sx/gx = worst send and longest gap between frames.
static void format_panel_title(StreamSession *s, const NativeStatus *st, const unsigned dg[7], const unsigned hist[5],
                               unsigned maxgap, unsigned long wr, char *out, size_t size) {
    const char *cipher = cast_client_cipher(&s->client);
    const char *with = strstr(cipher, "WITH-");
    if (with) {
        cipher = with + 5;
    }
    unsigned long span = s->first_frame_ms ? (unsigned long)(osGetTime() - s->first_frame_ms) : 0;
    unsigned long avg10 = (span && s->all_frames) ? (unsigned long)(s->all_frames * 10000ULL / span) : 0; // FPS x10
    unsigned long avgkb = s->all_frames ? (unsigned long)(s->sent_bytes / 1024ULL / s->all_frames) : 0;   // KB/frame
    unsigned long same[2];
    for (unsigned i = 0; i < 2; i++) {
        unsigned long long dt = video[i].cols_total - s->window.cols_total[i];
        unsigned long long ds = video[i].cols_same - s->window.cols_same[i];
        same[i] = dt ? (unsigned long)(ds * 100ULL / dt) : 0;
        s->window.cols_total[i] = video[i].cols_total;
        s->window.cols_same[i] = video[i].cols_same;
    }
    unsigned part_rects = atomic_exchange(&stat_rect_frames, 0), part_full = atomic_exchange(&stat_full_frames, 0);
    snprintf(
        out, size,
        "%08lX%08lX dsp%u wr%lu.%lu g%u/%u/%u/%u/%u max%u avg%lu.%lufps/%luKB same T%lu/B%lu%% part%u/%u sx%u gx%u %s",
        (unsigned long)(st->title_id >> 32), (unsigned long)(st->title_id & 0xFFFFFFFFu), dg[6], wr / 10, wr % 10,
        hist[0], hist[1], hist[2], hist[3], hist[4], maxgap, avg10 / 10, avg10 % 10, avgkb, same[0], same[1],
        part_rects, part_full, s->send_max, s->gap_max, cipher);
    s->send_max = s->gap_max = 0;
}

// Once a second: the whole pipeline's numbers for the TV debug panel. False: the link broke.
static bool send_stats(StreamSession *s) {
    NativeStatus st;
    LightLock_Lock(&state_lock);
    st = status;
    LightLock_Unlock(&state_lock);
    NativeDspDebug d;
    unsigned kind, probes, reads, pushed, silent, dg[7], hist[5], maxgap;
    LightLock_Lock(&audio_lock);
    d = audio_dbg.dsp;
    kind = audio_dbg.kind;
    pushed = audio_dbg.pushed;
    memcpy(dg, audio_dbg.diag, sizeof(dg));
    memcpy(hist, audio_dbg.hist, sizeof(hist));
    maxgap = audio_dbg.maxgap;
    LightLock_Unlock(&audio_lock);
    probes = atomic_load(&dbg_probes);
    reads = atomic_load(&dbg_reads);
    silent = atomic_load(&dbg_silent);
    // Per-frame cost split (tenths of ms): TLS write, network send() vs TLS/CPU, and J's parts.
    StatsWindow *w = &s->window;
    unsigned frames = st.frames - w->frames;
    if (!frames || st.frames < w->frames) {
        frames = 1;
    }
    unsigned long wr = tenth_ms_per_frame(cast_write_ticks, &w->write, frames),
                  tn = tenth_ms_per_frame(cast_net_ticks, &w->net, frames),
                  jc = tenth_ms_per_frame(video[0].t_convert + video[1].t_convert, &w->conv, frames),
                  jj = tenth_ms_per_frame(video[0].t_jpeg + video[1].t_jpeg, &w->jpeg, frames),
                  jb = tenth_ms_per_frame(video[0].t_b64 + video[1].t_b64, &w->b64, frames);
    unsigned long kb = (unsigned long)((s->sent_bytes - w->bytes) * 10ULL / 1024ULL / frames);
    w->bytes = s->sent_bytes;
    w->frames = st.frames;
    char title[STATS_PANEL_TITLE_SIZE];
    format_panel_title(s, &st, dg, hist, maxgap, wr, title, sizeof(title));
    unsigned q_now = atomic_load(&stat_q_now), q_lo = atomic_exchange(&stat_q_min, 1000),
             q_hi = atomic_exchange(&stat_q_max, 0);
    if (q_hi == 0) {
        q_lo = q_hi = q_now; // no frame in the last second: repeat the last quality
    }
    char json[STATS_JSON_SIZE];
    snprintf(json, sizeof(json),
             "{\"type\":\"STATS\",\"fps\":%lu,\"r\":%lu,\"j\":%lu,\"t\":%lu,\"core\":%lu,\"fb\":%lu,"
             "\"title\":\"%s\",\"rej\":%lu,\"as\":%lu,\"ak\":%u,\"ap\":%u,\"ar\":%u,\"an\":%u,"
             "\"sent\":%lu,\"drop\":%lu,\"c\":[%u,%u,%u,%u],\"nz\":[%u,%u,%u,%u],\"h\":[%u,%u],"
             "\"tn\":%lu,\"jc\":%lu,\"jj\":%lu,\"jb\":%lu,\"sl\":%u,\"kb\":%lu,\"q\":%u,\"qn\":%u,\"qx\":%u,\"rc\":%u}",
             (unsigned long)st.fps, (unsigned long)st.capture_ms, (unsigned long)st.encode_ms,
             (unsigned long)st.send_ms, (unsigned long)st.worker_core, (unsigned long)st.core_fallback, title,
             (unsigned long)st.rejected, (unsigned long)st.audio_state, kind, probes, reads, pushed,
             (unsigned long)st.audio_packets, (unsigned long)st.audio_dropped, d.counter[0], d.counter[1], d.counter[2],
             d.counter[3], dg[0], dg[1], dg[2], dg[3], dg[4], dg[5], tn, jc, jj, jb, silent, kb, q_now, q_lo, q_hi,
             atomic_load(&stat_reconnects));
    if (!cast_client_send_json(&s->client, json)) {
        cast_client_disconnect(&s->client);
        stream_retry(s->applied, "Stats send failed");
        return false;
    }
    return true;
}

// Transmits one finished picture: as RECT messages when the receiver composes rectangles, else as whole screens.
static bool transmit_slot(StreamSession *s, const FrameSlot *slot) {
    if (slot->rect_mode && s->client.peer_rects) {
        // Partial updates: every rectangle (a whole screen is one) is its own small RECT message.
        CastRect list[2 * NATIVE_MAX_RECTS];
        unsigned count = 0;
        for (unsigned i = 0; i < 2; i++) {
            if (!slot->has[i]) {
                continue;
            }
            for (unsigned k = 0; k < slot->nrect[i] && count < 2 * NATIVE_MAX_RECTS; k++) {
                const NativeOutRect *r = &slot->rect[i][k];
                list[count++] = (CastRect){(uint8_t)i, r->x, r->y, r->w, r->h, slot->img[i] + r->off, r->len};
            }
        }
        return count > 0 && cast_client_send_rects(&s->client, &s->active, list, count);
    }
    const char *text[2] = {NULL, NULL};
    for (unsigned i = 0; i < 2; i++) {
        if (slot->has[i]) {
            text[i] = slot->img[i];
        }
    }
    return cast_client_send_frame(&s->client, &s->active, text[0], text[1]);
}

// Bookkeeping after a picture went out: worst send/gap, the once-a-second FPS window and the app's status line.
static void account_frame(StreamSession *s, unsigned slot_bytes, uint64_t sent_at) {
    uint64_t done = osGetTime();
    unsigned send_time = (unsigned)(done - sent_at);
    if (send_time > s->send_max) {
        s->send_max = send_time;
    }
    if (s->last_done) {
        unsigned gap = (unsigned)(done - s->last_done);
        if (gap > s->gap_max) {
            s->gap_max = gap;
        }
    }
    s->last_done = done;
    s->send_ms += osGetTime() - sent_at;
    s->stats_frames++;
    s->sent_bytes += slot_bytes;
    if (!s->first_frame_ms) {
        s->first_frame_ms = osGetTime();
    }
    s->all_frames++;
    LightLock_Lock(&state_lock);
    if (osGetTime() - s->stats_since >= STATS_INTERVAL_MS && s->stats_frames) {
        status.fps = s->stats_frames * 1000 / (osGetTime() - s->stats_since);
        status.capture_ms = atomic_exchange(&stat_capture_ms, 0) / s->stats_frames;
        status.encode_ms = atomic_exchange(&stat_encode_ms, 0) / s->stats_frames;
        status.send_ms = s->send_ms / s->stats_frames;
        s->stats_frames = s->send_ms = 0;
        s->stats_since = osGetTime();
    }
    if (s->applied == atomic_load(&generation) && atomic_load(&wanted)) {
        status.frames++;
        status.state = NATIVE_STREAMING;
        snprintf(status.message, sizeof(status.message), "%s",
                 s->source == NATIVE_SOURCE_SCREEN ? "Streaming screen directly to TV"
                                                   : "Streaming game video directly to TV");
    }
    LightLock_Unlock(&state_lock);
}

void stream_worker(void *argument) {
    unsigned token = (unsigned)(uintptr_t)argument;
    if (token != atomic_load(&worker_token)) {
        return; // started too late after a fallback
    }
    atomic_store(&chosen_token, token);
    atomic_store(&worker_started, true);
    StreamSession s = {0};
    s.client.sock_fd = -1;
    s.source = NATIVE_SOURCE_GAME;
    s.stats_since = osGetTime();
    unsigned encoder_core;
    bool core_fallback;
    Thread encoder = start_encoder(&encoder_core, &core_fallback);
    if (!encoder) {
        request_failed(atomic_load(&generation), "Could not start encoder thread");
        return;
    }
    LightLock_Lock(&state_lock);
    status.worker_core = encoder_core;
    status.core_fallback = core_fallback;
    LightLock_Unlock(&state_lock);
    while (!atomic_load(&quitting) && token == atomic_load(&worker_token)) {
        unsigned current = atomic_load(&generation);
        if (!atomic_load(&wanted)) {
            atomic_store(&enc_enabled, false);
            if (s.client_ok && s.client.state == CAST_STATE_STREAMING) {
                cast_client_disconnect(&s.client);
                set_status(NATIVE_IDLE, "Streaming stopped");
            }
            svcSleepThread(IDLE_SLEEP_NS);
            continue;
        }
        if ((s.client.state != CAST_STATE_STREAMING || s.applied != current) && !connect_session(&s, current)) {
            continue;
        }
        if (!atomic_load(&wanted)) {
            continue;
        }
        if (!cast_client_poll(&s.client, osGetTime())) {
            stream_retry(s.applied, s.client.status_msg);
            continue;
        }
        if (s.active.audio_enabled && !send_audio(&s)) {
            continue;
        }
        if (s.active.show_osd && osGetTime() >= s.next_stats) {
            s.next_stats = osGetTime() + STATS_INTERVAL_MS;
            if (!send_stats(&s)) {
                continue;
            }
        }
        atomic_store(&rects_mode,
                     s.client.peer_rects); // every pass: the receiver's HELLO may arrive while the picture is still
        FrameSlot *slot = take_ready_slot(atomic_load(&enc_epoch));
        if (!slot) {
            svcSleepThread(FRAME_WAIT_NS);
            continue;
        }
        uint64_t sent_at = osGetTime();
        unsigned slot_bytes = slot->bytes;
        bool sent = transmit_slot(&s, slot);
        atomic_store(&slot->state, SLOT_FREE);
        if (!sent) {
            cast_client_disconnect(&s.client);
            stream_retry(s.applied, "Frame send failed");
            continue;
        }
        account_frame(&s, slot_bytes, sent_at);
    }
    atomic_store(&enc_enabled, false);
    threadJoin(encoder, U64_MAX);
    threadFree(encoder);
    if (s.client_ok) {
        cast_client_cleanup(&s.client);
    }
}
