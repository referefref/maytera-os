// image.c - Image loader implementation for MayteraOS
// Supports BMP, PNG, and JPEG formats
#include "image.h"
#include "png.h"
#include "jpeg.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../video/framebuffer.h"

// #404 Phase U: Rust BMP decode port (rustkern.rs). Same signature as
// bmp_decode_c; a mismatch here fails the C compile (the FFI "lock"). Live under
// -DRUST_BMP via the bmp_decode() dispatcher below.
extern int bmp_decode_rs(const uint8_t *buf, uint32_t len,
                         uint32_t *out_px, uint32_t out_cap_px,
                         uint32_t *out_w, uint32_t *out_h);

// Debug output macro (can be disabled for production)
#define IMAGE_DEBUG 1

#if IMAGE_DEBUG
#define IMG_DBG(fmt, ...) kprintf("[IMG] " fmt, ##__VA_ARGS__)
#else
#define IMG_DBG(fmt, ...) ((void)0)
#endif

/**
 * Get error string for error code
 */
const char *image_error_string(int err) {
    switch (err) {
        case IMAGE_SUCCESS:        return "Success";
        case IMAGE_ERR_NULL_PTR:   return "Null pointer";
        case IMAGE_ERR_INVALID_SIG: return "Invalid BMP signature";
        case IMAGE_ERR_UNSUPPORTED: return "Unsupported BMP format";
        case IMAGE_ERR_NOMEM:      return "Out of memory";
        case IMAGE_ERR_CORRUPT:    return "Corrupt BMP data";
        case IMAGE_ERR_TOO_SMALL:  return "Data too small for BMP";
        default:                   return "Unknown error";
    }
}

// ============================================================================
// #404 Phase U: pure BMP decode seam (bmp_decode_c) + strangler dispatch to the
// Rust port (bmp_decode_rs, rustkern.rs) under -DRUST_BMP.
//
// bmp_decode_c is the header-parse + validate + pixel-decode lifted VERBATIM out
// of image_load_bmp, decoding into a caller-BOUNDED output buffer instead of a
// self-allocated one (NO alloc, NO GUI - genuinely pure). image_load_bmp below
// keeps the kmalloc + img_t plumbing in C and calls the seam twice: once with
// out_px==NULL to validate + learn the dimensions, then once to decode into the
// exactly-sized kmalloc buffer. Behavior (accept/reject + pixels) is unchanged
// from the original; only the header is now parsed by the seam.
//
// The Rust bmp_decode_rs is proven byte-identical on THIS build by
// bmp_rust_selftest() (a boot-time [RUST-DIFF] over real + synthetic BMPs) and,
// offline, pixel-identical on the live boot.bmp + a 2,000,000-vector differential.
// Drop -DRUST_BMP to roll straight back to bmp_decode_c.
// ============================================================================

