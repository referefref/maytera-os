// jpeg.c - JPEG Image Decoder for MayteraOS
// Full baseline JPEG decoder

#include "jpeg.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"

// Debug macro
#define JPEG_DEBUG 0
#if JPEG_DEBUG
#define JPEG_DBG(fmt, ...) kprintf("[JPEG] " fmt, ##__VA_ARGS__)
#else
#define JPEG_DBG(fmt, ...) ((void)0)
#endif

// ============================================================================
// Decoder State Structure
// ============================================================================

typedef struct {
    // Quantization tables (up to 4)
    uint8_t quant[4][64];
    int quant_valid[4];

    // Huffman tables: [type][id]
    // type 0 = DC, type 1 = AC
    // id 0 = luminance, id 1 = chrominance
    uint8_t huff_bits[2][4][16];     // Number of codes per length
    uint8_t huff_vals[2][4][256];    // Symbol values
    int huff_valid[2][4];

    // Fast Huffman lookup tables
    int16_t huff_fast[2][4][1 << 10];
    int huff_maxcode[2][4][17];
    int huff_valptr[2][4][17];

    // Image info
    uint32_t width, height;
    int precision;
    int components;
    
    // Per-component info
    struct {
        int id;
        int h_sample;
        int v_sample;
        int quant_idx;
        int dc_tbl;
        int ac_tbl;
    } comp[JPEG_MAX_COMPONENTS];

    // MCU dimensions
    int mcu_width, mcu_height;
    int mcu_count_x, mcu_count_y;
    int max_h_sample, max_v_sample;
    int restart_interval;

    // Bit reader state
    const uint8_t *data;
    uint32_t data_len;
    uint32_t pos;
    uint32_t bits;
    int bits_left;

    // DC prediction for each component
    int dc_pred[JPEG_MAX_COMPONENTS];

    // Output pixels
    uint32_t *pixels;
} jpeg_decoder_t;

// ============================================================================
// Zigzag Ordering
// ============================================================================

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

// ============================================================================
// Utility Functions
// ============================================================================

static inline int clamp(int x) {
    if (x < 0) return 0;
    if (x > 255) return 255;
    return x;
}

static inline int read_byte(jpeg_decoder_t *d) {
    if (d->pos >= d->data_len) return -1;
    return d->data[d->pos++];
}

static inline int read_u16(jpeg_decoder_t *d) {
    int hi = read_byte(d);
    int lo = read_byte(d);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 8) | lo;
}

// ============================================================================
// Bit Reader
// ============================================================================

static int get_bit(jpeg_decoder_t *d) {
    if (d->bits_left == 0) {
        int c = read_byte(d);
        if (c < 0) return -1;
        
        if (c == 0xFF) {
            int c2 = read_byte(d);
            if (c2 != 0) {
                // Found marker, back up
                d->pos -= 2;
                return -1;
            }
            // 0xFF00 = stuffed 0xFF byte
        }
        d->bits = c;
        d->bits_left = 8;
    }
    d->bits_left--;
    return (d->bits >> d->bits_left) & 1;
}

static int get_bits(jpeg_decoder_t *d, int n) {
    int val = 0;
    for (int i = 0; i < n; i++) {
        int bit = get_bit(d);
        if (bit < 0) return -1;
        val = (val << 1) | bit;
    }
    return val;
}

static int peek_bits(jpeg_decoder_t *d, int n) {
    // Save state
    uint32_t saved_pos = d->pos;
    uint32_t saved_bits = d->bits;
    int saved_left = d->bits_left;
    
    int val = get_bits(d, n);
    
    // Restore state
    d->pos = saved_pos;
    d->bits = saved_bits;
    d->bits_left = saved_left;
    
    return val;
}

static void skip_bits(jpeg_decoder_t *d, int n) {
    for (int i = 0; i < n; i++) {
        get_bit(d);
    }
}

// ============================================================================
// Huffman Decoding
// ============================================================================

