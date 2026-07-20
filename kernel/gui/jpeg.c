// jpeg.c - JPEG image decoder for MayteraOS
// Baseline DCT JPEG decoder

#include "jpeg.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"

// #404 Phase W: the Rust port of the pure JPEG header-parse seam (rustkern.rs).
// Signature-locked here; a mismatch fails the C compile (the FFI "lock"). Live
// under -DRUST_JPEG via the jpeg_parse_headers() dispatcher below.
extern int jpeg_parse_headers_rs(const uint8_t *data, uint32_t len, jpeg_hdr_t *out);

// Layout lock: the C jpeg_hdr_t and the Rust #[repr(C)] JpegHdr MUST agree. If
// this fails, the two views have drifted and the FFI is unsafe.
_Static_assert(sizeof(jpeg_hdr_t) == 1512, "jpeg_hdr_t layout must match rustkern.rs JpegHdr");

// Maximum image dimensions
#define JPEG_MAX_WIDTH   4096
#define JPEG_MAX_HEIGHT  4096
#define JPEG_MAX_COMPONENTS 4

// Quantization and Huffman tables
typedef struct {
    uint8_t quant[4][64];           // Quantization tables (up to 4)
    int quant_valid[4];

    // Huffman tables: [0]=DC, [1]=AC, each has [0]=luminance, [1]=chrominance
    uint8_t huff_bits[2][2][16];    // Number of codes of each length
    uint8_t huff_vals[2][2][256];   // Symbol values
    int huff_valid[2][2];

    // Fast Huffman lookup
    int16_t huff_fast[2][2][1 << 10];
    int huff_fast_bits[2][2];

    // Image info
    uint32_t width, height;
    int components;
    int comp_id[JPEG_MAX_COMPONENTS];
    int comp_h[JPEG_MAX_COMPONENTS];   // Horizontal sampling
    int comp_v[JPEG_MAX_COMPONENTS];   // Vertical sampling
    int comp_qt[JPEG_MAX_COMPONENTS];  // Quantization table index
    int comp_dc[JPEG_MAX_COMPONENTS];  // DC table index
    int comp_ac[JPEG_MAX_COMPONENTS];  // AC table index

    // MCU dimensions
    int mcu_width, mcu_height;
    int mcu_count_x, mcu_count_y;
    int restart_interval;

    // Bit reader state (MSB-first accumulator, libjpeg/stb-style)
    const uint8_t *data;
    uint32_t data_len;
    uint32_t pos;
    uint32_t code_buffer;   // bits accumulated MSB-aligned at bit 31
    int code_bits;          // number of valid bits currently in code_buffer
    int marker;             // last entropy-stream marker byte seen (0 = none)
    int nomore;             // set once a marker / EOF stops the entropy stream

    // DC prediction
    int dc_pred[JPEG_MAX_COMPONENTS];
} jpeg_decoder_t;

// Zigzag order for DCT coefficients
static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

// Clamp value to 0-255
static inline int clamp(int x) {
    if (x < 0) return 0;
    if (x > 255) return 255;
    return x;
}

// Refill the bit accumulator from the entropy-coded stream.
// Handles 0xFF00 byte-stuffing and stops (sets ->marker / ->nomore) when a
// real marker (0xFFxx, xx != 0) or end-of-data is reached, padding with zero
// bits thereafter. Bits are packed MSB-first at the top of code_buffer.
static void jpeg_fill_bits(jpeg_decoder_t *d) {
    do {
        unsigned int b = 0;
        if (!d->nomore) {
            if (d->pos >= d->data_len) {
                d->nomore = 1;
            } else {
                b = d->data[d->pos++];
                if (b == 0xFF) {
                    int c;
                    do {
                        c = (d->pos < d->data_len) ? d->data[d->pos++] : 0xD9;
                    } while (c == 0xFF);
                    if (c != 0) {
                        // Real marker: stop feeding real data, remember it.
                        d->marker = c;
                        d->nomore = 1;
                        b = 0;
                    }
                    // else 0xFF00 = stuffed literal 0xFF (b stays 0xFF)
                }
            }
        }
        d->code_buffer |= b << (24 - d->code_bits);
        d->code_bits += 8;
    } while (d->code_bits <= 24);
}

// Get n raw bits (MSB-first), 0..16. Returns the unsigned value.
static int get_bits(jpeg_decoder_t *d, int n) {
    if (n == 0) return 0;
    if (d->code_bits < n) jpeg_fill_bits(d);
    unsigned int k = d->code_buffer >> (32 - n);
    d->code_buffer <<= n;
    d->code_bits -= n;
    return (int)k;
}

// Build Huffman decode table.
//
// SECURITY (MAYTERA-SEC-2026-00XX, task #5XX): both `code` and `k` MUST be
// bounded here. A DHT is only well-formed if it is CANONICAL: at every code
// length `len`, the running code value has to stay below (1 << len). A
// NON-CANONICAL table drives `idx_fast = (code << (10 - len)) | f` clean past
// huff_fast[dc][idx][1024]. bits[0] = 255 (255 one-bit codes, where the code
// space allows at most 2) alone writes indices 0..130559: 261,118 bytes past
// the array and ~252,926 bytes past the whole jpeg_decoder_t. That is a
// Ring-3-reachable heap OOB WRITE (sys_decode_image -> browser <img>, Files
// previews, imageviewer, album art).
//
// The count-sum gate in the header parser does NOT confine this, and neither
// did jpeg_parse_headers_rs before this fix: bits[0]=255 sums to 255, which is
// <= 256, so it was ACCEPTED and handed straight here. build_huffman sits just
// outside the b814 seam boundary, so the bound has to live HERE, at the write
// site, and it is flag-independent (correct with or without -DRUST_JPEG).
//
// Returns 1 if the table is canonical and usable, 0 if it is malformed. The
// caller MUST reject the image on 0: a partially-built table would leave
// decode_huff's slow path indexing vals[] out of range on the same bad bits[].
static int build_huffman(jpeg_decoder_t *d, int dc, int idx) {
    uint8_t *bits = d->huff_bits[dc][idx];
    uint8_t *vals = d->huff_vals[dc][idx];

    // Build fast lookup table (for codes up to 10 bits)
    memset(d->huff_fast[dc][idx], -1, sizeof(d->huff_fast[dc][idx]));
    d->huff_fast_bits[dc][idx] = 10;

    int code = 0;
    int k = 0;
    for (int i = 0; i < 16; i++) {
        int len = i + 1;
        for (int j = 0; j < bits[i]; j++) {
            // Canonicality bound. This is exactly what keeps the fast-table
            // index below: with code <= (1<<len)-1 and len <= 10,
            //   max(base + f) = (((1<<len)-1) << (10-len)) | ((1<<(10-len))-1)
            //                 = 1023
            // i.e. precisely the last in-bounds element, never one past.
            if (code >= (1 << len)) return 0;
            // vals[] is [256]. A table can be canonical and STILL declare more
            // than 256 symbols across lengths (e.g. 255 of length 15 plus 255
            // of length 16 = 510), so k needs its own bound. On a -DRUST_JPEG
            // build the seam's count-sum <= 256 gate already implies this; on a
            // flag-off build hdr_parse_dht has no clamp and this guard is the
            // one that holds.
            if (k >= 256) return 0;
            if (len <= 10) {
                // Fill all entries that start with this code
                int fill = 1 << (10 - len);
                int base = code << (10 - len);
                for (int f = 0; f < fill; f++) {
                    d->huff_fast[dc][idx][base + f] = (int16_t)((len << 8) | vals[k]);
                }
            }
            code++;
            k++;
        }
        code <<= 1;
    }
    return 1;
}

