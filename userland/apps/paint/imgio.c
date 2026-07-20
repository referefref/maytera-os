// imgio.c - Maytera Studio file I/O: BMP load/save (24-bit BGR, or 32-bit
// BI_BITFIELDS BGRA when the doc has transparency), native layered .MSTU,
// PNG export (real fixed-Huffman deflate + LZ77, RGB or RGBA). See studio.h
// for the module contract.
//
// All buffers are malloc'd (no big static arrays, blame #444; the only static
// table is the 1KB CRC32 LUT). File access uses the raw sys_open/sys_read/
// sys_write primitives (the same proven path aiclient.c and the shell tools
// use) so we do not depend on stdio buffering for multi-MB pixel streams.
#include "studio.h"
#include "../../libc/syscall.h"
#include "../../libc/fcntl.h"
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

// Case-insensitive extension match ("bmp", "mstu", "png").
static int ext_is(const char *path, const char *ext) {
    const char *dot = 0;
    for (const char *p = path; *p; p++) if (*p == '.') dot = p;
    if (!dot) return 0;
    dot++;
    while (*dot && *ext) {
        if (lower(*dot) != lower(*ext)) return 0;
        dot++; ext++;
    }
    return *dot == 0 && *ext == 0;
}

// Write fully (sys_write may be partial); returns 0 ok, -1 error.
static int wr_all(int fd, const void *buf, unsigned long len) {
    const unsigned char *p = (const unsigned char *)buf;
    while (len) {
        unsigned long chunk = len > 65536 ? 65536 : len;
        long n = sys_write(fd, p, chunk);
        if (n <= 0) return -1;
        p += n;
        len -= (unsigned long)n;
    }
    return 0;
}

// Read a whole file into a malloc'd buffer. Returns buf (caller frees) and
// sets *out_len; NULL on error.
static unsigned char *read_whole(const char *path, long *out_len) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    long cap = 65536, len = 0;
    unsigned char *buf = (unsigned char *)malloc((size_t)cap);
    if (!buf) { sys_close(fd); return 0; }
    for (;;) {
        if (len == cap) {
            long ncap = cap * 2;
            unsigned char *nb = (unsigned char *)realloc(buf, (size_t)ncap);
            if (!nb) { free(buf); sys_close(fd); return 0; }
            buf = nb; cap = ncap;
        }
        long n = sys_read(fd, buf + len, (unsigned long)(cap - len));
        if (n < 0) { free(buf); sys_close(fd); return 0; }
        if (n == 0) break;
        len += n;
    }
    sys_close(fd);
    *out_len = len;
    return buf;
}

