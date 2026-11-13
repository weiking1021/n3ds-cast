#include "service_internal.h"

#define DSP_RAM_BASE 0x1FF40000u
#define DSP_RAM_SIZE 0x40000u
#define DSP_MIX_BYTES 640         // one 160-sample stereo block of PCM16
#define PACKET_QUEUE_SIZE 3       // packets waiting for the sender (older ones are dropped)
#define DRAIN_GUARD 8             // most blocks drained per pass (a late thread owes several silent ones)
#define PROBE_RETRY_MS 200        // counters can momentarily be equal: look for the DSP layout again soon
#define STALL_RETRY_MS 500        // after a stall the layout is probed again this much later
#define LIVE_WINDOW_MS 1000       // a block this recent means the audio state is LIVE
#define STALL_MS 2000             // no block for this long: start over
#define DEBUG_INTERVAL_MS 1000    // how often the TV debug view is refreshed
#define ACTIVE_SLEEP_NS 1000000LL // layout known: poll every ms
#define IDLE_SLEEP_NS 20000000LL  // nothing to sample: poll the flags every 20 ms
#define AUDIO_RATE_PER_KBPS                                                                                            \
    250 // factor from the audio bitrate setting (kbit/s) to native_game_audio_reset's rate argument

// Audio sampling must continue during JPEG/TLS work. Only the main thread
// accesses Cast/TLS; the sampler publishes a bounded queue of owned packets.
LightLock audio_lock;
static AudioPacket audio_queue[PACKET_QUEUE_SIZE];
static unsigned audio_head, audio_count, audio_epoch;
static uint64_t audio_title;
// Debug view of the audio path, shown on the TV.
atomic_uint dbg_probes = 0, dbg_reads = 0, dbg_silent = 0;
AudioDebug audio_dbg; // protected by audio_lock
atomic_uint audio_gen = 0;
atomic_bool audio_started = false;

// Tracker tallies are cumulative for the whole stream: a game exit must not erase them.
static void fold_tracker(NativeDspTracker *t, unsigned total[7]) {
    total[0] += t->ticks;
    total[1] += t->real;
    total[2] += t->missed;
    total[3] += t->stale;
    total[4] += t->mid_write;
    total[5] += t->resync;
    total[6] += t->changes;
    memset(t, 0, sizeof(*t));
}
// The DSP RAM mapping may be cacheable. The DSP writes it behind the CPU's back, so stale cache
// lines make one 640-byte block a patchwork of different moments (seen: seams every 64 bytes).
static void dsp_invalidate(const NativeDspLayout *l, unsigned *failures) {
    const uintptr_t base = DSP_RAM_BASE;
    if (!l->valid) {
        return;
    }
    Result a = svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)(base + 0x10000 + l->mix), DSP_MIX_BYTES);
    Result b = svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)(base + 0x30000 + l->mix), DSP_MIX_BYTES);
    if (a != 0 || b != 0) {
        (*failures)++;
    }
}
static void dsp_invalidate_all(unsigned *failures) {
    Result r = svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)DSP_RAM_BASE, DSP_RAM_SIZE);
    if (r != 0) {
        (*failures)++;
    }
}
// State of the sampler thread.
typedef struct {
    NativeDspLayout layout;
    NativeGameAudio audio;
    NativeDspTracker tracker;
    uint16_t fake_counter;
    unsigned total[7], hist[5], maxgap; // tracker tallies of past runs, poll-gap histogram and worst gap
    uint64_t last_loop;
    unsigned inval_fail;
    unsigned epoch, kbps;
    uint64_t title, next_probe, last_frame, next_debug;
    bool synced; // audio_epoch/audio_title published to the sender
    AudioPacket held[PACKET_QUEUE_SIZE];
    unsigned held_count; // packets waiting for the queue lock
} Sampler;

// New request, other game or audio switched off: start the audio chain from scratch.
static void restart_stream(Sampler *s, unsigned epoch, uint64_t title) {
    s->epoch = epoch;
    s->title = title;
    s->layout.valid = false;
    s->next_probe = 0;
    s->last_frame = 0;
    s->last_loop = 0;
    fold_tracker(&s->tracker, s->total);
    native_game_audio_reset(&s->audio, s->kbps * AUDIO_RATE_PER_KBPS);
    s->held_count = 0;
    s->synced = false;
}

