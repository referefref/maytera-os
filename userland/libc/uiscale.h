#ifndef _LIBC_UISCALE_H
#define _LIBC_UISCALE_H
// uiscale.h (Ring 3) - the global UI scale factor, for userland.
//
// THE FACTOR ITSELF IS NOT HERE. It lives in exactly one place, the kernel
// (rustkern/uiscale.rs), and this header reads it. Read that file's header for
// the design and the rounding contract; nothing is restated here that could
// drift from it.
//
// WHY THERE IS ARITHMETIC IN THIS FILE AT ALL, given that duplicating a value
// or a formula is this project's most expensive recurring fault:
//
//   Most Ring 3 code needs NO arithmetic here. An ordinary app draws through
//   the window syscalls and the kernel scales its coordinates at the boundary,
//   so the app never sees the factor and there is nothing to duplicate. That
//   covers Settings, Files, Terminal, the first-run wizard, all of it.
//
//   The COMPOSITOR is the exception, and it is a real one. It owns the
//   framebuffer, draws the dock, the taskbar and the desktop at absolute screen
//   coordinates, and therefore must scale its own chrome. It does that a few
//   thousand times a frame, so a syscall per coordinate is not available.
//
// So the formula is duplicated, and the duplication is made CHECKABLE rather
// than promised: ui_scale_selfcheck() below asks the KERNEL for the same
// answers through the UISC_PX/UNPX/SPAN oracle opcodes and compares. If they
// ever disagree, it says so, loudly, with the values. A copied formula with a
// live oracle is a different thing from a copied formula with a comment.

#include "syscall.h"

// --- the live value, one syscall ------------------------------------------
static inline int ui_scale_pct(void)    { return (int)syscall2(SYS_UI_SCALE, UISC_GET, 0); }
static inline int ui_scale_auto(void)   { return (int)syscall2(SYS_UI_SCALE, UISC_AUTO, 0); }
static inline int ui_scale_max(void)    { return (int)syscall2(SYS_UI_SCALE, UISC_MAX, 0); }
static inline int ui_scale_src(void)    { return (int)syscall2(SYS_UI_SCALE, UISC_SRC, 0); }
static inline int ui_scale_gen(void)    { return (int)syscall2(SYS_UI_SCALE, UISC_GEN, 0); }
static inline int ui_scale_laptop(void) { return (int)syscall2(SYS_UI_SCALE, UISC_LAPTOP, 0); }
// The REAL display geometry, unaffected by scale. fb_info() answers a
// scale-transparent app in LOGICAL pixels on purpose (that is what it draws
// in); this is for the one case that needs the truth on screen: telling the
// user what their panel is running at.
static inline void ui_scale_fb_phys(int *w, int *h) {
    int v = (int)syscall2(SYS_UI_SCALE, UISC_FBPHYS, 0);
    if (w) *w = (v >> 16) & 0xFFFF;
    if (h) *h = v & 0xFFFF;
}
// Returns the percent ACTUALLY ADOPTED after clamping, which may differ from
// what was asked. Show the return value, never the argument.
static inline int ui_scale_set(int pct) { return (int)syscall2(SYS_UI_SCALE, UISC_SET, (long)pct); }
static inline int ui_scale_save(void)   { return (int)syscall2(SYS_UI_SCALE, UISC_SAVE, 0); }

// "I draw in REAL screen pixels; do not scale my coordinates." The COMPOSITOR
// calls this, and nothing else should. Call it FIRST, before any fb_info():
// the kernel's framebuffer-owner backstop is not yet true at that point, and
// answering fb_info() with the logical size is what painted the wallpaper into
// the top-left 1280x720 of a 1920x1080 screen the first time this was tried.
static inline int ui_scale_mark_native(void) { return (int)syscall2(SYS_UI_SCALE, UISC_NATIVE, 0); }

// --- the cached factor, for code that scales thousands of values a frame ---
// Refresh with ui_scale_refresh() once per frame (it is one syscall, and it
// only does work when the generation counter has moved).
extern int g_ui_scale_pct;
extern int g_ui_scale_gen;

// Returns 1 if the factor CHANGED, so a caller can re-derive cached geometry.
int  ui_scale_refresh(void);
// 0 = the local arithmetic agrees with the kernel over the checked range;
// otherwise the number of disagreements. Call once at startup and report it.
int  ui_scale_selfcheck(void);

// --- the arithmetic. MUST MATCH rustkern/uiscale.rs; selfcheck proves it. ---

// LOGICAL -> PHYSICAL, one coordinate or one standalone length. Round half up,
// odd in the sign, and a nonzero never becomes zero.
static inline int ui_px(int v) {
    int p = g_ui_scale_pct;
    if (p == 100 || v == 0) return v;
    long long a = v < 0 ? -(long long)v : (long long)v;
    long long r = (a * p + 50) / 100;
    if (r < 1) r = 1;
    return v > 0 ? (int)r : -(int)r;
}

// LOGICAL -> PHYSICAL for an extent starting at `origin`. USE THIS, not ui_px()
// on a width: it scales both EDGES and takes the difference, which is what
// keeps adjacent boxes sharing an edge exactly at 1.25x and 1.5x instead of
// overlapping or gapping by a pixel.
static inline int ui_span(int origin, int extent) {
    if (g_ui_scale_pct == 100 || extent == 0) return extent;
    int d = ui_px(origin + extent) - ui_px(origin);
    if (extent > 0 && d < 1) return 1;
    if (extent < 0 && d > -1) return -1;
    return d;
}

// PHYSICAL -> LOGICAL. The exact inverse of ui_px(): see the derivation in
// rustkern/uiscale.rs. This is the hit-testing direction wherever the
// compositor has to map a real mouse position back onto an unscaled model.
static inline int ui_unpx(int v) {
    int p = g_ui_scale_pct;
    if (p == 100 || v == 0) return v;
    long long a = v < 0 ? -(long long)v : (long long)v;
    int q = (int)((a * 100 + 49) / p);
    return v > 0 ? q : -q;
}

#endif // _LIBC_UISCALE_H
