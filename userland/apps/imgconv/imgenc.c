// imgenc.c - BMP24 and PNG writers shared by imgconv and sprite. See imgenc.h.
#include "imgenc.h"
#include "../../libc/syscall.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/fcntl.h"

int imgenc_write_all(int fd, const void *buf, long n) {
    const unsigned char *p = (const unsigned char *)buf;
    long done = 0;
    while (done < n) {
        long w = sys_write(fd, p + done, (unsigned long)(n - done));
        if (w <= 0) return -1;
        done += w;
    }
    return 0;
}

long imgenc_read_file(const char *path, unsigned char **out, long cap) {
    *out = 0;
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    long size = cap < (64 * 1024) ? cap : (64 * 1024);
    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf) { sys_close(fd); return -1; }
    long total = 0;
    for (;;) {
        if (total == size) {
            if (size >= cap) { free(buf); sys_close(fd); return -2; }
            long nsize = size * 2;
            if (nsize > cap) nsize = cap;
            unsigned char *nb = (unsigned char *)realloc(buf, (size_t)nsize);
            if (!nb) { free(buf); sys_close(fd); return -1; }
            buf = nb;
            size = nsize;
        }
        long n = sys_read(fd, buf + total, (unsigned long)(size - total));
        if (n < 0) { free(buf); sys_close(fd); return -1; }
        if (n == 0) break;
        total += n;
    }
    sys_close(fd);
    *out = buf;
    return total;
}

// ---------------------------------------------------------------------------
// BMP
// ---------------------------------------------------------------------------
static void put32le(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void put16le(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}

int imgenc_write_bmp24(const char *path, const uint32_t *px, int w, int h,
                       int stride) {
    if (!path || !px || w < 1 || h < 1 || w > 16384 || h > 16384) return -1;
    if (stride < w) stride = w;
    long row_bytes = ((long)w * 3 + 3) & ~3L;
    unsigned char *row = (unsigned char *)malloc((size_t)row_bytes);
    if (!row) return -1;

    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { free(row); return -1; }

    unsigned char hdr[54];
    memset(hdr, 0, sizeof(hdr));
    unsigned isz = (unsigned)(row_bytes * h);
    hdr[0] = 'B'; hdr[1] = 'M';
    put32le(hdr + 2, 54u + isz);
    put32le(hdr + 10, 54);
    put32le(hdr + 14, 40);
    put32le(hdr + 18, (unsigned)w);
    put32le(hdr + 22, (unsigned)h);
    put16le(hdr + 26, 1);
    put16le(hdr + 28, 24);
    put32le(hdr + 34, isz);
    if (imgenc_write_all(fd, hdr, 54) != 0) { sys_close(fd); free(row); return -1; }

    for (int y = h - 1; y >= 0; y--) {
        const uint32_t *src = px + (long)y * stride;
        long o = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = src[x];
            row[o++] = (unsigned char)(c & 0xFF);
            row[o++] = (unsigned char)((c >> 8) & 0xFF);
            row[o++] = (unsigned char)((c >> 16) & 0xFF);
        }
        while (o < row_bytes) row[o++] = 0;
        if (imgenc_write_all(fd, row, row_bytes) != 0) { sys_close(fd); free(row); return -1; }
    }
    sys_close(fd);
    free(row);
    return 0;
}

// ---------------------------------------------------------------------------
// PNG (stored deflate)
// ---------------------------------------------------------------------------
static uint32_t g_crc_table[256];
static int g_crc_ready = 0;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[n] = c;
    }
    g_crc_ready = 1;
}