static unsigned rd16le(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32le(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static void wr32le(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void wr16le(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}
static void wr32be(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

// Decide whether a save should carry an alpha channel, and pick the pixel
// source to sample. Rule (shared by PNG and BMP): a SINGLE-layer document with
// any partially-transparent pixel is written WITH alpha, sampling that layer's
// raw pixels (which hold true coverage); every other case is written opaque
// from the flattened composite. Multi-layer docs always use the opaque
// composite here because there is no premultiplied-alpha flatten helper exposed
// across modules (doc_composite() produces an opaque view only). Returns 1 if
// an alpha channel should be written, 0 otherwise, and sets *src to the buffer
// to read. Calls doc_composite() so g_doc.comp is valid for the opaque path.
static int alpha_source(const uint32_t **src) {
    doc_composite();
    if (g_doc.nlayers == 1 && g_doc.layer[0].px) {
        const uint32_t *lp = g_doc.layer[0].px;
        long npx = (long)g_doc.w * g_doc.h;
        for (long i = 0; i < npx; i++) {
            if (px_a(lp[i]) < 255) { *src = lp; return 1; }
        }
    }
    *src = g_doc.comp;   // may be NULL if compositing failed; every caller checks
    return 0;
}

// Fill one PNG scanline: leading filter-type byte (0 = None) followed by RGB or
// RGBA samples in network byte order. Shared by both IDAT paths (deflate and
// the low-memory stored fallback).
static void png_fill_row(unsigned char *row, const uint32_t *src, int w, int bpp) {
    row[0] = 0;                                  // filter type 0 (None)
    if (bpp == 4) {
        for (int x = 0; x < w; x++) {
            uint32_t p = src[x];
            row[1 + x * 4]     = (unsigned char)px_r(p);
            row[1 + x * 4 + 1] = (unsigned char)px_g(p);
            row[1 + x * 4 + 2] = (unsigned char)px_b(p);
            row[1 + x * 4 + 3] = (unsigned char)px_a(p);
        }
    } else {
        for (int x = 0; x < w; x++) {
            uint32_t p = src[x];
            row[1 + x * 3]     = (unsigned char)px_r(p);
            row[1 + x * 3 + 1] = (unsigned char)px_g(p);
            row[1 + x * 3 + 2] = (unsigned char)px_b(p);
        }
    }
}

// ---------------------------------------------------------------------------
// BMP
// ---------------------------------------------------------------------------
static int bmp_load(const char *path) {
    long len = 0;
    unsigned char *f = read_whole(path, &len);
    if (!f) return -1;
    if (len < 54 || f[0] != 'B' || f[1] != 'M') { free(f); return -1; }

    unsigned dataoff = rd32le(f + 10);
    int w    = (int)rd32le(f + 18);
    int hraw = (int)rd32le(f + 22);
    int bpp  = (int)rd16le(f + 28);
    unsigned comp = rd32le(f + 30);
    int topdown = 0, h = hraw;
    if (hraw < 0) { topdown = 1; h = -hraw; }

    // Accept uncompressed 24/32-bit (compression 0, or 3 = BI_BITFIELDS with
    // the standard BGRA masks, common for 32-bit screenshots).
    if (w <= 0 || h <= 0 || w > STUDIO_MAX_W || h > STUDIO_MAX_H ||
        (bpp != 24 && bpp != 32) ||
        (comp != 0 && !(comp == 3 && bpp == 32))) {
        free(f); return -1;
    }
    long stride = (bpp == 24) ? ((long)w * 3 + 3) & ~3L : (long)w * 4;
    if ((long)dataoff + stride * h > len) { free(f); return -1; }

    if (doc_new(w, h, 0xFFFFFFFFu) < 0) { free(f); return -1; }
    // CONTRACT-DEVIATION: imgio writes layer pixel buffers directly (the
    // contract has no bulk pixel setter); doc.c owns the allocation.
    uint32_t *px = g_doc.layer[0].px;
    for (int y = 0; y < h; y++) {
        int srow = topdown ? y : (h - 1 - y);
        const unsigned char *r = f + dataoff + (long)srow * stride;
        uint32_t *dst = px + (long)y * w;
        if (bpp == 24) {
            for (int x = 0; x < w; x++)
                dst[x] = argb(255, r[x * 3 + 2], r[x * 3 + 1], r[x * 3]);
        } else {
            for (int x = 0; x < w; x++)
                dst[x] = argb(255, r[x * 4 + 2], r[x * 4 + 1], r[x * 4]);
        }
    }
    free(f);
    g_doc.comp_dirty = 1;
    g_doc.modified = 0;
    strlcpy(g_doc.path, path, sizeof(g_doc.path));
    return 0;
}

// Scaled thumbnail decode (BMP/DIB only), nearest-neighbour, into a caller
// buffer. Never allocates or mutates g_doc; used by the file-browser preview.
int io_thumb(const char *path, uint32_t *out, int tw, int th) {
    if (!out || tw < 1 || th < 1) return -1;
    if (!(ext_is(path, "bmp") || ext_is(path, "dib"))) return -1;
    long len = 0;
    unsigned char *f = read_whole(path, &len);
    if (!f) return -1;
    if (len < 54 || f[0] != 'B' || f[1] != 'M') { free(f); return -1; }
    unsigned dataoff = rd32le(f + 10);
    int w = (int)rd32le(f + 18), hraw = (int)rd32le(f + 22), bpp = (int)rd16le(f + 28);
    unsigned comp = rd32le(f + 30);
    int topdown = 0, h = hraw;
    if (hraw < 0) { topdown = 1; h = -hraw; }
    if (w <= 0 || h <= 0 || w > STUDIO_MAX_W || h > STUDIO_MAX_H ||
        (bpp != 24 && bpp != 32) || (comp != 0 && !(comp == 3 && bpp == 32))) {
        free(f); return -1;
    }
    long stride = (bpp == 24) ? ((long)w * 3 + 3) & ~3L : (long)w * 4;
    if ((long)dataoff + stride * h > len) { free(f); return -1; }
    for (int ty = 0; ty < th; ty++) {
        int sy = ty * h / th; if (sy >= h) sy = h - 1;
        int srow = topdown ? sy : (h - 1 - sy);
        const unsigned char *r = f + dataoff + (long)srow * stride;
        uint32_t *d = out + (long)ty * tw;
        for (int tx = 0; tx < tw; tx++) {
            int sx = tx * w / tw; if (sx >= w) sx = w - 1;
            if (bpp == 24) d[tx] = argb(255, r[sx * 3 + 2], r[sx * 3 + 1], r[sx * 3]);
            else           d[tx] = argb(255, r[sx * 4 + 2], r[sx * 4 + 1], r[sx * 4]);
        }
    }
    free(f);
    return 0;
}

// Classic 24-bit BGR, bottom-up, 4-byte aligned rows (BITMAPINFOHEADER).
static int bmp_save_bgr24(int fd, const uint32_t *src, int w, int h) {
    long stride = ((long)w * 3 + 3) & ~3L;
    unsigned filesz = 54 + (unsigned)(stride * h);

    unsigned char hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    wr32le(hdr + 2, filesz);
    wr32le(hdr + 10, 54);             // bfOffBits
    wr32le(hdr + 14, 40);             // biSize
    wr32le(hdr + 18, (unsigned)w);
    wr32le(hdr + 22, (unsigned)h);
    wr16le(hdr + 26, 1);
    wr16le(hdr + 28, 24);
    wr32le(hdr + 34, (unsigned)(stride * h));
    wr32le(hdr + 38, 2835);           // ~72 DPI
    wr32le(hdr + 42, 2835);
    if (wr_all(fd, hdr, 54) != 0) return -1;

    unsigned char *row = (unsigned char *)malloc((size_t)stride);
    if (!row) return -1;
    memset(row, 0, (size_t)stride);
    int rc = 0;
    for (int y = h - 1; y >= 0 && rc == 0; y--) {       // bottom-up
        const uint32_t *s = src + (long)y * w;
        for (int x = 0; x < w; x++) {
            row[x * 3]     = (unsigned char)px_b(s[x]);
            row[x * 3 + 1] = (unsigned char)px_g(s[x]);
            row[x * 3 + 2] = (unsigned char)px_r(s[x]);
        }
        rc = wr_all(fd, row, (unsigned long)stride);
    }
    free(row);
    return rc;
}

// 32-bit BGRA with BI_BITFIELDS masks (BITMAPINFOHEADER + 3 masks = 66-byte
// header, pixel data at offset 66). Bytes are stored B,G,R,A so a little-endian
// 32-bit read yields 0xAARRGGBB; hence the masks R=0x00FF0000, G=0x0000FF00,
// B=0x000000FF. The alpha byte is preserved for readers that honour it. Our own
// bmp_load() accepts (comp==3 && bpp==32) and reloads the RGB (forcing opaque),
// so RGB round-trips exactly. Rows are w*4 bytes, already 4-byte aligned.
static int bmp_save_bgra32(int fd, const uint32_t *src, int w, int h) {
    long stride = (long)w * 4;
    unsigned dataoff = 54 + 12;                 // 14 file hdr + 40 info hdr + 12 masks
    unsigned filesz = dataoff + (unsigned)(stride * h);

    unsigned char hdr[66];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    wr32le(hdr + 2, filesz);
    wr32le(hdr + 10, dataoff);                  // bfOffBits
    wr32le(hdr + 14, 40);                       // biSize (BITMAPINFOHEADER)
    wr32le(hdr + 18, (unsigned)w);
    wr32le(hdr + 22, (unsigned)h);
    wr16le(hdr + 26, 1);                        // planes
    wr16le(hdr + 28, 32);                       // bpp
    wr32le(hdr + 30, 3);                        // biCompression = BI_BITFIELDS
    wr32le(hdr + 34, (unsigned)(stride * h));   // biSizeImage
    wr32le(hdr + 38, 2835);
    wr32le(hdr + 42, 2835);
    wr32le(hdr + 54, 0x00FF0000u);              // red   mask
    wr32le(hdr + 58, 0x0000FF00u);              // green mask
    wr32le(hdr + 62, 0x000000FFu);              // blue  mask
    if (wr_all(fd, hdr, 66) != 0) return -1;

    unsigned char *row = (unsigned char *)malloc((size_t)stride);
    if (!row) return -1;
    int rc = 0;
    for (int y = h - 1; y >= 0 && rc == 0; y--) {       // bottom-up
        const uint32_t *s = src + (long)y * w;
        for (int x = 0; x < w; x++) {
            uint32_t p = s[x];
            row[x * 4]     = (unsigned char)px_b(p);
            row[x * 4 + 1] = (unsigned char)px_g(p);
            row[x * 4 + 2] = (unsigned char)px_r(p);
            row[x * 4 + 3] = (unsigned char)px_a(p);
        }
        rc = wr_all(fd, row, (unsigned long)stride);
    }
    free(row);
    return rc;
}

static int bmp_save(const char *path) {
    const uint32_t *src = 0;
    int rgba = alpha_source(&src);            // 1 => 32-bit BGRA, else 24-bit BGR
    if (!src) return -1;
    int w = g_doc.w, h = g_doc.h;

    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    int rc = rgba ? bmp_save_bgra32(fd, src, w, h)
                  : bmp_save_bgr24(fd, src, w, h);
    sys_close(fd);
    return rc;
}

// ---------------------------------------------------------------------------
// MSTU native layered format (all integers little-endian i32):
//   "MSTU" magic, version=1, w, h, nlayers, active
//   per layer (bottom to top): name[24], opacity, visible, blend, w*h*4 ARGB
//   sel_active; if nonzero: w*h selection mask bytes
// ---------------------------------------------------------------------------
static int mstu_save(const char *path) {
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    unsigned char h32[24];
    memcpy(h32, "MSTU", 4);
    wr32le(h32 + 4, 1);
    wr32le(h32 + 8, (unsigned)g_doc.w);
    wr32le(h32 + 12, (unsigned)g_doc.h);
    wr32le(h32 + 16, (unsigned)g_doc.nlayers);
    wr32le(h32 + 20, (unsigned)g_doc.active);
    if (wr_all(fd, h32, 24) != 0) { sys_close(fd); return -1; }

    long npx = (long)g_doc.w * g_doc.h;
    for (int i = 0; i < g_doc.nlayers; i++) {
        const layer_t *L = &g_doc.layer[i];
        unsigned char lh[STUDIO_NAME_LEN + 12];
        memset(lh, 0, sizeof(lh));
        memcpy(lh, L->name, STUDIO_NAME_LEN);
        lh[STUDIO_NAME_LEN - 1] = 0;
        wr32le(lh + STUDIO_NAME_LEN, (unsigned)L->opacity);
        wr32le(lh + STUDIO_NAME_LEN + 4, (unsigned)L->visible);
        wr32le(lh + STUDIO_NAME_LEN + 8, (unsigned)L->blend);
        if (wr_all(fd, lh, sizeof(lh)) != 0) { sys_close(fd); return -1; }
        if (wr_all(fd, L->px, (unsigned long)npx * 4) != 0) { sys_close(fd); return -1; }
    }
    unsigned char sa[4];
    wr32le(sa, (unsigned)(g_doc.sel_active && g_doc.sel ? 1 : 0));
    if (wr_all(fd, sa, 4) != 0) { sys_close(fd); return -1; }
    if (g_doc.sel_active && g_doc.sel) {
        if (wr_all(fd, g_doc.sel, (unsigned long)npx) != 0) { sys_close(fd); return -1; }
    }
    sys_close(fd);
    return 0;
}

static int mstu_load(const char *path) {
    long len = 0;
    unsigned char *f = read_whole(path, &len);
    if (!f) return -1;
    if (len < 28 || memcmp(f, "MSTU", 4) != 0 || rd32le(f + 4) != 1) { free(f); return -1; }
    int w  = (int)rd32le(f + 8);
    int h  = (int)rd32le(f + 12);
    int nl = (int)rd32le(f + 16);
    int ac = (int)rd32le(f + 20);
    if (w <= 0 || h <= 0 || w > STUDIO_MAX_W || h > STUDIO_MAX_H ||
        nl < 1 || nl > STUDIO_MAX_LAYERS) { free(f); return -1; }
    long npx = (long)w * h;
    long need = 24 + (long)nl * (STUDIO_NAME_LEN + 12 + npx * 4) + 4;
    if (len < need) { free(f); return -1; }

    if (doc_new(w, h, 0xFFFFFFFFu) < 0) { free(f); return -1; }
    // Grow to nl layers via the contract API (insert above active).
    for (int i = 1; i < nl; i++) {
        g_doc.active = i - 1;                    // CONTRACT-DEVIATION: direct
        if (layer_add("Layer", 0) < 0) { free(f); return -1; }
    }
    // CONTRACT-DEVIATION: imgio rebuilds layer metadata + pixels directly;
    // the contract has no bulk setters and doc.c owns the buffers.
    const unsigned char *p = f + 24;
    for (int i = 0; i < nl; i++) {
        layer_t *L = &g_doc.layer[i];
        memcpy(L->name, p, STUDIO_NAME_LEN);
        L->name[STUDIO_NAME_LEN - 1] = 0;
        L->opacity = clampi((int)rd32le(p + STUDIO_NAME_LEN), 0, 255);
        L->visible = rd32le(p + STUDIO_NAME_LEN + 4) ? 1 : 0;
        int b = (int)rd32le(p + STUDIO_NAME_LEN + 8);
        L->blend = (b >= 0 && b < BLEND_COUNT) ? (blend_t)b : BLEND_NORMAL;
        p += STUDIO_NAME_LEN + 12;
        memcpy(L->px, p, (size_t)npx * 4);
        p += npx * 4;
    }
    int sel_flag = (int)rd32le(p);
    p += 4;
    if (sel_flag && (p - f) + npx <= len) {
        uint8_t *m = (uint8_t *)malloc((size_t)npx);
        if (m) {
            memcpy(m, p, (size_t)npx);
            free(g_doc.sel);
            g_doc.sel = m;
            g_doc.sel_active = 1;
        }
    }
    g_doc.active = clampi(ac, 0, g_doc.nlayers - 1);
    g_doc.comp_dirty = 1;
    g_doc.modified = 0;
    strlcpy(g_doc.path, path, sizeof(g_doc.path));
    free(f);
    return 0;
}

// ---------------------------------------------------------------------------
// PNG export: 8-bit RGB (color type 2) or RGBA (color type 6, when the doc has
// transparency, per alpha_source()), filter 0 per scanline. The IDAT is a real
// zlib stream produced by our own fixed-Huffman deflate compressor with LZ77
// matching (see below). If the compressor cannot allocate its working buffers,
// png_save falls back to the original STORED (uncompressed) deflate path, which
// streams in bounded memory (~70KB scratch) at any doc size.
// ---------------------------------------------------------------------------
static uint32_t g_crc_tab[256];      // small LUT: the one allowed static table
static int g_crc_ready = 0;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        g_crc_tab[n] = c;
    }
    g_crc_ready = 1;
}
static uint32_t crc_update(uint32_t crc, const unsigned char *buf, long len) {
    for (long i = 0; i < len; i++)
        crc = g_crc_tab[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

#define ADLER_MOD 65521u

// Write one PNG chunk (length + type + data + CRC).
static int png_chunk(int fd, const char *type, const unsigned char *data, long len) {
    unsigned char n4[4];
    wr32be(n4, (unsigned)len);
    if (wr_all(fd, n4, 4) != 0) return -1;
    if (wr_all(fd, type, 4) != 0) return -1;
    if (len && wr_all(fd, data, (unsigned long)len) != 0) return -1;
    uint32_t crc = crc_update(0xFFFFFFFFu, (const unsigned char *)type, 4);
    if (len) crc = crc_update(crc, data, len);
    wr32be(n4, crc ^ 0xFFFFFFFFu);
    return wr_all(fd, n4, 4);
}

#define PNG_BLOCK 65535

// ===========================================================================
// Fixed-Huffman DEFLATE (RFC 1951) with LZ77, used for the PNG IDAT stream.
//
// Design:
//   * Single BTYPE=01 (fixed Huffman) block, BFINAL=1, wrapped in a zlib
//     stream (CMF=0x78, FLG=0x01, trailing big-endian adler32 over the raw
//     bytes). 0x78,0x01 satisfies (CMF*256+FLG) % 31 == 0.
//   * Bit packing is LSB-first per RFC1951 3.1.1: elements enter the byte from
//     the least-significant bit. Huffman codes, however, are transmitted
//     MOST-significant bit first, so each canonical code is bit-reversed
//     (rev_bits) before being handed to the LSB-first writer. Extra bits
//     (length/distance) are plain LSB-first integers and are NOT reversed.
//   * LZ77: 32KB sliding window (max distance 32768), hash-chain matcher keyed
//     on 3 bytes (15-bit hash), chain depth capped at 32. Greedy matching (no
//     lazy step; lazy is a permitted RFC option we skip for simplicity). All
//     positions inside an emitted match are inserted into the chains so later
//     matches can reference them. Min match 3, max match 258.
//
// RFC1951 length codes 257..285 (base length + extra bits), verified from the
// spec table:
//   code 257..264: len 3..10, 0 extra
//   265..268: 11..18, 1 extra    269..272: 19..34, 2 extra
//   273..276: 35..66, 3 extra    277..280: 67..130, 4 extra
//   281..284: 131..257, 5 extra  285: 258, 0 extra
// RFC1951 distance codes 0..29 (base distance + extra bits):
//   0..3: 1..4, 0 extra          4..5: 5..8, 1 extra
//   6..7: 9..16, 2 extra         8..9: 17..32, 3 extra
//   10..11: 33..64, 4 extra      12..13: 65..128, 5 extra
//   14..15: 129..256, 6 extra    16..17: 257..512, 7 extra
//   18..19: 513..1024, 8 extra   20..21: 1025..2048, 9 extra
//   22..23: 2049..4096, 10 extra 24..25: 4097..8192, 11 extra
//   26..27: 8193..16384, 12 extra 28..29: 16385..32768, 13 extra
// ===========================================================================
#define DEF_WINDOW   32768
#define DEF_WMASK    32767
#define DEF_HASHSIZE 32768
#define DEF_HASHMASK 32767
#define DEF_MINMATCH 3
#define DEF_MAXMATCH 258
#define DEF_MAXCHAIN 32

// LSB-first bit writer over a bounded buffer. On overflow it sets err and stops
// storing (so the caller can fall back to the stored path).
typedef struct {
    unsigned char *buf;
    long           cap;
    long           len;       // bytes emitted so far
    unsigned       bitbuf;    // pending bits (max ~20 in flight, fits 32)
    int            bitcnt;    // number of valid low bits in bitbuf
    int            err;
} bitw_t;

static void bw_bits(bitw_t *b, unsigned val, int nbits) {
    b->bitbuf |= (val << b->bitcnt);
    b->bitcnt += nbits;
    while (b->bitcnt >= 8) {
        if (b->len < b->cap) b->buf[b->len] = (unsigned char)(b->bitbuf & 0xFF);
        else b->err = 1;
        b->len++;
        b->bitbuf >>= 8;
        b->bitcnt -= 8;
    }
}
static void bw_flush(bitw_t *b) {              // pad the final partial byte with 0s
    if (b->bitcnt > 0) {
        if (b->len < b->cap) b->buf[b->len] = (unsigned char)(b->bitbuf & 0xFF);
        else b->err = 1;
        b->len++;
        b->bitbuf = 0; b->bitcnt = 0;
    }
}

// Reverse the low `len` bits of `code` (Huffman codes go out MSB-first).
static unsigned rev_bits(unsigned code, int len) {
    unsigned r = 0;
    for (int i = 0; i < len; i++) { r = (r << 1) | (code & 1u); code >>= 1; }
    return r;
}

// Emit one fixed-Huffman literal/length symbol (0..287). Canonical fixed codes
// from RFC1951 3.2.6, then bit-reversed for the LSB-first writer.
static void emit_litlen(bitw_t *b, int sym) {
    unsigned code; int len;
    if (sym <= 143)      { len = 8; code = 0x30u + (unsigned)sym; }
    else if (sym <= 255) { len = 9; code = 0x190u + (unsigned)(sym - 144); }
    else if (sym <= 279) { len = 7; code = (unsigned)(sym - 256); }
    else                 { len = 8; code = 0xC0u + (unsigned)(sym - 280); }
    bw_bits(b, rev_bits(code, len), len);
}

// Length/distance base + extra-bit tables (RFC1951 3.2.5), indexed by
// (length code - 257) and by distance code respectively.
static const int len_base[29]  = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,
                                  43,51,59,67,83,99,115,131,163,195,227,258};
static const int len_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,
                                  4,5,5,5,5,0};
static const int dist_base[30]  = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
                                   257,385,513,769,1025,1537,2049,3073,4097,6145,
                                   8193,12289,16385,24577};
static const int dist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,
                                   10,10,11,11,12,12,13,13};

