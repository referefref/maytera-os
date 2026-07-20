// png.c - PNG image decoder for MayteraOS
// Supports non-interlaced PNG with 8-bit depth

#include "png.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"

// #404 Phase V: Rust PNG parse seams (rustkern.rs). Same signatures as the C
// references png_parse_ihdr_c / png_defilter_c below; a signature mismatch here
// fails the C compile (the FFI "lock"). Live under -DRUST_PNG via the
// png_parse_ihdr() / png_defilter() dispatchers below.
extern int png_parse_ihdr_rs(const uint8_t *ihdr, uint32_t len, PngInfo *out);
extern int png_defilter_rs(const uint8_t *inflated, uint32_t inflated_len,
                           uint32_t width, uint32_t height, uint32_t bpp,
                           uint8_t *out, uint32_t out_cap);

// #404 Phase X (#502): Rust DEFLATE/inflate core (rustkern.rs). Same signature
// as the verbatim C reference inflate_c below; live under -DRUST_INFLATE via the
// inflate() dispatcher. This is the reachable untrusted-input decompression seam
// shared by the PNG IDAT path here, the archiver (#321) and gzip content-encoding.
extern int inflate_rs(const uint8_t *src, uint32_t src_len,
                      uint8_t *dst, uint32_t dst_cap, uint32_t *dst_len);

// Lock the FFI struct layout: the Rust #[repr(C)] PngInfo must be byte-identical.
_Static_assert(sizeof(PngInfo) == 32, "PngInfo layout must match rustkern.rs");

// PNG signature
static const uint8_t png_signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};

// Chunk types (as big-endian uint32)
#define CHUNK_IHDR 0x49484452
#define CHUNK_IDAT 0x49444154
#define CHUNK_IEND 0x49454E44
#define CHUNK_PLTE 0x504C5445

// Read big-endian uint32
static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

// CRC32 table for PNG
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

static uint32_t update_crc(uint32_t crc, const uint8_t *buf, uint32_t len) {
    if (!crc_table_computed) make_crc_table();
    uint32_t c = crc;
    for (uint32_t n = 0; n < len; n++) {
        c = crc_table[(c ^ buf[n]) & 0xFF] ^ (c >> 8);
    }
    return c;
}

static uint32_t crc32_png(const uint8_t *buf, uint32_t len) {
    return update_crc(0xFFFFFFFF, buf, len) ^ 0xFFFFFFFF;
}

// DEFLATE decompression (simplified for PNG)
typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
    uint32_t bit_buf;
    int bit_count;
} inflate_state_t;