static void build_huffman(jpeg_decoder_t *d, int type, int id) {
    uint8_t *bits = d->huff_bits[type][id];
    uint8_t *vals = d->huff_vals[type][id];
    
    // Build maxcode and valptr tables
    int code = 0;
    int k = 0;
    
    for (int i = 0; i < 16; i++) {
        d->huff_valptr[type][id][i + 1] = k;
        for (int j = 0; j < bits[i]; j++) {
            k++;
        }
        if (bits[i]) {
            d->huff_maxcode[type][id][i + 1] = code + bits[i] - 1;
        } else {
            d->huff_maxcode[type][id][i + 1] = -1;
        }
        code = (code + bits[i]) << 1;
    }
    
    // Build fast lookup table (up to 10 bits)
    memset(d->huff_fast[type][id], -1, sizeof(d->huff_fast[type][id]));
    
    code = 0;
    k = 0;
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < bits[i]; j++) {
            int len = i + 1;
            if (len <= 10) {
                int fill = 1 << (10 - len);
                for (int f = 0; f < fill; f++) {
                    int idx = (code << (10 - len)) | f;
                    d->huff_fast[type][id][idx] = (len << 8) | vals[k];
                }
            }
            code++;
            k++;
        }
        code <<= 1;
    }
}

static int decode_huffman(jpeg_decoder_t *d, int type, int id) {
    // Try fast lookup
    int peek = peek_bits(d, 10);
    if (peek < 0) return -1;
    
    int16_t entry = d->huff_fast[type][id][peek];
    if (entry >= 0) {
        int len = entry >> 8;
        int val = entry & 0xFF;
        skip_bits(d, len);
        return val;
    }
    
    // Slow decode for longer codes
    int code = 0;
    for (int i = 1; i <= 16; i++) {
        int bit = get_bit(d);
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        
        if (code <= d->huff_maxcode[type][id][i]) {
            int j = d->huff_valptr[type][id][i] + code - 
                    (d->huff_maxcode[type][id][i] - d->huff_bits[type][id][i-1] + 1);
            return d->huff_vals[type][id][j];
        }
    }
    
    return -1;
}

// Sign extend value based on number of bits
static int extend(int val, int bits) {
    if (bits == 0) return 0;
    int vt = 1 << (bits - 1);
    if (val < vt) {
        val = val - (1 << bits) + 1;
    }
    return val;
}

// ============================================================================
// Inverse Discrete Cosine Transform (IDCT)
// ============================================================================

// Fixed-point IDCT constants (scaled by 4096)
#define FIX_0_298631336  ((int)  2446)
#define FIX_0_390180644  ((int)  3196)
#define FIX_0_541196100  ((int)  4433)
#define FIX_0_765366865  ((int)  6270)
#define FIX_0_899976223  ((int)  7373)
#define FIX_1_175875602  ((int)  9633)
#define FIX_1_501321110  ((int) 12299)
#define FIX_1_847759065  ((int) 15137)
#define FIX_1_961570560  ((int) 16069)
#define FIX_2_053119869  ((int) 16819)
#define FIX_2_562915447  ((int) 20995)
#define FIX_3_072711026  ((int) 25172)


#define CONST_BITS  13
#define PASS1_BITS  2

#define DESCALE(x, n)  (((x) + (1 << ((n)-1))) >> (n))

#define FIX_1_414213562 ((int) 362)
#define FIX_1_082392200 ((int) 277)
#define FIX_2_613125930 ((int) 669)

