// screensaver_gfx.h - shared low-res-buffer + upscale + bloom + palette
// pipeline for the psychedelic screensaver effects (docs/
// SCREENSAVER_PSYCHEDELIC_DESIGN.md, sections 5, 6, 7).
//
// This is a NEW file, deliberately split out of screensaver.c: it is the
// only place in the compositor's screensaver code that mallocs (see the
// big buffer comment above the static arrays in screensaver_gfx.c).
// screensaver.c itself stays "no malloc, all state static" as documented in
// its own header comment; the three psychedelic effects call into this
// module for anything that needs a sizeable buffer, and keep their own
// small per-effect state (feature points, chaos-game position, ...) as
// ordinary function-local statics the same way SS_MATRIX already does.
//
// Fixed-point / direct-pixel only, matching screensaver.c's existing idiom
// (SS_SIN/SS_COS, ss_hue, ss_rand). No libgl/TinyGL, no libm.

#ifndef SCREENSAVER_GFX_H
#define SCREENSAVER_GFX_H

#include <stdint.h>

// ----------------------------------------------------------------------------
// Two standard internal low-res sizes (design doc §5). "HI" is for effects
// whose per-cell cost is cheap (table lookups only): the flame tone-map and
// Plasma Reborn both use this. "LO" is for effects with a heavier per-cell
// inner loop (Stained-Glass Warp's N-point Voronoi scan).
// ----------------------------------------------------------------------------
#define SS_LORES_HI_W 320
#define SS_LORES_HI_H 200
#define SS_LORES_LO_W 200
#define SS_LORES_LO_H 125

// The fractal flame's histogram is the same footprint as the HI buffer
// (320x200) so its tone-mapped output can be written straight into the HI
// color buffer without a second resolution class.
#define SS_FLAME_W SS_LORES_HI_W
#define SS_FLAME_H SS_LORES_HI_H

// ----------------------------------------------------------------------------
// Palettes (design doc §7.1): five hand-tuned 256-entry ARGB LUTs, built once.
// ----------------------------------------------------------------------------
#define SS_PAL_ACID        0   // magenta -> orange -> yellow -> cyan -> magenta
#define SS_PAL_ULTRAVIOLET 1   // indigo -> violet -> hot pink -> white-hot core
#define SS_PAL_DEEPSEA     2   // teal -> emerald -> gold
#define SS_PAL_MOLTEN      3   // black -> deep red -> orange -> white
#define SS_PAL_SPECTRUM    4   // full-saturation HSV wheel (ss_hue, kept verbatim)
#define SS_PAL_COUNT       5

// Builds the 5 palette LUTs. Idempotent (safe to call every time
// screensaver_init() runs, i.e. on every effect switch) - only does real
// work once. Call before any ss_pal()/ss_lores_*() use.
void ss_gfx_init(void);

// idx is masked to 0..255 internally, so callers never need to clamp.
uint32_t ss_pal(int pal, int idx);

// Shared palette-cycling phase (design doc §7.2): one shared mechanism, opt
// in per effect by adding ss_palette_phase to your LUT index. Advance it
// (slowly - every few frames) by calling ss_palette_tick() once per frame
// from an effect that wants cycling.
extern uint8_t ss_palette_phase;
void ss_palette_tick(void);

// ----------------------------------------------------------------------------
// Low-res color buffer(s). ARGB, same format as g_fb. Malloc'd once at first
// use (see screensaver_gfx.c for the fallback-on-failure story); only the two
// standard sizes above are supported (anything else returns NULL, and the
// caller must treat that as "skip this frame", never dereference it).
// ----------------------------------------------------------------------------
uint32_t *ss_lores_buf(int w, int h);
void ss_lores_clear(uint32_t *buf, int w, int h, uint32_t argb);

// Separable box blur (sliding running-sum, O(w*h) not O(w*h*radius)) then
// additive re-composite: final = min(255, sharp + blurred*intensity_pct/100).
// Operates on `buf` in place. radius in low-res cells (2-4 is typical);
// intensity_pct is the blend strength (design doc §6).
void ss_lores_bloom(uint32_t *buf, int w, int h, int radius, int intensity_pct);

// Smoothly upscales `buf` (w x h) into g_fb (g_fb_width x g_fb_height).
// General bilinear, correct for any destination size; takes an integer
// fast path when the scale factor is a clean small integer in both axes
// (the 1280x800 = 4x case for the HI buffer).
void ss_lores_upscale_to_fb(const uint32_t *buf, int w, int h);

// ----------------------------------------------------------------------------
// Fractal flame histogram (design doc §4.1). Fixed 320x200 (SS_FLAME_W x
// SS_FLAME_H). Malloc'd once here (never in screensaver.c). Returns 1 and
// fills *density/*hue on success (including the static-fallback path, which
// is still safe to read/write - see screensaver_gfx.c), 0 if truly
// unavailable (caller should skip the effect for this frame rather than
// dereference NULL).
int ss_flame_hist(uint16_t **density, uint8_t **hue);

// ----------------------------------------------------------------------------
// Small fixed-point helper(s) shared by the direct-pixel effects.
// ----------------------------------------------------------------------------
// Integer square root (bit-by-bit algorithm, ~16 iterations, no float/div
// library call). Used for Plasma Reborn's radial term and Stained-Glass
// Warp's Voronoi distances, and for the flame's density->brightness gamma.
int ss_isqrt(uint32_t v);

#endif // SCREENSAVER_GFX_H