// Emit an LZ77 match: length symbol (+extra), then 5-bit distance code (+extra).
static void emit_match(bitw_t *b, int len, int dist) {
    int i = 28;
    while (i > 0 && len < len_base[i]) i--;
    emit_litlen(b, 257 + i);
    if (len_extra[i]) bw_bits(b, (unsigned)(len - len_base[i]), len_extra[i]);

    int j = 29;
    while (j > 0 && dist < dist_base[j]) j--;
    bw_bits(b, rev_bits((unsigned)j, 5), 5);        // fixed 5-bit distance code
    if (dist_extra[j]) bw_bits(b, (unsigned)(dist - dist_base[j]), dist_extra[j]);
}

static unsigned def_hash(const unsigned char *p) {
    return (((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5) ^ (unsigned)p[2])
           & DEF_HASHMASK;
}

// Compress the flattened scanline stream into one fixed-Huffman IDAT chunk.
// Returns 0 on success, 1 to ask the caller to use the stored fallback (an
// allocation failed or the bounded output overflowed BEFORE anything was
// written to fd), or -1 on an fd write error.
static int png_write_idat_deflate(int fd, const uint32_t *src, int w, int h, int bpp) {
    long rowlen = (long)w * bpp + 1;
    long rawlen = rowlen * h;
    if (rawlen <= 0) return 1;

    unsigned char *raw = (unsigned char *)malloc((size_t)rawlen);
    if (!raw) return 1;
    for (int y = 0; y < h; y++)
        png_fill_row(raw + (long)y * rowlen, src + (long)y * w, w, bpp);

    // adler32 over the raw (uncompressed) bytes. Per-byte reduction is overflow
    // safe: a1<65521, a2<65521, so neither +255 nor +a1 can wrap 32 bits.
    uint32_t a1 = 1, a2 = 0;
    for (long i = 0; i < rawlen; i++) {
        a1 += raw[i]; if (a1 >= ADLER_MOD) a1 -= ADLER_MOD;
        a2 += a1;     if (a2 >= ADLER_MOD) a2 -= ADLER_MOD;
    }

    int *head = (int *)malloc((size_t)DEF_HASHSIZE * sizeof(int));
    int *prev = (int *)malloc((size_t)DEF_WINDOW * sizeof(int));
    // Worst-case fixed-Huffman output is 9 bits/byte = rawlen*9/8; the margin
    // rawlen/8 + overhead covers it, and bw.err provides a hard safety net.
    long outcap = rawlen + rawlen / 8 + 128;
    unsigned char *out = (unsigned char *)malloc((size_t)outcap);
    if (!head || !prev || !out) { free(head); free(prev); free(out); free(raw); return 1; }
    for (long i = 0; i < DEF_HASHSIZE; i++) head[i] = -1;
    // prev[] needs no init: a chain only reaches positions still inside the
    // 32KB window, and every such position's prev slot was written when it was
    // inserted (slots for out-of-window positions are filtered by the window
    // test before they are dereferenced).

    bitw_t bw;
    bw.buf = out; bw.cap = outcap; bw.len = 2; bw.bitbuf = 0; bw.bitcnt = 0; bw.err = 0;
    out[0] = 0x78; out[1] = 0x01;                   // zlib header

    bw_bits(&bw, 1, 1);                             // BFINAL = 1
    bw_bits(&bw, 1, 2);                             // BTYPE  = 01 (fixed Huffman)

    long pos = 0;
    while (pos < rawlen) {
        int best_len = 0, best_dist = 0;
        if (pos + DEF_MINMATCH <= rawlen) {
            unsigned hh = def_hash(raw + pos);
            int cur = head[hh];
            int max_len = (int)((rawlen - pos) < DEF_MAXMATCH ? (rawlen - pos)
                                                              : DEF_MAXMATCH);
            long limit = pos - DEF_WINDOW;          // positions <= limit are out of window
            int chain = DEF_MAXCHAIN;
            while (cur >= 0 && (long)cur > limit && chain-- > 0) {
                // Quick reject: if the byte at best_len differs, this candidate
                // cannot beat the current best (best_len < max_len here).
                if (raw[cur + best_len] == raw[pos + best_len]) {
                    int l = 0;
                    while (l < max_len && raw[cur + l] == raw[pos + l]) l++;
                    if (l > best_len) {
                        best_len = l;
                        best_dist = (int)(pos - cur);
                        if (l >= max_len) break;
                    }
                }
                cur = prev[cur & DEF_WMASK];
            }
        }
        if (best_len >= DEF_MINMATCH) {
            emit_match(&bw, best_len, best_dist);
            long end = pos + best_len;
            for (; pos < end; pos++) {              // insert every covered position
                if (pos + DEF_MINMATCH <= rawlen) {
                    unsigned hh = def_hash(raw + pos);
                    prev[pos & DEF_WMASK] = head[hh];
                    head[hh] = (int)pos;
                }
            }
        } else {
            emit_litlen(&bw, raw[pos]);
            if (pos + DEF_MINMATCH <= rawlen) {
                unsigned hh = def_hash(raw + pos);
                prev[pos & DEF_WMASK] = head[hh];
                head[hh] = (int)pos;
            }
            pos++;
        }
    }
    emit_litlen(&bw, 256);                          // end-of-block symbol
    bw_flush(&bw);

    free(head); free(prev); free(raw);

    if (bw.err || bw.len + 4 > outcap) { free(out); return 1; }  // overflow -> fallback
    wr32be(out + bw.len, (a2 << 16) | a1);          // zlib trailer: adler32 (BE)
    long zlen = bw.len + 4;

    int rc = png_chunk(fd, "IDAT", out, zlen);
    free(out);
    return rc == 0 ? 0 : -1;
}

// Low-memory fallback: STORED (uncompressed) deflate blocks, streamed one IDAT
// chunk per 64KB block. This is the original bounded-memory path, generalised
// to 3 or 4 bytes per pixel. Returns 0 on success, -1 on error.
static int png_write_idat_stored(int fd, const uint32_t *src, int w, int h, int bpp) {
    long rowlen = (long)w * bpp + 1;
    unsigned char *blk = (unsigned char *)malloc(PNG_BLOCK + 16);
    unsigned char *row = (unsigned char *)malloc((size_t)rowlen);
    if (!blk || !row) { free(blk); free(row); return -1; }

    uint32_t a1 = 1, a2 = 0;
    long fill = 0;
    int first = 1;
    long total_raw = rowlen * h, done_raw = 0;
    int rc = 0;
    for (int y = 0; y < h && rc == 0; y++) {
        png_fill_row(row, src + (long)y * w, w, bpp);
        for (long i = 0; i < rowlen; i++) {         // adler over the raw stream
            a1 += row[i]; if (a1 >= ADLER_MOD) a1 -= ADLER_MOD;
            a2 += a1;     if (a2 >= ADLER_MOD) a2 -= ADLER_MOD;
        }
        long roff = 0;
        while (roff < rowlen && rc == 0) {
            long space = PNG_BLOCK - fill;
            long take = (rowlen - roff) < space ? (rowlen - roff) : space;
            long base = (first ? 2 : 0) + 5;        // reserve zlib hdr + block hdr
            memcpy(blk + base + fill, row + roff, (size_t)take);
            fill += take; roff += take; done_raw += take;
            if (fill == PNG_BLOCK || done_raw == total_raw) {
                int final = (done_raw == total_raw);
                long o = 0;
                if (first) { blk[0] = 0x78; blk[1] = 0x01; o = 2; first = 0; }
                blk[o]     = (unsigned char)(final ? 1 : 0);   // BFINAL, BTYPE=00
                wr16le(blk + o + 1, (unsigned)fill);
                wr16le(blk + o + 3, (unsigned)(~(unsigned)fill & 0xFFFF));
                long clen = o + 5 + fill;
                if (final) { wr32be(blk + clen, (a2 << 16) | a1); clen += 4; }
                rc = png_chunk(fd, "IDAT", blk, clen);
                fill = 0;
            }
        }
    }
    free(blk);
    free(row);
    return rc;
}

static int png_save(const char *path) {
    const uint32_t *src = 0;
    int rgba = alpha_source(&src);                 // calls doc_composite()
    if (!src) return -1;
    if (!g_crc_ready) crc_init();
    int w = g_doc.w, h = g_doc.h;
    int bpp = rgba ? 4 : 3;

    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;

    static const unsigned char sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    int rc = wr_all(fd, sig, 8);

    unsigned char ihdr[13];
    wr32be(ihdr, (unsigned)w);
    wr32be(ihdr + 4, (unsigned)h);
    ihdr[8] = 8;                                   // 8 bits per channel
    ihdr[9] = (unsigned char)(rgba ? 6 : 2);       // 6 = truecolor+alpha, 2 = truecolor
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    if (rc == 0) rc = png_chunk(fd, "IHDR", ihdr, 13);

    if (rc == 0) {
        int dr = png_write_idat_deflate(fd, src, w, h, bpp);
        if (dr == 1) dr = png_write_idat_stored(fd, src, w, h, bpp);  // low-mem fallback
        rc = dr;
    }
    if (rc == 0) rc = png_chunk(fd, "IEND", 0, 0);
    sys_close(fd);
    return rc == 0 ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Universal decode via the kernel image codec (SYS_DECODE_IMAGE): handles PNG,
// JPEG, GIF and BMP (everything the kernel image_load() understands). The
// result is scaled to fit within the canvas maximum, so an oversized photo
// loads at a sensible working size instead of failing outright. Builds a fresh
// single-layer document. Alpha is forced opaque (the decoder returns composited
// RGB), matching the BMP loader's behaviour.
// ---------------------------------------------------------------------------
static int decode_load(const char *path) {
    long len = 0;
    unsigned char *f = read_whole(path, &len);
    if (!f) return -1;
    unsigned long cap = (unsigned long)STUDIO_MAX_W * STUDIO_MAX_H * 4u;
    uint32_t *out = (uint32_t *)malloc((size_t)cap);
    if (!out) { free(f); return -1; }
    int dims[2] = {0, 0};
    int n = decode_image(f, (unsigned)len, STUDIO_MAX_W, STUDIO_MAX_H,
                         out, (unsigned)cap, dims);
    free(f);
    int w = dims[0], h = dims[1];
    if (n <= 0 || w < 1 || h < 1) { free(out); return -1; }
    if (doc_new(w, h, 0xFFFFFFFFu) < 0) { free(out); return -1; }
    uint32_t *px = g_doc.layer[0].px;
    long npx = (long)w * h;
    // Alpha is deliberately forced to 0xFF here. The SYS_DECODE_IMAGE contract
    // returns an already-composited OPAQUE RGB image (any source transparency is
    // flattened by the kernel decoder onto its background), so the top byte of
    // out[] is not a meaningful coverage value and must not be trusted as one;
    // forcing 0xFF gives a fully-opaque working layer, matching bmp_load(). If a
    // future kernel decoder starts returning genuine straight-alpha, this line
    // should copy out[i] verbatim instead.
    for (long i = 0; i < npx; i++) px[i] = 0xFF000000u | (out[i] & 0x00FFFFFFu);
    free(out);
    g_doc.comp_dirty = 1;
    g_doc.modified = 0;
    // TODO(imgio): the decoder scales oversized images down to fit STUDIO_MAX_W/H
    // but reports no scaled/original-size signal, so we cannot tell the user the
    // canvas is a reduced copy. When a size signal exists (or dims are compared
    // against the source header), surface a "(scaled)" note. UI text is owned by
    // ui.c, so no user-visible string is set from here.
    strlcpy(g_doc.path, path, sizeof(g_doc.path));
    return 0;
}

// Public API
// ---------------------------------------------------------------------------
int io_load(const char *path) {
    if (!path || !path[0]) return -1;
    if (ext_is(path, "mstu")) return mstu_load(path);
    if (ext_is(path, "bmp") || ext_is(path, "dib")) {
        if (bmp_load(path) == 0) return 0;         // fast native, exact pixels
        return decode_load(path);                  // compressed/oversized fallback
    }
    // PNG / JPEG / JPG / GIF (and anything else): the kernel codec sniffs the
    // magic bytes, so we route unknown extensions through it too.
    return decode_load(path);
}

int io_save(const char *path) {
    if (!path || !path[0]) return -1;
    int rc;
    if (ext_is(path, "bmp") || ext_is(path, "dib")) rc = bmp_save(path);
    else if (ext_is(path, "mstu"))                  rc = mstu_save(path);
    else if (ext_is(path, "png"))                   rc = png_save(path);
    else return -1;
    if (rc == 0) {
        strlcpy(g_doc.path, path, sizeof(g_doc.path));
        g_doc.modified = 0;
    }
    return rc;
}

// ---------------------------------------------------------------------------
// Printing (#318 IPP). We flatten the document to a temporary PNG and hand it
// to the kernel print pipeline (SYS_PRINT_IMAGE), which renders + submits it to
// a configured printer over IPP. Reuses the same codec + printer registry the
// rest of the OS uses; nothing is reinvented here.
// ---------------------------------------------------------------------------
#ifndef SYS_PRINT_LIST
#define SYS_PRINT_LIST  291
#endif
#ifndef SYS_PRINT_IMAGE
#define SYS_PRINT_IMAGE 296
#endif

// Mirror of the kernel printer_cfg_t (net/ipp.h) so we can read the registry.
typedef struct {
    char           name[32];
    char           host[64];
    char           queue[64];
    unsigned short port;
    int            is_default;
    int            valid;
} studio_printer_t;

// Copy the default (or first) configured printer's friendly name into `out`.
// Returns 1 if at least one printer is configured, 0 if none.
int io_printer_default(char *out, int cap) {
    if (cap > 0) out[0] = 0;
    studio_printer_t list[8];
    for (int i = 0; i < 8; i++) list[i].valid = 0;
    int n = (int)syscall2(SYS_PRINT_LIST, (long)list, 8);
    if (n <= 0) return 0;
    int pick = -1;
    for (int i = 0; i < n && i < 8; i++) {
        if (!list[i].valid) continue;
        if (list[i].is_default) { pick = i; break; }
        if (pick < 0) pick = i;
    }
    if (pick < 0) return 0;
    strlcpy(out, list[pick].name, cap);
    return 1;
}

// Flatten to a temporary PNG and print it. `printer` may be NULL for the system
// default. Returns 0 on success, -1 on failure.
int io_print(const char *printer) {
    const char *tmp = "/CONFIG/STUPRINT.PNG";      // /CONFIG is a writable FAT dir
    if (png_save(tmp) != 0) return -1;
    long rc = syscall2(SYS_PRINT_IMAGE, (long)printer, (long)tmp);
    return rc == 0 ? 0 : -1;
}
