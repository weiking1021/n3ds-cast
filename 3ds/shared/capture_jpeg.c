#include "capture_jpeg.h"
#include <string.h>
#include <stdint.h>
#ifdef __3DS__
#include <3ds.h>
#define JPEG_TICK() svcGetSystemTick()
#else
#define JPEG_TICK() 0ull
#endif

// Where the time of the 4:2:0 encoder goes (see capture_jpeg.h): only every 16th MCU is timed.
volatile uint64_t capture_jpeg_ticks[5];
static uint64_t *dct_acc; // when set, encode_block adds the time of its DCT here

void capture_convert_3ds_bgr8_to_rgb24(const uint8_t *src_bgr8, uint8_t *dst_rgb24, int width, int height) {
    for (int y = 0; y < height; y++) {
        uint8_t *dst_row = dst_rgb24 + (y * width * 3);
        int src_y = (height - 1) - y;
        for (int x = 0; x < width; x++) {
            const uint8_t *src_px = src_bgr8 + ((x * height + src_y) * 3);
            dst_row[x * 3 + 0] = src_px[2]; // R
            dst_row[x * 3 + 1] = src_px[1]; // G
            dst_row[x * 3 + 2] = src_px[0]; // B
        }
    }
}

void capture_convert_3ds_rgb565_to_rgb24(const uint8_t *src_rgb565, uint8_t *dst_rgb24, int width, int height) {
    const uint16_t *src16 = (const uint16_t *)src_rgb565;
    for (int y = 0; y < height; y++) {
        uint8_t *dst_row = dst_rgb24 + (y * width * 3);
        int src_y = (height - 1) - y;
        for (int x = 0; x < width; x++) {
            uint16_t px = src16[x * height + src_y];
            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >> 5) & 0x3F;
            uint8_t b5 = px & 0x1F;
            dst_row[x * 3 + 0] = (r5 << 3) | (r5 >> 2);
            dst_row[x * 3 + 1] = (g6 << 2) | (g6 >> 4);
            dst_row[x * 3 + 2] = (b5 << 3) | (b5 >> 2);
        }
    }
}

/* =========================================================================
 * Bit-Exact IJG (jfdctint) Integer FDCT & Division-Free Baseline JPEG Encoder
 * Optimized for ARMv6K / ARM11 MPCore (zero inner-loop software divisions)
 * ========================================================================= */

static const uint8_t std_lum_quant[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                          14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                          18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                          49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};

