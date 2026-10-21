/**
 * jpge.cpp — 微型 JPEG 编码器 for ESP32-S3
 *
 * 参考 public-domain jpge 库精简改写, 仅支持:
 *   - Baseline DCT Huffman
 *   - 8-bit YCbCr 4:2:0 或 grayscale
 *   - 质量 1-100
 */

#include "jpge.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>

/* ── 标准 JPEG 量化表 + Huffman 表 (baseline) ──────────── */
/* Standard luminance quant table (quality scaled) */
static const uint8_t _std_lum_quant[64] = {
    16,11,10,16,24,40,51,61,
    12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56,
    14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77,
    24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101,
    72,92,95,98,112,100,103,99
};

static const uint8_t _std_chr_quant[64] = {
    17,18,24,47,99,99,99,99,
    18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99,
    47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99
};

/* ── Zigzag order ──────────────────────────────────── */
static const uint8_t _zigzag[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

/* ── Standard JPEG Huffman tables (DC + AC for Y and CbCr) ── */
/* We use predefined Huffman tables (K.3.3 from JPEG spec) */
static const uint8_t _dc_lum_bits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t _dc_lum_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t _ac_lum_bits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const uint8_t _ac_lum_vals[162] = {
    1,2,3,0,4,17,5,18,33,49,65,6,19,81,97,7,34,113,20,50,129,145,161,
    8,35,66,177,193,21,82,209,240,36,51,98,114,130,9,10,22,23,24,25,26,
    37,38,39,40,41,42,52,53,54,55,56,57,58,67,68,69,70,71,72,73,74,83,
    84,85,86,87,88,89,90,99,100,101,102,103,104,105,106,115,116,117,118,
    119,120,121,122,131,132,133,134,135,136,137,138,146,147,148,149,150,
    151,152,153,154,162,163,164,165,166,167,168,169,170,178,179,180,181,
    182,183,184,185,186,194,195,196,197,198,199,200,201,202,210,211,212,
    213,214,215,216,217,218,225,226,227,228,229,230,231,232,233,234,241,
    242,243,244,245,246,247,248,249,250
};

static const uint8_t _dc_chr_bits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t _dc_chr_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t _ac_chr_bits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const uint8_t _ac_chr_vals[162] = {
    0,1,2,3,17,4,5,33,49,6,18,65,81,7,97,113,19,34,50,129,8,20,66,145,
    161,177,193,9,35,51,82,240,21,98,114,209,10,22,36,52,225,37,241,23,
    24,25,26,38,39,40,41,42,53,54,55,56,57,58,67,68,69,70,71,72,73,74,
    83,84,85,86,87,88,89,90,99,100,101,102,103,104,105,106,115,116,117,
    118,119,120,121,122,130,131,132,133,134,135,136,137,138,146,147,148,
    149,150,151,152,153,154,162,163,164,165,166,167,168,169,170,178,179,
    180,181,182,183,184,185,186,194,195,196,197,198,199,200,201,202,210,
    211,212,213,214,215,216,217,218,226,227,228,229,230,231,232,233,234,
    242,243,244,245,246,247,248,249,250
};

/* ── Bit writer ─────────────────────────────────────── */
typedef struct {
    uint8_t *buf; int pos; int max;
    uint8_t bit_buf; int bit_cnt;
} bits_t;

static void bits_init(bits_t *bs, uint8_t *buf, int max) {
    bs->buf = buf; bs->pos = 0; bs->max = max;
    bs->bit_buf = 0; bs->bit_cnt = 0;
}
static void bits_put(bits_t *bs, uint16_t code, int len) {
    while (len > 0) {
        int n = len < 8 - bs->bit_cnt ? len : 8 - bs->bit_cnt;
        bs->bit_buf |= (uint8_t)((code >> (len - n)) << (8 - bs->bit_cnt - n));
        bs->bit_cnt += n; len -= n;
        if (bs->bit_cnt == 8) {
            if (bs->pos < bs->max) bs->buf[bs->pos++] = bs->bit_buf;
            /* Byte stuffing: insert 0x00 after 0xFF in scan data */
            if (bs->bit_buf == 0xFF && bs->pos < bs->max) bs->buf[bs->pos++] = 0;
            bs->bit_buf = 0; bs->bit_cnt = 0;
        }
    }
}
static void bits_flush(bits_t *bs) {
    if (bs->bit_cnt > 0) {
        if (bs->pos < bs->max) bs->buf[bs->pos++] = bs->bit_buf;
        bs->bit_buf = 0; bs->bit_cnt = 0;
    }
}

/* ── Huffman table builder ──────────────────────────── */
typedef struct {
    uint16_t codes[256]; uint8_t code_lens[256];
} huff_t;

static void _build_huff(huff_t *tbl, const uint8_t *bits, const uint8_t *vals) {
    uint16_t code = 0;
    int idx = 0;
    /* Build lookup tables: given symbol value, get code & length */
    memset(tbl->codes, 0, sizeof(tbl->codes));
    memset(tbl->code_lens, 0, sizeof(tbl->code_lens));
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < bits[i]; j++) {
            uint8_t sym = vals[idx];
            tbl->codes[sym] = code;        /* codes[symbol] = code */
            tbl->code_lens[sym] = i + 1;   /* code_lens[symbol] = length */
            code++; idx++;
        }
        code <<= 1;
    }
}

