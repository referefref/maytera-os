#ifndef _KERNEL_GUI_UISCALE_H
#define _KERNEL_GUI_UISCALE_H
// uiscale.h - the C face of THE global UI scale factor.
//
// The factor itself, all of its arithmetic and its property self-test live in
// rustkern/uiscale.rs. Read that file's header for the design: why the value is
// an integer percent, why it is NOT a theme key, what the rounding contract is,
// and exactly how far auto-detection can honestly go on hardware whose physical
// panel size we cannot read.
//
// THIS HEADER ADDS NO SECOND COPY OF ANYTHING. It is `extern` declarations and
// two one-line convenience wrappers. If you find yourself about to write
// `x * scale / 100` anywhere in the kernel, you want ui_px()/ui_span() instead:
// they are the same rounding as every other scaled pixel in the system, and
// "the same rounding as everything else" is the entire point.

#include "../types.h"

// The live factor, in percent. 100 == 1x.
extern int32_t uiscale_pct_rs(void);
// Where the live value came from: UI_SRC_* below.
extern int32_t uiscale_src_rs(void);
// Bumped on every accepted change. A consumer that caches derived geometry
// (a menu bar's row height, the compositor's chrome) compares this against
// what it last saw, with one load, and re-derives on a difference. This is how
// a scale change reaches cached state without anyone inventing a broadcast.
extern int32_t uiscale_gen_rs(void);
// Set the factor. Returns the value ACTUALLY ADOPTED after clamping, which may
// differ from what was asked. Report the return value, never the argument, or
// the UI will claim a scale that is not in force.
extern int32_t uiscale_set_pct_rs(int32_t pct, int32_t src);
// The largest factor this framebuffer can carry with the logical screen still
// at least 1024x600. A hard bound on every path, including a hand-edited
// config file.
extern int32_t uiscale_max_pct_rs(int32_t fb_w, int32_t fb_h);
// The auto-detected default. `laptop`: 1 yes, 0 no, -1 could not ask.
extern int32_t uiscale_auto_pct_rs(int32_t fb_w, int32_t fb_h, int32_t laptop);

// LOGICAL -> PHYSICAL, one coordinate or one standalone length.
extern int32_t uiscale_px_rs(int32_t v);
// LOGICAL -> PHYSICAL for an extent that starts at `origin`. USE THIS, not
// ui_px() on the width, whenever you have an (x,w) or (y,h) pair: it scales
// both EDGES and takes the difference, which is what keeps adjacent boxes
// sharing an edge exactly at 1.25x and 1.5x instead of overlapping or gapping
// by a pixel. See the rounding contract in rustkern/uiscale.rs.
extern int32_t uiscale_span_rs(int32_t origin, int32_t extent);
// PHYSICAL -> LOGICAL. The hit-testing direction, and the exact inverse of
// ui_px(): every physical pixel inside a logical pixel maps back to that
// logical pixel, with no gaps and no overlaps.
extern int32_t uiscale_unpx_rs(int32_t v);

// Scale-native processes: see rustkern/uiscale.rs. The compositor marks itself
// (SYS_UI_SCALE/UISC_NATIVE) and claiming the framebuffer marks it as a
// backstop; releasing the framebuffer clears it.
extern void    uiscale_mark_native_rs(int32_t pid);
extern void    uiscale_clear_native_rs(int32_t pid);
extern int32_t uiscale_is_native_rs(int32_t pid);

// 0 on pass; otherwise a bitmask naming WHICH property failed.
extern uint32_t uiscale_selftest_rs(void);

// Source codes, mirroring the SRC_* constants in rustkern/uiscale.rs.
#define UI_SRC_DEFAULT 0
#define UI_SRC_AUTO    1
#define UI_SRC_CONFIG  2
#define UI_SRC_USER    3
#define UI_SRC_ESP     4   // pinned by /UISCALE.TXT on the FAT boot partition

static inline int32_t ui_pct(void)                 { return uiscale_pct_rs(); }
static inline int32_t ui_px(int32_t v)             { return uiscale_px_rs(v); }
static inline int32_t ui_span(int32_t o, int32_t e){ return uiscale_span_rs(o, e); }
static inline int32_t ui_unpx(int32_t v)           { return uiscale_unpx_rs(v); }
static inline int     ui_scaled(void)              { return uiscale_pct_rs() != 100; }

// Boot-time bring-up: run the self-test, work out the default for this
// framebuffer, then let /CONFIG/DISPLAY.CFG override it. Called once from the
// GUI bring-up path, AFTER the framebuffer and the root filesystem are up.
void uiscale_init(int fb_w, int fb_h);

// Persist the current factor to /CONFIG/DISPLAY.CFG. Returns 0 on success.
int uiscale_save(void);

// Apply a new factor at runtime: adopt it, resize every scaled user window so
// its LOGICAL size is unchanged, and tell the apps to repaint. Returns the
// adopted percent.
int32_t uiscale_apply(int32_t pct, int32_t src);

// A one-line human description of where the live value came from, for the boot
// log and for Settings. Never NULL.
const char *uiscale_src_name(void);

// What auto-detection concluded for this display, independent of whatever the
// live value ended up being. Settings shows both, so a user can tell "the
// machine guessed this" from "I chose this" - which is the difference between
// a setting they trust and one they fight.
int32_t uiscale_auto_pct(void);
// The largest percent this framebuffer can carry with the logical screen still
// at least 1024x600. Settings uses it to stop offering steps that cannot draw.
int32_t uiscale_max_pct(void);
// 1 the firmware declares a battery, 0 it declares none, -1 we could not ask.
int32_t uiscale_is_laptop(void);

#endif // _KERNEL_GUI_UISCALE_H