// Simplified IDCT for performance
static void idct_block(int *block) {
    int tmp[64];
    
    // Row IDCT
    for (int i = 0; i < 8; i++) {
        int *row = block + i * 8;
        
        // Check for all-zero AC coefficients
        if (row[1] == 0 && row[2] == 0 && row[3] == 0 &&
            row[4] == 0 && row[5] == 0 && row[6] == 0 && row[7] == 0) {
            int dc = row[0] << PASS1_BITS;
            for (int j = 0; j < 8; j++) {
                tmp[i * 8 + j] = dc;
            }
            continue;
        }
        
        int s0 = row[0], s1 = row[1], s2 = row[2], s3 = row[3];
        int s4 = row[4], s5 = row[5], s6 = row[6], s7 = row[7];
        
        // Even part
        int t0 = (s0 + s4) << CONST_BITS;
        int t1 = (s0 - s4) << CONST_BITS;
        int t2 = s2 * FIX_0_541196100 - s6 * FIX_1_847759065;
        int t3 = s2 * FIX_1_847759065 + s6 * FIX_0_541196100;
        
        int e0 = t0 + t3;
        int e1 = t1 + t2;
        int e2 = t1 - t2;
        int e3 = t0 - t3;
        
        // Odd part
        int z1 = s7 + s1;
        int z2 = s5 + s3;
        int z3 = s7 + s3;
        int z4 = s5 + s1;
        int z5 = (z3 + z4) * FIX_1_175875602;
        
        int o0 = s7 * FIX_0_298631336;
        int o1 = s5 * FIX_2_053119869;
        int o2 = s3 * FIX_3_072711026;
        int o3 = s1 * FIX_1_501321110;
        
        z1 = z1 * -FIX_0_899976223;
        z2 = z2 * -FIX_2_562915447;
        z3 = z3 * -FIX_1_961570560;
        z4 = z4 * -FIX_0_390180644;
        
        z3 += z5;
        z4 += z5;
        
        o0 += z1 + z3;
        o1 += z2 + z4;
        o2 += z2 + z3;
        o3 += z1 + z4;
        
        tmp[i * 8 + 0] = DESCALE(e0 + o3, CONST_BITS - PASS1_BITS);
        tmp[i * 8 + 7] = DESCALE(e0 - o3, CONST_BITS - PASS1_BITS);
        tmp[i * 8 + 1] = DESCALE(e1 + o2, CONST_BITS - PASS1_BITS);
        tmp[i * 8 + 6] = DESCALE(e1 - o2, CONST_BITS - PASS1_BITS);
        tmp[i * 8 + 2] = DESCALE(e2 + o1, CONST_BITS - PASS1_BITS);
        tmp[i * 8 + 5] = DESCALE(e2 - o1, CONST_BITS - PASS1_BITS);
        tmp[i * 8 + 3] = DESCALE(e3 + o0, CONST_BITS - PASS1_BITS);
        tmp[i * 8 + 4] = DESCALE(e3 - o0, CONST_BITS - PASS1_BITS);
    }
    
    // Column IDCT
    for (int i = 0; i < 8; i++) {
        int s0 = tmp[i], s1 = tmp[i + 8], s2 = tmp[i + 16], s3 = tmp[i + 24];
        int s4 = tmp[i + 32], s5 = tmp[i + 40], s6 = tmp[i + 48], s7 = tmp[i + 56];
        
        // Even part
        int t0 = (s0 + s4) << CONST_BITS;
        int t1 = (s0 - s4) << CONST_BITS;
        int t2 = s2 * FIX_0_541196100 - s6 * FIX_1_847759065;
        int t3 = s2 * FIX_1_847759065 + s6 * FIX_0_541196100;
        
        int e0 = t0 + t3;
        int e1 = t1 + t2;
        int e2 = t1 - t2;
        int e3 = t0 - t3;
        
        // Odd part
        int z1 = s7 + s1;
        int z2 = s5 + s3;
        int z3 = s7 + s3;
        int z4 = s5 + s1;
        int z5 = (z3 + z4) * FIX_1_175875602;
        
        int o0 = s7 * FIX_0_298631336;
        int o1 = s5 * FIX_2_053119869;
        int o2 = s3 * FIX_3_072711026;
        int o3 = s1 * FIX_1_501321110;
        
        z1 = z1 * -FIX_0_899976223;
        z2 = z2 * -FIX_2_562915447;
        z3 = z3 * -FIX_1_961570560;
        z4 = z4 * -FIX_0_390180644;
        
        z3 += z5;
        z4 += z5;
        
        o0 += z1 + z3;
        o1 += z2 + z4;
        o2 += z2 + z3;
        o3 += z1 + z4;
        
        block[i]      = DESCALE(e0 + o3, CONST_BITS + PASS1_BITS + 3);
        block[i + 56] = DESCALE(e0 - o3, CONST_BITS + PASS1_BITS + 3);
        block[i + 8]  = DESCALE(e1 + o2, CONST_BITS + PASS1_BITS + 3);
        block[i + 48] = DESCALE(e1 - o2, CONST_BITS + PASS1_BITS + 3);
        block[i + 16] = DESCALE(e2 + o1, CONST_BITS + PASS1_BITS + 3);
        block[i + 40] = DESCALE(e2 - o1, CONST_BITS + PASS1_BITS + 3);
        block[i + 24] = DESCALE(e3 + o0, CONST_BITS + PASS1_BITS + 3);
        block[i + 32] = DESCALE(e3 - o0, CONST_BITS + PASS1_BITS + 3);
    }
}