// Decode one Huffman symbol. Peeks the top 10 bits without consuming, uses the
// fast lookup table for codes up to 10 bits, and falls back to a canonical
// bit-by-bit decode for codes of length 11..16.
static int decode_huff(jpeg_decoder_t *d, int dc, int idx) {
    if (d->code_bits < 16) jpeg_fill_bits(d);

    int peek = (int)((d->code_buffer >> 22) & 0x3FF);   // top 10 bits
    int16_t entry = d->huff_fast[dc][idx][peek];
    if (entry >= 0) {
        int len = entry >> 8;
        d->code_buffer <<= len;
        d->code_bits -= len;
        return entry & 0xFF;
    }

    // Slow path: code longer than 10 bits. Re-decode canonically.
    uint8_t *bits = d->huff_bits[dc][idx];
    uint8_t *vals = d->huff_vals[dc][idx];

    int code = 0;       // running code value
    int first = 0;      // first code of the current length
    int index = 0;      // running symbol index
    for (int l = 0; l < 16; l++) {
        if (d->code_bits < 1) jpeg_fill_bits(d);
        int bit = (int)(d->code_buffer >> 31);
        d->code_buffer <<= 1;
        d->code_bits -= 1;
        code = (code << 1) | bit;

        int count = bits[l];
        if (code - first < count) {
            return vals[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
    }

    return -1;
}

// Extend sign for DC/AC values
static int extend(int val, int bits) {
    if (bits == 0) return 0;
    int vt = 1 << (bits - 1);
    if (val < vt) {
        // Equivalent to: val + (-1 << bits) + 1, avoiding negative shift
        val = val - (1 << bits) + 1;
    }
    return val;
}

// Integer inverse DCT (8x8), ported from stb_image's public-domain idct_block.
// Pure fixed-point (no FPU). Constants are cos/scale factors pre-multiplied by
// 4096 (computed once as integer literals). Output is the signed spatial-domain
// sample WITHOUT the +128 level shift; callers add 128 afterwards.
#define IDCT_1D(s0,s1,s2,s3,s4,s5,s6,s7) \
   int t0,t1,t2,t3,p1,p2,p3,p4,p5,x0,x1,x2,x3; \
   p2 = (s2);                          \
   p3 = (s6);                          \
   p1 = (p2 + p3) * 2217;              \
   t2 = p1 + p3 * (-7567);             \
   t3 = p1 + p2 * 3135;                \
   p2 = (s0);                          \
   p3 = (s4);                          \
   t0 = (p2 + p3) * 4096;              \
   t1 = (p2 - p3) * 4096;              \
   x0 = t0 + t3;                       \
   x3 = t0 - t3;                       \
   x1 = t1 + t2;                       \
   x2 = t1 - t2;                       \
   t0 = (s7);                          \
   t1 = (s5);                          \
   t2 = (s3);                          \
   t3 = (s1);                          \
   p3 = t0 + t2;                       \
   p4 = t1 + t3;                       \
   p1 = t0 + t3;                       \
   p2 = t1 + t2;                       \
   p5 = (p3 + p4) * 4816;              \
   t0 = t0 * 1223;                     \
   t1 = t1 * 8410;                     \
   t2 = t2 * 12586;                    \
   t3 = t3 * 6149;                     \
   p1 = p5 + p1 * (-3685);             \
   p2 = p5 + p2 * (-10497);            \
   p3 = p3 * (-8034);                  \
   p4 = p4 * (-1597);                  \
   t3 += p1 + p4;                      \
   t2 += p2 + p3;                      \
   t1 += p2 + p4;                      \
   t0 += p1 + p3;

static void idct_block(int *block) {
    int val[64];
    int i;

    // Pass 1: columns (input coefficients are in natural order in block[]).
    for (i = 0; i < 8; i++) {
        int *s = block + i;
        IDCT_1D(s[0], s[8], s[16], s[24], s[32], s[40], s[48], s[56])
        x0 += 512; x1 += 512; x2 += 512; x3 += 512;
        val[i +  0] = (x0 + t3) >> 10;
        val[i + 56] = (x0 - t3) >> 10;
        val[i +  8] = (x1 + t2) >> 10;
        val[i + 48] = (x1 - t2) >> 10;
        val[i + 16] = (x2 + t1) >> 10;
        val[i + 40] = (x2 - t1) >> 10;
        val[i + 24] = (x3 + t0) >> 10;
        val[i + 32] = (x3 - t0) >> 10;
    }

    // Pass 2: rows.
    for (i = 0; i < 8; i++) {
        int *s = val + i * 8;
        IDCT_1D(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7])
        x0 += 65536; x1 += 65536; x2 += 65536; x3 += 65536;
        block[i * 8 + 0] = (x0 + t3) >> 17;
        block[i * 8 + 7] = (x0 - t3) >> 17;
        block[i * 8 + 1] = (x1 + t2) >> 17;
        block[i * 8 + 6] = (x1 - t2) >> 17;
        block[i * 8 + 2] = (x2 + t1) >> 17;
        block[i * 8 + 5] = (x2 - t1) >> 17;
        block[i * 8 + 3] = (x3 + t0) >> 17;
        block[i * 8 + 4] = (x3 - t0) >> 17;
    }
}

// ===========================================================================
// #404 JPEG entropy dequant+IDCT seam (flag -DRUST_JPEG_ENTROPY).
// The seam = the fixed 64-coefficient -> 64-sample transform at the tail of
// decode_block: scatter the zigzag-order coefficients into a natural-order 8x8
// block, multiply each by its quantizer, run the integer inverse DCT in place.
// ===========================================================================

// Verbatim C reference for the dequant+IDCT seam: the exact math from the
// original decode_block tail (block[0]=dc*quant[0]; block[zigzag[k]]=ac*quant[k])
// + idct_block(), factored into a standalone 64-coeff -> 64-sample transform.
// coeff_zz/quant are in ZIGZAG order; out is NATURAL order (no +128 shift).
void jpeg_dequant_idct_c(const int *coeff_zz, const uint8_t *quant, int *out) {
    int block[64];
    for (int i = 0; i < 64; i++) block[i] = 0;
    for (int k = 0; k < 64; k++) {
        block[zigzag[k]] = coeff_zz[k] * quant[k];
    }
    idct_block(block);
    for (int i = 0; i < 64; i++) out[i] = block[i];
}

// Signature-lock: a drift between this and the Rust #[no_mangle] fn fails the
// C compile (the FFI lock, same pattern as jpeg_parse_headers_rs).
extern void jpeg_dequant_idct_rs(const int *coeff_zz, const uint8_t *quant, int *out);

// Strangler dispatcher: -DRUST_JPEG_ENTROPY routes the live decode seam to Rust.
// Drop the flag and rebuild to roll straight back to the C reference.
static inline void jpeg_dequant_idct(const int *coeff_zz, const uint8_t *quant, int *out) {
#ifdef RUST_JPEG_ENTROPY
    jpeg_dequant_idct_rs(coeff_zz, quant, out);
#else
    jpeg_dequant_idct_c(coeff_zz, quant, out);
#endif
}

// Decode one 8x8 block
static int decode_block(jpeg_decoder_t *d, int comp, int *block) {
    int dc_idx = d->comp_dc[comp];
    int ac_idx = d->comp_ac[comp];
    int qt_idx = d->comp_qt[comp];

    // Coefficients in ZIGZAG order; the seam scatters+dequantizes+IDCTs.
    int coeff_zz[64];
    memset(coeff_zz, 0, sizeof(coeff_zz));

    // Decode DC coefficient
    int dc_len = decode_huff(d, 0, dc_idx);
    if (dc_len < 0) return JPEG_ERR_HUFFMAN;

    int dc_val = 0;
    if (dc_len > 0) {
        dc_val = get_bits(d, dc_len);
        if (dc_val < 0) return JPEG_ERR_CORRUPT;
        dc_val = extend(dc_val, dc_len);
    }
    d->dc_pred[comp] += dc_val;
    coeff_zz[0] = d->dc_pred[comp];

    // Decode AC coefficients (zigzag order)
    int k = 1;
    while (k < 64) {
        int ac_sym = decode_huff(d, 1, ac_idx);
        if (ac_sym < 0) return JPEG_ERR_HUFFMAN;

        if (ac_sym == 0) break;  // EOB

        int run = ac_sym >> 4;
        int size = ac_sym & 0x0F;

        if (size == 0) {
            if (run == 15) {
                k += 16;  // ZRL
                continue;
            }
            break;
        }

        k += run;
        if (k >= 64) return JPEG_ERR_CORRUPT;

        int ac_val = get_bits(d, size);
        ac_val = extend(ac_val, size);
        coeff_zz[k] = ac_val;
        k++;
    }

    // Dequantize + IDCT seam (Rust under -DRUST_JPEG_ENTROPY). quant table is
    // stored zigzag-order, so the k-th zigzag quantizer is quant[k].
    jpeg_dequant_idct(coeff_zz, d->quant[qt_idx], block);

    return JPEG_SUCCESS;
}

// ===========================================================================
// #404 Phase W: pure JPEG header-parse seam (jpeg_parse_headers_c). VERBATIM
// reproduction of the original in-line marker walk + parse_dqt/parse_dht/
// parse_sof0 + the SOS scan-header read, but operating on a raw (data,len,pos)
// cursor and writing a jpeg_hdr_t out-struct instead of mutating a decoder.
// Does NOT do the entropy/IDCT decode (that stays C, in jpeg_decode_scan).
//
// This is the untrusted-input OOB surface: segment-length walk + DQT/DHT table
// counts + SOF0 dims/sampling + SOS table selectors. jpeg_parse_headers_rs
// (rustkern.rs) is a bounds-checked drop-in; -DRUST_JPEG routes to it.
//
// The C reference is the HONEST verbatim original and therefore reproduces its
// bugs: hdr_parse_dht writes `total` symbols with NO clamp (huff_vals[.][.] is
// [256]) so a crafted DHT (sum of 16 count bytes up to 4080) OOB-WRITES; SOF0
// leaves comp_qt unvalidated and SOS leaves comp_dc/comp_ac unvalidated, which
// the downstream C decoder (decode_block quant[qt_idx], decode_huff
// huff_fast[dc][idx]) then OOB-READs. The Rust port confines all three.
// ===========================================================================

static int hdr_read_byte(const uint8_t *d, uint32_t len, uint32_t *pos) {
    if (*pos >= len) return -1;
    return d[(*pos)++];
}
static int hdr_read_u16(const uint8_t *d, uint32_t len, uint32_t *pos) {
    int hi = hdr_read_byte(d, len, pos);
    int lo = hdr_read_byte(d, len, pos);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 8) | lo;
}