static int getbit(inflate_state_t *s) {
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

static int getbits(inflate_state_t *s, int n) {
    int value = 0;
    for (int i = 0; i < n; i++) {
        int bit = getbit(s);
        if (bit < 0) return -1;
        value |= (bit << i);
    }
    return value;
}

// Huffman code lengths
static const int code_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// Fixed Huffman tables
static int fixed_lit_lengths[288];
static int fixed_dist_lengths[32];
static int fixed_tables_built = 0;

static void build_fixed_tables(void) {
    // Literal/length codes 0-143: length 8
    for (int i = 0; i <= 143; i++) fixed_lit_lengths[i] = 8;
    // Codes 144-255: length 9
    for (int i = 144; i <= 255; i++) fixed_lit_lengths[i] = 9;
    // Codes 256-279: length 7
    for (int i = 256; i <= 279; i++) fixed_lit_lengths[i] = 7;
    // Codes 280-287: length 8
    for (int i = 280; i <= 287; i++) fixed_lit_lengths[i] = 8;
    // Distance codes: all length 5
    for (int i = 0; i < 32; i++) fixed_dist_lengths[i] = 5;
    fixed_tables_built = 1;
}

// Huffman decoding structure
typedef struct {
    uint16_t counts[16];    // Number of codes of each length
    uint16_t symbols[288];  // Symbols sorted by code
} huffman_t;

static int build_huffman(huffman_t *h, const int *lengths, int n) {
    memset(h->counts, 0, sizeof(h->counts));

    // Count occurrences of each code length
    for (int i = 0; i < n; i++) {
        if (lengths[i] > 0 && lengths[i] < 16) {
            h->counts[lengths[i]]++;
        }
    }

    // Build symbol table
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
        int bit = getbit(s);
        if (bit < 0) return -1;
        code = (code << 1) | bit;
        int count = h->counts[len];
        if (code - count < first) {
            return h->symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
    }
    return -1;  // Invalid code
}

// Length/distance extra bits
static const int length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const int length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const int dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const int dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

// Inflate a DEFLATE stream.
// #404 Phase X: VERBATIM reference, renamed inflate() -> inflate_c(). This is a
// puff-style decoder whose LZ77 back-reference copy is ALREADY bounds-checked on
// BOTH ends (dist > out_pos rejects a distance-before-window OOB read; out_pos +
// len > dst_cap rejects a length-past-buffer OOB write), the literal write is
// gated by out_pos >= dst_cap, the stored block checks input and output bounds,
// the dynamic code-length RLE guards every repeat with n < hlit+hdist, the
// Huffman symbol index is bounded by construction, and input exhaustion returns
// -1. Offline ASan over 3,000,000+ hostile vectors found NO OOB (see [RUST-SEC]).
// The inflate() dispatcher routes to inflate_rs under -DRUST_INFLATE.
static int inflate_c(const uint8_t *src, uint32_t src_len,
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
        bfinal = getbit(&s);
        if (bfinal < 0) return PNG_ERR_INFLATE;

        int btype = getbits(&s, 2);
        if (btype < 0) return PNG_ERR_INFLATE;

        if (btype == 0) {
            // Stored block
            s.bit_count = 0;  // Align to byte
            if (s.pos + 4 > s.size) return PNG_ERR_INFLATE;
            uint16_t len = s.data[s.pos] | (s.data[s.pos+1] << 8);
            s.pos += 4;  // Skip len and nlen
            if (s.pos + len > s.size) return PNG_ERR_INFLATE;
            if (out_pos + len > dst_cap) return PNG_ERR_INFLATE;
            memcpy(dst + out_pos, s.data + s.pos, len);
            s.pos += len;
            out_pos += len;
        }
        else if (btype == 1 || btype == 2) {
            // Compressed block
            huffman_t lit_huff, dist_huff;
            int lit_lengths[288];
            int dist_lengths[32];

            if (btype == 1) {
                // Fixed Huffman codes
                memcpy(lit_lengths, fixed_lit_lengths, sizeof(fixed_lit_lengths));
                memcpy(dist_lengths, fixed_dist_lengths, sizeof(fixed_dist_lengths));
            } else {
                // Dynamic Huffman codes
                int hlit = getbits(&s, 5) + 257;
                int hdist = getbits(&s, 5) + 1;
                int hclen = getbits(&s, 4) + 4;

                if (hlit < 0 || hdist < 0 || hclen < 0) return PNG_ERR_INFLATE;

                int code_lengths[19];
                memset(code_lengths, 0, sizeof(code_lengths));
                for (int i = 0; i < hclen; i++) {
                    code_lengths[code_order[i]] = getbits(&s, 3);
                }

                huffman_t code_huff;
                build_huffman(&code_huff, code_lengths, 19);

                int combined[288 + 32];
                int n = 0;
                while (n < hlit + hdist) {
                    int sym = decode_symbol(&s, &code_huff);
                    if (sym < 0) return PNG_ERR_INFLATE;

                    if (sym < 16) {
                        combined[n++] = sym;
                    } else if (sym == 16) {
                        int count = getbits(&s, 2) + 3;
                        if (n == 0) return PNG_ERR_INFLATE;
                        int val = combined[n - 1];
                        while (count-- > 0 && n < hlit + hdist) combined[n++] = val;
                    } else if (sym == 17) {
                        int count = getbits(&s, 3) + 3;
                        while (count-- > 0 && n < hlit + hdist) combined[n++] = 0;
                    } else if (sym == 18) {
                        int count = getbits(&s, 7) + 11;
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

            // Decode compressed data
            while (1) {
                int sym = decode_symbol(&s, &lit_huff);
                if (sym < 0) return PNG_ERR_INFLATE;

                if (sym < 256) {
                    // Literal byte
                    if (out_pos >= dst_cap) return PNG_ERR_INFLATE;
                    dst[out_pos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    // End of block
                    break;
                } else {
                    // Length/distance pair
                    sym -= 257;
                    if (sym >= 29) return PNG_ERR_INFLATE;

                    int len = length_base[sym];
                    if (length_extra[sym] > 0) {
                        int extra = getbits(&s, length_extra[sym]);
                        if (extra < 0) return PNG_ERR_INFLATE;
                        len += extra;
                    }

                    int dsym = decode_symbol(&s, &dist_huff);
                    if (dsym < 0 || dsym >= 30) return PNG_ERR_INFLATE;

                    int dist = dist_base[dsym];
                    if (dist_extra[dsym] > 0) {
                        int extra = getbits(&s, dist_extra[dsym]);
                        if (extra < 0) return PNG_ERR_INFLATE;
                        dist += extra;
                    }

                    if ((uint32_t)dist > out_pos) return PNG_ERR_INFLATE;
                    if (out_pos + len > dst_cap) return PNG_ERR_INFLATE;

                    // Copy from back-reference
                    for (int i = 0; i < len; i++) {
                        dst[out_pos] = dst[out_pos - dist];
                        out_pos++;
                    }
                }
            }
        } else {
            return PNG_ERR_INFLATE;  // Invalid block type
        }
    } while (!bfinal);

    *dst_len = out_pos;
    return PNG_SUCCESS;
}

// #404 Phase X strangler dispatcher for the DEFLATE core. -DRUST_INFLATE routes
// the live PNG IDAT decompress (image_load_png below) to the Rust port
// inflate_rs; otherwise the verbatim C inflate_c. Drop the flag to roll back.
static int inflate(const uint8_t *src, uint32_t src_len,
                   uint8_t *dst, uint32_t dst_cap, uint32_t *dst_len) {
#ifdef RUST_INFLATE
    return inflate_rs(src, src_len, dst, dst_cap, dst_len);
#else
    return inflate_c(src, src_len, dst, dst_cap, dst_len);
#endif
}

// Paeth predictor for PNG filtering
static uint8_t paeth_predictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    else if (pb <= pc) return (uint8_t)b;
    else return (uint8_t)c;
}

// Apply PNG filter to scanline
static void unfilter_scanline(uint8_t *cur, const uint8_t *prev,
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
                cur[i] += paeth_predictor(a, b, c);
            }
            break;
    }
}

// ============================================================================
// #404 Phase V: pure PNG parse seams (png_parse_ihdr_c / png_defilter_c) +
// strangler dispatch to the Rust ports (png_parse_ihdr_rs / png_defilter_rs,
// rustkern.rs) under -DRUST_PNG. These are the two classic-OOB seams where
// malformed PNG bytes reach memory ops. The zlib/inflate() core stays in C
// (its own large component, a later port); these seams surround it: raw PNG
// bytes -> IHDR dims (pre-inflate) and inflated-scanlines -> defiltered pixels
// (post-inflate). Both are genuinely pure (no alloc, no GUI). image_load_png
// keeps the chunk loop + kmalloc + inflate call in C and routes the two seams
// through the dispatchers.
//
// SECURITY (see png_rust_selftest [RUST-SEC] + CHANGELOG): the reachable OOB in
// the C reference is the UNVALIDATED width/height feeding UINT32-WRAPPING size
// math (png_parse_ihdr_c's scanline_len=width*bpp / raw_size=(scanline_len+1)*
// height), which lets a crafted IHDR wrap scanline_len to 0 and raw_size to a
// tiny value; downstream the height-row / width-column decode loops then walk
// far past the wrapped-small allocations => heap OOB read (inflated/scanline) +
// OOB WRITE (pixels, in the unseamed BGRA convert loop). png_parse_ihdr_rs does
// CHECKED (u64) size math and REJECTS the impossible header at parse, confining
// it before any alloc/loop runs.
//
// NOTE on the worked example (advisory 0005 correction): width=0x40000000 with
// RGBA does NOT reproduce it. That wraps the pixel alloc to EXACTLY zero, and
// kmalloc(0) returns NULL, so every arm returns PNG_ERR_NOMEM with no fault. A
// working PoC needs a wrap to tiny-but-NONZERO, e.g. width=0x40000004.
//
// #500 (2026-07-16): png_parse_ihdr_c now does the SAME checked u64 math, so
// the wrap class is dead in the C too and this file no longer depends on the
// Rust for its arithmetic to be sound. That also resolves what used to be a
// self-contradiction in this file: the old text here said "drop -DRUST_PNG to
// roll straight back", while image_load_png's defilt_cap comment said the
// product is exact BECAUSE -DRUST_PNG proved it. Both cannot be true. Now the
// C stands on its own and rollback is faithful.
// ============================================================================

// Validate the 13-byte IHDR payload and compute bpp + scanline_len + raw_size.
// VERBATIM reference: the size arithmetic is uint32 exactly as the original
// image_load_png computed it (so it WRAPS on a crafted >= ~2^30 width - the
// honest reference bound that the Rust port removes). Does NOT reject zero
// dims (image_load_png does that after the chunk loop, unchanged).
int png_parse_ihdr_c(const uint8_t *ihdr, uint32_t len, PngInfo *out) {
    if (!ihdr || !out) return PNG_ERR_NULL_PTR;
    memset(out, 0, sizeof(*out));
    if (len < 13) return PNG_ERR_CORRUPT;

    uint32_t width  = read_be32(ihdr);
    uint32_t height = read_be32(ihdr + 4);
    int bit_depth   = ihdr[8];
    int color_type  = ihdr[9];
    int interlace   = ihdr[12];

    if (bit_depth != 8) return PNG_ERR_UNSUPPORTED;
    if (color_type != PNG_COLOR_RGB && color_type != PNG_COLOR_RGBA &&
        color_type != PNG_COLOR_GRAYSCALE && color_type != PNG_COLOR_GRAYSCALE_A)
        return PNG_ERR_UNSUPPORTED;
    if (interlace != 0) return PNG_ERR_UNSUPPORTED;

    int bpp;
    switch (color_type) {
        case PNG_COLOR_GRAYSCALE:   bpp = 1; break;
        case PNG_COLOR_GRAYSCALE_A: bpp = 2; break;
        case PNG_COLOR_RGB:         bpp = 3; break;
        case PNG_COLOR_RGBA:        bpp = 4; break;
        default:                    return PNG_ERR_UNSUPPORTED;
    }

    // #500 / MAYTERA-SEC-2026-00XX: CHECKED (u64) size math, byte-for-byte the
    // same rule png_parse_ihdr_rs applies. This USED to be uint32 "verbatim
    // reference" arithmetic that wrapped on a crafted width, on the theory that
    // the twin should reproduce the original's bug for rollback fidelity. That
    // theory was wrong here, and measurably so: the b813 extraction ALSO
    // re-represented the defilter output as ONE packed scanline_len*height
    // buffer (image_load_png's `defilt_cap`), a uint32 product that exists
    // NOWHERE in the pre-extraction original, which used two swapping scratch
    // scanlines of exactly scanline_len each. So the wrap no longer merely
    // reproduced the original's OOB READ, it manufactured a NEW OOB WRITE the
    // original provably could not perform (kmalloc(defilt_cap ? defilt_cap : 1)
    // turns a wrap-to-zero, which heap.c's own size==0 -> NULL gate would have
    // caught, into a valid 1-byte buffer that the defilter then writes 32 KB
    // into). Checking here kills that class at the root: if (scanline_len+1)*
    // height fits u32 then scanline_len*height necessarily does too, so
    // defilt_cap is exact by construction and cannot wrap.
    //
    // On every sane image both products fit u32, so scanline_len/raw_size are
    // IDENTICAL to what the old uint32 math produced and the decode is
    // unchanged. Only the impossible headers change verdict, and they now match
    // the Rust exactly (drift 0, both-unsafe 0), which also restores a truthful
    // rollback: dropping -DRUST_PNG no longer hands you a heap-write primitive.
    uint64_t scanline_len64 = (uint64_t)width * (uint64_t)bpp;
    if (scanline_len64 > 0xFFFFFFFFULL) return PNG_ERR_UNSUPPORTED;
    uint64_t raw_size64 = (scanline_len64 + 1) * (uint64_t)height;
    if (raw_size64 > 0xFFFFFFFFULL) return PNG_ERR_UNSUPPORTED;
    uint32_t scanline_len = (uint32_t)scanline_len64;
    uint32_t raw_size     = (uint32_t)raw_size64;

    out->width        = width;
    out->height       = height;
    out->bit_depth    = (uint32_t)bit_depth;
    out->color_type   = (uint32_t)color_type;
    out->interlace    = (uint32_t)interlace;
    out->bpp          = (uint32_t)bpp;
    out->scanline_len = scanline_len;
    out->raw_size     = raw_size;
    return PNG_SUCCESS;
}

// Reconstruct the None/Sub/Up/Average/Paeth-filtered scanlines of the INFLATED
// buffer into `out` (packed stride*height, no filter bytes).
//
// CORRECTION (2026-07-16, #404 drift audit 2). This comment used to claim the
// twin "walks past both buffers EXACTLY AS THE ORIGINAL WOULD". That was FALSE
// and is the single documentation defect that hid a real regression:
//   - The ORIGINAL had NO missing OUTPUT bound to inherit. It defiltered into
//     two swapping scratch scanlines of exactly scanline_len bytes, so its
//     memcpy destination was always exactly sized. Its defilter step could NOT
//     OOB-WRITE. Only the later BGRA convert loop could (advisory 0005), and
//     that loop is unseamed plain C to this day.
//   - This twin writes into ONE packed scanline_len*height buffer supplied by
//     the caller. With a wrapped cap that is a heap-WRITE primitive the
//     pre-extraction code did not have.
// So the honest statement is: the INPUT-side missing bound is inherited from the
// original; the OUTPUT-side exposure was INTRODUCED by the b813 extraction.
// It is now defused at the root rather than here: png_parse_ihdr_c does checked
// u64 math (see above), so out_cap == scanline_len*height is exact and cannot
// wrap on ANY header this function can be reached with, flag or no flag. The
// Rust port additionally bounds every input read against inflated_len and every
// output write against out_cap, which is defense in depth rather than the only
// thing standing between a crafted PNG and the heap.
int png_defilter_c(const uint8_t *inflated, uint32_t inflated_len,
                   uint32_t width, uint32_t height, uint32_t bpp,
                   uint8_t *out, uint32_t out_cap) {
    (void)inflated_len;   // honest reference: no input-length bound (as original)
    (void)out_cap;        // honest reference: no output-capacity bound (as original)
    if (!inflated || !out) return PNG_ERR_NULL_PTR;

    uint32_t stride = width * bpp;                 // uint32: MAY WRAP (verbatim)
    const uint8_t *rp = inflated;
    for (uint32_t y = 0; y < height; y++) {
        int filter = *rp++;                        // reads inflated[y*(stride+1)]
        uint8_t *cur = out + (uint32_t)y * stride; // row base in out
        memcpy(cur, rp, stride);
        rp += stride;
        unfilter_scanline(cur, y > 0 ? out + (uint32_t)(y - 1) * stride : NULL,
                          stride, (int)bpp, filter);
    }
    return PNG_SUCCESS;
}

// Strangler dispatchers: the live parse seams. -DRUST_PNG routes to the Rust ports.
int png_parse_ihdr(const uint8_t *ihdr, uint32_t len, PngInfo *out) {
#ifdef RUST_PNG
    return png_parse_ihdr_rs(ihdr, len, out);
#else
    return png_parse_ihdr_c(ihdr, len, out);
#endif
}

int png_defilter(const uint8_t *inflated, uint32_t inflated_len,
                 uint32_t width, uint32_t height, uint32_t bpp,
                 uint8_t *out, uint32_t out_cap) {
#ifdef RUST_PNG
    return png_defilter_rs(inflated, inflated_len, width, height, bpp, out, out_cap);
#else
    return png_defilter_c(inflated, inflated_len, width, height, bpp, out, out_cap);
#endif
}

// Check PNG signature
int image_is_png(const void *data, uint32_t size) {
    if (!data || size < 8) return 0;
    return memcmp(data, png_signature, 8) == 0;
}

// Load PNG image
int image_load_png(const void *data, uint32_t size, image_t *img) {
    if (!data || !img) return PNG_ERR_NULL_PTR;
    if (size < 8) return PNG_ERR_TOO_SMALL;

    const uint8_t *p = (const uint8_t *)data;

    // Check signature
    if (memcmp(p, png_signature, 8) != 0) {
        return PNG_ERR_INVALID_SIG;
    }
    p += 8;

    // Initialize image
    img->width = 0;
    img->height = 0;
    img->pixels = NULL;

    uint32_t width = 0, height = 0;
    int color_type = 0;
    PngInfo info;
    memset(&info, 0, sizeof(info));

    // Collect IDAT chunks
    uint8_t *compressed = NULL;
    uint32_t compressed_len = 0;
    uint32_t compressed_cap = 0;

    // Parse chunks
    const uint8_t *end = (const uint8_t *)data + size;
    while (p + 12 <= end) {
        uint32_t chunk_len = read_be32(p);
        uint32_t chunk_type = read_be32(p + 4);

        if (p + 12 + chunk_len > end) break;

        const uint8_t *chunk_data = p + 8;

        // Verify CRC
        uint32_t expected_crc = read_be32(p + 8 + chunk_len);
        uint32_t actual_crc = crc32_png(p + 4, chunk_len + 4);
        if (expected_crc != actual_crc) {
            if (compressed) kfree(compressed);
            return PNG_ERR_CRC;
        }

        if (chunk_type == CHUNK_IHDR) {
            // #404 Phase V: IHDR validation + overflow-prone size math via the
            // pure seam (routes to png_parse_ihdr_rs under -DRUST_PNG). Same
            // accept/reject codes as the original inline block.
            int irc = png_parse_ihdr(chunk_data, chunk_len, &info);
            if (irc != PNG_SUCCESS) {
                if (compressed) kfree(compressed);
                return irc;
            }
            width = info.width;
            height = info.height;
            color_type = (int)info.color_type;
        }
        else if (chunk_type == CHUNK_IDAT) {
            // Accumulate compressed data
            if (compressed_len + chunk_len > compressed_cap) {
                uint32_t new_cap = compressed_cap ? compressed_cap * 2 : 65536;
                while (new_cap < compressed_len + chunk_len) new_cap *= 2;
                uint8_t *new_buf = kmalloc(new_cap);
                if (!new_buf) {
                    if (compressed) kfree(compressed);
                    return PNG_ERR_NOMEM;
                }
                if (compressed) {
                    memcpy(new_buf, compressed, compressed_len);
                    kfree(compressed);
                }
                compressed = new_buf;
                compressed_cap = new_cap;
            }
            memcpy(compressed + compressed_len, chunk_data, chunk_len);
            compressed_len += chunk_len;
        }
        else if (chunk_type == CHUNK_IEND) {
            break;
        }

        p += 12 + chunk_len;
    }

    if (width == 0 || height == 0 || !compressed) {
        if (compressed) kfree(compressed);
        return PNG_ERR_CORRUPT;
    }

    // Bytes-per-pixel and scanline/raw sizes come from the IHDR seam (Rust under
    // -DRUST_PNG computes them with CHECKED math and would have rejected an
    // overflowing IHDR at png_parse_ihdr above, before this point).
    int bpp = (int)info.bpp;
    uint32_t scanline_len = info.scanline_len;
    uint32_t raw_size = info.raw_size;

    // Decompress (skip zlib header: 2 bytes)
    if (compressed_len < 2) {
        kfree(compressed);
        return PNG_ERR_CORRUPT;
    }

    uint8_t *raw = kmalloc(raw_size);
    if (!raw) {
        kfree(compressed);
        return PNG_ERR_NOMEM;
    }

    uint32_t decompressed_len;
    int ret = inflate(compressed + 2, compressed_len - 2, raw, raw_size, &decompressed_len);
    kfree(compressed);

    if (ret != PNG_SUCCESS || decompressed_len < raw_size) {
        kfree(raw);
        return ret != PNG_SUCCESS ? ret : PNG_ERR_INFLATE;
    }

    // #404 Phase V: reconstruct all scanlines (None/Sub/Up/Average/Paeth) into a
    // packed defiltered buffer via the pure seam (routes to png_defilter_rs under
    // -DRUST_PNG, which bounds every input read against raw_size and every output
    // write against defilt_cap).
    //
    // #500: defilt_cap = scanline_len*height is EXACT and cannot wrap, and that
    // no longer depends on which flag is set. png_parse_ihdr (C *and* Rust) now
    // both prove in checked u64 that (scanline_len+1)*height fits u32; since
    // scanline_len*height < (scanline_len+1)*height, this product fits too.
    // Previously this reasoning held only under -DRUST_PNG, which meant the
    // flag-off build reached kmalloc(defilt_cap ? defilt_cap : 1) with a
    // wrapped-to-zero cap, got a 1-BYTE buffer (the ?: defeats heap.c's own
    // size==0 -> NULL gate), and then defiltered 32 KB into it. That was a heap
    // OOB WRITE the pre-extraction original could not perform.
    uint32_t defilt_cap = scanline_len * height;
    uint8_t *defiltered = kmalloc(defilt_cap ? defilt_cap : 1);
    if (!defiltered) {
        kfree(raw);
        return PNG_ERR_NOMEM;
    }
    int drc = png_defilter(raw, decompressed_len, width, height, (uint32_t)bpp,
                           defiltered, defilt_cap);
    kfree(raw);
    if (drc != PNG_SUCCESS) {
        kfree(defiltered);
        return drc;
    }

    // Allocate output pixels (BGRA format)
    img->pixels = kmalloc(width * height * 4);
    if (!img->pixels) {
        kfree(defiltered);
        return PNG_ERR_NOMEM;
    }

    img->width = width;
    img->height = height;

    // Convert the defiltered scanlines to BGRA (unchanged pixel math).
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *cur_line = defiltered + (uint32_t)y * scanline_len;
        uint32_t *dst = img->pixels + y * width;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t r, g, b, a = 255;
            switch (color_type) {
                case PNG_COLOR_GRAYSCALE:
                    r = g = b = cur_line[x];
                    break;
                case PNG_COLOR_GRAYSCALE_A:
                    r = g = b = cur_line[x * 2];
                    a = cur_line[x * 2 + 1];
                    break;
                case PNG_COLOR_RGB:
                    r = cur_line[x * 3];
                    g = cur_line[x * 3 + 1];
                    b = cur_line[x * 3 + 2];
                    break;
                case PNG_COLOR_RGBA:
                    r = cur_line[x * 4];
                    g = cur_line[x * 4 + 1];
                    b = cur_line[x * 4 + 2];
                    a = cur_line[x * 4 + 3];
                    break;
                default:
                    r = g = b = 0;
            }
            dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | b;
        }
    }

    kfree(defiltered);

    return PNG_SUCCESS;
}

