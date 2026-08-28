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
#include "../../libc/userconf.h"  // #148: userhome_path() - the user's own home dir join
#include "../../libc/notify.h"    // #148: notify_post() - toast confirming a saved shot
#include "../../libc/stdio.h"     // #148: snprintf() for the toast body
#include "../../libc/tz.h"        // #148 (local 164): tz_local_stamp() - THE local-clock stamp

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

// ===========================================================================
// #148: PrintScreen hotkey - a DIFFERENT save target from screenshot_capture()
// above on purpose. That one trades quality for size to survive an SSH exec
// read (downscaled, 8-bit 3-3-2 palette); a key someone presses to grab their
// own screen has no remote-read size ceiling and wants a full-quality shot.
//
// SAVE LOCATION (owner decision, 2026-08-18, local 164): <home>/SCREENSHOTS,
// via userhome_path() - THE home join (userconf.c), the same one desktop.c
// uses for <home>/DESKTOP. This used to be <home>/PICTURES ("every other
// desktop OS puts screenshots in Pictures"); the owner overrode that today
// with an explicit single directory for ALL captures (this hotkey AND Maytera
// Snap's own manual Save, see snapshot/main.c), so there is one place to look,
// not two. SCREENSHOTS is uppercase to match every sibling home subdirectory
// (DESKTOP, PICTURES, CONFIG, APPS, ...) users_make_home_skeleton() creates
// (kernel/proc/users.c) - root's own session (home "/") does not get that
// skeleton, which is why this still mkdir()s it on demand below.
// ===========================================================================

#define SHOT_HOTKEY_SUB   "SCREENSHOTS"
// Real hardware and the framebuffer geometry this desktop actually ships
// (see the module comment's "1280x800" example) stay far under this; it is a
// sanity ceiling so a corrupt/absurd g_fb_width can only make the capture
// fail cleanly, never overrun the static row buffer below.
#define SHOT_HOTKEY_MAX_W 4096

// Full-resolution 24-bit BMP writer: the SAME g_fb sampling shape as
// screenshot_capture() above (bottom-up rows, one row of pixels read
// straight from the composited backbuffer per iteration), with neither the
// downscale nor the 8-bit palette quantization - this file is written once
// to local disk, not pulled over a slow SSH channel, so neither constraint
// applies. Returns 0 on success, negative on failure.
static int screenshot_capture_full(const char *path) {
    if (!g_fb || g_fb_width <= 0 || g_fb_height <= 0) return -1;
    if (g_fb_width > SHOT_HOTKEY_MAX_W) return -1;   // see the ceiling comment above

    const int W = (int)g_fb_width;
    const int H = (int)g_fb_height;
    const int P = (int)g_fb_pitch;

    int row_bytes = (W * 3 + 3) & ~3;   // BMP rows pad to a 4-byte boundary

    unsigned char hdr[14 + 40];
    unsigned int pixels_off = sizeof(hdr);
    unsigned int filesize   = pixels_off + (unsigned int)row_bytes * (unsigned int)H;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    put_u32(hdr + 2, filesize);
    put_u32(hdr + 10, pixels_off);
    put_u32(hdr + 14, 40);
    put_u32(hdr + 18, (unsigned int)W);
    put_u32(hdr + 22, (unsigned int)H);   // positive => bottom-up rows
    put_u16(hdr + 26, 1);                 // planes
    put_u16(hdr + 28, 24);                // bits per pixel
    put_u32(hdr + 30, 0);                 // BI_RGB (no compression)
    put_u32(hdr + 34, (unsigned int)row_bytes * (unsigned int)H);
    put_u32(hdr + 38, 2835);              // ~72 DPI x
    put_u32(hdr + 42, 2835);              // ~72 DPI y

    int fd = sys_open(path, SHOT_O_WRITE);
    if (fd < 0) return -2;
    if (shot_write_all(fd, hdr, sizeof(hdr)) < 0) { sys_close(fd); return -3; }

    static unsigned char rowbuf[SHOT_HOTKEY_MAX_W * 3 + 4];
    for (int y = H - 1; y >= 0; y--) {              // BMP is bottom-up
        const uint32_t *srow = &g_fb[y * P];
        int o = 0;
        for (int x = 0; x < W; x++) {
            uint32_t c = srow[x];                    // 0x00RRGGBB
            rowbuf[o++] = (unsigned char)(c & 0xFF);         // B
            rowbuf[o++] = (unsigned char)((c >> 8) & 0xFF);  // G
            rowbuf[o++] = (unsigned char)((c >> 16) & 0xFF); // R
        }
        for (int p = W * 3; p < row_bytes; p++) rowbuf[p] = 0;
        if (shot_write_all(fd, rowbuf, (unsigned long)row_bytes) < 0) {
            sys_close(fd);
            return -4;
        }
    }
    sys_close(fd);
    return 0;
}

