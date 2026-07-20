// screenshot.c - Remote screen capture for the MayteraOS compositor.
//
// The machine is real hardware driven over SSH (no host-side QMP capture), so
// we grab the compositor's own composited backbuffer (g_fb, the exact buffer
// fb_flip presents) and write it to a small, valid image file that SSH can
// `cat` back. Two triggers, both file-driven so they work over an SSH exec
// channel whose stdout is not captured:
//
//   1. The msh `screenshot [path]` builtin writes /SCREENSHOT.REQ.
//   2. Anything that creates /SCREENSHOT.REQ (e.g. `echo 1 > /SCREENSHOT.REQ`).
//
// screenshot_poll() is called once per compositor frame from the main loop
// (NOT a busy/spin loop, see CLAUDE.md #426): it checks for the request file at
// the normal idle/frame cadence, captures, then deletes the request.
//
// OUTPUT SIZE CONSTRAINT: the on-device file READ over SSH is reliable only for
// files under ~500KB (a full 1280x800x32 fb ~4MB reads back as 0 bytes). So the
// capture is DOWNSCALED by an integer box-average factor to keep the long edge
// small, and written as an 8-bit paletted (3-3-2) BMP. For a 1280x800 desktop
// this yields a 640x400 image of ~257KB, comfortably under the ceiling, that
// opens in any normal image viewer.

#include "compositor.h"
#include "../../libc/syscall.h"

// Pixel-data budget (bytes). 8-bit paletted, so bytes ~= out_w * out_h. Kept
// well under the ~350KB SSH-read safety target (headers + 256-entry palette add
// only ~1078 bytes on top).
#define SHOT_MAX_LONG_EDGE 700     // downscale so the long edge <= this
#define SHOT_MAX_OUT_W     1024    // static row-buffer ceiling

static const char *SHOT_REQ_PATH     = "/SCREENSHOT.REQ";
static const char *SHOT_DEFAULT_PATH = "/SCREENSHOT.BMP";

// O_WRONLY | O_CREAT | O_TRUNC  (same flags the rest of the compositor uses).
#define SHOT_O_WRITE (0x41 | 0x200)

// ---- tiny little-endian header writers ---------------------------------------
static void put_u16(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}
static void put_u32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

// Write the whole buffer, tolerating short writes.
static int shot_write_all(int fd, const void *buf, unsigned long n) {
    const unsigned char *p = (const unsigned char *)buf;
    unsigned long done = 0;
    while (done < n) {
        long w = sys_write(fd, p + done, n - done);
        if (w <= 0) return -1;
        done += (unsigned long)w;
    }
    return 0;
}

// Quantize an 8-bit-per-channel color to a 3-3-2 palette index.
//   bits 7..5 = R (3), bits 4..2 = G (3), bits 1..0 = B (2)
static inline unsigned char rgb_to_332(unsigned int r, unsigned int g, unsigned int b) {
    return (unsigned char)((r & 0xE0) | ((g & 0xE0) >> 3) | ((b & 0xC0) >> 6));
}