static const uint8_t std_chrom_quant[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                            24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                            99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                            99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

static const uint8_t zigzag_index[64] = {0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
                                         3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
                                         10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
                                         21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};

static const uint8_t dc_lum_bits[17] = {0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t dc_lum_val[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t dc_chr_bits[17] = {0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t dc_chr_val[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

static const uint8_t ac_lum_bits[17] = {0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t ac_lum_val[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

static const uint8_t ac_chr_bits[17] = {0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t ac_chr_val[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

typedef struct {
    uint16_t code[256];
    uint8_t size[256];
} HuffTable;

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    uint32_t bit_buf;
    int bit_count;
} JpegBitWriter;

static void build_huff_table(const uint8_t *bits, const uint8_t *vals, HuffTable *ht) {
    memset(ht, 0, sizeof(HuffTable));
    uint16_t code = 0;
    int k = 0;
    for (int i = 1; i <= 16; i++) {
        for (int j = 0; j < bits[i]; j++) {
            uint8_t sym = vals[k++];
            ht->code[sym] = code++;
            ht->size[sym] = (uint8_t)i;
        }
        code <<= 1;
    }
}

static inline void bw_put_byte(JpegBitWriter *bw, uint8_t b) {
    if (bw->pos < bw->cap) {
        bw->buf[bw->pos] = b;
    }
    bw->pos++;
}

static inline void bw_put_bits(JpegBitWriter *bw, uint16_t bits, int len) {
    bw->bit_buf = (bw->bit_buf << len) | (bits & ((1u << len) - 1));
    bw->bit_count += len;
    if (bw->pos + 6 <= bw->cap) {
        // Fast path: at most 2 whole bytes (4 with 0xFF stuffing) are produced, all in bounds.
        uint8_t *p = bw->buf + bw->pos;
        while (bw->bit_count >= 8) {
            bw->bit_count -= 8;
            uint8_t b = (uint8_t)(bw->bit_buf >> bw->bit_count);
            *p++ = b;
            if (b == 0xFF) {
                *p++ = 0x00;
            }
        }
        bw->pos = (size_t)(p - bw->buf);
        return;
    }
    while (bw->bit_count >= 8) {
        bw->bit_count -= 8;
        uint8_t b = (uint8_t)((bw->bit_buf >> bw->bit_count) & 0xFF);
        bw_put_byte(bw, b);
        if (b == 0xFF) {
            bw_put_byte(bw, 0x00);
        }
    }
}

/* Exact IJG (libjpeg jfdctint.c) 8x8 Forward DCT */
static void fdct_int8x8(int32_t *block) {
    /* Pass 1: process rows (output scaled up by 2^2 = 4) */
    for (int i = 0; i < 8; i++) {
        int32_t *row = block + (i << 3);
        int32_t tmp0 = row[0] + row[7];
        int32_t tmp7 = row[0] - row[7];
        int32_t tmp1 = row[1] + row[6];
        int32_t tmp6 = row[1] - row[6];
        int32_t tmp2 = row[2] + row[5];
        int32_t tmp5 = row[2] - row[5];
        int32_t tmp3 = row[3] + row[4];
        int32_t tmp4 = row[3] - row[4];

        int32_t tmp10 = tmp0 + tmp3;
        int32_t tmp13 = tmp0 - tmp3;
        int32_t tmp11 = tmp1 + tmp2;
        int32_t tmp12 = tmp1 - tmp2;

        row[0] = (tmp10 + tmp11) * 4;
        row[4] = (tmp10 - tmp11) * 4;

        int32_t z1 = (tmp12 + tmp13) * 4433;
        row[2] = (z1 + tmp13 * 6270 + 1024) >> 11;
        row[6] = (z1 - tmp12 * 15137 + 1024) >> 11;

        z1 = tmp4 + tmp7;
        int32_t z2 = tmp5 + tmp6;
        int32_t z3 = tmp4 + tmp6;
        int32_t z4 = tmp5 + tmp7;
        int32_t z5 = (z3 + z4) * 9633;

        tmp4 = tmp4 * 2446;
        tmp5 = tmp5 * 16819;
        tmp6 = tmp6 * 25172;
        tmp7 = tmp7 * 12299;
        z1 = -z1 * 7373;
        z2 = -z2 * 20995;
        z3 = -z3 * 16069 + z5;
        z4 = -z4 * 3196 + z5;

        row[7] = (tmp4 + z1 + z3 + 1024) >> 11;
        row[5] = (tmp5 + z2 + z4 + 1024) >> 11;
        row[3] = (tmp6 + z2 + z3 + 1024) >> 11;
        row[1] = (tmp7 + z1 + z4 + 1024) >> 11;
    }

    /* Pass 2: process columns (descale by 2^5 = 32 to true DCT-II scale) */
    for (int i = 0; i < 8; i++) {
        int32_t *col = block + i;
        int32_t tmp0 = col[0 * 8] + col[7 * 8];
        int32_t tmp7 = col[0 * 8] - col[7 * 8];
        int32_t tmp1 = col[1 * 8] + col[6 * 8];
        int32_t tmp6 = col[1 * 8] - col[6 * 8];
        int32_t tmp2 = col[2 * 8] + col[5 * 8];
        int32_t tmp5 = col[2 * 8] - col[5 * 8];
        int32_t tmp3 = col[3 * 8] + col[4 * 8];
        int32_t tmp4 = col[3 * 8] - col[4 * 8];

        int32_t tmp10 = tmp0 + tmp3;
        int32_t tmp13 = tmp0 - tmp3;
        int32_t tmp11 = tmp1 + tmp2;
        int32_t tmp12 = tmp1 - tmp2;

        col[0 * 8] = (tmp10 + tmp11 + 16) >> 5;
        col[4 * 8] = (tmp10 - tmp11 + 16) >> 5;

        int32_t z1 = (tmp12 + tmp13) * 4433;
        col[2 * 8] = (z1 + tmp13 * 6270 + 131072) >> 18;
        col[6 * 8] = (z1 - tmp12 * 15137 + 131072) >> 18;

        z1 = tmp4 + tmp7;
        int32_t z2 = tmp5 + tmp6;
        int32_t z3 = tmp4 + tmp6;
        int32_t z4 = tmp5 + tmp7;
        int32_t z5 = (z3 + z4) * 9633;

        tmp4 = tmp4 * 2446;
        tmp5 = tmp5 * 16819;
        tmp6 = tmp6 * 25172;
        tmp7 = tmp7 * 12299;
        z1 = -z1 * 7373;
        z2 = -z2 * 20995;
        z3 = -z3 * 16069 + z5;
        z4 = -z4 * 3196 + z5;

        col[7 * 8] = (tmp4 + z1 + z3 + 131072) >> 18;
        col[5 * 8] = (tmp5 + z2 + z4 + 131072) >> 18;
        col[3 * 8] = (tmp6 + z2 + z3 + 131072) >> 18;
        col[1 * 8] = (tmp7 + z1 + z4 + 131072) >> 18;
    }
}

/* Single-cycle ARMv6K CLZ bit-length calculation */
static inline int bit_magnitude(int val, uint16_t *out_bits) {
    if (val == 0) {
        *out_bits = 0;
        return 0;
    }
    uint32_t abs_v = (uint32_t)(val < 0 ? -val : val);
    int len = 32 - __builtin_clz(abs_v);
    *out_bits = val > 0 ? (uint16_t)val : (uint16_t)(val + (1 << len) - 1);
    return len;
}

/* Zigzag order -> natural index (inverse of zigzag_index). */
static const uint8_t unzig_index[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* Bit-exact with the previous implementation, but:
 *  - a constant 8x8 block (flat sky, UI panels) skips the DCT: DC = 8*sample, all AC = 0;
 *  - quantisation is fused with the zigzag scan (no 64-entry temp array, no sign branches);
 *  - the bit writer has a bounds-check-free fast path. */
static void encode_block(JpegBitWriter *bw, int32_t *block, const uint32_t *inv_qtable, int32_t *prev_dc,
                         const HuffTable *dc_ht, const HuffTable *ac_ht) {
    bool flat = true;
    int32_t first = block[0];
    for (int i = 1; i < 64; i++) {
        if (block[i] != first) {
            flat = false;
            break;
        }
    }
    int32_t dc_coef;
    if (flat) {
        dc_coef = first * 8; /* exact result of fdct_int8x8 for a constant block */
    } else {
        if (dct_acc) {
            uint64_t started = JPEG_TICK();
            fdct_int8x8(block);
            *dct_acc += JPEG_TICK() - started;
        } else {
            fdct_int8x8(block);
        }
        dc_coef = block[0];
    }

    // DC coefficient
    int32_t dsgn = dc_coef >> 31;
    uint32_t dq = (((uint32_t)((dc_coef ^ dsgn) - dsgn)) * inv_qtable[0] + 32768u) >> 16;
    int32_t dc_q = (int32_t)((dq ^ (uint32_t)dsgn) - (uint32_t)dsgn);
    int32_t diff = dc_q - *prev_dc;
    *prev_dc = dc_q;
    uint16_t mag_bits = 0;
    int mag_len = bit_magnitude(diff, &mag_bits);
    bw_put_bits(bw, dc_ht->code[mag_len], dc_ht->size[mag_len]);
    if (mag_len > 0) {
        bw_put_bits(bw, mag_bits, mag_len);
    }

    if (flat) {
        bw_put_bits(bw, ac_ht->code[0x00], ac_ht->size[0x00]); // EOB (63 zero coefficients)
        return;
    }

    // AC coefficients, quantised in zigzag order
    int zero_run = 0;
    for (int k = 1; k < 64; k++) {
        int idx = unzig_index[k];
        int32_t v = block[idx];
        int32_t sgn = v >> 31;
        uint32_t magnitude = (uint32_t)((v ^ sgn) - sgn);
        // quantised to zero (no multiplication needed to tell): magnitude * inv + 32768 < 65536
        if (magnitude <= inv_qtable[64 + idx]) {
            zero_run++;
            continue;
        }
        uint32_t q = (magnitude * inv_qtable[idx] + 32768u) >> 16;
        while (zero_run >= 16) {
            bw_put_bits(bw, ac_ht->code[0xF0], ac_ht->size[0xF0]);
            zero_run -= 16;
        }
        int32_t ac = (int32_t)((q ^ (uint32_t)sgn) - (uint32_t)sgn);
        mag_len = bit_magnitude(ac, &mag_bits);
        uint8_t sym = (uint8_t)((zero_run << 4) | mag_len);
        bw_put_bits(bw, ac_ht->code[sym], ac_ht->size[sym]);
        bw_put_bits(bw, mag_bits, mag_len);
        zero_run = 0;
    }
    if (zero_run > 0) {
        bw_put_bits(bw, ac_ht->code[0x00], ac_ht->size[0x00]); // EOB
    }
}

typedef struct {
    uint8_t id;
    const uint8_t *bits;
    const uint8_t *vals;
    int vlen;
} DhtSpec;

static void write_jpeg_headers_ex(JpegBitWriter *bw, int width, int height, const uint8_t *q_lum, const uint8_t *q_chr,
                                  bool is_420, const DhtSpec *htables) {
    // SOI
    bw_put_byte(bw, 0xFF);
    bw_put_byte(bw, 0xD8);

    // APP0 (JFIF)
    const uint8_t app0[] = {0xFF, 0xE0, 0x00, 0x10, 'J',  'F',  'I',  'F',  0x00,
                            0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
    for (size_t i = 0; i < sizeof(app0); i++) {
        bw_put_byte(bw, app0[i]);
    }

    // DQT (Luminance & Chrominance in ZigZag order!)
    bw_put_byte(bw, 0xFF);
    bw_put_byte(bw, 0xDB);
    bw_put_byte(bw, 0x00);
    bw_put_byte(bw, 132);
    bw_put_byte(bw, 0x00);
    for (int i = 0; i < 64; i++) {
        bw_put_byte(bw, q_lum[i]);
    }
    bw_put_byte(bw, 0x01);
    for (int i = 0; i < 64; i++) {
        bw_put_byte(bw, q_chr[i]);
    }

    // SOF0 (Baseline DCT)
    bw_put_byte(bw, 0xFF);
    bw_put_byte(bw, 0xC0);
    bw_put_byte(bw, 0x00);
    bw_put_byte(bw, 17);
    bw_put_byte(bw, 8);
    bw_put_byte(bw, (uint8_t)((height >> 8) & 0xFF));
    bw_put_byte(bw, (uint8_t)(height & 0xFF));
    bw_put_byte(bw, (uint8_t)((width >> 8) & 0xFF));
    bw_put_byte(bw, (uint8_t)(width & 0xFF));
    bw_put_byte(bw, 3);
    bw_put_byte(bw, 1);
    bw_put_byte(bw, is_420 ? 0x22 : 0x11);
    bw_put_byte(bw, 0);
    bw_put_byte(bw, 2);
    bw_put_byte(bw, 0x11);
    bw_put_byte(bw, 1);
    bw_put_byte(bw, 3);
    bw_put_byte(bw, 0x11);
    bw_put_byte(bw, 1);

    // DHT
    for (int t = 0; t < 4; t++) {
        int len = 2 + 1 + 16 + htables[t].vlen;
        bw_put_byte(bw, 0xFF);
        bw_put_byte(bw, 0xC4);
        bw_put_byte(bw, (uint8_t)((len >> 8) & 0xFF));
        bw_put_byte(bw, (uint8_t)(len & 0xFF));
        bw_put_byte(bw, htables[t].id);
        for (int i = 1; i <= 16; i++) {
            bw_put_byte(bw, htables[t].bits[i]);
        }
        for (int i = 0; i < htables[t].vlen; i++) {
            bw_put_byte(bw, htables[t].vals[i]);
        }
    }

    // SOS
    const uint8_t sos[] = {0xFF, 0xDA, 0x00, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0x00, 0x3F, 0x00};
    for (size_t i = 0; i < sizeof(sos); i++) {
        bw_put_byte(bw, sos[i]);
    }
}

static void write_jpeg_headers(JpegBitWriter *bw, int width, int height, const uint8_t *q_lum, const uint8_t *q_chr,
                               bool is_420) {
    static const DhtSpec standard[4] = {{0x00, dc_lum_bits, dc_lum_val, 12},
                                        {0x10, ac_lum_bits, ac_lum_val, 162},
                                        {0x01, dc_chr_bits, dc_chr_val, 12},
                                        {0x11, ac_chr_bits, ac_chr_val, 162}};
    write_jpeg_headers_ex(bw, width, height, q_lum, q_chr, is_420, standard);
}

static void init_quant_tables(int quality, uint8_t *dqt_lum, uint8_t *dqt_chr, uint32_t *inv_lum, uint32_t *inv_chr) {
    if (quality < 10) {
        quality = 10;
    }
    if (quality > 100) {
        quality = 100;
    }
    int scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);

    for (int i = 0; i < 64; i++) {
        int l = (std_lum_quant[i] * scale + 50) / 100;
        int c = (std_chrom_quant[i] * scale + 50) / 100;
        if (l < 1) {
            l = 1;
        } else if (l > 255) {
            l = 255;
        }
        if (c < 1) {
            c = 1;
        } else if (c > 255) {
            c = 255;
        }

        // DQT marker requires quantization values in ZigZag order!
        dqt_lum[zigzag_index[i]] = (uint8_t)l;
        dqt_chr[zigzag_index[i]] = (uint8_t)c;

        // Precompute 16-bit reciprocal for division-free quantization in natural order
        inv_lum[i] = (uint32_t)((65536 + (l >> 1)) / l);
        inv_chr[i] = (uint32_t)((65536 + (c >> 1)) / c);
        // the largest magnitude that still quantises to zero: m * inv + 32768 < 65536  <=>  m <= 32767 / inv
        inv_lum[64 + i] = 32767u / inv_lum[i];
        inv_chr[64 + i] = 32767u / inv_chr[i];
    }
}

/* Gathers one 16x16 MCU from the column-major BGR8 framebuffer into four luma blocks and two 2x2 averaged
 * chroma blocks (all level shifted exactly as before). Shared by the standard and the optimised encoder. */
static inline void gather_mcu420(const uint8_t *src_bgr8, int height, int mcu_x, int mcu_y, int32_t *blk_y00,
                                 int32_t *blk_y01, int32_t *blk_y10, int32_t *blk_y11, int32_t *blk_cb,
                                 int32_t *blk_cr) {
    for (int sy = 0; sy < 16; sy += 2) {
        int src_y0 = (height - 1) - (mcu_y + sy);
        int src_y1 = src_y0 - 1;
        int32_t *y_row0_left = (sy < 8) ? (blk_y00 + sy * 8) : (blk_y10 + (sy - 8) * 8);
        int32_t *y_row1_left = (sy < 8) ? (blk_y00 + (sy + 1) * 8) : (blk_y10 + (sy - 7) * 8);
        int32_t *y_row0_right = (sy < 8) ? (blk_y01 + sy * 8) : (blk_y11 + (sy - 8) * 8);
        int32_t *y_row1_right = (sy < 8) ? (blk_y01 + (sy + 1) * 8) : (blk_y11 + (sy - 7) * 8);
        int c_row_offset = (sy >> 1) * 8;

        for (int sx = 0; sx < 16; sx += 2) {
            const uint8_t *col0 = src_bgr8 + ((mcu_x + sx) * height) * 3;
            const uint8_t *col1 = col0 + height * 3;

            const uint8_t *p00 = col0 + src_y0 * 3;
            const uint8_t *p10 = col0 + src_y1 * 3;
            const uint8_t *p01 = col1 + src_y0 * 3;
            const uint8_t *p11 = col1 + src_y1 * 3;

            int b00 = p00[0], g00 = p00[1], r00 = p00[2];
            int b01 = p01[0], g01 = p01[1], r01 = p01[2];
            int b10 = p10[0], g10 = p10[1], r10 = p10[2];
            int b11 = p11[0], g11 = p11[1], r11 = p11[2];

            int y00 = ((77 * r00 + 150 * g00 + 29 * b00) >> 8) - 128;
            int y01 = ((77 * r01 + 150 * g01 + 29 * b01) >> 8) - 128;
            int y10 = ((77 * r10 + 150 * g10 + 29 * b10) >> 8) - 128;
            int y11 = ((77 * r11 + 150 * g11 + 29 * b11) >> 8) - 128;

            if (sx < 8) {
                y_row0_left[sx] = y00;
                y_row0_left[sx + 1] = y01;
                y_row1_left[sx] = y10;
                y_row1_left[sx + 1] = y11;
            } else {
                int rx = sx - 8;
                y_row0_right[rx] = y00;
                y_row0_right[rx + 1] = y01;
                y_row1_right[rx] = y10;
                y_row1_right[rx + 1] = y11;
            }

            // 2x2 averaged chroma
            int r_avg = (r00 + r01 + r10 + r11 + 2) >> 2;
            int g_avg = (g00 + g01 + g10 + g11 + 2) >> 2;
            int b_avg = (b00 + b01 + b10 + b11 + 2) >> 2;
            int c_idx = c_row_offset + (sx >> 1);
            blk_cb[c_idx] = ((-43 * r_avg - 85 * g_avg + 128 * b_avg) >> 8);
            blk_cr[c_idx] = ((128 * r_avg - 107 * g_avg - 21 * b_avg) >> 8);
        }
    }
}

/* Encodes the w x h rectangle at (x0, y0) of a column-major BGR8 framebuffer whose columns are `column_rows`
 * pixels tall, as a standalone JPEG. x0, y0, w, h must be multiples of 16. The 16x16 MCUs are gathered exactly
 * like the full-frame encoder does, so every block has the same quantised coefficients as in the full picture
 * and a decoder reproduces the same pixels for that area. */
static size_t encode_420_rect(const uint8_t *src_bgr8, int column_rows, int x0, int y0, int width, int height,
                              int quality, uint8_t *out_jpeg, size_t out_capacity) {
    uint8_t dqt_lum[64], dqt_chr[64];
    uint32_t inv_lum[128], inv_chr[128]; // [64..127]: the largest magnitude that quantises to zero
    init_quant_tables(quality, dqt_lum, dqt_chr, inv_lum, inv_chr);

    HuffTable dc_lum_ht, dc_chr_ht, ac_lum_ht, ac_chr_ht;
    build_huff_table(dc_lum_bits, dc_lum_val, &dc_lum_ht);
    build_huff_table(dc_chr_bits, dc_chr_val, &dc_chr_ht);
    build_huff_table(ac_lum_bits, ac_lum_val, &ac_lum_ht);
    build_huff_table(ac_chr_bits, ac_chr_val, &ac_chr_ht);

    JpegBitWriter bw = {out_jpeg, out_capacity, 0, 0, 0};
    write_jpeg_headers(&bw, width, height, dqt_lum, dqt_chr, true);

    int32_t dc_y = 0, dc_cb = 0, dc_cr = 0;
    int32_t blk_y00[64], blk_y01[64], blk_y10[64], blk_y11[64];
    int32_t blk_cb[64], blk_cr[64];

    // Process 16x16 MCUs directly from 3DS column-major BGR8 framebuffer
    unsigned mcu_count = 0;
    for (int mcu_y = 0; mcu_y < height; mcu_y += 16) {
        for (int mcu_x = 0; mcu_x < width; mcu_x += 16) {
            const bool timed = (mcu_count++ & 15u) == 0; // every 16th MCU is timed (see capture_jpeg_ticks)
            uint64_t dct_ticks = 0, t0 = timed ? JPEG_TICK() : 0;
            gather_mcu420(src_bgr8, column_rows, x0 + mcu_x, y0 + mcu_y, blk_y00, blk_y01, blk_y10, blk_y11, blk_cb,
                          blk_cr);
            uint64_t t1 = timed ? JPEG_TICK() : 0;
            dct_acc = timed ? &dct_ticks : NULL;
            encode_block(&bw, blk_y00, inv_lum, &dc_y, &dc_lum_ht, &ac_lum_ht);
            encode_block(&bw, blk_y01, inv_lum, &dc_y, &dc_lum_ht, &ac_lum_ht);
            encode_block(&bw, blk_y10, inv_lum, &dc_y, &dc_lum_ht, &ac_lum_ht);
            encode_block(&bw, blk_y11, inv_lum, &dc_y, &dc_lum_ht, &ac_lum_ht);
            encode_block(&bw, blk_cb, inv_chr, &dc_cb, &dc_chr_ht, &ac_chr_ht);
            encode_block(&bw, blk_cr, inv_chr, &dc_cr, &dc_chr_ht, &ac_chr_ht);
            dct_acc = NULL;
            if (timed) {
                capture_jpeg_ticks[0] += t1 - t0;
                capture_jpeg_ticks[1] += dct_ticks;
                capture_jpeg_ticks[2] += JPEG_TICK() - t1;
                capture_jpeg_ticks[3]++;
            }
            capture_jpeg_ticks[4]++;
        }
    }

    if (bw.bit_count > 0) {
        bw_put_bits(&bw, 0x7F, 8 - bw.bit_count);
    }
    bw_put_byte(&bw, 0xFF);
    bw_put_byte(&bw, 0xD9);

    return (bw.pos <= bw.cap) ? bw.pos : 0;
}

size_t capture_encode_3ds_bgr8_jpeg_420(const uint8_t *src_bgr8, int width, int height, int quality, uint8_t *out_jpeg,
                                        size_t out_capacity) {
    return encode_420_rect(src_bgr8, height, 0, 0, width, height, quality, out_jpeg, out_capacity);
}

size_t capture_encode_3ds_bgr8_jpeg_420_rect(const uint8_t *src_bgr8, int column_rows, int x0, int y0, int width,
                                             int height, int quality, uint8_t *out_jpeg, size_t out_capacity) {
    if (!src_bgr8 || !out_jpeg || column_rows <= 0 || x0 < 0 || y0 < 0 || width <= 0 || height <= 0 ||
        (x0 | y0 | width | height) & 15 || y0 + height > column_rows) {
        return 0;
    }
    return encode_420_rect(src_bgr8, column_rows, x0, y0, width, height, quality, out_jpeg, out_capacity);
}

size_t capture_encode_jpeg_rgb24(const uint8_t *rgb24, int width, int height, int quality, uint8_t *out_jpeg,
                                 size_t out_capacity) {
    uint8_t dqt_lum[64], dqt_chr[64];
    uint32_t inv_lum[128], inv_chr[128]; // [64..127]: the largest magnitude that quantises to zero
    init_quant_tables(quality, dqt_lum, dqt_chr, inv_lum, inv_chr);

    HuffTable dc_lum_ht, dc_chr_ht, ac_lum_ht, ac_chr_ht;
    build_huff_table(dc_lum_bits, dc_lum_val, &dc_lum_ht);
    build_huff_table(dc_chr_bits, dc_chr_val, &dc_chr_ht);
    build_huff_table(ac_lum_bits, ac_lum_val, &ac_lum_ht);
    build_huff_table(ac_chr_bits, ac_chr_val, &ac_chr_ht);

    JpegBitWriter bw = {out_jpeg, out_capacity, 0, 0, 0};
    write_jpeg_headers(&bw, width, height, dqt_lum, dqt_chr, false);

    int32_t dc_y = 0, dc_cb = 0, dc_cr = 0;
    int32_t blk_y[64], blk_cb[64], blk_cr[64];

    for (int by = 0; by < height; by += 8) {
        for (int bx = 0; bx < width; bx += 8) {
            for (int y = 0; y < 8; y++) {
                int py = (by + y < height) ? (by + y) : (height - 1);
                const uint8_t *row = rgb24 + (py * width * 3);
                for (int x = 0; x < 8; x++) {
                    int px = (bx + x < width) ? (bx + x) : (width - 1);
                    int r = row[px * 3 + 0];
                    int g = row[px * 3 + 1];
                    int b = row[px * 3 + 2];

                    int idx = y * 8 + x;
                    blk_y[idx] = ((77 * r + 150 * g + 29 * b) >> 8) - 128;
                    blk_cb[idx] = ((-43 * r - 85 * g + 128 * b) >> 8);
                    blk_cr[idx] = ((128 * r - 107 * g - 21 * b) >> 8);
                }
            }
            encode_block(&bw, blk_y, inv_lum, &dc_y, &dc_lum_ht, &ac_lum_ht);
            encode_block(&bw, blk_cb, inv_chr, &dc_cb, &dc_chr_ht, &ac_chr_ht);
            encode_block(&bw, blk_cr, inv_chr, &dc_cr, &dc_chr_ht, &ac_chr_ht);
        }
    }

    if (bw.bit_count > 0) {
        bw_put_bits(&bw, 0x7F, 8 - bw.bit_count);
    }
    bw_put_byte(&bw, 0xFF);
    bw_put_byte(&bw, 0xD9);

    return (bw.pos <= bw.cap) ? bw.pos : 0;
}

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t capture_base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_capacity) {
    size_t out_len = 4 * ((src_len + 2) / 3);
    if (out_len + 1 > dst_capacity) {
        return 0;
    }

    size_t i = 0, j = 0;
    while (i + 2 < src_len) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[j++] = b64_table[(v >> 18) & 0x3F];
        dst[j++] = b64_table[(v >> 12) & 0x3F];
        dst[j++] = b64_table[(v >> 6) & 0x3F];
        dst[j++] = b64_table[v & 0x3F];
        i += 3;
    }

    if (i < src_len) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < src_len) {
            v |= (uint32_t)src[i + 1] << 8;
        }

        dst[j++] = b64_table[(v >> 18) & 0x3F];
        dst[j++] = b64_table[(v >> 12) & 0x3F];
        dst[j++] = (i + 1 < src_len) ? b64_table[(v >> 6) & 0x3F] : '=';
        dst[j++] = '=';
    }

    dst[j] = '\0';
    return j;
}