// ============================================================================
// Block Decoding
// ============================================================================

static int decode_block(jpeg_decoder_t *d, int comp, int *block) {
    memset(block, 0, 64 * sizeof(int));
    
    int dc_tbl = d->comp[comp].dc_tbl;
    int ac_tbl = d->comp[comp].ac_tbl;
    int qt_idx = d->comp[comp].quant_idx;
    
    // Decode DC coefficient
    int dc_len = decode_huffman(d, 0, dc_tbl);
    if (dc_len < 0) return JPEG_ERR_HUFFMAN;
    
    int dc_val = 0;
    if (dc_len > 0) {
        dc_val = get_bits(d, dc_len);
        if (dc_val < 0) return JPEG_ERR_CORRUPT;
        dc_val = extend(dc_val, dc_len);
    }
    d->dc_pred[comp] += dc_val;
    block[0] = d->dc_pred[comp] * d->quant[qt_idx][0];
    
    // Decode AC coefficients
    int k = 1;
    while (k < 64) {
        int ac_sym = decode_huffman(d, 1, ac_tbl);
        if (ac_sym < 0) return JPEG_ERR_HUFFMAN;
        
        if (ac_sym == 0) break;  // EOB
        
        int run = ac_sym >> 4;
        int size = ac_sym & 0x0F;
        
        if (size == 0) {
            if (run == 15) {
                k += 16;  // ZRL (16 zeros)
                continue;
            }
            break;  // EOB
        }
        
        k += run;
        if (k >= 64) return JPEG_ERR_CORRUPT;
        
        int ac_val = get_bits(d, size);
        if (ac_val < 0) return JPEG_ERR_CORRUPT;
        ac_val = extend(ac_val, size);
        
        block[zigzag[k]] = ac_val * d->quant[qt_idx][zigzag[k]];
        k++;
    }
    
    // Perform IDCT
    idct_block(block);
    
    return JPEG_OK;
}

// ============================================================================
// Marker Parsing
// ============================================================================

static int parse_dqt(jpeg_decoder_t *d) {
    int len = read_u16(d);
    if (len < 0) return JPEG_ERR_CORRUPT;
    len -= 2;
    
    while (len > 0) {
        int info = read_byte(d);
        if (info < 0) return JPEG_ERR_CORRUPT;
        len--;
        
        int prec = info >> 4;
        int id = info & 0x0F;
        
        if (id >= 4) return JPEG_ERR_DQT;
        if (prec != 0) return JPEG_ERR_UNSUPPORTED;  // 16-bit not supported
        
        for (int i = 0; i < 64; i++) {
            int val = read_byte(d);
            if (val < 0) return JPEG_ERR_CORRUPT;
            d->quant[id][i] = (uint8_t)val;
        }
        d->quant_valid[id] = 1;
        len -= 64;
        
        JPEG_DBG("DQT: table %d, precision %d\n", id, prec);
    }
    
    return JPEG_OK;
}