// screenshot_capture: sample the current composited backbuffer (g_fb), downscale
// by an integer box-average factor, and write an 8-bit 3-3-2 BMP to `path`.
// Returns 0 on success, negative on failure.
int screenshot_capture(const char *path) {
    if (!g_fb || g_fb_width <= 0 || g_fb_height <= 0) return -1;

    const int W = (int)g_fb_width;
    const int H = (int)g_fb_height;
    const int P = (int)g_fb_pitch;

    // Choose the smallest integer downscale factor keeping the long edge small.
    int longedge = (W > H) ? W : H;
    int s = 1;
    while ((longedge + s - 1) / s > SHOT_MAX_LONG_EDGE) s++;

    int ow = W / s;
    int oh = H / s;
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
    if (ow > SHOT_MAX_OUT_W) ow = SHOT_MAX_OUT_W;

    // BMP rows are padded to a 4-byte boundary.
    int row_bytes = (ow + 3) & ~3;

    // --- headers + palette -----------------------------------------------------
    unsigned char hdr[14 + 40];
    unsigned char pal[256 * 4];
    unsigned int palette_off = 14 + 40;
    unsigned int pixels_off  = palette_off + sizeof(pal);
    unsigned int filesize    = pixels_off + (unsigned int)row_bytes * (unsigned int)oh;

    memset(hdr, 0, sizeof(hdr));
    // BITMAPFILEHEADER
    hdr[0] = 'B'; hdr[1] = 'M';
    put_u32(hdr + 2, filesize);
    put_u32(hdr + 10, pixels_off);
    // BITMAPINFOHEADER (40 bytes)
    put_u32(hdr + 14, 40);
    put_u32(hdr + 18, (unsigned int)ow);
    put_u32(hdr + 22, (unsigned int)oh);   // positive => bottom-up rows
    put_u16(hdr + 26, 1);                  // planes
    put_u16(hdr + 28, 8);                  // bits per pixel
    put_u32(hdr + 30, 0);                  // BI_RGB (no compression)
    put_u32(hdr + 34, (unsigned int)row_bytes * (unsigned int)oh);
    put_u32(hdr + 38, 2835);               // ~72 DPI x
    put_u32(hdr + 42, 2835);               // ~72 DPI y
    put_u32(hdr + 46, 256);                // colors used
    put_u32(hdr + 50, 0);                  // colors important

    // 3-3-2 palette entries, stored B,G,R,0.
    for (int i = 0; i < 256; i++) {
        unsigned int r3 = (unsigned int)((i >> 5) & 0x07);
        unsigned int g3 = (unsigned int)((i >> 2) & 0x07);
        unsigned int b2 = (unsigned int)(i & 0x03);
        pal[i * 4 + 0] = (unsigned char)((b2 * 255) / 3);
        pal[i * 4 + 1] = (unsigned char)((g3 * 255) / 7);
        pal[i * 4 + 2] = (unsigned char)((r3 * 255) / 7);
        pal[i * 4 + 3] = 0;
    }

    int fd = sys_open(path, SHOT_O_WRITE);
    if (fd < 0) return -2;

    if (shot_write_all(fd, hdr, sizeof(hdr)) < 0) { sys_close(fd); return -3; }
    if (shot_write_all(fd, pal, sizeof(pal)) < 0) { sys_close(fd); return -3; }

    // --- pixels: bottom-up, one downscaled row at a time -----------------------
    static unsigned char rowbuf[SHOT_MAX_OUT_W + 4];
    unsigned int inv = (unsigned int)(s * s);  // box size for averaging

    for (int oy = oh - 1; oy >= 0; oy--) {          // BMP is bottom-up
        int sy0 = oy * s;
        for (int ox = 0; ox < ow; ox++) {
            int sx0 = ox * s;
            unsigned int sr = 0, sg = 0, sb = 0;
            for (int dy = 0; dy < s; dy++) {
                const uint32_t *srow = &g_fb[(sy0 + dy) * P + sx0];
                for (int dx = 0; dx < s; dx++) {
                    uint32_t c = srow[dx];              // 0x00RRGGBB
                    sr += (c >> 16) & 0xFF;
                    sg += (c >> 8) & 0xFF;
                    sb += c & 0xFF;
                }
            }
            rowbuf[ox] = rgb_to_332(sr / inv, sg / inv, sb / inv);
        }
        // pad to 4-byte boundary
        for (int p = ow; p < row_bytes; p++) rowbuf[p] = 0;
        if (shot_write_all(fd, rowbuf, (unsigned long)row_bytes) < 0) {
            sys_close(fd);
            return -4;
        }
    }

    sys_close(fd);
    return 0;
}

// screenshot_poll: called once per compositor frame. Cheap open() of the request
// file; when present, read an optional target path from it, delete it, capture.
// No busy-wait: this rides the existing adaptive frame/idle cadence.
void screenshot_poll(void) {
    int fd = sys_open(SHOT_REQ_PATH, 0 /* O_RDONLY */);
    if (fd < 0) return;   // no request pending (fast common path)

    char buf[160];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';

    // Consume the request so we do not re-capture every frame.
    sys_unlink(SHOT_REQ_PATH);

    // If the request body names an absolute path (starts with '/'), honor it;
    // otherwise write the default. Stop the path at the first whitespace/newline.
    const char *path = SHOT_DEFAULT_PATH;
    if (buf[0] == '/') {
        for (int i = 0; buf[i]; i++) {
            if (buf[i] == '\r' || buf[i] == '\n' || buf[i] == ' ' || buf[i] == '\t') {
                buf[i] = '\0';
                break;
            }
        }
        path = buf;
    }

    screenshot_capture(path);
}
