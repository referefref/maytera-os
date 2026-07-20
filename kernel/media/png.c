// png.c - PNG Image Decoder for MayteraOS
// Enhanced PNG decoder with Adam7 interlacing and 16-bit support

#include "png.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"

// Debug macro
#define PNG_DEBUG 0
#if PNG_DEBUG
#define PNG_DBG(fmt, ...) kprintf("[PNG] " fmt, ##__VA_ARGS__)
#else
#define PNG_DBG(fmt, ...) ((void)0)
#endif

// PNG signature
static const uint8_t png_signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};

// Chunk types (big-endian)
#define CHUNK_IHDR  0x49484452
#define CHUNK_PLTE  0x504C5445
#define CHUNK_IDAT  0x49444154
#define CHUNK_IEND  0x49454E44
#define CHUNK_tRNS  0x74524E53
#define CHUNK_gAMA  0x67414D41
#define CHUNK_bKGD  0x624B4744
#define CHUNK_cHRM  0x6348524D
#define CHUNK_sRGB  0x73524742
#define CHUNK_iCCP  0x69434350
#define CHUNK_tEXt  0x74455874
#define CHUNK_zTXt  0x7A545874
#define CHUNK_iTXt  0x69545874
#define CHUNK_pHYs  0x70485973
#define CHUNK_tIME  0x74494D45

// Read big-endian uint32
static inline uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

// Read big-endian uint16
static inline uint16_t read_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

// ============================================================================
// CRC32 Implementation
// ============================================================================

static uint32_t crc_table[256];
static int crc_table_computed = 0;

static void make_crc_table(void) {
    for (int n = 0; n < 256; n++) {
        uint32_t c = (uint32_t)n;
        for (int k = 0; k < 8; k++) {
            if (c & 1)
                c = 0xEDB88320 ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc_table[n] = c;
    }
    crc_table_computed = 1;
}

uint32_t png_crc32(const uint8_t *data, uint32_t len) {
    if (!crc_table_computed) make_crc_table();
    
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// ============================================================================
// DEFLATE Decompression
// ============================================================================

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
    uint32_t bit_buf;
    int bit_count;
} inflate_state_t;

static int inf_getbit(inflate_state_t *s) {
    if (s->bit_count == 0) {
        if (s->pos >= s->size) return -1;
        s->bit_buf = s->data[s->pos++];
        s->bit_count = 8;
    }
    int bit = s->bit_buf & 1;
    s->bit_buf >>= 1;
    s->bit_count--;
    return bit;
}

static int inf_getbits(inflate_state_t *s, int n) {
    int val = 0;
    for (int i = 0; i < n; i++) {
        int bit = inf_getbit(s);
        if (bit < 0) return -1;
        val |= (bit << i);
    }
    return val;
}

// Code length order for dynamic Huffman
static const int code_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// Length base values and extra bits
static const int length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const int length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

// Distance base values and extra bits
static const int dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const int dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

// Huffman table structure
typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
} huffman_t;

static int build_huffman(huffman_t *h, const int *lengths, int n) {
    memset(h->counts, 0, sizeof(h->counts));
    
    for (int i = 0; i < n; i++) {
        if (lengths[i] > 0 && lengths[i] < 16) {
            h->counts[lengths[i]]++;
        }
    }
    
    uint16_t offsets[16];
    offsets[0] = 0;
    for (int i = 1; i < 16; i++) {
        offsets[i] = offsets[i-1] + h->counts[i-1];
    }
    
    for (int i = 0; i < n; i++) {
        if (lengths[i] > 0 && lengths[i] < 16) {
            h->symbols[offsets[lengths[i]]++] = (uint16_t)i;
        }
    }
    
    return 0;
}