// Pick the first unused <home>/<dirsub>/SHOT-<local-stamp>.BMP slot (dirsub
// may be 0 for a bare <home>/SHOT-<stamp>.BMP - see the fallback note on
// screenshot_hotkey_fire() below). Returns 0 and fills `out` (>= 256 bytes)
// on success, -1 if the path cannot even be NAMED (buffer too small).
//
// TIMESTAMPED, NOT SEQUENTIAL (#148 today's deliverable 2, local 164):
// tz_local_stamp() (userland/libc/tz.c) - LOCAL time, not UTC. Every visible
// clock in this OS (taskbar, clock widget, Settings) is local; the RTC read
// underneath it is UTC and has been correct since #135 fixed the BCD/binary
// write-encoder, and tz_local_now() is THE one place the offset is applied
// (tz.h). A screenshot's filename is a "when did I take this" label a human
// reads later, the same category as every other clock in the OS, NOT an
// absolute-instant value like a TOTP code or an audit-log line - tz.h itself
// documents that distinction and which callers must stay on the raw-RTC/UTC
// side of it; a screenshot name belongs on the local side. Zero-padded
// YYYYMMDD-HHMMSS so a plain lexical (filename) sort is also a chronological
// sort, which a counter reset to 1 on every boot never was.
//
// Same-second collision (two presses inside one RTC tick): append -2, -3, ...
// rather than silently overwrite the first shot.
static int shot_hotkey_pick_name(const char *dirsub, char *out, unsigned long cap) {
    char stamp[TZ_STAMP_LEN];
    tz_local_stamp(stamp, sizeof(stamp));

    for (int suffix = 0; suffix <= 99; suffix++) {
        char name[40];
        if (suffix == 0) snprintf(name, sizeof(name), "SHOT-%s.BMP", stamp);
        else             snprintf(name, sizeof(name), "SHOT-%s-%d.BMP", stamp, suffix + 1);
        if (userhome_path(dirsub, name, out, cap) != 0) return -1;
        int fd = sys_open(out, 0 /* O_RDONLY */);
        if (fd >= 0) { sys_close(fd); continue; }   // already taken this second
        return 0;
    }
    return -1;   // 100 shots in the same second without a gap: give up honestly
}

// ===========================================================================
// #148 (local 164, 2026-08-18): the fullscreen-only toast. Owner's spec:
// "when we're full screen just fire and forget capture, maybe overlay a
// thumbnail of the screenshot in the bottom right of the screen as a toast
// 'Screenshot saved' for 2 seconds while maintaining the native full screen."
//
// This CANNOT go through the ordinary notif.c toast system (top-right,
// text-only, driven by the normal damage/composite path) because #158
// deliberately SUPPRESSES per-window compositing while a window is native-
// fullscreen - that suppression is the entire feature, and resuming full
// compositing to draw a toast would defeat it. So this toast is drawn
// DIRECTLY into g_fb from inside main.c's native_fullscreen_try_render()
// success path (see the call site in main.c), the same back buffer #158's
// own SYS_WM_FULLSCREEN_RENDER just wrote the app's fresh frame into -
// never through notif.c, draw.c's window-content blitters, or a resumed
// composite.
//
// GHOSTING: every call to native_fullscreen_try_render() that succeeds
// re-blits the app's FULL committed content rect from scratch (kernel/proc/
// syscall.c sys_wm_fullscreen_render() row-copies sw x sh unconditionally,
// every time it is called). So this toast needs NO restore/erase step: once
// screenshot_fs_toast_active() returns false, the caller simply stops
// calling screenshot_fs_toast_draw(), and the very next successful fullscreen
// render overwrites the whole region with the app's own current frame. The
// owner's warned-about failure mode (stale pixels after #158's screen-margin
// clear, and the cursor-only partial-present path that used to re-expose
// them - see fullscreen_in_list()'s exclusion of cursor_only in main.c) is a
// DIFFERENT hazard: a path that DOESN'T re-blit the whole rect every frame.
// The native-fullscreen fast path this toast rides always does, so that
// class of bug does not apply here BY CONSTRUCTION, not by care taken in
// this file alone - see the "force a render tick" fold-in in main.c for the
// one gap that remains (a perfectly STATIC fullscreen app might not tick
// the fast path every frame on its own).
// ===========================================================================
#define FS_TOAST_THUMB_W  160
#define FS_TOAST_THUMB_H  100
#define FS_TOAST_DWELL_MS 2000
#define FS_TOAST_MARGIN   16
#define FS_TOAST_PAD      10

static uint32_t g_fs_toast_thumb[FS_TOAST_THUMB_W * FS_TOAST_THUMB_H];
static int      g_fs_toast_active = 0;
static uint64_t g_fs_toast_start_ms = 0;