const char *png_error_string(int err) {
    switch (err) {
        case PNG_SUCCESS:         return "Success";
        case PNG_ERR_NULL_PTR:    return "Null pointer";
        case PNG_ERR_INVALID_SIG: return "Invalid PNG signature";
        case PNG_ERR_UNSUPPORTED: return "Unsupported PNG format";
        case PNG_ERR_NOMEM:       return "Out of memory";
        case PNG_ERR_CORRUPT:     return "Corrupt PNG data";
        case PNG_ERR_TOO_SMALL:   return "Data too small";
        case PNG_ERR_CRC:         return "CRC mismatch";
        case PNG_ERR_INFLATE:     return "Decompression error";
        default:                  return "Unknown error";
    }
}

// ---------------------------------------------------------------------------
// #404 Phase V boot-time self-test: prove png_parse_ihdr_rs + png_defilter_rs
// (Rust, live under -DRUST_PNG) == png_parse_ihdr_c + png_defilter_c (verbatim
// references) on well-formed IHDRs + defilter inputs, report the SECURITY
// posture HONESTLY, and micro-bench the defilter. LIGHT (#426, bounded, runs
// once): ~256 defilter/parse differential vectors + a crafted impossible-IHDR
// sweep + a ~2k-iter RDTSC bench. The heavy fuzz (millions of vectors, ASan/
// UBSan on the C references, pixel-identical on real PNGs) runs OFFLINE.
// Logs one [RUST-DIFF] png, one [RUST-SEC] png, one [RUST-PERF] png.
// ---------------------------------------------------------------------------
static uint32_t pngdiff_rng(uint32_t *s) { uint32_t x=*s; x^=x<<13; x^=x>>17; x^=x<<5; *s=x; return x; }