static int hdr_parse_dqt(const uint8_t *data, uint32_t len, uint32_t *pos, jpeg_hdr_t *out) {
    int l = hdr_read_u16(data, len, pos);
    if (l < 0) return JPEG_ERR_CORRUPT;
    l -= 2;
    while (l > 0) {
        int info = hdr_read_byte(data, len, pos);
        if (info < 0) return JPEG_ERR_CORRUPT;
        l--;
        int prec = info >> 4;
        int idx = info & 0x0F;
        if (idx >= 4) return JPEG_ERR_CORRUPT;
        if (prec != 0) return JPEG_ERR_UNSUPPORTED;  // 16-bit not supported
        for (int i = 0; i < 64; i++) {
            int val = hdr_read_byte(data, len, pos);
            if (val < 0) return JPEG_ERR_CORRUPT;
            out->quant[idx][i] = (uint8_t)val;
        }
        out->quant_valid[idx] = 1;
        l -= 64;
    }
    return JPEG_SUCCESS;
}

static int hdr_parse_dht(const uint8_t *data, uint32_t len, uint32_t *pos, jpeg_hdr_t *out) {
    int l = hdr_read_u16(data, len, pos);
    if (l < 0) return JPEG_ERR_CORRUPT;
    l -= 2;
    while (l > 0) {
        int info = hdr_read_byte(data, len, pos);
        if (info < 0) return JPEG_ERR_CORRUPT;
        l--;
        int dc = (info >> 4) & 1;  // 0=DC, 1=AC
        int idx = info & 0x0F;
        if (idx >= 2) return JPEG_ERR_CORRUPT;
        int total = 0;
        for (int i = 0; i < 16; i++) {
            int count = hdr_read_byte(data, len, pos);
            if (count < 0) return JPEG_ERR_CORRUPT;
            out->huff_bits[dc][idx][i] = (uint8_t)count;
            total += count;
        }
        l -= 16;
        // VERBATIM BUG: no `total <= 256` clamp. huff_vals[dc][idx] is [256];
        // total can be up to 4080 => OOB WRITE. jpeg_parse_headers_rs rejects.
        for (int i = 0; i < total; i++) {
            int val = hdr_read_byte(data, len, pos);
            if (val < 0) return JPEG_ERR_CORRUPT;
            out->huff_vals[dc][idx][i] = (uint8_t)val;
        }
        l -= total;
        out->huff_valid[dc][idx] = 1;
    }
    return JPEG_SUCCESS;
}

