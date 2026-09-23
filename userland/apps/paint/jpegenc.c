// jpegenc.c - Maytera Studio baseline JPEG encoder (Studio plan P5: file
// formats). A pure function over an ARGB buffer: no document, no UI, no file
// I/O. imgio.c owns the flatten + write around it; the host-side self-test in
// tools/ (see the paintphase CHANGELOG entry) compiles this same file with
// -DJPEGENC_HOST and decodes the result with an independent decoder.
//
// What it writes: ITU T.81 baseline sequential DCT, 8-bit, JFIF 1.1 APP0,
// YCbCr, the Annex K.1 quantisation tables scaled by the IJG quality formula,
// the Annex K.3 "typical" Huffman tables (written into the file, so any
// decoder derives exactly the codes used here), one interleaved scan, no
// restart intervals. Chroma is 4:2:0 (2x2 box-averaged) by default or 4:4:4.
// The kernel decoder (kernel/gui/jpeg.c) handles both sampling layouts via
// max_h/max_v with box upsampling, which is what the Open path uses to read
// the file back.
//
// Why float: this is Ring-3 userland, which is hardware SSE2 float (see
// CLAUDE.md, "USERLAND IS HARDWARE FLOAT"); the kernel's soft-float rule does
// not apply here. The forward DCT is the plain separable 8x8 transform over a
// cosine table built from eight exact constants (no libm, no cos()).
#ifdef JPEGENC_HOST
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#else
#include "studio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#endif

