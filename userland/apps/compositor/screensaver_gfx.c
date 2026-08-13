// screensaver_gfx.c - shared low-res-buffer + upscale + bloom + palette
// pipeline for the psychedelic screensaver effects. See screensaver_gfx.h
// for the module contract and docs/SCREENSAVER_PSYCHEDELIC_DESIGN.md
// (sections 5, 6, 7) for the design this implements.
//
// THIS IS THE ONLY FILE IN THE COMPOSITOR'S SCREENSAVER CODE THAT MALLOCS.
// screensaver.c's own header comment commits it to "no malloc, all state
// static"; that convention is preserved by keeping every sizeable new buffer
// here instead. Buffers are malloc'd once, lazily, at first use.
//
// Fallback-on-malloc-failure: falls back to a same-size static array, not a
// reduced-resolution one, so every caller (which sizes its loops from the
// SS_LORES_HI_W/H etc. macros, not from a runtime-reported size) stays in
// bounds regardless of which path was taken. This is safe at this link base
// (user.ld base 0x80000000): wallpaper.c's g_file_buf is a 4MB static .bss
// array in this exact compositor binary today with no -mcmodel=large, so a
// same-size static fallback here (worst case ~800KB total across all the
// buffers below) is well inside that existing, already-shipping precedent.
// malloc essentially never fails in practice for a handful of sub-1MB
// requests made once after boot, so this path is a defensive last resort,
// not something expected to run.

#include "compositor.h"
#include "screensaver_gfx.h"
#include "../../libc/stdlib.h"   // malloc/free

// ============================================================================
// Palettes (design doc §7.1)
// ============================================================================

static uint32_t s_pal[SS_PAL_COUNT][256];
static int      s_pal_ready = 0;

uint8_t ss_palette_phase = 0;
static uint32_t s_phase_frame_ctr = 0;

void ss_palette_tick(void) {
    // +1 every 6 frames (~5x/sec at the 33ms compositor budget), wraps mod 256.
    if ((++s_phase_frame_ctr % 6) == 0) ss_palette_phase++;
}

