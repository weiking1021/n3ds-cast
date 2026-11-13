#ifndef N3DS_AUDIO_STREAM_H
#define N3DS_AUDIO_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIO_PACKET_MS 100
#define AUDIO_MAX_SAMPLES 3200
#define AUDIO_MAX_BYTES (4 + AUDIO_MAX_SAMPLES / 2)

typedef struct {
    uint64_t next_ms;
    uint32_t phase;
    uint32_t sequence;
    int sample_rate;
    int16_t pcm[AUDIO_MAX_SAMPLES];
} AudioStream;

typedef struct {
    uint32_t sequence;
    int sample_rate;
    int samples;
    size_t size;
    uint8_t data[AUDIO_MAX_BYTES];
} AudioPacket;

// Independent blocks: LE initial PCM16, step index, reserved zero, then
// IMA ADPCM nibbles (low first). The first PCM sample is in the header.
size_t audio_encode_ima_mono(const int16_t *pcm, size_t samples, uint8_t *out, size_t capacity);
void audio_stream_reset(AudioStream *stream);
// Generate at most one 100 ms test-tone block; skip stale blocks after stalls.
bool audio_stream_next(AudioStream *stream, uint64_t now_ms, int kbps, AudioPacket *packet);

#endif