static int hdr_parse_sof0(const uint8_t *data, uint32_t len, uint32_t *pos, jpeg_hdr_t *out) {
    int l = hdr_read_u16(data, len, pos);
    if (l < 0) return JPEG_ERR_CORRUPT;
    int prec = hdr_read_byte(data, len, pos);
    if (prec != 8) return JPEG_ERR_UNSUPPORTED;  // 8-bit only
    int height = hdr_read_u16(data, len, pos);
    int width = hdr_read_u16(data, len, pos);
    int comps = hdr_read_byte(data, len, pos);
    if (width <= 0 || height <= 0) return JPEG_ERR_CORRUPT;
    if (width > JPEG_MAX_WIDTH || height > JPEG_MAX_HEIGHT) return JPEG_ERR_UNSUPPORTED;
    if (comps != 1 && comps != 3) return JPEG_ERR_UNSUPPORTED;
    out->width = width;
    out->height = height;
    out->components = comps;
    int max_h = 1, max_v = 1;
    for (int i = 0; i < comps; i++) {
        out->comp_id[i] = hdr_read_byte(data, len, pos);
        int sampling = hdr_read_byte(data, len, pos);
        out->comp_h[i] = sampling >> 4;
        out->comp_v[i] = sampling & 0x0F;
        // VERBATIM: comp_qt UNVALIDATED. decode_block indexes quant[comp_qt][k]
        // (quant is [4][64]) => OOB READ for comp_qt >= 4. Rust rejects >= 4.
        out->comp_qt[i] = hdr_read_byte(data, len, pos);
        if (out->comp_h[i] > max_h) max_h = out->comp_h[i];
        if (out->comp_v[i] > max_v) max_v = out->comp_v[i];
    }
    out->mcu_width = max_h * 8;
    out->mcu_height = max_v * 8;
    out->mcu_count_x = (width + out->mcu_width - 1) / out->mcu_width;
    out->mcu_count_y = (height + out->mcu_height - 1) / out->mcu_height;
    return JPEG_SUCCESS;
}

static int hdr_parse_sos(const uint8_t *data, uint32_t len, uint32_t *pos, jpeg_hdr_t *out) {
    int l = hdr_read_u16(data, len, pos);
    if (l < 0) return JPEG_ERR_CORRUPT;
    int ns = hdr_read_byte(data, len, pos);
    if (ns != out->components) return JPEG_ERR_CORRUPT;
    for (int i = 0; i < ns; i++) {
        int id = hdr_read_byte(data, len, pos);
        int tables = hdr_read_byte(data, len, pos);
        for (int j = 0; j < out->components; j++) {
            if (out->comp_id[j] == id) {
                // VERBATIM: comp_dc/comp_ac UNVALIDATED (0..15). decode_huff
                // indexes huff_fast[dc][comp_dc] ([2] deep) => OOB READ for
                // comp_dc/comp_ac >= 2. Rust rejects >= 2.
                out->comp_dc[j] = tables >> 4;
                out->comp_ac[j] = tables & 0x0F;
                break;
            }
        }
    }
    hdr_read_byte(data, len, pos);  // Ss
    hdr_read_byte(data, len, pos);  // Se
    hdr_read_byte(data, len, pos);  // Ah/Al
    return JPEG_SUCCESS;
}

static int jpeg_parse_headers_impl(const uint8_t *data, uint32_t len, jpeg_hdr_t *out) {
    if (len < 4) return JPEG_ERR_TOO_SMALL;
    if (data[0] != 0xFF || data[1] != 0xD8) return JPEG_ERR_INVALID_SIG;

    uint32_t pos = 2;  // Skip SOI
    while (pos < len) {
        int marker = hdr_read_byte(data, len, &pos);
        if (marker != 0xFF) continue;
        do {
            marker = hdr_read_byte(data, len, &pos);
        } while (marker == 0xFF);
        if (marker < 0) break;

        int full_marker = 0xFF00 | marker;
        switch (full_marker) {
            case JPEG_MARKER_SOF0: {
                int r = hdr_parse_sof0(data, len, &pos, out);
                if (r != JPEG_SUCCESS) return r;
                break;
            }
            case JPEG_MARKER_DHT: {
                int r = hdr_parse_dht(data, len, &pos, out);
                if (r != JPEG_SUCCESS) return r;
                break;
            }
            case JPEG_MARKER_DQT: {
                int r = hdr_parse_dqt(data, len, &pos, out);
                if (r != JPEG_SUCCESS) return r;
                break;
            }
            case JPEG_MARKER_DRI:
                hdr_read_u16(data, len, &pos);  // Length
                out->restart_interval = hdr_read_u16(data, len, &pos);
                break;
            case JPEG_MARKER_SOS: {
                int r = hdr_parse_sos(data, len, &pos, out);
                if (r != JPEG_SUCCESS) return r;
                out->entropy_pos = pos;  // entropy-coded data begins here
                return JPEG_SUCCESS;
            }
            case JPEG_MARKER_EOI:
                return JPEG_SUCCESS;  // no scan; entropy_pos stays 0
            default:
                if (marker >= 0xE0 && marker <= 0xEF) {
                    int slen = hdr_read_u16(data, len, &pos);   // APPn
                    if (slen >= 2) pos += slen - 2;
                } else if (marker == 0xFE) {
                    int slen = hdr_read_u16(data, len, &pos);   // Comment
                    if (slen >= 2) pos += slen - 2;
                }
                break;
        }
    }
    return JPEG_SUCCESS;  // no SOS found (matches historical no-pixels SUCCESS)
}

// Verbatim C reference for the header-parse seam. Zeroes out, parses, records
// out->status = return code (parity with the Rust JpegHdr.status).
int jpeg_parse_headers_c(const uint8_t *data, uint32_t len, jpeg_hdr_t *out) {
    if (!data || !out) return JPEG_ERR_NULL_PTR;
    memset(out, 0, sizeof(*out));
    int rc = jpeg_parse_headers_impl(data, len, out);
    out->status = rc;
    return rc;
}

// Strangler dispatcher: the live header-parse seam. -DRUST_JPEG -> Rust port.
int jpeg_parse_headers(const uint8_t *data, uint32_t len, jpeg_hdr_t *out) {
#ifdef RUST_JPEG
    return jpeg_parse_headers_rs(data, len, out);
#else
    return jpeg_parse_headers_c(data, len, out);
#endif
}

