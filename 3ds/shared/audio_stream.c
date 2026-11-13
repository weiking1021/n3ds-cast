#include "audio_stream.h"
#include <string.h>

static const int step_table[89] = {
    7,    8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,   28,
    31,   34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,  118,
    130,  143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,  494,
    544,  598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878, 2066,
    2272, 2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int index_delta[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
// Quiet 440 Hz sine, 1/8 full-scale; no floating point in the sender loop.
static const int16_t sine[64] = {
    0,     401,   799,   1189,  1567,  1931,  2276,  2598,  2896,  3166,  3406,  3612,  3784,  3920,  4017,  4076,
    4096,  4076,  4017,  3920,  3784,  3612,  3406,  3166,  2896,  2598,  2276,  1931,  1567,  1189,  799,   401,
    0,     -401,  -799,  -1189, -1567, -1931, -2276, -2598, -2896, -3166, -3406, -3612, -3784, -3920, -4017, -4076,
    -4096, -4076, -4017, -3920, -3784, -3612, -3406, -3166, -2896, -2598, -2276, -1931, -1567, -1189, -799,  -401};

size_t audio_encode_ima_mono(const int16_t *pcm, size_t samples, uint8_t *out, size_t capacity) {
    if (!pcm || !out || !samples || samples > AUDIO_MAX_SAMPLES) {
        return 0;
    }
    size_t bytes = 4 + samples / 2;
    if (capacity < bytes) {
        return 0;
    }
    int predictor = pcm[0];
    int index = 40;
    out[0] = (uint8_t)predictor;
    out[1] = (uint8_t)((uint16_t)predictor >> 8);
    out[2] = (uint8_t)index;
    out[3] = 0;
    memset(out + 4, 0, bytes - 4);
    for (size_t i = 1; i < samples; i++) {
        int difference = (int)pcm[i] - predictor;
        int code = difference < 0 ? 8 : 0;
        if (difference < 0) {
            difference = -difference;
        }
        int step = step_table[index];
        int delta = step >> 3;
        if (difference >= step) {
            code |= 4;
            difference -= step;
            delta += step;
        }
        if (difference >= (step >> 1)) {
            code |= 2;
            difference -= step >> 1;
            delta += step >> 1;
        }
        if (difference >= (step >> 2)) {
            code |= 1;
            delta += step >> 2;
        }
        predictor += code & 8 ? -delta : delta;
        if (predictor > 32767) {
            predictor = 32767;
        }
        if (predictor < -32768) {
            predictor = -32768;
        }
        index += index_delta[code & 7];
        if (index < 0) {
            index = 0;
        }
        if (index > 88) {
            index = 88;
        }
        out[4 + (i - 1) / 2] |= (uint8_t)(code << (((i - 1) & 1) * 4));
    }
    return bytes;
}

void audio_stream_reset(AudioStream *stream) {
    memset(stream, 0, sizeof(*stream));
}

bool audio_stream_next(AudioStream *stream, uint64_t now_ms, int kbps, AudioPacket *packet) {
    if (!stream || !packet || (kbps != 32 && kbps != 64 && kbps != 128)) {
        return false;
    }
    int rate = kbps * 250; // 4 bits/sample, mono
    if (stream->sample_rate != rate) {
        audio_stream_reset(stream);
        stream->sample_rate = rate;
        stream->next_ms = now_ms;
    }
    if (now_ms < stream->next_ms) {
        return false;
    }
    int samples = rate / 10;
    uint32_t phase_step = (uint32_t)((UINT64_C(440) << 32) / (unsigned)rate);
    uint64_t skipped = (now_ms - stream->next_ms) / AUDIO_PACKET_MS;
    // Never burst old audio after a slow video frame or a blocking keyboard.
    stream->phase += phase_step * (uint32_t)samples * (uint32_t)skipped;
    stream->sequence += (uint32_t)skipped;
    stream->next_ms += (skipped + 1) * AUDIO_PACKET_MS;
    for (int i = 0; i < samples; i++) {
        stream->pcm[i] = sine[stream->phase >> 26];
        stream->phase += phase_step;
    }
    packet->sequence = stream->sequence++;
    packet->sample_rate = rate;
    packet->samples = samples;
    packet->size = audio_encode_ima_mono(stream->pcm, samples, packet->data, sizeof(packet->data));
    return packet->size != 0;
}