// Downscale g_fb - the frame screenshot_capture_full() JUST sampled; nothing
// has touched g_fb between that call and this one - into the fixed-size
// toast thumbnail. Box-average, same shape as screenshot_capture()'s own
// downscale above (an integer box per output pixel), producing in-memory RGB
// instead of an 8-bit palette BMP file - the algorithm is reused, not a
// second scaling design; only the output format differs because this one is
// blitted straight to screen, never written to disk.
static void fs_toast_build_thumb(void) {
    if (!g_fb || g_fb_width <= 0 || g_fb_height <= 0) return;
    for (int oy = 0; oy < FS_TOAST_THUMB_H; oy++) {
        int sy0 = oy * g_fb_height / FS_TOAST_THUMB_H;
        int sy1 = (oy + 1) * g_fb_height / FS_TOAST_THUMB_H;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > g_fb_height) sy1 = g_fb_height;
        for (int ox = 0; ox < FS_TOAST_THUMB_W; ox++) {
            int sx0 = ox * g_fb_width / FS_TOAST_THUMB_W;
            int sx1 = (ox + 1) * g_fb_width / FS_TOAST_THUMB_W;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > g_fb_width) sx1 = g_fb_width;
            unsigned int sr = 0, sg = 0, sb = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint32_t *srow = &g_fb[sy * g_fb_pitch];
                for (int sx = sx0; sx < sx1; sx++) {
                    uint32_t c = srow[sx];
                    sr += (c >> 16) & 0xFF; sg += (c >> 8) & 0xFF; sb += c & 0xFF;
                    n++;
                }
            }
            uint32_t out = n ? (0xFF000000u | (((sr / n) & 0xFF) << 16) |
                                              (((sg / n) & 0xFF) << 8)  |
                                               ((sb / n) & 0xFF))
                             : 0xFF000000u;
            g_fs_toast_thumb[oy * FS_TOAST_THUMB_W + ox] = out;
        }
    }
}

// Called from screenshot_hotkey_fire() below when the capture happened while
// native-fullscreen. Builds the thumbnail and (re)starts the 2s dwell. A
// second PrintScreen press mid-dwell just re-arms it with the new shot - no
// queue, matching "fire and forget".
static void screenshot_fs_toast_arm(void) {
    fs_toast_build_thumb();
    g_fs_toast_active = 1;
    g_fs_toast_start_ms = uptime_ms();
}

// Read by main.c: (a) to decide whether to call screenshot_fs_toast_draw()
// this frame, and (b) folded into the "force a full render tick" interactive
// predicate so the dwell is guaranteed to end on a real frame even if the
// fullscreen app itself is static (see the "force a render tick" comment
// above). Self-clearing: once expired it flips g_fs_toast_active off and
// every later call is a cheap `return 0` until the next arm.
int screenshot_fs_toast_active(void) {
    if (!g_fs_toast_active) return 0;
    if (uptime_ms() - g_fs_toast_start_ms >= FS_TOAST_DWELL_MS) {
        g_fs_toast_active = 0;
        return 0;
    }
    return 1;
}

// Draw the toast panel (thumbnail + "Screenshot saved") bottom-right, DIRECTLY
// into g_fb via plain pointer writes - the same pattern draw.c's own
// draw_putpixel()/draw_fill_rect() already use (g_fb is a real mapped buffer,
// not something reached through a syscall), so the thumbnail blit is not
// routed through kernel fb_put_row() (that helper is kernel-internal to
// sys_wm_fullscreen_render(), not exposed to userland). Chrome (panel, border,
// text) reuses the exact primitives notif.c's draw_toast() uses
// (draw_rounded_rect/draw_rect_outline/draw_text_ttf, CLR_MENU_BG/BORDER,
// readable_ink()) for a consistent look, not a bespoke style.
void screenshot_fs_toast_draw(void) {
    if (!screenshot_fs_toast_active()) return;
    if (!g_fb || g_fb_width <= 0 || g_fb_height <= 0) return;

    int w = FS_TOAST_THUMB_W + FS_TOAST_PAD * 2;
    int h = FS_TOAST_THUMB_H + FS_TOAST_PAD * 2 + 24;   // + text row
    int x = g_fb_width  - w - FS_TOAST_MARGIN;
    int y = g_fb_height - h - FS_TOAST_MARGIN;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    draw_rounded_rect(x, y, w, h, 9, CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);

    int tx = x + FS_TOAST_PAD, ty = y + FS_TOAST_PAD;
    for (int ry = 0; ry < FS_TOAST_THUMB_H; ry++) {
        int py = ty + ry;
        if (py < 0 || py >= g_fb_height) continue;
        uint32_t *dst = &g_fb[py * g_fb_pitch];
        const uint32_t *src = &g_fs_toast_thumb[ry * FS_TOAST_THUMB_W];
        for (int rx = 0; rx < FS_TOAST_THUMB_W; rx++) {
            int px = tx + rx;
            if (px < 0 || px >= g_fb_width) continue;
            dst[px] = src[rx];
        }
    }
    draw_rect_outline(tx, ty, FS_TOAST_THUMB_W, FS_TOAST_THUMB_H, CLR_MENU_BORDER);

    uint32_t ink = readable_ink(CLR_MENU_BG);
    draw_text_ttf(tx, ty + FS_TOAST_THUMB_H + 6, "Screenshot saved", 14, ink);
}