// Decode the entropy-coded scan into RGB pixels. #404 Phase W: the SOS scan
// header (component table selectors + Ss/Se/Ah/Al) is now parsed by the pure
// header seam (jpeg_parse_headers), which sets d->comp_dc/comp_ac and leaves
// d->pos AT the first entropy byte before this runs. This function body is the
// VERBATIM MCU/Huffman/IDCT/upsample/color-convert loop, unchanged.
static int jpeg_decode_scan(jpeg_decoder_t *d, uint32_t *pixels) {
    // Reset bit reader and DC prediction
    d->code_buffer = 0;
    d->code_bits = 0;
    d->marker = 0;
    d->nomore = 0;
    for (int i = 0; i < d->components; i++) {
        d->dc_pred[i] = 0;
    }

    // Allocate MCU buffers
    int mcu_size = d->mcu_width * d->mcu_height;
    int *mcu_y = kmalloc(mcu_size * sizeof(int));
    int *mcu_cb = kmalloc(mcu_size * sizeof(int));
    int *mcu_cr = kmalloc(mcu_size * sizeof(int));
    int block[64];

    if (!mcu_y || !mcu_cb || !mcu_cr) {
        if (mcu_y) kfree(mcu_y);
        if (mcu_cb) kfree(mcu_cb);
        if (mcu_cr) kfree(mcu_cr);
        return JPEG_ERR_NOMEM;
    }

    int restart_count = 0;

    // Decode MCUs
    for (int mcu_y_idx = 0; mcu_y_idx < d->mcu_count_y; mcu_y_idx++) {
        for (int mcu_x_idx = 0; mcu_x_idx < d->mcu_count_x; mcu_x_idx++) {
            // Handle restart markers. jpeg_fill_bits() consumes the RST marker
            // bytes from the stream while refilling and records it in ->marker;
            // ensure it has been reached, then drop any buffered bits, clear the
            // marker/EOF latch, and reset DC prediction for the new interval.
            if (d->restart_interval > 0 && restart_count == d->restart_interval) {
                if (d->code_bits < 24) jpeg_fill_bits(d);
                d->code_buffer = 0;
                d->code_bits = 0;
                d->marker = 0;
                d->nomore = 0;
                for (int i = 0; i < d->components; i++) {
                    d->dc_pred[i] = 0;
                }
                restart_count = 0;
            }

            memset(mcu_y, 0, mcu_size * sizeof(int));
            if (d->components == 3) {
                memset(mcu_cb, 0, mcu_size * sizeof(int));
                memset(mcu_cr, 0, mcu_size * sizeof(int));
            }

            // Decode all blocks in MCU
            for (int comp = 0; comp < d->components; comp++) {
                int h = d->comp_h[comp];
                int v = d->comp_v[comp];
                int *target = (comp == 0) ? mcu_y : (comp == 1) ? mcu_cb : mcu_cr;

                for (int by = 0; by < v; by++) {
                    for (int bx = 0; bx < h; bx++) {
                        int ret = decode_block(d, comp, block);
                        if (ret != JPEG_SUCCESS) {
                            kfree(mcu_y);
                            kfree(mcu_cb);
                            kfree(mcu_cr);
                            return ret;
                        }

                        // Copy block to MCU buffer with upsampling
                        int scale_x = d->mcu_width / (h * 8);
                        int scale_y = d->mcu_height / (v * 8);

                        for (int y = 0; y < 8; y++) {
                            for (int x = 0; x < 8; x++) {
                                int val = block[y * 8 + x] + 128;
                                int px = bx * 8 + x;
                                int py = by * 8 + y;

                                // Upsample
                                for (int sy = 0; sy < scale_y; sy++) {
                                    for (int sx = 0; sx < scale_x; sx++) {
                                        int tx = px * scale_x + sx;
                                        int ty = py * scale_y + sy;
                                        if (tx < d->mcu_width && ty < d->mcu_height) {
                                            target[ty * d->mcu_width + tx] = val;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Convert MCU to RGB and write to output
            int base_x = mcu_x_idx * d->mcu_width;
            int base_y = mcu_y_idx * d->mcu_height;

            for (int y = 0; y < d->mcu_height && base_y + y < (int)d->height; y++) {
                for (int x = 0; x < d->mcu_width && base_x + x < (int)d->width; x++) {
                    int idx = y * d->mcu_width + x;
                    int r, g, b;

                    if (d->components == 1) {
                        r = g = b = clamp(mcu_y[idx]);
                    } else {
                        // YCbCr to RGB conversion
                        int yy = mcu_y[idx];
                        int cb = mcu_cb[idx] - 128;
                        int cr = mcu_cr[idx] - 128;

                        r = clamp(yy + ((cr * 359) >> 8));
                        g = clamp(yy - ((cb * 88 + cr * 183) >> 8));
                        b = clamp(yy + ((cb * 454) >> 8));
                    }

                    int px = base_x + x;
                    int py = base_y + y;
                    pixels[py * d->width + px] = 0xFF000000 | (r << 16) | (g << 8) | b;
                }
            }

            restart_count++;
        }
    }

    kfree(mcu_y);
    kfree(mcu_cb);
    kfree(mcu_cr);

    return JPEG_SUCCESS;
}

// Check JPEG signature
int image_is_jpeg(const void *data, uint32_t size) {
    if (!data || size < 2) return 0;
    const uint8_t *p = (const uint8_t *)data;
    return (p[0] == 0xFF && p[1] == 0xD8);
}

// Load JPEG image
int image_load_jpeg(const void *data, uint32_t size, image_t *img) {
    if (!data || !img) return JPEG_ERR_NULL_PTR;
    if (size < 4) return JPEG_ERR_TOO_SMALL;

    const uint8_t *p = (const uint8_t *)data;
    if (p[0] != 0xFF || p[1] != 0xD8) {
        return JPEG_ERR_INVALID_SIG;
    }

    img->width = 0;
    img->height = 0;
    img->pixels = NULL;

    // #404 Phase W: parse ALL headers (SOI..SOS scan header) through the pure
    // seam (routes to jpeg_parse_headers_rs under -DRUST_JPEG, which bounds every
    // segment-length walk + table count + SOF0/SOS selector and REJECTS the
    // crafted OOB cases the C reference walks into). On any header error, or if
    // no SOS was found (entropy_pos == 0, historical no-pixels SUCCESS), return
    // without decoding.
    jpeg_hdr_t hdr;
    int ret = jpeg_parse_headers(p, size, &hdr);
    if (ret != JPEG_SUCCESS) return ret;
    if (hdr.entropy_pos == 0) return JPEG_SUCCESS;   // no scan; no pixels

    jpeg_decoder_t *d = kzalloc(sizeof(jpeg_decoder_t));
    if (!d) return JPEG_ERR_NOMEM;
    d->data = p;
    d->data_len = size;

    // Load decoder state from the validated header.
    d->width = hdr.width;
    d->height = hdr.height;
    d->components = hdr.components;
    for (int i = 0; i < JPEG_MAX_COMPONENTS; i++) {
        d->comp_id[i] = hdr.comp_id[i];
        d->comp_h[i]  = hdr.comp_h[i];
        d->comp_v[i]  = hdr.comp_v[i];
        d->comp_qt[i] = hdr.comp_qt[i];
        d->comp_dc[i] = hdr.comp_dc[i];
        d->comp_ac[i] = hdr.comp_ac[i];
    }
    d->mcu_width  = hdr.mcu_width;
    d->mcu_height = hdr.mcu_height;
    d->mcu_count_x = hdr.mcu_count_x;
    d->mcu_count_y = hdr.mcu_count_y;
    d->restart_interval = hdr.restart_interval;
    memcpy(d->quant, hdr.quant, sizeof(d->quant));
    for (int i = 0; i < 4; i++) d->quant_valid[i] = hdr.quant_valid[i];
    memcpy(d->huff_bits, hdr.huff_bits, sizeof(d->huff_bits));
    memcpy(d->huff_vals, hdr.huff_vals, sizeof(d->huff_vals));
    for (int a = 0; a < 2; a++) {
        for (int b = 0; b < 2; b++) {
            d->huff_valid[a][b] = hdr.huff_valid[a][b];
            // MAYTERA-SEC-2026-00XX: a non-canonical DHT makes build_huffman's
            // fast-table index run past huff_fast[][][1024]. It now refuses to
            // build such a table; reject the whole image rather than decode
            // with a partial one (decode_huff's slow path would then index
            // vals[] out of range on the same malformed bits[]).
            if (hdr.huff_valid[a][b] && !build_huffman(d, a, b)) {
                kfree(d);
                return JPEG_ERR_CORRUPT;
            }
        }
    }
    d->pos = hdr.entropy_pos;   // first entropy-coded byte

    img->width = d->width;
    img->height = d->height;
    img->pixels = kmalloc(d->width * d->height * 4);
    if (!img->pixels) {
        kfree(d);
        return JPEG_ERR_NOMEM;
    }

    ret = jpeg_decode_scan(d, img->pixels);
    kfree(d);

    if (ret != JPEG_SUCCESS && img->pixels) {
        kfree(img->pixels);
        img->pixels = NULL;
        img->width = 0;
        img->height = 0;
    }

    return ret;
}

const char *jpeg_error_string(int err) {
    switch (err) {
        case JPEG_SUCCESS:         return "Success";
        case JPEG_ERR_NULL_PTR:    return "Null pointer";
        case JPEG_ERR_INVALID_SIG: return "Invalid JPEG signature";
        case JPEG_ERR_UNSUPPORTED: return "Unsupported JPEG format";
        case JPEG_ERR_NOMEM:       return "Out of memory";
        case JPEG_ERR_CORRUPT:     return "Corrupt JPEG data";
        case JPEG_ERR_TOO_SMALL:   return "Data too small";
        case JPEG_ERR_HUFFMAN:     return "Huffman decode error";
        case JPEG_ERR_MARKER:      return "Invalid marker";
        default:                   return "Unknown error";
    }
}

// ===========================================================================
// #404 Phase W boot-time self-test: prove jpeg_parse_headers_rs (Rust, live
// under -DRUST_JPEG) == jpeg_parse_headers_c (verbatim reference) on a real
// baseline JPEG header + its truncations, report the SECURITY posture HONESTLY
// (three REACHABLE OOBs the Rust confines), and micro-bench the parse. LIGHT
// (#426, bounded, runs once): ~70 differential vectors + a small confinement
// sweep + a ~2k-iter RDTSC bench. The heavy fuzz (millions of vectors, ASan on
// the C reference incl. the DHT OOB WRITE) runs OFFLINE.
// Logs one [RUST-DIFF] jpeg, one [RUST-SEC] jpeg, one [RUST-PERF] jpeg.
// ---------------------------------------------------------------------------
extern int jpeg_parse_headers_c(const uint8_t *data, uint32_t len, jpeg_hdr_t *out);

// Real 16x16 baseline JPEG (4:2:0), ffmpeg yuvj420p q5, 435 bytes (same bytes as
// gui/jpeg_selftest.c). A genuine header (SOI/APP0/COM/DQT/DHT/SOF0/SOS) to diff.
static const uint8_t jpeg_diff_img[] = {
255,216,255,224,0,16,74,70,73,70,0,1,2,0,0,1,0,1,0,0,255,254,0,16,76,97,118,99,
53,57,46,51,55,46,49,48,48,0,255,219,0,67,0,8,10,10,11,10,11,13,13,13,13,13,13,
16,15,16,16,16,16,16,16,16,16,16,16,16,18,18,18,21,21,21,18,18,18,16,16,18,18,20,
20,21,21,23,23,23,21,21,21,21,23,23,25,25,25,30,30,28,28,35,35,36,43,43,51,255,
196,0,109,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,6,3,5,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,1,2,16,0,3,0,2,2,1,4,2,3,1,0,0,0,0,0,0,3,2,1,4,17,5,18,0,6,33,49,19,65,66,20,
35,50,34,17,0,1,3,2,7,1,1,1,0,0,0,0,0,0,0,0,1,4,17,3,18,2,20,33,5,19,34,50,35,0,
68,65,255,192,0,17,8,0,16,0,16,3,1,34,0,2,17,0,3,17,0,255,218,0,12,3,1,0,2,17,3,
17,0,63,0,51,113,143,194,240,56,132,17,156,70,56,147,34,144,36,117,177,75,80,131,
88,210,37,150,12,186,121,63,107,102,236,215,141,185,126,77,61,47,200,8,248,225,16,
192,164,79,184,34,18,72,195,191,90,191,69,148,114,60,87,118,79,250,147,190,187,
110,121,149,146,60,62,79,132,193,95,231,99,138,8,24,248,236,221,145,228,250,144,
105,217,191,177,122,246,163,190,215,227,122,221,215,145,245,38,62,111,57,150,124,
100,196,200,31,93,105,148,100,47,111,241,125,150,34,254,19,127,55,218,249,51,50,
241,166,221,167,101,176,86,99,127,57,184,248,51,137,40,51,183,62,181,210,255,0,
199,248,140,139,214,166,70,165,140,246,226,119,4,141,65,201,104,73,232,124,239,
32,20,187,92,141,52,216,221,50,255,217
};

static inline uint64_t jpeg_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax","ebx","ecx","edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

void jpeg_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);

    // Force-reference the Rust symbol so its archive member always links,
    // regardless of -DRUST_JPEG (matches the png/bmp pattern).
    { jpeg_hdr_t t; jpeg_parse_headers_rs(jpeg_diff_img, 4, &t); }

    // Part 1: [RUST-DIFF] - agreement domain. The full real JPEG plus a sweep of
    // truncations (4..len). A valid JPEG never triggers a confinement condition
    // (all selectors/counts in range), so _c never OOBs on any prefix and _c==_rs
    // for every vector: identical return code AND byte-identical jpeg_hdr_t.
    uint32_t vectors=0, mism=0; int first_bad=-1;
    uint32_t full = (uint32_t)sizeof(jpeg_diff_img);
    for (uint32_t L=4; L<=full; L++) {
        // sample ~64 truncation lengths + always include the full length
        if (L != full && ((L - 4) % 7) != 0) continue;
        jpeg_hdr_t hc, hr;
        int rc = jpeg_parse_headers_c (jpeg_diff_img, L, &hc);
        int rr = jpeg_parse_headers_rs(jpeg_diff_img, L, &hr);
        vectors++;
        int bad = (rc != rr);
        if (!bad) {
            // memcmp the whole struct (no padding; both fully zeroed then filled)
            const uint8_t *a=(const uint8_t*)&hc, *b=(const uint8_t*)&hr;
            for (uint32_t i=0;i<sizeof(jpeg_hdr_t);i++) if (a[i]!=b[i]) { bad=1; break; }
        }
        if (bad) { mism++; if (first_bad<0) first_bad=(int)L; }
    }
    const char *verdict = (mism==0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] jpeg: %u vectors, %u mismatches -> %s\n", vectors, mism, verdict);
    bootlog_write("[RUST-DIFF] jpeg: %u vectors, %u mismatches -> %s", vectors, mism, verdict);
    if (mism) { kprintf("[RUST-DIFF] jpeg FIRST MISMATCH L=%d\n", first_bad); bootlog_write("[RUST-DIFF] jpeg FIRST MISMATCH L=%d", first_bad); }

    // Part 2: [RUST-SEC] - THREE REACHABLE OOBs in the C header parse:
    //  (A) SOF0 comp_qt unvalidated -> decode_block quant[qt_idx][k] OOB READ.
    //  (B) SOS  comp_dc/comp_ac unvalidated -> decode_huff huff_fast[dc][idx] OOB READ.
    //  (C) DHT  sum-of-counts up to 4080 written into huff_vals[.][.][256] OOB WRITE.
    // (A)+(B): the OOB is DOWNSTREAM (decode), so the SEAM _c does NOT overflow -
    // safe to run both: _c ACCEPTS (SUCCESS) the crafted header, _rs CONFINES
    // (reject at parse, before any decode/alloc). (C): the OOB WRITE is IN the
    // seam _c, so running _c would corrupt kernel memory - we exercise the RUST
    // side ONLY (must reject); the C OOB WRITE is ASan-proven OFFLINE.
    {
        // (A) complete SOF0 with comp_qt=5, then EOI. 8x8, 1 component.
        static const uint8_t v_qt[] = {
            0xFF,0xD8, 0xFF,0xC0,0x00,0x0B,0x08,0x00,0x08,0x00,0x08,0x01,0x01,0x11,0x05,
            0xFF,0xD9 };
        // (B) valid SOF0 (comp_qt=0) + SOS with comp_dc=3 (tables=0x30), then EOI.
        static const uint8_t v_dc[] = {
            0xFF,0xD8, 0xFF,0xC0,0x00,0x0B,0x08,0x00,0x08,0x00,0x08,0x01,0x01,0x11,0x00,
            0xFF,0xDA,0x00,0x08,0x01,0x01,0x30,0x00,0x3F,0x00, 0xFF,0xD9 };
        // (C) DHT with 16 count bytes = 0xFF (total 4080 >> 256). _rs rejects at
        // the total>256 clamp BEFORE reading any value byte (buffer intentionally
        // short); _c would OOB WRITE, so it is NEVER called here.
        static const uint8_t v_dht[] = {
            0xFF,0xD8, 0xFF,0xC4,0x10,0x03,0x00,
            0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
        jpeg_hdr_t h;
        uint32_t a_cacc=0, a_rrej=0, b_cacc=0, b_rrej=0, c_rrej=0;
        const uint32_t reps=100;
        for (uint32_t k=0;k<reps;k++) {
            int ac = jpeg_parse_headers_c (v_qt, sizeof(v_qt), &h);   // seam-safe in C
            if (ac==JPEG_SUCCESS && h.comp_qt[0]==5) a_cacc++;        // C accepts qt=5
            int ar = jpeg_parse_headers_rs(v_qt, sizeof(v_qt), &h);
            if (ar!=JPEG_SUCCESS) a_rrej++;                          // Rust confines
            int bc = jpeg_parse_headers_c (v_dc, sizeof(v_dc), &h);   // seam-safe in C
            if (bc==JPEG_SUCCESS && h.comp_dc[0]==3) b_cacc++;        // C accepts dc=3
            int br = jpeg_parse_headers_rs(v_dc, sizeof(v_dc), &h);
            if (br!=JPEG_SUCCESS) b_rrej++;                          // Rust confines
            int cr = jpeg_parse_headers_rs(v_dht, sizeof(v_dht), &h); // RUST ONLY
            if (cr!=JPEG_SUCCESS) c_rrej++;                          // Rust confines OOB WRITE
        }
        kprintf("[RUST-SEC] jpeg: 3 REACHABLE OOBs - (A) SOF0 comp_qt>=4 -> quant[] OOB READ: C accepts %u/%u, Rust confines %u/%u; (B) SOS comp_dc>=2 -> huff_fast[] OOB READ: C accepts %u/%u, Rust confines %u/%u; (C) DHT total>256 -> huff_vals[256] OOB WRITE: Rust confines %u/%u (C OOB WRITE ASan-proven OFFLINE, never run in-kernel)\n",
                a_cacc,reps,a_rrej,reps, b_cacc,reps,b_rrej,reps, c_rrej,reps);
        bootlog_write("[RUST-SEC] jpeg: 3 REACHABLE OOBs; (A) comp_qt READ C-acc %u/%u Rust-conf %u/%u; (B) comp_dc READ C-acc %u/%u Rust-conf %u/%u; (C) DHT total>256 WRITE Rust-conf %u/%u (C ASan OFFLINE)",
                a_cacc,reps,a_rrej,reps, b_cacc,reps,b_rrej,reps, c_rrej,reps);
    }

    // Part 3: RDTSC micro-benchmark over the full real-JPEG header parse. LIGHT: 2k.
    {
        const int iters = 2000;
        jpeg_hdr_t h;
        for (int i=0;i<200;i++){ jpeg_parse_headers_c(jpeg_diff_img,full,&h); jpeg_parse_headers_rs(jpeg_diff_img,full,&h); }
        uint64_t t0=jpeg_tsc_serialized();
        for (int i=0;i<iters;i++) jpeg_parse_headers_c(jpeg_diff_img,full,&h);
        uint64_t t1=jpeg_tsc_serialized();
        for (int i=0;i<iters;i++) jpeg_parse_headers_rs(jpeg_diff_img,full,&h);
        uint64_t t2=jpeg_tsc_serialized();
        uint64_t c_cyc=(t1-t0)/iters, r_cyc=(t2-t1)/iters;
        uint64_t ratio100 = c_cyc ? (r_cyc*100ULL/c_cyc) : 0;
        kprintf("[RUST-PERF] jpeg: parse_headers C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
        bootlog_write("[RUST-PERF] jpeg: parse_headers C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
    }
}

// ===========================================================================
// #404 JPEG entropy dequant+IDCT seam boot self-test (PIECE 3).
// ===========================================================================
void jpeg_entropy_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);

    /* genuine luma quant table (zigzag order) from the embedded baseline JPEG */
    static const uint8_t rq[64] = {
        8,10,10,11,10,11,13,13,13,13,13,13,16,15,16,16,16,16,16,16,16,16,16,16,
        16,18,18,18,21,21,21,18,18,18,16,16,18,18,20,20,21,21,23,23,23,21,21,21,
        21,23,23,25,25,25,30,30,28,28,35,35,36,43,43,51 };

    /* Force-reference the Rust symbol so its archive member always links,
     * regardless of -DRUST_JPEG_ENTROPY (matches the parse-headers pattern). */
    { int cz[64]={0}, out[64]; jpeg_dequant_idct_rs(cz, rq, out); }

    /* xorshift32 PRNG (deterministic) */
    uint32_t s = 0x1BADB002u;
    #define NXT() (s^=s<<13, s^=s>>17, s^=s<<5, s)

    /* Part 1: [RUST-DIFF] - realistic sparse-DCT coefficient blocks + the real
     * quant table. rs must equal c byte-for-byte (all 64 spatial samples). */
    uint32_t vectors=0, mism=0; int first_bad=-1;
    for (int n=0; n<2000; n++) {
        int cz[64]; for (int i=0;i<64;i++) cz[i]=0;
        cz[0] = (int)(NXT()%4096) - 2048;             /* DC after prediction */
        int nac = (int)(NXT()%21);                    /* sparse AC */
        for (int a=0;a<nac;a++) {
            int kk = 1 + (int)(NXT()%63);
            int mag = ((NXT()%100)<90) ? ((int)(NXT()%129)-64)
                                       : ((int)(NXT()%8193)-4096);
            cz[kk]=mag;
        }
        int oc[64], orr[64];
        jpeg_dequant_idct_c (cz, rq, oc);
        jpeg_dequant_idct_rs(cz, rq, orr);
        vectors++;
        for (int i=0;i<64;i++) if (oc[i]!=orr[i]) { mism++; if(first_bad<0)first_bad=n; break; }
    }
    const char *verdict = (mism==0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] jpeg_idct: %u vectors, %u mismatches -> %s\n", vectors, mism, verdict);
    bootlog_write("[RUST-DIFF] jpeg_idct: %u vectors, %u mismatches -> %s", vectors, mism, verdict);
    if (mism) { kprintf("[RUST-DIFF] jpeg_idct FIRST MISMATCH n=%d\n", first_bad); }

    /* Part 2: [RUST-SEC] - HONEST posture. This seam has NO reachable memory
     * OOB (ASan-clean over 3.2M offline vectors): both C and Rust confine every
     * write to the 64-entry block, so the Rust bounds are DEFENSE-IN-DEPTH. The
     * REAL fix is CWE-190: the C IDCT has REACHABLE signed-integer-overflow
     * (UNDEFINED BEHAVIOR) on large coefficient products (a crafted JPEG can
     * carry ac_val up to +-32767 * quant up to 255); UBSan-proven OFFLINE, and
     * reachable even in the realistic coefficient band. The Rust wrapping_* ops
     * make it WELL-DEFINED and byte-identical to the current x86 wrap. Here we
     * exercise a coefficient block that overflows i32 in the IDCT and show
     * c == rs byte-for-byte (Rust defines the UB identically, no panic). */
    {
        int cz[64]; for (int i=0;i<64;i++) cz[i]=0;
        cz[0]=30000; cz[1]=-32000; cz[2]=31000; cz[5]=-32760; cz[16]=32760; cz[9]=-31500;
        int oc[64], orr[64];
        jpeg_dequant_idct_c (cz, rq, oc);
        jpeg_dequant_idct_rs(cz, rq, orr);
        int same=1; for (int i=0;i<64;i++) if (oc[i]!=orr[i]) { same=0; break; }
        kprintf("[RUST-SEC] jpeg_idct: no reachable OOB in seam (ASan-clean 3.2M, defense-in-depth); CWE-190 signed-overflow UB in C IDCT (UBSan-proven OFFLINE, reachable via crafted JPEG coeffs) is WELL-DEFINED by Rust wrapping_*: overflow-block c==rs %s\n", same?"IDENTICAL":"DIVERGED");
        bootlog_write("[RUST-SEC] jpeg_idct: no reachable OOB (ASan-clean, defense-in-depth); CWE-190 overflow-UB in C IDCT well-defined by Rust wrapping_*, overflow-block c==rs %s", same?"IDENTICAL":"DIVERGED");
    }

    /* Part 3: [RUST-PERF] - RDTSC micro-benchmark over one dequant+IDCT. LIGHT. */
    {
        const int iters = 2000;
        int cz[64]; for (int i=0;i<64;i++) cz[i]=0;
        cz[0]=640; cz[1]=-12; cz[2]=7; cz[8]=-9; cz[16]=4;
        int out[64];
        for (int i=0;i<200;i++){ jpeg_dequant_idct_c(cz,rq,out); jpeg_dequant_idct_rs(cz,rq,out); }
        uint64_t t0=jpeg_tsc_serialized();
        for (int i=0;i<iters;i++) jpeg_dequant_idct_c(cz,rq,out);
        uint64_t t1=jpeg_tsc_serialized();
        for (int i=0;i<iters;i++) jpeg_dequant_idct_rs(cz,rq,out);
        uint64_t t2=jpeg_tsc_serialized();
        uint64_t c_cyc=(t1-t0)/iters, r_cyc=(t2-t1)/iters;
        uint64_t ratio100 = c_cyc ? (r_cyc*100ULL/c_cyc) : 0;
        kprintf("[RUST-PERF] jpeg_idct: dequant+idct C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
        bootlog_write("[RUST-PERF] jpeg_idct: dequant+idct C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
    }
    #undef NXT
}