// --------------------------------------------------------------------------
// Standard tables (ITU T.81 Annex K)
// --------------------------------------------------------------------------
static const unsigned char ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};
// K.1 luminance / chrominance quantisation tables (natural order).
static const unsigned char QLUM[64] = {
    16,11,10,16,24,40,51,61,   12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56,   14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99
};
static const unsigned char QCHR[64] = {
    17,18,24,47,99,99,99,99,   18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99,   47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,   99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99,   99,99,99,99,99,99,99,99
};
// K.3 Huffman table specifications: BITS[1..16] then HUFFVAL.
static const unsigned char DC_LUM_BITS[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const unsigned char DC_LUM_VAL[12]  = {0,1,2,3,4,5,6,7,8,9,10,11};
static const unsigned char DC_CHR_BITS[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const unsigned char DC_CHR_VAL[12]  = {0,1,2,3,4,5,6,7,8,9,10,11};
static const unsigned char AC_LUM_BITS[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const unsigned char AC_LUM_VAL[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};
static const unsigned char AC_CHR_BITS[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
static const unsigned char AC_CHR_VAL[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

// --------------------------------------------------------------------------
// Encoder state
// --------------------------------------------------------------------------
typedef struct { unsigned short code[256]; unsigned char len[256]; } hufftab_t;

typedef struct {
    unsigned char *buf; long len, cap; int oom;
    unsigned int bitbuf; int nbits;
    hufftab_t dc_l, ac_l, dc_c, ac_c;
    unsigned char qt[2][64];          // natural order, scaled
    float qdiv[2][64];                // 1/qt for the quantiser
    float cosv[8][8];                 // cosv[x][u] = cos((2x+1)u*pi/16)
    int dcpred[3];
} jenc_t;

// Canonical code assignment (T.81 Annex C): codes are assigned in increasing
// length, increasing value order, exactly as a decoder rebuilds them from the
// DHT segment this file writes.
static void huff_build(hufftab_t *t, const unsigned char *bits, const unsigned char *vals) {
    memset(t, 0, sizeof(*t));
    unsigned code = 0; int k = 0;
    for (int l = 1; l <= 16; l++) {
        for (int i = 0; i < bits[l - 1]; i++) {
            t->code[vals[k]] = (unsigned short)code;
            t->len[vals[k]]  = (unsigned char)l;
            k++; code++;
        }
        code <<= 1;
    }
}

// Eight exact cosines cos(k*pi/16), k=0..7. The full (2x+1)u table follows
// from these by the symmetry of cos, so no libm is needed.
static const float COS16[8] = {
    1.0f, 0.98078528f, 0.92387953f, 0.83146961f,
    0.70710678f, 0.55557023f, 0.38268343f, 0.19509032f
};
static float cos_idx(int a) {         // cos(a*pi/16) for any integer a
    a &= 31;
    if (a == 8 || a == 24) return 0.0f;
    if (a < 8)  return  COS16[a];
    if (a < 16) return -COS16[16 - a];
    if (a < 24) return -COS16[a - 16];
    return COS16[32 - a];
}

static void enc_init(jenc_t *e, int quality, long cap0) {
    memset(e, 0, sizeof(*e));
    e->cap = cap0 < 4096 ? 4096 : cap0;
    e->buf = (unsigned char *)malloc((size_t)e->cap);
    if (!e->buf) { e->oom = 1; e->cap = 0; }
    huff_build(&e->dc_l, DC_LUM_BITS, DC_LUM_VAL);
    huff_build(&e->ac_l, AC_LUM_BITS, AC_LUM_VAL);
    huff_build(&e->dc_c, DC_CHR_BITS, DC_CHR_VAL);
    huff_build(&e->ac_c, AC_CHR_BITS, AC_CHR_VAL);
    // IJG quality scaling: 1..100 -> a percentage applied to the K.1 tables.
    int q = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
    int scale = q < 50 ? 5000 / q : 200 - 2 * q;
    for (int i = 0; i < 64; i++) {
        int l = (QLUM[i] * scale + 50) / 100; if (l < 1) l = 1; if (l > 255) l = 255;
        int c = (QCHR[i] * scale + 50) / 100; if (c < 1) c = 1; if (c > 255) c = 255;
        e->qt[0][i] = (unsigned char)l; e->qdiv[0][i] = 1.0f / (float)l;
        e->qt[1][i] = (unsigned char)c; e->qdiv[1][i] = 1.0f / (float)c;
    }
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++)
            e->cosv[x][u] = cos_idx((2 * x + 1) * u);
}

static void put_byte(jenc_t *e, unsigned char b) {
    if (e->oom) return;
    if (e->len >= e->cap) {
        long nc = e->cap * 2;
        unsigned char *nb = (unsigned char *)realloc(e->buf, (size_t)nc);
        if (!nb) { e->oom = 1; return; }
        e->buf = nb; e->cap = nc;
    }
    e->buf[e->len++] = b;
}
static void put_bytes(jenc_t *e, const unsigned char *p, int n) { for (int i = 0; i < n; i++) put_byte(e, p[i]); }
static void put_marker(jenc_t *e, unsigned char m) { put_byte(e, 0xFF); put_byte(e, m); }
static void put_u16(jenc_t *e, unsigned v) { put_byte(e, (unsigned char)(v >> 8)); put_byte(e, (unsigned char)v); }

// Entropy-coded segment bit writer. A 0xFF data byte is followed by a stuffed
// 0x00 so it can never be mistaken for a marker.
static void put_bits(jenc_t *e, unsigned code, int nbits) {
    e->bitbuf = (e->bitbuf << nbits) | (code & ((1u << nbits) - 1u));
    e->nbits += nbits;
    while (e->nbits >= 8) {
        unsigned char b = (unsigned char)(e->bitbuf >> (e->nbits - 8));
        put_byte(e, b);
        if (b == 0xFF) put_byte(e, 0x00);
        e->nbits -= 8;
    }
    e->bitbuf &= (1u << e->nbits) - 1u;
}
static void flush_bits(jenc_t *e) {
    if (e->nbits > 0) put_bits(e, 0x7F, 8 - e->nbits);   // pad with 1-bits
}

// Magnitude category (number of bits) of a coefficient, T.81 Table F.1.
static int bit_size(int v) {
    if (v < 0) v = -v;
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}
// Additional bits: v itself if positive, else (v - 1) in the low `size` bits.
static unsigned extra_bits(int v, int size) {
    return (v >= 0 ? (unsigned)v : (unsigned)(v - 1)) & ((1u << size) - 1u);
}

// Forward DCT + quantise one 8x8 block of level-shifted samples into zigzag
// order, then Huffman-code it. `blk` holds 64 samples 0..255 row-major.
static void encode_block(jenc_t *e, const unsigned char *blk, int tq,
                         const hufftab_t *dct, const hufftab_t *act, int comp) {
    float in[64], tmp[64];
    for (int i = 0; i < 64; i++) in[i] = (float)blk[i] - 128.0f;
    // rows: tmp[y][u] = sum_x in[y][x] * cos((2x+1)u pi/16)
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0.0f;
            for (int x = 0; x < 8; x++) s += in[y * 8 + x] * e->cosv[x][u];
            tmp[y * 8 + u] = s;
        }
    // columns + normalisation: F[v][u] = C(u)C(v)/4 * sum_y tmp[y][u] cos((2y+1)v pi/16)
    int q[64];
    for (int v = 0; v < 8; v++)
        for (int u = 0; u < 8; u++) {
            float s = 0.0f;
            for (int y = 0; y < 8; y++) s += tmp[y * 8 + u] * e->cosv[y][v];
            float cu = (u == 0) ? 0.70710678f : 1.0f;
            float cv = (v == 0) ? 0.70710678f : 1.0f;
            float f = s * cu * cv * 0.25f * e->qdiv[tq][v * 8 + u];
            q[v * 8 + u] = f >= 0.0f ? (int)(f + 0.5f) : -(int)(-f + 0.5f);
        }
    // DC: difference from the previous block of this component.
    int dc = q[0], diff = dc - e->dcpred[comp];
    e->dcpred[comp] = dc;
    int sz = bit_size(diff);
    put_bits(e, dct->code[sz], dct->len[sz]);
    if (sz) put_bits(e, extra_bits(diff, sz), sz);
    // AC: run-length of zeros in zigzag order, ZRL (0xF0) per 16 zeros, EOB.
    int run = 0;
    for (int k = 1; k < 64; k++) {
        int c = q[ZIGZAG[k]];
        if (c == 0) { run++; continue; }
        while (run > 15) { put_bits(e, act->code[0xF0], act->len[0xF0]); run -= 16; }
        int s = bit_size(c);
        int sym = (run << 4) | s;
        put_bits(e, act->code[sym], act->len[sym]);
        put_bits(e, extra_bits(c, s), s);
        run = 0;
    }
    if (run > 0) put_bits(e, act->code[0x00], act->len[0x00]);
}

// JFIF YCbCr from 8-bit RGB (fixed point x256; rounding at the end).
static void rgb_to_ycc(uint32_t p, int *y, int *cb, int *cr) {
    int r = (int)(p >> 16) & 255, g = (int)(p >> 8) & 255, b = (int)p & 255;
    int yy = ( 77 * r + 150 * g +  29 * b + 128) >> 8;
    int cbv = ((-43 * r -  85 * g + 128 * b + 128) >> 8) + 128;
    int crv = ((128 * r - 107 * g -  21 * b + 128) >> 8) + 128;
    *y  = yy  < 0 ? 0 : (yy  > 255 ? 255 : yy);
    *cb = cbv < 0 ? 0 : (cbv > 255 ? 255 : cbv);
    *cr = crv < 0 ? 0 : (crv > 255 ? 255 : crv);
}
// Edge-replicated sample fetch (the image is not a multiple of the MCU size in
// general; T.81 leaves the padding to the encoder and replication keeps the
// edge blocks free of ringing from an artificial hard edge).
static uint32_t px_at(const uint32_t *src, int w, int h, int x, int y) {
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    return src[(long)y * w + x];
}

static void write_headers(jenc_t *e, int w, int h, int chroma444) {
    put_marker(e, 0xD8);                                   // SOI
    put_marker(e, 0xE0); put_u16(e, 16);                   // APP0 JFIF
    static const unsigned char jfif[14] = {'J','F','I','F',0, 1,1, 0, 0,1, 0,1, 0,0};
    put_bytes(e, jfif, 14);
    put_marker(e, 0xDB); put_u16(e, 2 + 65 * 2);           // DQT, both tables, zigzag order
    for (int t = 0; t < 2; t++) {
        put_byte(e, (unsigned char)t);
        for (int k = 0; k < 64; k++) put_byte(e, e->qt[t][ZIGZAG[k]]);
    }
    put_marker(e, 0xC0); put_u16(e, 8 + 3 * 3);            // SOF0 baseline, 3 components
    put_byte(e, 8); put_u16(e, (unsigned)h); put_u16(e, (unsigned)w); put_byte(e, 3);
    put_byte(e, 1); put_byte(e, chroma444 ? 0x11 : 0x22); put_byte(e, 0);   // Y
    put_byte(e, 2); put_byte(e, 0x11); put_byte(e, 1);                       // Cb
    put_byte(e, 3); put_byte(e, 0x11); put_byte(e, 1);                       // Cr
    put_marker(e, 0xC4);                                   // DHT, all four tables
    put_u16(e, 2 + (17 + 12) * 2 + (17 + 162) * 2);
    put_byte(e, 0x00); put_bytes(e, DC_LUM_BITS, 16); put_bytes(e, DC_LUM_VAL, 12);
    put_byte(e, 0x10); put_bytes(e, AC_LUM_BITS, 16); put_bytes(e, AC_LUM_VAL, 162);
    put_byte(e, 0x01); put_bytes(e, DC_CHR_BITS, 16); put_bytes(e, DC_CHR_VAL, 12);
    put_byte(e, 0x11); put_bytes(e, AC_CHR_BITS, 16); put_bytes(e, AC_CHR_VAL, 162);
    put_marker(e, 0xDA); put_u16(e, 6 + 2 * 3);            // SOS
    put_byte(e, 3);
    put_byte(e, 1); put_byte(e, 0x00);
    put_byte(e, 2); put_byte(e, 0x11);
    put_byte(e, 3); put_byte(e, 0x11);
    put_byte(e, 0); put_byte(e, 63); put_byte(e, 0);       // Ss, Se, Ah/Al
}

// Encode `src` (w*h ARGB8888, alpha ignored: the caller flattens) to a
// malloc'd JPEG in *out / *outlen. quality 1..100; chroma444 selects 4:4:4
// sampling (0 = 4:2:0). Returns 0 ok, -1 on bad args or out of memory. The
// caller frees *out.
int jpeg_encode_argb(const uint32_t *src, int w, int h, int quality, int chroma444,
                     unsigned char **out, long *outlen) {
    if (!src || !out || !outlen || w < 1 || h < 1 || w > 65535 || h > 65535) return -1;
    *out = 0; *outlen = 0;
    jenc_t *e = (jenc_t *)malloc(sizeof(jenc_t));
    if (!e) return -1;
    enc_init(e, quality, (long)w * h / 2 + 8192);
    if (e->oom) { free(e); return -1; }
    write_headers(e, w, h, chroma444);

    unsigned char yb[4][64], cbb[64], crb[64];
    if (chroma444) {
        // One MCU = one 8x8 block of each component.
        for (int my = 0; my < h; my += 8)
            for (int mx = 0; mx < w; mx += 8) {
                for (int j = 0; j < 8; j++)
                    for (int i = 0; i < 8; i++) {
                        int y, cb, cr;
                        rgb_to_ycc(px_at(src, w, h, mx + i, my + j), &y, &cb, &cr);
                        yb[0][j * 8 + i] = (unsigned char)y;
                        cbb[j * 8 + i] = (unsigned char)cb;
                        crb[j * 8 + i] = (unsigned char)cr;
                    }
                encode_block(e, yb[0], 0, &e->dc_l, &e->ac_l, 0);
                encode_block(e, cbb, 1, &e->dc_c, &e->ac_c, 1);
                encode_block(e, crb, 1, &e->dc_c, &e->ac_c, 2);
            }
    } else {
        // One MCU = a 16x16 pixel square: four Y blocks (raster order) then
        // one Cb and one Cr block, each chroma sample the mean of a 2x2 cell.
        for (int my = 0; my < h; my += 16)
            for (int mx = 0; mx < w; mx += 16) {
                int cbs[64], crs[64];
                memset(cbs, 0, sizeof(cbs)); memset(crs, 0, sizeof(crs));
                for (int j = 0; j < 16; j++)
                    for (int i = 0; i < 16; i++) {
                        int y, cb, cr;
                        rgb_to_ycc(px_at(src, w, h, mx + i, my + j), &y, &cb, &cr);
                        int blk = (j >> 3) * 2 + (i >> 3);
                        yb[blk][(j & 7) * 8 + (i & 7)] = (unsigned char)y;
                        cbs[(j >> 1) * 8 + (i >> 1)] += cb;
                        crs[(j >> 1) * 8 + (i >> 1)] += cr;
                    }
                for (int k = 0; k < 64; k++) {
                    cbb[k] = (unsigned char)((cbs[k] + 2) >> 2);
                    crb[k] = (unsigned char)((crs[k] + 2) >> 2);
                }
                for (int b = 0; b < 4; b++) encode_block(e, yb[b], 0, &e->dc_l, &e->ac_l, 0);
                encode_block(e, cbb, 1, &e->dc_c, &e->ac_c, 1);
                encode_block(e, crb, 1, &e->dc_c, &e->ac_c, 2);
            }
    }
    flush_bits(e);
    put_marker(e, 0xD9);                                   // EOI
    if (e->oom) { free(e->buf); free(e); return -1; }
    *out = e->buf; *outlen = e->len;
    free(e);
    return 0;
}

#ifdef JPEGENC_HOST
// Host self-test: builds a synthetic image (gradients + a hard-edged box +
// a thin diagonal), encodes it at a few qualities in both samplings, and
// writes the files for an independent decoder (PIL) to check dimensions and
// PSNR. Also checks the K.3 AC tables cover every run/size symbol exactly
// once, which is what makes every coefficient encodable.
#include <stdio.h>
static int table_covers(const unsigned char *vals) {
    int seen[256]; memset(seen, 0, sizeof(seen));
    for (int i = 0; i < 162; i++) { if (seen[vals[i]]) return 0; seen[vals[i]] = 1; }
    for (int r = 0; r < 16; r++) for (int s = 1; s <= 10; s++) if (!seen[(r << 4) | s]) return 0;
    return seen[0x00] && seen[0xF0];
}
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!table_covers(AC_LUM_VAL) || !table_covers(AC_CHR_VAL)) { printf("AC table coverage FAIL\n"); return 1; }
    int w = 333, h = 211;                        // deliberately not MCU-aligned
    uint32_t *img = (uint32_t *)malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int r = x * 255 / (w - 1), g = y * 255 / (h - 1), b = 128;
        if (x > 60 && x < 140 && y > 40 && y < 120) { r = 20; g = 200; b = 40; }
        if (x == y || x == y + 1) { r = 255; g = 255; b = 255; }
        img[y * w + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    // Raw reference for the decoder-side PSNR check.
    FILE *raw = fopen("ref.rgb", "wb");
    for (int i = 0; i < w * h; i++) { unsigned char p[3] = { (img[i]>>16)&255, (img[i]>>8)&255, img[i]&255 }; fwrite(p, 1, 3, raw); }
    fclose(raw);
    int qs[4] = { 30, 75, 90, 100 };
    for (int c = 0; c < 2; c++) for (int i = 0; i < 4; i++) {
        unsigned char *out; long len;
        if (jpeg_encode_argb(img, w, h, qs[i], c, &out, &len) != 0) { printf("encode FAIL q=%d c=%d\n", qs[i], c); return 1; }
        char name[64]; snprintf(name, sizeof name, "t_q%d_%s.jpg", qs[i], c ? "444" : "420");
        FILE *f = fopen(name, "wb"); fwrite(out, 1, (size_t)len, f); fclose(f);
        printf("%s %ld bytes\n", name, len);
        free(out);
    }
    printf("W=%d H=%d\n", w, h);
    return 0;
}
#endif