// screenshot_hotkey_fire: the PrintScreen entry point, called once per press
// from main.c's process_events() global-shortcuts block. Not a busy/spin loop
// and not called every frame (see the module comment's #426 note) - it runs
// exactly once, synchronously, on the key press, the same shape
// screenshot_poll() already uses for the SSH-triggered capture just above.
//
// #148 (local 164, 2026-08-18) OWNER'S TWO-PATH SPEC, both triggered by this
// one hotkey, both capturing the full screen:
//   A. Native-fullscreen (#158) on top: fire-and-forget, STAY fullscreen, and
//      show the bottom-right thumbnail toast above instead of the normal
//      notify_post() toast (which cannot draw while fullscreen compositing
//      is suppressed - see the toast block's own module comment).
//   B. Anything else: open Maytera Snap showing the preview, positioned
//      bottom-left, WITHOUT stealing focus from whatever the user was
//      working in (win_create_bg(), #148 local 164 - see kernel/proc/
//      syscall.h SYS_WIN_CREATE_BG and snapshot/main.c's preview_mode).
// The fullscreen check is READ-ONLY (sys_wm_fullscreen_status()) and runs
// AFTER the capture above, which is what "capture first" actually requires:
// neither branch below ever calls anything that changes fullscreen state, so
// there is nothing for the capture to race.
void screenshot_hotkey_fire(void) {
    char path[256];

    // Ensure <home>/SCREENSHOTS exists. users_make_home_skeleton() creates it
    // for every user session, but a root session's home is "/" (see
    // userconf.c's root-is-a-no-op note) and a root desktop is not
    // guaranteed to have /SCREENSHOTS; mkdir is idempotent, so this is the
    // same "create if missing, ignore if it's already there" idiom
    // userconf_open_write() uses for <home>/CONFIG, not a second one.
    char dir[256];
    if (userhome_path(0, SHOT_HOTKEY_SUB, dir, sizeof(dir)) == 0) {
        sys_mkdir(dir, 0755);
    }

    // #148 (deliverable: a create/write failure - read-only media, full disk,
    // a home tree that mkdir above could not actually create - must not lose
    // the capture or fail silently). Try <home>/SCREENSHOTS first; only if
    // the ACTUAL WRITE fails (screenshot_capture_full()'s own open-for-write,
    // not merely "does a file already exist here") fall back to a bare
    // <home>/SHOT-<stamp>.BMP, which needs no subdirectory to exist. Report
    // the real failure only if neither location took the shot.
    int ok = 0;
    if (shot_hotkey_pick_name(SHOT_HOTKEY_SUB, path, sizeof(path)) == 0 &&
        screenshot_capture_full(path) == 0) {
        ok = 1;
    } else if (shot_hotkey_pick_name(0, path, sizeof(path)) == 0 &&
               screenshot_capture_full(path) == 0) {
        ok = 1;   // fallback location took it - the capture is not lost
    }

    if (!ok) {
        // This failure toast goes through the normal notify_post() path
        // regardless of fullscreen state. That is a known limitation, not an
        // oversight: it will not be VISIBLE while native-fullscreen (same
        // suppression the success toast above works around), but a failure
        // here is rare (disk full / read-only media) and the alternative -
        // routing an error string through the bottom-right pixel toast too -
        // was judged not worth the complexity for today's ticket.
        notify_post("Screenshot", "Could not save screenshot", NOTIFY_ERROR);
        return;
    }

    if (sys_wm_fullscreen_status() >= 0) {
        // Path A.
        screenshot_fs_toast_arm();
        return;
    }

    // Path B.
    char *av[3];
    av[0] = (char *)"/APPS/SNAPSHOT";
    av[1] = path;
    av[2] = (char *)"--preview";
    if (sys_spawn_args("/APPS/SNAPSHOT", av, 3) < 0) {
        // Spawn failed (out of process slots, missing binary, ...): still
        // confirm the file was saved rather than leaving the user with no
        // signal at all.
        char body[192];
        snprintf(body, sizeof(body), "Saved to %s", path);
        notify_post("Screenshot", body, NOTIFY_SUCCESS);
    }
}
