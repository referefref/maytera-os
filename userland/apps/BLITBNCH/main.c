// BLITBNCH - #blitguard: what sys_win_blit() actually costs, per pixel.
//
// WHY A DEDICATED APP AND NOT A TRACE OF A REAL ONE
// -------------------------------------------------
// The visualiser, chess and glcube all blit, but each does so inside its own
// frame loop, so scraping their syscall totals measures their loop as much as
// the syscall. This runs the SAME BINARY against a before-kernel and an
// after-kernel, does nothing but blit, and reports microseconds per megapixel,
// which is the only form of the number that can be quoted at a resolution the
// test machine does not have.
//
// TWO PHASES, because they answer different questions:
//   A  BLIT ONLY. win_invalidate() is never called, so uw->ever_committed
//      stays 0 and sys_win_blit() skips uw_commit_content(). This isolates the
//      copy this change actually rewrote.
//   B  BLIT + PRESENT. win_invalidate() every frame, which is what every real
//      caller does, so the number includes the TWO full-buffer memcpys
//      (blit's own commit plus invalidate's) and the whole-window damage
//      rect. This is the honest per-frame cost of using this API.
//
// AND TWO GEOMETRIES, because the fix took two different paths:
//   1:1    src_w == dst_w. No horizontal resampling, so the new code
//          copy_from_user's each source row STRAIGHT into the destination row.
//          This is what every real caller does.
//   SCALED src half the destination's width, so the new code must bounce a row
//          into kernel memory and resample it. The path that pays for safety.
#include <stdio.h>
#include <stdlib.h>
#include "syscall.h"

#define SYS_WIN_BLIT_NUM 35

static long blit_raw(int h, unsigned int sw, unsigned int sh, unsigned long p) {
    unsigned long packed = (sw & 0xFFFFu) | ((sh & 0xFFFFu) << 16);
    return syscall5(SYS_WIN_BLIT_NUM, (long)h, 0, 0, (long)packed, (long)p);
}

static void emit(const char *s) { printf("%s\n", s); sys_bootlog(s); }

// No trustworthy snprintf in this freestanding subset for what follows, so the
// line is assembled by hand. Ugly, exact.
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *putu(char *p, unsigned long long v) {
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}

static void bench(const char *label, int h, int dw, int dh,
                  unsigned int *buf, unsigned int sw, unsigned int sh,
                  int frames, int present) {
    // Warm: fault the buffer in and let any first-call growth settle, so the
    // measured window is steady state rather than one-off setup.
    for (int i = 0; i < 8; i++) {
        blit_raw(h, sw, sh, (unsigned long)buf);
        if (present) win_invalidate(h);
    }
    unsigned long long t0 = mono_us();
    for (int i = 0; i < frames; i++) {
        blit_raw(h, sw, sh, (unsigned long)buf);
        if (present) win_invalidate(h);
    }
    unsigned long long t1 = mono_us();
    unsigned long long tot = (t1 > t0) ? (t1 - t0) : 0;
    unsigned long long per = tot / (unsigned)frames;            // us per frame
    unsigned long long px  = (unsigned long long)dw * (unsigned long long)dh;
    // us per megapixel, x1000 so one decimal survives integer division
    unsigned long long uspermpx = px ? (tot * 1000ULL * 1000000ULL) / (px * (unsigned)frames) : 0;

    char line[240]; char *p = line;
    p = put(p, "[BLITBNCH] ");
    p = put(p, label);
    p = put(p, " dst="); p = putu(p, (unsigned)dw); *p++ = 'x'; p = putu(p, (unsigned)dh);
    p = put(p, " src="); p = putu(p, sw); *p++ = 'x'; p = putu(p, sh);
    p = put(p, present ? " present=yes" : " present=no ");
    p = put(p, " frames="); p = putu(p, (unsigned)frames);
    p = put(p, " us/frame="); p = putu(p, per);
    p = put(p, " ns/Mpx="); p = putu(p, uspermpx);
    // The whole point of the ns/Mpx column: multiply out to a 4K panel.
    p = put(p, " => 3840x2160 us/frame="); p = putu(p, (uspermpx * 8294400ULL) / 1000000000ULL);
    *p = 0;
    emit(line);
}

int main(void) {
    emit("[BLITBNCH] start: sys_win_blit cost, us per megapixel");

    int h = win_create("blit bench", 20, 20, 1200, 720);
    if (h < 0) { emit("[BLITBNCH] ABORT win_create failed"); return 1; }
    int dw = 0, dh = 0;
    if (win_get_size(h, &dw, &dh) != 0 || dw < 1 || dh < 1) {
        emit("[BLITBNCH] ABORT win_get_size failed"); win_destroy(h); return 1;
    }

    unsigned int *buf = (unsigned int *)malloc((size_t)dw * (size_t)dh * 4u);
    if (!buf) { emit("[BLITBNCH] ABORT malloc failed"); win_destroy(h); return 1; }
    for (long i = 0; i < (long)dw * (long)dh; i++) buf[i] = 0xFF3060A0u + (unsigned)(i & 31);

    // 1:1, the path every real caller takes.
    bench("1to1  ", h, dw, dh, buf, (unsigned)dw, (unsigned)dh, 120, 0);
    bench("1to1  ", h, dw, dh, buf, (unsigned)dw, (unsigned)dh, 120, 1);
    // Scaled: half-size source stretched over the same window.
    bench("scaled", h, dw, dh, buf, (unsigned)(dw / 2), (unsigned)(dh / 2), 120, 0);
    bench("scaled", h, dw, dh, buf, (unsigned)(dw / 2), (unsigned)(dh / 2), 120, 1);

    emit("[BLITBNCH] done");
    free(buf);
    sys_sleep(500);
    win_destroy(h);
    return 0;
}