static int parse_dht(jpeg_decoder_t *d) {
    int len = read_u16(d);
    if (len < 0) return JPEG_ERR_CORRUPT;
    len -= 2;
    
    while (len > 0) {
        int info = read_byte(d);
        if (info < 0) return JPEG_ERR_CORRUPT;
        len--;
        
        int type = (info >> 4) & 1;  // 0=DC, 1=AC
        int id = info & 0x0F;
        
        if (id >= 4) return JPEG_ERR_DHT;
        
        int total = 0;
        for (int i = 0; i < 16; i++) {
            int count = read_byte(d);
            if (count < 0) return JPEG_ERR_CORRUPT;
            d->huff_bits[type][id][i] = (uint8_t)count;
            total += count;
        }
        len -= 16;
        
        for (int i = 0; i < total; i++) {
            int val = read_byte(d);
            if (val < 0) return JPEG_ERR_CORRUPT;
            d->huff_vals[type][id][i] = (uint8_t)val;
        }
        len -= total;
        
        d->huff_valid[type][id] = 1;
        build_huffman(d, type, id);
        
        JPEG_DBG("DHT: type %s, id %d, %d symbols\n",
                 type ? "AC" : "DC", id, total);
    }
    
    return JPEG_OK;
}

static int parse_sof0(jpeg_decoder_t *d) {
    int len = read_u16(d);
    if (len < 0) return JPEG_ERR_CORRUPT;
    
    d->precision = read_byte(d);
    if (d->precision != 8) return JPEG_ERR_UNSUPPORTED;
    
    int height = read_u16(d);
    int width = read_u16(d);
    int comps = read_byte(d);
    
    if (width <= 0 || height <= 0) return JPEG_ERR_SOF;
    if (width > JPEG_MAX_WIDTH || height > JPEG_MAX_HEIGHT) return JPEG_ERR_UNSUPPORTED;
    if (comps != 1 && comps != 3) return JPEG_ERR_UNSUPPORTED;
    
    d->width = width;
    d->height = height;
    d->components = comps;
    
    d->max_h_sample = 1;
    d->max_v_sample = 1;
    
    for (int i = 0; i < comps; i++) {
        d->comp[i].id = read_byte(d);
        int sampling = read_byte(d);
        d->comp[i].h_sample = sampling >> 4;
        d->comp[i].v_sample = sampling & 0x0F;
        d->comp[i].quant_idx = read_byte(d);
        
        if (d->comp[i].h_sample > d->max_h_sample)
            d->max_h_sample = d->comp[i].h_sample;
        if (d->comp[i].v_sample > d->max_v_sample)
            d->max_v_sample = d->comp[i].v_sample;
    }
    
    d->mcu_width = d->max_h_sample * 8;
    d->mcu_height = d->max_v_sample * 8;
    d->mcu_count_x = (width + d->mcu_width - 1) / d->mcu_width;
    d->mcu_count_y = (height + d->mcu_height - 1) / d->mcu_height;
    
    JPEG_DBG("SOF0: %dx%d, %d components, MCU %dx%d\n",
             width, height, comps, d->mcu_width, d->mcu_height);
    
    return JPEG_OK;
}

