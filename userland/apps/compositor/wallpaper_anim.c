// wallpaper_anim.c - the four animated, window-reactive wallpaper effects
// (#wpanim, owner request 2026-09-18). See wallpaper_anim.h for the module
// contract and coordinate-space convention.
//
// PERFORMANCE SHAPE (why each effect is written the way it is):
//   - PLASMA fills every cell of the low-res buffer (it IS a field), so its
//     per-cell inner loop avoids libm transcendentals in the hot path: a
//     256-entry sine LUT (built once, see wp_sin()) and a clamped
//     exp(-x)-falloff LUT (wp_expfall()) replace sinf()/expf(). sqrtf() is
//     kept (single SQRTSS instruction on this hardware-SSE2 userland, see
//     libc/math.c) rather than approximated.
//   - MAGNETIC/FLOW/GRAVITY are PARTICLE systems (a few hundred points, not
//     one term per cell), so their cost is O(particles * windows) regardless
//     of buffer resolution - cheap by construction, drawn with a fading
//     trail so the low-res buffer does not need clearing every tick.
//
// This file never calls a syscall and never blocks (#426): all state is
// static arrays, no malloc. The caller (wallpaper.c) owns the
// uptime_ms()-gated recompute cadence.

#include "compositor.h"          // bool/true/false, NULL (freestanding shim)
#include "wallpaper_anim.h"
#include "../../libc/math.h"

// ============================================================================
// Shared small helpers: sine LUT, exp-falloff LUT, tiny PRNG, pixel plot.
// ============================================================================

#define WP_TWO_PI 6.28318530717958647692f

static float s_wp_sin[256];
static int   s_wp_lut_ready = 0;

// exp(-x) for x in [0, WP_EXPFALL_MAX), clamped to 0 beyond (negligible
// influence anyway - this IS the intended cull, not an approximation error
// that matters visually).
#define WP_EXPFALL_N   256
#define WP_EXPFALL_MAX 8.0f
static float s_wp_expfall[WP_EXPFALL_N];

void wpanim_init(void)
{
    if (s_wp_lut_ready) return;
    for (int i = 0; i < 256; i++)
        s_wp_sin[i] = sinf((float)i * (WP_TWO_PI / 256.0f));
    for (int i = 0; i < WP_EXPFALL_N; i++) {
        float x = (float)i * (WP_EXPFALL_MAX / (float)WP_EXPFALL_N);
        s_wp_expfall[i] = expf(-x);
    }
    s_wp_lut_ready = 1;
}

// angle in radians -> LUT lookup, O(1), no libm call.
static inline float wp_sin(float angle)
{
    float a = angle * (256.0f / WP_TWO_PI);
    int idx = (int)a;
    idx &= 255;   // relies on 2's-complement wraparound for negative a; masking
                  // a negative int with 0xFF still yields 0..255 on this target.
    return s_wp_sin[idx];
}
static inline float wp_cos(float angle) { return wp_sin(angle + (WP_TWO_PI * 0.25f)); }

// exp(-x) for x >= 0 via the clamped LUT above.
static inline float wp_expfall(float x)
{
    if (x < 0.0f) x = 0.0f;
    if (x >= WP_EXPFALL_MAX) return 0.0f;
    int idx = (int)(x * ((float)WP_EXPFALL_N / WP_EXPFALL_MAX));
    if (idx < 0) idx = 0;
    if (idx >= WP_EXPFALL_N) idx = WP_EXPFALL_N - 1;
    return s_wp_expfall[idx];
}

// Tiny xorshift32 PRNG, local to this file (screensaver.c's ss_rand() is
// static to that file, not exported - see wallpaper_anim.h's header note on
// having zero cross-module dependency). Seeded once from the first non-zero
// time_ms this file sees, so particle layout differs run to run without
// needing a real entropy source (cosmetic only, not security-relevant).
static uint32_t s_wp_rng = 2463534242u;
static inline uint32_t wp_rand_u32(void)
{
    uint32_t x = s_wp_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_wp_rng = x;
    return x;
}
static inline float wp_randf(void) { return (float)(wp_rand_u32() & 0xFFFFFF) / (float)0x1000000; }

static inline void wp_plot(uint32_t *buf, int w, int h, int x, int y, uint32_t argb)
{
    if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
    buf[y * w + x] = argb;
}