/* ── JPEG marker writer ─────────────────────────────── */
static void _w16(uint8_t *buf, int *pos, uint16_t v) {
    buf[(*pos)++] = (uint8_t)(v >> 8); buf[(*pos)++] = (uint8_t)(v & 0xFF);
}
static void _marker(uint8_t *buf, int *pos, uint8_t m) {
    buf[(*pos)++] = 0xFF; buf[(*pos)++] = m;
}
static void _write_huff_table(uint8_t *buf, int *pos, int cls, int id,
                               const uint8_t *bits, const uint8_t *vals, int val_cnt) {
    _marker(buf, pos, 0xC4);
    int len_pos = *pos; *pos += 2;
    buf[(*pos)++] = (uint8_t)((cls << 4) | id);
    for (int i = 0; i < 16; i++) buf[(*pos)++] = bits[i];
    for (int i = 0; i < val_cnt; i++) buf[(*pos)++] = vals[i];
    _w16(buf, &len_pos, (uint16_t)(*pos - len_pos));
}

/* ── Write scan header + Huffman-encode one MCU block ── */
static void _encode_mcu_block(bits_t *bs, int16_t block[64],
                               int prev_dc, huff_t *dc_huff, huff_t *ac_huff) {
    int diff = block[0] - prev_dc;
    /* DC */
    int mag = abs(diff), bits = 0;
    while (mag) { bits++; mag >>= 1; }
    if (bits > 0) {
        bits_put(bs, dc_huff->codes[bits], dc_huff->code_lens[bits]);
        bits_put(bs, (uint16_t)(diff < 0 ? diff - 1 : diff) & ((1 << bits) - 1), bits);
    } else {
        bits_put(bs, dc_huff->codes[0], dc_huff->code_lens[0]);
    }
    /* AC */
    int nz = 0;
    for (int k = 1; k < 64; k++) {
        int val = block[_zigzag[k]];
        if (val == 0) { nz++; continue; }
        while (nz >= 16) {
            bits_put(bs, ac_huff->codes[0xF0], ac_huff->code_lens[0xF0]);
            nz -= 16;
        }
        mag = abs(val); bits = 0;
        while (mag) { bits++; mag >>= 1; }
        int rs = (nz << 4) | bits;
        bits_put(bs, ac_huff->codes[rs], ac_huff->code_lens[rs]);
        bits_put(bs, (uint16_t)(val < 0 ? val - 1 : val) & ((1 << bits) - 1), bits);
        nz = 0;
    }
    if (nz > 0) { /* EOB */
        bits_put(bs, ac_huff->codes[0], ac_huff->code_lens[0]);
    }
}