// Same fully-saturated HSV wheel screensaver.c's ss_hue() already computes,
// duplicated here (not called across files) so this module has no dependency
// on screensaver.c's static functions. Kept verbatim as LUT #5 per §7.1.
static uint32_t ss_gfx_hue(int h) {
    h &= 0xFF;
    int region = h / 43;
    int rem = (h % 43) * 6;
    int v = 255, p = 0, q = 255 - rem, u = rem;
    int r, g, b;
    switch (region) {
        case 0:  r = v; g = u; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = u; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = u; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Linear-interpolate between two ARGB stops (t in 0..255).
static uint32_t ss_lerp_argb(uint32_t a, uint32_t b, int t) {
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    int r = ar + ((br - ar) * t) / 255;
    int g = ag + ((bg - ag) * t) / 255;
    int bch = ab + ((bb - ab) * t) / 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bch;
}

// Build one 256-entry LUT from a small list of stops evenly spaced over 0..255.
static void ss_build_gradient(uint32_t *lut, const uint32_t *stops, int nstops) {
    if (nstops < 2) { for (int i = 0; i < 256; i++) lut[i] = stops ? stops[0] : 0xFF000000u; return; }
    int seg = 255 / (nstops - 1);
    for (int i = 0; i < 256; i++) {
        int s = i / (seg ? seg : 1);
        if (s >= nstops - 1) s = nstops - 2;
        int base = s * seg;
        int span = seg ? seg : 1;
        int t = i - base;
        if (t < 0) t = 0;
        int tt = (t * 255) / span;
        if (tt > 255) tt = 255;
        lut[i] = ss_lerp_argb(stops[s], stops[s + 1], tt);
    }
}

void ss_gfx_init(void) {
    if (s_pal_ready) return;

    // 1. Acid: magenta -> orange -> yellow -> cyan -> magenta, velvet-black
    //    anchor at index 0 (the "hard dark" several effects rely on).
    {
        static const uint32_t stops[] = {
            0xFF000000u, 0xFFFF00C8u, 0xFFFF6A00u, 0xFFFFF000u, 0xFF00E8FFu, 0xFFFF00C8u
        };
        ss_build_gradient(s_pal[SS_PAL_ACID], stops, 6);
    }
    // 2. Ultraviolet: deep indigo -> violet -> hot pink -> white-hot core.
    {
        static const uint32_t stops[] = {
            0xFF0A0030u, 0xFF3A0080u, 0xFF9000C0u, 0xFFFF20A0u, 0xFFFFFFFFu
        };
        ss_build_gradient(s_pal[SS_PAL_ULTRAVIOLET], stops, 5);
    }
    // 3. Deep Sea Bloom: near-black -> teal -> emerald -> gold, darker range
    //    overall (good contrast for stained-glass ink edges).
    {
        static const uint32_t stops[] = {
            0xFF001018u, 0xFF004858u, 0xFF00A080u, 0xFF40D060u, 0xFFE0C040u
        };
        ss_build_gradient(s_pal[SS_PAL_DEEPSEA], stops, 5);
    }
    // 4. Molten: black -> deep red -> orange -> white.
    {
        static const uint32_t stops[] = {
            0xFF000000u, 0xFF400000u, 0xFFA01000u, 0xFFFF8000u, 0xFFFFFFE0u
        };
        ss_build_gradient(s_pal[SS_PAL_MOLTEN], stops, 5);
    }
    // 5. Spectrum: the existing full-saturation HSV wheel, kept verbatim.
    for (int i = 0; i < 256; i++) s_pal[SS_PAL_SPECTRUM][i] = ss_gfx_hue(i);

    s_pal_ready = 1;
}

uint32_t ss_pal(int pal, int idx) {
    if (!s_pal_ready) ss_gfx_init();
    if (pal < 0 || pal >= SS_PAL_COUNT) pal = SS_PAL_SPECTRUM;
    return s_pal[pal][idx & 0xFF];
}

// ============================================================================
// Buffers: low-res color buffer(s), bloom scratch, flame histogram.
// Malloc'd once at first use; same-size static fallback on failure (see file
// header for why that is safe here).
// ============================================================================

static uint32_t *s_hi_buf = 0;
static uint32_t *s_lo_buf = 0;
static uint32_t *s_scratch = 0;    // bloom scratch, sized to the larger (HI) buffer
static uint16_t *s_flame_density = 0;
static uint8_t  *s_flame_hue = 0;

static uint32_t s_fallback_hi[SS_LORES_HI_W * SS_LORES_HI_H];
static uint32_t s_fallback_lo[SS_LORES_LO_W * SS_LORES_LO_H];
static uint32_t s_fallback_scratch[SS_LORES_HI_W * SS_LORES_HI_H];
static uint16_t s_fallback_density[SS_FLAME_W * SS_FLAME_H];
static uint8_t  s_fallback_hue[SS_FLAME_W * SS_FLAME_H];

uint32_t *ss_lores_buf(int w, int h) {
    if (w == SS_LORES_HI_W && h == SS_LORES_HI_H) {
        if (!s_hi_buf) {
            s_hi_buf = (uint32_t *)malloc((unsigned long)(w * h) * sizeof(uint32_t));
            if (!s_hi_buf) s_hi_buf = s_fallback_hi;
        }
        return s_hi_buf;
    }
    if (w == SS_LORES_LO_W && h == SS_LORES_LO_H) {
        if (!s_lo_buf) {
            s_lo_buf = (uint32_t *)malloc((unsigned long)(w * h) * sizeof(uint32_t));
            if (!s_lo_buf) s_lo_buf = s_fallback_lo;
        }
        return s_lo_buf;
    }
    return (uint32_t *)0;   // only the two standard sizes are supported
}

// Bloom scratch buffer, sized to the larger of the two standard buffers
// (HI, 320x200). Internal to this file; ss_lores_bloom() is the only caller.
static uint32_t *ss_gfx_scratch(void) {
    if (!s_scratch) {
        s_scratch = (uint32_t *)malloc((unsigned long)(SS_LORES_HI_W * SS_LORES_HI_H) * sizeof(uint32_t));
        if (!s_scratch) s_scratch = s_fallback_scratch;
    }
    return s_scratch;
}

int ss_flame_hist(uint16_t **density, uint8_t **hue) {
    if (!s_flame_density) {
        s_flame_density = (uint16_t *)malloc((unsigned long)(SS_FLAME_W * SS_FLAME_H) * sizeof(uint16_t));
        if (!s_flame_density) s_flame_density = s_fallback_density;
    }
    if (!s_flame_hue) {
        s_flame_hue = (uint8_t *)malloc((unsigned long)(SS_FLAME_W * SS_FLAME_H) * sizeof(uint8_t));
        if (!s_flame_hue) s_flame_hue = s_fallback_hue;
    }
    if (!s_flame_density || !s_flame_hue) return 0;
    *density = s_flame_density;
    *hue = s_flame_hue;
    return 1;
}

void ss_lores_clear(uint32_t *buf, int w, int h, uint32_t argb) {
    if (!buf || w <= 0 || h <= 0) return;
    int32_t n = w * h;
    for (int32_t i = 0; i < n; i++) buf[i] = argb;
}

// ============================================================================
// isqrt: bit-by-bit integer square root, ~16 iterations worst case, no
// float/div-library dependency (matches screensaver.c's fixed-point idiom).
// ============================================================================
int ss_isqrt(uint32_t v) {
    uint32_t res = 0;
    uint32_t bit = 1u << 30;   // highest power-of-4 <= largest possible input
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= res + bit) {
            v -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (int)res;
}

// ============================================================================
// Shared bloom (design doc §6): separable box blur via sliding running-sum,
// then additive composite back into the same buffer.
// ============================================================================

// Box-blur one row/column of length n with the given radius. src and dst
// MUST be distinct buffers (the running-sum recurrence reads src[x-radius],
// which for an in-place call would already have been overwritten by an
// earlier dst[] write - this is why the vertical pass below copies each
// column out to a small local buffer first rather than blurring in place).
static void ss_hblur(const uint32_t *src, uint32_t *dst, int n, int radius) {
    if (n <= 0) return;
    int32_t rs = 0, gs = 0, bs = 0, cnt = 0;
    int32_t init_end = radius;
    if (init_end >= n) init_end = n - 1;
    for (int32_t x = 0; x <= init_end; x++) {
        uint32_t c = src[x];
        rs += (int32_t)((c >> 16) & 0xFF);
        gs += (int32_t)((c >> 8) & 0xFF);
        bs += (int32_t)(c & 0xFF);
        cnt++;
    }
    for (int32_t x = 0; x < n; x++) {
        int32_t c1 = (cnt > 0) ? cnt : 1;
        dst[x] = 0xFF000000u | (((uint32_t)(rs / c1)) << 16) |
                 (((uint32_t)(gs / c1)) << 8) | (uint32_t)(bs / c1);
        int32_t xin = x + radius + 1;
        int32_t xout = x - radius;
        if (xin < n) {
            uint32_t c = src[xin];
            rs += (int32_t)((c >> 16) & 0xFF);
            gs += (int32_t)((c >> 8) & 0xFF);
            bs += (int32_t)(c & 0xFF);
            cnt++;
        }
        if (xout >= 0) {
            uint32_t c = src[xout];
            rs -= (int32_t)((c >> 16) & 0xFF);
            gs -= (int32_t)((c >> 8) & 0xFF);
            bs -= (int32_t)(c & 0xFF);
            cnt--;
        }
    }
}

void ss_lores_bloom(uint32_t *buf, int w, int h, int radius, int intensity_pct) {
    if (!buf || w <= 0 || h <= 0 || radius <= 0 || intensity_pct <= 0) return;
    if (w > SS_LORES_HI_W || h > SS_LORES_HI_H) return;   // only the two standard sizes fit the scratch

    uint32_t *tmp = ss_gfx_scratch();
    if (!tmp) return;   // never crash: just skip bloom, the sharp image still renders

    // Horizontal pass: buf -> tmp.
    for (int y = 0; y < h; y++) {
        ss_hblur(&buf[y * w], &tmp[y * w], w, radius);
    }

    // Vertical pass: tmp -> tmp, via small per-column local buffers (h is at
    // most SS_LORES_HI_H = 200, so these are tiny, safe stack arrays).
    {
        uint32_t colin[SS_LORES_HI_H];
        uint32_t colout[SS_LORES_HI_H];
        for (int x = 0; x < w; x++) {
            for (int y = 0; y < h; y++) colin[y] = tmp[y * w + x];
            ss_hblur(colin, colout, h, radius);
            for (int y = 0; y < h; y++) tmp[y * w + x] = colout[y];
        }
    }

    // Additive composite: final = min(255, sharp + blurred*intensity/100).
    int32_t n = w * h;
    for (int32_t i = 0; i < n; i++) {
        uint32_t s = buf[i], b = tmp[i];
        int32_t sr = (int32_t)((s >> 16) & 0xFF), sg = (int32_t)((s >> 8) & 0xFF), sb = (int32_t)(s & 0xFF);
        int32_t br = (int32_t)((b >> 16) & 0xFF), bg = (int32_t)((b >> 8) & 0xFF), bb = (int32_t)(b & 0xFF);
        int32_t r = sr + (br * intensity_pct) / 100; if (r > 255) r = 255;
        int32_t g = sg + (bg * intensity_pct) / 100; if (g > 255) g = 255;
        int32_t bl = sb + (bb * intensity_pct) / 100; if (bl > 255) bl = 255;
        buf[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
    }
}

// ============================================================================
// Upscale: general bilinear with an integer fast path (design doc §5).
// ============================================================================

static inline uint32_t ss_bilerp(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11,
                                  int32_t wx, int32_t wy) {
    int32_t r00 = (int32_t)((c00 >> 16) & 0xFF), g00 = (int32_t)((c00 >> 8) & 0xFF), b00 = (int32_t)(c00 & 0xFF);
    int32_t r10 = (int32_t)((c10 >> 16) & 0xFF), g10 = (int32_t)((c10 >> 8) & 0xFF), b10 = (int32_t)(c10 & 0xFF);
    int32_t r01 = (int32_t)((c01 >> 16) & 0xFF), g01 = (int32_t)((c01 >> 8) & 0xFF), b01 = (int32_t)(c01 & 0xFF);
    int32_t r11 = (int32_t)((c11 >> 16) & 0xFF), g11 = (int32_t)((c11 >> 8) & 0xFF), b11 = (int32_t)(c11 & 0xFF);
    int32_t rt = r00 + (((r10 - r00) * wx) >> 8);
    int32_t gt = g00 + (((g10 - g00) * wx) >> 8);
    int32_t bt = b00 + (((b10 - b00) * wx) >> 8);
    int32_t rb = r01 + (((r11 - r01) * wx) >> 8);
    int32_t gb = g01 + (((g11 - g01) * wx) >> 8);
    int32_t bb = b01 + (((b11 - b01) * wx) >> 8);
    int32_t r = rt + (((rb - rt) * wy) >> 8);
    int32_t g = gt + (((gb - gt) * wy) >> 8);
    int32_t b = bt + (((bb - bt) * wy) >> 8);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// ---------------------------------------------------------------------------
// #650: the per-output-pixel divides are GONE, and the two paths are now one.
//
// WHAT WAS WRONG. Both upscale paths divided ONCE PER OUTPUT PIXEL, and the
// general path (the one 1920x1080 actually took) did a 64-bit multiply AND a
// 64-bit divide per pixel:
//
//     fast path:    sx0 = dx / fx;   wx = wtab[dx % fx];
//     general path: sxf = ((int64_t)dx * sxnum * 256) / dxm1;
//
// At 1920x1080 that is 2,073,600 64-bit divides PER FRAME. At ~25-40 cycles
// each it is ~60-80 Mcycle/frame, which accounts for essentially the whole
// measured cost.
//
// MEASURED before this change (throwaway VM 2630, golden-998 kernel, plasma
// saver, serial logging silenced over the timed region):
//     1280x800   18.0 fps   COMPOSIT 34%   18.4 ms-core per Mpx
//     1920x1080  11.5 fps   COMPOSIT 58%   24.3 ms-core per Mpx
// The per-pixel cost RISING with resolution is the second bug below.
//
// THE FIX. The column mapping does not depend on the row: dx -> (sx0, wx) is
// identical for every one of the dh rows, so it is a loop-invariant the
// compiler cannot hoist for itself (fx and dxm1 are runtime values, so it can
// neither strength-reduce the divide nor prove the expression row-invariant).
// It is now computed ONCE per frame into a table of dw entries and then read.
// That turns dw*dh divides per frame into dw: at 1080p, 2,073,600 -> 1,920.
//
// SECOND BUG, now fixed by construction: the integer "fast" path required
// dw%w==0 AND dh%h==0 AND fx==fy. At 1920x1080 against the 320x200 HI buffer,
// 1920/320 = 6 but 1080/200 = 5.4, so fy was 0 and 1080p silently fell through
// to the SLOWER general path. That is why cost per pixel rose from 18.4 to
// 24.3 ms-core/Mpx between the two resolutions: the bigger mode was also
// taking the more expensive code path.
//
// Rather than widen the qualification test, the special case is DELETED. With
// the column table there is no longer any performance reason to keep a second
// path, and one path cannot silently mis-qualify. This is the "improve the
// shared primitive, do not fork a private copy" rule applied to this file.
//
// HONEST NOTE ON OUTPUT. This is NOT bit-identical at 1280x800. The deleted
// fast path sampled dx -> dx/fx with weight (dx%fx)*256/fx (block-relative,
// with the final block extrapolating past w-1 and clamping); the surviving
// general path samples edge-to-edge, dx -> dx*(w-1)/(dw-1), which is the
// standard bilinear mapping and the more correct of the two. For a plasma the
// difference is imperceptible, but it IS a change and is recorded as one.
// ---------------------------------------------------------------------------

// Per-column source mapping, packed as (sx0 << 8) | wx. Sized for any sane
// framebuffer width; a wider mode than this falls back to computing the
// mapping inline (still correct, just without the hoist).
#define SS_XTAB_MAX 4096
static int32_t ss_xtab[SS_XTAB_MAX];
static int32_t ss_xtab_dw = -1;   // dw the table was built for
static int32_t ss_xtab_w  = -1;   // source w the table was built for

void ss_lores_upscale_to_fb(const uint32_t *buf, int w, int h) {
    if (!buf || w <= 0 || h <= 0 || !g_fb) return;
    int32_t dw = g_fb_width, dh = g_fb_height;
    if (dw <= 0 || dh <= 0) return;

    int32_t dxm1  = (dw > 1) ? (dw - 1) : 1;
    int32_t dym1  = (dh > 1) ? (dh - 1) : 1;
    int32_t sxnum = (w > 1) ? (w - 1) : 0;
    int32_t synum = (h > 1) ? (h - 1) : 0;

    // Build the per-column table once per (dw, w) pair. The screensaver holds
    // both fixed for its whole run, so in practice this runs on the first
    // frame only and every later frame is pure table reads.
    const int use_tab = (dw <= SS_XTAB_MAX);
    if (use_tab && (ss_xtab_dw != dw || ss_xtab_w != w)) {
        for (int32_t dx = 0; dx < dw; dx++) {
            int32_t sxf = (w > 1) ? (int32_t)(((int64_t)dx * sxnum * 256) / dxm1) : 0;
            int32_t sx0 = sxf >> 8;
            if (sx0 >= w) sx0 = w - 1;
            ss_xtab[dx] = (sx0 << 8) | (sxf & 0xFF);
        }
        ss_xtab_dw = dw;
        ss_xtab_w  = w;
    }

    for (int32_t dy = 0; dy < dh; dy++) {
        // One divide per ROW (dh per frame), not per pixel.
        int32_t syf = (h > 1) ? (int32_t)(((int64_t)dy * synum * 256) / dym1) : 0;
        int32_t sy0 = syf >> 8, wy = syf & 0xFF;
        if (sy0 >= h) sy0 = h - 1;
        int32_t sy1 = sy0 + 1; if (sy1 >= h) sy1 = h - 1;
        const uint32_t *row0 = buf + (int32_t)sy0 * w;
        const uint32_t *row1 = buf + (int32_t)sy1 * w;
        uint32_t *drow = &g_fb[dy * g_fb_pitch];

        if (use_tab) {
            for (int32_t dx = 0; dx < dw; dx++) {
                int32_t e   = ss_xtab[dx];
                int32_t sx0 = e >> 8, wx = e & 0xFF;
                int32_t sx1 = sx0 + 1; if (sx1 >= w) sx1 = w - 1;
                drow[dx] = ss_bilerp(row0[sx0], row0[sx1], row1[sx0], row1[sx1], wx, wy);
            }
        } else {
            for (int32_t dx = 0; dx < dw; dx++) {
                int32_t sxf = (w > 1) ? (int32_t)(((int64_t)dx * sxnum * 256) / dxm1) : 0;
                int32_t sx0 = sxf >> 8, wx = sxf & 0xFF;
                if (sx0 >= w) sx0 = w - 1;
                int32_t sx1 = sx0 + 1; if (sx1 >= w) sx1 = w - 1;
                drow[dx] = ss_bilerp(row0[sx0], row0[sx1], row1[sx0], row1[sx1], wx, wy);
            }
        }
    }
}