// Additive-blend plot (for particle glow / trails): src channels added,
// clamped to 255, alpha forced opaque (these buffers are always opaque ARGB,
// same convention as screensaver_gfx.c).
static inline void wp_plot_add(uint32_t *buf, int w, int h, int x, int y,
                                int32_t ar, int32_t ag, int32_t ab)
{
    if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
    uint32_t c = buf[y * w + x];
    int32_t r = (int32_t)((c >> 16) & 0xFF) + ar; if (r > 255) r = 255;
    int32_t g = (int32_t)((c >> 8)  & 0xFF) + ag; if (g > 255) g = 255;
    int32_t b = (int32_t)(c & 0xFF)         + ab; if (b > 255) b = 255;
    buf[y * w + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Multiply every channel by `num/den` (den>0), for trail fade. Alpha forced
// opaque.
static void wp_fade(uint32_t *buf, int w, int h, int num, int den)
{
    int32_t n = w * h;
    for (int32_t i = 0; i < n; i++) {
        uint32_t c = buf[i];
        int32_t r = (int32_t)((c >> 16) & 0xFF) * num / den;
        int32_t g = (int32_t)((c >> 8)  & 0xFF) * num / den;
        int32_t b = (int32_t)(c & 0xFF)         * num / den;
        buf[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

// HSL (h in degrees, any range - wrapped internally; s,l in 0..1) -> ARGB.
// Standard formula; called once per PLASMA cell so kept simple, not LUT'd.
static uint32_t wp_hsl(float h, float s, float l)
{
    h = h - 360.0f * floorf(h / 360.0f);   // wrap into [0,360)
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float r1, g1, b1;
    if      (hp < 1.0f) { r1 = c; g1 = x; b1 = 0; }
    else if (hp < 2.0f) { r1 = x; g1 = c; b1 = 0; }
    else if (hp < 3.0f) { r1 = 0; g1 = c; b1 = x; }
    else if (hp < 4.0f) { r1 = 0; g1 = x; b1 = c; }
    else if (hp < 5.0f) { r1 = x; g1 = 0; b1 = c; }
    else                { r1 = c; g1 = 0; b1 = x; }
    float m = l - c * 0.5f;
    int r = (int)((r1 + m) * 255.0f); if (r < 0) r = 0; if (r > 255) r = 255;
    int g = (int)((g1 + m) * 255.0f); if (g < 0) g = 0; if (g > 255) g = 255;
    int b = (int)((b1 + m) * 255.0f); if (b < 0) b = 0; if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// ============================================================================
// #wpcolor (owner request 2026-09-18): shared colour-source helpers used by
// every effect below, so the four effects behave identically for "base hue"
// and "Content/Spectrum/Mono" rather than each inventing its own rule.
// ============================================================================

// Sample the windows' own content colour near (px,py) (same coordinate
// space as everything else in this file - the low-res buffer). Distance
// falloff is the SAME shape (wp_expfall over a per-window range) already
// used elsewhere in this file for position influence, so "how strongly a
// window's content tints the field nearby" tracks "how strongly it warps/
// repels the field nearby" - one falloff idiom, not two. Cheap by
// construction: nw is capped at 16 by every caller, and the cutoff test
// skips the sqrt/expfall for cells/particles far from a given window.
static void wp_content_sample(float px, float py, const wp_win_rect_t *wins, int nw,
                               float base, float *cr, float *cg, float *cb, float *cw)
{
    for (int i = 0; i < nw; i++) {
        if (!wins[i].has_content) continue;
        float cx = (float)wins[i].x + (float)wins[i].w * 0.5f;
        float cy = (float)wins[i].y + (float)wins[i].h * 0.5f;
        float hw = (float)wins[i].w * 0.5f, hh = (float)wins[i].h * 0.5f;
        float dx = px - cx, dy = py - cy;
        float d2 = dx * dx + dy * dy;
        float range = (hw + hh) * 1.2f + base;
        float cutoff = range * WP_EXPFALL_MAX;
        if (d2 > cutoff * cutoff) continue;
        float d = sqrtf(d2);
        float w8 = wp_expfall(d / range);
        *cr += (float)wins[i].cr * w8;
        *cg += (float)wins[i].cg * w8;
        *cb += (float)wins[i].cb * w8;
        *cw += w8;
    }
}

// SPECTRUM keeps each effect's own dynamic hue progression (the motion
// that already existed) and simply offsets it by the user's base hue, so
// turning the hue slider recolors the same animation rather than replacing
// it. MONO/CONTENT use a fixed structural hue (the base hue itself); MONO's
// "shaded by the field" comes from the caller varying saturation/lightness
// with its own field value, CONTENT's colour comes from wp_apply_content()
// below (this fixed hue is just its fallback when no window is nearby).
static inline float wp_resolve_hue(float dynamic_hue, float hue_base, int palette)
{
    return (palette == WP_PALETTE_SPECTRUM) ? (hue_base + dynamic_hue) : hue_base;
}

// Blend `structural` (the colour the effect would have drawn anyway) toward
// the accumulated window-content colour, by the accumulated weight (capped
// at 1). CONTENT-only: SPECTRUM/MONO never look at window content, exactly
// the three-way split the picker offers. A weight <= 0.05 (no window close
// enough to matter) falls back to `structural` untouched, so CONTENT still
// looks reasonable over open desktop rather than reading as black/undefined.
static inline uint32_t wp_apply_content(uint32_t structural, int palette,
                                         float cr, float cg, float cb, float cw)
{
    if (palette != WP_PALETTE_CONTENT || cw <= 0.05f) return structural;
    if (cw > 1.0f) cw = 1.0f;
    int32_t sr = (int32_t)((structural >> 16) & 0xFF);
    int32_t sg = (int32_t)((structural >> 8)  & 0xFF);
    int32_t sb = (int32_t)(structural & 0xFF);
    int32_t r = (int32_t)((float)sr * (1.0f - cw) + (cr / cw) * cw);
    int32_t g = (int32_t)((float)sg * (1.0f - cw) + (cg / cw) * cw);
    int32_t b = (int32_t)((float)sb * (1.0f - cw) + (cb / cw) * cw);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Resolution-independent baseline additive term. The approved prototype's
// formulas were tuned against its own canvas and used a literal "+40" /
// "+30" / "+120" style constant; this buffer can be either of the two
// standard ss_lores sizes (200x125 or 320x200), so those constants are
// expressed relative to buffer width instead of hardcoded - at w=200 (the
// size wallpaper.c actually selects for these effects) WP_BASE(w) == 40,
// reproducing the prototype's own number exactly; at other sizes the look
// stays proportionally the same rather than the falloff radius silently
// becoming tiny or huge relative to the buffer.
#define WP_BASE(w) (0.20f * (float)(w))

// ============================================================================
// PLASMA - multi-sine field, domain-warped around windows (#wpanim default).
// ============================================================================

static void wpanim_plasma(uint32_t *buf, int w, int h,
                           const wp_win_rect_t *wins, int nwin,
                           uint64_t time_ms, int intensity, int repel,
                           float hue_deg, int palette)
{
    float t    = (float)time_ms * 0.001f;
    float I01  = ((float)intensity / 100.0f) * 2.0f;         // 0..2
    float sc   = 0.045f + 0.02f * I01;
    float sgn  = repel ? 1.0f : -1.0f;
    float base = WP_BASE(w);
    float infl_amp = 28.0f + 40.0f * I01;
    float ccx = (float)w * 0.5f, ccy = (float)h * 0.5f;

    // Precompute per-window terms that do not depend on the cell.
    float wcx[16], wcy[16], wrange[16];
    int   nw = nwin > 16 ? 16 : nwin;
    for (int i = 0; i < nw; i++) {
        wcx[i] = (float)wins[i].x + (float)wins[i].w * 0.5f;
        wcy[i] = (float)wins[i].y + (float)wins[i].h * 0.5f;
        wrange[i] = ((float)wins[i].w + (float)wins[i].h) * 1.5f + base;
    }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float sx = (float)x, sy = (float)y;
            for (int i = 0; i < nw; i++) {
                float dx = sx - wcx[i], dy = sy - wcy[i];
                float dist2 = dx * dx + dy * dy;
                float range = wrange[i];
                // Cheap cull: beyond ~WP_EXPFALL_MAX*range the term is
                // indistinguishable from zero - skip the sqrt/lut entirely.
                float cutoff = range * WP_EXPFALL_MAX;
                if (dist2 > cutoff * cutoff) continue;
                float dist = sqrtf(dist2) + 1.0f;
                float infl = wp_expfall(dist / range) * sgn * infl_amp;
                sx += dx / dist * infl;
                sy += dy / dist * infl;
            }
            float rdx = sx - ccx, rdy = sy - ccy;
            float radial = sqrtf(rdx * rdx + rdy * rdy);
            float v = wp_sin(sx * sc + t)
                    + wp_sin(sy * sc * 0.9f - 0.8f * t)
                    + wp_sin((sx + sy) * sc * 0.6f + 0.5f * t)
                    + wp_sin(radial * sc * 0.8f - 1.2f * t);
            v *= 0.25f;   // -1..1
            float v01 = v * 0.5f + 0.5f;   // 0..1
            float dyn_hue = v01 * 300.0f + t * 8.0f;
            float hue = wp_resolve_hue(dyn_hue, hue_deg, palette);
            float sat   = (palette == WP_PALETTE_MONO) ? 0.55f : 0.85f;
            float light = (palette == WP_PALETTE_MONO) ? (0.25f + 0.45f * v01) : 0.50f;
            uint32_t structural = wp_hsl(hue, sat, light);
            // #wpcolor content influence: sample at the REAL (unwarped) cell
            // position, not sx/sy (which the per-window loop above already
            // displaced for the field's own domain-warp look) - the tint
            // must sit where the window actually is on screen.
            float ccr = 0.0f, ccg = 0.0f, ccb = 0.0f, ccw = 0.0f;
            wp_content_sample((float)x, (float)y, wins, nw, base, &ccr, &ccg, &ccb, &ccw);
            buf[y * w + x] = wp_apply_content(structural, palette, ccr, ccg, ccb, ccw);
        }
    }
}

// ============================================================================
// MAGNETIC - each window is a dipole; particles advect along field lines.
// ============================================================================

#define WP_MAG_PARTICLES 220

typedef struct { float x, y; int life; } wp_particle_t;
static wp_particle_t s_mag_p[WP_MAG_PARTICLES];
static int s_mag_ready = 0;

static void wp_mag_respawn(wp_particle_t *p, int w, int h)
{
    p->x = wp_randf() * (float)w;
    p->y = wp_randf() * (float)h;
    p->life = 60 + (int)(wp_randf() * 120.0f);
}

// Field at point (px,py) from every window's dipole (+ pole offset +0.8*hw,
// - pole offset -0.8*hw, both on the window's own y-center). Returns
// (bx,by) and, via return value, |B| (for color).
static float wp_mag_field(float px, float py, const wp_win_rect_t *wins, int nw,
                           float sgn, float *bx_out, float *by_out)
{
    float bx = 0.0f, by = 0.0f;
    for (int i = 0; i < nw; i++) {
        float cx = (float)wins[i].x + (float)wins[i].w * 0.5f;
        float cy = (float)wins[i].y + (float)wins[i].h * 0.5f;
        float hw = (float)wins[i].w * 0.5f, hh = (float)wins[i].h * 0.5f;
        float q  = (hw + hh) * 0.9f * sgn;
        float ppx = cx + hw * 0.8f, ppy = cy;   // + pole
        float npx = cx - hw * 0.8f, npy = cy;   // - pole
        float dpx = px - ppx, dpy = py - ppy;
        float pd2 = dpx * dpx + dpy * dpy + 40.0f;
        float dnx = px - npx, dny = py - npy;
        float nd2 = dnx * dnx + dny * dny + 40.0f;
        bx += q * dpx / pd2 - q * dnx / nd2;
        by += q * dpy / pd2 - q * dny / nd2;
    }
    float mag = sqrtf(bx * bx + by * by);
    *bx_out = bx; *by_out = by;
    return mag;
}

static void wpanim_magnetic(uint32_t *buf, int w, int h,
                             const wp_win_rect_t *wins, int nwin,
                             uint64_t time_ms, int intensity, int repel,
                             float hue_deg, int palette)
{
    (void)time_ms;
    if (!s_mag_ready) {
        for (int i = 0; i < WP_MAG_PARTICLES; i++) wp_mag_respawn(&s_mag_p[i], w, h);
        s_mag_ready = 1;
    }
    // Near-black background with a faint fade so trails persist briefly.
    wp_fade(buf, w, h, 220, 256);

    float sgn = repel ? 1.0f : -1.0f;
    float speed = 1.2f + 1.6f * ((float)intensity / 100.0f);
    int nw = nwin > 16 ? 16 : nwin;

    for (int i = 0; i < WP_MAG_PARTICLES; i++) {
        wp_particle_t *p = &s_mag_p[i];
        float bx, by;
        float mag = wp_mag_field(p->x, p->y, wins, nw, sgn, &bx, &by);
        float inv = (mag > 0.0001f) ? (1.0f / mag) : 0.0f;
        p->x += bx * inv * speed;
        p->y += by * inv * speed;
        p->life--;
        if (p->life <= 0 || p->x < 0 || p->x >= (float)w || p->y < 0 || p->y >= (float)h) {
            wp_mag_respawn(p, w, h);
            continue;
        }
        // Field strength -> colour. Previously a fixed cyan-white ramp;
        // #wpcolor makes this palette-aware while keeping the same
        // "brighter where the field is stronger" shape via light/sat.
        float m = mag * 0.6f; if (m > 1.0f) m = 1.0f;
        float dyn_hue = 190.0f + 60.0f * m;   // cyan..blue sweep (SPECTRUM offset)
        float hue = wp_resolve_hue(dyn_hue, hue_deg, palette);
        float sat   = (palette == WP_PALETTE_MONO) ? 0.55f : 0.75f;
        float light = 0.35f + 0.45f * m;
        uint32_t structural = wp_hsl(hue, sat, light);
        float ccr = 0.0f, ccg = 0.0f, ccb = 0.0f, ccw = 0.0f;
        wp_content_sample(p->x, p->y, wins, nw, WP_BASE(w), &ccr, &ccg, &ccb, &ccw);
        uint32_t final_c = wp_apply_content(structural, palette, ccr, ccg, ccb, ccw);
        int32_t ar = (int32_t)((final_c >> 16) & 0xFF);
        int32_t ag = (int32_t)((final_c >> 8)  & 0xFF);
        int32_t ab = (int32_t)(final_c & 0xFF);
        wp_plot_add(buf, w, h, (int)p->x, (int)p->y, ar, ag, ab);
    }
}

// ============================================================================
// FLOW - base rightward current, windows repel + add curl so streamlines
// wrap around them.
// ============================================================================

#define WP_FLOW_PARTICLES 260

static wp_particle_t s_flow_p[WP_FLOW_PARTICLES];
static int s_flow_ready = 0;

static void wp_flow_respawn(wp_particle_t *p, int w, int h)
{
    p->x = -wp_randf() * (float)w * 0.05f;   // just off the left edge
    p->y = wp_randf() * (float)h;
    p->life = 200 + (int)(wp_randf() * 200.0f);
}

static void wpanim_flow(uint32_t *buf, int w, int h,
                         const wp_win_rect_t *wins, int nwin,
                         uint64_t time_ms, int intensity, int repel,
                         float hue_deg, int palette)
{
    float t = (float)time_ms * 0.001f;
    if (!s_flow_ready) {
        for (int i = 0; i < WP_FLOW_PARTICLES; i++) wp_flow_respawn(&s_flow_p[i], w, h);
        s_flow_ready = 1;
    }
    wp_fade(buf, w, h, 235, 256);

    float sgn = repel ? 1.0f : -1.0f;
    float speed = 0.8f + 1.6f * ((float)intensity / 100.0f);
    float base = WP_BASE(w);
    int nw = nwin > 16 ? 16 : nwin;

    for (int i = 0; i < WP_FLOW_PARTICLES; i++) {
        wp_particle_t *p = &s_flow_p[i];
        float vx = 1.0f, vy = 0.15f * wp_sin(p->y * 0.05f + t);
        for (int k = 0; k < nw; k++) {
            float cx = (float)wins[k].x + (float)wins[k].w * 0.5f;
            float cy = (float)wins[k].y + (float)wins[k].h * 0.5f;
            float hw = (float)wins[k].w * 0.5f, hh = (float)wins[k].h * 0.5f;
            float dx = p->x - cx, dy = p->y - cy;
            float d2 = dx * dx + dy * dy;
            float range = (hw + hh) * 1.2f + base * 0.75f;
            float cutoff = range * WP_EXPFALL_MAX;
            if (d2 > cutoff * cutoff) continue;
            float d = sqrtf(d2) + 1.0f;
            float fall = wp_expfall(d / range) * sgn;
            // Outward normal (repulsion) + perpendicular curl so the stream
            // wraps around the rect instead of just stopping at it.
            vx += (dx / d) * fall * 1.0f - (dy / d) * fall * 0.9f;
            vy += (dy / d) * fall * 1.0f + (dx / d) * fall * 0.9f;
        }
        float vlen = sqrtf(vx * vx + vy * vy);
        if (vlen > 0.0001f) { vx /= vlen; vy /= vlen; }
        p->x += vx * speed;
        p->y += vy * speed;
        p->life--;
        if (p->life <= 0 || p->x >= (float)w || p->y < -4 || p->y >= (float)h + 4) {
            wp_flow_respawn(p, w, h);
            continue;
        }
        float sp = speed / 3.0f; if (sp > 1.0f) sp = 1.0f;
        // Previously a fixed blue-ish ramp; #wpcolor makes it palette-aware.
        float dyn_hue = 205.0f + 35.0f * sp;
        float hue = wp_resolve_hue(dyn_hue, hue_deg, palette);
        float sat   = (palette == WP_PALETTE_MONO) ? 0.5f : 0.65f;
        float light = 0.35f + 0.35f * sp;
        uint32_t structural = wp_hsl(hue, sat, light);
        float ccr = 0.0f, ccg = 0.0f, ccb = 0.0f, ccw = 0.0f;
        wp_content_sample(p->x, p->y, wins, nw, base, &ccr, &ccg, &ccb, &ccw);
        uint32_t final_c = wp_apply_content(structural, palette, ccr, ccg, ccb, ccw);
        int32_t ar = (int32_t)((final_c >> 16) & 0xFF);
        int32_t ag = (int32_t)((final_c >> 8)  & 0xFF);
        int32_t ab = (int32_t)(final_c & 0xFF);
        wp_plot_add(buf, w, h, (int)p->x, (int)p->y, ar, ag, ab);
        wp_plot_add(buf, w, h, (int)p->x - 1, (int)p->y, ar / 2, ag / 2, ab / 2);
    }
}

// ============================================================================
// GRAVITY - windows are point masses; particles orbit/bounce, drawn as warm
// additive metaball glow.
// ============================================================================

#define WP_GRAV_PARTICLES 60

typedef struct { float x, y, vx, vy; } wp_gparticle_t;
static wp_gparticle_t s_grav_p[WP_GRAV_PARTICLES];
static int s_grav_ready = 0;

static void wp_grav_respawn(wp_gparticle_t *p, int w, int h)
{
    p->x = wp_randf() * (float)w;
    p->y = wp_randf() * (float)h;
    p->vx = (wp_randf() - 0.5f) * 0.6f;
    p->vy = (wp_randf() - 0.5f) * 0.6f;
}

static void wpanim_gravity(uint32_t *buf, int w, int h,
                            const wp_win_rect_t *wins, int nwin,
                            uint64_t time_ms, int intensity, int repel,
                            float hue_deg, int palette)
{
    (void)time_ms;
    if (!s_grav_ready) {
        for (int i = 0; i < WP_GRAV_PARTICLES; i++) wp_grav_respawn(&s_grav_p[i], w, h);
        s_grav_ready = 1;
    }
    wp_fade(buf, w, h, 200, 256);

    float sgn = repel ? -1.0f : 1.0f;   // GRAVITY's natural sense is "attract";
                                         // repel flips it to a push, same as
                                         // the other three effects.
    float G = 0.06f + 0.10f * ((float)intensity / 100.0f);
    int nw = nwin > 16 ? 16 : nwin;
    float speed_cap = 3.0f;

    for (int i = 0; i < WP_GRAV_PARTICLES; i++) {
        wp_gparticle_t *p = &s_grav_p[i];
        float ax = 0.0f, ay = 0.0f;
        for (int k = 0; k < nw; k++) {
            float cx = (float)wins[k].x + (float)wins[k].w * 0.5f;
            float cy = (float)wins[k].y + (float)wins[k].h * 0.5f;
            float m = (float)wins[k].w * (float)wins[k].h * 0.00004f * sgn;
            float dx = cx - p->x, dy = cy - p->y;
            float d2 = dx * dx + dy * dy + 120.0f;
            ax += G * m * dx / d2;
            ay += G * m * dy / d2;
        }
        p->vx = (p->vx + ax) * 0.985f;
        p->vy = (p->vy + ay) * 0.985f;
        float sp = sqrtf(p->vx * p->vx + p->vy * p->vy);
        if (sp > speed_cap) { p->vx = p->vx / sp * speed_cap; p->vy = p->vy / sp * speed_cap; }
        p->x += p->vx;
        p->y += p->vy;
        if (p->x < 0)      { p->x = 0;          p->vx = -p->vx * 0.6f; }
        if (p->x >= (float)w) { p->x = (float)w - 1; p->vx = -p->vx * 0.6f; }
        if (p->y < 0)      { p->y = 0;          p->vy = -p->vy * 0.6f; }
        if (p->y >= (float)h) { p->y = (float)h - 1; p->vy = -p->vy * 0.6f; }

        // Warm magma additive glow, a few pixels wide (metaball feel without
        // a real density-field pass, which would be a second per-cell loop).
        // Previously a fixed warm-orange ramp (90/40/8 peak); #wpcolor keeps
        // the same peak magnitude but derives the hue from the palette, and
        // the per-tap weights below reproduce the old relative falloff
        // (1.0 centre, 0.5 the four neighbours) against the new peak.
        float dyn_hue = 30.0f;   // warm ember, GRAVITY's own character
        float hue = wp_resolve_hue(dyn_hue, hue_deg, palette);
        float sat   = (palette == WP_PALETTE_MONO) ? 0.6f : 0.85f;
        uint32_t structural = wp_hsl(hue, sat, 0.30f);
        float ccr = 0.0f, ccg = 0.0f, ccb = 0.0f, ccw = 0.0f;
        wp_content_sample(p->x, p->y, wins, nw, WP_BASE(w), &ccr, &ccg, &ccb, &ccw);
        uint32_t final_c = wp_apply_content(structural, palette, ccr, ccg, ccb, ccw);
        int32_t r0 = (int32_t)((final_c >> 16) & 0xFF);
        int32_t g0 = (int32_t)((final_c >> 8)  & 0xFF);
        int32_t b0 = (int32_t)(final_c & 0xFF);
        int cx0 = (int)p->x, cy0 = (int)p->y;
        static const int off[5][2] = { {0,0}, {1,0}, {-1,0}, {0,1}, {0,-1} };
        static const float wgt[5] = { 1.0f, 0.5f, 0.5f, 0.5f, 0.5f };
        for (int j = 0; j < 5; j++)
            wp_plot_add(buf, w, h, cx0 + off[j][0], cy0 + off[j][1],
                        (int32_t)((float)r0 * wgt[j]), (int32_t)((float)g0 * wgt[j]), (int32_t)((float)b0 * wgt[j]));
    }
}

// ============================================================================
// EMFIELD - EM-field / plasma swirl (#emfield, owner request 2026-09-22).
//
// Windows are EM sources whose fields SUPERPOSE (extends wp_mag_field's dipole
// idea to a per-cell velocity field); a MOVING window injects a velocity-driven
// VORTEX (the FLOW mode's perpendicular-curl idiom, keyed on the window's
// velocity instead of a constant) that swirls a persistent DYE buffer. The dye
// is advected once per tick by that velocity field with a single-sample
// semi-Lagrangian back-trace - the CHEAP half of Stam's stable-fluids, with NO
// pressure-projection solve (rejected under #426: the draw thread must never
// over-run). It decays each tick and is re-injected at each window using the
// window's content colour + the user hue/palette, so swirls persist and
// dissipate like plasma. Frosted glass already samples the live g_fb, so it
// shows this swirl through it (see draw.c's glass recompute tune).
//
// Cost: O(cells * windows) for the velocity eval + one bilinear sample per cell,
// the same order as PLASMA; it rides PLASMA's 50ms-capped LO budget (the
// measured 34-58%-of-one-core low-res path). Never blocks, no syscall, all
// state static (#426).
// ============================================================================

// EMFIELD runs at the LO buffer (200x125) only; size the dye planes to that.
// A caller that ever hands a bigger buffer gets the non-persistent fallback
// below rather than an overflow.
#define WP_DYE_MAX_CELLS (200 * 125)   // == SS_LORES_LO_W * SS_LORES_LO_H

static float s_dye_r[WP_DYE_MAX_CELLS];
static float s_dye_g[WP_DYE_MAX_CELLS];
static float s_dye_b[WP_DYE_MAX_CELLS];
static float s_dye2_r[WP_DYE_MAX_CELLS];
static float s_dye2_g[WP_DYE_MAX_CELLS];
static float s_dye2_b[WP_DYE_MAX_CELLS];
static float s_mag[WP_DYE_MAX_CELLS];     // per-cell EM field magnitude, reused in render
static int   s_dye_ready = 0;

// Velocity field at cell (px,py): a faint ambient swirl (so an idle desktop
// still breathes) + per-window EM radial drift (superposition) + per-window
// velocity-driven drag and vortex. Same falloff idiom (wp_expfall over a
// per-window range) used everywhere else in this file.
static inline void wp_emfield_vel(float px, float py,
                                  const wp_win_rect_t *wins, int nw,
                                  float sgn, int w, int h,
                                  float *ovx, float *ovy)
{
    float acx = (float)w * 0.5f, acy = (float)h * 0.5f;
    // Faint solid-body rotation about the screen centre; tiny so it never
    // overpowers a window vortex, present so trails keep drifting when nothing
    // is being dragged.
    float vx = -(py - acy) * 0.0016f;
    float vy =  (px - acx) * 0.0016f;
    float base = WP_BASE(w);

    for (int i = 0; i < nw; i++) {
        // #emfield edge-source (owner 2026-09-22): the field emanates from the
        // window's OUTER EDGE, not its centre, so the halo is RECTANGULAR
        // (contours hug the perimeter) rather than a circle around the middle.
        // d is the distance to the nearest point on the window rectangle and
        // the outward normal is that edge's perpendicular (radial at corners).
        float hw = (float)wins[i].w * 0.5f, hh = (float)wins[i].h * 0.5f;
        float rx0 = (float)wins[i].x, ry0 = (float)wins[i].y;
        float rx1 = rx0 + (float)wins[i].w, ry1 = ry0 + (float)wins[i].h;
        float nearx = px < rx0 ? rx0 : (px > rx1 ? rx1 : px);
        float neary = py < ry0 ? ry0 : (py > ry1 ? ry1 : py);
        float dx = px - nearx, dy = py - neary;   // outward from the nearest edge point
        float d2 = dx * dx + dy * dy;             // squared distance TO THE EDGE (0 inside)
        float range = base * 0.5f + (hw + hh) * 0.20f;
        float cutoff = range * WP_EXPFALL_MAX;
        if (d2 > cutoff * cutoff) continue;   // cheap cull, same as the others
        float d = sqrtf(d2) + 1.0f;
        float fall = wp_expfall(d / range);
        float nx = dx / d, ny = dy / d;        // outward normal FROM THE EDGE (perpendicular)

        // EM radial: focused window is a source (+), unfocused a weaker sink
        // (-); charge ~ size; sgn flips the whole sense with repel/attract.
        float q = (wins[i].focused ? 1.0f : -0.6f) * ((hw + hh) * 0.02f) * sgn;
        vx += nx * fall * q;
        vy += ny * fall * q;

        // Window velocity, capped so a fast drag cannot blow the back-trace
        // clean off-grid (semi-Lagrangian stays stable, but an over-long trace
        // reads as a teleport rather than a swirl).
        float wvx = wins[i].vx, wvy = wins[i].vy;
        float sp2 = wvx * wvx + wvy * wvy;
        const float SPEED_CAP = 10.0f;
        if (sp2 > SPEED_CAP * SPEED_CAP) {
            float sc = SPEED_CAP / sqrtf(sp2);
            wvx *= sc; wvy *= sc;
        }
        // DRAG: fluid near a moving window is carried with it.
        vx += wvx * fall * 0.9f;
        vy += wvy * fall * 0.9f;
        // VORTEX: rotational term (perpendicular to the outward normal) whose
        // sense follows the motion so the swirl trails the drag; magnitude
        // grows with the window's speed.
        float speed = sqrtf(wvx * wvx + wvy * wvy);
        float spin  = (wvx * (-ny) + wvy * (nx));   // motion . perpendicular
        float curl  = spin * 0.8f + speed * 0.15f;
        vx += (-ny) * fall * curl;
        vy += ( nx) * fall * curl;
    }
    *ovx = vx; *ovy = vy;
}

static void wpanim_emfield(uint32_t *buf, int w, int h,
                           const wp_win_rect_t *wins, int nwin,
                           uint64_t time_ms, int intensity, int repel,
                           float hue_deg, int palette)
{
    float t     = (float)time_ms * 0.001f;
    float I01   = (float)intensity / 100.0f;               // 0..1
    float sgn   = repel ? 1.0f : -1.0f;
    int   nw    = nwin > 16 ? 16 : nwin;
    int   ncell = w * h;
    int   have_dye = (ncell <= WP_DYE_MAX_CELLS);
    float vgain = 0.9f + 1.8f * I01;                        // swirl energy (punchier)
    float decay = 0.955f + 0.030f * I01;                    // 0.955..0.985 (longer plasma trails)

    // The baseline is now the EM field itself, made visible across the WHOLE
    // screen in the render step below (coloured by local field magnitude from
    // s_mag[]), so an idle desktop reads as a living field, not near-black.
    float msat = (palette == WP_PALETTE_MONO) ? 0.55f : 0.85f;

    if (!have_dye) {
        // Fallback (never taken on the LO path wallpaper.c uses): render the
        // instantaneous EM field magnitude, no persistence.
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float vx, vy;
                wp_emfield_vel((float)x, (float)y, wins, nw, sgn, w, h, &vx, &vy);
                float mag = sqrtf(vx * vx + vy * vy) * 0.5f;
                if (mag > 1.0f) mag = 1.0f;
                float dyn_hue = 260.0f - 120.0f * mag + t * 8.0f;
                float hue = wp_resolve_hue(dyn_hue, hue_deg, palette);
                float sat = (palette == WP_PALETTE_MONO) ? 0.55f : 0.85f;
                buf[y * w + x] = wp_hsl(hue, sat, 0.12f + 0.45f * mag);
            }
        }
        return;
    }

    if (!s_dye_ready) {
        for (int i = 0; i < WP_DYE_MAX_CELLS; i++) {
            s_dye_r[i] = s_dye_g[i] = s_dye_b[i] = 0.0f;
        }
        s_dye_ready = 1;
    }

    // (1) ADVECT: back-trace each cell along the velocity field and bilinearly
    //     sample the PREVIOUS dye, applying decay. Semi-Lagrangian, one sample.
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float vx, vy;
            wp_emfield_vel((float)x, (float)y, wins, nw, sgn, w, h, &vx, &vy);
            s_mag[y * w + x] = sqrtf(vx * vx + vy * vy);
            // #emfield-fix (owner 2026-09-22): a gentle, divergence-light GLOBAL
            // drift added to the semi-Lagrangian back-trace, so the advected dye
            // keeps MOVING even where the window-driven velocity is ~0 - i.e. the
            // interior of a STILL window, which used to read as frozen. Kept small
            // (|drift| <= ~1.5 cells/frame) versus the edge radial/vortex terms so
            // window EDGES still dominate the motion (the owner-approved rectangular
            // halos from 64e61e8e are untouched); just enough that a still window's
            // interior visibly flows. Added to the back-trace only, NOT to s_mag, so
            // the interior stays DIMMER than the edges (edge dominance preserved).
            float drx = sinf((float)y * 0.060f + t * 0.50f) * 0.9f
                      + sinf((float)x * 0.040f - t * 0.35f) * 0.6f;
            float dry = cosf((float)x * 0.055f - t * 0.60f) * 0.9f
                      + sinf((float)y * 0.045f + t * 0.30f) * 0.6f;
            float bx = (float)x - vx * vgain - drx;
            float by = (float)y - vy * vgain - dry;
            if (bx < 0.0f) bx = 0.0f; else if (bx > (float)(w - 1)) bx = (float)(w - 1);
            if (by < 0.0f) by = 0.0f; else if (by > (float)(h - 1)) by = (float)(h - 1);
            int x0 = (int)bx, y0 = (int)by;
            int x1 = (x0 + 1 < w) ? x0 + 1 : x0;
            int y1 = (y0 + 1 < h) ? y0 + 1 : y0;
            float fx = bx - (float)x0, fy = by - (float)y0;
            int i00 = y0 * w + x0, i10 = y0 * w + x1;
            int i01 = y1 * w + x0, i11 = y1 * w + x1;
            float w00 = (1.0f - fx) * (1.0f - fy), w10 = fx * (1.0f - fy);
            float w01 = (1.0f - fx) * fy,          w11 = fx * fy;
            int o = y * w + x;
            s_dye2_r[o] = (s_dye_r[i00]*w00 + s_dye_r[i10]*w10 + s_dye_r[i01]*w01 + s_dye_r[i11]*w11) * decay;
            s_dye2_g[o] = (s_dye_g[i00]*w00 + s_dye_g[i10]*w10 + s_dye_g[i01]*w01 + s_dye_g[i11]*w11) * decay;
            s_dye2_b[o] = (s_dye_b[i00]*w00 + s_dye_b[i10]*w10 + s_dye_b[i01]*w01 + s_dye_b[i11]*w11) * decay;
        }
    }
    for (int i = 0; i < ncell; i++) {
        s_dye_r[i] = s_dye2_r[i];
        s_dye_g[i] = s_dye2_g[i];
        s_dye_b[i] = s_dye2_b[i];
    }

    // (2) INJECT coloured energy at each window (its content colour in CONTENT
    //     mode, else a hue-driven structural colour) in a soft disc.
    for (int i = 0; i < nw; i++) {
        float ir, ig, ib;
        if (palette == WP_PALETTE_CONTENT && wins[i].has_content) {
            ir = (float)wins[i].cr; ig = (float)wins[i].cg; ib = (float)wins[i].cb;
        } else {
            float dyn_hue = t * 18.0f + (float)i * 47.0f;
            float hue = wp_resolve_hue(dyn_hue, hue_deg, palette);
            float sat = (palette == WP_PALETTE_MONO) ? 0.6f : 0.9f;
            uint32_t c = wp_hsl(hue, sat, 0.55f);
            ir = (float)((c >> 16) & 0xFF); ig = (float)((c >> 8) & 0xFF); ib = (float)(c & 0xFF);
        }
        float cx = (float)wins[i].x + (float)wins[i].w * 0.5f;
        float cy = (float)wins[i].y + (float)wins[i].h * 0.5f;
        float amp = (wins[i].focused ? 1.0f : 0.6f) * (0.7f + 0.6f * I01);
        int rad = 5 + (int)(((float)wins[i].w + (float)wins[i].h) * 0.05f);
        float rdenom = (float)rad * 0.6f + 1.0f;
        for (int dy = -rad; dy <= rad; dy++) {
            for (int dx = -rad; dx <= rad; dx++) {
                int px = (int)cx + dx, py = (int)cy + dy;
                if ((unsigned)px >= (unsigned)w || (unsigned)py >= (unsigned)h) continue;
                float fall = wp_expfall(sqrtf((float)(dx * dx + dy * dy)) / rdenom);
                float add = amp * fall * 95.0f;
                int o = py * w + px;
                s_dye_r[o] += ir * (add / 255.0f); if (s_dye_r[o] > 255.0f) s_dye_r[o] = 255.0f;
                s_dye_g[o] += ig * (add / 255.0f); if (s_dye_g[o] > 255.0f) s_dye_g[o] = 255.0f;
                s_dye_b[o] += ib * (add / 255.0f); if (s_dye_b[o] > 255.0f) s_dye_b[o] = 255.0f;
            }
        }
    }

    // (3) RENDER: full-screen EM-field baseline (from s_mag, with a faint
    //     organic shimmer so calm regions still move) + the advected dye on top.
    for (int i = 0; i < ncell; i++) {
        int cx = i % w, cy = i / w;
        // #emfield-fix (owner 2026-09-22): the field emanates from window EDGES,
        // so a STILL window's interior has s_mag~0 and used to read near-static.
        // Strengthen the always-on organic shimmer (a second, slower octave on top
        // of the original) so calm interior regions visibly BREATHE. This is a
        // baseline-only lift bounded to <=~0.52, well under a real field source, so
        // the edge halos (driven by s_mag) stay the brightest, dominant feature.
        float shim = 0.17f * (sinf((float)cx * 0.10f + t * 0.7f)
                            + sinf((float)cy * 0.09f - t * 0.9f))
                   + 0.09f * (sinf(((float)cx + (float)cy) * 0.045f - t * 0.5f)
                            + sinf(((float)cx - (float)cy) * 0.050f + t * 0.6f));
        float m01 = s_mag[i] * 2.2f + shim;
        if (m01 < 0.0f) m01 = 0.0f; else if (m01 > 1.0f) m01 = 1.0f;
        float bhue = wp_resolve_hue(250.0f - 170.0f * m01 + t * 10.0f, hue_deg, palette);
        float blight = 0.16f + 0.30f * m01;   // visible everywhere, bright near windows
        uint32_t bc = wp_hsl(bhue, msat, blight);
        int R = (int)(s_dye_r[i] + (float)((bc >> 16) & 0xFF)); if (R > 255) R = 255;
        int G = (int)(s_dye_g[i] + (float)((bc >> 8)  & 0xFF)); if (G > 255) G = 255;
        int B = (int)(s_dye_b[i] + (float)(bc & 0xFF));         if (B > 255) B = 255;
        buf[i] = 0xFF000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
    }
}

// ============================================================================
// Dispatch
// ============================================================================

void wpanim_render(int mode, uint32_t *buf, int w, int h,
                    const wp_win_rect_t *wins, int nwin,
                    uint64_t time_ms, int intensity, int repel,
                    float hue_deg, int palette)
{
    if (!buf || w <= 0 || h <= 0) return;
    if (intensity < 0) intensity = 0;
    if (intensity > 100) intensity = 100;
    // Wrap hue into [0,360); clamp an out-of-range palette to SPECTRUM
    // rather than reading past WP_PALETTE_COUNT's small lookup-by-value uses.
    hue_deg = hue_deg - 360.0f * floorf(hue_deg / 360.0f);
    if (palette < 0 || palette >= WP_PALETTE_COUNT) palette = WP_PALETTE_SPECTRUM;
    wpanim_init();
    switch (mode) {
        case WPANIM_PLASMA:   wpanim_plasma(buf, w, h, wins, nwin, time_ms, intensity, repel, hue_deg, palette);   break;
        case WPANIM_MAGNETIC: wpanim_magnetic(buf, w, h, wins, nwin, time_ms, intensity, repel, hue_deg, palette); break;
        case WPANIM_FLOW:     wpanim_flow(buf, w, h, wins, nwin, time_ms, intensity, repel, hue_deg, palette);     break;
        case WPANIM_GRAVITY:  wpanim_gravity(buf, w, h, wins, nwin, time_ms, intensity, repel, hue_deg, palette);  break;
        case WPANIM_EMFIELD:  wpanim_emfield(buf, w, h, wins, nwin, time_ms, intensity, repel, hue_deg, palette);  break;
        default: break;   // WPANIM_OFF or garbage: leave buf untouched
    }
}

const char *wpanim_mode_name(int mode)
{
    switch (mode) {
        case WPANIM_PLASMA:   return "Plasma";
        case WPANIM_MAGNETIC: return "Magnetic";
        case WPANIM_FLOW:     return "Flow";
        case WPANIM_GRAVITY:  return "Gravity";
        case WPANIM_EMFIELD:  return "EM Field";
        default:              return "Off";
    }
}

const char *wpanim_palette_name(int palette)
{
    switch (palette) {
        case WP_PALETTE_CONTENT: return "Content";
        case WP_PALETTE_MONO:    return "Mono";
        default:                 return "Spectrum";
    }
}

uint32_t wpanim_hue_preview_color(float hue_deg)
{
    return wp_hsl(hue_deg, 0.85f, 0.50f);
}