static const float _dct_tbl[8][8] = {
    {0.353553f,0.490393f,0.461940f,0.415735f,0.353553f,0.277785f,0.191342f,0.097545f},
    {0.353553f,0.415735f,0.191342f,-0.097545f,-0.353553f,-0.490393f,-0.461940f,-0.277785f},
    {0.353553f,0.277785f,-0.191342f,-0.490393f,-0.353553f,0.097545f,0.461940f,0.415735f},
    {0.353553f,0.097545f,-0.461940f,-0.277785f,0.353553f,0.415735f,-0.191342f,-0.490393f},
    {0.353553f,-0.097545f,-0.461940f,0.277785f,0.353553f,-0.415735f,-0.191342f,0.490393f},
    {0.353553f,-0.277785f,-0.191342f,0.490393f,-0.353553f,-0.097545f,0.461940f,-0.415735f},
    {0.353553f,-0.415735f,0.191342f,0.097545f,-0.353553f,0.490393f,-0.461940f,0.277785f},
    {0.353553f,-0.490393f,0.461940f,-0.415735f,0.353553f,-0.277785f,0.191342f,-0.097545f}
};

/* ── 8x8 FDCT ────────────────────────────────────────── */
static void fdct(const float src[64], float dst[64]) {
    float tmp[64];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            float s = 0;
            for (int k = 0; k < 8; k++) s += src[i * 8 + k] * _dct_tbl[j][k];
            tmp[i * 8 + j] = s;
        }
    }
    for (int j = 0; j < 8; j++) {
        for (int i = 0; i < 8; i++) {
            float s = 0;
            for (int k = 0; k < 8; k++) s += _dct_tbl[i][k] * tmp[k * 8 + j];
            dst[i * 8 + j] = s;
        }
    }
}

/* ═══════════════════════════════════════════════════════
 * Public API
 * ═══════════════════════════════════════════════════════ */