static int parse_sos(jpeg_decoder_t *d) {
    int len = read_u16(d);
    if (len < 0) return JPEG_ERR_CORRUPT;
    
    int ns = read_byte(d);
    if (ns != d->components) return JPEG_ERR_SOS;
    
    for (int i = 0; i < ns; i++) {
        int id = read_byte(d);
        int tables = read_byte(d);
        
        // Find component by ID and set tables
        for (int j = 0; j < d->components; j++) {
            if (d->comp[j].id == id) {
                d->comp[j].dc_tbl = tables >> 4;
                d->comp[j].ac_tbl = tables & 0x0F;
                break;
            }
        }
    }
    
    // Skip Ss, Se, Ah/Al
    read_byte(d);
    read_byte(d);
    read_byte(d);
    
    // Reset bit reader and DC prediction
    d->bits = 0;
    d->bits_left = 0;
    for (int i = 0; i < d->components; i++) {
        d->dc_pred[i] = 0;
    }
    
    // Allocate MCU buffers
    int mcu_pixels = d->mcu_width * d->mcu_height;
    int *mcu_y = kmalloc(mcu_pixels * sizeof(int));
    int *mcu_cb = kmalloc(mcu_pixels * sizeof(int));
    int *mcu_cr = kmalloc(mcu_pixels * sizeof(int));
    int block[64];
    
    if (!mcu_y || !mcu_cb || !mcu_cr) {
        if (mcu_y) kfree(mcu_y);
        if (mcu_cb) kfree(mcu_cb);
        if (mcu_cr) kfree(mcu_cr);
        return JPEG_ERR_NOMEM;
    }
    
    int restart_count = 0;
    int ret = JPEG_OK;
    
    // Decode MCUs
    for (int mcu_row = 0; mcu_row < d->mcu_count_y; mcu_row++) {
        for (int mcu_col = 0; mcu_col < d->mcu_count_x; mcu_col++) {
            // Handle restart markers
            if (d->restart_interval > 0 && restart_count == d->restart_interval) {
                d->bits_left = 0;
                
                // Skip to next restart marker
                int m1 = read_byte(d);
                int m2 = read_byte(d);
                (void)m1; (void)m2;
                
                for (int i = 0; i < d->components; i++) {
                    d->dc_pred[i] = 0;
                }
                restart_count = 0;
            }
            
            memset(mcu_y, 0, mcu_pixels * sizeof(int));
            if (d->components == 3) {
                memset(mcu_cb, 0, mcu_pixels * sizeof(int));
                memset(mcu_cr, 0, mcu_pixels * sizeof(int));
            }
            
            // Decode all blocks in MCU
            for (int comp = 0; comp < d->components; comp++) {
                int h = d->comp[comp].h_sample;
                int v = d->comp[comp].v_sample;
                int *target = (comp == 0) ? mcu_y : (comp == 1) ? mcu_cb : mcu_cr;
                
                for (int by = 0; by < v; by++) {
                    for (int bx = 0; bx < h; bx++) {
                        ret = decode_block(d, comp, block);
                        if (ret != JPEG_OK) goto decode_done;
                        
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
            int base_x = mcu_col * d->mcu_width;
            int base_y = mcu_row * d->mcu_height;
            
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
                        
                        // Standard JFIF conversion
                        r = clamp(yy + ((cr * 359) >> 8));
                        g = clamp(yy - ((cb * 88 + cr * 183) >> 8));
                        b = clamp(yy + ((cb * 454) >> 8));
                    }
                    
                    int px = base_x + x;
                    int py = base_y + y;
                    // Store as BGRA for framebuffer
                    d->pixels[py * d->width + px] = 0xFF000000 | (r << 16) | (g << 8) | b;
                }
            }
            
            restart_count++;
        }
    }
    
decode_done:
    kfree(mcu_y);
    kfree(mcu_cb);
    kfree(mcu_cr);
    
    return ret;
}

// ============================================================================
// Public API
// ============================================================================

int jpeg_is_jpeg(const void *data, uint32_t size) {
    if (!data || size < 2) return 0;
    const uint8_t *p = (const uint8_t *)data;
    return (p[0] == 0xFF && p[1] == 0xD8);
}

int jpeg_get_info(const void *data, uint32_t size,
                  uint32_t *width, uint32_t *height, int *comps) {
    if (!data || size < 4) return JPEG_ERR_TOO_SMALL;
    
    const uint8_t *p = (const uint8_t *)data;
    if (p[0] != 0xFF || p[1] != 0xD8) {
        return JPEG_ERR_INVALID_SIG;
    }
    
    jpeg_decoder_t d;
    memset(&d, 0, sizeof(d));
    d.data = p;
    d.data_len = size;
    d.pos = 2;
    
    // Search for SOF0 marker
    while (d.pos < d.data_len - 1) {
        int marker = read_byte(&d);
        if (marker != 0xFF) continue;
        
        do {
            marker = read_byte(&d);
        } while (marker == 0xFF);
        
        if (marker < 0) break;
        
        int full = 0xFF00 | marker;
        if (full == JPEG_MARKER_SOF0) {
            int len = read_u16(&d);
            (void)len;
            int prec = read_byte(&d);
            (void)prec;
            int h = read_u16(&d);
            int w = read_u16(&d);
            int c = read_byte(&d);
            
            if (width) *width = w;
            if (height) *height = h;
            if (comps) *comps = c;
            return JPEG_OK;
        }
        
        // Skip marker content
        if (marker >= 0xC0 && marker <= 0xFE) {
            int len = read_u16(&d);
            if (len >= 2) d.pos += len - 2;
        }
    }
    
    return JPEG_ERR_CORRUPT;
}