// Tells the sender which epoch/title the queue now belongs to (never waits: only try-locks).
static void sync_queue(Sampler *s) {
    if (s->synced || LightLock_TryLock(&audio_lock) != 0) {
        return;
    }
    audio_count = 0;
    audio_epoch = s->epoch;
    audio_title = s->title;
    memcpy(audio_dbg.diag, s->total, sizeof(s->total));
    LightLock_Unlock(&audio_lock);
    s->synced = true;
}

static void probe_layout(Sampler *s, uint64_t now) {
    if (s->layout.valid || now < s->next_probe) {
        return;
    }
    dsp_invalidate_all(&s->inval_fail);
    s->layout.valid = native_dsp_layout((const volatile uint8_t *)DSP_RAM_BASE, NATIVE_DSP_BYTES, &s->layout);
    s->next_probe = now + PROBE_RETRY_MS;
    atomic_fetch_add(&dbg_probes, 1);
}

// Cheap debug view for the TV: four counter words plus the tracker's own tallies (no RAM survey).
static void publish_debug(Sampler *s, uint64_t now) {
    if (now < s->next_debug || LightLock_TryLock(&audio_lock) != 0) {
        return;
    }
    const volatile uint8_t *dram = (const volatile uint8_t *)DSP_RAM_BASE;
    for (unsigned bank = 0; bank < 4; bank++) {
        unsigned at = bank * 0x10000u + 0x7FFEu;
        audio_dbg.dsp.counter[bank] = (uint16_t)(dram[at] | (dram[at + 1] << 8));
    }
    audio_dbg.kind = s->layout.valid ? (s->layout.table ? 1 : 2) : 0;
    audio_dbg.pushed = s->audio.sequence;
    audio_dbg.diag[0] = s->total[0] + s->tracker.ticks;
    audio_dbg.diag[1] = s->total[1] + s->tracker.real;
    audio_dbg.diag[2] = s->total[2] + s->tracker.missed;
    audio_dbg.diag[3] = s->total[3] + s->tracker.stale;
    audio_dbg.diag[4] = s->total[4] + s->tracker.mid_write;
    audio_dbg.diag[5] = s->total[5] + s->tracker.resync;
    audio_dbg.diag[6] = s->total[6] + s->tracker.changes;
    memcpy(audio_dbg.hist, s->hist, sizeof(s->hist));
    audio_dbg.maxgap = s->maxgap;
    LightLock_Unlock(&audio_lock);
    s->next_debug = now + DEBUG_INTERVAL_MS;
}

// Poll-gap histogram (<=2/<=5/<=10/<=50/>50 ms) of the sampler loop, for the TV debug view.
static void record_gap(Sampler *s, uint64_t now) {
    if (s->last_loop) {
        unsigned gap = (unsigned)(now - s->last_loop);
        s->hist[gap <= 2 ? 0 : gap <= 5 ? 1 : gap <= 10 ? 2 : gap <= 50 ? 3 : 4]++;
        if (gap > s->maxgap) {
            s->maxgap = gap;
        }
    }
    s->last_loop = now;
}

// Drains every block that is due and turns them into packets (held until the queue lock is free).
static void drain_blocks(Sampler *s, uint64_t now) {
    int16_t stereo[DSP_MIX_BYTES / 2];
    for (unsigned guard = 0; guard < DRAIN_GUARD; guard++) {
        // The DSP RAM may be cacheable and the DSP does not keep the CPU cache coherent: drop
        // our cached copy of the mix buffers (and counters) so every read sees what the DSP wrote.
        dsp_invalidate(&s->layout, &s->inval_fail);
        int rc = native_dsp_poll((const volatile uint8_t *)DSP_RAM_BASE, NATIVE_DSP_BYTES, &s->layout, &s->tracker, now,
                                 stereo);
        if (rc < 0) {
            s->layout.valid = false;
            break;
        }
        if (rc == 0) {
            break;
        }
        atomic_fetch_add(rc == 1 ? &dbg_reads : &dbg_silent, 1);
        s->last_frame = now;
        s->fake_counter = (uint16_t)(s->fake_counter + 1);
        if (s->fake_counter < 2) {
            s->fake_counter = 2;
        }
        AudioPacket packet;
        bool made = native_game_audio_push(&s->audio, s->fake_counter, stereo, &packet);
        if (made && s->held_count < PACKET_QUEUE_SIZE) {
            s->held[s->held_count++] = packet;
        }
    }
}