static int decode_symbol(inflate_state_t *s, huffman_t *h) {
    int code = 0;
    int first = 0;
    int index = 0;
    
    for (int len = 1; len < 16; len++) {
        int bit = inf_getbit(s);
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        int count = h->counts[len];
        if (code - count < first) {
            return h->symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
    }
    return -1;
}

// Fixed Huffman tables
static int fixed_lit_lengths[288];
static int fixed_dist_lengths[32];
static int fixed_tables_built = 0;

static void build_fixed_tables(void) {
    for (int i = 0; i <= 143; i++) fixed_lit_lengths[i] = 8;
    for (int i = 144; i <= 255; i++) fixed_lit_lengths[i] = 9;
    for (int i = 256; i <= 279; i++) fixed_lit_lengths[i] = 7;
    for (int i = 280; i <= 287; i++) fixed_lit_lengths[i] = 8;
    for (int i = 0; i < 32; i++) fixed_dist_lengths[i] = 5;
    fixed_tables_built = 1;
}

int png_inflate(const uint8_t *src, uint32_t src_len,
                uint8_t *dst, uint32_t dst_cap, uint32_t *dst_len) {
    inflate_state_t s;
    s.data = src;
    s.size = src_len;
    s.pos = 0;
    s.bit_buf = 0;
    s.bit_count = 0;
    
    if (!fixed_tables_built) build_fixed_tables();
    
    uint32_t out_pos = 0;
    int bfinal;
    
    do {
        bfinal = inf_getbit(&s);
        if (bfinal < 0) return PNG_ERR_INFLATE;
        
        int btype = inf_getbits(&s, 2);
        if (btype < 0) return PNG_ERR_INFLATE;
        
        if (btype == 0) {
            // Stored block
            s.bit_count = 0;
            if (s.pos + 4 > s.size) return PNG_ERR_INFLATE;
            uint16_t len = s.data[s.pos] | (s.data[s.pos+1] << 8);
            s.pos += 4;
            if (s.pos + len > s.size) return PNG_ERR_INFLATE;
            if (out_pos + len > dst_cap) return PNG_ERR_INFLATE;
            memcpy(dst + out_pos, s.data + s.pos, len);
            s.pos += len;
            out_pos += len;
        }
        else if (btype == 1 || btype == 2) {
            huffman_t lit_huff, dist_huff;
            int lit_lengths[288];
            int dist_lengths[32];
            
            if (btype == 1) {
                memcpy(lit_lengths, fixed_lit_lengths, sizeof(fixed_lit_lengths));
                memcpy(dist_lengths, fixed_dist_lengths, sizeof(fixed_dist_lengths));
            } else {
                int hlit = inf_getbits(&s, 5) + 257;
                int hdist = inf_getbits(&s, 5) + 1;
                int hclen = inf_getbits(&s, 4) + 4;
                
                if (hlit < 0 || hdist < 0 || hclen < 0) return PNG_ERR_INFLATE;
                
                int code_lengths[19];
                memset(code_lengths, 0, sizeof(code_lengths));
                for (int i = 0; i < hclen; i++) {
                    int val = inf_getbits(&s, 3);
                    if (val < 0) return PNG_ERR_INFLATE;
                    code_lengths[code_order[i]] = val;
                }
                
                huffman_t code_huff;
                build_huffman(&code_huff, code_lengths, 19);
                
                int combined[320];
                int n = 0;
                while (n < hlit + hdist) {
                    int sym = decode_symbol(&s, &code_huff);
                    if (sym < 0) return PNG_ERR_INFLATE;
                    
                    if (sym < 16) {
                        combined[n++] = sym;
                    } else if (sym == 16) {
                        int count = inf_getbits(&s, 2) + 3;
                        if (count < 0 || n == 0) return PNG_ERR_INFLATE;
                        int val = combined[n - 1];
                        while (count-- > 0 && n < hlit + hdist) combined[n++] = val;
                    } else if (sym == 17) {
                        int count = inf_getbits(&s, 3) + 3;
                        if (count < 0) return PNG_ERR_INFLATE;
                        while (count-- > 0 && n < hlit + hdist) combined[n++] = 0;
                    } else if (sym == 18) {
                        int count = inf_getbits(&s, 7) + 11;
                        if (count < 0) return PNG_ERR_INFLATE;
                        while (count-- > 0 && n < hlit + hdist) combined[n++] = 0;
                    }
                }
                
                memcpy(lit_lengths, combined, hlit * sizeof(int));
                memset(lit_lengths + hlit, 0, (288 - hlit) * sizeof(int));
                memcpy(dist_lengths, combined + hlit, hdist * sizeof(int));
                memset(dist_lengths + hdist, 0, (32 - hdist) * sizeof(int));
            }
            
            build_huffman(&lit_huff, lit_lengths, 288);
            build_huffman(&dist_huff, dist_lengths, 32);
            
            while (1) {
                int sym = decode_symbol(&s, &lit_huff);
                if (sym < 0) return PNG_ERR_INFLATE;
                
                if (sym < 256) {
                    if (out_pos >= dst_cap) return PNG_ERR_INFLATE;
                    dst[out_pos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return PNG_ERR_INFLATE;
                    
                    int len = length_base[sym];
                    if (length_extra[sym] > 0) {
                        int extra = inf_getbits(&s, length_extra[sym]);
                        if (extra < 0) return PNG_ERR_INFLATE;
                        len += extra;
                    }
                    
                    int dsym = decode_symbol(&s, &dist_huff);
                    if (dsym < 0 || dsym >= 30) return PNG_ERR_INFLATE;
                    
                    int dist = dist_base[dsym];
                    if (dist_extra[dsym] > 0) {
                        int extra = inf_getbits(&s, dist_extra[dsym]);
                        if (extra < 0) return PNG_ERR_INFLATE;
                        dist += extra;
                    }
                    
                    if ((uint32_t)dist > out_pos) return PNG_ERR_INFLATE;
                    if (out_pos + len > dst_cap) return PNG_ERR_INFLATE;
                    
                    for (int i = 0; i < len; i++) {
                        dst[out_pos] = dst[out_pos - dist];
                        out_pos++;
                    }
                }
            }
        } else {
            return PNG_ERR_INFLATE;
        }
    } while (!bfinal);
    
    *dst_len = out_pos;
    return PNG_OK;
}

// ============================================================================
// PNG Filter Functions
// ============================================================================

uint8_t png_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    else if (pb <= pc) return (uint8_t)b;
    else return (uint8_t)c;
}

static void unfilter_row(uint8_t *cur, const uint8_t *prev,
                         uint32_t len, int bpp, int filter) {
    switch (filter) {
        case PNG_FILTER_NONE:
            break;
            
        case PNG_FILTER_SUB:
            for (uint32_t i = bpp; i < len; i++) {
                cur[i] += cur[i - bpp];
            }
            break;
            
        case PNG_FILTER_UP:
            if (prev) {
                for (uint32_t i = 0; i < len; i++) {
                    cur[i] += prev[i];
                }
            }
            break;
            
        case PNG_FILTER_AVERAGE:
            for (uint32_t i = 0; i < len; i++) {
                int a = (i >= (uint32_t)bpp) ? cur[i - bpp] : 0;
                int b = prev ? prev[i] : 0;
                cur[i] += (uint8_t)((a + b) / 2);
            }
            break;
            
        case PNG_FILTER_PAETH:
            for (uint32_t i = 0; i < len; i++) {
                int a = (i >= (uint32_t)bpp) ? cur[i - bpp] : 0;
                int b = prev ? prev[i] : 0;
                int c = (prev && i >= (uint32_t)bpp) ? prev[i - bpp] : 0;
                cur[i] += png_paeth(a, b, c);
            }
            break;
    }
}

// ============================================================================
// Adam7 Interlacing Support
// ============================================================================

// Adam7 pass parameters: [pass][0]=x_start, [1]=y_start, [2]=x_step, [3]=y_step
static const int adam7[7][4] = {
    {0, 0, 8, 8},  // Pass 1
    {4, 0, 8, 8},  // Pass 2
    {0, 4, 4, 8},  // Pass 3
    {2, 0, 4, 4},  // Pass 4
    {0, 2, 2, 4},  // Pass 5
    {1, 0, 2, 2},  // Pass 6
    {0, 1, 1, 2}   // Pass 7
};

static uint32_t adam7_pass_width(uint32_t width, int pass) {
    int x_start = adam7[pass][0];
    int x_step = adam7[pass][2];
    if (width <= (uint32_t)x_start) return 0;
    return (width - x_start + x_step - 1) / x_step;
}

static uint32_t adam7_pass_height(uint32_t height, int pass) {
    int y_start = adam7[pass][1];
    int y_step = adam7[pass][3];
    if (height <= (uint32_t)y_start) return 0;
    return (height - y_start + y_step - 1) / y_step;
}

// ============================================================================
// Pixel Conversion
// ============================================================================

// Calculate bytes per pixel for a given format
static int get_bpp(int color_type, int bit_depth) {
    int samples;
    switch (color_type) {
        case PNG_COLOR_GRAYSCALE:   samples = 1; break;
        case PNG_COLOR_RGB:         samples = 3; break;
        case PNG_COLOR_INDEXED:     samples = 1; break;
        case PNG_COLOR_GRAYSCALE_A: samples = 2; break;
        case PNG_COLOR_RGBA:        samples = 4; break;
        default: return 0;
    }
    return (samples * bit_depth + 7) / 8;
}

// Convert scanline to BGRA pixels
static void convert_scanline_to_bgra(png_decoder_t *dec, const uint8_t *src,
                                     uint32_t *dst, uint32_t width) {
    int color_type = dec->ihdr.color_type;
    int bit_depth = dec->ihdr.bit_depth;
    
    for (uint32_t x = 0; x < width; x++) {
        uint8_t r, g, b, a = 255;
        
        switch (color_type) {
            case PNG_COLOR_GRAYSCALE:
                if (bit_depth == 16) {
                    r = g = b = src[x * 2];
                    if (dec->has_trns && read_be16(src + x * 2) == dec->trns_gray) {
                        a = 0;
                    }
                } else if (bit_depth == 8) {
                    r = g = b = src[x];
                    if (dec->has_trns && src[x] == (dec->trns_gray & 0xFF)) {
                        a = 0;
                    }
                } else {
                    // 1, 2, 4 bit
                    int bits_per_byte = 8 / bit_depth;
                    int byte_idx = x / bits_per_byte;
                    int bit_idx = (bits_per_byte - 1 - (x % bits_per_byte)) * bit_depth;
                    int val = (src[byte_idx] >> bit_idx) & ((1 << bit_depth) - 1);
                    val = val * 255 / ((1 << bit_depth) - 1);
                    r = g = b = val;
                }
                break;
                
            case PNG_COLOR_RGB:
                if (bit_depth == 16) {
                    r = src[x * 6];
                    g = src[x * 6 + 2];
                    b = src[x * 6 + 4];
                    if (dec->has_trns) {
                        uint16_t tr = read_be16(src + x * 6);
                        uint16_t tg = read_be16(src + x * 6 + 2);
                        uint16_t tb = read_be16(src + x * 6 + 4);
                        if (tr == dec->trns_rgb[0] && 
                            tg == dec->trns_rgb[1] && 
                            tb == dec->trns_rgb[2]) {
                            a = 0;
                        }
                    }
                } else {
                    r = src[x * 3];
                    g = src[x * 3 + 1];
                    b = src[x * 3 + 2];
                    if (dec->has_trns) {
                        if (r == (dec->trns_rgb[0] & 0xFF) &&
                            g == (dec->trns_rgb[1] & 0xFF) &&
                            b == (dec->trns_rgb[2] & 0xFF)) {
                            a = 0;
                        }
                    }
                }
                break;
                
            case PNG_COLOR_INDEXED:
                {
                    int idx;
                    if (bit_depth == 8) {
                        idx = src[x];
                    } else {
                        int bits_per_byte = 8 / bit_depth;
                        int byte_idx = x / bits_per_byte;
                        int bit_idx = (bits_per_byte - 1 - (x % bits_per_byte)) * bit_depth;
                        idx = (src[byte_idx] >> bit_idx) & ((1 << bit_depth) - 1);
                    }
                    if (idx < dec->palette_count) {
                        r = dec->palette[idx][0];
                        g = dec->palette[idx][1];
                        b = dec->palette[idx][2];
                        a = dec->palette_alpha[idx];
                    } else {
                        r = g = b = 0;
                    }
                }
                break;
                
            case PNG_COLOR_GRAYSCALE_A:
                if (bit_depth == 16) {
                    r = g = b = src[x * 4];
                    a = src[x * 4 + 2];
                } else {
                    r = g = b = src[x * 2];
                    a = src[x * 2 + 1];
                }
                break;
                
            case PNG_COLOR_RGBA:
                if (bit_depth == 16) {
                    r = src[x * 8];
                    g = src[x * 8 + 2];
                    b = src[x * 8 + 4];
                    a = src[x * 8 + 6];
                } else {
                    r = src[x * 4];
                    g = src[x * 4 + 1];
                    b = src[x * 4 + 2];
                    a = src[x * 4 + 3];
                }
                break;
                
            default:
                r = g = b = 0;
        }
        
        // Store as BGRA (for framebuffer compatibility)
        dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                 ((uint32_t)g << 8) | b;
    }
}

// ============================================================================
// Chunk Parsing
// ============================================================================

static int parse_ihdr(png_decoder_t *dec, const uint8_t *data, uint32_t len) {
    if (len < 13) return PNG_ERR_INVALID_IHDR;
    
    dec->ihdr.width = read_be32(data);
    dec->ihdr.height = read_be32(data + 4);
    dec->ihdr.bit_depth = data[8];
    dec->ihdr.color_type = data[9];
    dec->ihdr.compression = data[10];
    dec->ihdr.filter = data[11];
    dec->ihdr.interlace = data[12];
    
    // Validate dimensions
    if (dec->ihdr.width == 0 || dec->ihdr.height == 0) {
        return PNG_ERR_INVALID_IHDR;
    }
    if (dec->ihdr.width > PNG_MAX_WIDTH || dec->ihdr.height > PNG_MAX_HEIGHT) {
        return PNG_ERR_UNSUPPORTED;
    }
    
    // Validate bit depth for color type
    int color = dec->ihdr.color_type;
    int depth = dec->ihdr.bit_depth;
    
    if (color == PNG_COLOR_GRAYSCALE) {
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) {
            return PNG_ERR_INVALID_IHDR;
        }
    } else if (color == PNG_COLOR_RGB || color == PNG_COLOR_GRAYSCALE_A ||
               color == PNG_COLOR_RGBA) {
        if (depth != 8 && depth != 16) {
            return PNG_ERR_INVALID_IHDR;
        }
    } else if (color == PNG_COLOR_INDEXED) {
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8) {
            return PNG_ERR_INVALID_IHDR;
        }
    } else {
        return PNG_ERR_INVALID_IHDR;
    }
    
    // Validate interlace
    if (dec->ihdr.interlace > 1) {
        return PNG_ERR_INVALID_IHDR;
    }
    
    PNG_DBG("IHDR: %dx%d, %d-bit, color %d, interlace %d\n",
            dec->ihdr.width, dec->ihdr.height, depth, color,
            dec->ihdr.interlace);
    
    return PNG_OK;
}