// out_px==NULL => validate + report *out_w/*out_h only (no decode). Else decode
// width*height BGR(A)->0x00RRGGBB pixels into out_px (rejects IMAGE_ERR_NOMEM if
// width*height exceeds out_cap_px). Returns IMAGE_SUCCESS or a negative IMAGE_ERR_*.
int bmp_decode_c(const uint8_t *buf, uint32_t len,
                 uint32_t *out_px, uint32_t out_cap_px,
                 uint32_t *out_w, uint32_t *out_h) {
    if (!buf || !out_w || !out_h) return IMAGE_ERR_NULL_PTR;
    *out_w = 0;
    *out_h = 0;

    if (len < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t))
        return IMAGE_ERR_TOO_SMALL;

    const bmp_file_header_t *file_hdr = (const bmp_file_header_t *)buf;
    const bmp_info_header_t *info_hdr = (const bmp_info_header_t *)(buf + sizeof(bmp_file_header_t));

    if (file_hdr->signature != BMP_SIGNATURE) return IMAGE_ERR_INVALID_SIG;
    if (info_hdr->header_size < 40) return IMAGE_ERR_UNSUPPORTED;

    int32_t width = info_hdr->width;
    int32_t height = info_hdr->height;
    bool top_down = false;
    if (height < 0) { height = -height; top_down = true; }
    if (width <= 0 || height <= 0) return IMAGE_ERR_CORRUPT;

    if (info_hdr->bpp != 24 && info_hdr->bpp != 32) return IMAGE_ERR_UNSUPPORTED;
    if (info_hdr->compression != BMP_COMPRESSION_RGB &&
        info_hdr->compression != BMP_COMPRESSION_BITFIELDS) return IMAGE_ERR_UNSUPPORTED;
    if (info_hdr->compression == BMP_COMPRESSION_BITFIELDS && info_hdr->bpp != 32)
        return IMAGE_ERR_UNSUPPORTED;

    if (file_hdr->data_offset >= len) return IMAGE_ERR_CORRUPT;

    uint32_t bytes_per_pixel = info_hdr->bpp / 8;
    uint32_t row_size_unpadded = (uint32_t)width * bytes_per_pixel;
    uint32_t row_padding = (4 - (row_size_unpadded % 4)) % 4;
    uint32_t row_size = row_size_unpadded + row_padding;
    uint32_t pixel_data_size = row_size * (uint32_t)height;
    if (file_hdr->data_offset + pixel_data_size > len) return IMAGE_ERR_CORRUPT;

    *out_w = (uint32_t)width;
    *out_h = (uint32_t)height;
    if (!out_px) return IMAGE_SUCCESS; // validate + dimensions only

    // Output-capacity gate (mirrors image_load_bmp's 64-bit alloc sizing).
    if ((uint64_t)(uint32_t)width * (uint64_t)(uint32_t)height > (uint64_t)out_cap_px)
        return IMAGE_ERR_NOMEM;

    const uint8_t *src_data = buf + file_hdr->data_offset;
    for (int32_t y = 0; y < height; y++) {
        int32_t src_y = top_down ? y : (height - 1 - y);
        const uint8_t *src_row = src_data + (uint32_t)src_y * row_size;
        uint32_t *dst_row = out_px + (uint32_t)y * (uint32_t)width;
        for (int32_t x = 0; x < width; x++) {
            const uint8_t *src_pixel = src_row + (uint32_t)x * bytes_per_pixel;
            uint8_t b = src_pixel[0];
            uint8_t g = src_pixel[1];
            uint8_t r = src_pixel[2];
            dst_row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
    return IMAGE_SUCCESS;
}

// Strangler dispatch: the live decode seam. -DRUST_BMP routes to the Rust port.
int bmp_decode(const uint8_t *buf, uint32_t len,
               uint32_t *out_px, uint32_t out_cap_px,
               uint32_t *out_w, uint32_t *out_h) {
#ifdef RUST_BMP
    return bmp_decode_rs(buf, len, out_px, out_cap_px, out_w, out_h);
#else
    return bmp_decode_c(buf, len, out_px, out_cap_px, out_w, out_h);
#endif
}

/**
 * Load a BMP image from memory
 *
 * BMP Format Overview:
 * - File Header (14 bytes): signature, file size, data offset
 * - Info Header (40+ bytes): dimensions, bit depth, compression
 * - Optional Color Table: for indexed color modes (not supported here)
 * - Pixel Data: stored bottom-up by default, row-aligned to 4 bytes
 *
 * Two-pass over the pure bmp_decode() seam: pass 1 validates the headers and
 * reports the dimensions (no alloc), pass 2 decodes into the exact kmalloc buffer.
 */
int image_load_bmp(void *data, uint32_t size, image_t *img) {
    if (!data || !img) {
        IMG_DBG("Error: null pointer (data=%p, img=%p)\n", data, img);
        return IMAGE_ERR_NULL_PTR;
    }

    img->width = 0;
    img->height = 0;
    img->pixels = NULL;

    // Pass 1: validate + get dimensions (routes to Rust under -DRUST_BMP).
    uint32_t w = 0, h = 0;
    int rc = bmp_decode((const uint8_t *)data, size, NULL, 0, &w, &h);
    if (rc != IMAGE_SUCCESS) {
        IMG_DBG("Error: BMP validate failed: %s\n", image_error_string(rc));
        return rc;
    }

    // Allocate the output pixel buffer (32-bit 0x00RRGGBB, framebuffer-compatible).
    // 64-bit sizing: w,h are each in (0, 2^31) so this never overflows size_t; a
    // too-large image simply fails kmalloc -> IMAGE_ERR_NOMEM (unchanged behavior).
    size_t pixel_buffer_size = (size_t)w * (size_t)h * sizeof(uint32_t);
    uint32_t *pixels = (uint32_t *)kmalloc(pixel_buffer_size);
    if (!pixels) {
        IMG_DBG("Error: failed to allocate %lu bytes for pixel buffer\n",
                (unsigned long)pixel_buffer_size);
        return IMAGE_ERR_NOMEM;
    }

    // Pass 2: decode into the exactly-sized buffer (out_cap = w*h pixels; kmalloc
    // succeeded so w*h < 2^26 and fits uint32 with no overflow).
    rc = bmp_decode((const uint8_t *)data, size, pixels, w * h, &w, &h);
    if (rc != IMAGE_SUCCESS) {
        IMG_DBG("Error: BMP decode failed: %s\n", image_error_string(rc));
        kfree(pixels);
        return rc;
    }

    img->width = w;
    img->height = h;
    img->pixels = pixels;

    IMG_DBG("Successfully loaded %ux%u image\n", img->width, img->height);
    return IMAGE_SUCCESS;
}

/**
 * Free an image's pixel buffer
 */
void image_free(image_t *img) {
    if (!img) {
        return;
    }

    if (img->pixels) {
        kfree(img->pixels);
        img->pixels = NULL;
    }

    img->width = 0;
    img->height = 0;
}

/**
 * Draw an image to the framebuffer
 */
void image_blit(image_t *img, int x, int y) {
    if (!img || !img->pixels || img->width == 0 || img->height == 0) {
        return;
    }

    // Use framebuffer's blit function for full image
    // Handle negative coordinates by adjusting source offset
    if (x < 0 || y < 0) {
        // Need to clip source
        uint32_t sx = (x < 0) ? (uint32_t)(-x) : 0;
        uint32_t sy = (y < 0) ? (uint32_t)(-y) : 0;
        int dx = (x < 0) ? 0 : x;
        int dy = (y < 0) ? 0 : y;

        if (sx >= img->width || sy >= img->height) {
            return; // Completely off-screen
        }

        uint32_t sw = img->width - sx;
        uint32_t sh = img->height - sy;

        image_blit_region(img, dx, dy, sx, sy, sw, sh);
    } else {
        // Simple case: no negative coordinates
        fb_blit((uint32_t)x, (uint32_t)y, img->width, img->height, img->pixels);
    }
}

/**
 * Draw a portion of an image to the framebuffer
 */
void image_blit_region(image_t *img, int dx, int dy,
                       uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh) {
    if (!img || !img->pixels) {
        return;
    }

    // Validate source region
    if (sx >= img->width || sy >= img->height) {
        return;
    }

    // Clip source region to image bounds
    if (sx + sw > img->width) {
        sw = img->width - sx;
    }
    if (sy + sh > img->height) {
        sh = img->height - sy;
    }

    if (sw == 0 || sh == 0) {
        return;
    }

    // Handle negative destination coordinates
    if (dx < 0) {
        uint32_t skip = (uint32_t)(-dx);
        if (skip >= sw) return;
        sx += skip;
        sw -= skip;
        dx = 0;
    }
    if (dy < 0) {
        uint32_t skip = (uint32_t)(-dy);
        if (skip >= sh) return;
        sy += skip;
        sh -= skip;
        dy = 0;
    }

    // Get framebuffer dimensions for clipping
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();

    // Clip to framebuffer bounds
    if ((uint32_t)dx >= fb_w || (uint32_t)dy >= fb_h) {
        return;
    }
    if ((uint32_t)dx + sw > fb_w) {
        sw = fb_w - (uint32_t)dx;
    }
    if ((uint32_t)dy + sh > fb_h) {
        sh = fb_h - (uint32_t)dy;
    }

    // Blit row by row from the source region
    for (uint32_t y = 0; y < sh; y++) {
        uint32_t *src_row = img->pixels + (sy + y) * img->width + sx;
        for (uint32_t x = 0; x < sw; x++) {
            fb_put_pixel((uint32_t)dx + x, (uint32_t)dy + y, src_row[x]);
        }
    }
}

/**
 * Draw an image scaled to fit destination dimensions
 * Uses nearest-neighbor scaling for performance
 */
void image_blit_scaled(image_t *img, int dx, int dy, uint32_t dw, uint32_t dh) {
    if (!img || !img->pixels || img->width == 0 || img->height == 0) {
        return;
    }

    if (dw == 0 || dh == 0) {
        return;
    }

    // Get framebuffer dimensions for clipping
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();

    // Skip if completely off-screen
    if (dx >= (int)fb_w || dy >= (int)fb_h) {
        return;
    }

    // Calculate scaling factors (fixed-point, 16.16)
    uint32_t x_ratio = ((img->width << 16) / dw);
    uint32_t y_ratio = ((img->height << 16) / dh);

    // Starting source coordinates (for clipping)
    uint32_t src_x_start = 0;
    uint32_t src_y_start = 0;
    uint32_t dest_x_start = (dx < 0) ? 0 : (uint32_t)dx;
    uint32_t dest_y_start = (dy < 0) ? 0 : (uint32_t)dy;

    // Adjust for negative destination coords
    if (dx < 0) {
        src_x_start = (uint32_t)((-dx) * x_ratio) >> 16;
    }
    if (dy < 0) {
        src_y_start = (uint32_t)((-dy) * y_ratio) >> 16;
    }

    // Calculate visible destination dimensions
    uint32_t visible_w = dw;
    uint32_t visible_h = dh;

    if (dx < 0) {
        visible_w = (uint32_t)((int)dw + dx);
    }
    if (dy < 0) {
        visible_h = (uint32_t)((int)dh + dy);
    }

    // Clip to framebuffer
    if (dest_x_start + visible_w > fb_w) {
        visible_w = fb_w - dest_x_start;
    }
    if (dest_y_start + visible_h > fb_h) {
        visible_h = fb_h - dest_y_start;
    }

    // Render scaled image (nearest-neighbor) ONE ROW AT A TIME via fb_put_row
    // (a single memcpy per row) instead of a per-pixel fb_put_pixel call. When
    // there is no horizontal scaling, blit the source row slice directly with no
    // per-pixel work at all - the common case for a screen-sized wallpaper.
    if (visible_w == 0 || visible_h == 0) return;
    uint32_t no_xscale = (x_ratio == (1u << 16));
    uint32_t *rowbuf = (uint32_t *)kmalloc((size_t)visible_w * 4);

    for (uint32_t y = 0; y < visible_h; y++) {
        uint32_t src_y = ((y * y_ratio + (src_y_start << 16)) >> 16);
        if (src_y >= img->height) src_y = img->height - 1;

        uint32_t *src_row = img->pixels + src_y * img->width;

        if (no_xscale && src_x_start + visible_w <= img->width) {
            fb_put_row(dest_x_start, dest_y_start + y, visible_w,
                       src_row + src_x_start);
        } else if (rowbuf) {
            for (uint32_t x = 0; x < visible_w; x++) {
                uint32_t src_x = ((x * x_ratio + (src_x_start << 16)) >> 16);
                if (src_x >= img->width) src_x = img->width - 1;
                rowbuf[x] = src_row[src_x];
            }
            fb_put_row(dest_x_start, dest_y_start + y, visible_w, rowbuf);
        } else {
            // Allocation failed: fall back to per-pixel writes.
            for (uint32_t x = 0; x < visible_w; x++) {
                uint32_t src_x = ((x * x_ratio + (src_x_start << 16)) >> 16);
                if (src_x >= img->width) src_x = img->width - 1;
                fb_put_pixel(dest_x_start + x, dest_y_start + y, src_row[src_x]);
            }
        }
    }
    if (rowbuf) kfree(rowbuf);
}

/**
 * Detect image format from data
 * Returns: 'B'=BMP, 'P'=PNG, 'J'=JPEG, 0=unknown
 */
int image_detect_format(const void *data, uint32_t size) {
    if (!data || size < 4) return 0;

    const uint8_t *p = (const uint8_t *)data;

    // Check BMP signature ("BM")
    if (size >= 2 && p[0] == 'B' && p[1] == 'M') {
        return 'B';
    }

    // Check PNG signature
    if (size >= 8 && p[0] == 137 && p[1] == 80 && p[2] == 78 && p[3] == 71) {
        return 'P';
    }

    // Check JPEG signature (FFD8)
    if (size >= 2 && p[0] == 0xFF && p[1] == 0xD8) {
        return 'J';
    }

    return 0;
}

/**
 * Load an image from memory, auto-detecting format
 */
int image_load(const void *data, uint32_t size, image_t *img) {
    if (!data || !img) return IMAGE_ERR_NULL_PTR;
    if (size < 4) return IMAGE_ERR_TOO_SMALL;

    int format = image_detect_format(data, size);

    switch (format) {
        case 'B':
            return image_load_bmp((void *)data, size, img);
        case 'P':
            return image_load_png(data, size, img);
        case 'J':
            return image_load_jpeg(data, size, img);
        default:
            IMG_DBG("Unknown image format\n");
            return IMAGE_ERR_UNSUPPORTED;
    }
}

// ---------------------------------------------------------------------------
// #404 Phase U boot-time self-test: prove bmp_decode_rs (Rust, live under
// -DRUST_BMP) == bmp_decode_c (verbatim reference) on well-formed 24/32bpp BMPs
// (bottom-up + top-down), report the SECURITY posture HONESTLY, and micro-bench
// both. LIGHT (#426, bounded, runs once): ~256 differential vectors + a crafted
// impossible-header sweep + a ~2k-iter RDTSC bench. The heavy fuzz (2,000,000
// vectors + ASan/UBSan + pixel-identical on the real boot.bmp) runs OFFLINE.
// Logs one [RUST-DIFF] bmp, one [RUST-SEC] bmp, one [RUST-PERF] bmp.
// ---------------------------------------------------------------------------
static uint32_t bmpdiff_rng(uint32_t *s) { uint32_t x=*s; x^=x<<13; x^=x>>17; x^=x<<5; *s=x; return x; }

static inline uint64_t bmp_tsc_serialized(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax","ebx","ecx","edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void bmp_wr16(uint8_t*b,int o,uint16_t v){ b[o]=v&0xFF; b[o+1]=(v>>8)&0xFF; }
static void bmp_wr32(uint8_t*b,int o,uint32_t v){ b[o]=v&0xFF; b[o+1]=(v>>8)&0xFF; b[o+2]=(v>>16)&0xFF; b[o+3]=(v>>24)&0xFF; }

// Build a well-formed BMP (bpp 24/32, bottom-up if h_signed>0, top-down if <0)
// into buf. Returns total length; pixel bytes filled from the rng.
static uint32_t bmp_build(uint8_t*buf, int32_t w, int32_t h_signed, uint16_t bpp, uint32_t comp, uint32_t*seed) {
    int32_t h = h_signed<0 ? -h_signed : h_signed;
    uint32_t bpp_b = bpp/8;
    uint32_t rowu = (uint32_t)w*bpp_b;
    uint32_t row = rowu + ((4-(rowu%4))%4);
    uint32_t total = 54 + row*(uint32_t)h;
    for (uint32_t i=0;i<54;i++) buf[i]=0;
    bmp_wr16(buf,0,BMP_SIGNATURE); bmp_wr32(buf,2,total); bmp_wr32(buf,10,54);
    bmp_wr32(buf,14,40); bmp_wr32(buf,18,(uint32_t)w); bmp_wr32(buf,22,(uint32_t)h_signed);
    bmp_wr16(buf,26,1); bmp_wr16(buf,28,bpp); bmp_wr32(buf,30,comp);
    for (uint32_t i=54;i<total;i++) buf[i]=(uint8_t)(bmpdiff_rng(seed)&0xFF);
    return total;
}

void bmp_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    // Max generated image 40x40 -> 1600 px. Static (no kernel-stack pressure).
    static uint8_t inbuf[8192];
    static uint32_t pc[1600], pr[1600];
    uint32_t seed = 0xB17A9C3D;
    uint32_t vectors=0, mism=0; int first_bad=-1;

    // Force-reference the Rust symbol so its archive member is always linked
    // (matches the icmp/arp/dns/dhcp pattern), regardless of -DRUST_BMP.
    { uint32_t w,h; bmp_decode_rs(inbuf, 0, NULL, 0, &w, &h); }

    // Part 1: agreement domain (~256 well-formed 24/32bpp, bottom-up + top-down).
    for (uint32_t it=0; it<256; it++) {
        uint32_t r = bmpdiff_rng(&seed);
        int32_t w = 1 + (int)(r % 40);
        int32_t h = 1 + (int)((r>>8) % 40);
        uint16_t bpp = (r & 0x10000) ? 32 : 24;
        uint32_t comp = (bpp==32 && (r & 0x20000)) ? BMP_COMPRESSION_BITFIELDS : BMP_COMPRESSION_RGB;
        int32_t hs = (r & 0x40000) ? -h : h;
        uint32_t len = bmp_build(inbuf, w, hs, bpp, comp, &seed);
        uint32_t wc,hc,wr,hr;
        int rc = bmp_decode_c(inbuf, len, pc, (uint32_t)(w*h), &wc, &hc);
        int rr = bmp_decode_rs(inbuf, len, pr, (uint32_t)(w*h), &wr, &hr);
        vectors++;
        int bad = (rc!=rr) || (rc==IMAGE_SUCCESS && (wc!=wr || hc!=hr));
        if (!bad && rc==IMAGE_SUCCESS) {
            for (int32_t i=0;i<w*h;i++) if (pc[i]!=pr[i]) { bad=1; break; }
        }
        if (bad) { mism++; if (first_bad<0) first_bad=(int)it; }
    }
    const char *verdict = (mism==0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] bmp: %u vectors, %u mismatches -> %s\n", vectors, mism, verdict);
    bootlog_write("[RUST-DIFF] bmp: %u vectors, %u mismatches -> %s", vectors, mism, verdict);
    if (mism) { kprintf("[RUST-DIFF] bmp FIRST MISMATCH it=%d\n", first_bad); bootlog_write("[RUST-DIFF] bmp FIRST MISMATCH it=%d", first_bad); }

    // Part 2: [RUST-SEC] - the C's uint32 row_size*height wraps and ACCEPTS an
    // impossible (multi-GB) header at the dims-only parse seam; the Rust's u64
    // checked arithmetic rejects it at parse. HONEST: LATENT/defense-in-depth -
    // live, the C path still fails SAFELY at the 64-bit kmalloc (no OOB), because
    // image_load_bmp's output alloc is 64-bit-accurate; this removes the
    // integer-overflow CLASS, not a live OOB. (The classic undersized-alloc OOB
    // WRITE is likewise already unreachable in this C for the same reason.)
    {
        struct { uint32_t w, h; uint16_t bpp; } fam[6] = {
            {16384,65536,32},{8192,131072,32},{32768,32768,32},
            {65536,16384,32},{4096,262144,32},{21846,65538,24}
        };
        uint32_t c_accept=0, rs_reject=0, n=0;
        for (int fi=0; fi<6; fi++) {
            for (int k=0;k<50;k++) {
                for (uint32_t i=0;i<64;i++) inbuf[i]=0;
                bmp_wr16(inbuf,0,BMP_SIGNATURE); bmp_wr32(inbuf,10,54); bmp_wr32(inbuf,14,40);
                bmp_wr32(inbuf,18,fam[fi].w); bmp_wr32(inbuf,22,fam[fi].h);
                bmp_wr16(inbuf,26,1); bmp_wr16(inbuf,28,fam[fi].bpp); bmp_wr32(inbuf,30,0);
                uint32_t wc,hc,wr,hr;
                int rc = bmp_decode_c(inbuf, 64, NULL, 0, &wc, &hc);   // dims-only
                int rr = bmp_decode_rs(inbuf, 64, NULL, 0, &wr, &hr);
                n++;
                if (rc==IMAGE_SUCCESS) c_accept++;
                if (rr!=IMAGE_SUCCESS) rs_reject++;
            }
        }
        kprintf("[RUST-SEC] bmp: C uint32 size-arith wraps -> accepts impossible header at parse %u/%u; Rust (u64 checked) confines %u/%u (LATENT integer-overflow class; live C fails safe at 64-bit kmalloc)\n", c_accept,n,rs_reject,n);
        bootlog_write("[RUST-SEC] bmp: C accepts impossible header (uint32 size wrap) %u/%u; Rust confines %u/%u (LATENT overflow class, defense-in-depth)", c_accept,n,rs_reject,n);
    }

    // Part 3: RDTSC micro-benchmark over a fixed 32x32 24bpp decode. LIGHT: 2k.
    {
        const int iters = 2000;
        uint32_t s3 = 0x2f9a71c4;
        uint32_t len = bmp_build(inbuf, 32, 32, 24, BMP_COMPRESSION_RGB, &s3);
        uint32_t w,h;
        for (int i=0;i<200;i++){ bmp_decode_c(inbuf,len,pc,1024,&w,&h); bmp_decode_rs(inbuf,len,pr,1024,&w,&h); }
        uint64_t t0=bmp_tsc_serialized();
        for (int i=0;i<iters;i++) bmp_decode_c(inbuf,len,pc,1024,&w,&h);
        uint64_t t1=bmp_tsc_serialized();
        for (int i=0;i<iters;i++) bmp_decode_rs(inbuf,len,pr,1024,&w,&h);
        uint64_t t2=bmp_tsc_serialized();
        uint64_t c_cyc=(t1-t0)/iters, r_cyc=(t2-t1)/iters;
        uint64_t ratio100 = c_cyc ? (r_cyc*100ULL/c_cyc) : 0;
        kprintf("[RUST-PERF] bmp: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
        bootlog_write("[RUST-PERF] bmp: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
    }
}