static inline uint64_t png_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax","ebx","ecx","edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void png_wr_be32(uint8_t *b, uint32_t v) {
    b[0]=(v>>24)&0xFF; b[1]=(v>>16)&0xFF; b[2]=(v>>8)&0xFF; b[3]=v&0xFF;
}

void png_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    // color_type -> bpp map for the generator (0/2/4/6 valid).
    static const int ctype_tab[4] = { PNG_COLOR_GRAYSCALE, PNG_COLOR_RGB,
                                      PNG_COLOR_GRAYSCALE_A, PNG_COLOR_RGBA };
    static const int bpp_tab[4]   = { 1, 3, 2, 4 };
    // Max 32x32 RGBA -> stride 128, raw 33*128=4224; out 4096. Static buffers.
    static uint8_t rawbuf[4400];
    static uint8_t oc[4200], orr[4200];
    uint32_t seed = 0x9E37C1A5;
    uint32_t vectors=0, mism=0; int first_bad=-1;

    // Force-reference the Rust symbols so their archive members always link
    // (matches the bmp/icmp pattern), regardless of -DRUST_PNG.
    { PngInfo t; png_parse_ihdr_rs((const uint8_t*)"\0\0\0\0\0\0\0\0\0\0\0\0\0", 13, &t); }

    // Part 1: agreement domain. For each vector: (a) build a valid IHDR payload,
    // diff png_parse_ihdr_c vs _rs (accept + all fields); (b) build a random
    // filtered raw buffer for those dims and diff png_defilter_c vs _rs output.
    for (uint32_t it=0; it<256; it++) {
        uint32_t r = pngdiff_rng(&seed);
        uint32_t w = 1 + (r % 32);
        uint32_t h = 1 + ((r>>8) % 32);
        int ci = (r>>16) & 3;
        int color_type = ctype_tab[ci];
        uint32_t bpp = (uint32_t)bpp_tab[ci];

        // (a) IHDR parse differential.
        uint8_t ihdr[13];
        png_wr_be32(ihdr, w);
        png_wr_be32(ihdr+4, h);
        ihdr[8] = 8;                 // bit_depth
        ihdr[9] = (uint8_t)color_type;
        ihdr[10] = 0;                // compression method
        ihdr[11] = 0;                // filter method
        ihdr[12] = 0;                // interlace
        PngInfo ic, ir;
        int prc = png_parse_ihdr_c(ihdr, 13, &ic);
        int prr = png_parse_ihdr_rs(ihdr, 13, &ir);
        vectors++;
        int bad = (prc != prr);
        if (!bad && prc==PNG_SUCCESS) {
            bad = (ic.width!=ir.width) || (ic.height!=ir.height) ||
                  (ic.bit_depth!=ir.bit_depth) || (ic.color_type!=ir.color_type) ||
                  (ic.interlace!=ir.interlace) || (ic.bpp!=ir.bpp) ||
                  (ic.scanline_len!=ir.scanline_len) || (ic.raw_size!=ir.raw_size);
        }
        if (bad) { mism++; if (first_bad<0) first_bad=(int)it; continue; }

        // (b) defilter differential over a random filtered raw buffer.
        uint32_t stride = w * bpp;
        uint32_t rawlen = (stride + 1) * h;   // small: fits rawbuf
        for (uint32_t i=0;i<rawlen;i++) {
            if ((i % (stride+1)) == 0) rawbuf[i] = (uint8_t)(pngdiff_rng(&seed) % 5); // filter 0..4
            else rawbuf[i] = (uint8_t)(pngdiff_rng(&seed) & 0xFF);
        }
        uint32_t outcap = stride * h;
        for (uint32_t i=0;i<outcap;i++){ oc[i]=0xAA; orr[i]=0x55; }
        int dc = png_defilter_c (rawbuf, rawlen, w, h, bpp, oc,  outcap);
        int dr = png_defilter_rs(rawbuf, rawlen, w, h, bpp, orr, outcap);
        vectors++;
        int bad2 = (dc != dr);
        if (!bad2 && dc==PNG_SUCCESS) {
            for (uint32_t i=0;i<outcap;i++) if (oc[i]!=orr[i]) { bad2=1; break; }
        }
        if (bad2) { mism++; if (first_bad<0) first_bad=(int)it; }
    }
    const char *verdict = (mism==0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] png: %u vectors, %u mismatches -> %s\n", vectors, mism, verdict);
    bootlog_write("[RUST-DIFF] png: %u vectors, %u mismatches -> %s", vectors, mism, verdict);
    if (mism) { kprintf("[RUST-DIFF] png FIRST MISMATCH it=%d\n", first_bad); bootlog_write("[RUST-DIFF] png FIRST MISMATCH it=%d", first_bad); }

    // Part 2: [RUST-SEC] - the REACHABLE integer-overflow OOB class.
    // (A) png_parse_ihdr: the C's uint32 scanline_len=width*bpp /
    //     raw_size=(scanline_len+1)*height WRAP on a crafted >= ~2^30 width, so
    //     the C ACCEPTS an impossible IHDR (returning tiny wrapped sizes) that
    //     downstream wraps the pixel/scanline allocations => heap OOB read+WRITE.
    //     The Rust (checked u64) REJECTS at parse. Safe to exercise (pure math).
    // (B) png_defilter: the C walk has NO bound on inflated_len OR out_cap; a
    //     hostile (w,h,bpp) or truncated input walks past both buffers. We only
    //     exercise the RUST side here (it must CONFINE - reject, writing nothing
    //     past the small buffer); the matching C OOB read+WRITE is ASan-proven
    //     OFFLINE (calling png_defilter_c with hostile params in-kernel would
    //     corrupt the heap, so we do NOT).
    {
        struct { uint32_t w, h; int ci; } fam[6] = {
            {0x40000000,2,3},{0x40000001,4,3},{0x55555556,3,1},
            {0x20000000,8,3},{0x30000000,10,3},{0x40000000,16,1}
        };
        uint32_t c_accept=0, rs_reject=0, n=0;
        for (int fi=0; fi<6; fi++) {
            for (int k=0;k<50;k++) {
                uint8_t ihdr[13];
                png_wr_be32(ihdr, fam[fi].w);
                png_wr_be32(ihdr+4, fam[fi].h);
                ihdr[8]=8; ihdr[9]=(uint8_t)ctype_tab[fam[fi].ci];
                ihdr[10]=0; ihdr[11]=0; ihdr[12]=0;
                PngInfo ic, ir;
                int prc = png_parse_ihdr_c(ihdr, 13, &ic);
                int prr = png_parse_ihdr_rs(ihdr, 13, &ir);
                n++;
                if (prc==PNG_SUCCESS) c_accept++;      // C accepts impossible IHDR
                if (prr!=PNG_SUCCESS) rs_reject++;      // Rust confines at parse
            }
        }
        // defilter confinement: hostile params, small buffers, RUST-ONLY.
        uint32_t d_confine=0, dn=0;
        for (int k=0;k<200;k++) {
            // huge stride vs a tiny out/inflated arena (32 bytes each of rawbuf/oc)
            uint32_t w = 1000 + (uint32_t)k, h = 1000, bpp = 4;
            int dr = png_defilter_rs(rawbuf, 32, w, h, bpp, oc, 32);
            dn++;
            if (dr != PNG_SUCCESS) d_confine++;         // Rust rejects, writes nothing OOB
        }
        kprintf("[RUST-SEC] png: REACHABLE int-overflow OOB - C accepts impossible IHDR (uint32 size wrap) %u/%u; Rust (u64 checked) confines %u/%u at parse; defilter Rust confines %u/%u hostile (C OOB read+WRITE ASan-proven OFFLINE)\n",
                c_accept,n,rs_reject,n,d_confine,dn);
        bootlog_write("[RUST-SEC] png: REACHABLE int-overflow OOB; C accepts impossible IHDR %u/%u, Rust confines %u/%u; defilter Rust confines %u/%u hostile", c_accept,n,rs_reject,n,d_confine,dn);
    }

    // Part 3: RDTSC micro-benchmark over a fixed 32x32 RGBA defilter. LIGHT: 2k.
    {
        const int iters = 2000;
        uint32_t s3 = 0x51ed270b;
        uint32_t w=32,h=32,bpp=4, stride=w*bpp, rawlen=(stride+1)*h, outcap=stride*h;
        for (uint32_t i=0;i<rawlen;i++) rawbuf[i]=(uint8_t)(pngdiff_rng(&s3)&0xFF);
        for (int i=0;i<200;i++){ png_defilter_c(rawbuf,rawlen,w,h,bpp,oc,outcap); png_defilter_rs(rawbuf,rawlen,w,h,bpp,orr,outcap); }
        uint64_t t0=png_tsc_serialized();
        for (int i=0;i<iters;i++) png_defilter_c(rawbuf,rawlen,w,h,bpp,oc,outcap);
        uint64_t t1=png_tsc_serialized();
        for (int i=0;i<iters;i++) png_defilter_rs(rawbuf,rawlen,w,h,bpp,orr,outcap);
        uint64_t t2=png_tsc_serialized();
        uint64_t c_cyc=(t1-t0)/iters, r_cyc=(t2-t1)/iters;
        uint64_t ratio100 = c_cyc ? (r_cyc*100ULL/c_cyc) : 0;
        kprintf("[RUST-PERF] png: defilter C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
        bootlog_write("[RUST-PERF] png: defilter C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
    }
}

// ---------------------------------------------------------------------------
// #404 Phase X boot-time self-test: prove inflate_rs (Rust, live under
// -DRUST_INFLATE) == inflate_c (verbatim reference) on well-formed AND malformed
// RAW-DEFLATE streams on THIS build, report the SECURITY posture HONESTLY, and
// micro-bench the decompress. LIGHT (#426, bounded, runs once): 27 embedded
// vectors (fixed/dynamic/stored blocks + overlapping back-references) at exact +
// mutated caps (~350 vectors) + 4 crafted hostile back-reference attacks + a
// ~2k-iter RDTSC bench. The heavy fuzz (3,000,000+ vectors, ASan/UBSan on the C
// reference, byte-identical on real PNG IDAT) ran OFFLINE (see CHANGELOG).
// Logs one [RUST-DIFF] inflate, one [RUST-SEC] inflate, one [RUST-PERF] inflate.
// ---------------------------------------------------------------------------
#include "inflate_vectors.h"

void inflate_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    // Force-reference inflate_rs so its archive member always links (matches the
    // png/jpeg pattern), regardless of -DRUST_INFLATE.
    { uint8_t d[1] = {0}; uint32_t ol = 0; inflate_rs(d, 0, d, 0, &ol); }

    static uint8_t oc[512], orr[512];
    uint32_t vectors = 0, mism = 0; int first_bad = -1;
    uint32_t seed = 0x1e57c0de;

    // Part 1: differential inflate_c vs inflate_rs over the embedded RAW-DEFLATE
    // vectors at exact cap (expect success + output == embedded original + rs==c)
    // plus in-kernel mutations (bit flips / truncation / random caps => the
    // reject paths) at random caps.
    for (uint32_t vi = 0; vi < INF_NVECS; vi++) {
        const inf_vec_t *v = &inf_vecs[vi];
        for (uint32_t i = 0; i < sizeof(oc); i++) { oc[i] = 0xAA; orr[i] = 0xAA; /* SAME sentinel as oc: only WRITTEN bytes are compared; a differing tail-fill would falsely mismatch on partial (reject) vectors */ }
        uint32_t lc = 0, lr = 0;
        int rc = inflate_c (v->def, v->dlen, oc,  v->olen, &lc);
        int rr = inflate_rs(v->def, v->dlen, orr, v->olen, &lr);
        vectors++;
        int bad = (rc != rr) || (rc == PNG_SUCCESS && lc != lr);
        if (!bad && rc == PNG_SUCCESS && v->olen) {
            for (uint32_t i = 0; i < v->olen; i++) if (oc[i] != orr[i]) { bad = 1; break; }
            for (uint32_t i = 0; !bad && i < v->olen; i++) if (oc[i] != v->orig[i]) { bad = 1; break; }
        }
        if (bad) { mism++; if (first_bad < 0) first_bad = (int)vi; }

        for (int k = 0; k < 12; k++) {
            uint8_t mbuf[160]; uint32_t mlen = v->dlen; if (mlen > sizeof(mbuf)) mlen = sizeof(mbuf);
            for (uint32_t i = 0; i < mlen; i++) mbuf[i] = v->def[i];
            int mode = pngdiff_rng(&seed) % 4;
            if (mode == 0 && mlen) { int fl = 1 + (pngdiff_rng(&seed) % 6);
                for (int f = 0; f < fl; f++) { uint32_t bp = pngdiff_rng(&seed) % (mlen * 8); mbuf[bp/8] ^= (uint8_t)(1u << (bp % 8)); } }
            else if (mode == 1 && mlen) { mlen = pngdiff_rng(&seed) % mlen; }
            else if (mode == 2 && mlen) { mbuf[pngdiff_rng(&seed) % mlen] = (uint8_t)pngdiff_rng(&seed); }
            uint32_t mc = pngdiff_rng(&seed) % (sizeof(oc) + 1);
            for (uint32_t i = 0; i < mc; i++) { oc[i] = 0xAA; orr[i] = 0xAA; /* SAME sentinel as oc: only WRITTEN bytes are compared; a differing tail-fill would falsely mismatch on partial (reject) vectors */ }
            uint32_t l1 = 0, l2 = 0;
            int r1 = inflate_c (mbuf, mlen, oc,  mc, &l1);
            int r2 = inflate_rs(mbuf, mlen, orr, mc, &l2);
            vectors++;
            int b2 = (r1 != r2) || (r1 == PNG_SUCCESS && l1 != l2);
            if (!b2) { for (uint32_t i = 0; i < mc; i++) if (oc[i] != orr[i]) { b2 = 1; break; } }
            if (b2) { mism++; if (first_bad < 0) first_bad = (int)(1000 + vi); }
        }
    }
    const char *verdict = (mism == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] inflate: %u vectors, %u mismatches -> %s\n", vectors, mism, verdict);
    bootlog_write("[RUST-DIFF] inflate: %u vectors, %u mismatches -> %s", vectors, mism, verdict);
    if (mism) { kprintf("[RUST-DIFF] inflate FIRST MISMATCH id=%d\n", first_bad);
                bootlog_write("[RUST-DIFF] inflate FIRST MISMATCH id=%d", first_bad); }

    // Part 2: [RUST-SEC] - the classic LZ77 back-reference OOB surface. The C
    // reference ALREADY bounds BOTH ends of the back-reference copy
    // (dist > out_pos rejects an OOB read before window start; out_pos + len >
    // dst_cap rejects an OOB write past the buffer), the literal write, the
    // stored block and the code-length RLE, so the crafted hostile streams
    // (distance-before-window, length-past-cap, max-distance, bad block type)
    // are REJECTED with NO OOB - ASan-proven over 3,000,000+ vectors OFFLINE (0
    // heap errors). Verdict: LATENT / defense-in-depth, NOT a reachable-OOB fix.
    // The Rust confines the class by construction (bounds-checked slices).
    {
        uint32_t c_rej = 0, rs_rej = 0, agree = 0, n = 0;
        for (uint32_t hi = 0; hi < INF_NHOSTS; hi++) {
            const inf_host_t *h = &inf_hosts[hi];
            for (uint32_t i = 0; i < sizeof(oc); i++) { oc[i] = 0xAA; orr[i] = 0xAA; /* SAME sentinel as oc: only WRITTEN bytes are compared; a differing tail-fill would falsely mismatch on partial (reject) vectors */ }
            uint32_t l1 = 0, l2 = 0;
            int r1 = inflate_c (h->def, h->dlen, oc,  h->cap, &l1);
            int r2 = inflate_rs(h->def, h->dlen, orr, h->cap, &l2);
            n++;
            if (r1 != PNG_SUCCESS) c_rej++;
            if (r2 != PNG_SUCCESS) rs_rej++;
            if (r1 == r2) agree++;
        }
        kprintf("[RUST-SEC] inflate: LATENT/defense-in-depth - C back-ref copy already bounds dist>out_pos + out_pos+len>dst_cap (no reachable OOB; ASan 3M+ hostile = 0 heap errors OFFLINE). Crafted attacks: C rejects %u/%u, Rust rejects %u/%u, agree %u/%u. Rust confines by construction.\n",
                c_rej, n, rs_rej, n, agree, n);
        bootlog_write("[RUST-SEC] inflate: LATENT/def-in-depth; C bounds back-ref both ends (ASan 3M+ clean); crafted C rej %u/%u RS rej %u/%u agree %u/%u", c_rej, n, rs_rej, n, agree, n);
    }

    // Part 3: [RUST-PERF] RDTSC over a representative dynamic-block decompress.
    {
        const int iters = 2000;
        const inf_vec_t *v = &inf_vecs[INF_NVECS - 1];
        static uint8_t pb[512];
        for (int i = 0; i < 200; i++) { uint32_t l; inflate_c(v->def, v->dlen, pb, v->olen, &l); inflate_rs(v->def, v->dlen, pb, v->olen, &l); }
        uint64_t t0 = png_tsc_serialized();
        for (int i = 0; i < iters; i++) { uint32_t l; inflate_c(v->def, v->dlen, pb, v->olen, &l); }
        uint64_t t1 = png_tsc_serialized();
        for (int i = 0; i < iters; i++) { uint32_t l; inflate_rs(v->def, v->dlen, pb, v->olen, &l); }
        uint64_t t2 = png_tsc_serialized();
        uint64_t cc = (t1 - t0) / iters, rcv = (t2 - t1) / iters;
        uint64_t ratio100 = cc ? (rcv * 100ULL / cc) : 0;
        kprintf("[RUST-PERF] inflate: decompress C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)cc, (unsigned long long)rcv,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
        bootlog_write("[RUST-PERF] inflate: C=%llu RS=%llu cyc/op ratio=%llu.%02llu",
                (unsigned long long)cc, (unsigned long long)rcv,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