// Hands the held packets to the sender's queue; the oldest are dropped when it is full.
static void flush_held(Sampler *s) {
    if (!s->held_count || !s->synced || LightLock_TryLock(&audio_lock) != 0) {
        return;
    }
    for (unsigned i = 0; i < s->held_count; i++) {
        if (audio_count == PACKET_QUEUE_SIZE) {
            audio_head = (audio_head + 1) % PACKET_QUEUE_SIZE;
            audio_count--;
            s->audio.dropped++;
        }
        audio_queue[(audio_head + audio_count) % PACKET_QUEUE_SIZE] = s->held[i];
        audio_count++;
    }
    s->held_count = 0;
    LightLock_Unlock(&audio_lock);
}

// One pass over a running game's audio. Returns the audio state to report.
static unsigned sample_game_audio(Sampler *s, uint64_t now) {
    probe_layout(s, now);
    publish_debug(s, now);
    unsigned state = s->layout.valid ? NATIVE_AUDIO_WAIT : NATIVE_AUDIO_UNSUPPORTED;
    if (!s->layout.valid) {
        return state;
    }
    record_gap(s, now);
    drain_blocks(s, now);
    flush_held(s);
    if (s->last_frame && now - s->last_frame < LIVE_WINDOW_MS) {
        state = NATIVE_AUDIO_LIVE;
    }
    if (s->last_frame && now - s->last_frame > STALL_MS) {
        native_game_audio_reset(&s->audio, s->kbps * AUDIO_RATE_PER_KBPS);
        s->held_count = 0;
        s->layout.valid = false;
        s->next_probe = now + STALL_RETRY_MS;
        fold_tracker(&s->tracker, s->total);
        s->synced = false;
    }
    return state;
}

void audio_worker(void *argument) {
    if ((unsigned)(uintptr_t)argument != atomic_load(&audio_gen)) {
        return; // started too late after a fallback
    }
    atomic_store(&audio_started, true);
    Sampler s = {0};
    while (!atomic_load(&quitting)) {
        unsigned current = atomic_load(&generation);
        uint64_t game = ((uint64_t)atomic_load(&game_hi) << 32) | atomic_load(&game_lo);
        unsigned audio_on = atomic_load(&cfg_audio_enabled);
        s.kbps = atomic_load(&cfg_audio_kbps);
        bool wanted_now = atomic_load(&wanted);
        bool enabled = wanted_now && audio_on && game;
        if (current != s.epoch || game != s.title || !enabled) {
            restart_stream(&s, current, game);
        }
        sync_queue(&s);
        unsigned state = (!audio_on || !wanted_now) ? NATIVE_AUDIO_OFF : NATIVE_AUDIO_WAIT;
        if (enabled) {
            state = sample_game_audio(&s, osGetTime());
        }
        if (LightLock_TryLock(&state_lock) == 0) {
            if (current == atomic_load(&generation)) {
                status.audio_state = state;
                status.audio_dropped = s.audio.dropped;
            }
            LightLock_Unlock(&state_lock);
        }
        if (!(enabled && s.layout.valid)) {
            s.last_loop = 0; // idle gaps are not scheduling gaps
        }
        svcSleepThread(enabled && s.layout.valid ? ACTIVE_SLEEP_NS : IDLE_SLEEP_NS);
    }
}
bool audio_pop(AudioPacket *packet, unsigned epoch, uint64_t title) {
    bool found = false;
    LightLock_Lock(&audio_lock);
    if (audio_count && audio_epoch == epoch && audio_title == title) {
        *packet = audio_queue[audio_head];
        audio_head = (audio_head + 1) % PACKET_QUEUE_SIZE;
        audio_count--;
        found = true;
    }
    LightLock_Unlock(&audio_lock);
    return found;
}
