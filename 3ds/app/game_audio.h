#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "audio_stream.h"
#define NATIVE_DSP_BYTES 0x40000u
#define NATIVE_AUDIO_MAX_GAP 6u // up to 5 missed 160-sample DSP frames (~24 ms) are filled with silence
typedef struct {
    unsigned table, counter, mix;
    uint16_t words[15];
    bool valid;
} NativeDspLayout;
typedef struct {
    uint16_t counter;
    bool primed;
    unsigned rate, phase, count, average_count;
    int average_sum;
    uint32_t sequence, dropped;
    int16_t pcm[AUDIO_MAX_SAMPLES];
} NativeGameAudio;
// The metadata is read from an existing audio pipe reply in DSP DRAM.
// Unknown/ambiguous layouts are refused; no fixed firmware sample offsets.
bool native_dsp_layout(const volatile uint8_t *dram, size_t size, NativeDspLayout *layout);
// 160 stereo PCM16 frames, verified against both counters and metadata.
int native_dsp_read(const volatile uint8_t *dram, size_t size, const NativeDspLayout *layout, uint16_t *counter,
                    int16_t stereo[320]);
void native_game_audio_reset(NativeGameAudio *state, unsigned rate);
// One packet at most per DSP tick. A discontinuity discards partial audio.
bool native_game_audio_push(NativeGameAudio *state, uint16_t counter, const int16_t stereo[320], AudioPacket *packet);
// Read-only DSP DRAM survey for the TV debug panel (never writes, never consumes).
typedef struct {
    uint16_t counter[4];      // u16 at 0x7FFE of each 64 KiB bank
    uint16_t nonzero[4];      // sampled non-zero words per bank (0..4096)
    unsigned headers, tables; // pipe "15" headers in bank 0 / of those, structurally valid
} NativeDspDebug;
void native_dsp_debug(const volatile uint8_t *dram, size_t size, NativeDspDebug *out);
// Content-driven DSP block tracker (see native_dsp_poll in game_audio.c).
#define NATIVE_DSP_FRAME_US 4889u // 160 samples at 32728 Hz
typedef struct {
    bool primed, have_cur;
    int last_bank;
    uint32_t fp[2], seen[2]; // fp: last block emitted per buffer; seen: previous poll
    uint64_t due_us;         // when the next block is due on the audio timeline
    unsigned pending_silence;
    int16_t cur[320];
    // Diagnostics shown on the TV panel. ticks/resync are unused (kept for the display).
    unsigned ticks, real, missed, stale, mid_write, resync, changes;
} NativeDspTracker;
// -1 layout unusable, 0 nothing yet, 1 `stereo` holds a real DSP block,
// 2 `stereo` is a silent block (real silence or a block this thread missed). Call until 0.
int native_dsp_poll(const volatile uint8_t *dram, size_t size, const NativeDspLayout *layout, NativeDspTracker *tracker,
                    uint64_t now_ms, int16_t stereo[320]);
