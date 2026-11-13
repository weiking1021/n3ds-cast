#include "game_audio.h"
#include <string.h>
static uint16_t read_word(const volatile uint8_t *p, unsigned at) {
    return (uint16_t)p[at] | ((uint16_t)p[at + 1] << 8);
}
static bool table_valid(const uint16_t *table) {
    for (unsigned i = 0; i < 15; i++) {
        if (table[i] < 0x8000 || table[i] >= 0xC000) {
            return false;
        }
        for (unsigned j = 0; j < i; j++) {
            if (table[j] == table[i]) {
                return false;
            }
        }
    }
    // The entire output, status and counter must fit their advertised banks.
    if (table[6] + 320 > 0xC000 || table[5] + 16 > 0xC000) {
        return false;
    }
    for (unsigned i = 0; i < 15; i++) {
        if (i != 6 && table[i] >= table[6] && table[i] < table[6] + 320) {
            return false;
        }
    }
    return true;
}
bool native_dsp_layout(const volatile uint8_t *dram, size_t size, NativeDspLayout *out) {
    if (!dram || !out || size < NATIVE_DSP_BYTES) {
        return false;
    }
    NativeDspLayout found = {0};
    // Pipe payloads reside in the first DRAM bank; never consume a game pipe.
    for (unsigned at = 0; at + 32 <= 0x10000; at += 2) {
        if (read_word(dram, at) != 15) {
            continue;
        }
        uint16_t table[15];
        for (unsigned i = 0; i < 15; i++) {
            table[i] = read_word(dram, at + 2 + i * 2);
        }
        if (!table_valid(table)) {
            continue;
        }
        unsigned mix = (table[6] - 0x8000) * 2, counter = (table[0] - 0x8000) * 2;
        if (found.valid && (found.mix != mix || found.counter != counter)) {
            return false;
        }
        found = (NativeDspLayout){.table = at, .counter = counter, .mix = mix, .valid = true};
        memcpy(found.words, table, sizeof(table));
    }
    if (!found.valid) {
        // The game consumed the init message before we attached. Fall back to the
        // standard Nintendo DSP firmware (libctru ndspVars[0]/[6]): ping-pong frame
        // counters at 0x7FFE of banks 1 and 3, 160 stereo PCM16 mix at 0x0A80.
        // Only this documented location is trusted; no heuristic scanning.
        uint16_t a = read_word(dram, 0x10000 + 0x7FFE), b = read_word(dram, 0x30000 + 0x7FFE);
        // libctru ndsp.c: slot 0 only ever holds even frame ids (init writes 4) and slot 1
        // odd ones. The two counters drift apart whenever the game's audio thread lags
        // (seen: 0x3066 vs 0x3045), so closeness is NOT a valid test; parity is.
        if (a && b && (a & 1) == 0 && (b & 1) == 1) {
            found = (NativeDspLayout){.table = 0, .counter = 0x7FFE, .mix = 0x0A80, .valid = true};
            found.words[0] = 0xBFFF;
            found.words[6] = 0x8540;
        }
    }
    if (!found.valid) {
        return false;
    }
    *out = found;
    return true;
}
int native_dsp_read(const volatile uint8_t *dram, size_t size, const NativeDspLayout *l, uint16_t *counter,
                    int16_t stereo[320]) {
    if (!dram || size < NATIVE_DSP_BYTES || !l || !l->valid || !counter || !stereo || l->mix + 640 > 0x8000 ||
        l->counter + 2 > 0x8000) {
        return -1;
    }
    if (l->table && read_word(dram, l->table) == 15) {
        for (unsigned i = 0; i < 15; i++) {
            if (read_word(dram, l->table + 2 + i * 2) != l->words[i]) {
                return -1;
            }
        }
    }
    uint16_t a = read_word(dram, 0x10000 + l->counter), b = read_word(dram, 0x30000 + l->counter);
    if (!a || !b || a == b) {
        return 0;
    }
    int16_t diff = (int16_t)(a - b);
    if (diff < -0x4000 || diff > 0x4000) {
        return 0;
    }
    // The game publishes a new counter after consuming that bank's output.
    // The DSP is processing the opposite bank. Handle the nonzero wraparound.
    unsigned bank = diff > 0 ? 0 : 1;
    unsigned base = bank ? 0x30000 : 0x10000;
    uint16_t newest = bank ? b : a;
    for (unsigned i = 0; i < 320; i++) {
        stereo[i] = (int16_t)read_word(dram, base + l->mix + i * 2);
    }
    if (a != read_word(dram, 0x10000 + l->counter) || b != read_word(dram, 0x30000 + l->counter)) {
        return 0;
    }
    *counter = newest;
    return 1;
}
void native_game_audio_reset(NativeGameAudio *s, unsigned rate) {
    memset(s, 0, sizeof(*s));
    s->rate = rate;
}
bool native_game_audio_push(NativeGameAudio *s, uint16_t counter, const int16_t stereo[320], AudioPacket *p) {
    if (!s || !stereo || !p || (s->rate != 8000 && s->rate != 16000 && s->rate != 32000)) {
        return false;
    }
    if (s->primed && counter == s->counter) {
        return false;
    }
    // Frames missed because this thread was not scheduled (the DSP ticks every
    // ~4.9 ms) are replaced by silence so the timeline stays intact. Only a large
    // jump is a real discontinuity that discards the partial packet.
    unsigned missed = 0;
    bool continuous = false;
    if (s->primed) {
        // The counter skips 0 and 1 when it wraps: 0xFFFF is followed by 2.
        uint32_t delta =
            counter > s->counter ? (uint32_t)(counter - s->counter) : (uint32_t)(0x10000u - s->counter) + counter - 2u;
        if (counter > 1 && delta >= 1 && delta <= NATIVE_AUDIO_MAX_GAP) {
            continuous = true;
            missed = delta - 1;
        }
    }
    if (!continuous) {
        if (s->primed) {
            s->dropped++;
            s->sequence++;
        }
        s->counter = counter;
        s->primed = true;
        s->phase = s->count = 0;
        s->average_sum = 0;
        s->average_count = 0;
        return false;
    }
    s->counter = counter;
    bool ready = false;
    static const int16_t silence[320] = {0};
    if (missed) {
        s->dropped += missed;
    }
    for (unsigned frame = 0; frame <= missed; frame++) {
        const int16_t *input = frame < missed ? silence : stereo;
        for (unsigned i = 0; i < 160; i++) {
            s->average_sum += ((int)input[i * 2] + input[i * 2 + 1]) / 2;
            s->average_count++;
            s->phase += s->rate;
            if (s->phase < 32728) {
                continue;
            }
            s->phase -= 32728;
            s->pcm[s->count++] = (int16_t)(s->average_sum / (int)s->average_count);
            s->average_sum = 0;
            s->average_count = 0;
            if (s->count == s->rate / 10) {
                p->sequence = s->sequence++;
                p->sample_rate = s->rate;
                p->samples = s->count;
                p->size = audio_encode_ima_mono(s->pcm, s->count, p->data, sizeof(p->data));
                ready = p->size != 0;
                s->count = 0;
            }
        }
    }
    return ready;
}
void native_dsp_debug(const volatile uint8_t *dram, size_t size, NativeDspDebug *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!dram || size < NATIVE_DSP_BYTES) {
        return;
    }
    for (unsigned bank = 0; bank < 4; bank++) {
        unsigned base = bank * 0x10000u, count = 0;
        out->counter[bank] = read_word(dram, base + 0x7FFE);
        for (unsigned at = 0; at < 0x10000; at += 16) {
            if (read_word(dram, base + at)) {
                count++;
            }
        }
        out->nonzero[bank] = (uint16_t)count;
    }
    for (unsigned at = 0; at + 32 <= 0x10000; at += 2) {
        if (read_word(dram, at) != 15) {
            continue;
        }
        out->headers++;
        uint16_t table[15];
        for (unsigned i = 0; i < 15; i++) {
            table[i] = read_word(dram, at + 2 + i * 2);
        }
        if (table_valid(table)) {
            out->tables++;
        }
    }
}
static uint32_t block_fingerprint(const volatile uint8_t *dram, unsigned base) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < 320; i += 7) {
        h = (h ^ read_word(dram, base + i * 2)) * 16777619u;
    }
    return h;
}
static void copy_block(const volatile uint8_t *dram, unsigned base, int16_t *out) {
    for (unsigned i = 0; i < 320; i++) {
        out[i] = (int16_t)read_word(dram, base + i * 2);
    }
}
// The DSP writes one 160-frame block per ~4.889 ms into one of the two mix buffers
// (Azahar hle.cpp: always the region with the LOWER frame counter). The game bumps the
// counters only about every 1.9 ticks (measured: 13616 buffer rewrites vs 7336 counter
// steps in the same minute), so the counters are NOT a timeline and the DSP may rewrite
// the SAME buffer on consecutive ticks. The buffer CONTENT is the truth: a block is new
// when a buffer's content differs from the last block emitted from it and has been
// unchanged since the previous poll (the DSP finished writing); the block is then read
// twice and must match. Wall-clock pacing fills real silence and gaps this thread missed.
int native_dsp_poll(const volatile uint8_t *dram, size_t size, const NativeDspLayout *l, NativeDspTracker *t,
                    uint64_t now_ms, int16_t stereo[320]) {
    if (!dram || size < NATIVE_DSP_BYTES || !l || !l->valid || !t || !stereo || l->mix + 640 > 0x8000) {
        return -1;
    }
    const uint64_t frame = NATIVE_DSP_FRAME_US;
    uint64_t now_us = now_ms * 1000u;
    if (t->pending_silence) {
        t->pending_silence--;
        memset(stereo, 0, 640);
        return 2;
    }
    if (t->have_cur) {
        t->have_cur = false;
        memcpy(stereo, t->cur, 640);
        return 1;
    }
    unsigned base[2] = {0x10000 + l->mix, 0x30000 + l->mix};
    uint32_t fp[2] = {block_fingerprint(dram, base[0]), block_fingerprint(dram, base[1])};
    if (!t->primed) {
        t->fp[0] = t->seen[0] = fp[0];
        t->fp[1] = t->seen[1] = fp[1];
        t->primed = true;
        t->last_bank = -1;
        t->due_us = now_us;
        return 0;
    }
    bool stable[2];
    for (unsigned bank = 0; bank < 2; bank++) {
        bool moved = fp[bank] != t->seen[bank];
        if (moved) {
            t->seen[bank] = fp[bank];
            t->changes++;
            t->stale++;
        }
        stable[bank] = !moved;
    }
    int order[2] = {0, 1};
    if (t->last_bank == 0) {
        order[0] = 1;
        order[1] = 0;
    } // alternate first, but either may be rewritten again
    for (unsigned k = 0; k < 2; k++) {
        int bank = order[k];
        if (!stable[bank] || fp[bank] == t->fp[bank]) {
            continue;
        }
        int16_t again[320];
        copy_block(dram, base[bank], t->cur);
        copy_block(dram, base[bank], again);
        if (memcmp(t->cur, again, sizeof(again)) != 0 || block_fingerprint(dram, base[bank]) != fp[bank]) {
            t->mid_write++;
            continue; // rewritten while reading: retry next poll
        }
        t->fp[bank] = fp[bank];
        t->last_bank = bank;
        uint64_t behind = now_us > t->due_us ? now_us - t->due_us : 0;
        unsigned silence = behind >= 2 * frame ? (behind / frame > 5 ? 5 : (unsigned)(behind / frame)) : 0;
        t->pending_silence = silence;
        t->missed += silence;
        t->due_us += (silence + 1) * frame;
        if (t->due_us > now_us + 3 * frame || t->due_us + 8 * frame < now_us) {
            t->due_us = now_us + frame; // resync
        }
        t->have_cur = true;
        t->real++;
        return native_dsp_poll(dram, size, l, t, now_ms, stereo);
    }
    // No new data: digital silence or a paused game. Keep the timeline moving.
    if (now_us >= t->due_us + 2 * frame) {
        t->due_us += frame;
        t->missed++;
        memset(stereo, 0, 640);
        return 2;
    }
    return 0;
}