int jpge_encode_rgb888(const uint8_t *pixels, int w, int h, int channels,
                        uint8_t *out, int out_size, int quality) {
    if (!pixels || !out || w < 8 || h < 8 || out_size < 1024) return -1;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    int pos = 0;
    int mcu_w = (w + 7) / 8 * 8;
    int mcu_h = (h + 7) / 8 * 8;

    /* SOI */
    _marker(out, &pos, 0xD8);

    /* ── Quantization tables ── */
    _marker(out, &pos, 0xDB);
    int len_pos = pos;  /* save position of length field */
    _w16(out, &pos, 0); /* placeholder for length */
    out[pos++] = 0; /* table 0: luminance, 8-bit */
    float q_scale = (quality < 50) ? (5000.0f / quality) : (200.0f - quality * 2.0f);
    for (int i = 0; i < 64; i++) {
        int v = (int)((_std_lum_quant[i] * q_scale + 50.0f) / 100.0f);
        if (v < 1) v = 1;
            if (v > 255) v = 255;
        out[pos++] = (uint8_t)v;
    }
    out[pos++] = 1; /* table 1: chrominance */
    for (int i = 0; i < 64; i++) {
        int v = (int)((_std_chr_quant[i] * q_scale + 50.0f) / 100.0f);
        if (v < 1) v = 1;
            if (v > 255) v = 255;
        out[pos++] = (uint8_t)v;
    }
    /* Write actual length back to len_pos */
    int qlen = pos - len_pos;
    out[len_pos] = (uint8_t)(qlen >> 8);
    out[len_pos + 1] = (uint8_t)(qlen & 0xFF);

    /* ── Frame header (SOF0 baseline) ── */
    _marker(out, &pos, 0xC0);
    int ncomp = (channels >= 3) ? 3 : 1;
    _w16(out, &pos, (uint16_t)(8 + 3 * ncomp)); /* len = 8 + 3*ncomp */
    out[pos++] = 8; /* precision */
    _w16(out, &pos, (uint16_t)h); _w16(out, &pos, (uint16_t)w);
    out[pos++] = (uint8_t)ncomp;
    if (ncomp >= 3) {
        out[pos++] = 1; out[pos++] = 0x22; out[pos++] = 0; /* Y: h=2,v=2, qt=0 */
        out[pos++] = 2; out[pos++] = 0x11; out[pos++] = 1; /* Cb: h=1,v=1, qt=1 */
        out[pos++] = 3; out[pos++] = 0x11; out[pos++] = 1; /* Cr: h=1,v=1, qt=1 */
    } else {
        out[pos++] = 1; out[pos++] = 0x11; out[pos++] = 0; /* Y: h=1,v=1, qt=0 */
    }

    /* ── Huffman tables ── */
    _write_huff_table(out, &pos, 0, 0, _dc_lum_bits, _dc_lum_vals, 12);
    _write_huff_table(out, &pos, 1, 0, _ac_lum_bits, _ac_lum_vals, 162);
    if (ncomp >= 3) {
        _write_huff_table(out, &pos, 0, 1, _dc_chr_bits, _dc_chr_vals, 12);
        _write_huff_table(out, &pos, 1, 1, _ac_chr_bits, _ac_chr_vals, 162);
    }

    /* ── Scan header ── */
    _marker(out, &pos, 0xDA);
    _w16(out, &pos, 6 + ncomp * 2); /* len */
    out[pos++] = (uint8_t)ncomp;
    if (ncomp >= 3) {
        out[pos++] = 1; out[pos++] = 0x00; /* Y: DC+AC table 0 */
        out[pos++] = 2; out[pos++] = 0x11; /* Cb: DC+AC table 1 */
        out[pos++] = 3; out[pos++] = 0x11; /* Cr: DC+AC table 1 */
    } else {
        out[pos++] = 1; out[pos++] = 0x00;
    }
    out[pos++] = 0; out[pos++] = 63; out[pos++] = 0; /* Ss=0, Se=63, Ah/Al=0 */

    /* ── Build Huffman code tables ── */
    huff_t dc_lum, ac_lum, dc_chr, ac_chr;
    _build_huff(&dc_lum, _dc_lum_bits, _dc_lum_vals);
    _build_huff(&ac_lum, _ac_lum_bits, _ac_lum_vals);
    _build_huff(&dc_chr, _dc_chr_bits, _dc_chr_vals);
    _build_huff(&ac_chr, _ac_chr_bits, _ac_chr_vals);

    /* ── Encode MCUs ── */
    bits_t bs; bits_init(&bs, out + pos, out_size - pos);

    int prev_dc_y = 0, prev_dc_cb = 0, prev_dc_cr = 0;

    uint8_t *Y = NULL, *Cb = NULL, *Cr = NULL;
    if (channels >= 3) {
        Y  = (uint8_t *)malloc((size_t)w * h);
        Cb = (uint8_t *)malloc((size_t)w * h);
        Cr = (uint8_t *)malloc((size_t)w * h);
    }

    if (channels >= 3 && Y && Cb && Cr) {
        /* RGB→YCbCr */
        for (int i = 0; i < w * h; i++) {
            int r = pixels[i * 3], g = pixels[i * 3 + 1], b = pixels[i * 3 + 2];
            Y[i]  = (uint8_t)(0.299f * r + 0.587f * g + 0.114f * b);
            Cb[i] = (uint8_t)(-0.1687f * r - 0.3313f * g + 0.5f * b + 128.0f);
            Cr[i] = (uint8_t)(0.5f * r - 0.4187f * g - 0.0813f * b + 128.0f);
        }
        for (int my = 0; my < mcu_h; my += 16) {
            for (int mx = 0; mx < mcu_w; mx += 16) {
                /* 4 Y blocks (16x16) */
                for (int by = 0; by < 2; by++) {
                    for (int bx = 0; bx < 2; bx++) {
                        float src[64], dct[64];
                        for (int dy = 0; dy < 8; dy++) {
                            for (int dx = 0; dx < 8; dx++) {
                                int px = mx + bx * 8 + dx, py = my + by * 8 + dy;
                                uint8_t v = (px < w && py < h) ? Y[py * w + px] : Y[(py < h ? py : h - 1) * w + (px < w ? px : w - 1)];
                                src[dy * 8 + dx] = v - 128.0f;
                            }
                        }
                        fdct(src, dct);
                        int16_t qblock[64];
                        float *qt = (float *)malloc(64 * sizeof(float));
                        /* build scaled quant */
                        for (int k = 0; k < 64; k++) {
                            int qv = (int)((_std_lum_quant[k] * q_scale + 50.0f) / 100.0f);
                            if (qv < 1) qv = 1;
                            qt[k] = (float)qv;
                        }
                        for (int k = 0; k < 64; k++) {
                            int v = (int)roundf(dct[k] / qt[k]);
                            if (v < -2047) v = -2047;
                            if (v > 2047) v = 2047;
                            qblock[k] = (int16_t)v;
                        }
                        free(qt);
                        _encode_mcu_block(&bs, qblock, prev_dc_y, &dc_lum, &ac_lum);
                        prev_dc_y = qblock[0];
                    }
                }
                /* Cb block (subsampled) */
                float src[64], dct[64];
                for (int dy = 0; dy < 8; dy++) {
                    for (int dx = 0; dx < 8; dx++) {
                        int px = (mx >> 1) + dx, py = (my >> 1) + dy;
                        if (px >= w) px = w - 1;
                            if (py >= h) py = h - 1;
                        src[dy * 8 + dx] = Cb[py * w + px] - 128.0f;
                    }
                }
                fdct(src, dct);
                float *qt2 = (float *)malloc(64 * sizeof(float));
                for (int k = 0; k < 64; k++) {
                    int qv = (int)((_std_chr_quant[k] * q_scale + 50.0f) / 100.0f);
                    if (qv < 1) qv = 1;
                    qt2[k] = (float)qv;
                }
                int16_t qblock_cb[64], qblock_cr[64];
                for (int k = 0; k < 64; k++) qblock_cb[k] = (int16_t)roundf(dct[k] / qt2[k]);
                free(qt2);
                _encode_mcu_block(&bs, qblock_cb, prev_dc_cb, &dc_chr, &ac_chr);
                prev_dc_cb = qblock_cb[0];

                /* Cr block */
                for (int dy = 0; dy < 8; dy++) {
                    for (int dx = 0; dx < 8; dx++) {
                        int px = (mx >> 1) + dx, py = (my >> 1) + dy;
                        if (px >= w) px = w - 1;
                            if (py >= h) py = h - 1;
                        src[dy * 8 + dx] = Cr[py * w + px] - 128.0f;
                    }
                }
                fdct(src, dct);
                float *qt3 = (float *)malloc(64 * sizeof(float));
                for (int k = 0; k < 64; k++) {
                    int qv = (int)((_std_chr_quant[k] * q_scale + 50.0f) / 100.0f);
                    if (qv < 1) qv = 1;
                    qt3[k] = (float)qv;
                }
                for (int k = 0; k < 64; k++) qblock_cr[k] = (int16_t)roundf(dct[k] / qt3[k]);
                free(qt3);
                _encode_mcu_block(&bs, qblock_cr, prev_dc_cr, &dc_chr, &ac_chr);
                prev_dc_cr = qblock_cr[0];
            }
        }
    } else {
        /* Grayscale: same as Y-only above but simpler */
        /* Fill with dummy gray for now */
        for (int my = 0; my < mcu_h; my += 8) {
            for (int mx = 0; mx < mcu_w; mx += 8) {
                float src[64], dct[64];
                for (int dy = 0; dy < 8; dy++) {
                    for (int dx = 0; dx < 8; dx++) {
                        int px = mx + dx, py = my + dy;
                        uint8_t v = 128;
                        if (px < w && py < h) v = pixels[(py * w + px) * channels];
                        src[dy * 8 + dx] = v - 128.0f;
                    }
                }
                fdct(src, dct);
                int16_t qblock[64];
                for (int k = 0; k < 64; k++) {
                    int qv = (int)((_std_lum_quant[k] * q_scale + 50.0f) / 100.0f);
                    if (qv < 1) qv = 1;
                    int v = (int)roundf(dct[k] / qv);
                    if (v < -2047) v = -2047;
                            if (v > 2047) v = 2047;
                    qblock[k] = (int16_t)v;
                }
                _encode_mcu_block(&bs, qblock, prev_dc_y, &dc_lum, &ac_lum);
                prev_dc_y = qblock[0];
            }
        }
    }

    bits_flush(&bs);
    pos += bs.pos;

    if (Y) free(Y);
    if (Cb) free(Cb);
    if (Cr) free(Cr);

    /* EOI */
    if (pos + 2 <= out_size) { _marker(out, &pos, 0xD9); }
    return pos;
}