static int parse_plte(png_decoder_t *dec, const uint8_t *data, uint32_t len) {
    if (len % 3 != 0 || len > PNG_MAX_PALETTE * 3) {
        return PNG_ERR_CORRUPT;
    }
    
    dec->palette_count = len / 3;
    for (int i = 0; i < dec->palette_count; i++) {
        dec->palette[i][0] = data[i * 3];
        dec->palette[i][1] = data[i * 3 + 1];
        dec->palette[i][2] = data[i * 3 + 2];
        dec->palette_alpha[i] = 255;
    }
    
    PNG_DBG("PLTE: %d colors\n", dec->palette_count);
    return PNG_OK;
}

static int parse_trns(png_decoder_t *dec, const uint8_t *data, uint32_t len) {
    dec->has_trns = 1;
    
    if (dec->ihdr.color_type == PNG_COLOR_GRAYSCALE) {
        if (len >= 2) {
            dec->trns_gray = read_be16(data);
        }
    } else if (dec->ihdr.color_type == PNG_COLOR_RGB) {
        if (len >= 6) {
            dec->trns_rgb[0] = read_be16(data);
            dec->trns_rgb[1] = read_be16(data + 2);
            dec->trns_rgb[2] = read_be16(data + 4);
        }
    } else if (dec->ihdr.color_type == PNG_COLOR_INDEXED) {
        for (uint32_t i = 0; i < len && i < PNG_MAX_PALETTE; i++) {
            dec->palette_alpha[i] = data[i];
        }
    }
    
    return PNG_OK;
}