int jpeg_decode(const void *data, uint32_t size, image_t *img) {
    if (!data || !img) return JPEG_ERR_NULL_PTR;
    if (size < 4) return JPEG_ERR_TOO_SMALL;
    
    const uint8_t *p = (const uint8_t *)data;
    if (p[0] != 0xFF || p[1] != 0xD8) {
        return JPEG_ERR_INVALID_SIG;
    }
    
    jpeg_decoder_t *d = kzalloc(sizeof(jpeg_decoder_t));
    if (!d) return JPEG_ERR_NOMEM;
    
    d->data = p;
    d->data_len = size;
    d->pos = 2;  // Skip SOI
    
    img->width = 0;
    img->height = 0;
    img->pixels = NULL;
    
    int ret = JPEG_OK;
    
    // Parse markers
    while (d->pos < d->data_len) {
        int marker = read_byte(d);
        if (marker != 0xFF) continue;
        
        do {
            marker = read_byte(d);
        } while (marker == 0xFF);
        
        if (marker < 0) break;
        
        int full = 0xFF00 | marker;
        
        switch (full) {
            case JPEG_MARKER_SOF0:
                ret = parse_sof0(d);
                if (ret != JPEG_OK) goto done;
                break;
                
            case JPEG_MARKER_SOF2:
                // Progressive not supported
                ret = JPEG_ERR_UNSUPPORTED;
                goto done;
                
            case JPEG_MARKER_DHT:
                ret = parse_dht(d);
                if (ret != JPEG_OK) goto done;
                break;
                
            case JPEG_MARKER_DQT:
                ret = parse_dqt(d);
                if (ret != JPEG_OK) goto done;
                break;
                
            case JPEG_MARKER_DRI:
                read_u16(d);  // Length
                d->restart_interval = read_u16(d);
                break;
                
            case JPEG_MARKER_SOS:
                // Allocate output buffer
                img->width = d->width;
                img->height = d->height;
                img->pixels = kmalloc(d->width * d->height * 4);
                if (!img->pixels) {
                    ret = JPEG_ERR_NOMEM;
                    goto done;
                }
                d->pixels = img->pixels;
                ret = parse_sos(d);
                goto done;
                
            case JPEG_MARKER_EOI:
                goto done;
                
            default:
                // Skip APP and COM markers
                if ((marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE) {
                    int len = read_u16(d);
                    if (len >= 2) d->pos += len - 2;
                }
                break;
        }
    }
    
done:
    kfree(d);
    
    if (ret != JPEG_OK && img->pixels) {
        kfree(img->pixels);
        img->pixels = NULL;
        img->width = 0;
        img->height = 0;
    }
    
    return ret;
}

const char *jpeg_error_string(int err) {
    switch (err) {
        case JPEG_OK:              return "Success";
        case JPEG_ERR_NULL_PTR:    return "Null pointer";
        case JPEG_ERR_INVALID_SIG: return "Invalid JPEG signature";
        case JPEG_ERR_UNSUPPORTED: return "Unsupported JPEG format";
        case JPEG_ERR_NOMEM:       return "Out of memory";
        case JPEG_ERR_CORRUPT:     return "Corrupt JPEG data";
        case JPEG_ERR_TOO_SMALL:   return "Data too small";
        case JPEG_ERR_HUFFMAN:     return "Huffman decode error";
        case JPEG_ERR_MARKER:      return "Invalid marker";
        case JPEG_ERR_DQT:         return "Invalid DQT";
        case JPEG_ERR_DHT:         return "Invalid DHT";
        case JPEG_ERR_SOF:         return "Invalid SOF";
        case JPEG_ERR_SOS:         return "Invalid SOS";
        default:                   return "Unknown error";
    }
}
