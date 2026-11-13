#include "game_audio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void word(uint8_t *p, unsigned at, unsigned value) {
    p[at] = value;
    p[at + 1] = value >> 8;
}
int main(void) {
    uint8_t *dram = calloc(1, NATIVE_DSP_BYTES), *before = malloc(NATIVE_DSP_BYTES);
    unsigned table[] = {0xBFFF, 0x9E92, 0x8680, 0xA792, 0x9430, 0x8400, 0x8540, 0x9492,
                        0x8710, 0x8410, 0xA912, 0xAA12, 0xAAD2, 0xAC52, 0xAC5C};
    word(dram, 64, 15);
    for (unsigned i = 0; i < 15; i++) {
        word(dram, 66 + i * 2, table[i]);
    }
    NativeDspLayout layout = {0};
    assert(native_dsp_layout(dram, NATIVE_DSP_BYTES, &layout));
    assert(layout.mix == 0xA80 && layout.counter == 0x7FFE);
    word(dram, 0x17FFE, 4);
    word(dram, 0x37FFE, 5);
    for (unsigned i = 0; i < 320; i++) {
        word(dram, 0x30000 + layout.mix + i * 2, (unsigned)(int16_t)(i * 4 - 640));
    }
    memcpy(before, dram, NATIVE_DSP_BYTES);
    int16_t stereo[320];
    uint16_t counter;
    assert(native_dsp_read(dram, NATIVE_DSP_BYTES, &layout, &counter, stereo) == 1 && counter == 5);
    assert(stereo[0] == -640 && stereo[319] == 636 && !memcmp(before, dram, NATIVE_DSP_BYTES));
    word(dram, 0x17FFE, 0xFFFF);
    word(dram, 0x37FFE, 2);
    assert(native_dsp_read(dram, NATIVE_DSP_BYTES, &layout, &counter, stereo) == 1 && counter == 2);
    word(dram, 66 + 12, 0xC000);
    assert(native_dsp_read(dram, NATIVE_DSP_BYTES, &layout, &counter, stereo) == -1);
    assert(!native_dsp_layout(dram, NATIVE_DSP_BYTES, &layout));
    word(dram, 66 + 12, table[6]);
    // A second valid but conflicting table is refused rather than guessed.
    word(dram, 128, 15);
    for (unsigned i = 0; i < 15; i++) {
        word(dram, 130 + i * 2, table[i]);
    }
    word(dram, 130 + 12, 0x8100);
    assert(!native_dsp_layout(dram, NATIVE_DSP_BYTES, &layout));
    // Fallback mode when pipe FIFO init message is no longer in Bank 0
    memset(dram, 0, 0x10000);
    word(dram, 0x17FFE, 10);
    word(dram, 0x37FFE, 11);
    for (unsigned i = 0; i < 320; i++) {
        word(dram, 0x30000 + 0x0A80 + i * 2, 500);
    }
    NativeDspLayout fb_layout = {0};
    assert(native_dsp_layout(dram, NATIVE_DSP_BYTES, &fb_layout));
    assert(fb_layout.counter == 0x7FFE && fb_layout.mix == 0x0A80);
    assert(native_dsp_read(dram, NATIVE_DSP_BYTES, &fb_layout, &counter, stereo) == 1 && counter == 11);
    for (unsigned rate = 8000; rate <= 32000; rate *= 2) {
        NativeGameAudio audio;
        native_game_audio_reset(&audio, rate);
        AudioPacket packet;
        unsigned packets = 0;
        for (unsigned i = 0; i < 320; i++) {
            stereo[i] = 1000;
        }
        for (unsigned tick = 1; tick <= 206; tick++) {
            if (native_game_audio_push(&audio, tick, stereo, &packet)) {
                packets++;
                assert(packet.sample_rate == (int)rate && packet.samples == (int)rate / 10);
                assert(packet.size == 4 + rate / 20 && packet.data[0] == 0xE8 && packet.data[1] == 3);
            }
        }
        assert(packets == 10);
        unsigned count = audio.count;
        assert(!native_game_audio_push(&audio, 206, stereo, &packet) && audio.count == count);
        assert(!native_game_audio_push(&audio, 220, stereo, &packet) && !audio.count && audio.dropped == 1);
        { // a few DSP frames missed by a starved thread: filled with silence, not discarded
            NativeGameAudio gap;
            native_game_audio_reset(&gap, rate);
            for (unsigned tick = 1; tick <= 3; tick++) {
                native_game_audio_push(&gap, tick, stereo, &packet);
            }
            unsigned before_count = gap.count;
            native_game_audio_push(&gap, 6, stereo, &packet);
            assert(gap.dropped == 2 && gap.counter == 6 && gap.primed && gap.count > before_count);
        }
        audio.counter = 0xFFFF;
        assert(!native_game_audio_push(&audio, 2, stereo, &packet) && audio.count > 0);
    }
    {
        NativeDspDebug dbg;
        memset(dram, 0, NATIVE_DSP_BYTES);
        word(dram, 0x10000 + 0x7FFE, 7);
        word(dram, 0x30000 + 16, 1);
        native_dsp_debug(dram, NATIVE_DSP_BYTES, &dbg);
        assert(dbg.counter[1] == 7 && dbg.counter[0] == 0 && dbg.nonzero[3] == 1 && dbg.headers == 0);
    }
    { // content-driven tracker: the buffer content is the timeline, the game counters are not
        NativeDspLayout tl = {.table = 0, .counter = 0x7FFE, .mix = 0xA80, .valid = true};
        NativeDspTracker tr;
        memset(&tr, 0, sizeof(tr));
        memset(dram, 0, NATIVE_DSP_BYTES);
        int16_t out[320];
        uint64_t t = 1000;
#define DSP_BLOCK(bank, seed)                                                                                          \
    do {                                                                                                               \
        unsigned b_ = (bank) ? 0x30000 : 0x10000;                                                                      \
        for (unsigned i_ = 0; i_ < 320; i_++)                                                                          \
            word(dram, b_ + 0xA80 + i_ * 2, (unsigned)(int16_t)((seed) + i_));                                         \
    } while (0)
#define POLL() native_dsp_poll(dram, NATIVE_DSP_BYTES, &tl, &tr, t, out)
        assert(POLL() == 0 && tr.primed);
        DSP_BLOCK(0, 1000);
        t += 1;
        assert(POLL() == 0); // changed this poll: wait for the DSP to finish
        t += 1;
        assert(POLL() == 1 && out[0] == 1000 && out[319] == 1319 && tr.last_bank == 0);
        t += 1;
        assert(POLL() == 0); // nothing new
        DSP_BLOCK(0, 2000);
        t += 1; // the SAME buffer rewritten on the next tick (game counters lag)
        assert(POLL() == 0);
        t += 1;
        assert(POLL() == 1 && out[0] == 2000 && tr.last_bank == 0);
        DSP_BLOCK(1, 3000);
        DSP_BLOCK(0, 4000);
        t += 1; // both rewritten between polls: alternate buffer first
        assert(POLL() == 0);
        t += 1;
        assert(POLL() == 1 && out[0] == 3000 && tr.last_bank == 1);
        assert(POLL() == 1 && out[0] == 4000 && tr.last_bank == 0);
        assert(POLL() == 0);
        DSP_BLOCK(1, 5000);
        t += 1;
        assert(POLL() == 0); // DSP still writing: content moves between polls
        word(dram, 0x30A80, (unsigned)(int16_t)5100);
        t += 1;
        assert(POLL() == 0);
        t += 1;
        assert(POLL() == 1 && out[0] == 5100);
        t += 30;
        {
            int rc = POLL();
            assert(rc == 2 && out[0] == 0 && out[319] == 0);
        } // real silence: the clock paces zeros
        for (unsigned guard = 0; guard < 10 && POLL() == 2; guard++)
            ;
        DSP_BLOCK(0, 6000);
        t += 1;
        assert(POLL() == 0); // a block arrives long after the last one: gap filled
        t += 20;
        {
            unsigned silent = 0;
            int rc;
            while ((rc = POLL()) == 2) {
                assert(out[0] == 0);
                silent++;
            }
            assert(rc == 1 && out[0] == 6000 && silent == 5);
        }
        assert(tr.real == 6 && tr.changes >= 6 && tr.mid_write == 0);
        tl.mix = 0x8000;
        assert(POLL() == -1);
#undef POLL
#undef DSP_BLOCK
    }
    free(dram);
    free(before);
    puts(
        "PASS: dynamic DSP metadata validation, conflicting layout refusal, read-only PCM snapshots, counter wrap, 32/64/128 kbps resampling, duplicates and discontinuities");
}