// ============================================================================
// Main Decode Function
// ============================================================================

int png_is_png(const void *data, uint32_t size) {
    if (!data || size < 8) return 0;
    return memcmp(data, png_signature, 8) == 0;
}

int png_get_info(const void *data, uint32_t size,
                 uint32_t *width, uint32_t *height,
                 uint8_t *depth, uint8_t *color) {
    if (!data || size < 8) return PNG_ERR_TOO_SMALL;
    
    const uint8_t *p = (const uint8_t *)data;
    if (memcmp(p, png_signature, 8) != 0) {
        return PNG_ERR_INVALID_SIG;
    }
    p += 8;
    
    if (size < 8 + 12 + 13) return PNG_ERR_TOO_SMALL;
    
    uint32_t chunk_len = read_be32(p);
    uint32_t chunk_type = read_be32(p + 4);
    
    if (chunk_type != CHUNK_IHDR || chunk_len < 13) {
        return PNG_ERR_CORRUPT;
    }
    
    const uint8_t *ihdr = p + 8;
    if (width) *width = read_be32(ihdr);
    if (height) *height = read_be32(ihdr + 4);
    if (depth) *depth = ihdr[8];
    if (color) *color = ihdr[9];
    
    return PNG_OK;
}

int png_decode(const void *data, uint32_t size, image_t *img) {
    if (!data || !img) return PNG_ERR_NULL_PTR;
    if (size < 8) return PNG_ERR_TOO_SMALL;
    
    const uint8_t *p = (const uint8_t *)data;
    
    // Check signature
    if (memcmp(p, png_signature, 8) != 0) {
        return PNG_ERR_INVALID_SIG;
    }
    p += 8;
    
    // Initialize decoder
    png_decoder_t *dec = kzalloc(sizeof(png_decoder_t));
    if (!dec) return PNG_ERR_NOMEM;
    
    dec->data = (const uint8_t *)data;
    dec->size = size;
    
    img->width = 0;
    img->height = 0;
    img->pixels = NULL;
    
    int ret = PNG_OK;
    int have_ihdr = 0;
    
    // Parse chunks
    const uint8_t *end = (const uint8_t *)data + size;
    while (p + 12 <= end) {
        uint32_t chunk_len = read_be32(p);
        uint32_t chunk_type = read_be32(p + 4);
        
        if (p + 12 + chunk_len > end) {
            ret = PNG_ERR_CHUNK_SIZE;
            break;
        }
        
        const uint8_t *chunk_data = p + 8;
        
        // Verify CRC
        uint32_t expected_crc = read_be32(p + 8 + chunk_len);
        uint32_t actual_crc = png_crc32(p + 4, chunk_len + 4);
        if (expected_crc != actual_crc) {
            PNG_DBG("CRC mismatch: expected %08x, got %08x\n", expected_crc, actual_crc);
            ret = PNG_ERR_CRC;
            break;
        }
        
        switch (chunk_type) {
            case CHUNK_IHDR:
                ret = parse_ihdr(dec, chunk_data, chunk_len);
                if (ret != PNG_OK) goto done;
                have_ihdr = 1;
                break;
                
            case CHUNK_PLTE:
                ret = parse_plte(dec, chunk_data, chunk_len);
                if (ret != PNG_OK) goto done;
                break;
                
            case CHUNK_tRNS:
                ret = parse_trns(dec, chunk_data, chunk_len);
                if (ret != PNG_OK) goto done;
                break;
                
            case CHUNK_IDAT:
                // Accumulate compressed data
                if (dec->compressed_len + chunk_len > dec->compressed_cap) {
                    uint32_t new_cap = dec->compressed_cap ? dec->compressed_cap * 2 : 65536;
                    while (new_cap < dec->compressed_len + chunk_len) new_cap *= 2;
                    uint8_t *new_buf = kmalloc(new_cap);
                    if (!new_buf) {
                        ret = PNG_ERR_NOMEM;
                        goto done;
                    }
                    if (dec->compressed) {
                        memcpy(new_buf, dec->compressed, dec->compressed_len);
                        kfree(dec->compressed);
                    }
                    dec->compressed = new_buf;
                    dec->compressed_cap = new_cap;
                }
                memcpy(dec->compressed + dec->compressed_len, chunk_data, chunk_len);
                dec->compressed_len += chunk_len;
                break;
                
            case CHUNK_IEND:
                goto process_image;
                
            default:
                // Skip unknown chunks
                break;
        }
        
        p += 12 + chunk_len;
    }
    
process_image:
    if (!have_ihdr || !dec->compressed || dec->compressed_len == 0) {
        ret = PNG_ERR_CORRUPT;
        goto done;
    }
    
    // Check for required PLTE in indexed color
    if (dec->ihdr.color_type == PNG_COLOR_INDEXED && dec->palette_count == 0) {
        ret = PNG_ERR_MISSING_PLTE;
        goto done;
    }
    
    uint32_t width = dec->ihdr.width;
    uint32_t height = dec->ihdr.height;
    int bpp = get_bpp(dec->ihdr.color_type, dec->ihdr.bit_depth);
    
    // Calculate raw size based on interlacing
    uint32_t raw_size;
    if (dec->ihdr.interlace == PNG_INTERLACE_NONE) {
        uint32_t scanline_bytes = (width * bpp * dec->ihdr.bit_depth + 7) / 8;
        raw_size = (scanline_bytes + 1) * height;
    } else {
        // Adam7: sum of all passes
        raw_size = 0;
        for (int pass = 0; pass < 7; pass++) {
            uint32_t pw = adam7_pass_width(width, pass);
            uint32_t ph = adam7_pass_height(height, pass);
            if (pw > 0 && ph > 0) {
                uint32_t scanline = (pw * bpp * dec->ihdr.bit_depth + 7) / 8;
                raw_size += (scanline + 1) * ph;
            }
        }
    }
    
    // Decompress
    if (dec->compressed_len < 2) {
        ret = PNG_ERR_CORRUPT;
        goto done;
    }
    
    dec->raw = kmalloc(raw_size);
    if (!dec->raw) {
        ret = PNG_ERR_NOMEM;
        goto done;
    }
    
    ret = png_inflate(dec->compressed + 2, dec->compressed_len - 2,
                      dec->raw, raw_size, &dec->raw_len);
    if (ret != PNG_OK) goto done;
    
    // Allocate output pixels
    dec->pixels = kmalloc(width * height * 4);
    if (!dec->pixels) {
        ret = PNG_ERR_NOMEM;
        goto done;
    }
    
    // Process scanlines
    if (dec->ihdr.interlace == PNG_INTERLACE_NONE) {
        uint32_t scanline_bytes = (width * bpp * dec->ihdr.bit_depth + 7) / 8;
        uint8_t *cur_line = kmalloc(scanline_bytes);
        uint8_t *prev_line = kzalloc(scanline_bytes);
        
        if (!cur_line || !prev_line) {
            if (cur_line) kfree(cur_line);
            if (prev_line) kfree(prev_line);
            ret = PNG_ERR_NOMEM;
            goto done;
        }
        
        uint8_t *raw_ptr = dec->raw;
        for (uint32_t y = 0; y < height; y++) {
            int filter = *raw_ptr++;
            memcpy(cur_line, raw_ptr, scanline_bytes);
            raw_ptr += scanline_bytes;
            
            unfilter_row(cur_line, y > 0 ? prev_line : NULL, scanline_bytes, bpp, filter);
            convert_scanline_to_bgra(dec, cur_line, dec->pixels + y * width, width);
            
            uint8_t *tmp = prev_line;
            prev_line = cur_line;
            cur_line = tmp;
        }
        
        kfree(cur_line);
        kfree(prev_line);
    } else {
        // Adam7 interlacing
        uint8_t *raw_ptr = dec->raw;
        
        // Initialize output to transparent
        memset(dec->pixels, 0, width * height * 4);
        
        for (int pass = 0; pass < 7; pass++) {
            uint32_t pw = adam7_pass_width(width, pass);
            uint32_t ph = adam7_pass_height(height, pass);
            
            if (pw == 0 || ph == 0) continue;
            
            uint32_t scanline_bytes = (pw * bpp * dec->ihdr.bit_depth + 7) / 8;
            uint8_t *cur_line = kmalloc(scanline_bytes);
            uint8_t *prev_line = kzalloc(scanline_bytes);
            uint32_t *row_pixels = kmalloc(pw * 4);
            
            if (!cur_line || !prev_line || !row_pixels) {
                if (cur_line) kfree(cur_line);
                if (prev_line) kfree(prev_line);
                if (row_pixels) kfree(row_pixels);
                ret = PNG_ERR_NOMEM;
                goto done;
            }
            
            int x_start = adam7[pass][0];
            int y_start = adam7[pass][1];
            int x_step = adam7[pass][2];
            int y_step = adam7[pass][3];
            
            for (uint32_t py = 0; py < ph; py++) {
                int filter = *raw_ptr++;
                memcpy(cur_line, raw_ptr, scanline_bytes);
                raw_ptr += scanline_bytes;
                
                unfilter_row(cur_line, py > 0 ? prev_line : NULL, scanline_bytes, bpp, filter);
                convert_scanline_to_bgra(dec, cur_line, row_pixels, pw);
                
                // Place pixels at interlaced positions
                uint32_t dst_y = y_start + py * y_step;
                for (uint32_t px = 0; px < pw; px++) {
                    uint32_t dst_x = x_start + px * x_step;
                    dec->pixels[dst_y * width + dst_x] = row_pixels[px];
                }
                
                uint8_t *tmp = prev_line;
                prev_line = cur_line;
                cur_line = tmp;
            }
            
            kfree(cur_line);
            kfree(prev_line);
            kfree(row_pixels);
        }
    }
    
    // Success
    img->width = width;
    img->height = height;
    img->pixels = dec->pixels;
    dec->pixels = NULL;  // Transfer ownership
    ret = PNG_OK;
    
done:
    if (dec->compressed) kfree(dec->compressed);
    if (dec->raw) kfree(dec->raw);
    if (dec->pixels) kfree(dec->pixels);
    kfree(dec);
    
    return ret;
}

const char *png_error_string(int err) {
    switch (err) {
        case PNG_OK:               return "Success";
        case PNG_ERR_NULL_PTR:     return "Null pointer";
        case PNG_ERR_INVALID_SIG:  return "Invalid PNG signature";
        case PNG_ERR_UNSUPPORTED:  return "Unsupported PNG format";
        case PNG_ERR_NOMEM:        return "Out of memory";
        case PNG_ERR_CORRUPT:      return "Corrupt PNG data";
        case PNG_ERR_TOO_SMALL:    return "Data too small";
        case PNG_ERR_CRC:          return "CRC mismatch";
        case PNG_ERR_INFLATE:      return "Decompression error";
        case PNG_ERR_INVALID_IHDR: return "Invalid IHDR chunk";
        case PNG_ERR_MISSING_PLTE: return "Missing PLTE chunk";
        case PNG_ERR_CHUNK_SIZE:   return "Invalid chunk size";
        default:                   return "Unknown error";
    }
}