static uint32_t crc_update(uint32_t crc, const unsigned char *p, long n) {
    uint32_t c = crc;
    for (long i = 0; i < n; i++) c = g_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void put32be(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

// Emit one chunk: length, type, data, crc(type+data).
static int png_chunk(int fd, const char *type, const unsigned char *data, long len) {
    unsigned char hdr[8], tail[4];
    put32be(hdr, (uint32_t)len);
    hdr[4] = (unsigned char)type[0]; hdr[5] = (unsigned char)type[1];
    hdr[6] = (unsigned char)type[2]; hdr[7] = (unsigned char)type[3];
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc_update(crc, hdr + 4, 4);
    if (len > 0) crc = crc_update(crc, data, len);
    put32be(tail, crc ^ 0xFFFFFFFFu);
    if (imgenc_write_all(fd, hdr, 8) != 0) return -1;
    if (len > 0 && imgenc_write_all(fd, data, len) != 0) return -1;
    return imgenc_write_all(fd, tail, 4);
}

static long png_raw_size(int w, int h, int bpp) {
    return (long)h * (1 + (long)w * bpp);
}

long imgenc_png_size(int w, int h, int with_alpha) {
    long raw = png_raw_size(w, h, with_alpha ? 4 : 3);
    long blocks = (raw + 65534) / 65535;
    if (blocks < 1) blocks = 1;
    // sig 8 + IHDR 25 + IDAT (12 + 2 + blocks*5 + raw + 4) + IEND 12
    return 8 + 25 + 12 + 2 + blocks * 5 + raw + 4 + 12;
}

int imgenc_write_png(const char *path, const uint32_t *px, int w, int h,
                     int stride, int with_alpha) {
    if (!path || !px || w < 1 || h < 1 || w > 16384 || h > 16384) return -1;
    if (stride < w) stride = w;
    if (!g_crc_ready) crc_init();
    int bpp = with_alpha ? 4 : 3;
    long raw = png_raw_size(w, h, bpp);
    long blocks = (raw + 65534) / 65535;
    long zlen = 2 + blocks * 5 + raw + 4;
    unsigned char *z = (unsigned char *)malloc((size_t)zlen);
    if (!z) return -1;

    // Build the zlib stream: header, stored blocks with the filtered rows
    // streamed straight into them, adler32 trailer.
    long zo = 0;
    z[zo++] = 0x78; z[zo++] = 0x01;
    uint32_t a = 1, b = 0;              // adler32 running sums
    long remaining = raw;
    long emitted = 0;                   // raw bytes emitted so far
    int y = 0, x = 0, comp = -1;        // filtered-row cursor: comp -1 = filter byte
    while (remaining > 0) {
        long chunk = remaining > 65535 ? 65535 : remaining;
        z[zo++] = (unsigned char)((remaining - chunk) == 0 ? 1 : 0);   // BFINAL
        z[zo++] = (unsigned char)(chunk & 0xFF);
        z[zo++] = (unsigned char)(chunk >> 8);
        z[zo++] = (unsigned char)(~chunk & 0xFF);
        z[zo++] = (unsigned char)((~chunk >> 8) & 0xFF);
        for (long i = 0; i < chunk; i++) {
            unsigned char v;
            if (comp < 0) {
                v = 0;                                  // filter type 0 (None)
                comp = 0;
            } else {
                uint32_t c = px[(long)y * stride + x];
                if (comp == 0) v = (unsigned char)(c >> 16);        // R
                else if (comp == 1) v = (unsigned char)(c >> 8);    // G
                else if (comp == 2) v = (unsigned char)c;           // B
                else v = (unsigned char)(c >> 24);                  // A
                comp++;
                if (comp == bpp) { comp = 0; x++; if (x == w) { x = 0; y++; comp = -1; } }
            }
            z[zo++] = v;
            a += v; if (a >= 65521) a -= 65521;
            b += a; if (b >= 65521) b -= 65521;
        }
        remaining -= chunk;
        emitted += chunk;
    }
    (void)emitted;
    uint32_t adler = (b << 16) | a;
    put32be(z + zo, adler); zo += 4;

    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { free(z); return -1; }
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    unsigned char ihdr[13];
    put32be(ihdr, (uint32_t)w);
    put32be(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;                                   // bit depth
    ihdr[9] = (unsigned char)(with_alpha ? 6 : 2); // colour type
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;      // deflate, filter 0, no interlace
    int ok = 0;
    if (imgenc_write_all(fd, sig, 8) == 0 &&
        png_chunk(fd, "IHDR", ihdr, 13) == 0 &&
        png_chunk(fd, "IDAT", z, zo) == 0 &&
        png_chunk(fd, "IEND", 0, 0) == 0) ok = 1;
    sys_close(fd);
    free(z);
    return ok ? 0 : -1;
}
