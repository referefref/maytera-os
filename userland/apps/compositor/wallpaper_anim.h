// wallpaper_anim.h - animated, window-reactive wallpaper effects (#wpanim,
// owner request 2026-09-18). Four selectable physics-field backgrounds
// (PLASMA, MAGNETIC field lines, FLOW fluid, GRAVITY metaballs) that treat
// live compositor windows as obstacles: the field warps/repels (or, in
// "attract" mode, pulls toward) each window's rectangle.
//
// This module is PURE COMPUTE: it fills a caller-owned low-res ARGB buffer
// (reuse screensaver_gfx.c's ss_lores_buf()/ss_lores_upscale_to_fb_clipped()
// for the buffer + the upscale-to-framebuffer blit - do not duplicate those
// here). It never touches g_fb, never calls a syscall, and never blocks - the
// caller (wallpaper.c) is responsible for the uptime_ms()-gated recompute
// cadence (WPANIM_FRAME_MIN_MS) that keeps this off the compositor's
// never-block draw-thread budget (#426).
//
// Coordinate space: `buf`/`w`/`h` and `wins[].x/y/w/h` are ALL in the SAME
// space (the low-res cell grid the caller sized wins into, typically
// SS_LORES_LO_W x SS_LORES_LO_H) - the caller pre-scales window rects from
// framebuffer pixels into that space before calling. Distances/ranges below
// are expressed relative to `w`/`h`, so the look is resolution-independent
// regardless of which of the two standard ss_lores sizes is used.

#ifndef WALLPAPER_ANIM_H
#define WALLPAPER_ANIM_H

#include <stdint.h>
// Deliberately NOT including compositor.h's bool (typedef int bool) here -
// this header, like screensaver_gfx.h, has zero dependency on compositor.h
// so it can be included standalone. `focused`/`repel` are plain int (0/1)
// for the same reason.

// Effect selector values (also the UIPROFIL.YML "wallpaper_anim" int and the
// picker's radio-button order). 0 = off, so an old profile with no key at
// all (prof_apply default-inits to 0) safely means "static wallpaper",
// never "silently start animating".
#define WPANIM_OFF        0
#define WPANIM_PLASMA     1
#define WPANIM_MAGNETIC   2
#define WPANIM_FLOW       3
#define WPANIM_GRAVITY    4
// #emfield (owner request 2026-09-22): EM-field / plasma-swirl. Each window is
// an EM source (superposition, extends wp_mag_field); moving a window injects a
// velocity-driven vortex that swirls a persistent, advected dye buffer, and
// frosted glass shows the live swirl through it. Built on THIS CPU low-res
// engine, NOT TinyGL (TinyGL is a software triangle rasteriser, slower than
// this per-pixel path for a full-screen field - see docs/EMFIELD_WALLPAPER_PLAN.md).
#define WPANIM_EMFIELD    5
#define WPANIM_MODE_COUNT 6

// Colour source (#wpcolor, owner request 2026-09-18: "make sure we can
// adjust the colours"). Also the UIPROFIL.YML "wallpaper_anim_palette" int
// and the picker's Content/Spectrum/Mono button order. An out-of-range value
// (old profile, hand edit) is clamped to WP_PALETTE_SPECTRUM by the setter in
// wallpaper.c, never left to read garbage.
#define WP_PALETTE_CONTENT  0   // field colour driven by nearby windows' own content
#define WP_PALETTE_SPECTRUM 1   // rainbow, offset by the user's base hue (default)
#define WP_PALETTE_MONO     2   // single base hue, shaded by the field
#define WP_PALETTE_COUNT    3

// A live window, already translated into the low-res buffer's coordinate
// space (see file header). `focused` is currently unused by the effects
// (reserved: a later pass could make the focused window a stronger
// obstacle) but kept in the struct now so wallpaper.c does not need a
// second, effect-specific window array.
//
// `cr`/`cg`/`cb` (#wpcolor) are this window's own content colour, an average
// sampled by the CALLER (wallpaper.c) from the previous composited frame -
// see wallpaper.c's wp_sample_window_content() for why that is the cheap,
// correct source (the wallpaper draws before window contents each frame, so
// the framebuffer under a window's rect at the START of a tick still holds
// last frame's real pixels there). `has_content` is 0 only if the caller
// never sampled this slot (defensive; the caller always sets it today).
typedef struct {
    int32_t x, y, w, h;
    int     focused;   // 0/1
    uint8_t cr, cg, cb;
    int     has_content;
    // #emfield: per-window VELOCITY in THIS buffer's cell space per tick (the
    // same low-res coordinate space as x/y above). The kernel window info has
    // no velocity field (wm_window_info_t, libc/syscall.h), so the CALLER
    // (wallpaper.c) tracks previous position keyed by window id and fills
    // vx/vy here; a moving window then swirls the EMFIELD dye. The other four
    // effects ignore these; a caller that does not set them must zero them (a
    // still window = no vortex, the correct default).
    float   vx, vy;
} wp_win_rect_t;

// One-time LUT build (sin table, exp-falloff table). Idempotent, safe to
// call every frame (matches ss_gfx_init()'s contract) - only does real work
// once. wpanim_render() calls this itself, so callers do not need to.
void wpanim_init(void);

// Renders one animation tick of `mode` into `buf` (w*h ARGB cells, opaque).
// `wins`/`nwin` are the live windows (already visible+non-minimized
// filtered, already coordinate-translated - see file header). `time_ms` is
// uptime_ms(). `intensity` is 0..100 (UI slider range; clamped internally).
// `repel`: true = windows push the field away (default), false = attract.
// `hue_deg` (#wpcolor) is the user's base hue, 0..360 (wrapped internally).
// `palette` is one of the WP_PALETTE_* values above (clamped internally).
//
// Does nothing (leaves *buf untouched) if mode is out of range or buf/w/h
// are invalid - callers must not rely on this to clear anything.
void wpanim_render(int mode, uint32_t *buf, int w, int h,
                    const wp_win_rect_t *wins, int nwin,
                    uint64_t time_ms, int intensity, int repel,
                    float hue_deg, int palette);

// Human-readable label for the picker UI ("Off"/"Plasma"/...). Never NULL;
// an out-of-range mode returns "Off".
const char *wpanim_mode_name(int mode);

// Human-readable label for a WP_PALETTE_* value ("Content"/"Spectrum"/
// "Mono"). Never NULL; an out-of-range value returns "Spectrum" (the
// default), matching wpanim_render()'s own clamp.
const char *wpanim_palette_name(int palette);

// #wpcolor: a single saturated ARGB swatch for `hue_deg` (wrapped
// internally), for the picker's hue-slider preview - so the slider shows
// the actual colour it is choosing rather than a plain grey bar. Exposed
// here (rather than duplicated in wallpaper.c) so there is exactly one HSL
// formula in this module, per the "reuse the canonical primitive" rule.
uint32_t wpanim_hue_preview_color(float hue_deg);

#endif // WALLPAPER_ANIM_H
