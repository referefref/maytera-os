/* Maytera Squadron - a 1942-style top-down vertical-scrolling arcade shoot-em-up.
 *
 * A userland compositor app: it renders every frame into an ARGB backbuffer and
 * pushes it with SYS_WIN_BLIT (the same path glcube / arena use). Pure 2D shmup.
 *
 * EXPANSION (2026-07): diverse enemy bullet types (aimed/spread/wave/ring/homing
 * missile/plasma), 11 enemy archetypes incl. scrolling stationary space-station
 * defenses (turrets/silos/station core), many movement patterns, a 3-phase boss
 * that fires several weapon systems at once, weapon power-ups (twin/tri/wide/
 * laser beam/homing missiles/wave/bomb + shield/life), a 5-background vertical
 * crossfade per level (level 1 = rainbow aurora), and additive-glow bloom on all
 * projectiles/beams/explosions/pickups/engine trails.
 *
 * Controls: ship follows the mouse; WASD/Arrows also move it. Fire is HELD,
 * not auto: hold the LEFT mouse button (or SPACE) to fire, release to stop.
 * RIGHT-click (or B) drops a screen-clearing bomb. ESC/P pause.
 *
 * Art: 24-bit BMPs under /SQUADRON, magenta (#FF00FF) colour-key. Every sprite
 * and background has a procedural fallback so the game runs before art is on
 * disk. Backgrounds: procedural pixel art (pxbg_draw, #562) drawn behind
 * everything - see Part C below; L<lvl>BG1..5.BMP loading is dormant code
 * kept for a possible future authored-pixel-art pass. LOGO.BMP is the title.
 *
 * HUD (#475): 6 readout boxes baked into each side-panel's art (SIDEBARL/R.BMP,
 * measured at 400x900 art px; see SB_BOX_L/SB_BOX_R below) hold the live stats,
 * replacing the old wide top-of-panel rectangles. The panel art itself is
 * static, so it is rendered ONCE (rebuild_sidebars(), on first frame / resize)
 * instead of being re-scaled into the present buffer every frame; each frame
 * only restores the 12 box rects from a cached clean snapshot and redraws
 * their (possibly changed) text, which is what actually varies. This was the
 * fix for the choppy framerate the new panel art introduced: the old
 * per-frame sidebar_blit() of the full panel did a per-pixel divide over the
 * whole panel height twice a frame, at a resolution that scales with the real
 * window size, regardless of whether anything in the art had changed.
 *
 * PARALLAX BACKGROUND (#476): draw_background() layers, slowest/furthest to
 * fastest/nearest: the L<lvl>BG art (or proc_bg fallback) scrolling at a
 * fixed gentle 0.35px/frame -> a precomputed lattice-noise nebula wash
 * (nebula_draw, baked once in neb_init) drifting slower still -> a far
 * layer of AI-generated planets/a ringed planet/an asteroid/a nebula puff
 * (bgobj_draw_all, per-object shape data cached at spawn, never per-frame) ->
 * a 3-depth starfield (stars_draw; far/mid/near differ in speed, size and
 * brightness). Every layer's speed is kept below every enemy pattern's vy so
 * the depth ordering reads correctly. All state is precomputed/cached, not
 * recomputed per pixel per frame, per the framerate-fix lesson above.
 *
 * Part B (2026-07, AI sprite art): the OpenAI images key on file now works
 * (gpt-image-1, background=transparent). BG_PLANET/BG_RINGED/BG_ASTEROID/
 * BG_NEBULA.BMP replace the far-layer procedural shapes in bgobj_draw(), and
 * TXT_GAMEOVER/TXT_FINALSCORE/TXT_REACHEDSTAGE/TXT_TRYAGAIN/TXT_ESCMENU.BMP
 * replace the sq_text_stylized() placeholders in draw_gameover(). Every
 * asset was generated transparent, then flattened onto the game's magenta
 * (#FF00FF) colour-key so bmp_load() loads it exactly like any other sprite,
 * and EVERY one of those draws still falls back to the original procedural/
 * sq_text_stylized code if its BMP is missing (A_*.ok == 0) - the fallback
 * path from the first pass (see CHANGELOG) was kept deliberately so a
 * partial or absent /SQUADRON asset set can never crash or blank the game.
 * sq_text_stylized() itself (outline + glow halo + gradient text) is kept
 * below for that fallback and for the pause/stage-clear/menu screens it
 * still renders (no AI art was requested for those).
 *
 * Part C (2026-07-21, #562, pixel-art backdrop): the whole backdrop was
 * photoreal AI photography and it clashed with the flat-shaded, glow-bloom
 * look of everything else in the game - two separate things, both fixed:
 * (1) the deployed VM/golden turned out to have real L<set>BGx.BMP full-
 * screen 640x960 photoreal nebula/aurora art on disk (bg_fill_scroll'd as
 * the base scrolling layer via bgset_load), NEVER checked into git - the
 * exact same asset-drift gap as #535's "git repo missing source for
 * shipping apps" - and this, not the smaller far-layer objects, is what a
 * player actually sees as "the same backgrounds". (2) BG_PLANET/RINGED/
 * ASTEROID/NEBULA.BMP (the far-layer floating landmark objects) were also
 * photoreal. Fix: BG_PLANET/RINGED/ASTEROID/NEBULA.BMP were regenerated as
 * true chunky pixel art (hard blocks + Bayer ordered dithering, see
 * tools/genart.py), and draw_background() now always renders the base layer
 * with the new procedural pxbg_draw() (same technique), never bg_fill_scroll
 * of the old L<set>BGx.BMP art - see the pxbg_draw() and draw_background()
 * comments below for the full rationale and which level palettes have been
 * visually verified.
 *
 * Part D (2026-07-21, Direction A, user-approved polish pass on the #562
 * prototype): four fixes. (1) g_neb[]'s lattice interpolation was non-cyclic
 * ("open ended" across the lattice, see neb_init()'s comment) so wrapping it
 * with `% NEB_H`/`% NEB_W` stitched two unrelated lattice corners together -
 * a visible horizontal seam every wrap. Rebuilt as a proper TOROIDAL lattice
 * (both axes), which is the standard fix for tileable noise: fully seamless
 * now on every axis, proven with paired screendumps straddling the wrap
 * boundary (see CHANGELOG). (2) pxbg_draw() gained a second, coarser
 * toroidal "zone" field (g_nebz/nebz_init) that darkens cells one step in
 * VOID patches and brightens + hue-shifts them to a neighbouring-hue palette
 * (g_pxbg_pal2) in CORE patches, so each stage reads as a designed starscape
 * with depth and adjacent-hue variety, not a monotone wash - still strictly
 * within each stage's own hue family. (3) Added a 4th, even-further/dimmer/
 * slower star-CLUSTER layer (clust_init/update/draw) behind the existing
 * 3-depth starfield for extra depth, deliberately kept dim and sparse so it
 * never competes with gameplay readability. (4) All of the above is generic
 * over `set` (0/1/2), so violet/blue-hive/ember-red all get the identical
 * treatment - see pxbg_draw()'s comment for per-set verification status.
 */
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "stdlib.h"    /* O_RDONLY, malloc/free, exit */
#include "../../libc/pthread.h"   /* #it20: the audio worker thread */

typedef signed char GLbyte;
#include "../../libgl/src/font8x8_basic.h"

/* ============================================================ backbuffer === */
#define MAXW 3840
#define MAXH 2160
static uint32_t *g_blit = 0;
static long  g_blit_cap = 0;
static int   W = 1024, H = 768;         /* PLAYFIELD (narrow portrait strip) size */
static int   g_win = -1;

/* Portrait presentation: the game renders into the narrow W x H g_blit playfield,
 * then g_blit is composited into the centre column of a full-window present buffer
 * (g_present, FBW x FBH) that also carries the left + right sidebar art + HUD. */
static int   FBW = 1024, FBH = 768;     /* full window / present buffer size */
static int   PF_OX = 0;                  /* x offset of the playfield within g_present */
static uint32_t *g_present = 0;
static long  g_present_cap = 0;

#define OPAQUE 0xFF000000u

/* ================================================================= maths === */
static float fsinf(float x) {
    while (x >  3.14159265f) x -= 6.28318531f;
    while (x < -3.14159265f) x += 6.28318531f;
    float x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f) + x2 * x2 * (1.0f / 120.0f)
                     - x2 * x2 * x2 * (1.0f / 5040.0f));
}
static float fcosf(float x) { return fsinf(x + 1.57079633f); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int   iabs(int v) { return v < 0 ? -v : v; }
/* fast normalised direction from (dx,dy); writes unit vector. */
static void norm_dir(float dx, float dy, float *ox, float *oy) {
    float d = dx * dx + dy * dy; if (d < 1) d = 1;
    float g = 0.0006f; for (int i = 0; i < 6; i++) g = g * (1.5f - 0.5f * d * g * g);
    *ox = dx * g; *oy = dy * g;
}

/* xorshift PRNG */
static uint32_t g_rng = 0x1234abcdu;
/* g_now: moved here (was declared much further down, by the input-handling
 * globals) once add_score()'s #it9 new-high-score toast needed it earlier in
 * the file than anything previously did. A plain unsigned with no init-order
 * dependency on anything else, so declaring it this early is always safe;
 * see blame.md for the general "a file-scope static must be declared before
 * ANY function in the same translation unit that reads it" pitfall this hit
 * twice while adding this pass's features (also bit screen_shake() earlier). */
static unsigned g_now;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int rndrange(int lo, int hi) { if (hi <= lo) return lo; return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

/* HSV->RGB (h 0..360, s,v 0..255) for rainbow effects. */
static uint32_t hsv(int h, int s, int v) {
    h = ((h % 360) + 360) % 360;
    int c = v * s / 255;
    int hh = h / 60;
    int xx = c * (60 - iabs((h % 120) - 60)) / 60;
    int m = v - c, r = 0, g = 0, b = 0;
    switch (hh) {
        case 0: r = c; g = xx; break; case 1: r = xx; g = c; break;
        case 2: g = c; b = xx; break; case 3: g = xx; b = c; break;
        case 4: r = xx; b = c; break; default: r = c; b = xx; break;
    }
    return (uint32_t)(((r + m) << 16) | ((g + m) << 8) | (b + m));
}

/* ======================================================== draw primitives = */
static inline void put_px(int x, int y, uint32_t rgb) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    g_blit[y * W + x] = rgb;
}
static void fill_rect(int x, int y, int rw, int rh, uint32_t rgb) {
    if (x < 0) { rw += x; x = 0; } if (y < 0) { rh += y; y = 0; }
    if (x + rw > W) rw = W - x; if (y + rh > H) rh = H - y;
    if (rw <= 0 || rh <= 0) return;
    for (int j = 0; j < rh; j++) { uint32_t *row = g_blit + (y + j) * W + x; for (int i = 0; i < rw; i++) row[i] = rgb; }
}
static uint32_t blend(uint32_t dst, uint32_t src, int a) {
    int ia = 255 - a;
    uint32_t r = (((src >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia) / 255;
    uint32_t g = (((src >>  8) & 0xFF) * a + ((dst >>  8) & 0xFF) * ia) / 255;
    uint32_t b = (((src      ) & 0xFF) * a + ((dst      ) & 0xFF) * ia) / 255;
    return (r << 16) | (g << 8) | b;
}
static void blend_rect(int x, int y, int rw, int rh, uint32_t rgb, int a) {
    if (a <= 0) return; if (a > 255) a = 255;
    if (x < 0) { rw += x; x = 0; } if (y < 0) { rh += y; y = 0; }
    if (x + rw > W) rw = W - x; if (y + rh > H) rh = H - y;
    if (rw <= 0 || rh <= 0) return;
    for (int j = 0; j < rh; j++) { uint32_t *row = g_blit + (y + j) * W + x; for (int i = 0; i < rw; i++) row[i] = blend(row[i], rgb, a); }
}
/* Additive radial glow (bloom). Cheap: bounded r; quadratic falloff; saturating add. */
static void add_glow(int cx, int cy, int r, uint32_t rgb, int inten) {
    if (r < 1) r = 1; if (r > 40) r = 40;
    int sr = (rgb >> 16) & 0xFF, sg = (rgb >> 8) & 0xFF, sb = rgb & 0xFF;
    int r2 = r * r;
    for (int y = -r; y <= r; y++) {
        int py = cy + y; if ((unsigned)py >= (unsigned)H) continue;
        uint32_t *row = g_blit + py * W;
        for (int x = -r; x <= r; x++) {
            int px = cx + x; if ((unsigned)px >= (unsigned)W) continue;
            int d2 = x * x + y * y; if (d2 > r2) continue;
            int f = inten - inten * d2 / r2; if (f <= 0) continue;
            uint32_t d = row[px];
            int dr = ((d >> 16) & 0xFF) + sr * f / 255; if (dr > 255) dr = 255;
            int dg = ((d >>  8) & 0xFF) + sg * f / 255; if (dg > 255) dg = 255;
            int db = ( d        & 0xFF) + sb * f / 255; if (db > 255) db = 255;
            row[px] = (dr << 16) | (dg << 8) | db;
        }
    }
}
/* 8x8 bitmap text with integer scale */
static void sq_text(int x, int y, const char *s, uint32_t rgb, int scale) {
    if (!s || scale < 1) return; int cx = x;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\n') { cx = x; y += 9 * scale; continue; }
        if (c > 127) c = '?';
        const GLbyte *g = font8x8_basic[c];
        for (int row = 0; row < 8; row++) { int bits = g[row]; if (!bits) continue;
            for (int col = 0; col < 8; col++) { if (!(bits & (1 << col))) continue;
                if (scale == 1) put_px(cx + col, y + row, rgb);
                else fill_rect(cx + col * scale, y + row * scale, scale, scale, rgb); } }
        cx += 8 * scale;
    }
}
static void sq_text_sh(int x, int y, const char *s, uint32_t rgb, int scale) {
    sq_text(x + scale, y + scale, s, 0x00101018, scale); sq_text(x, y, s, rgb, scale);
}
static int text_w(const char *s, int scale) { int n = 0; if (!s) return 0; while (*s++) n++; return n * 8 * scale; }
static void sq_text_center(int cx, int y, const char *s, uint32_t rgb, int scale) { sq_text_sh(cx - text_w(s, scale) / 2, y, s, rgb, scale); }
static void num_to_str(long v, char *buf) {
    char tmp[24]; int n = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; } if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 20) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; if (neg) buf[j++] = '-'; while (n > 0) buf[j++] = tmp[--n]; buf[j] = 0;
}

/* ============================================ stylized "sprite-look" text ==
 * #476 Part B: outline + soft glow halo + top-lit/bottom-shade gradient text
 * renderer, used on the pause/stage-clear/menu screens (no AI art was
 * requested for those) and as the FALLBACK for the game-over screen's
 * GAME OVER / FINAL SCORE / REACHED STAGE / TRY AGAIN? / ESC FOR MENU sprite
 * art (see draw_gameover and the file header) whenever a TXT_*.BMP asset is
 * missing from /SQUADRON, so that screen degrades gracefully instead of
 * showing blank space. Only used on menu / game-over / stage screens (a
 * handful of characters), never per-frame gameplay HUD, so the extra passes
 * cost nothing measurable. */
static void put_px_a(int x, int y, uint32_t rgb, int a) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    if (a <= 0) return;
    if (a >= 255) { g_blit[y * W + x] = rgb; return; }
    g_blit[y * W + x] = blend(g_blit[y * W + x], rgb, a);
}
static void sq_text_a(int x, int y, const char *s, uint32_t rgb, int scale, int a) {
    if (!s || scale < 1) return; int cx = x;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\n') { cx = x; y += 9 * scale; continue; }
        if (c > 127) c = '?';
        const GLbyte *g = font8x8_basic[c];
        for (int row = 0; row < 8; row++) { int bits = g[row]; if (!bits) continue;
            for (int col = 0; col < 8; col++) { if (!(bits & (1 << col))) continue;
                for (int yy = 0; yy < scale; yy++) for (int xx = 0; xx < scale; xx++)
                    put_px_a(cx + col * scale + xx, y + row * scale + yy, rgb, a); } }
        cx += 8 * scale;
    }
}
static void sq_text_stylized(int cx, int y, const char *s, uint32_t lit, uint32_t shade, uint32_t glow, int scale) {
    int x0 = cx - text_w(s, scale) / 2;
    for (int off = 3; off >= 1; off--) {              /* soft additive-ish glow halo */
        int a = 55 - off * 12; if (a < 8) a = 8;
        sq_text_a(x0 - off, y, s, glow, scale, a); sq_text_a(x0 + off, y, s, glow, scale, a);
        sq_text_a(x0, y - off, s, glow, scale, a); sq_text_a(x0, y + off, s, glow, scale, a);
    }
    sq_text(x0 - 1, y, s, 0x00000000, scale); sq_text(x0 + 1, y, s, 0x00000000, scale);   /* hard outline */
    sq_text(x0, y - 1, s, 0x00000000, scale); sq_text(x0, y + 1, s, 0x00000000, scale);
    sq_text(x0 + 2, y + 2, s, 0x00000000, scale);                                          /* drop shadow */
    int cxp = x0;
    for (const char *p = s; *p; p++) {                 /* top-lit / bottom-shade gradient fill */
        unsigned char c = (unsigned char)*p; if (c > 127) c = '?';
        const GLbyte *g = font8x8_basic[c];
        for (int row = 0; row < 8; row++) { int bits = g[row]; if (!bits) continue;
            uint32_t rc = blend(lit, shade, row * 255 / 7);
            for (int col = 0; col < 8; col++) { if (!(bits & (1 << col))) continue;
                if (scale == 1) put_px(cxp + col, y + row, rc);
                else fill_rect(cxp + col * scale, y + row * scale, scale, scale, rc); } }
        cxp += 8 * scale;
    }
}

/* ===== absolute-coord primitives writing the FULL-WINDOW present buffer ===== */
/* Used only for the sidebar art + HUD readouts, at FBW stride over g_present. */
static inline void put_px_abs(int x, int y, uint32_t rgb) {
    if ((unsigned)x >= (unsigned)FBW || (unsigned)y >= (unsigned)FBH) return;
    g_present[y * FBW + x] = rgb;
}
static void fill_rect_abs(int x, int y, int rw, int rh, uint32_t rgb) {
    if (x < 0) { rw += x; x = 0; } if (y < 0) { rh += y; y = 0; }
    if (x + rw > FBW) rw = FBW - x; if (y + rh > FBH) rh = FBH - y;
    if (rw <= 0 || rh <= 0) return;
    for (int j = 0; j < rh; j++) { uint32_t *row = g_present + (y + j) * FBW + x; for (int i = 0; i < rw; i++) row[i] = rgb; }
}
static void sq_text_abs(int x, int y, const char *s, uint32_t rgb, int scale) {
    if (!s || scale < 1) return; int cx = x;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\n') { cx = x; y += 9 * scale; continue; }
        if (c > 127) c = '?';
        const GLbyte *g = font8x8_basic[c];
        for (int row = 0; row < 8; row++) { int bits = g[row]; if (!bits) continue;
            for (int col = 0; col < 8; col++) { if (!(bits & (1 << col))) continue;
                if (scale == 1) put_px_abs(cx + col, y + row, rgb);
                else fill_rect_abs(cx + col * scale, y + row * scale, scale, scale, rgb); } }
        cx += 8 * scale;
    }
}
static void sq_text_abs_sh(int x, int y, const char *s, uint32_t rgb, int scale) {
    sq_text_abs(x + scale, y + scale, s, 0x00050810, scale); sq_text_abs(x, y, s, rgb, scale);
}
static void sq_text_abs_center(int cx, int y, const char *s, uint32_t rgb, int scale) {
    sq_text_abs_sh(cx - text_w(s, scale) / 2, y, s, rgb, scale);
}

/* ============================================================== sprites === */
typedef struct { int w, h; uint32_t *px; int ok; } Sprite;

static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static int bmp_load(const char *path, Sprite *s) {
    s->ok = 0; s->px = 0; s->w = s->h = 0;
    int fd = sys_open(path, O_RDONLY); if (fd < 0) return -1;
    unsigned char hdr[54];
    if (sys_read(fd, hdr, 54) != 54) { sys_close(fd); return -1; }
    if (hdr[0] != 'B' || hdr[1] != 'M') { sys_close(fd); return -1; }
    uint32_t dataoff = rd32(hdr + 10);
    int32_t  w = (int32_t)rd32(hdr + 18), h = (int32_t)rd32(hdr + 22);
    int      bpp = hdr[28] | (hdr[29] << 8); uint32_t comp = rd32(hdr + 30);
    if (bpp != 24 || comp != 0) { sys_close(fd); return -1; }
    int flip = 1; if (h < 0) { h = -h; flip = 0; }
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) { sys_close(fd); return -1; }
    sys_seek(fd, (long)dataoff, 0);
    int rowsz = (w * 3 + 3) & ~3;
    unsigned char *row = (unsigned char *)malloc(rowsz);
    uint32_t *px = (uint32_t *)malloc((unsigned long)w * h * 4);
    if (!row || !px) { if (row) free(row); if (px) free(px); sys_close(fd); return -1; }
    for (int r = 0; r < h; r++) {
        if (sys_read(fd, row, rowsz) != rowsz) { free(row); free(px); sys_close(fd); return -1; }
        int dy = flip ? (h - 1 - r) : r; uint32_t *dst = px + dy * w;
        for (int x = 0; x < w; x++) {
            unsigned b = row[x * 3], g = row[x * 3 + 1], rr = row[x * 3 + 2];
            if (rr >= 0xE0 && g <= 0x30 && b >= 0xE0) dst[x] = 0;
            else dst[x] = OPAQUE | ((uint32_t)rr << 16) | ((uint32_t)g << 8) | b;
        }
    }
    free(row); sys_close(fd); s->w = w; s->h = h; s->px = px; s->ok = 1; return 0;
}
static void spr_blit(const Sprite *s, int dx, int dy) {
    if (!s->ok) return;
    for (int y = 0; y < s->h; y++) { int py = dy + y; if ((unsigned)py >= (unsigned)H) continue;
        const uint32_t *src = s->px + y * s->w; uint32_t *drow = g_blit + py * W;
        for (int x = 0; x < s->w; x++) { uint32_t p = src[x]; if (!(p & 0xFF000000u)) continue;
            int px = dx + x; if ((unsigned)px >= (unsigned)W) continue; drow[px] = p & 0x00FFFFFFu; } }
}
static void spr_blit_c(const Sprite *s, int cx, int cy) { if (s->ok) spr_blit(s, cx - s->w / 2, cy - s->h / 2); }
/* #it13: centre-blit a sprite MODULATED toward a tint colour. Per channel the
 * source pixel is multiplied by the tint (src*tint/255) and then mixed back
 * toward the untouched source by (255-amt)/255, so amt==0 is exactly
 * spr_blit_c() and amt==255 is a fully recoloured silhouette. A multiply,
 * not a blend-with-a-flat-colour, is what keeps the sprite's own internal
 * shading and highlights readable instead of flattening it into a coloured
 * blob. Cost is one multiply-add per opaque pixel, on a 46x40-ish sprite. */
static void spr_blit_tint_c(const Sprite *s, int cx, int cy, uint32_t tint, int amt) {
    if (!s->ok) return;
    if (amt <= 0) { spr_blit_c(s, cx, cy); return; }
    if (amt > 255) amt = 255;
    int tr = (int)((tint >> 16) & 0xFF), tg = (int)((tint >> 8) & 0xFF), tb = (int)(tint & 0xFF);
    int dx = cx - s->w / 2, dy = cy - s->h / 2, ia = 255 - amt;
    for (int y = 0; y < s->h; y++) {
        int py = dy + y; if ((unsigned)py >= (unsigned)H) continue;
        const uint32_t *src = s->px + y * s->w; uint32_t *drow = g_blit + py * W;
        for (int x = 0; x < s->w; x++) {
            uint32_t p = src[x]; if (!(p & 0xFF000000u)) continue;
            int px = dx + x; if ((unsigned)px >= (unsigned)W) continue;
            int sr = (int)((p >> 16) & 0xFF), sg = (int)((p >> 8) & 0xFF), sb = (int)(p & 0xFF);
            int r = (sr * tr / 255 * amt + sr * ia) / 255;
            int g = (sg * tg / 255 * amt + sg * ia) / 255;
            int b = (sb * tb / 255 * amt + sb * ia) / 255;
            drow[px] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}
static void spr_blit_scaled_c(const Sprite *s, int cx, int cy, int dw, int dh) {
    if (!s->ok || dw <= 0 || dh <= 0) return; int x0 = cx - dw / 2, y0 = cy - dh / 2;
    for (int y = 0; y < dh; y++) { int py = y0 + y; if ((unsigned)py >= (unsigned)H) continue;
        int sy = y * s->h / dh; const uint32_t *src = s->px + sy * s->w; uint32_t *drow = g_blit + py * W;
        for (int x = 0; x < dw; x++) { uint32_t p = src[x * s->w / dw]; if (!(p & 0xFF000000u)) continue;
            int px = x0 + x; if ((unsigned)px >= (unsigned)W) continue; drow[px] = p & 0x00FFFFFFu; } }
}
/* #476 Part B: scale a sprite DOWN (never up) so it fits within max_w x max_h,
 * preserving aspect, then centre-blit it. Used for the AI text/art sprites so
 * they never overflow the narrow portrait playfield (W ranges 380-620, see
 * compute_layout) regardless of the source art's native resolution. */
static void spr_blit_fit_c(const Sprite *s, int cx, int cy, int max_w, int max_h) {
    if (!s->ok || max_w <= 0 || max_h <= 0 || s->w <= 0 || s->h <= 0) return;
    float sw = (float)max_w / (float)s->w, sh = (float)max_h / (float)s->h;
    float sc = sw < sh ? sw : sh; if (sc > 1.0f) sc = 1.0f;
    int ow = (int)(s->w * sc), oh = (int)(s->h * sc);
    if (ow < 1) ow = 1; if (oh < 1) oh = 1;
    spr_blit_scaled_c(s, cx, cy, ow, oh);
}
static void spr_blit_frame_c(const Sprite *sheet, int fw, int fh, int cols, int idx, int cx, int cy) {
    if (!sheet->ok) return; int fx = (idx % cols) * fw, fy = (idx / cols) * fh; int x0 = cx - fw / 2, y0 = cy - fh / 2;
    for (int y = 0; y < fh; y++) { int py = y0 + y; if ((unsigned)py >= (unsigned)H) continue; int sy = fy + y; if (sy >= sheet->h) continue;
        const uint32_t *src = sheet->px + sy * sheet->w; uint32_t *drow = g_blit + py * W;
        for (int x = 0; x < fw; x++) { int sx = fx + x; if (sx >= sheet->w) continue; uint32_t p = src[sx]; if (!(p & 0xFF000000u)) continue;
            if (((p >> 16) & 0xFF) < 24 && ((p >> 8) & 0xFF) < 24 && (p & 0xFF) < 24) continue;
            int px = x0 + x; if ((unsigned)px >= (unsigned)W) continue; drow[px] = p & 0x00FFFFFFu; } }
}
/* Vertical-scroll blit of an opaque bg, stretched to fill W and scrolled by `scroll`.
 * #562: no longer called (see draw_background) - kept for a possible future
 * authored-pixel-art L<set>BGx.BMP pass. */
__attribute__((unused)) static void bg_fill_scroll(const Sprite *s, int scroll) {
    if (!s->ok) return; int ih = s->h;
    int base = ((scroll % ih) + ih) % ih;
    for (int y = 0; y < H; y++) { int sy = (base + y) % ih; const uint32_t *src = s->px + sy * s->w; uint32_t *drow = g_blit + y * W;
        for (int x = 0; x < W; x++) drow[x] = src[x * s->w / W] & 0x00FFFFFFu; }
}

/* ============================================================== assets ==== */
static Sprite A_logo;
static Sprite A_player, A_enemy[4], A_boss, A_pbullet, A_ebullet, A_explode;
static Sprite A_sidebarL, A_sidebarR;   /* left/right portrait HUD console art */
static Sprite A_bgset[5];   /* the current level's 5 backgrounds */
static int    A_bgset_level = -1;

/* #476 Part B: real OpenAI (gpt-image-1) sprite art, generated with a
 * transparent background then flattened onto the game's magenta (#FF00FF)
 * colour-key so bmp_load()'s existing colour-key path loads them exactly
 * like every other sprite. Each has a code-drawn procedural fallback (see
 * bgobj_draw / draw_gameover) so a missing asset file can never crash or
 * blank the game - only degrade it back to the prior look. */
static Sprite A_bg_planet, A_bg_ringed, A_bg_asteroid, A_bg_nebula;   /* far parallax layer */
static Sprite A_txt_gameover, A_txt_finalscore, A_txt_reachedstage, A_txt_tryagain, A_txt_escmenu;

static void assets_load(void) {
    bmp_load("/SQUADRON/LOGO.BMP", &A_logo);
    bmp_load("/SQUADRON/SIDEBARL.BMP", &A_sidebarL);
    bmp_load("/SQUADRON/SIDEBARR.BMP", &A_sidebarR);
    bmp_load("/SQUADRON/PLAYER.BMP",  &A_player);
    bmp_load("/SQUADRON/ENEMY1.BMP",  &A_enemy[0]);
    bmp_load("/SQUADRON/ENEMY2.BMP",  &A_enemy[1]);
    bmp_load("/SQUADRON/ENEMY3.BMP",  &A_enemy[2]);
    bmp_load("/SQUADRON/ENEMY4.BMP",  &A_enemy[3]);
    bmp_load("/SQUADRON/BOSS.BMP",    &A_boss);
    bmp_load("/SQUADRON/PBULLET.BMP", &A_pbullet);
    bmp_load("/SQUADRON/EBULLET.BMP", &A_ebullet);
    bmp_load("/SQUADRON/EXPLODE.BMP", &A_explode);
    bmp_load("/SQUADRON/BG_PLANET.BMP",       &A_bg_planet);
    bmp_load("/SQUADRON/BG_RINGED.BMP",       &A_bg_ringed);
    bmp_load("/SQUADRON/BG_ASTEROID.BMP",     &A_bg_asteroid);
    bmp_load("/SQUADRON/BG_NEBULA.BMP",       &A_bg_nebula);
    bmp_load("/SQUADRON/TXT_GAMEOVER.BMP",       &A_txt_gameover);
    bmp_load("/SQUADRON/TXT_FINALSCORE.BMP",     &A_txt_finalscore);
    bmp_load("/SQUADRON/TXT_REACHEDSTAGE.BMP",   &A_txt_reachedstage);
    bmp_load("/SQUADRON/TXT_TRYAGAIN.BMP",       &A_txt_tryagain);
    bmp_load("/SQUADRON/TXT_ESCMENU.BMP",        &A_txt_escmenu);
}
/* Load ONE background for the stage into A_bgset[0].
 * The previous single-scrolling-background system read cleaner than the 5-way
 * vertical crossfade, and decoding five full-screen BMPs at every stage change
 * was the long inter-level freeze on the iMac (5 sequential blocking disk reads
 * + decodes). We keep ALL the new art: which of the 5 backgrounds (across the 3
 * art sets) shows is derived from the stage, so the scenery still changes as you
 * progress, but only ONE BMP is decoded per stage - 5x less load time and I/O.
 * #562: no longer called - draw_background() now always uses the procedural
 * pxbg_draw() instead (see its comment), so nothing decodes L<set>BGx.BMP
 * today. Kept, unused, for a possible future authored-pixel-art pass. */
__attribute__((unused)) static void bgset_load(int level) {
    if (level == A_bgset_level) return;
    int set = ((level - 1) % 3) + 1;      /* L1../L2../L3.. art set        */
    int idx = ((level - 1) % 5) + 1;      /* which of the 5 within the set */
    char path[40]; char n[4];
    strcpy(path, "/SQUADRON/L"); n[0] = (char)('0' + set); n[1] = 0; strcat(path, n);
    strcat(path, "BG"); n[0] = (char)('0' + idx); n[1] = 0; strcat(path, n); strcat(path, ".BMP");
    bmp_load(path, &A_bgset[0]);
    A_bgset_level = level;
}

/* ============================================================= entities === */
#define MAX_PB   96
#define MAX_EB   360
#define MAX_EN   56
#define MAX_PU   12
#define MAX_EX   48
#define MAX_STARS 140
#define MAX_CLUSTERS       5
#define STARS_PER_CLUSTER  7
#define MAX_CLUSTSTARS     (MAX_CLUSTERS * STARS_PER_CLUSTER)

/* bullet kinds (shared struct for player + enemy). */
enum { BK_STRAIGHT, BK_AIMED, BK_WAVE, BK_PLASMA, BK_MISSILE,   /* enemy */
       PK_SHOT, PK_LASER, PK_MISSILE, PK_WAVE };                /* player */
typedef struct { int alive, kind; float x, y, vx, vy; float t; int life; } Bullet;

/* enemy archetypes */
enum { ET_DRONE, ET_WEAVER, ET_DIVER, ET_STRAFER, ET_GUNSHIP, ET_TANK,
       ET_KAMIKAZE, ET_MINELAYER, ET_TURRET, ET_SILO, ET_STATION };
/* movement patterns */
enum { PT_STRAIGHT, PT_SINE, PT_SCURVE, PT_FIG8, PT_DIVE, PT_SWEEP, PT_CIRCLE, PT_HOVER, PT_STATIONARY };

/* #it15: boss archetypes. Until this iteration EVERY stage's boss was the
 * same single entity: descend, sine-sweep across the top, three fixed attack
 * phases. Stage 1's boss and stage 30's boss differed only in HP and bullet
 * speed, so the climax of every stage in the game played identically, which
 * is the one place where sameness is most obvious because it is where the
 * player is paying the most attention. Three archetypes now cycle by stage. */
enum { BOSS_SWEEPER, BOSS_FORTRESS, BOSS_INTERCEPTOR, BOSS_KINDS };
static const char *boss_name(int k) {
    return k == BOSS_FORTRESS ? "FORTRESS" : k == BOSS_INTERCEPTOR ? "INTERCEPTOR" : "SWEEPER";
}
typedef struct {
    int alive, type, boss, pattern, stationary;
    float x, y, vx, vy, t, basex, basey, amp;
    int   hp, maxhp, fire_ms, score, boss_phase;
    int   boss_kind;      /* #it15: BOSS_* , meaningful only when boss != 0 */
    unsigned col;         /* tint for fallback sprite / glow */
} Enemy;
typedef struct { int alive, kind; float x, y, vy; } Powerup;   /* kind: see PUK_ */
typedef struct { int alive; float x, y; int frame, t, big; unsigned col; } Explosion;
typedef struct { float x, y, speed; int size; uint32_t col; } Star;
typedef struct { float x, y, speed; int size; uint32_t col; } ClustStar;   /* see clust_init below */

static Bullet   g_pb[MAX_PB];
static Bullet   g_eb[MAX_EB];
static Enemy    g_en[MAX_EN];
static Powerup  g_pu[MAX_PU];
static Explosion g_ex[MAX_EX];
static Star     g_stars[MAX_STARS];
static ClustStar g_clust[MAX_CLUSTSTARS];

/* ============================================================== audio ===== *
 * #it20 (sound effects). Squadron shipped with ZERO audio and the two prior
 * passes both deferred it, correctly, because the failure mode is the one
 * this game must never have: a render loop that stalls. Everything below is
 * arranged so that CANNOT happen, and the arrangement is the feature.
 *
 * THE RULE. sys_audio_pcm_write() BLOCKS on a kernel wait queue while the DAC
 * ring is full, and sys_audio_pcm_close() blocks until the ring drains. Both
 * are therefore forbidden on the thread that draws frames. They live on a
 * dedicated worker thread here and nowhere else. The game thread's ONLY
 * interaction with audio is sfx(), which takes a mutex, writes one byte into
 * a ring, and drops it - a few instructions, no syscall, no allocation, and
 * no possibility of blocking on the audio device however badly the device is
 * behaving. If the worker never starts, or the device refuses to open, the
 * game is silent and every other line of it runs exactly as before.
 *
 * WHY THE WORKER NEEDS NO CONDITION VARIABLE, which is the non-obvious part.
 * The natural instinct is "sleep until there is a sound to play". That would
 * be wrong here: the stream is continuous, so the worker must hand the kernel
 * a block every ~23ms whether or not anything is making noise, and the
 * blocking write is EXACTLY that pacing. The loop is drain, render, write,
 * and the write puts the thread to sleep on the kernel's own wait queue until
 * the pump has consumed frames. There is no sleep(), no yield, and no poll
 * anywhere in it - the shared blocking primitive does all the waiting, which
 * is what this codebase's concurrency rule asks for. Adding a condvar would
 * make it worse, not better: it would let the stream underrun.
 *
 * THE ZERO-RETURN TRAP. A write that returns <= 0 means the sink is gone. It
 * must END the loop, not be retried, because retrying a stopped stream is an
 * unpaced spin that burns a core (the #fmzombie bug: 89% of a core). So the
 * test is `if (w <= 0) break;`, never `if (w < 0)`.
 *
 * OWNERSHIP. The PCM stream is owned by the thread GROUP, not the thread, so
 * opening and writing from this worker is legal, and the kernel reclaims the
 * slot if the process exits without closing. The device is a 4-slot mixer,
 * not the single exclusive slot older notes describe, so holding one stream
 * for the app's lifetime does not mute the rest of the machine.
 *
 * SYNTHESIS. Every effect is generated from a formula at 22050 Hz mono: no
 * WAV files, so no new assets to ship, no disk I/O on any path, and nothing
 * that can be missing at runtime. 8 voices mix into an int32 accumulator and
 * clamp once, so overlapping explosions cannot wrap around into a click. */
#define SND_RATE    22050
#define SND_CH      1
#define SND_FMT     0x0002          /* AUDIO_FORMAT_S16_LE, the only one accepted */
#define SND_FRAMES  512             /* 23.2 ms per block, ~43 blocks/sec */
#define SND_VOICES  8
#define SFX_Q_LEN   32

enum { SFX_SHOT, SFX_EXPLODE, SFX_BOSS_DIE, SFX_POWERUP, SFX_PLAYER_HIT,
       SFX_BOMB, SFX_PHASE, SFX_STAGE, SFX_KINDS };

/* Length of each effect in samples, and its peak amplitude (0..32767). SHOT is
 * deliberately the quietest thing in the game: it fires up to ~7 times a
 * second for the whole run, so anything louder becomes the only sound you
 * hear. BOSS_DIE is the loudest and longest, because it happens once a stage. */
static const unsigned g_sfx_len[SFX_KINDS] = { 1000, 7000, 22000, 5200, 9900, 15000, 8800, 13000 };
static const int      g_sfx_amp[SFX_KINDS] = { 2600, 9000, 15000, 6800,  9500, 12000,  7000,  6000 };

typedef struct { int kind, active; unsigned pos; } SndVoice;
static SndVoice      g_voice[SND_VOICES];
static short         g_sndbuf[SND_FRAMES];        /* .bss: the Ring-3 stack is 16 KB */
static unsigned char g_sfx_q[SFX_Q_LEN];
static unsigned      g_sfx_head = 0, g_sfx_tail = 0;
static pthread_mutex_t g_sfx_mtx = PTHREAD_MUTEX_INITIALIZER;
static int           g_snd_started = 0;           /* worker thread is alive          */
static int           g_snd_on = 1;                /* player-facing mute (title menu)  */
static int           g_dbg_nosound = 0;           /* --nosound: never start the worker */
static volatile unsigned g_snd_blocks = 0;        /* health counter, read by fps_tick */
static volatile int  g_snd_quit = 0;
static uint32_t      g_snd_rng = 0x9E3779B9u;

/* Called from the GAME thread. The only audio call the game thread ever
 * makes. Drops the event rather than blocking if the ring is full: a lost
 * blip is nothing, a stalled frame is the failure this whole design exists
 * to prevent. */
static void sfx(int kind) {
    if (!g_snd_started || !g_snd_on) return;
    if (kind < 0 || kind >= SFX_KINDS) return;
    pthread_mutex_lock(&g_sfx_mtx);
    if (g_sfx_head - g_sfx_tail < SFX_Q_LEN) { g_sfx_q[g_sfx_head % SFX_Q_LEN] = (unsigned char)kind; g_sfx_head++; }
    pthread_mutex_unlock(&g_sfx_mtx);
}

static uint32_t snd_rnd(void) { g_snd_rng ^= g_snd_rng << 13; g_snd_rng ^= g_snd_rng >> 17; g_snd_rng ^= g_snd_rng << 5; return g_snd_rng; }
/* A square wave from a running sample index, given a frequency in Hz. Integer
 * phase, so no accumulating float drift over a long-held note. */
static int snd_sq(unsigned pos, int hz) { if (hz < 1) hz = 1; unsigned period = (unsigned)SND_RATE / (unsigned)hz; if (!period) period = 1; return (pos % period) * 2 < period ? 1 : -1; }
static int snd_saw(unsigned pos, int hz) { if (hz < 1) hz = 1; unsigned period = (unsigned)SND_RATE / (unsigned)hz; if (!period) period = 1; return (int)((pos % period) * 2000 / period) - 1000; }

/* One voice's sample at its current position. Pure function of (kind, pos). */
static int snd_sample(int kind, unsigned pos) {
    unsigned len = g_sfx_len[kind]; if (pos >= len) return 0;
    int amp = g_sfx_amp[kind];
    int rem = (int)(len - pos), dec = (int)((long)rem * 1024 / (long)len);   /* 1024 -> 0 linear */
    switch (kind) {
        case SFX_SHOT: {            /* short descending square blip */
            int hz = 1500 - (int)(pos * 900 / len);
            return snd_sq(pos, hz) * amp * dec / 1024; }
        case SFX_EXPLODE: {         /* noise with a squared decay + a low thump */
            int n = (int)(snd_rnd() >> 16) - 32768;
            int env = dec * dec / 1024;
            return (n / 4 * env / 1024) * amp / 8192 + snd_sq(pos, 70) * amp / 6 * env / 1024; }
        case SFX_BOSS_DIE: {        /* the biggest sound in the game: noise + rumble */
            int n = (int)(snd_rnd() >> 16) - 32768;
            int env = dec * dec / 1024;
            return (n / 4 * env / 1024) * amp / 8192
                 + snd_saw(pos, 38 + (int)(pos / 900)) * amp / 900 * env / 1024; }
        case SFX_POWERUP: {         /* three-step ascending arpeggio */
            int step = (int)(pos * 3 / len);
            static const int hz[3] = { 520, 700, 950 };
            return snd_sq(pos, hz[step > 2 ? 2 : step]) * amp * dec / 1024; }
        case SFX_PLAYER_HIT: {      /* long falling saw: unmistakably bad news */
            int hz = 900 - (int)(pos * 780 / len);
            return snd_saw(pos, hz) * amp / 1000 * dec / 1024; }
        case SFX_BOMB: {            /* noise sweep over a sub-bass drop */
            int n = (int)(snd_rnd() >> 16) - 32768;
            int env = dec * dec / 1024;
            return (n / 6 * env / 1024) * amp / 8192 + snd_sq(pos, 120 - (int)(pos * 90 / len)) * amp / 5 * env / 1024; }
        case SFX_PHASE: {           /* two-tone alarm, alternating 4 times */
            int half = (int)(pos * 8 / len) & 1;
            return snd_sq(pos, half ? 660 : 440) * amp * dec / 1024; }
        default: {                  /* SFX_STAGE: a rising four-note fanfare */
            int step = (int)(pos * 4 / len);
            static const int hz[4] = { 392, 494, 587, 784 };
            return snd_sq(pos, hz[step > 3 ? 3 : step]) * amp * (768 + dec / 4) / 1024; }
    }
}

static void snd_render(short *out, int frames) {
    for (int i = 0; i < frames; i++) {
        int acc = 0;
        for (int v = 0; v < SND_VOICES; v++) {
            SndVoice *sv = &g_voice[v]; if (!sv->active) continue;
            if (sv->pos >= g_sfx_len[sv->kind]) { sv->active = 0; continue; }
            acc += snd_sample(sv->kind, sv->pos); sv->pos++;
        }
        if (acc >  32767) acc =  32767;
        if (acc < -32768) acc = -32768;
        out[i] = (short)acc;
    }
}

/* Take everything the game queued since the last block and start a voice for
 * each. Held under the mutex for the copy only; starting the voices happens
 * after the unlock so the game thread is never waiting on this loop. */
static void snd_drain(void) {
    unsigned char local[SFX_Q_LEN]; int n = 0;
    pthread_mutex_lock(&g_sfx_mtx);
    while (g_sfx_tail != g_sfx_head && n < SFX_Q_LEN) { local[n++] = g_sfx_q[g_sfx_tail % SFX_Q_LEN]; g_sfx_tail++; }
    pthread_mutex_unlock(&g_sfx_mtx);
    for (int i = 0; i < n; i++) {
        int k = local[i], slot = -1;
        for (int v = 0; v < SND_VOICES; v++) if (!g_voice[v].active) { slot = v; break; }
        if (slot < 0) {   /* all busy: steal the one closest to finishing */
            unsigned best = 0;
            for (int v = 0; v < SND_VOICES; v++) {
                unsigned len = g_sfx_len[g_voice[v].kind];
                unsigned prog = len ? g_voice[v].pos * 1024 / len : 1024;
                if (prog >= best) { best = prog; slot = v; }
            }
        }
        if (slot < 0) slot = 0;
        g_voice[slot].kind = k; g_voice[slot].pos = 0; g_voice[slot].active = 1;
    }
}

static void *snd_worker(void *arg) {
    (void)arg;
    int h = sys_audio_pcm_open(SND_RATE, SND_CH, SND_FMT);
    if (h < 1) { g_snd_started = 0; return 0; }   /* -2 busy, -6 no device: stay silent */
    while (!g_snd_quit) {
        snd_drain();
        if (g_snd_on) snd_render(g_sndbuf, SND_FRAMES);
        else { for (int i = 0; i < SND_FRAMES; i++) g_sndbuf[i] = 0;
               for (int v = 0; v < SND_VOICES; v++) g_voice[v].active = 0; }
        unsigned sent = 0;
        while (sent < SND_FRAMES) {
            int w = sys_audio_pcm_write(h, g_sndbuf + sent, SND_FRAMES - sent);
            if (w <= 0) { g_snd_quit = 1; break; }   /* sink gone: LEAVE, never spin */
            sent += (unsigned)w;
        }
        g_snd_blocks++;
    }
    sys_audio_pcm_close(h);
    return 0;
}

static void snd_init(void) {
    if (g_dbg_nosound) return;
    /* The device-present check takes no handle, so unlike an open/close probe
     * it cannot race the kernel's own stream teardown. */
    if (sys_audio_pcm_ctl(0, AUDIO_PCM_CTL_AVAIL, 0, 0) != 1) return;
    pthread_t th;
    g_snd_started = 1;
    if (pthread_create(&th, 0, snd_worker, 0) != 0) g_snd_started = 0;
    else pthread_detach(th);
}

/* ============================================================ game state == */
enum { GS_MENU, GS_PLAYING, GS_PAUSED, GS_STAGECLEAR, GS_GAMEOVER };
static int g_state = GS_MENU;

/* #it10: debug/verification-only entry points. These exist to make boss
 * fights, high stages and no-death play SESSIONS reachable in seconds by a
 * scripted test harness instead of costing several minutes of real dodging
 * per attempt (see docs/SQUADRON_ITERATIONS.md "Deferred / next up" #1/#2).
 * They are parsed ONLY from argv in main() below, never from anything the
 * real Start-menu / desktop-icon launch path can supply: launch_app() in
 * userland/apps/compositor/main.c spawns every app with sys_spawn(path),
 * which passes argv = {path} and nothing else (confirmed by reading that
 * function, not assumed). A player can only reach these by typing flags at
 * a terminal ("squadron --debug-boss --debug-stage=N --debug-god"), which
 * is a developer/tester action, not the shipping play path. Default state
 * (all zero/off) is byte-for-byte how the game already behaved before this
 * iteration, so a normal launch is provably unaffected. */
static int g_dbg_stage = 0;   /* >0: new_game() starts directly on this stage */
static int g_dbg_boss  = 0;   /* 1: skip straight to that stage's boss fight */
static int g_dbg_god   = 0;   /* 1: player_hit() is a permanent no-op */
/* #it10b (2026-09-26, harness extension, NOT a gameplay iteration): start with
 * N lives instead of 3. Added to close the ONE verification item iteration 10's
 * own harness could not reach: the perfect-clear WITHHOLD branch needs a run
 * that genuinely takes a hit through player_hit() (so g_stage_hit_taken is set
 * by the real code path, not forced) and STILL clears the stage. --debug-god
 * makes that impossible by construction, and a normal 3-life parked run dies
 * long before a 200 HP boss falls. A large life pool is the smallest change
 * that makes the real path reachable: every hit still runs player_hit() in
 * full. Same argv-only reachability argument as the other three flags. */
static int g_dbg_lives = 0;   /* >0: new_game() starts with this many lives */
/* #it10c (harness extension, NOT a gameplay iteration): override the spawned
 * boss's HP. Added after a first attempt at the withhold proof failed for a
 * reason worth recording: a scripted ship parked at the bottom centre lands
 * only a fraction of its shots on a boss that sweeps the full width, so 220
 * scripted SPACE taps plus two bombs left a 200 HP stage-1 boss alive. The
 * arithmetic the withhold test actually checks (does the boss kill award
 * e->score * combo_mult() and nothing else?) does not depend on the boss's
 * HP at all, only on e->score, which this does NOT touch. So a low-HP boss
 * is the honest way to make the branch reachable, rather than faking the
 * outcome or giving up on it a third time. */
static int g_dbg_bosshp = 0;  /* >0: spawn_boss() uses this HP instead */
/* #it10d (harness extension, NOT a gameplay iteration): make every kill drop
 * a powerup. Iteration 12 changed set_weapon() so that POWER climbs on ANY
 * weapon pickup rather than only on a duplicate, and the only way to see that
 * is to actually collect several powerups - which at the shipping 16% drop
 * rate a scripted, non-moving ship does perhaps once a minute. This raises
 * the DROP RATE only. It does not touch set_weapon(), apply_powerup(), the
 * level cap, or which kind is rolled, so the behaviour under test runs
 * exactly as it ships. */
static int g_dbg_exitafter = 0;   /* #sqearlyexit harness: self-exit after N ms */
static int g_dbg_drops = 0;

/* player weapons */
enum { WP_SINGLE, WP_TWIN, WP_TRI, WP_WIDE, WP_LASER, WP_MISSILE, WP_WAVE, WP_COUNT };
/* powerup kinds */
/* #it1 (2026-09, iteration 1 of the Squadron polish pass): PUK_TRI added.
 * WP_TRI ("TRI" 3-way spread, see player_fire()/WP_ enum and the WEAPON HUD
 * box) was fully implemented but had NO powerup that ever granted it: this
 * enum ran RAPID/TWIN/WIDE/LASER/MISSILE/WAVE/SHIELD/BOMB/LIFE, so
 * maybe_drop_powerup()'s `rnd() % PUK_COUNT` could never roll it. A player
 * could reach every weapon EXCEPT the one whose HUD abbreviation ("TRI") and
 * fire pattern already existed in the shipped binary - dead content, not a
 * design choice. Fixing it is one enum entry plus its 3 switch arms below. */
enum { PUK_RAPID, PUK_TWIN, PUK_TRI, PUK_WIDE, PUK_LASER, PUK_MISSILE, PUK_WAVE, PUK_SHIELD, PUK_BOMB, PUK_LIFE, PUK_COUNT };

static float g_px, g_py;
static int   g_lives, g_stage, g_wave;
static long  g_score;
static int   g_weapon;            /* WP_* */
static int   g_wlevel;            /* 1..3 power level within weapon */
static int   g_bombs;
static unsigned g_rapid_until, g_shield_until, g_invuln_until, g_fire_cd, g_wave_delay;
/* #it17 (respawn fly-in): losing a life used to be a single frame of
 * bookkeeping. player_hit() spawned one explosion and TELEPORTED the ship to
 * (W/2, H-90) in the same instant, so the player's eye lost track of their
 * own ship at exactly the moment they most needed to find it again, and the
 * only cue that anything had happened was a number in a sidebar box changing.
 * Death is the most important event in the game and it had less presentation
 * than picking up a powerup. Now the ship flies in from below the bottom edge
 * over RESPAWN_MS with input locked, which does three things at once: it
 * gives the death a beat, it puts the ship back under the player's eye by
 * moving it there instead of blinking it there, and the input lock stops the
 * held fire button from immediately dumping a volley the player did not
 * choose to fire. Also used for the very first spawn of a game/stage, so
 * every entrance in the game is the same entrance. */
#define RESPAWN_MS 750
static unsigned g_respawn_until = 0;
static void begin_respawn(void) { g_respawn_until = g_now + RESPAWN_MS; }
static int      g_boss_active;
static unsigned g_stageclear_until, g_banner_until, g_pu_label_until;
static const char *g_pu_label = 0;
static int      g_difficulty = 1;
#define MENU_ITEMS 4   /* #it20: PLAY / DIFFICULTY / SOUND / QUIT */
static int      g_menu_sel = 0;
/* #it18 (arcade continue): GAME OVER used to be a dead end with exactly one
 * exit. SPACE restarted from stage 1 with the score wiped, so a player who
 * reached stage 12 and lost their last life had no route back to stage 12
 * except another twenty minutes of stages they had already beaten. That is
 * the single worst thing a long-run game can do to someone who was enjoying
 * it, and it is the reason arcade machines have had a CONTINUE prompt since
 * the early eighties. Three continues per game: each one restarts the stage
 * you died on with a fresh 3 lives and your score intact, but costs you the
 * weapon ladder (back to WP_SINGLE LV1, exactly as a normal death does) and
 * one of the three. When they are gone the only option left is a new game,
 * so a run still ends - it just ends because you ran out of chances, not
 * because you ran out of patience. */
#define MAX_CONTINUES 3
static int g_continues = MAX_CONTINUES;
static int g_go_sel = 0;        /* 0 = CONTINUE, 1 = NEW GAME */
static int      g_hit_flash;
static int      g_bomb_flash;
static float    g_bgpos;          /* background crossfade progress (px) */

/* HUD stats (#475 side-panel boxes): tracked cheaply at the event that
 * produces them, not recomputed. g_highscore is IN-SESSION only (resets on
 * app restart, not persisted to disk) to keep this scoped. */
static long  g_kills = 0;             /* enemies destroyed this game        */
static long  g_shots_fired = 0;       /* fire volleys (not pellets)         */
static long  g_shots_hit = 0;         /* bullet-enemy collision events      */
static int   g_combo = 0;             /* consecutive kills since last hit   */
/* #it16 (combo decay + a deeper ceiling): the multiplier used to be a pure
 * one-way ratchet. g_combo only ever went UP, and the only thing that reset it
 * was taking a hit, so once a player had banked 32 kills they held x5 for the
 * rest of the run no matter how they played: standing in a corner plinking
 * one enemy a minute scored exactly the same per kill as clearing a wave in
 * four seconds. A multiplier that cannot be lost is not a risk/reward
 * mechanic, it is a milestone. Two changes make it one:
 *  - it DECAYS. If no kill lands within the window the combo drops by one
 *    TIER (to the floor of the tier below), not to zero. A tier floor rather
 *    than a wipe is deliberate: a full reset for one slow second is punishing
 *    in a way that makes players stop trying, while a single step down is a
 *    visible, recoverable nudge to keep pressing forward.
 *  - the ceiling rises from x5 to x8, so there is somewhere worth pressing
 *    forward TO. Reaching x8 needs 56 kills held under decay pressure, which
 *    only sustained aggressive play can do; the old x5 at 32 kills stays on
 *    the way there, so nothing a player could reach before got harder.
 * The window tightens as the tier climbs, so holding the top tier is strictly
 * harder than holding the bottom one. Taking a hit still wipes the whole
 * combo (player_hit), which remains by far the biggest penalty. */
static unsigned g_combo_until = 0;
#define COMBO_TIER 8
static unsigned combo_window(void) {
    /* SIGNED arithmetic on purpose. Written unsigned first, this read
     * `3800 - tier * 180` and g_combo has no upper bound: it keeps counting
     * past the x8 cap, so at tier 22 (176 kills in one unbroken streak) the
     * subtraction WRAPPED to about 4.29 billion ms, the `< 2200` floor could
     * never catch a number that large, and the combo would have stopped
     * decaying entirely - the exact bug this iteration exists to remove,
     * reintroduced for the best players only. Caught by review, not by the
     * compiler, which is why the arithmetic is signed and clamped here. */
    int tier = g_combo / COMBO_TIER;
    int w = 3800 - tier * 180;
    if (w < 2200) w = 2200;
    return (unsigned)w;
}
/* #it7 (iteration 7, streak popup): combo_mult() already steps up every 8
 * kills (g_combo/8, capped at x5), and the MULTIPLIER HUD box already shows
 * the current value continuously - but nothing marks the MOMENT it steps up,
 * so a player racking up a streak gets no feedback beyond a HUD number they
 * are not staring at mid-dodge. A one-shot center-screen pop at each step
 * fixes that cheaply: it reuses g_combo, which enemy_killed() already
 * increments, so it costs one modulo check per kill. */
static unsigned g_streak_until = 0;
static char     g_streak_txt[24] = "";
static long  g_highscore = 0;         /* max g_score seen this app session  */
/* #it9 (iteration 9, new-high-score toast): g_hs_baseline is the high score
 * this RUN started under (0 on the very first game of the session, which
 * deliberately suppresses the toast then - beating a baseline of 0 on your
 * first kill ever is not a meaningful "new high score", just the absence of
 * one yet). Reuses the EXISTING pickup-name toast slot (g_pu_label/
 * g_pu_label_until, already rendered by draw_side_hud()'s g_toastR box) so
 * this needed no new drawing code, only the logic in add_score() below. */
static long  g_hs_baseline = 0;
static int   g_new_high_shown = 0;
static int   g_stage_spawned = 0;     /* enemies (incl boss) queued this stage */
static int   g_stage_killed = 0;      /* of those, how many are dead        */

/* #it3 (iteration 3, boss phase telegraph): tracks the boss's PREVIOUS
 * boss_phase so update_world() can detect the moment it steps up (0->1->2)
 * and fire a shake + a banner exactly once per transition, instead of every
 * frame the new phase is true. -1 means "no boss yet / just spawned", set by
 * spawn_boss() so a freshly spawned boss's initial phase 0 is never
 * mistaken for a transition. */
static int      g_last_boss_phase   = -1;
static unsigned g_phase_banner_until = 0;
static char     g_phase_banner_txt[32] = "";
/* #it15: the boss-arrival banner used to be the constant string
 * "WARNING - BOSS". Now that three archetypes cycle by stage it names the one
 * you are about to fight, which is the cheapest way to make the variety
 * legible as variety rather than as "the boss feels different sometimes".
 * 32 bytes holds "WARNING - " (10) + the longest name, "INTERCEPTOR" (11). */
static char     g_boss_name_txt[32] = "WARNING - BOSS";

/* #it4 (iteration 4, perfect-stage bonus): did the player take a real hit
 * (player_hit() past its invuln/shield guard) since this stage started?
 * Reset at new_game() and at every STAGECLEAR->PLAYING rollover; read once,
 * at the moment the stage boss dies (enemy_killed()), to award a bonus for a
 * stage cleared without taking damage. g_perfect_bonus is 0 unless the last
 * stage-clear actually earned one, and draw_stageclear() only shows the
 * "PERFECT" line when it is nonzero - never guessed from g_stage_hit_taken
 * directly, since that flag is reset again before the clear screen is drawn. */
static int   g_stage_hit_taken = 0;
static long  g_perfect_bonus = 0;

/* #it14 (stage intro card): every stage used to begin the instant the previous
 * one's clear screen timed out, announced only by the same 3-scale "STAGE N"
 * text the game has always drawn over live play while enemies were already
 * descending. So a stage transition had no beat to it, and nothing ever told
 * the player that stage 6 / 11 / 16 unlock new formations (iteration 11) or
 * that enemies keep getting faster (iteration 6) - progression the game
 * implements but never communicates. The card is a real pause: the first wave
 * is held back until it clears (see the g_wave_delay push in new_game() and at
 * the STAGECLEAR rollover), so it reads as a breather between stages rather
 * than a label pasted over an ongoing fight. */
#define INTRO_MS 2300
static unsigned g_intro_until = 0;
/* Eight sector names cycled by stage, so a long run reads as a journey rather
 * than an integer counter. Deliberately short: they are drawn at scale 2 in a
 * playfield that can be as narrow as 380px (compute_layout's floor), and the
 * longest of these is 15 chars = 240px at that scale, which fits. */
static const char *stage_sector_name(int stage) {
    static const char *n[8] = { "AURORA REACH", "VIOLET DRIFT", "EMBER SPAN",
                                "HOLLOW GATE", "CINDER BELT", "PALE HARBOUR",
                                "IRON MERIDIAN", "LAST LIGHT" };
    int s = stage - 1; if (s < 0) s = 0;
    return n[s % 8];
}
/* Only the three stages that actually unlock something return a line here, so
 * the callout means something when it appears instead of being wallpaper.
 * Kept in lockstep with formation_pool_size() by construction: same stages. */
static const char *stage_new_threat(int stage) {
    if (stage == 6)  return "NEW THREAT: PINCER";
    if (stage == 11) return "NEW THREAT: CROSSWEAVE";
    if (stage == 16) return "NEW THREAT: BASTION";
    return 0;
}

static void add_score(long amt) {
    g_score += amt;
    if (g_score > g_highscore) g_highscore = g_score;
    if (!g_new_high_shown && g_hs_baseline > 0 && g_score > g_hs_baseline) {
        g_new_high_shown = 1; g_pu_label = "NEW HIGH SCORE!"; g_pu_label_until = g_now + 1800;
    }
}
static int  combo_mult(void) { int m = 1 + g_combo / COMBO_TIER; return m > 8 ? 8 : m; }   /* #it16: ceiling x5 -> x8 */

#define WAVES_PER_STAGE 5
#define PLAYER_W 52
#define PLAYER_H 44

/* mouse + keyboard input */
static int  g_have_mouse, g_mx, g_my, g_mouse_fire;
#define KEY_GRACE_MS 90
static unsigned char g_keys[256];
static unsigned      g_key_rel[256];

/* #it2 (2026-09, iteration 2, screen shake): a single decaying shake driven
 * off wall-clock g_now, not a per-frame counter, so its feel does not change
 * with framerate. A new, stronger shake always overrides a weaker one still
 * running (screen_shake() below); it never STACKS two shakes' magnitudes,
 * which would read as chaotic rather than punchy. Applied once, at present
 * time (present_frame()), as an integer pixel offset when compositing the
 * playfield into g_present - never touches game-logic coordinates, so it
 * cannot desync hitboxes from what is drawn. */
static float    g_shake_amp   = 0.0f;   /* peak magnitude (px) of the current shake */
static unsigned g_shake_dur   = 1;      /* its total duration (ms), never 0 (div guard) */
static unsigned g_shake_until = 0;      /* g_now deadline; shake is over at/after this */
static void screen_shake(float amp, unsigned dur_ms) {
    /* keep the stronger of "what's already running" vs "what just happened" -
     * a weak hit-flash shake must not cut a boss-death shake short. */
    unsigned rem = (g_now < g_shake_until) ? (g_shake_until - g_now) : 0;
    float cur = (g_shake_dur > 0) ? g_shake_amp * (float)rem / (float)g_shake_dur : 0.0f;
    if (amp < cur) return;
    g_shake_amp = amp; g_shake_dur = dur_ms ? dur_ms : 1; g_shake_until = g_now + dur_ms;
}
/* Current shake as an integer pixel offset, decaying linearly to 0 over
 * g_shake_dur. rnd() re-picks a fresh direction every call (every frame),
 * which is what makes a shake read as a shake rather than a smooth pan. */
static void shake_offset(int *ox, int *oy) {
    *ox = 0; *oy = 0;
    if (g_now >= g_shake_until) return;
    unsigned rem = g_shake_until - g_now;
    float mag = g_shake_amp * (float)rem / (float)g_shake_dur;
    int imag = (int)(mag + 0.5f);
    if (imag < 1) return;
    *ox = (int)(rnd() % (uint32_t)(imag * 2 + 1)) - imag;
    *oy = (int)(rnd() % (uint32_t)(imag * 2 + 1)) - imag;
}

static int key_down(int k) { if (k < 0 || k > 255) return 0; if (g_keys[k]) return 1; if (g_key_rel[k] && (g_now - g_key_rel[k]) < KEY_GRACE_MS) return 1; return 0; }
static void key_apply(int k, int down) { if (k <= 0 || k > 255) return; if (down) { g_keys[k] = 1; g_key_rel[k] = 0; } else if (g_keys[k]) { g_keys[k] = 0; g_key_rel[k] = g_now ? g_now : 1; } }
static void key_set(const gui_event_t *ev, int down) { key_apply((int)(unsigned)ev->keycode, down); key_apply((int)(unsigned char)ev->key_char, down); }
static int held(int lo, int scan) { int up = (lo >= 'a' && lo <= 'z') ? lo - 32 : lo; return key_down(lo) || key_down(up) || key_down(scan); }

/* ============================================================ allocators == */
static Bullet *pb_alloc(void) { for (int i = 0; i < MAX_PB; i++) if (!g_pb[i].alive) return &g_pb[i]; return 0; }
static Bullet *eb_alloc(void) { for (int i = 0; i < MAX_EB; i++) if (!g_eb[i].alive) return &g_eb[i]; return 0; }
static Enemy  *en_alloc(void) { for (int i = 0; i < MAX_EN; i++) if (!g_en[i].alive) return &g_en[i]; return 0; }
static Powerup*pu_alloc(void) { for (int i = 0; i < MAX_PU; i++) if (!g_pu[i].alive) return &g_pu[i]; return 0; }
static Explosion *ex_alloc(void){ for (int i = 0; i < MAX_EX; i++) if (!g_ex[i].alive) return &g_ex[i]; return 0; }

static void spawn_explosion_c(float x, float y, int big, unsigned col) { Explosion *e = ex_alloc(); if (!e) return; e->alive = 1; e->x = x; e->y = y; e->frame = 0; e->t = 0; e->big = big; e->col = col; }
static void spawn_explosion(float x, float y, int big) { spawn_explosion_c(x, y, big, 0x00FFA030); }

/* ============================================================ starfield === */
/* #476: 3 explicit depth layers (far/mid/near). Speeds are deliberately kept
 * BELOW every enemy pattern's vy (enemy min ~0.4px/frame on easy TANK, up to
 * ~2.2px/frame on hard KAMIKAZE - see enemy_reset()/diff_enemy_speed()) so
 * the parallax reads correctly: background layers always drift slower than
 * the action in front of them. Brightness and pixel size scale with layer so
 * "near" stars are visibly bigger/brighter than "far" ones, on top of being
 * faster. All state lives in the existing g_stars[] array/loop; nothing here
 * is recomputed per pixel, only per star per frame (cheap, O(MAX_STARS)). */
static void stars_init(void) {
    static const float lyr_base[3]  = { 0.15f, 0.40f, 0.80f };   /* far, mid, near px/frame */
    static const float lyr_range[3] = { 0.20f, 0.30f, 0.50f };
    for (int i = 0; i < MAX_STARS; i++) {
        g_stars[i].x = (float)(rnd() % (uint32_t)(W > 0 ? W : 1024));
        g_stars[i].y = (float)(rnd() % (uint32_t)(H > 0 ? H : 768));
        int layer = i % 3;
        g_stars[i].speed = lyr_base[layer] + (float)(rnd() % 100) * 0.01f * lyr_range[layer];
        g_stars[i].size  = layer == 2 ? 2 : 1;
        int b = 70 + layer * 60;                                  /* far=dim .. near=bright */
        int hi = b + 40 > 255 ? 255 : b + 40;
        g_stars[i].col = (uint32_t)((b << 16) | (b << 8) | hi);
    }
}
static void stars_update(void) { for (int i = 0; i < MAX_STARS; i++) { g_stars[i].y += g_stars[i].speed; if (g_stars[i].y >= H) { g_stars[i].y = 0; g_stars[i].x = (float)(rnd() % (uint32_t)(W > 0 ? W : 1024)); } } }
static void stars_draw(void) { for (int i = 0; i < MAX_STARS; i++) { int s = g_stars[i].size; fill_rect((int)g_stars[i].x, (int)g_stars[i].y, s, s, g_stars[i].col); } }

/* ================================================ distant star-cluster layer
 * (Direction A, item 3): a FOURTH depth layer, further and slower than every
 * one of the 3 existing starfield layers, so it reads as a background
 * star-cluster glimpsed far behind the actual scrolling starfield rather
 * than "more stars of the same kind". Members of one cluster keep a FIXED
 * offset from each other (never re-randomised on wrap, unlike stars_update's
 * per-star respawn) so the cluster shape stays coherent as it drifts - it is
 * meant to look like one distant structure, not loose scattered points.
 * Speed (0.06px/frame) is below every stars_init() layer's minimum (0.15) so
 * the depth ordering holds. Brightness is held deliberately DIM (below the
 * existing "near" star brightness) and the layer is SPARSE (35 points total
 * across the whole screen) - gameplay readability over background prettiness
 * (see #476 depth-ordering rule and the pxbg_draw dark-backdrop rule this
 * file already documents). Drawn in draw_background() before bgobj/stars. */
static void clust_init(void) {
    int ww = W > 0 ? W : 1024, hh = H > 0 ? H : 768;
    for (int c = 0; c < MAX_CLUSTERS; c++) {
        float ccx = (float)rndrange(30, ww - 30);
        float ccy = (float)rndrange(0, hh);
        for (int i = 0; i < STARS_PER_CLUSTER; i++) {
            int k = c * STARS_PER_CLUSTER + i;
            float x = ccx + (float)rndrange(-24, 24); if (x < 1) x = 1; if (x > ww - 2) x = (float)(ww - 2);
            g_clust[k].x = x;
            g_clust[k].y = ccy + (float)rndrange(-20, 20);
            g_clust[k].speed = 0.06f;                              /* slowest of every bg layer */
            g_clust[k].size = (rnd() % 8 == 0) ? 2 : 1;            /* rare slightly-bigger core star */
            int b = 95 + rndrange(0, 55);                          /* dim, but brighter than far starfield dust */
            int hi = b + 25 > 200 ? 200 : b + 25;                  /* capped well below white: stays dim */
            g_clust[k].col = (uint32_t)((b << 16) | (b << 8) | hi);
        }
    }
}
static void clust_update(void) {
    int hh = H > 0 ? H : 768;
    for (int i = 0; i < MAX_CLUSTSTARS; i++) { g_clust[i].y += g_clust[i].speed; if (g_clust[i].y >= hh) g_clust[i].y -= hh; }
}
static void clust_draw(void) { for (int i = 0; i < MAX_CLUSTSTARS; i++) { int s = g_clust[i].size; fill_rect((int)g_clust[i].x, (int)g_clust[i].y, s, s, g_clust[i].col); } }

/* ================================================== nebula wash (cheap) === *
 * A tiny lattice-noise "cloud" field is baked ONCE at startup (neb_init) into
 * a small buffer via bilinear upsampling of a coarse random grid - there is
 * no noise function evaluated per pixel per frame. nebula_draw() only reads
 * that buffer and advances an integer scroll offset, painting it as coarse
 * low-alpha blocks (blocky is fine at this alpha; it reads as a soft drifting
 * cloud, not a texture), so the per-frame cost is a handful of blend_rect
 * calls, not a full-resolution pass. Drift speed is the slowest of every
 * background layer (#476 depth ordering). */
#define NEB_W 24
#define NEB_H 48
#define NEB_LX 6
#define NEB_LY 10
static unsigned char g_neb[NEB_H][NEB_W];
/* #A (2026-07-21, seam fix): the original interpolation spanned the lattice
 * OPEN-ENDED (row 0 .. row NEB_LY-1 across y 0..NEB_H-1), so g_neb[0] and
 * g_neb[NEB_H-1] were two INDEPENDENT random lattice corners with no relation
 * to each other. Every caller then samples this buffer with `% NEB_H` /
 * `% NEB_W` (nebula_draw, pxbg_draw) to scroll it infinitely, which stitches
 * row NEB_H-1 directly against row 0 - a hard brightness discontinuity every
 * NEB_H*block px, visible as the faint horizontal seam lines in the prototype.
 * Fix: generate the lattice as a TORUS. Both axes map the *whole* output
 * range across the *whole* lattice cycle (`y * NEB_LY / NEB_H`, no "-1"), and
 * the neighbour lattice cell wraps with modulo (`(ly + 1) % NEB_LY`) instead
 * of reading lat[ly+1] off the end. That makes lattice cell NEB_LY-1 blend
 * back into lattice cell 0 exactly as y completes one full NEB_H cycle, so
 * g_neb[NEB_H-1] -> g_neb[0] (wrap) is just another interpolated step, not a
 * seam. Same fix applied on X so horizontal tiling (NEB_W period) is seamless
 * too. This is the standard "tileable lattice noise" construction. */
static void neb_init(void) {
    unsigned char lat[NEB_LY][NEB_LX];
    for (int y = 0; y < NEB_LY; y++) for (int x = 0; x < NEB_LX; x++) lat[y][x] = (unsigned char)(rnd() & 0xFF);
    for (int y = 0; y < NEB_H; y++) {
        int ly = (y * NEB_LY) / NEB_H, ly2 = (ly + 1) % NEB_LY;
        int fy = ((y * NEB_LY * 256) / NEB_H) & 255;
        for (int x = 0; x < NEB_W; x++) {
            int lx = (x * NEB_LX) / NEB_W, lx2 = (lx + 1) % NEB_LX;
            int fx = ((x * NEB_LX * 256) / NEB_W) & 255;
            int v00 = lat[ly][lx], v10 = lat[ly][lx2], v01 = lat[ly2][lx], v11 = lat[ly2][lx2];
            int top = v00 + ((v10 - v00) * fx) / 256;
            int bot = v01 + ((v11 - v01) * fx) / 256;
            g_neb[y][x] = (unsigned char)(top + ((bot - top) * fy) / 256);
        }
    }
}
/* Second, coarser toroidal field: large soft "zones" (voids / brighter
 * nebula cores, adjacent-hue bands) sampled at PXZ_CELL granularity in
 * pxbg_draw. Same torus construction as g_neb, just fewer lattice cells over
 * a bigger on-screen block so patches read as big, slow-moving regions
 * instead of the fine cloud-shape detail g_neb provides. A different period
 * (NEBZ_W x NEBZ_H at PXZ_CELL, vs NEB_W x NEB_H at PX_CELL) keeps the two
 * fields' wrap points decorrelated so they never line up into a moire. */
#define NEBZ_W 12
#define NEBZ_H 20
#define NEBZ_LX 3
#define NEBZ_LY 5
#define PXZ_CELL 24
static unsigned char g_nebz[NEBZ_H][NEBZ_W];
static void nebz_init(void) {
    unsigned char lat[NEBZ_LY][NEBZ_LX];
    for (int y = 0; y < NEBZ_LY; y++) for (int x = 0; x < NEBZ_LX; x++) lat[y][x] = (unsigned char)(rnd() & 0xFF);
    for (int y = 0; y < NEBZ_H; y++) {
        int ly = (y * NEBZ_LY) / NEBZ_H, ly2 = (ly + 1) % NEBZ_LY;
        int fy = ((y * NEBZ_LY * 256) / NEBZ_H) & 255;
        for (int x = 0; x < NEBZ_W; x++) {
            int lx = (x * NEBZ_LX) / NEBZ_W, lx2 = (lx + 1) % NEBZ_LX;
            int fx = ((x * NEBZ_LX * 256) / NEBZ_W) & 255;
            int v00 = lat[ly][lx], v10 = lat[ly][lx2], v01 = lat[ly2][lx], v11 = lat[ly2][lx2];
            int top = v00 + ((v10 - v00) * fx) / 256;
            int bot = v01 + ((v11 - v01) * fx) / 256;
            g_nebz[y][x] = (unsigned char)(top + ((bot - top) * fy) / 256);
        }
    }
}
static void nebula_draw(int level, float scroll) {
    int set = (level - 1) % 3;
    uint32_t tint = set == 0 ? 0x00402868u : set == 1 ? 0x00103050u : 0x00501828u;
    int block = 16;
    int nscroll = (int)(scroll * 0.06f);   /* slower than every star layer and every enemy */
    for (int by = 0; by < H; by += block) {
        int ny = ((by + nscroll) / block) % NEB_H; if (ny < 0) ny += NEB_H;
        for (int bx = 0; bx < W; bx += block) {
            int nx = (bx / block) % NEB_W; if (nx < 0) nx += NEB_W;
            int v = g_neb[ny][nx];
            if (v < 130) continue;
            int a = (v - 130) * 70 / 125; if (a < 4) continue;
            blend_rect(bx, by, block, block, tint, a);
        }
    }
}

/* =============================================== far background art layer =
 * #476 Part B fallback (no OpenAI sprite art shipped, see CHANGELOG): planets
 * / a ringed planet / an asteroid / a soft nebula puff, drawn procedurally as
 * an EXTRA far parallax layer behind the stars. Per-object shape data (crater
 * offsets, cloud-puff alpha grid) is rolled ONCE at spawn and cached in the
 * struct; only x/y advance every frame, so this is O(object count), never a
 * per-pixel-per-frame random draw. Speed is picked below every star layer's
 * minimum and every enemy's minimum, so it reads as the furthest thing on
 * screen (#476 depth ordering: nebula wash < bg art < far stars < mid stars
 * < near stars < enemies < bullets). */
#define MAX_BGOBJ 3
#define BGOBJ_CRATERS 4
enum { BGO_PLANET, BGO_RINGED, BGO_ASTEROID, BGO_CLOUD };
typedef struct {
    int active, kind; float x, y, speed; int r;
    uint32_t col1, col2;                                       /* col1=lit, col2=shadow/detail */
    signed char cdx[BGOBJ_CRATERS], cdy[BGOBJ_CRATERS], crr[BGOBJ_CRATERS]; /* asteroid craters, % of r */
    unsigned char cloud[16];                                    /* nebula-puff 4x4 alpha grid */
} BgObj;
static BgObj g_bgobj[MAX_BGOBJ];
static void bgobj_spawn(BgObj *o, int fresh) {
    o->active = 1; o->kind = (int)(rnd() % 4);
    o->x = (float)rndrange(50, W - 50);
    o->y = fresh ? (float)rndrange(0, H) : (float)(-90 - rndrange(0, 160));
    o->speed = 0.12f + (float)(rnd() % 100) * 0.002f;           /* 0.12-0.32 px/frame */
    o->r = rndrange(24, 52);
    int hue = (int)(rnd() % 360);
    o->col1 = hsv(hue, 130, 150);
    o->col2 = hsv(hue, 170, 60);
    for (int i = 0; i < BGOBJ_CRATERS; i++) {
        o->cdx[i] = (signed char)rndrange(-60, 60);
        o->cdy[i] = (signed char)rndrange(-60, 60);
        o->crr[i] = (signed char)rndrange(18, 38);
    }
    for (int i = 0; i < 16; i++) o->cloud[i] = (unsigned char)(rnd() % 200);
}
static void bgobj_init(void) { for (int i = 0; i < MAX_BGOBJ; i++) bgobj_spawn(&g_bgobj[i], 1); }
static void bgobj_update(void) {
    for (int i = 0; i < MAX_BGOBJ; i++) { BgObj *o = &g_bgobj[i]; if (!o->active) continue;
        o->y += o->speed; if (o->y - o->r > H) bgobj_spawn(o, 0); }
}
static void bgobj_draw_sphere(BgObj *o) {
    int cx = (int)o->x, cy = (int)o->y, r = o->r, r2 = r * r;
    for (int y = -r; y <= r; y++) { int py = cy + y; if ((unsigned)py >= (unsigned)H) continue;
        for (int x = -r; x <= r; x++) { int d2 = x * x + y * y; if (d2 > r2) continue;
            int px = cx + x; if ((unsigned)px >= (unsigned)W) continue;
            int lit = (x + r) * 255 / (2 * r); if (lit > 255) lit = 255; if (lit < 0) lit = 0;
            put_px(px, py, blend(o->col2, o->col1, lit)); } }
}
/* #476 Part B: each kind tries its real AI sprite first (spr_blit_scaled_c,
 * sized off the object's own radius so spawn/update geometry is untouched)
 * and falls back to the original procedural draw whenever the asset is
 * missing (A_bg_*.ok == 0), so a partial or absent asset set never crashes
 * or blanks the background - it just looks like it did before this art. */
static void bgobj_draw(BgObj *o) {
    if (o->kind == BGO_PLANET) {
        if (A_bg_planet.ok) { spr_blit_scaled_c(&A_bg_planet, (int)o->x, (int)o->y, o->r * 2, o->r * 2); return; }
        bgobj_draw_sphere(o);
    } else if (o->kind == BGO_RINGED) {
        if (A_bg_ringed.ok) { spr_blit_scaled_c(&A_bg_ringed, (int)o->x, (int)o->y, o->r * 3, o->r * 2); return; }
        bgobj_draw_sphere(o);
        int cx = (int)o->x, cy = (int)o->y, r = o->r;
        for (int ang = 0; ang < 360; ang += 3) { float rad = (float)ang * 0.01745f;
            int rx = (int)((float)r * 1.55f * fcosf(rad)), ry = (int)((float)r * 0.42f * fsinf(rad));
            put_px(cx + rx, cy + ry, 0x00D8C8A0u); }
    } else if (o->kind == BGO_ASTEROID) {
        if (A_bg_asteroid.ok) { spr_blit_scaled_c(&A_bg_asteroid, (int)o->x, (int)o->y, o->r * 2, o->r * 2); return; }
        int cx = (int)o->x, cy = (int)o->y, r = o->r * 3 / 4, r2 = r * r;
        for (int y = -r; y <= r; y++) { int py = cy + y; if ((unsigned)py >= (unsigned)H) continue;
            for (int x = -r; x <= r; x++) { int d2 = x * x + y * y; if (d2 > r2) continue;
                int px = cx + x; if ((unsigned)px >= (unsigned)W) continue; put_px(px, py, o->col1); } }
        for (int i = 0; i < BGOBJ_CRATERS; i++) {
            int kcx = cx + o->cdx[i] * r / 100, kcy = cy + o->cdy[i] * r / 100, kr = o->crr[i] * r / 100;
            if (kr < 2) kr = 2; int kr2 = kr * kr;
            for (int y = -kr; y <= kr; y++) { int py = kcy + y; if ((unsigned)py >= (unsigned)H) continue;
                for (int x = -kr; x <= kr; x++) { if (x * x + y * y > kr2) continue;
                    int px = kcx + x; if ((unsigned)px >= (unsigned)W) continue; put_px(px, py, o->col2); } } }
    } else {   /* BGO_CLOUD */
        if (A_bg_nebula.ok) { int d = (int)(o->r * 2.6f); spr_blit_scaled_c(&A_bg_nebula, (int)o->x, (int)o->y, d, d); return; }
        /* soft puff, coarse precomputed 4x4 alpha grid */
        int cx = (int)o->x, cy = (int)o->y, r = o->r, block = r / 2; if (block < 6) block = 6;
        for (int gy = 0; gy < 4; gy++) for (int gx = 0; gx < 4; gx++) {
            int a = o->cloud[gy * 4 + gx] / 3; if (a < 6) continue;
            int bx = cx - 2 * block + gx * block, by = cy - 2 * block + gy * block;
            blend_rect(bx, by, block, block, o->col1, a); }
    }
}
static void bgobj_draw_all(void) { for (int i = 0; i < MAX_BGOBJ; i++) if (g_bgobj[i].active) bgobj_draw(&g_bgobj[i]); }

/* ============================================================ difficulty == */
/* #it6 (2026-09, iteration 6, per-stage difficulty escalation): before this,
 * every one of these three depended ONLY on g_difficulty (a 3-way EASY/
 * NORMAL/HARD knob picked once at the menu), never on g_stage. The only
 * per-stage scaling anywhere was enemy_reset()'s `e->hp += g_stage / 2`
 * (integer division: 0 extra HP for stages 1-2, +1 for 3-4, ...) plus
 * spawn_boss()'s HP formula - i.e. stage 40 played at EXACTLY the same enemy
 * speed and fire rate as stage 1, just with slowly spongier targets. That is
 * a flat difficulty curve wearing a bullet-sponge costume, not a game that
 * gets harder. Fix: a gentle, CAPPED per-stage multiplier layered on top of
 * the existing difficulty tiers (never replacing them), so EASY/NORMAL/HARD
 * still set the baseline but every stage still ramps from there. Capped
 * (1.6x speed / 55% of base fire interval by stage ~18-20) so late stages
 * get harder, not unfair or physically unreadable. */
static float stage_speed_mult(void) {
    float m = 1.0f + (float)(g_stage - 1) * 0.035f;
    if (m > 1.6f) m = 1.6f;
    if (m < 1.0f) m = 1.0f;
    return m;
}
static float stage_fire_mult(void) {
    float m = 1.0f - (float)(g_stage - 1) * 0.02f;
    if (m < 0.55f) m = 0.55f;
    if (m > 1.0f) m = 1.0f;
    return m;
}
static float diff_enemy_speed(void) { return (g_difficulty == 0 ? 0.8f : (g_difficulty == 2 ? 1.35f : 1.0f)) * stage_speed_mult(); }
static int   diff_fire_ms(void)     { return (int)((float)(g_difficulty == 0 ? 1600 : (g_difficulty == 2 ? 750 : 1100)) * stage_fire_mult()); }
static float diff_ebullet_speed(void){ float base = g_difficulty == 0 ? 3.0f : (g_difficulty == 2 ? 5.0f : 4.0f); return base * (1.0f + (stage_speed_mult() - 1.0f) * 0.6f); }

/* ============================================================ enemy bullets */
static Bullet *spawn_eb(float x, float y, float vx, float vy, int kind) {
    Bullet *b = eb_alloc(); if (!b) return 0;
    /* life must be long enough for a straight shot to cross the full portrait
     * height (~H); the offscreen cull (b->y > H+20) still removes them. */
    b->alive = 1; b->kind = kind; b->x = x; b->y = y; b->vx = vx; b->vy = vy; b->t = 0; b->life = 6000;
    return b;
}
static void eb_aimed(float x, float y, float sp, int kind) { float ux, uy; norm_dir(g_px - x, g_py - y, &ux, &uy); Bullet *b = spawn_eb(x, y, ux * sp, uy * sp, kind); if (b && b->vy < 0.6f) b->vy = 0.6f; }
static void eb_spread(float x, float y, int n, float spread, float sp, int kind) {
    for (int i = 0; i < n; i++) { float ux, uy; norm_dir(g_px - x, g_py - y, &ux, &uy);
        float base = 1.57079633f; float ang = base; /* fan around aim */
        float aim = 0; { float a = ux; float b = uy; (void)a; (void)b; }
        /* build angle around straight-down then rotate toward player a bit */
        ang = base + ((float)i / (float)(n > 1 ? n - 1 : 1) - 0.5f) * spread;
        Bullet *bb = spawn_eb(x, y, fcosf(ang) * sp + ux * sp * 0.35f, fsinf(ang) * sp + uy * sp * 0.15f, kind);
        if (bb && bb->vy < 0.5f) bb->vy = 0.5f; (void)aim;
    }
}
static void eb_ring(float x, float y, int n, float sp, float rot, int kind) {
    for (int i = 0; i < n; i++) { float ang = rot + (float)i * 6.2831853f / (float)n; spawn_eb(x, y, fcosf(ang) * sp, fsinf(ang) * sp, kind); }
}

/* ============================================================ spawning ==== */
static unsigned enemy_tint(int type) {
    switch (type) {
        case ET_DRONE:    return 0x00FF6060; case ET_WEAVER:  return 0x0060FF80;
        case ET_DIVER:    return 0x00FFC040; case ET_STRAFER: return 0x0040E0FF;
        case ET_GUNSHIP:  return 0x00FF80FF; case ET_TANK:    return 0x00C0C0D0;
        case ET_KAMIKAZE: return 0x00FF4020; case ET_MINELAYER: return 0x00A0FF40;
        case ET_TURRET:   return 0x00FFD040; case ET_SILO:    return 0x00FF8040;
        case ET_STATION:  return 0x00B0B0FF; default: return 0x00FF6060;
    }
}
static void enemy_reset(Enemy *e, int type, float x, float y) {
    e->alive = 1; e->boss = 0; e->type = type; e->x = x; e->y = y; e->basex = x; e->basey = y;
    e->t = 0; e->vx = 0; e->vy = 1.4f * diff_enemy_speed(); e->amp = 60.0f; e->stationary = 0;
    e->fire_ms = rndrange(diff_fire_ms() / 2, diff_fire_ms()); e->boss_phase = 0; e->col = enemy_tint(type);
    e->pattern = PT_STRAIGHT;
    switch (type) {
        case ET_DRONE:    e->hp = 2; e->score = 100; e->pattern = PT_STRAIGHT; break;
        case ET_WEAVER:   e->hp = 3; e->score = 150; e->vy = 1.1f * diff_enemy_speed(); e->pattern = PT_SINE; break;
        case ET_DIVER:    e->hp = 2; e->score = 120; e->pattern = PT_DIVE; break;
        case ET_STRAFER:  e->hp = 3; e->score = 180; e->vy = 0.7f * diff_enemy_speed(); e->pattern = PT_SWEEP; break;
        case ET_GUNSHIP:  e->hp = 6; e->score = 300; e->vy = 0.6f * diff_enemy_speed(); e->pattern = PT_HOVER; break;
        case ET_TANK:     e->hp = 12; e->score = 400; e->vy = 0.5f * diff_enemy_speed(); e->pattern = PT_STRAIGHT; break;
        case ET_KAMIKAZE: e->hp = 2; e->score = 160; e->vy = 1.6f * diff_enemy_speed(); e->pattern = PT_DIVE; break;
        case ET_MINELAYER:e->hp = 4; e->score = 220; e->vy = 0.8f * diff_enemy_speed(); e->pattern = PT_SCURVE; break;
        case ET_TURRET:   e->hp = 5; e->score = 250; e->stationary = 1; e->pattern = PT_STATIONARY; break;
        case ET_SILO:     e->hp = 7; e->score = 320; e->stationary = 1; e->pattern = PT_STATIONARY; break;
        case ET_STATION:  e->hp = 30; e->score = 900; e->stationary = 1; e->pattern = PT_STATIONARY; break;
        default:          e->hp = 2; e->score = 100; break;
    }
    e->hp += g_stage / 2; e->maxhp = e->hp;
    g_stage_spawned++;   /* feeds the left "% COMPLETE" HUD box */
}

/* #it11: the formation POOL grows with stage instead of staying fixed at 7
 * forever. Before this, `(stage*3+idx) % 7` meant stage 40 cycled through
 * EXACTLY the same 7 wave shapes as stage 5 (see docs/SQUADRON_ITERATIONS.md
 * "Deferred / next up" #5) - only the per-enemy speed/fire-rate (iteration 6)
 * changed, so a long run's wave VARIETY flattened out even as its lethality
 * kept climbing. Three new patterns (7/8/9 below) unlock progressively so a
 * long run keeps seeing new shapes, not just faster old ones. Unlock stages
 * chosen so a fresh player reaches each once before it starts repeating
 * (pool 8 by stage 6, pool 9 by stage 11, pool 10 by stage 16), matching the
 * cadence iteration 6's own escalation already ramps on. */
static int formation_pool_size(void) {
    int n = 7;
    if (g_stage >= 6)  n = 8;
    if (g_stage >= 11) n = 9;
    if (g_stage >= 16) n = 10;
    return n;
}
static void spawn_formation(int idx) {
    int pat = (g_stage * 3 + idx) % formation_pool_size();
    int extra = g_stage > 3 ? 3 : g_stage;
    if (pat == 0) { int n = 5 + extra; int gap = W / (n + 1); for (int i = 0; i < n; i++) { Enemy *e = en_alloc(); if (e) enemy_reset(e, ET_DRONE, (float)(gap * (i + 1)), (float)(-40 - i * 10)); } }
    else if (pat == 1) { int n = 6; for (int i = 0; i < n; i++) { Enemy *e = en_alloc(); if (!e) continue; float x = (float)(W / 2 + (i - n / 2) * 90); enemy_reset(e, ET_WEAVER, x, (float)(-40 - iabs(i - n / 2) * 34)); } }
    else if (pat == 2) { for (int i = 0; i < 4; i++) { Enemy *l = en_alloc(); if (l) enemy_reset(l, ET_DIVER, 90.0f, (float)(-40 - i * 70)); Enemy *r = en_alloc(); if (r) enemy_reset(r, ET_DIVER, (float)(W - 90), (float)(-40 - i * 70)); } }
    else if (pat == 3) { int n = 5; for (int i = 0; i < n; i++) { Enemy *e = en_alloc(); if (!e) continue; enemy_reset(e, ET_STRAFER, (float)(120 + i * (W - 240) / (n - 1)), (float)(-50 - i * 24)); } }
    else if (pat == 4) { Enemy *g1 = en_alloc(); if (g1) enemy_reset(g1, ET_GUNSHIP, (float)(W / 3), -60.0f); Enemy *g2 = en_alloc(); if (g2) enemy_reset(g2, ET_GUNSHIP, (float)(2 * W / 3), -110.0f); for (int i = 0; i < 3; i++) { Enemy *e = en_alloc(); if (e) enemy_reset(e, ET_KAMIKAZE, (float)(W / 4 + i * W / 4), (float)(-160 - i * 40)); } }
    else if (pat == 5) { /* station wall of stationary turrets scrolling down */ Enemy *st = en_alloc(); if (st) enemy_reset(st, ET_STATION, (float)(W / 2), -180.0f); Enemy *t1 = en_alloc(); if (t1) enemy_reset(t1, ET_TURRET, (float)(W / 4), -120.0f); Enemy *t2 = en_alloc(); if (t2) enemy_reset(t2, ET_TURRET, (float)(3 * W / 4), -120.0f); Enemy *s1 = en_alloc(); if (s1) enemy_reset(s1, ET_SILO, 120.0f, -220.0f); Enemy *s2 = en_alloc(); if (s2) enemy_reset(s2, ET_SILO, (float)(W - 120), -220.0f); }
    else if (pat == 6) { Enemy *tk = en_alloc(); if (tk) enemy_reset(tk, ET_TANK, (float)(W / 2), -70.0f); for (int i = 0; i < 4; i++) { Enemy *e = en_alloc(); if (e) enemy_reset(e, ET_MINELAYER, (float)(100 + i * (W - 200) / 3), (float)(-40 - i * 30)); } }
    /* #it11 new formations, stage-gated by formation_pool_size() above: */
    else if (pat == 7) { /* PINCER (stage>=6): two homing SILOs bracket a KAMIKAZE
         * rush down the middle - the player has to juggle a slow homing threat on
         * each flank while a fast rammer closes head-on, a combination neither
         * pattern 4 (gunship+kamikaze, no homing) nor pattern 5 (all-stationary,
         * no rush) produces alone. */
        Enemy *s1 = en_alloc(); if (s1) enemy_reset(s1, ET_SILO, 110.0f, -140.0f);
        Enemy *s2 = en_alloc(); if (s2) enemy_reset(s2, ET_SILO, (float)(W - 110), -140.0f);
        for (int i = 0; i < 3 + (extra > 0 ? 1 : 0); i++) { Enemy *e = en_alloc(); if (e) enemy_reset(e, ET_KAMIKAZE, (float)(W / 2), (float)(-40 - i * 60)); }
    }
    else if (pat == 8) { /* CROSSWEAVE (stage>=11): WEAVER and DIVER interleaved
         * across the full width, alternating types at each x slot so their two
         * different bullet kinds (BK_WAVE vs BK_AIMED) arrive from overlapping
         * lanes at once - denser and harder to read than either pure pattern 1
         * or pattern 2, which is the point at this stage. */
        int n = 7; for (int i = 0; i < n; i++) { Enemy *e = en_alloc(); if (!e) continue;
            float x = (float)(W / (n + 1) * (i + 1));
            if (i % 2 == 0) enemy_reset(e, ET_WEAVER, x, (float)(-40 - i * 26));
            else            enemy_reset(e, ET_DIVER,  x, (float)(-70 - i * 26)); }
    }
    else { /* pat == 9, BASTION (stage>=16): a STATION core flanked by two
         * TURRETs with a TANK trailing behind - the toughest regular (non-boss)
         * wave in the game, reserved for the longest runs. Combines three
         * stationary/heavy archetypes that never appear together before this
         * stage: pattern 5 already uses STATION+TURRET+SILO but never TANK, and
         * pattern 6's TANK is otherwise always escorted by MINELAYERs only. */
        Enemy *st = en_alloc(); if (st) enemy_reset(st, ET_STATION, (float)(W / 2), -200.0f);
        Enemy *t1 = en_alloc(); if (t1) enemy_reset(t1, ET_TURRET, (float)(W / 2 - 150), -140.0f);
        Enemy *t2 = en_alloc(); if (t2) enemy_reset(t2, ET_TURRET, (float)(W / 2 + 150), -140.0f);
        Enemy *tk = en_alloc(); if (tk) enemy_reset(tk, ET_TANK, (float)(W / 2), -320.0f);
    }
}

static void spawn_boss(void) {
    Enemy *e = en_alloc(); if (!e) return;
    e->alive = 1; e->boss = 1; e->type = ET_DRONE; e->stationary = 0; e->pattern = PT_SINE;
    e->x = (float)(W / 2); e->y = -140.0f; e->basex = e->x; e->basey = 110.0f;
    e->vx = 2.0f * diff_enemy_speed(); e->vy = 1.2f; e->t = 0; e->amp = (float)(W / 2 - 130);
    /* Boss HP is balanced against ACTUAL player damage output, which is low:
     * pb_damage() gives 1 per shot for the default gun and laser, 2 for wave,
     * 3 for missiles. The previous 340 + stage*130 meant a stage-1 boss took
     * 470 hits with the starting weapon, which played as a slog rather than a
     * fight. A bomb only removes 40 and is clamped so it can never finish a
     * boss, so it does not shorten this meaningfully either. */
    e->hp = 110 + g_stage * 90; e->maxhp = e->hp; e->col = 0x00FF60A0;
    e->fire_ms = 500; e->score = 3000 + g_stage * 1000; e->boss_phase = 0;
    /* #it15: pick this stage's archetype and re-tune the fields that define
     * how it FIGHTS, on top of the shared setup above. HP is scaled per
     * archetype so "tougher" and "faster" trade off against each other rather
     * than stacking: the FORTRESS is a slow wall you grind down, the
     * INTERCEPTOR is a fast duel you finish quickly, the SWEEPER is the
     * original middle ground. The stage-1 boss is deliberately still the
     * SWEEPER (kind 0), so a new player's first boss is the one this game has
     * always opened with and nothing about stage 1 changed. */
    e->boss_kind = (g_stage - 1) % BOSS_KINDS;
    switch (e->boss_kind) {
        case BOSS_FORTRESS:
            e->basey = 92.0f;                 /* sits high: it is a wall, not a duellist */
            e->amp   = (float)(W / 2 - 150) * 0.45f;
            e->hp    = e->hp * 7 / 5; e->maxhp = e->hp;      /* +40% HP */
            e->score = e->score * 6 / 5;
            e->col   = 0x0080A0FF; e->fire_ms = 700;
            break;
        case BOSS_INTERCEPTOR:
            e->basey = 158.0f;                /* comes DOWN into your half */
            e->amp   = (float)(W / 2 - 110);
            e->hp    = e->hp * 3 / 4; e->maxhp = e->hp;      /* -25% HP */
            e->col   = 0x00FFA040; e->fire_ms = 380;
            break;
        default: break;                       /* BOSS_SWEEPER: as set above */
    }
    if (g_dbg_bosshp > 0) { e->hp = g_dbg_bosshp; e->maxhp = e->hp; }   /* #it10c, harness only */
    strcpy(g_boss_name_txt, "WARNING - "); strcat(g_boss_name_txt, boss_name(e->boss_kind));
    g_boss_active = 1; g_banner_until = g_now + 2200;
    g_stage_spawned++;   /* boss counts as the final unit of "% COMPLETE" */
    g_last_boss_phase = -1;   /* #it3: -1 = "no phase change telegraphed yet" for this boss */
}

/* ============================================================ new game ==== */
static void reset_entities(void) {
    for (int i = 0; i < MAX_PB; i++) g_pb[i].alive = 0; for (int i = 0; i < MAX_EB; i++) g_eb[i].alive = 0;
    for (int i = 0; i < MAX_EN; i++) g_en[i].alive = 0; for (int i = 0; i < MAX_PU; i++) g_pu[i].alive = 0;
    for (int i = 0; i < MAX_EX; i++) g_ex[i].alive = 0;
}
static void new_game(void) {
    reset_entities();
    g_lives = 3; g_stage = 1; g_wave = 0; g_score = 0;
    g_weapon = WP_SINGLE; g_wlevel = 1; g_bombs = 2;
    g_rapid_until = 0; g_shield_until = 0;
    g_px = (float)(W / 2); g_py = (float)(H + 60); begin_respawn();   /* #it17 */
    g_invuln_until = g_now + 1500; g_fire_cd = 0; g_wave_delay = g_now + 600; g_boss_active = 0;
    g_banner_until = g_now + 2000; g_hit_flash = 0; g_bgpos = 0;
    g_kills = 0; g_shots_fired = 0; g_shots_hit = 0; g_combo = 0; g_combo_until = 0;
    g_stage_spawned = 0; g_stage_killed = 0;   /* g_highscore deliberately NOT reset: it is per-session */
    g_stage_hit_taken = 0; g_perfect_bonus = 0; g_last_boss_phase = -1; g_phase_banner_until = 0;
    g_shake_until = 0; g_hs_baseline = g_highscore; g_new_high_shown = 0;
    g_continues = MAX_CONTINUES; g_go_sel = 0;   /* #it18 */
    g_state = GS_PLAYING;
    /* #it10: debug entry points, see the g_dbg_* declarations above. Applied
     * AFTER every other field is set to its normal new-game value, so this
     * only ever overrides g_stage/g_wave and nothing else about a fresh
     * game's state. g_dbg_stage clamped to >=1 (stage 0 does not exist). */
    if (g_dbg_stage > 0) g_stage = g_dbg_stage;
    if (g_dbg_boss) g_wave = WAVES_PER_STAGE;   /* update_waves() then spawns the boss on the next tick */
    if (g_dbg_lives > 0) g_lives = g_dbg_lives; /* #it10b, harness only */
    /* #it14: the intro card replaces the old plain "STAGE N" banner, and holds
     * the first wave back until it has been read. Applied AFTER the debug
     * overrides above so the card announces the stage actually being played. */
    g_intro_until = g_now + INTRO_MS; g_banner_until = 0; sfx(SFX_STAGE);   /* #it20 */
    g_wave_delay = g_now + INTRO_MS + 300;
    g_invuln_until = g_now + INTRO_MS + 1200;
}

/* #it18: resume the run on the stage it ended on. Deliberately NOT a call to
 * new_game() with a stage override: this must keep g_score, g_highscore,
 * g_hs_baseline, g_new_high_shown and the accuracy/kill counters, because a
 * continue continues the SAME run's statistics. It resets only what a death
 * resets plus the life count, and it costs a continue. The stage intro card
 * (#it14) doubles as the "you are back in" beat, so this needs no separate
 * announcement, and the fly-in (#it17) gives the ship a real entrance. */
static void continue_game(void) {
    if (g_continues <= 0) return;
    g_continues--;
    reset_entities();
    g_lives = 3; g_bombs = 2; g_weapon = WP_SINGLE; g_wlevel = 1;
    g_wave = 0; g_boss_active = 0; g_combo = 0; g_combo_until = 0;
    g_rapid_until = 0; g_shield_until = 0; g_fire_cd = 0;
    g_stage_spawned = 0; g_stage_killed = 0; g_stage_hit_taken = 0;
    g_perfect_bonus = 0; g_last_boss_phase = -1; g_phase_banner_until = 0;
    g_shake_until = 0; g_streak_until = 0; g_hit_flash = 0; g_bomb_flash = 0;
    g_px = (float)(W / 2); g_py = (float)(H + 60); begin_respawn();
    g_intro_until = g_now + INTRO_MS; g_banner_until = 0; sfx(SFX_STAGE);   /* #it20 */
    g_wave_delay = g_now + INTRO_MS + 300;
    g_invuln_until = g_now + INTRO_MS + 1200;
    g_state = GS_PLAYING;
}

/* ============================================================ player fire == */
static void spawn_pb(float x, float y, float vx, float vy, int kind) { Bullet *b = pb_alloc(); if (!b) return; b->alive = 1; b->kind = kind; b->x = x; b->y = y; b->vx = vx; b->vy = vy; b->t = 0; b->life = 600; }
static void player_fire(void) {
    int rapid = g_now < g_rapid_until;
    unsigned cd; float top = g_py - PLAYER_H / 2;
    switch (g_weapon) {
        case WP_LASER:  cd = 60; break;
        case WP_MISSILE:cd = rapid ? 160 : 240; break;
        case WP_WAVE:   cd = rapid ? 120 : 200; break;
        default:        cd = rapid ? 80 : 150; break;
    }
    if (g_now < g_fire_cd) return;
    g_fire_cd = g_now + cd; g_shots_fired++; sfx(SFX_SHOT);   /* #it20 */
    int lv = g_wlevel;
    switch (g_weapon) {
        case WP_SINGLE: spawn_pb(g_px, top, 0, -16, PK_SHOT); if (lv >= 2) spawn_pb(g_px, top - 10, 0, -16, PK_SHOT); break;
        case WP_TWIN:   spawn_pb(g_px - 12, top, 0, -16, PK_SHOT); spawn_pb(g_px + 12, top, 0, -16, PK_SHOT); if (lv >= 3) spawn_pb(g_px, top - 8, 0, -16, PK_SHOT); break;
        /* #it12: TRI now scales with g_wlevel like every other weapon. It was
         * the ONLY weapon in the switch that ignored `lv` entirely - a flat
         * 3-way at LV1, LV2 and LV3 alike - so the POWER "LV n/3" HUD box was
         * telling a TRI player something that did nothing. That went unnoticed
         * because until iteration 1 no powerup could grant TRI at all, so the
         * dead scaling sat behind dead content. 1+2*lv pellets (3/5/7) fanning
         * out at 3.0 vx per step, with the outer pellets slightly slower so the
         * volley reads as a widening arc rather than a flat line. */
        case WP_TRI: { int n = 1 + 2 * lv;
            for (int i = 0; i < n; i++) { int k = i - n / 2;
                spawn_pb(g_px + (float)k * 7.0f, top, (float)k * 3.0f, -16.0f + (float)iabs(k) * 0.4f, PK_SHOT); } } break;
        case WP_WIDE:   { int n = 3 + lv; for (int i = 0; i < n; i++) { float a = ((float)i / (float)(n - 1) - 0.5f) * 1.1f; spawn_pb(g_px, top, fsinf(a) * 8.0f, -15.0f, PK_SHOT); } } break;
        case WP_LASER:  spawn_pb(g_px, top, 0, -22, PK_LASER); if (lv >= 2) { spawn_pb(g_px - 10, top, 0, -22, PK_LASER); spawn_pb(g_px + 10, top, 0, -22, PK_LASER); } break;
        case WP_MISSILE:spawn_pb(g_px - 16, top, -1.5f, -8, PK_MISSILE); spawn_pb(g_px + 16, top, 1.5f, -8, PK_MISSILE); if (lv >= 3) spawn_pb(g_px, top, 0, -9, PK_MISSILE); break;
        case WP_WAVE:   spawn_pb(g_px, top, 0, -12, PK_WAVE); if (lv >= 2) { spawn_pb(g_px - 8, top, 0, -12, PK_WAVE); spawn_pb(g_px + 8, top, 0, -12, PK_WAVE); } break;
    }
}
static int pb_damage(int kind) { return kind == PK_LASER ? 1 : kind == PK_MISSILE ? 3 : kind == PK_WAVE ? 2 : 1; }

static void drop_bomb(void) {
    if (g_bombs <= 0 || g_state != GS_PLAYING) return;
    g_bombs--; g_bomb_flash = 14; screen_shake(6.0f, 200);   /* #it2 */ sfx(SFX_BOMB);   /* #it20 */
    for (int i = 0; i < MAX_EB; i++) g_eb[i].alive = 0;                 /* clear enemy bullets */
    for (int i = 0; i < MAX_EN; i++) { Enemy *e = &g_en[i]; if (!e->alive) continue; if (e->boss) { e->hp -= 40; if (e->hp <= 0) e->hp = 1; } else { spawn_explosion_c(e->x, e->y, 0, e->col); e->hp -= 6; if (e->hp <= 0) { add_score(e->score); g_kills++; g_stage_killed++; e->alive = 0; } } }
}

/* ============================================================ boss fire === */
/* #it15: FORTRESS attack: a full-width wall of descending shots with a gap
 * that SLIDES sideways between volleys, so the pattern is survivable by
 * reading it and moving to where the gap will be, not by twitch-dodging.
 * `gaps` widens the safe channel on easy and narrows it as phases escalate.
 * The wall is built from the playfield width, not from the boss's own x, so
 * it stays a wall no matter where the FORTRESS has drifted to. */
static void boss_wall(Enemy *e, float sp, int gaps, int gapw) {
    const int cols = 13;
    int slot = (int)(e->t * 2.1f);
    for (int i = 0; i < cols; i++) {
        int safe = 0;
        for (int g = 0; g < gaps; g++) {
            int c = ((slot + g * 5) % cols);
            for (int k = 0; k < gapw; k++) if (i == (c + k) % cols) safe = 1;
        }
        if (safe) continue;
        float x = (float)W * ((float)i + 0.5f) / (float)cols;
        spawn_eb(x, e->y + 26.0f, 0, sp * 0.8f, BK_STRAIGHT);
    }
}
static void boss_fire(Enemy *e) {
    float sp = diff_ebullet_speed();
    int hard = g_difficulty == 2;
    if (e->boss_kind == BOSS_FORTRESS) {
        /* Slow, heavy, readable. Escalation is "the safe channel gets
         * narrower and something else fills it in", not "more of the same". */
        switch (e->boss_phase) {
            case 0: boss_wall(e, sp, 2, hard ? 2 : 3); break;
            case 1: boss_wall(e, sp, 2, 2); eb_aimed(e->x, e->y + 24, sp, BK_AIMED);
                    eb_ring(e->x, e->y, hard ? 10 : 8, sp * 0.55f, e->t, BK_PLASMA); break;
            default: boss_wall(e, sp, 1, 2);
                     for (int i = 0; i < 3; i++) eb_aimed(e->x + (i - 1) * 70, e->y + 20, sp * 0.8f, BK_MISSILE);
                     eb_ring(e->x, e->y, hard ? 14 : 11, sp * 0.65f, -e->t, BK_PLASMA); break;
        }
        return;
    }
    if (e->boss_kind == BOSS_INTERCEPTOR) {
        /* Fast, close, personal. Everything is AIMED, so standing still is
         * fatal and moving is always the answer - the opposite lesson to the
         * FORTRESS, which punishes moving without reading the gap first. */
        switch (e->boss_phase) {
            case 0: for (int i = 0; i < 3; i++) eb_aimed(e->x + (i - 1) * 26, e->y + 26, sp * 1.05f, BK_AIMED); break;
            case 1: eb_spread(e->x, e->y + 26, hard ? 7 : 5, 0.9f, sp * 1.05f, BK_AIMED);
                    eb_aimed(e->x - 60, e->y, sp * 0.9f, BK_MISSILE);
                    eb_aimed(e->x + 60, e->y, sp * 0.9f, BK_MISSILE); break;
            default: eb_spread(e->x, e->y + 26, hard ? 9 : 7, 1.3f, sp * 1.1f, BK_AIMED);
                     for (int i = 0; i < 2; i++) eb_aimed(e->x + (i * 2 - 1) * 80, e->y, sp, BK_MISSILE);
                     eb_ring(e->x, e->y, hard ? 12 : 9, sp * 0.8f, e->t * 2.0f, BK_PLASMA); break;
        }
        return;
    }
    switch (e->boss_phase) {
        case 0:  /* opening: wide aimed volley + a homing shot to keep you moving */
            eb_spread(e->x, e->y + 34, hard ? 11 : 9, 2.0f, sp * 0.95f, BK_STRAIGHT);
            eb_aimed(e->x, e->y + 20, sp * 0.9f, BK_AIMED);
            break;
        case 1:  /* pressured: counter-rotating twin plasma rings + an aimed shot */
            eb_ring(e->x, e->y + 10, hard ? 22 : 18, sp * 0.85f,  e->t * 1.3f, BK_PLASMA);
            eb_ring(e->x, e->y + 10, hard ? 14 : 11, sp * 0.60f, -e->t * 1.1f, BK_PLASMA);
            eb_aimed(e->x, e->y + 20, sp, BK_AIMED);
            break;
        case 2:  /* enraged: 4 homing missiles + spread + a ring, all at once */
            for (int i = 0; i < 4; i++) eb_aimed(e->x + (i - 2) * 55, e->y + 20, sp * 0.8f, BK_MISSILE);
            eb_spread(e->x, e->y + 30, hard ? 13 : 11, 2.4f, sp, BK_STRAIGHT);
            eb_ring(e->x, e->y + 10, hard ? 16 : 12, sp * 0.7f, e->t, BK_PLASMA);
            break;
    }
}

/* ============================================================ powerups ==== */
static const char *puk_name(int k) {
    switch (k) { case PUK_RAPID: return "RAPID FIRE"; case PUK_TWIN: return "TWIN SHOT"; case PUK_TRI: return "TRI SHOT"; case PUK_WIDE: return "WIDE SPREAD";
        case PUK_LASER: return "LASER BEAM"; case PUK_MISSILE: return "HOMING MISSILES"; case PUK_WAVE: return "WAVE BEAM";
        case PUK_SHIELD: return "SHIELD"; case PUK_BOMB: return "SMART BOMB"; case PUK_LIFE: return "EXTRA LIFE"; default: return ""; }
}
static void maybe_drop_powerup(float x, float y, int guaranteed) {
    int roll = (int)(rnd() % 100);
    if (!guaranteed && !g_dbg_drops && roll >= 16) return;   /* #it10d, harness only */
    Powerup *p = pu_alloc(); if (!p) return;
    int kind = (int)(rnd() % PUK_COUNT);
    p->alive = 1; p->kind = kind; p->x = x; p->y = y; p->vy = 2.0f;
}
/* #it12: POWER is now a SHIP upgrade, not a per-weapon one. The old rule was
 * "level up only if you picked up the weapon you are already holding, else
 * switch and reset to LV1". Seven of the ten powerup kinds are weapons and
 * maybe_drop_powerup() draws uniformly, so the chance a given weapon pickup
 * matched what you were holding was 1/7: in practice the POWER box sat at
 * LV 1/3 for an entire run and LV2/LV3 fire patterns (which every weapon
 * except TRI already implemented, see the switch in player_fire) were content
 * almost nobody ever saw. Now every weapon pickup raises POWER by one to a cap
 * of 3 and swaps the weapon, so the box climbs as the run goes on. The penalty
 * side is untouched and is what keeps this from being a free ride:
 * player_hit() still drops you to WP_SINGLE at LV1, so a single hit costs the
 * whole ladder. */
static void set_weapon(int w) { if (g_wlevel < 3) g_wlevel++; else add_score(400); g_weapon = w; }
static void apply_powerup(int kind) {
    switch (kind) {
        case PUK_RAPID:   g_rapid_until = g_now + 10000; break;
        case PUK_TWIN:    set_weapon(WP_TWIN); break;
        case PUK_TRI:     set_weapon(WP_TRI); break;
        case PUK_WIDE:    set_weapon(WP_WIDE); break;
        case PUK_LASER:   set_weapon(WP_LASER); break;
        case PUK_MISSILE: set_weapon(WP_MISSILE); break;
        case PUK_WAVE:    set_weapon(WP_WAVE); break;
        case PUK_SHIELD:  g_shield_until = g_now + 8000; break;
        case PUK_BOMB:    if (g_bombs < 5) g_bombs++; else add_score(300); break;
        case PUK_LIFE:    if (g_lives < 6) g_lives++; else add_score(500); break;
    }
    g_pu_label = puk_name(kind); g_pu_label_until = g_now + 1400; add_score(60); sfx(SFX_POWERUP);   /* #it20 */
}

/* ============================================================ collisions == */
static int aabb(float ax, float ay, int aw, int ah, float bx, float by, int bw, int bh) { return iabs((int)ax - (int)bx) * 2 < (aw + bw) && iabs((int)ay - (int)by) * 2 < (ah + bh); }
static int enemy_box_w(Enemy *e) { return e->boss ? 190 : (e->type == ET_STATION ? 120 : (e->type == ET_TANK ? 60 : 46)); }
static int enemy_box_h(Enemy *e) { return e->boss ? 140 : (e->type == ET_STATION ? 100 : (e->type == ET_TANK ? 52 : 40)); }

static void player_hit(void) {
    if (g_dbg_god) return;   /* #it10: debug invulnerability, see g_dbg_god declaration above */
    if (g_now < g_shield_until || g_now < g_invuln_until) return;
    spawn_explosion(g_px, g_py, 1); g_lives--; g_hit_flash = 8; g_combo = 0; g_combo_until = 0;   /* #it16 */
    g_stage_hit_taken = 1;   /* #it4: this stage is no longer eligible for the perfect-clear bonus */
    screen_shake(10.0f, 220);   /* #it2 */ sfx(SFX_PLAYER_HIT);   /* #it20 */
    if (g_weapon != WP_SINGLE) { g_weapon = WP_SINGLE; g_wlevel = 1; } else if (g_wlevel > 1) g_wlevel = 1;
    g_invuln_until = g_now + 2000; g_shield_until = 0;
    if (g_lives <= 0) { g_state = GS_GAMEOVER; g_banner_until = 0; g_go_sel = g_continues > 0 ? 0 : 1; }   /* #it18 */
    else { g_px = (float)(W / 2); g_py = (float)(H + 60); begin_respawn(); }   /* #it17: fly in, do not teleport */
}
static void enemy_killed(Enemy *e) {
    spawn_explosion_c(e->x, e->y, e->boss, e->col); add_score(e->score * combo_mult());
    int prev_mult = combo_mult(); g_combo++;
    g_combo_until = g_now + combo_window();   /* #it16: every kill re-arms the decay */
    /* #it7, corrected by #it16: pop only when combo_mult() ACTUALLY changed.
     * The old `g_combo % 8 == 0` test kept firing "STREAK xN!" every 8 kills
     * past the cap, claiming a step-up that the capped multiplier had stopped
     * granting - a lie the player could read straight off the unchanged
     * MULTIPLIER box next to it. */
    if (combo_mult() > prev_mult) {
        char b[8]; num_to_str((long)combo_mult(), b);
        strcpy(g_streak_txt, "STREAK x"); strcat(g_streak_txt, b); strcat(g_streak_txt, "!");
        g_streak_until = g_now + 1100;
    }
    g_kills++; g_stage_killed++;
    sfx(e->boss ? SFX_BOSS_DIE : SFX_EXPLODE);   /* #it20 */
    if (e->boss) {
        g_boss_active = 0; for (int i = 0; i < 4; i++) maybe_drop_powerup(e->x + (i - 2) * 44, e->y, 1);
        screen_shake(18.0f, 500);   /* #it2: the biggest shake in the game, reserved for a boss kill */
        /* #it4: award the perfect-clear bonus now, while g_stage_hit_taken still
         * reflects THIS stage (it is reset below, and again on the next stage's
         * PLAYING rollover, before the player can take a new hit). */
        g_perfect_bonus = g_stage_hit_taken ? 0 : (500 + (long)g_stage * 250);
        if (g_perfect_bonus > 0) add_score(g_perfect_bonus);
        g_state = GS_STAGECLEAR; g_stageclear_until = g_now + 2600;
    }
    else maybe_drop_powerup(e->x, e->y, 0);
    e->alive = 0;
}

/* ============================================================ update ====== */
static void update_input(void) {
    if (g_state != GS_PLAYING) return;
    /* #it17: during a respawn the ship is on rails and takes no input. The
     * lerp is driven off wall-clock g_now (like every other timed effect in
     * this file) rather than a frame counter, so the entrance takes the same
     * 750ms whatever the framerate is doing. */
    if (g_now < g_respawn_until) {
        float f = 1.0f - (float)(g_respawn_until - g_now) / (float)RESPAWN_MS;
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        f = f * f * (3.0f - 2.0f * f);          /* smoothstep: decelerates into place */
        g_px = (float)(W / 2);
        g_py = (float)(H + 60) + ((float)(H - 90) - (float)(H + 60)) * f;
        return;
    }
    float sp = 7.5f; int moved_kb = 0; float tx = g_px, ty = g_py;
    if (held('a', 0x1E) || key_down(0x80 + 3) || key_down(0x4B)) { tx -= sp; moved_kb = 1; }
    if (held('d', 0x20) || key_down(0x80 + 2) || key_down(0x4D)) { tx += sp; moved_kb = 1; }
    if (held('w', 0x11) || key_down(0x80 + 0) || key_down(0x48)) { ty -= sp; moved_kb = 1; }
    if (held('s', 0x1F) || key_down(0x80 + 1) || key_down(0x50)) { ty += sp; moved_kb = 1; }
    /* mouse is reported in full-window coords; map into the playfield strip. */
    if (moved_kb) { g_px = tx; g_py = ty; } else if (g_have_mouse) { g_px = (float)(g_mx - PF_OX); g_py = (float)g_my; }
    g_px = clampf(g_px, PLAYER_W / 2, (float)(W - PLAYER_W / 2));
    g_py = clampf(g_py, (float)(H / 5), (float)(H - PLAYER_H / 2 - 8));
    /* Auto-fire removed (coordinator direction): fire only while LMB is held
     * (g_mouse_fire, set/cleared by EVENT_MOUSE_DOWN/UP for the left button)
     * or SPACE is held as the keyboard fire key. RMB is the bomb trigger and
     * must NOT hold the fire flag (handled separately at EVENT_MOUSE_DOWN). */
    if (g_mouse_fire || key_down(' ') || key_down(0x39)) player_fire();
}

static void enemy_move(Enemy *e, int dt) {
    float sc = e->vy;
    switch (e->pattern) {
        case PT_STRAIGHT:   e->y += sc; break;
        case PT_SINE:       e->y += sc; e->x = e->basex + fsinf(e->t * 2.2f) * 70.0f; break;
        case PT_SCURVE:     e->y += sc; e->x = e->basex + fsinf(e->t * 1.4f) * 120.0f; break;
        case PT_FIG8:       e->y += sc * 0.7f; e->x = e->basex + fsinf(e->t * 2.0f) * 90.0f; e->y += fsinf(e->t * 4.0f) * 0.6f; break;
        case PT_DIVE:       e->vy += 0.03f * diff_enemy_speed(); e->y += e->vy; e->x += (g_px > e->x ? 0.8f : -0.8f); break;
        case PT_SWEEP:      e->y += sc * 0.35f; e->x += (fsinf(e->t * 0.9f) > 0 ? 2.6f : -2.6f); if (e->x < 40) e->x = 40; if (e->x > W - 40) e->x = W - 40; break;
        case PT_CIRCLE:     e->y += sc * 0.4f; e->x = e->basex + fcosf(e->t * 2.5f) * 60.0f; break;
        case PT_HOVER:      if (e->y < H / 4) e->y += sc; else e->x = e->basex + fsinf(e->t * 1.2f) * 130.0f; break;
        case PT_STATIONARY: e->y += 1.6f * diff_enemy_speed(); break;   /* scrolls with the terrain */
        default:            e->y += sc; break;
    }
    (void)dt;
}
static void enemy_shoot(Enemy *e) {
    float sp = diff_ebullet_speed();
    switch (e->type) {
        case ET_DRONE:    spawn_eb(e->x, e->y + 16, 0, sp, BK_STRAIGHT); break;
        case ET_WEAVER:   spawn_eb(e->x, e->y + 16, fsinf(e->t) * 1.5f, sp, BK_WAVE); break;
        case ET_DIVER:    eb_aimed(e->x, e->y + 16, sp, BK_AIMED); break;
        case ET_STRAFER:  eb_aimed(e->x, e->y + 16, sp * 1.1f, BK_AIMED); break;
        case ET_GUNSHIP:  eb_spread(e->x, e->y + 20, 5, 1.4f, sp * 0.9f, BK_STRAIGHT); break;
        case ET_TANK:     eb_ring(e->x, e->y, 8, sp * 0.7f, e->t, BK_PLASMA); break;
        case ET_KAMIKAZE: break;   /* rams */
        case ET_MINELAYER:{ Bullet *b = spawn_eb(e->x, e->y + 10, 0, 0.4f, BK_PLASMA); if (b) b->life = 1600; } break;
        case ET_TURRET:   eb_aimed(e->x, e->y, sp, BK_AIMED); break;
        case ET_SILO:     eb_aimed(e->x, e->y, sp * 0.7f, BK_MISSILE); break;
        case ET_STATION:  eb_spread(e->x, e->y, 7, 2.2f, sp * 0.8f, BK_STRAIGHT); eb_aimed(e->x, e->y, sp, BK_AIMED); break;
    }
}

static int count_enemies(void);
static void update_world(int dt) {
    /* player bullets */
    for (int i = 0; i < MAX_PB; i++) {
        Bullet *b = &g_pb[i]; if (!b->alive) continue; b->t += dt * 0.001f;
        if (b->kind == PK_MISSILE) { /* home toward nearest enemy */
            float bestd = 1e18f, tx = b->x, ty = -100; int found = 0;
            for (int j = 0; j < MAX_EN; j++) { Enemy *e = &g_en[j]; if (!e->alive) continue; float dx = e->x - b->x, dy = e->y - b->y; float d = dx * dx + dy * dy; if (d < bestd) { bestd = d; tx = e->x; ty = e->y; found = 1; } }
            if (found) { float ux, uy; norm_dir(tx - b->x, ty - b->y, &ux, &uy); float sp = 13.0f; b->vx = b->vx * 0.8f + ux * sp * 0.2f; b->vy = b->vy * 0.8f + uy * sp * 0.2f; }
        } else if (b->kind == PK_WAVE) { b->x += fsinf(b->t * 12.0f) * 3.0f; }
        b->x += b->vx; b->y += b->vy;
        if (b->y < -30 || b->x < -30 || b->x > W + 30) { b->alive = 0; continue; }
        int dmg = pb_damage(b->kind), pierced = 0;
        for (int j = 0; j < MAX_EN; j++) { Enemy *e = &g_en[j]; if (!e->alive) continue;
            if (aabb(b->x, b->y, 10, 20, e->x, e->y, enemy_box_w(e), enemy_box_h(e))) {
                e->hp -= dmg; g_shots_hit++;
                if (e->hp <= 0) enemy_killed(e); else spawn_explosion_c(b->x, b->y - 6, 0, 0x00FFF0C0);
                if (b->kind == PK_LASER && pierced < 2) { pierced++; continue; }   /* laser pierces */
                b->alive = 0; break;
            }
        }
    }
    /* enemy bullets */
    for (int i = 0; i < MAX_EB; i++) {
        Bullet *b = &g_eb[i]; if (!b->alive) continue; b->t += dt * 0.001f; b->life -= dt;
        if (b->kind == BK_MISSILE) { float ux, uy; norm_dir(g_px - b->x, g_py - b->y, &ux, &uy); float sp = diff_ebullet_speed(); b->vx = b->vx * 0.9f + ux * sp * 0.1f; b->vy = b->vy * 0.9f + uy * sp * 0.1f; }
        else if (b->kind == BK_WAVE) { b->x += fsinf(b->t * 8.0f) * 2.0f; }
        b->x += b->vx; b->y += b->vy;
        if (b->life <= 0 || b->y > H + 20 || b->y < -30 || b->x < -30 || b->x > W + 30) { b->alive = 0; continue; }
        if (aabb(b->x, b->y, 10, 10, g_px, g_py, PLAYER_W - 14, PLAYER_H - 10)) { b->alive = 0; player_hit(); }
    }
    /* enemies */
    for (int i = 0; i < MAX_EN; i++) {
        Enemy *e = &g_en[i]; if (!e->alive) continue; e->t += dt * 0.001f;
        if (e->boss) {
            /* Descend faster, then sweep wider and quicker as its health drops so it
             * gets harder to corner in the later phases. */
            if (e->y < e->basey) e->y += 1.5f;
            else if (e->boss_kind == BOSS_FORTRESS) {
                /* #it15: barely moves. Its threat is the wall it puts up, not
                 * its position, and a slow drift keeps it readable as a
                 * stationary hazard while still not being literally static. */
                float rate = 0.30f + (float)e->boss_phase * 0.10f;
                e->x = e->basex + fsinf(e->t * rate) * e->amp;
            } else if (e->boss_kind == BOSS_INTERCEPTOR) {
                /* #it15: hunts the player's x with a capped closing speed, and
                 * bobs vertically. Capped (not a snap) so it can always be
                 * out-manoeuvred by moving, which is the whole point of the
                 * archetype; clamped to the playfield so its 190px hitbox can
                 * never park half off-screen where it cannot be shot. */
                float chase = 1.9f + (float)e->boss_phase * 0.7f;
                if (g_px > e->x + 3.0f)      e->x += chase;
                else if (g_px < e->x - 3.0f) e->x -= chase;
                e->x = clampf(e->x, 110.0f, (float)(W - 110));
                e->y = e->basey + fsinf(e->t * 1.6f) * 26.0f;
            } else { float rate = 0.9f + (float)e->boss_phase * 0.55f;
                   float amp  = e->amp * (1.0f + (float)e->boss_phase * 0.22f);
                   e->x = e->basex + fsinf(e->t * rate) * amp; }
            int lo = e->maxhp / 3, mid = 2 * e->maxhp / 3;
            e->boss_phase = e->hp > mid ? 0 : (e->hp > lo ? 1 : 2);
            /* #it3: telegraph an ESCALATING phase change (never a de-escalation,
             * which cannot happen here, but the > guard keeps this honest if the
             * HP formula ever changes) with a shake + a banner, once, the frame
             * it happens - not every frame the new phase holds true. */
            if (e->boss_phase != g_last_boss_phase) {
                if (g_last_boss_phase >= 0 && e->boss_phase > g_last_boss_phase) {
                    screen_shake(9.0f, 260); sfx(SFX_PHASE);   /* #it20 */
                    g_phase_banner_until = g_now + 1400;
                    strcpy(g_phase_banner_txt, e->boss_phase == 2 ? "CRITICAL - CORE EXPOSED" : "WARNING - PHASE 2");
                }
                g_last_boss_phase = e->boss_phase;
            }
            e->fire_ms -= dt;
            if (e->fire_ms <= 0) { boss_fire(e);
                int base = e->boss_phase == 2 ? 300 : (e->boss_phase == 1 ? 360 : 520);
                /* #it15: per-archetype cadence. The FORTRESS fires a whole
                 * screen-wide wall per volley, so it must fire much less often
                 * or the playfield is solid bullets; the INTERCEPTOR fires a
                 * handful of aimed shots, so it fires much more often. Same
                 * phase curve underneath, scaled. */
                if (e->boss_kind == BOSS_FORTRESS)         base = base * 9 / 5;
                else if (e->boss_kind == BOSS_INTERCEPTOR) base = base * 3 / 5;
                if (g_difficulty == 2) base = base * 3 / 4;   /* hard: 25% faster */
                e->fire_ms = base; }
        } else {
            enemy_move(e, dt);
            e->fire_ms -= dt;
            if (e->fire_ms <= 0 && e->y > -10 && e->y < H - 40 && e->type != ET_KAMIKAZE) {
                enemy_shoot(e);
                int base = e->type == ET_STATION ? diff_fire_ms() / 2 : diff_fire_ms();
                e->fire_ms = rndrange(base, base * 2);
            }
            if (e->y > H + 70) e->alive = 0;
        }
        if (e->alive && aabb(e->x, e->y, enemy_box_w(e), enemy_box_h(e), g_px, g_py, PLAYER_W - 12, PLAYER_H - 8)) {
            if (!e->boss && e->type != ET_STATION && e->type != ET_TURRET && e->type != ET_SILO) { spawn_explosion_c(e->x, e->y, 0, e->col); e->alive = 0; }
            player_hit();
        }
    }
    /* powerups */
    for (int i = 0; i < MAX_PU; i++) { Powerup *p = &g_pu[i]; if (!p->alive) continue; p->y += p->vy; if (p->y > H + 30) { p->alive = 0; continue; } if (aabb(p->x, p->y, 30, 30, g_px, g_py, PLAYER_W, PLAYER_H)) { apply_powerup(p->kind); p->alive = 0; } }
    /* explosions */
    for (int i = 0; i < MAX_EX; i++) { Explosion *e = &g_ex[i]; if (!e->alive) continue; e->t += dt; if (e->t >= 42) { e->t = 0; e->frame++; if (e->frame >= 16) e->alive = 0; } }
    /* #it16: combo decay. One tier per expiry, never below zero, and the
     * window re-arms so a stalled player keeps sliding down rather than
     * falling off a cliff once. Costs one compare per frame when idle.
     * Gated on GS_PLAYING and on the intro card being finished, because the
     * combo survives a stage change: without that guard a player would lose a
     * tier at EVERY stage transition for the crime of watching the stage
     * clear screen and the intro card, which is not a thing they did wrong
     * and not a thing they could have avoided. */
    if (g_state == GS_PLAYING && g_now >= g_intro_until &&
        g_combo > 0 && g_combo_until && g_now >= g_combo_until) {
        int tier = g_combo / COMBO_TIER;
        g_combo = tier > 0 ? (tier - 1) * COMBO_TIER : 0;
        g_combo_until = g_combo > 0 ? g_now + combo_window() : 0;
    }
    if (g_hit_flash > 0) g_hit_flash--; if (g_bomb_flash > 0) g_bomb_flash--;
}
static int count_enemies(void) { int n = 0; for (int i = 0; i < MAX_EN; i++) if (g_en[i].alive && !g_en[i].stationary) n++; return n; }
static int count_all_enemies(void) { int n = 0; for (int i = 0; i < MAX_EN; i++) if (g_en[i].alive) n++; return n; }

static void update_waves(void) {
    if (g_boss_active) return;
    if (count_enemies() > 0) return;
    if (g_now < g_wave_delay) return;
    if (g_wave < WAVES_PER_STAGE) { spawn_formation(g_wave); g_wave++; g_wave_delay = g_now + 900; }
    else if (count_all_enemies() == 0) { spawn_boss(); }
}

/* ============================================================ rendering === */
static void draw_ship_fallback(int cx, int cy, uint32_t body, int up) {
    int hw = PLAYER_W / 2, hh = PLAYER_H / 2;
    for (int y = 0; y < PLAYER_H; y++) { float f = (float)y / (float)PLAYER_H; int span = up ? (int)(hw * f) : (int)(hw * (1.0f - f)); fill_rect(cx - span, cy - hh + y, span * 2, 1, body); }
    fill_rect(cx - 3, cy - hh, 6, PLAYER_H, up ? 0x00DDE8FF : 0x00FFD0D0);
}
static void draw_player(void) {
    int shielded = g_now < g_shield_until, inv = g_now < g_invuln_until;
    /* engine bloom trail */
    add_glow((int)g_px, (int)(g_py + PLAYER_H / 2 + 4), 12, 0x0040A0FF, 150);
    add_glow((int)g_px, (int)(g_py + PLAYER_H / 2 + 10), 8, 0x00FFFFFF, 90);
    /* #it17: a much bigger burn while flying in, so the entrance reads as a
     * deliberate arrival under power rather than the ship simply appearing.
     * It fades out as the ship settles, handing back to the idle trail above
     * with no visible seam. */
    if (g_now < g_respawn_until) {
        int f = (int)((g_respawn_until - g_now) * 255 / RESPAWN_MS);
        add_glow((int)g_px, (int)(g_py + PLAYER_H / 2 + 16), 20, 0x0060C0FF, 60 + f / 2);
        add_glow((int)g_px, (int)(g_py + PLAYER_H / 2 + 30), 14, 0x00FFFFFF, f / 2);
    }
    if (!(inv && ((g_now / 80) & 1))) {
        if (A_player.ok) spr_blit_c(&A_player, (int)g_px, (int)g_py);
        else draw_ship_fallback((int)g_px, (int)g_py, 0x0080C8FF, 1);
    }
    if (shielded) { int r = 42 + (int)(fsinf((float)g_now * 0.01f) * 3.0f);
        for (int a = 0; a < 360; a += 10) { float rad = (float)a * 0.01745f; add_glow((int)(g_px + fcosf(rad) * r), (int)(g_py + fsinf(rad) * r), 4, 0x0040FFFF, 120); } }
}
static void draw_enemy(Enemy *e) {
    if (e->boss) {
        /* #it15: tint the shared BOSS.BMP by the archetype colour so the three
         * bosses are distinguishable on sight, not only by behaviour. The
         * scaled blit is the one path that had no tinted variant, so the
         * modulation is folded into the surrounding glow instead: a large,
         * strongly-coloured aura plus a coloured underlight, which reads at
         * the 210x158 size a boss occupies without needing a second scaled
         * blit pass every frame. */
        add_glow((int)e->x, (int)e->y, 40, e->col, 85);
        add_glow((int)e->x, (int)e->y + 40, 34, e->col, 70);
        if (A_boss.ok) spr_blit_scaled_c(&A_boss, (int)e->x, (int)e->y, 210, 158);
        else if (A_enemy[0].ok) spr_blit_scaled_c(&A_enemy[0], (int)e->x, (int)e->y, 210, 158);
        else { for (int i = 0; i < 3; i++) draw_ship_fallback((int)e->x + (i - 1) * 64, (int)e->y, e->col, 0); }
        int bw = W / 2, bx = (int)(W / 2 - bw / 2), by = 24; fill_rect(bx - 2, by - 2, bw + 4, 12, 0x00202028); fill_rect(bx, by, bw * (e->hp > 0 ? e->hp : 0) / e->maxhp, 8, 0x00FF4060);
        const char *ph = e->boss_phase == 0 ? "PHASE 1" : e->boss_phase == 1 ? "PHASE 2" : "PHASE 3 - CORE"; sq_text_sh(bx, by + 14, ph, 0x00FFC0C0, 1);
        /* #it15: the archetype name sits on the same row as the phase, right
         * aligned, so a player glancing at the health bar always knows WHICH
         * boss this is without having to have caught the arrival banner. */
        { const char *bn = boss_name(e->boss_kind); sq_text_sh(bx + bw - text_w(bn, 1), by + 14, bn, 0x00FFE0A0, 1); }
        return;
    }
    if (e->stationary) {
        /* draw as a metallic emplacement box + gun barrel + glow */
        int hw = enemy_box_w(e) / 2, hh = enemy_box_h(e) / 2;
        fill_rect((int)e->x - hw, (int)e->y - hh, hw * 2, hh * 2, 0x00404860);
        fill_rect((int)e->x - hw, (int)e->y - hh, hw * 2, 3, 0x00808CA8);
        if (e->type == ET_STATION) { for (int g = -2; g <= 2; g++) fill_rect((int)e->x + g * 22 - 3, (int)e->y + hh - 4, 6, 12, 0x00303848); add_glow((int)e->x, (int)e->y, 26, e->col, 70); }
        else { fill_rect((int)e->x - 4, (int)e->y, 8, hh + 10, 0x00303848); add_glow((int)e->x, (int)e->y, 14, e->col, 90); }
        /* #it8 (iteration 8, fire telegraph): stationary emplacements (TURRET/
         * SILO/STATION) used to open fire the instant e->fire_ms hit 0 with NO
         * warning at all - unlike every mobile enemy, whose muzzle flash at
         * least follows a readable approach. In the last TELEGRAPH_MS of the
         * countdown, brighten this emplacement's own glow toward white so a
         * shot is legible as "about to happen", not merely reactable after
         * the fact. Uses e->fire_ms directly (already ticked in
         * update_world()), so it needs no new per-enemy state and scales with
         * whatever interval diff_fire_ms()/stage scaling picked, whether that
         * is 300ms on HARD stage 20 or 2200ms on EASY stage 1. */
        { const int TELEGRAPH_MS = 220;
          if (e->fire_ms > 0 && e->fire_ms < TELEGRAPH_MS) {
              float f = 1.0f - (float)e->fire_ms / (float)TELEGRAPH_MS;
              add_glow((int)e->x, (int)e->y, 10 + (int)(10.0f * f), 0x00FFFFFF, 60 + (int)(150.0f * f));
          } }
        int barw = hw * 2 * (e->hp > 0 ? e->hp : 0) / e->maxhp; fill_rect((int)e->x - hw, (int)e->y - hh - 6, barw, 3, 0x00FFA040);
        return;
    }
    /* #it13 (enemy archetype readability): there are EIGHT mobile archetypes
     * and only FOUR enemy sprites, indexed `e->type % 4`, so four pairs have
     * always shared an identical silhouette: DRONE/GUNSHIP (2 HP vs 6 HP),
     * WEAVER/TANK (3 vs 12), DIVER/KAMIKAZE and STRAFER/MINELAYER. The only
     * thing separating them was an 8px radius, intensity-60 glow in e->col,
     * which is not legible once a late-stage screen has a few hundred
     * additive-glow bullets over it. DIVER/KAMIKAZE is the pair that actually
     * costs a life: a DIVER shoots and can be ignored at close range, a
     * KAMIKAZE never shoots and exists only to ram you, and they looked the
     * same. Three cheap fixes, all on top of the existing sprite:
     *  (1) modulate the shared sprite by the archetype tint (spr_blit_tint_c
     *      above), which turns 4 silhouettes into 8 distinguishable ships
     *      without needing 4 more BMPs;
     *  (2) a compact per-class marker glyph, so the distinction survives even
     *      where the backdrop washes out hue;
     *  (3) a stronger, pulsing red warning ring on the rammer specifically. */
    Sprite *s = &A_enemy[e->type % 4];
    if (s->ok) spr_blit_tint_c(s, (int)e->x, (int)e->y, e->col, 170);
    else draw_ship_fallback((int)e->x, (int)e->y, e->col, 0);
    add_glow((int)e->x, (int)(e->y - 6), 9, e->col, 95);
    { int ex = (int)e->x, ey = (int)e->y;
      switch (e->type) {
        case ET_KAMIKAZE: {   /* rammer: unmistakable pulsing red X + ring */
            int p = 120 + (int)(100.0f * fsinf((float)g_now * 0.014f));
            if (p < 60) p = 60;
            if (p > 255) p = 255;
            add_glow(ex, ey, 15, 0x00FF2000, p);
            for (int k = -9; k <= 9; k++) { put_px_a(ex + k, ey + k, 0x00FFFFFF, p);
                                            put_px_a(ex + k, ey - k, 0x00FFFFFF, p);
                                            put_px_a(ex + k + 1, ey + k, 0x00FF4020, p);
                                            put_px_a(ex + k + 1, ey - k, 0x00FF4020, p); }
        } break;
        case ET_GUNSHIP:      /* twin muzzle pips under the nose */
            fill_rect(ex - 13, ey + 13, 5, 5, 0x00FFD0FF); fill_rect(ex + 8, ey + 13, 5, 5, 0x00FFD0FF); break;
        case ET_MINELAYER: {  /* hollow ring: it leaves things behind */
            for (int a = 0; a < 360; a += 30) { float rr = (float)a * 0.01745f;
                fill_rect(ex + (int)(fcosf(rr) * 11.0f) - 1, ey + (int)(fsinf(rr) * 11.0f) - 1, 3, 3, 0x00C0FF60); } } break;
        case ET_STRAFER:      /* one wide lateral bar: it sweeps sideways */
            fill_rect(ex - 16, ey + 12, 32, 4, 0x0060E8FF); break;
        case ET_WEAVER:       /* a 3-step zigzag echoing its sine path */
            fill_rect(ex - 12, ey + 13, 7, 3, 0x0080FFA0); fill_rect(ex - 4, ey + 10, 7, 3, 0x0080FFA0);
            fill_rect(ex + 4, ey + 13, 7, 3, 0x0080FFA0); break;
        case ET_DIVER:        /* a downward chevron: it comes straight at you */
            for (int k = 0; k < 8; k++) { fill_rect(ex - 8 + k, ey + 8 + k, 2, 2, 0x00FFD060);
                                          fill_rect(ex + 7 - k, ey + 8 + k, 2, 2, 0x00FFD060); } break;
        default: break;   /* DRONE stays unadorned: it is the baseline shape */
      } }
    /* #it13: a HP pip row on every heavy (>4 HP), not just the TANK. A player
     * needs to know "this one soaks shots" BEFORE committing to it, and the
     * GUNSHIP (6 HP) previously showed nothing at all. */
    if (e->maxhp > 4) {
        int barw = 60 * (e->hp > 0 ? e->hp : 0) / e->maxhp;
        fill_rect((int)e->x - 30, (int)e->y - 30, 60, 3, 0x00201820);
        fill_rect((int)e->x - 30, (int)e->y - 30, barw, 3, 0x00FFA040);
    }
}
static void draw_powerup(Powerup *p) {
    uint32_t col = hsv((int)(g_now / 8) % 360, 255, 255);
    add_glow((int)p->x, (int)p->y, 16, col, 130);
    fill_rect((int)p->x - 13, (int)p->y - 13, 26, 26, 0x00101018);
    fill_rect((int)p->x - 11, (int)p->y - 11, 22, 22, col);
    const char *l = p->kind == PUK_RAPID ? "R" : p->kind == PUK_TWIN ? "II" : p->kind == PUK_TRI ? "3" : p->kind == PUK_WIDE ? "W" :
                    p->kind == PUK_LASER ? "L" : p->kind == PUK_MISSILE ? "M" : p->kind == PUK_WAVE ? "~" :
                    p->kind == PUK_SHIELD ? "S" : p->kind == PUK_BOMB ? "B" : "1";
    sq_text((int)p->x - text_w(l, 2) / 2, (int)p->y - 7, l, 0x00101018, 2);
}
static void draw_bullet_p(Bullet *b) {
    if (b->kind == PK_LASER) { add_glow((int)b->x, (int)b->y, 8, 0x0060FFFF, 200); fill_rect((int)b->x - 2, (int)b->y - 14, 4, 26, 0x00E0FFFF); fill_rect((int)b->x - 1, (int)b->y - 16, 2, 30, 0x00FFFFFF); }
    else if (b->kind == PK_MISSILE) { add_glow((int)b->x, (int)b->y + 6, 9, 0x00FF8020, 170); fill_rect((int)b->x - 3, (int)b->y - 6, 6, 14, 0x00FFD060); fill_rect((int)b->x - 2, (int)b->y - 8, 4, 6, 0x00FFFFFF); }
    else if (b->kind == PK_WAVE) { add_glow((int)b->x, (int)b->y, 12, 0x00C060FF, 150); for (int k = -6; k <= 6; k += 3) fill_rect((int)b->x + k - 1, (int)b->y - 2, 2, 4, 0x00E0B0FF); }
    else { add_glow((int)b->x, (int)b->y, 7, 0x0080FFFF, 130); fill_rect((int)b->x - 2, (int)b->y - 8, 4, 16, 0x0090FFFF); fill_rect((int)b->x - 1, (int)b->y - 10, 2, 20, 0x00FFFFFF); }
}
static void draw_bullet_e(Bullet *b) {
    uint32_t core, glow; int gr;
    switch (b->kind) {
        case BK_AIMED:   core = 0x00FFF060; glow = 0x00FF8020; gr = 7; break;
        case BK_WAVE:    core = 0x0060FF80; glow = 0x0020C060; gr = 7; break;
        case BK_PLASMA:  core = 0x00FF80FF; glow = 0x00A020C0; gr = 9; break;
        case BK_MISSILE: core = 0x00FFA0A0; glow = 0x00FF3030; gr = 9; break;
        default:         core = 0x00FF6060; glow = 0x00C02020; gr = 6; break;
    }
    add_glow((int)b->x, (int)b->y, gr, glow, 150);
    if (b->kind == BK_MISSILE) { fill_rect((int)b->x - 3, (int)b->y - 4, 6, 10, core); }
    else if (b->kind == BK_PLASMA) { fill_rect((int)b->x - 3, (int)b->y - 3, 6, 6, core); fill_rect((int)b->x - 1, (int)b->y - 1, 2, 2, 0x00FFFFFF); }
    else { fill_rect((int)b->x - 3, (int)b->y - 3, 6, 6, core); fill_rect((int)b->x - 1, (int)b->y - 1, 2, 2, 0x00FFFFFF); }
}
static void draw_explosion(Explosion *e) {
    if (A_explode.ok && e->frame < 16) { spr_blit_frame_c(&A_explode, 64, 64, 4, e->frame, (int)e->x, (int)e->y); }
    int r = e->frame * (e->big ? 8 : 4) + 4; int inten = 200 - e->frame * 12; if (inten < 0) inten = 0;
    add_glow((int)e->x, (int)e->y, e->big ? 30 : 16, e->col, inten);
    uint32_t c = e->frame < 5 ? 0x00FFF080 : e->frame < 10 ? 0x00FF9030 : 0x00A03010;
    int a = 220 - e->frame * 14; if (a < 0) a = 0;
    for (int ang = 0; ang < 360; ang += 20) { float rad = (float)ang * 0.01745f; blend_rect((int)(e->x + fcosf(rad) * r) - 2, (int)(e->y + fsinf(rad) * r) - 2, 4, 4, c, a); }
}

/* ======================================================= pixel-art backdrop
 * #562 (2026-07): the base backdrop layer used to be EITHER a bg_fill_scroll
 * of a per-level L<set>BGx.BMP (640x960 photoreal AI nebula/aurora art, found
 * live on the deployed VM/golden but never in git - see the Part C file
 * header comment) OR, if that file were ever absent, a smooth per-pixel
 * HSV/RGB gradient (proc_bg, removed). Both read as a vector/photo wash,
 * which is exactly what clashed with the also-photoreal BG_PLANET/RINGED/
 * ASTEROID/NEBULA.BMP far-layer sprites this task replaces: none of it
 * matched the flat-shaded, glow-bloom pixel/vector look of the ships,
 * bullets and enemies (draw_player/draw_enemy/draw_bullet_* below all render
 * flat-colour shapes with add_glow(), never smooth photoreal shading).
 * pxbg_draw() replaces BOTH with a genuinely CHUNKY pixel-art look,
 * built from two textbook pixel-art techniques used throughout this section:
 *   - hard PX_CELL x PX_CELL blocks: every "pixel" is really a small flat-
 *     colour square, so scrolling/scaling never introduces a smooth blur.
 *   - 4x4 ordered (Bayer) dithering at colour-band boundaries: how pixel art
 *     fakes more gradient steps than a tiny hand-picked palette has, as a
 *     speckled dither instead of a smooth blend.
 * It reuses the existing g_neb[] lattice-noise field (baked once in
 * neb_init(), see the nebula wash above) as its cloud-shape source, sampled
 * at PX_CELL granularity (nearest, unfiltered) rather than per-pixel, which
 * is what makes it read as a chunky mosaic instead of nebula_draw's smooth
 * wash despite sharing the same underlying noise; the two layers use
 * different scroll-speed factors so they never sit pixel-for-pixel aligned.
 * The backdrop stays deliberately DARK/desaturated (palette index 0 is near-
 * black) so it reads behind the saturated neon ships/bullets/enemies without
 * competing for attention - a bright pixel-art backdrop would look great and
 * also hide a bullet, which this layer must never do. Landmark sprites
 * (BG_PLANET/RINGED/ASTEROID/NEBULA.BMP) were regenerated with the identical
 * hard-block + Bayer-dither technique by tools/genart.py so the whole
 * backdrop reads as one cohesive pixel-art system, not "3 procedural layers
 * + 4 photoreal cutouts" (see CHANGELOG).
 *
 * Polish pass (2026-07-21, Direction A, user-approved): the prototype was a
 * single monotone 5-step ramp per set, which read as flat noise rather than
 * a designed starscape, and shared g_neb[]'s non-toroidal wrap seam (fixed
 * above in neb_init). Two things were added, both driven by the new coarse
 * g_nebz[] "zone" field (nebz_init) so they cost one extra array read per
 * cell, not a second noise evaluation:
 *   - VOID / CORE banding: where the zone field is low, cells sit ONE step
 *     darker on the SAME hue ramp (deep void patches); where it is high,
 *     cells sit ONE step brighter AND swap to the set's neighbouring-hue
 *     ramp (g_pxbg_pal2) - a brighter nebula core with a slightly different
 *     tint, exactly like a real emission-nebula core reads against its
 *     surrounding dust. Normal-zone cells are unchanged from the prototype.
 *   - g_pxbg_pal2[set] is a hand-authored ADJACENT hue for each family
 *     (violet leans magenta-pink for its cores; blue-hive leans indigo;
 *     ember leans crimson), never a different family - stage 1 stays a
 *     violet-family starscape, just no longer a monotone wash.
 * All three sets share this exact path (no per-set special-casing beyond the
 * palette tables themselves and set 1's existing circuit-grid overlay), so
 * the polish rolls out to every stage theme identically. Visually verified
 * on VM 2410 for all 3 sets as of this change (see CHANGELOG); stage 2/3
 * were forced via a temporary g_stage override for the screendump, then
 * verified again with that override removed before commit. */
#define PX_CELL 4
static const unsigned char g_bayer4x4[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};
/* per-level-set base palettes, darkest..brightest (index 0 near-black). Hues
 * match the 3 families nebula_draw() already uses (#476) so the translucent
 * wash layered on top of this never reads as a mismatched colour. Used for
 * NORMAL and VOID zones (see pxbg_draw). */
static const uint32_t g_pxbg_pal[3][5] = {
    { 0x00120A18, 0x00201432, 0x00352050, 0x004E3878, 0x006A50A0 },  /* set 0: violet nebula    */
    { 0x00080B14, 0x000E1628, 0x00182642, 0x00223A60, 0x002E4E80 },  /* set 1: deep blue / hive */
    { 0x00180A08, 0x00301210, 0x004E1C18, 0x00702620, 0x00943020 },  /* set 2: ember red        */
};
/* Neighbouring-hue palettes, same darkest..brightest brightness ramp as
 * g_pxbg_pal above (per index, roughly matched value/peak channel) but hue-
 * shifted within the same family: violet -> magenta-violet, blue-hive ->
 * indigo-blue, ember -> crimson. Used ONLY for CORE zones (see pxbg_draw),
 * so bright nebula cores read as a distinct, related colour rather than the
 * base ramp merely getting brighter. */
static const uint32_t g_pxbg_pal2[3][5] = {
    { 0x00180A16, 0x002A1230, 0x0046204C, 0x00663270, 0x008C48A0 },  /* set 0: violet -> magenta core */
    { 0x000C0916, 0x0014122A, 0x00241E44, 0x00342E62, 0x00463C82 },  /* set 1: blue  -> indigo core   */
    { 0x001A080C, 0x00320C16, 0x00501224, 0x00721830, 0x00961E3C },  /* set 2: ember -> crimson core  */
};
/* Quantise a 0..1023 noise value into one of 5 palette steps, ordered-
 * dithering the boundary between adjacent steps via the 4x4 Bayer matrix
 * (indexed by the cell's own integer coords, so the dither pattern is stable
 * frame to frame instead of flickering). */
static int pxbg_dither_idx(int v, int lx, int ly) {
    int steps = 4;                                  /* 5 palette entries -> 4 gaps */
    int scaled = v * steps;                         /* 0 .. steps*1024 */
    int idx = scaled >> 10; if (idx >= steps) idx = steps - 1;
    int frac = (scaled - (idx << 10)) >> 6;          /* 0..15 */
    int th = g_bayer4x4[ly & 3][lx & 3];
    return (frac > th) ? idx + 1 : idx;
}
static void pxbg_draw(int level, float scroll) {
    int set = (level - 1) % 3;
    const uint32_t *pal  = g_pxbg_pal[set];
    const uint32_t *pal2 = g_pxbg_pal2[set];
    /* base wash: the furthest thing on screen, so it scrolls below every
     * other layer's minimum speed (nebula wash 0.06x, stars' slowest 0.15,
     * every enemy pattern's vy - see #476 depth ordering). The zone field
     * scrolls at the same rate so cores/voids track the cloud shape they
     * modulate, but samples a differently-sized/periodled array (NEBZ_* vs
     * NEB_*, see nebz_init) so the two never sit in permanent lock-step. */
    int nscroll = (int)(scroll * 0.5f);
    for (int by = 0; by < H; by += PX_CELL) {
        int ny = ((by + nscroll) / PX_CELL) % NEB_H; if (ny < 0) ny += NEB_H;
        int nzy = ((by + nscroll) / PXZ_CELL) % NEBZ_H; if (nzy < 0) nzy += NEBZ_H;
        for (int bx = 0; bx < W; bx += PX_CELL) {
            int nx = (bx / PX_CELL) % NEB_W; if (nx < 0) nx += NEB_W;
            int nzx = (bx / PXZ_CELL) % NEBZ_W; if (nzx < 0) nzx += NEBZ_W;
            int v = (int)g_neb[ny][nx] * 4;                       /* 0..1020 */
            int idx = pxbg_dither_idx(v, bx / PX_CELL, by / PX_CELL);
            int zone = g_nebz[nzy][nzx];                          /* 0..255 */
            const uint32_t *use = pal;
            if (zone < 70)       { if (idx > 0) idx--; }                       /* deep void: darker, same hue */
            else if (zone > 190) { if (idx < 4) idx++; use = pal2; }           /* bright core: brighter, adjacent hue */
            fill_rect(bx, by, PX_CELL, PX_CELL, use[idx]);
        }
    }
    /* sparse bright "dust mote" flecks, far slower than the starfield, purely
     * for extra far-layer depth texture (deterministic per-index position,
     * only drifting with the (very slow) scroll so they wrap seamlessly). */
    int dscroll = (int)(scroll * 0.10f);
    int hh = H > 0 ? H : 1, ww = W > 0 ? W : 1;
    for (int i = 0; i < 26; i++) {
        int bx = (int)(((unsigned)(i * 733 + set * 91)) % (unsigned)ww);
        int by = (int)(((unsigned)(i * 977 + dscroll)) % (unsigned)hh);
        fill_rect((bx / PX_CELL) * PX_CELL, (by / PX_CELL) * PX_CELL, PX_CELL, PX_CELL, pal[4]);
    }
    /* set 1 ("hive/tech" stage): a faint chunky circuit-grid lattice, dimmer
     * than the palette's own base band so it reads as distant machinery
     * rather than competing with gameplay. */
    if (set == 1) {
        int gscroll = nscroll % (PX_CELL * 6); if (gscroll < 0) gscroll += PX_CELL * 6;
        for (int by = -gscroll; by < H; by += PX_CELL * 6) fill_rect(0, by, W, 1, pal[1]);
        for (int bx = 0; bx < W; bx += PX_CELL * 8) fill_rect(bx, 0, 1, H, pal[1]);
    }
}
static void draw_background(void) {
    int level = g_state == GS_MENU ? 1 : g_stage;
    /* #562: this used to bgset_load() + bg_fill_scroll() a per-level
     * L<set>BGx.BMP (640x960 photoreal AI nebula/aurora photography). Those
     * files turned out to be exactly the "same backgrounds" this task
     * replaces - AND they were never in git (an asset-drift gap, same class
     * as #535's "git repo missing source for shipping apps"), so relying on
     * them was fragile as well as visually wrong. The base backdrop now
     * always draws with the procedural pixel-art pxbg_draw() (see its
     * comment above), which also sidesteps decoding a ~1.8MB BMP on every
     * stage transition for no benefit (#444 ext2/DMA-under-load risk with
     * large assets, watched for on principle even though this VM hasn't hit
     * it). bg_fill_scroll()/A_bgset/bgset_load() are left defined, unused,
     * for a future pass that might author real pixel-art L<set>BGx.BMP
     * replacements instead of (or blended with) the procedural layer. */
    int pos = (int)g_bgpos; (void)pos;
    pxbg_draw(level, g_bgpos);
    /* #476 multi-layer parallax, slowest to fastest: nebula wash, distant
     * star-cluster layer (Direction A item 3: even further/slower/dimmer
     * than the 3-layer starfield below), far background art (planets/
     * asteroid/cloud), then the 3-layer starfield. Everything here is
     * deliberately slower than any enemy's vy. */
    nebula_draw(level, g_bgpos);
    clust_draw();
    bgobj_draw_all();
    stars_draw();
}

/* ====================================================== perf instrumentation */
/* Serial is silent under the compositor (GUI mode, per prior findings), so -
 * same idea as Arena's /ARENA/KEYLOG.TXT ground-truth breadcrumb - this writes
 * a small measured FPS log to disk instead of guessing: frames/sec sampled
 * once a second, capped at FPS_MAX_SAMPLES so it can never grow unbounded,
 * flushed after every sample so a mid-run screendump/mount always sees the
 * latest number. g_perf_fps also drives the live FPS readout in the sidebar. */
#define FPS_LOG_PATH    "/SQUADRON/FPSLOG.TXT"
#define FPS_LOG_CAP     3072
#define FPS_MAX_SAMPLES 90
static char     g_fps_log[FPS_LOG_CAP];
static int      g_fps_log_len = 0;
static int      g_fps_samples = 0;
static unsigned g_fps_win_start = 0;
static unsigned g_fps_win_frames = 0;
static int      g_perf_fps = 0;   /* last measured fps; shown in the RIGHT sidebar */
static unsigned g_snd_blocks_prev = 0;   /* #it20: previous g_snd_blocks sample */

#define FPS_DEAD_OPEN  0
#define FPS_DEAD_WRITE 1
static int g_fps_dead_said = 0;
static void fps_dead_once(int what, long rc) {
    if (g_fps_dead_said) return;
    g_fps_dead_said = 1;
    char b[160], n[24]; int i = 0;
    const char *p = "\n[SQPERF] FPS log is DEAD: sys_"; while (*p) b[i++] = *p++;
    p = (what == FPS_DEAD_OPEN) ? "open" : "write"; while (*p) b[i++] = *p++;
    p = "(\"" FPS_LOG_PATH "\") returned "; while (*p) b[i++] = *p++;
    num_to_str(rc, n); p = n; while (*p) b[i++] = *p++;
    p = ". Nothing is being recorded to that file; the on-screen FPS box and "
        "the SYS_BOOTLOG_WRITE line are the real numbers.\n";
    while (*p && i < 158) b[i++] = *p++;
    b[i] = 0;
    sys_write(1, b, (unsigned long)i);
}
static void fps_append(const char *s) { while (*s && g_fps_log_len < FPS_LOG_CAP - 1) g_fps_log[g_fps_log_len++] = *s++; }
static void fps_append_num(long v) { char b[24]; num_to_str(v, b); fps_append(b); }
/* #it20-fix: THIS LOG NEVER WORKED. The file-writing half below is the
 * original #475 code and its comment claims it "writes a small measured FPS
 * log to disk instead of guessing". MEASURED 2026-09-26: the pristine
 * golden carries no /SQUADRON/FPSLOG.TXT, nine Squadron launches on a clone
 * of it did not create one, and a 3072-byte file pre-created on the image
 * before boot was still byte-for-byte unchanged after a full run. So this
 * path produces nothing on the shipping image, and every claim that rested
 * on "the FPS log says..." rested on a file that does not exist.
 * (#sqearlyexit) The REASON is now measured too and it is a permission
 * refusal that applies to every path a uid=1000 app can name: see the
 * four-destination probe result in the block comment below.
 *
 * The fix is to ALSO emit each sample through SYS_BOOTLOG_WRITE. That path
 * IS ALIVE: MEASURED on a throwaway VM 2026-09-26, every sample appears on
 * the serial console as
 *   [BOOTLOG] [USERSPACE uid=1000] [SQPERF] fps=42 snd=0 audio=on
 * so the mechanism works even though the earlier pass could not find a
 * /BOOTLOG.TXT file afterwards. Those are two different claims and only the
 * second one is in doubt.
 *
 * (#sqearlyexit) AND THE REASON THE FILE WRITE FAILS IS NOW MEASURED, not
 * inferred: sys_open(O_WRONLY|O_CREAT|O_TRUNC) returns -13 (EACCES). It is
 * NOT a Squadron bug and NOT specific to /SQUADRON. A build with the
 * SQ_BOOTTRACE probe below tried four destinations from a normal uid=1000
 * launch on the shipping golden and got -13 from every one of them:
 *   /SQUADRON/FPSLOG.TXT  -13
 *   /SQPROBE.TXT          -13   (the ext2 root itself)
 *   /HOME/SQPROBE.TXT     -13
 *   /NOTES/SQPROBE.TXT    -13
 * /CONFIG/PERMS.DB ships `/:0:0:0755` and `/HOME:0:0:0755`, apps run as uid
 * 1000, and the image carries no /CONFIG/PASSWD, so there is no per-user home
 * that resolves to anything but "/". A Ring-3 app therefore has NOWHERE it
 * can create a file, which is a system-wide property worth knowing before
 * anyone else writes "the log file says X". The file write is kept, not
 * deleted: if that policy is ever changed it starts working again for free,
 * and deleting it would erase the evidence. What is NEW here is that it can
 * no longer fail silently, see fps_dead_once(). */
static void fps_flush(void) {
    int fd = sys_open(FPS_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        long w = sys_write(fd, g_fps_log, (unsigned long)g_fps_log_len);
        sys_close(fd);
        if (w == (long)g_fps_log_len) return;
        fps_dead_once(FPS_DEAD_WRITE, w);
        return;
    }
    /* #sqearlyexit: THE RETURN VALUE WAS NEVER CHECKED, which is the whole
     * reason this log could be dead for months while its own comment claimed
     * it wrote "a small measured FPS log to disk instead of guessing". A
     * diagnostic that cannot report its own failure is worse than no
     * diagnostic: it makes every downstream claim of the form "the FPS log
     * says X" unfalsifiable. Report it ONCE per run (this runs once a
     * second; a per-call line would drown the console) down the one channel
     * measured to work from a Ring-3 app, fd 1, which the kernel routes to
     * the serial console and the syslog for any process with no PTY. */
    fps_dead_once(FPS_DEAD_OPEN, fd);
}
static void fps_bootlog(int fps, unsigned snd) {
    char line[80], n[24]; int i = 0;
    const char *p = "[SQPERF] fps="; while (*p) line[i++] = *p++;
    num_to_str((long)fps, n); p = n; while (*p) line[i++] = *p++;
    p = " snd="; while (*p) line[i++] = *p++;
    num_to_str((long)snd, n); p = n; while (*p) line[i++] = *p++;
    p = " audio="; while (*p) line[i++] = *p++;
    p = g_snd_started ? "on" : "off"; while (*p) line[i++] = *p++;
    line[i] = 0;
    syscall1(SYS_BOOTLOG_WRITE, (long)line);
}
/* #it20-fix, third attempt, and the one that actually lands. Neither on-disk
 * path works for this app on the shipping image: sys_open(O_CREAT) on the
 * ext2 root leaves /SQUADRON/FPSLOG.TXT unwritten even when the file is
 * pre-created, and SYS_BOOTLOG_WRITE produced no /BOOTLOG.TXT on the FAT ESP
 * either. What DOES work is fd 1: a terminal-launched app's stdout is the
 * terminal, which is on screen and therefore screenshottable. So the run
 * summary - total frames, total wall-clock seconds, the derived mean
 * framerate, and the audio worker's block rate - is written straight to fd 1
 * on the way out. One line per run, printed at every exit path, which is
 * exactly the granularity an audio-on-versus-off comparison needs. */
static long     g_frames_total = 0;
static unsigned g_perf_t0 = 0;
/* #it20-fix: main() has three early-return paths that all exit silently, and
 * a silent immediate exit was observed three times across six verification
 * boots (see docs/SQUADRON_ITERATIONS.md "Deferred / next up"). The MISSING
 * [SQPERF] summary on those runs already proves the failure happens before
 * the first rendered frame, which narrows it to exactly these three; naming
 * which one turns the next recurrence from a mystery into a fact. Cold paths
 * only: on a successful launch none of these ever runs. */
static void die_msg(const char *why) {
    char b[96]; int i = 0;
    const char *p = "\n[SQFAIL] "; while (*p) b[i++] = *p++;
    while (*why && i < 90) b[i++] = *why++;
    b[i++] = '\n'; b[i] = 0;
    sys_write(1, b, (unsigned long)i);
}
/* #sqearlyexit: startup milestone trace. Compiled ONLY under -DSQ_BOOTTRACE
 * (never in a shipping build, the Makefile does not define it), so a golden
 * SQUADRON contains none of this. It exists because the silent early exit
 * this is chasing leaves NO output at all: [SQPERF] missing proves only
 * "before the first frame", which is ~everything main() does. One labelled
 * line per milestone on fd 1 turns that into a file:line. */
#ifdef SQ_BOOTTRACE
static void bt(const char *tag, long v1, long v2) {
    char b[128], n[24]; int i = 0;
    const char *p = "[SQBOOT] "; while (*p) b[i++] = *p++;
    while (*tag) b[i++] = *tag++;
    b[i++] = ' '; num_to_str(v1, n); p = n; while (*p) b[i++] = *p++;
    b[i++] = ' '; num_to_str(v2, n); p = n; while (*p) b[i++] = *p++;
    b[i++] = ' '; num_to_str((long)uptime_ms(), n); p = n; while (*p) b[i++] = *p++;
    b[i++] = '\n'; b[i] = 0;
    sys_write(1, b, (unsigned long)i);
}
#else
#define bt(a,b,c) ((void)0)
#endif
static void perf_summary(void) {
    if (!g_perf_t0) {
        /* #sqearlyexit: A RUN THAT DREW NO FRAME IS THE RUN THIS LINE IS MOST
         * NEEDED FOR, and it was the one case that printed nothing at all.
         * g_perf_t0 is set by the first fps_tick(), i.e. after the first
         * completed frame, so every exit path taken before that returned here
         * in silence: no window on screen, no error, no [SQPERF]. That is
         * precisely the reported symptom, and the diagnostic meant to explain
         * it was itself silent on the only path that produces it. Say so
         * instead. */
        const char *m = "\n[SQPERF] exited before the first rendered frame: "
                        "0 frames, no timing. See any [SQFAIL] line above for "
                        "the reason.\n";
        char b[160]; int i = 0; while (m[i] && i < 158) { b[i] = m[i]; i++; }
        b[i] = 0;
        sys_write(1, b, (unsigned long)i);
        return;
    }
    unsigned el = g_now > g_perf_t0 ? g_now - g_perf_t0 : 1;
    char b[160], n[24]; int i = 0;
    const char *p = "\n[SQPERF] audio="; while (*p) b[i++] = *p++;
    p = g_snd_started ? "ON " : "OFF"; while (*p) b[i++] = *p++;
    p = " frames="; while (*p) b[i++] = *p++;
    num_to_str(g_frames_total, n); p = n; while (*p) b[i++] = *p++;
    p = " ms="; while (*p) b[i++] = *p++;
    num_to_str((long)el, n); p = n; while (*p) b[i++] = *p++;
    p = " fps="; while (*p) b[i++] = *p++;
    num_to_str(g_frames_total * 1000 / (long)el, n); p = n; while (*p) b[i++] = *p++;
    p = " sndblocks="; while (*p) b[i++] = *p++;
    num_to_str((long)g_snd_blocks, n); p = n; while (*p) b[i++] = *p++;
    p = " sndps="; while (*p) b[i++] = *p++;
    num_to_str((long)g_snd_blocks * 1000 / (long)el, n); p = n; while (*p) b[i++] = *p++;
    b[i++] = '\n'; b[i] = 0;
    sys_write(1, b, (unsigned long)i);
}
static void fps_tick(void) {
    g_fps_win_frames++; g_frames_total++;
    if (!g_perf_t0) g_perf_t0 = g_now;
    if (g_fps_win_start == 0) g_fps_win_start = g_now;
    unsigned el = g_now - g_fps_win_start;
    if (el < 1000) return;
    g_perf_fps = (int)((long)g_fps_win_frames * 1000 / (el ? el : 1));
    unsigned nb = g_snd_blocks, sb = nb - g_snd_blocks_prev; g_snd_blocks_prev = nb;
    if (g_fps_samples < FPS_MAX_SAMPLES) {
        /* #it20: the audio worker's block rate goes in the SAME line as the
         * framerate, because the two numbers only mean anything together. At
         * 512 frames / 22050 Hz a healthy worker completes ~43 blocks a
         * second: far above that and the blocking write is returning without
         * blocking, i.e. the loop has become an unpaced spin; far below and
         * the sink is starving it. snd=0 means the worker never started
         * (no device, --nosound, or pthread_create failed), which is also
         * exactly the control condition for the with/without framerate
         * comparison. */
        fps_append("t="); fps_append_num((long)g_now); fps_append(" fps="); fps_append_num(g_perf_fps);
        fps_append(" snd="); fps_append_num((long)sb); fps_append("\n");
        g_fps_samples++; fps_flush();
    }
    fps_bootlog(g_perf_fps, sb);   /* #it20-fix: the path that actually lands on disk */
    g_fps_win_frames = 0; g_fps_win_start = g_now;
}

/* ===================================================== side-panel HUD boxes */
/* Measured (2026-07-19, #475) from the actual SIDEBARL/R.BMP art at its
 * native 400x900 resolution via connected-component analysis of the dark box
 * interiors (ImageMagick to PNG + scipy.ndimage.label): each panel bakes in
 * 6 evenly-spaced dark box outlines that used to sit empty while the HUD drew
 * its own wide rectangles near the top, disconnected from the artwork.
 * Coordinates are box-interior [x0,y0,x1,y1] in ART pixel space; SB_ART_W/H
 * is the art's native resolution (both panels share it). box_rect_dest()
 * scales a box into wherever the panel actually lands on screen, using the
 * same forward mapping sidebar_blit() uses to paint the art itself, so the
 * text can never drift off the real box regardless of window size. */
#define SB_ART_W 400
#define SB_ART_H 900
static const int SB_BOX_L[6][4] = {
    { 212,  62, 374, 154 }, { 212, 200, 374, 291 }, { 212, 337, 374, 427 },
    { 212, 473, 374, 564 }, { 212, 611, 374, 700 }, { 212, 747, 374, 838 },
};
static const int SB_BOX_R[6][4] = {
    { 40,  52, 224, 137 }, { 40, 191, 224, 278 }, { 40, 332, 224, 417 },
    { 40, 472, 224, 559 }, { 40, 612, 224, 698 }, { 40, 753, 224, 840 },
};
typedef struct { int x, y, w, h; uint32_t *clean; } HudBox;
static HudBox g_boxL[6], g_boxR[6], g_toastR;
/* Sidebar-cache key: rebuilt only when the panel geometry changes (a resize);
 * -1 sentinels force the build on the first frame. This IS the framerate fix
 * (see the file header comment): the panel art no longer gets re-scaled into
 * g_present every frame, only when this key is stale. */
static int g_sb_lw = -1, g_sb_rx = -1, g_sb_rw = -1, g_sb_fbh = -1;
static void sidebar_blit(const Sprite *s, int x0, int y0, int pw, int ph);   /* fwd, defined below */

/* A recessed dark "readout slot": used only by the PROCEDURAL sidebar
 * fallback (no SIDEBARL/R.BMP on disk), which has no baked-in box art to draw
 * text over, and by the transient pickup-name toast. */
static void hud_slot(int x, int y, int w, int h) {
    fill_rect_abs(x, y, w, h, 0x00080B12);
    fill_rect_abs(x, y, w, 1, 0x00000000);
    fill_rect_abs(x, y + h - 1, w, 1, 0x00303A4C);
    fill_rect_abs(x, y, 1, h, 0x00000000);
    fill_rect_abs(x + w - 1, y, 1, h, 0x00303A4C);
}
/* Tiny upward-pointing ship glyph for the lives row (present-buffer coords). */
static void ship_icon_abs(int cx, int cy, uint32_t col) {
    for (int y = 0; y < 10; y++) { int span = y * 6 / 10; fill_rect_abs(cx - span, cy - 5 + y, span * 2 + 1, 1, col); }
}
static void box_rect_dest(const int b[4], int x0, int y0, int pw, int ph, int *ox, int *oy, int *ow, int *oh) {
    int rx0 = x0 + b[0] * pw / SB_ART_W, ry0 = y0 + b[1] * ph / SB_ART_H;
    int rx1 = x0 + b[2] * pw / SB_ART_W, ry1 = y0 + b[3] * ph / SB_ART_H;
    *ox = rx0; *oy = ry0; *ow = rx1 - rx0; *oh = ry1 - ry0;
}
static void box_set(HudBox *b, int x, int y, int w, int h) {
    if (b->clean && (b->w != w || b->h != h)) { free(b->clean); b->clean = 0; }
    b->x = x; b->y = y; b->w = w; b->h = h;
    if (!b->clean && w > 0 && h > 0) b->clean = (uint32_t *)malloc((unsigned long)w * (unsigned long)h * 4);
}
/* Snapshot the panel-art pixels currently under a box (no HUD text on them
 * yet) so the per-frame draw can cheaply restore-then-redraw-text instead of
 * re-blitting the whole (static) panel every frame. If the clean-snapshot
 * malloc ever fails, box_restore() below still clears the box procedurally
 * before text is drawn (#hud-smear investigation), so a bad allocation can
 * never leave stale digits visible under new ones. */
static void box_snapshot(HudBox *b) {
    if (!b->clean) return;
    for (int y = 0; y < b->h; y++) {
        int py = b->y + y; uint32_t *dst = b->clean + (long)y * b->w;
        if ((unsigned)py >= (unsigned)FBH) { for (int x = 0; x < b->w; x++) dst[x] = 0; continue; }
        const uint32_t *src = g_present + (long)py * FBW;
        for (int x = 0; x < b->w; x++) { int px = b->x + x; dst[x] = ((unsigned)px < (unsigned)FBW) ? src[px] : 0; }
    }
}
/* Restore a box to its clean (textless) state before the caller redraws its
 * label/value. If no clean snapshot exists (the malloc in box_set() failed,
 * or box_snapshot() was never able to run), fall back to painting the same
 * recessed dark slot the procedural sidebar fallback uses (hud_slot()) so the
 * box is still cleared before new text lands, rather than silently no-op'ing
 * and letting the old digits show through the new ones (#hud-smear). */
static void box_restore(HudBox *b) {
    if (!b->clean) { hud_slot(b->x, b->y, b->w, b->h); return; }
    for (int y = 0; y < b->h; y++) {
        int py = b->y + y; if ((unsigned)py >= (unsigned)FBH) continue;
        uint32_t *dst = g_present + (long)py * FBW; const uint32_t *src = b->clean + (long)y * b->w;
        for (int x = 0; x < b->w; x++) { int px = b->x + x; if ((unsigned)px >= (unsigned)FBW) continue; dst[x] = src[x]; }
    }
}
/* (Re)paint both full sidebar panels into g_present and cache the 12 measured
 * box rects + their clean (textless) pixels, plus the pickup-toast gap.
 * Expensive (the per-pixel scale in sidebar_blit), so this only runs once
 * (first frame) and again on resize - never every frame. */
static void rebuild_sidebars(int lw, int rx, int rw) {
    sidebar_blit(&A_sidebarL, 0, 0, lw, FBH);
    sidebar_blit(&A_sidebarR, rx, 0, rw, FBH);
    for (int i = 0; i < 6; i++) {
        int x, y, w, h; box_rect_dest(SB_BOX_L[i], 0, 0, lw, FBH, &x, &y, &w, &h);
        if (!A_sidebarL.ok) hud_slot(x, y, w, h);
        box_set(&g_boxL[i], x, y, w, h); box_snapshot(&g_boxL[i]);
    }
    for (int i = 0; i < 6; i++) {
        int x, y, w, h; box_rect_dest(SB_BOX_R[i], rx, 0, rw, FBH, &x, &y, &w, &h);
        if (!A_sidebarR.ok) hud_slot(x, y, w, h);
        box_set(&g_boxR[i], x, y, w, h); box_snapshot(&g_boxR[i]);
    }
    { int gy0 = g_boxR[1].y + g_boxR[1].h + 4, gh = (g_boxR[2].y - 2) - gy0; if (gh < 8) gh = 8;
      box_set(&g_toastR, rx + 12, gy0, rw - 24, gh); box_snapshot(&g_toastR); }
    g_sb_lw = lw; g_sb_rx = rx; g_sb_rw = rw; g_sb_fbh = FBH;
}

/* Largest text scale (down to 1) that fits `maxw` px, so a big SCORE/HIGH
 * SCORE number shrinks to fit its (narrower than the old top rectangles) box
 * instead of overflowing it. */
static int fit_scale(const char *s, int maxw, int prefer) {
    for (int sc = prefer; sc > 1; sc--) if (text_w(s, sc) <= maxw) return sc;
    return 1;
}
/* label (small, top) + value (larger, auto-fit), vertically CENTERED as a
 * block in the box rather than riding high near the top. LABEL_H/GAP are the
 * fixed 8px glyph height (scale 1) and the small breathing room between the
 * label and the value; VH is the value's glyph height at whatever scale
 * fit_scale() picked, so the centering offset is derived from the box's
 * actual height and the actual text metrics, not a magic constant. */
static void box_label_value(HudBox *b, const char *label, const char *value, uint32_t lcol, uint32_t vcol) {
    int cx = b->x + b->w / 2;
    int sc = fit_scale(value, b->w - 8, 2);
    const int LABEL_H = 8, GAP = 3;
    int VH = 8 * sc;
    int block_h = LABEL_H + GAP + VH;
    int top = b->y + (b->h - block_h) / 2;
    if (top < b->y + 2) top = b->y + 2;   /* never clip above the box */
    sq_text_abs_center(cx, top, label, lcol, 1);
    sq_text_abs_center(cx, top + LABEL_H + GAP, value, vcol, sc);
}
static Enemy *find_boss(void) { for (int i = 0; i < MAX_EN; i++) if (g_en[i].alive && g_en[i].boss) return &g_en[i]; return 0; }

/* HUD readouts drawn INTO the 6 measured boxes baked into each side-panel's
 * art (#475). Box order:
 *   LEFT:  1 SCORE  2 STAGE  3 WEAPON  4 POWER(level)  5 DESTROYED(kills)
 *          6 COMPLETE(stage %)
 *   RIGHT: 1 LIVES  2 BOMBS(+control hint)  3 FPS(live)  4 MULTIPLIER(combo)
 *          5 ACCURACY  6 BOSS health bar when a boss is on screen, else
 *            SHIELD countdown, else a HULL bar (lives fraction)
 * Every box is restored from its clean snapshot first (rebuild_sidebars)
 * so a changed value never smears into the old one. Absolute coords over
 * g_present. */
static void draw_side_hud(int lw, int rx, int rw) {
    if (g_state == GS_MENU) return;
    (void)lw;
    char buf[32], val[48];

    /* -------- LEFT: SCORE / STAGE / WEAPON / POWER / DESTROYED / COMPLETE -- */
    box_restore(&g_boxL[0]); num_to_str(g_score, buf); box_label_value(&g_boxL[0], "SCORE", buf, 0x0090C0FF, 0x00FFFFFF);
    box_restore(&g_boxL[1]); num_to_str(g_stage, buf); box_label_value(&g_boxL[1], "STAGE", buf, 0x0090C0FF, 0x00FFE060);
    box_restore(&g_boxL[2]);
    { int rapid = g_now < g_rapid_until;
      const char *wn = g_weapon == WP_SINGLE ? "SHOT" : g_weapon == WP_TWIN ? "TWIN" : g_weapon == WP_TRI ? "TRI" : g_weapon == WP_WIDE ? "WIDE" : g_weapon == WP_LASER ? "LASER" : g_weapon == WP_MISSILE ? "MISSILE" : "WAVE";
      box_label_value(&g_boxL[2], rapid ? "RAPID!" : "WEAPON", wn, rapid ? 0x00FFE000 : 0x0090C0FF, 0x0080FFFF); }
    box_restore(&g_boxL[3]);
    { strcpy(val, "LV "); char d[4]; d[0] = (char)('0' + g_wlevel); d[1] = '/'; d[2] = '3'; d[3] = 0; strcat(val, d);
      box_label_value(&g_boxL[3], "POWER", val, 0x0090C0FF, 0x00FFC060); }
    box_restore(&g_boxL[4]); num_to_str(g_kills, buf); box_label_value(&g_boxL[4], "DESTROYED", buf, 0x0090C0FF, 0x00FF9060);
    box_restore(&g_boxL[5]);
    { int pct = g_stage_spawned > 0 ? (g_stage_killed * 100) / g_stage_spawned : 0; if (pct > 100) pct = 100;
      num_to_str(pct, buf); strcat(buf, "%"); box_label_value(&g_boxL[5], "COMPLETE", buf, 0x0090C0FF, 0x0060FF90); }

    /* -------- RIGHT: LIVES / BOMBS / FPS / MULTIPLIER / ACCURACY / BOSS-SHIELD */
    /* LIVES/BOMBS have a 3rd row (ship icons / the [RMB]/B hint) pinned to the
     * bottom of the box, so the label+value pair is centered in the area
     * ABOVE that reserved bottom row rather than in the full box height -
     * same "derive from box height + text metrics" approach as
     * box_label_value(), just with a bottom row carved out first (#hud-smear
     * centering pass). */
    box_restore(&g_boxR[0]);
    { int cx = g_boxR[0].x + g_boxR[0].w / 2;
      int bottom_reserve = 18;   /* ship-icon row */
      int avail_h = g_boxR[0].h - bottom_reserve; int block_h = 8 + 3 + 16;
      int top = g_boxR[0].y + (avail_h - block_h) / 2; if (top < g_boxR[0].y + 2) top = g_boxR[0].y + 2;
      sq_text_abs_center(cx, top, "LIVES", 0x0090C0FF, 1);
      num_to_str(g_lives, buf); strcpy(val, "x "); strcat(val, buf);
      sq_text_abs_center(cx, top + 11, val, 0x00FFFFFF, 2);
      int n = g_lives > 6 ? 6 : (g_lives < 0 ? 0 : g_lives);
      for (int i = 0; i < n; i++) ship_icon_abs(cx - n * 8 + i * 16 + 8, g_boxR[0].y + g_boxR[0].h - 12, 0x0080C8FF); }
    box_restore(&g_boxR[1]);
    { int cx = g_boxR[1].x + g_boxR[1].w / 2;
      int bottom_reserve = 16;   /* [RMB]/B hint row */
      int avail_h = g_boxR[1].h - bottom_reserve; int block_h = 8 + 3 + 16;
      int top = g_boxR[1].y + (avail_h - block_h) / 2; if (top < g_boxR[1].y + 2) top = g_boxR[1].y + 2;
      sq_text_abs_center(cx, top, "BOMBS", 0x0090C0FF, 1);
      num_to_str(g_bombs, buf); strcpy(val, "x "); strcat(val, buf);
      sq_text_abs_center(cx, top + 11, val, 0x00FF80FF, 2);
      /* discoverability: the actual trigger, right under the count (#475) */
      sq_text_abs_center(cx, g_boxR[1].y + g_boxR[1].h - 12, "[RMB]/B", 0x0090A0B0, 1); }
    box_restore(&g_boxR[2]);
    { num_to_str((long)g_perf_fps, buf); box_label_value(&g_boxR[2], "FPS", buf, 0x0090C0FF, g_perf_fps > 0 && g_perf_fps < 30 ? 0x00FF6060 : 0x0060FF90); }
    box_restore(&g_boxR[3]);
    { strcpy(val, "x"); char d[4]; num_to_str(combo_mult(), d); strcat(val, d);
      box_label_value(&g_boxR[3], "MULTIPLIER", val, 0x0090C0FF, 0x00FFE060);
      /* #it16: a decay bar pinned to the bottom of the box, showing how much
       * of the combo window is left before the multiplier steps DOWN a tier.
       * Without it the decay would be invisible until the number silently
       * dropped, which is exactly the kind of unexplained punishment that
       * makes a scoring mechanic feel arbitrary instead of tense. It turns
       * amber then red as the window runs out. Only drawn while a combo is
       * actually running, so the box looks unchanged in normal play. */
      if (g_combo > 0 && g_combo_until > g_now) {
          unsigned w = combo_window(); unsigned rem = g_combo_until - g_now;
          if (rem > w) rem = w;
          int bx2 = g_boxR[3].x + 8, bw2 = g_boxR[3].w - 16;
          int by2 = g_boxR[3].y + g_boxR[3].h - 8;
          int fw = bw2 * (int)rem / (int)(w ? w : 1);
          uint32_t c = rem * 3 < w ? 0x00FF4040 : (rem * 3 < w * 2 ? 0x00FFB040 : 0x0060FF90);
          fill_rect_abs(bx2, by2, bw2, 4, 0x00181C24);
          fill_rect_abs(bx2, by2, fw, 4, c);
      } }
    box_restore(&g_boxR[4]);
    { long fired = g_shots_fired, hit = g_shots_hit > fired ? fired : g_shots_hit;
      int acc = fired > 0 ? (int)(hit * 100 / fired) : 0;
      num_to_str(acc, buf); strcat(buf, "%"); box_label_value(&g_boxR[4], "ACCURACY", buf, 0x0090C0FF, 0x0080D0FF); }
    box_restore(&g_boxR[5]);
    { Enemy *boss = find_boss(); int cx = g_boxR[5].x + g_boxR[5].w / 2;
      int bw = g_boxR[5].w - 16, bx = g_boxR[5].x + 8;
      /* label (8px) + gap + a fixed-height bar/value row, centered as a block
       * the same way box_label_value() centers label+value (#hud-smear
       * centering pass); the bar/value row's own height stands in for VH. */
      int label_top = g_boxR[5].y + (g_boxR[5].h - (8 + 3 + 10)) / 2;
      if (label_top < g_boxR[5].y + 2) label_top = g_boxR[5].y + 2;
      int by = label_top + 8 + 3 + 5;   /* vertical center of the 10px-tall bar row */
      if (boss) {
          sq_text_abs_center(cx, label_top, "BOSS", 0x00FF9090, 1);
          fill_rect_abs(bx - 1, by - 1, bw + 2, 10, 0x00202028);
          int fillw = boss->maxhp > 0 ? bw * (boss->hp > 0 ? boss->hp : 0) / boss->maxhp : 0;
          fill_rect_abs(bx, by, fillw, 8, 0x00FF4060);
      } else if (g_now < g_shield_until) {
          sq_text_abs_center(cx, label_top, "SHIELD", 0x0090C0FF, 1);
          num_to_str((g_shield_until - g_now) / 1000 + 1, buf); strcat(buf, "s");
          sq_text_abs_center(cx, label_top + 11, buf, 0x0040C0FF, 2);
      } else {
          sq_text_abs_center(cx, label_top, "HULL", 0x0090C0FF, 1);
          int frac = g_lives > 6 ? 6 : (g_lives < 0 ? 0 : g_lives);
          fill_rect_abs(bx - 1, by - 1, bw + 2, 10, 0x00202028);
          fill_rect_abs(bx, by, bw * frac / 6, 8, 0x0060FF90);
      } }

    /* transient pickup-name toast: floats in the gap between the BOMBS and
     * FPS boxes, same relative spot it occupied before this rework. */
    box_restore(&g_toastR);
    if (g_now < g_pu_label_until && g_pu_label && g_toastR.w > 0) {
        hud_slot(g_toastR.x, g_toastR.y, g_toastR.w, g_toastR.h);
        sq_text_abs_center(rx + rw / 2, g_toastR.y + g_toastR.h / 2 - 3, g_pu_label, hsv((int)(g_now / 6) % 360, 255, 255), 1);
    }
}

static void draw_playfield(void) {
    draw_background();
    for (int i = 0; i < MAX_PU; i++) if (g_pu[i].alive) draw_powerup(&g_pu[i]);
    for (int i = 0; i < MAX_PB; i++) if (g_pb[i].alive) draw_bullet_p(&g_pb[i]);
    for (int i = 0; i < MAX_EN; i++) if (g_en[i].alive) draw_enemy(&g_en[i]);
    for (int i = 0; i < MAX_EB; i++) if (g_eb[i].alive) draw_bullet_e(&g_eb[i]);
    draw_player();
    for (int i = 0; i < MAX_EX; i++) if (g_ex[i].alive) draw_explosion(&g_ex[i]);
    if (g_hit_flash > 0) blend_rect(0, 0, W, H, 0x00FF0000, g_hit_flash * 12);
    if (g_bomb_flash > 0) blend_rect(0, 0, W, H, 0x00FFFFFF, g_bomb_flash * 14);
    /* #it5 (iteration 5, last-life warning): a pulsing red edge vignette while
     * g_lives == 1, so "one more hit ends the run" is readable at a glance
     * without having to read the LIVES HUD box. Four thin edge strips, not a
     * full-screen tint, so it never competes with reading bullets in the
     * middle of the playfield - the exact same "never hide a bullet"
     * constraint pxbg_draw()'s comment already applies to the backdrop. */
    if (g_state == GS_PLAYING && g_lives == 1) {
        /* Tuned up from an initial 40+/-30 alpha pass (#it5 v1): a screendump
         * at a confirmed 1-life frame (docs/SQUADRON_ITERATIONS.md, iteration
         * 5) showed that range was too close to the backdrop's own magenta
         * nebula blobs to read as a deliberate warning rather than scenery.
         * A two-band strip (a near-opaque 8px outer edge + a softer 16px
         * inner taper) reads as an unmistakable frame around the play area
         * at any point in the pulse, not just at its peak. */
        int puls = (int)(90.0f + 60.0f * fsinf((float)g_now * 0.006f));
        int outer = puls + 90; if (outer > 235) outer = 235;
        int th_out = 8, th_in = 16;
        blend_rect(0, 0, W, th_out, 0x00FF2020, outer);
        blend_rect(0, th_out, W, th_in, 0x00FF2020, puls);
        blend_rect(0, H - th_out, W, th_out, 0x00FF2020, outer);
        blend_rect(0, H - th_out - th_in, W, th_in, 0x00FF2020, puls);
        blend_rect(0, 0, th_out, H, 0x00FF2020, outer);
        blend_rect(th_out, 0, th_in, H, 0x00FF2020, puls);
        blend_rect(W - th_out, 0, th_out, H, 0x00FF2020, outer);
        blend_rect(W - th_out - th_in, 0, th_in, H, 0x00FF2020, puls);
    }
    /* #it17: name the cost of the death that just happened, in the playfield,
     * where the player is already looking. Suppressed during the stage intro
     * card (a fresh stage's entrance is not a death) by only drawing it when
     * the card is not up. */
    if (g_state == GS_PLAYING && g_now < g_respawn_until && g_now >= g_intro_until) {
        char b[24], line[32]; num_to_str(g_lives, b);
        strcpy(line, "LIVES LEFT "); strcat(line, b);
        sq_text_center(W / 2, H / 2 + 30, line, g_lives <= 1 ? 0x00FF6060 : 0x00FFE060, 2);
    }
    /* #it14: stage intro card. Drawn LAST of the overlays so nothing can be
     * painted over it, and only while g_intro_until is in the future. It fades
     * its own panel in and out over the first and last 300ms rather than
     * popping, which is what makes it read as a title card rather than a
     * dropped frame. Every coordinate is derived from W/H, so it is correct at
     * the 380px playfield floor and at the 620px ceiling alike. */
    if (g_now < g_intro_until) {
        unsigned rem = g_intro_until - g_now;
        unsigned el  = (rem < INTRO_MS) ? (INTRO_MS - rem) : 0;
        int a = 255;
        if (el < 300)      a = (int)(el * 255 / 300);
        else if (rem < 300) a = (int)(rem * 255 / 300);
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        int ph = 168, py = H / 2 - ph / 2 - 20, pw = W - 48, pxx = 24;
        blend_rect(pxx, py, pw, ph, 0x00060A14, a * 210 / 255);
        blend_rect(pxx, py, pw, 3, 0x0060C8FF, a);
        blend_rect(pxx, py + ph - 3, pw, 3, 0x0060C8FF, a);
        char b[24], line[40];
        num_to_str(g_stage, b); strcpy(line, "STAGE "); strcat(line, b);
        sq_text_a(W / 2 - text_w(line, 4) / 2, py + 22, line, 0x00FFFFFF, 4, a);
        const char *nm = stage_sector_name(g_stage);
        sq_text_a(W / 2 - text_w(nm, 2) / 2, py + 66, nm, 0x0080E0FF, 2, a);
        const char *nt = stage_new_threat(g_stage);
        if (nt) sq_text_a(W / 2 - text_w(nt, 2) / 2, py + 98, nt, 0x00FF8040, 2, a);
        else {
            /* No unlock this stage: show the pressure that IS rising, so the
             * card always carries information rather than blank space. */
            int spd = (int)(stage_speed_mult() * 100.0f + 0.5f);
            num_to_str(spd, b); strcpy(line, "ENEMY SPEED "); strcat(line, b); strcat(line, "%");
            sq_text_a(W / 2 - text_w(line, 2) / 2, py + 98, line, 0x00A0B0C0, 2, a);
        }
        num_to_str(g_lives, b); strcpy(line, "LIVES "); strcat(line, b);
        sq_text_a(W / 2 - text_w(line, 2) / 2, py + 130, line, 0x0090FFB0, 2, a);
    }
    /* HUD is rendered onto the sidebars at present time (draw_side_hud). */
    if (g_now < g_banner_until) { char b[24], line[40]; num_to_str(g_stage, b); strcpy(line, "STAGE "); strcat(line, b); if (g_boss_active) strcpy(line, g_boss_name_txt);   /* #it15: names the archetype */ sq_text_center(W / 2, H / 2 - 40, line, g_boss_active ? 0x00FF5050 : 0x00FFFFFF, 3); }
    /* #it3: boss phase-transition telegraph banner (see update_world()). Drawn
     * independently of the stage/boss-warning banner above since the two can
     * never legitimately overlap in time (a phase change only fires once a
     * boss the stage banner already announced is well into the fight). */
    if (g_now < g_phase_banner_until) sq_text_center(W / 2, H / 2 - 40, g_phase_banner_txt, 0x00FF7040, 2);
    /* #it7: streak popup, positioned above the stage/phase banner line so the
     * two can never overlap even if a kill happens to land in the same
     * instant as either (both of those are rarer, brief events too). */
    if (g_now < g_streak_until) sq_text_center(W / 2, H / 2 - 90, g_streak_txt, hsv((int)(g_now / 5) % 360, 255, 255), 2);
}

/* ============================================================ menus ======= */
static const char *diff_name(void) { return g_difficulty == 0 ? "EASY" : g_difficulty == 2 ? "HARD" : "NORMAL"; }
static void draw_menu(void) {
    draw_background();
    if (A_logo.ok) { int lw = A_logo.w, lh = A_logo.h; if (lw > W - 80) { lh = lh * (W - 80) / lw; lw = W - 80; } spr_blit_scaled_c(&A_logo, W / 2, H / 4, lw, lh); }
    else { sq_text_center(W / 2, H / 6, "MAYTERA", hsv((int)(g_now / 10) % 360, 255, 255), 5); sq_text_center(W / 2, H / 6 + 54, "SQUADRON", 0x00FFE060, 5); }
    /* #it20: SOUND joins DIFFICULTY as a persistent title-screen option, so
     * the new audio is something a player can turn off without quitting the
     * app. MENU_ITEMS is derived once instead of a 3 repeated in four places,
     * which is how this menu's bounds and its draw loop would otherwise have
     * drifted apart the moment anyone added an entry. */
    int y = H / 2 + 24; char line[40], sline[32]; const char *items[MENU_ITEMS];
    items[0] = "PLAY"; strcpy(line, "DIFFICULTY: "); strcat(line, diff_name()); items[1] = line;
    strcpy(sline, "SOUND: "); strcat(sline, g_snd_started ? (g_snd_on ? "ON" : "OFF") : "N/A"); items[2] = sline;
    items[3] = "QUIT";
    for (int i = 0; i < MENU_ITEMS; i++) { uint32_t c = (i == g_menu_sel) ? 0x00FFFF80 : 0x00A0A0B0; const char *txt = items[i]; int tw = text_w(txt, 3);
        if (i == g_menu_sel) sq_text_sh(W / 2 - tw / 2 - 40, y + i * 40, ">", 0x00FFFF80, 3);
        sq_text_center(W / 2, y + i * 40, txt, c, 3); }
    sq_text_center(W / 2, H - 90, "MOUSE MOVES SHIP   HOLD LMB/SPACE TO FIRE   RMB/B = BOMB", 0x0090A0B0, 1);
    sq_text_center(W / 2, H - 68, "ENTER/CLICK SELECT   ESC QUITS", 0x0070808F, 1);
}
/* #it19 (pause screen): PAUSE was one word and one hint line over a dimmed
 * playfield. Two things were missing from it, and pause is the only moment in
 * the game where there is time to read either.
 *  - THE RUN SO FAR. The six left-hand and six right-hand sidebar boxes each
 *    show one live number, which is right during play (glanceable) and wrong
 *    when stopped (scattered across two panels, none of them next to each
 *    other). A paused player wants the run as a single block.
 *  - THE CONTROLS. They appear exactly once, in 1-scale text at the bottom of
 *    the title screen, and are never reachable again without quitting to the
 *    menu. A player who forgot that RMB drops a bomb, or who never saw the
 *    title screen because the game auto-started, had no way to find out.
 * Everything is positioned off W/H, so it is correct at compute_layout()'s
 * 380px playfield floor and its 620px ceiling alike. */
static void pause_row(int px0, int pw, int y, const char *label, const char *value, uint32_t vc) {
    sq_text_sh(px0 + 14, y, label, 0x0090A8C0, 2);
    sq_text_sh(px0 + pw - 14 - text_w(value, 2), y, value, vc, 2);
}
static void draw_pause(void) {
    draw_playfield();
    blend_rect(0, 0, W, H, 0x00000010, 175);
    int pw = W - 36, px0 = 18, ph = 430, py = (H - ph) / 2;
    blend_rect(px0, py, pw, ph, 0x00060A14, 225);
    fill_rect(px0, py, pw, 3, 0x0060C8FF);
    fill_rect(px0, py + ph - 3, pw, 3, 0x0060C8FF);
    fill_rect(px0, py, 3, ph, 0x002A4460);
    fill_rect(px0 + pw - 3, py, 3, ph, 0x002A4460);
    sq_text_center(W / 2, py + 18, "PAUSED", 0x00FFFFFF, 4);

    char b[24], v[40];
    int y = py + 66, dy = 22;
    num_to_str(g_stage, b); pause_row(px0, pw, y, "STAGE", b, 0x00FFE060); y += dy;
    /* the sector name gets its own full-width row: it is too long to right-
     * align against a label at scale 2 in a 380px-floor playfield. */
    sq_text_center(W / 2, y, stage_sector_name(g_stage), 0x0080E0FF, 2); y += dy;
    num_to_str(g_score, b);      pause_row(px0, pw, y, "SCORE", b, 0x00FFFFFF); y += dy;
    num_to_str(g_kills, b);      pause_row(px0, pw, y, "DESTROYED", b, 0x00FF9060); y += dy;
    { long fired = g_shots_fired, hit = g_shots_hit > fired ? fired : g_shots_hit;
      num_to_str(fired > 0 ? (long)(hit * 100 / fired) : 0, b); strcat(b, "%");
      pause_row(px0, pw, y, "ACCURACY", b, 0x0080D0FF); } y += dy;
    strcpy(v, "x"); num_to_str(combo_mult(), b); strcat(v, b);
    pause_row(px0, pw, y, "MULTIPLIER", v, 0x00FFE060); y += dy;
    { const char *wn = g_weapon == WP_SINGLE ? "SHOT" : g_weapon == WP_TWIN ? "TWIN" : g_weapon == WP_TRI ? "TRI" : g_weapon == WP_WIDE ? "WIDE" : g_weapon == WP_LASER ? "LASER" : g_weapon == WP_MISSILE ? "MISSILE" : "WAVE";
      strcpy(v, wn); strcat(v, " LV"); b[0] = (char)('0' + g_wlevel); b[1] = 0; strcat(v, b);
      pause_row(px0, pw, y, "WEAPON", v, 0x0080FFFF); } y += dy;
    num_to_str(g_lives, b);      pause_row(px0, pw, y, "LIVES", b, 0x0090FFB0); y += dy;
    num_to_str(g_bombs, b);      pause_row(px0, pw, y, "BOMBS", b, 0x00FF80FF); y += dy;
    num_to_str(g_continues, b);  pause_row(px0, pw, y, "CONTINUES", b, 0x00C0C0D0); y += dy;
    pause_row(px0, pw, y, "DIFFICULTY", diff_name(), 0x00FFB060); y += dy + 10;

    fill_rect(px0 + 14, y, pw - 28, 1, 0x00304050); y += 10;
    sq_text_sh(px0 + 14, y, "CONTROLS", 0x0060C8FF, 1); y += 14;
    static const char *ctl[5] = { "MOUSE OR WASD/ARROWS   MOVE",
                                  "HOLD LMB OR SPACE      FIRE",
                                  "RMB OR B               BOMB",
                                  "P OR CLICK             RESUME",
                                  "ESC                    MENU" };
    for (int i = 0; i < 5; i++) { sq_text_sh(px0 + 14, y, ctl[i], 0x00A8B4C4, 1); y += 12; }
}
static void draw_stageclear(void) {
    draw_playfield(); blend_rect(0, 0, W, H, 0x00001000, 120);
    sq_text_center(W / 2, H / 2 - 40, "STAGE CLEAR", 0x0060FF80, 3);
    char b[24], line[40]; num_to_str(g_score, b); strcpy(line, "SCORE "); strcat(line, b);
    sq_text_center(W / 2, H / 2 + 30, line, 0x00FFFFFF, 2);
    /* #it4: perfect-clear bonus, only shown when this stage's clear actually
     * earned one (g_perfect_bonus is set once, in enemy_killed(), and stays
     * at that value for the whole GS_STAGECLEAR screen). */
    if (g_perfect_bonus > 0) {
        char pb[40]; num_to_str(g_perfect_bonus, b); strcpy(pb, "PERFECT CLEAR  +"); strcat(pb, b);
        sq_text_center(W / 2, H / 2 + 62, pb, hsv((int)(g_now / 6) % 360, 255, 255), 2);
    }
}
/* #476 Part B: real AI (gpt-image-1) title-card sprites for GAME OVER / FINAL
 * SCORE / REACHED STAGE / TRY AGAIN? / ESC FOR MENU, composited onto the
 * magenta colour-key and loaded like any other sprite (see assets_load). The
 * numeric score/stage stays code-rendered next to its title sprite (see
 * header comment: compositing a live number into baked art isn't practical).
 * Every sprite falls back to the original stylized code-drawn text
 * (sq_text_stylized: outline + glow halo + gradient) if its BMP is missing,
 * so a partial or absent asset set never crashes or blanks this screen. */
static void draw_gameover(void) {
    draw_background(); blend_rect(0, 0, W, H, 0x00100000, 160);
    int mw = W - 50;
    if (A_txt_gameover.ok) spr_blit_fit_c(&A_txt_gameover, W / 2, H / 3 - 10, mw, (int)(H * 0.20f));
    else sq_text_stylized(W / 2, H / 3, "GAME OVER", 0x00FFB090, 0x00801010, 0x00FF3020, 4);

    char b[24], line[40];
    num_to_str(g_score, b); strcpy(line, "FINAL SCORE "); strcat(line, b);
    if (A_txt_finalscore.ok) {
        int cy = H / 2 - 10, bh = (int)(H * 0.11f);
        spr_blit_fit_c(&A_txt_finalscore, W / 2, cy, mw, bh);
        sq_text_center(W / 2, cy + bh / 2 + 6, b, 0x00FFFFFF, 2);
    } else sq_text_stylized(W / 2, H / 2, line, 0x00FFFFFF, 0x006080B0, 0x0060A0FF, 3);

    num_to_str(g_stage, b); strcpy(line, "REACHED STAGE "); strcat(line, b);
    if (A_txt_reachedstage.ok) {
        int cy = H / 2 + (int)(H * 0.16f), bh = (int)(H * 0.11f);
        spr_blit_fit_c(&A_txt_reachedstage, W / 2, cy, mw, bh);
        sq_text_center(W / 2, cy + bh / 2 + 4, b, 0x00FFF0A0, 2);
    } else sq_text_stylized(W / 2, H / 2 + 50, line, 0x00FFF0A0, 0x00905010, 0x00FF9020, 2);

    if (A_txt_tryagain.ok) spr_blit_fit_c(&A_txt_tryagain, W / 2, H - 178, mw - 30, 56);
    else sq_text_stylized(W / 2, H - 196, "TRY AGAIN?", 0x00C0FFD0, 0x00105030, 0x0030D070, 3);

    /* #it18: a real two-option prompt replaces the old single
     * "SPACE OR CLICK TO PLAY AGAIN" hint line. CONTINUE is drawn greyed and
     * unselectable once the three are spent, rather than vanishing, so the
     * player can see WHY it is gone instead of the menu silently changing
     * shape under them. */
    { char b[24], line[64];
      const char *c0;
      if (g_continues > 0) { num_to_str(g_continues, b); strcpy(line, "CONTINUE  STAGE "); { char sb[24]; num_to_str(g_stage, sb); strcat(line, sb); } strcat(line, "  ("); strcat(line, b); strcat(line, " LEFT)"); c0 = line; }
      else c0 = "CONTINUE  (NONE LEFT)";
      uint32_t col0 = g_continues <= 0 ? 0x00606068 : (g_go_sel == 0 ? 0x00FFFF80 : 0x00A0A0B0);
      uint32_t col1 = g_go_sel == 1 ? 0x00FFFF80 : 0x00A0A0B0;
      int s0 = fit_scale(c0, W - 60, 2), s1 = 2;
      if (g_go_sel == 0 && g_continues > 0) sq_text_sh(W / 2 - text_w(c0, s0) / 2 - 24, H - 138, ">", 0x00FFFF80, s0);
      sq_text_center(W / 2, H - 138, c0, col0, s0);
      if (g_go_sel == 1) sq_text_sh(W / 2 - text_w("NEW GAME", s1) / 2 - 24, H - 106, ">", 0x00FFFF80, s1);
      sq_text_center(W / 2, H - 106, "NEW GAME", col1, s1);
      sq_text_center(W / 2, H - 82, "UP/DOWN SELECT   ENTER OR SPACE CONFIRM", 0x0080889A, 1); }

    if (A_txt_escmenu.ok) spr_blit_fit_c(&A_txt_escmenu, W / 2, H - 52, mw - 60, 30);
    else sq_text_stylized(W / 2, H - 58, "ESC FOR MENU", 0x00C0E0FF, 0x00203850, 0x0040A0FF, 2);
}
/* #it18: one place decides what a confirm on the game-over screen does, so
 * the keyboard and mouse paths can never drift apart. */
static void gameover_confirm(void) {
    if (g_go_sel == 0 && g_continues > 0) continue_game();
    else new_game();
}

static void menu_confirm(void) {
    if (g_menu_sel == 0) new_game();
    else if (g_menu_sel == 1) g_difficulty = (g_difficulty + 1) % 3;
    else if (g_menu_sel == 2) { g_snd_on = !g_snd_on; if (g_snd_on) sfx(SFX_POWERUP); }   /* #it20 */
    else { perf_summary(); win_destroy(g_win); exit(0); }   /* #it20-fix */
}
static void menu_key(int k) {
    if (k == 0x80 + 0 || k == 0x48 || k == 'w' || k == 'W') g_menu_sel = (g_menu_sel + MENU_ITEMS - 1) % MENU_ITEMS;
    else if (k == 0x80 + 1 || k == 0x50 || k == 's' || k == 'S') g_menu_sel = (g_menu_sel + 1) % MENU_ITEMS;
    else if (k == 0x80 + 3 || k == 0x4B) { if (g_menu_sel == 1) g_difficulty = (g_difficulty + 2) % 3; else if (g_menu_sel == 2) g_snd_on = !g_snd_on; }
    else if (k == 0x80 + 2 || k == 0x4D) { if (g_menu_sel == 1) g_difficulty = (g_difficulty + 1) % 3; else if (g_menu_sel == 2) g_snd_on = !g_snd_on; }
    else if (k == '\n' || k == '\r' || k == ' ') menu_confirm();
    else if (k == 0x1B) { perf_summary(); win_destroy(g_win); exit(0); }   /* #it20-fix */
}

/* ============================================================ present ===== */
/* Derive the centered portrait playfield geometry from the full window size. */
static void compute_layout(void) {
    int w = (int)(FBW * 0.42f);
    if (w < 380) w = 380;
    if (w > 620) w = 620;
    if (w > FBW) w = FBW;
    W = w; H = FBH; PF_OX = (FBW - W) / 2;
}
/* Opaque scale-blit of a sidebar panel into g_present rect [x0,y0,pw,ph];
 * falls back to a tasteful gunmetal gradient console if the art is missing. */
static void sidebar_blit(const Sprite *s, int x0, int y0, int pw, int ph) {
    if (pw <= 0 || ph <= 0) return;
    if (s->ok) {
        for (int y = 0; y < ph; y++) {
            int py = y0 + y; if ((unsigned)py >= (unsigned)FBH) continue;
            int sy = y * s->h / ph; const uint32_t *src = s->px + sy * s->w;
            uint32_t *drow = g_present + py * FBW;
            for (int x = 0; x < pw; x++) { int px = x0 + x; if ((unsigned)px >= (unsigned)FBW) continue; drow[px] = src[x * s->w / pw] & 0x00FFFFFFu; }
        }
        return;
    }
    /* procedural gunmetal fallback: vertical gradient + riveted frame */
    for (int y = 0; y < ph; y++) {
        int py = y0 + y; if ((unsigned)py >= (unsigned)FBH) continue;
        int t = y * 90 / ph;
        uint32_t c = (uint32_t)(((18 + t / 3) << 16) | ((22 + t / 3) << 8) | (30 + t / 2));
        uint32_t *drow = g_present + py * FBW;
        for (int x = 0; x < pw; x++) { int px = x0 + x; if ((unsigned)px >= (unsigned)FBW) continue; drow[px] = c; }
    }
    fill_rect_abs(x0, y0, pw, 2, 0x00505A70);
    fill_rect_abs(x0, y0 + ph - 2, pw, 2, 0x00090C12);
    fill_rect_abs(x0, y0, 2, ph, 0x00505A70);
    fill_rect_abs(x0 + pw - 2, y0, 2, ph, 0x00090C12);
    for (int ry = y0 + 30; ry < y0 + ph - 20; ry += 60) { fill_rect_abs(x0 + 8, ry, 3, 3, 0x00707A90); fill_rect_abs(x0 + pw - 11, ry, 3, 3, 0x00707A90); }
}
/* Composite the narrow playfield + sidebar art + HUD, then push to the window. */
static void present_frame(void) {
    int lw = PF_OX;              /* left panel width  */
    int rx = PF_OX + W;          /* right panel start */
    int rw = FBW - rx;           /* right panel width */
    /* Sidebar art is static: only re-paint it when the panel geometry has
     * actually changed (first frame, or a resize), not every frame (#475
     * framerate fix - see file header). */
    if (lw != g_sb_lw || rx != g_sb_rx || rw != g_sb_rw || FBH != g_sb_fbh) rebuild_sidebars(lw, rx, rw);
    /* #it2: screen shake is applied ONLY here, as an edge-clamped read offset
     * into the already-rendered g_blit playfield - never to any game-logic
     * coordinate (g_px/g_py, bullet/enemy positions, hitboxes). That keeps
     * shaking purely a presentation effect: it can never desync what is
     * drawn from what actually collides, and it costs nothing when no shake
     * is active (shx == shy == 0, the common case). */
    int shx, shy; shake_offset(&shx, &shy);
    for (int y = 0; y < H && y < FBH; y++) {
        int srcy = y + shy; if (srcy < 0) srcy = 0; else if (srcy >= H) srcy = H - 1;
        const uint32_t *src = g_blit + srcy * W;
        uint32_t *dst = g_present + y * FBW + PF_OX;
        if (shx == 0) { for (int x = 0; x < W; x++) dst[x] = src[x]; }
        else for (int x = 0; x < W; x++) {
            int srcx = x + shx; if (srcx < 0) srcx = 0; else if (srcx >= W) srcx = W - 1;
            dst[x] = src[srcx];
        }
    }
    draw_side_hud(lw, rx, rw);
    syscall5(SYS_WIN_BLIT, g_win, 0, 0, (FBW & 0xFFFF) | ((FBH & 0xFFFF) << 16), (long)g_present);
}

/* ============================================================ main ======== */
int main(int argc, char *argv[]) {
    /* #it10: debug/verification-only argv flags, see the g_dbg_* declarations
     * near GS_MENU above for why this can never be reached from a real
     * Start-menu or desktop-icon launch (those pass argv={path}, argc==1,
     * so this loop never runs its body on that path). Recognised flags:
     *   --debug-stage=N   start the game directly on stage N (N >= 1)
     *   --debug-boss      skip straight to that stage's boss (combine with
     *                     --debug-stage=N to reach a specific stage's boss)
     *   --debug-god       player_hit() becomes a permanent no-op
     * Unrecognised args are ignored (never a parse error / exit), matching
     * every other MayteraOS userland app's tolerant argv handling. */
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--debug-stage=", 14)) { int n = atoi(argv[i] + 14); if (n > 0) g_dbg_stage = n; }
        else if (!strcmp(argv[i], "--debug-boss")) g_dbg_boss = 1;
        else if (!strcmp(argv[i], "--debug-god")) g_dbg_god = 1;
        else if (!strncmp(argv[i], "--debug-lives=", 14)) { int n = atoi(argv[i] + 14); if (n > 0) g_dbg_lives = n; }
        else if (!strncmp(argv[i], "--debug-bosshp=", 15)) { int n = atoi(argv[i] + 15); if (n > 0) g_dbg_bosshp = n; }
        else if (!strcmp(argv[i], "--nosound")) g_dbg_nosound = 1;   /* #it20: no worker thread at all */
        else if (!strcmp(argv[i], "--debug-drops")) g_dbg_drops = 1;   /* #it10d, harness only */
        else if (!strncmp(argv[i], "--debug-exitafter=", 18)) { int n = atoi(argv[i] + 18); if (n > 0) g_dbg_exitafter = n; }   /* #sqearlyexit harness only */
    }
    bt("enter", argc, 0);
    g_rng ^= (uint32_t)uptime_ms() * 2654435761u; if (!g_rng) g_rng = 1;
    bt("rng", (long)g_rng, 0);
    fb_info_t fi; int sw = 1024, sh = 768;
    if (fb_info(&fi) == 0 && fi.width > 0 && fi.height > 0) { sw = (int)fi.width; sh = (int)fi.height; }
    if (sw > MAXW) sw = MAXW; if (sh > MAXH) sh = MAXH;
    bt("fbinfo", sw, sh);
    g_win = win_create("Maytera Squadron", 0, 0, sw, sh);
    bt("wincreate", g_win, 0);
    if (g_win < 0) { die_msg("win_create failed"); return 1; }
    win_set_nochrome(g_win); wm_focus(g_win);
    { gui_event_t pe; win_get_event(g_win, &pe, 60); (void)pe; }
    FBW = sw; FBH = sh;
    if (win_get_size(g_win, &FBW, &FBH) != 0 || FBW <= 0 || FBH <= 0) { FBW = sw; FBH = sh; }
    if (FBW > MAXW) FBW = MAXW; if (FBH > MAXH) FBH = MAXH;
    bt("winsize", FBW, FBH);
    compute_layout();
    g_present_cap = (long)FBW * FBH; g_present = (uint32_t *)malloc((unsigned long)g_present_cap * 4);
    g_blit_cap   = (long)W   * H;   g_blit   = (uint32_t *)malloc((unsigned long)g_blit_cap   * 4);
    if (!g_present || !g_blit) {
        if (g_present) free(g_present);
        if (g_blit) free(g_blit);
        FBW = 1024; FBH = 768; compute_layout();
        g_present_cap = (long)FBW * FBH; g_present = (uint32_t *)malloc((unsigned long)g_present_cap * 4);
        g_blit_cap   = (long)W   * H;   g_blit   = (uint32_t *)malloc((unsigned long)g_blit_cap   * 4);
        if (!g_present || !g_blit) { die_msg("malloc failed for the backbuffers, even at the 1024x768 fallback size"); win_destroy(g_win); return 1; }
    }
    bt("bufs", (long)(unsigned long)g_present, (long)(unsigned long)g_blit);
#ifdef SQ_BOOTTRACE
    /* #sqearlyexit: WHERE CAN A RING-3 APP ACTUALLY CREATE A FILE? The #475
     * FPS log writes to /SQUADRON/FPSLOG.TXT and produces nothing; before
     * moving it anywhere, measure which destinations work rather than
     * guessing one. Three opens, three numbers, one line each. */
    {   int f;
        f = sys_open("/SQUADRON/FPSLOG.TXT", O_WRONLY | O_CREAT | O_TRUNC); bt("open-sqdir", f, 0); if (f >= 0) { sys_write(f, "probe\n", 6); sys_close(f); }
        f = sys_open("/SQPROBE.TXT",        O_WRONLY | O_CREAT | O_TRUNC); bt("open-root",  f, 0); if (f >= 0) { sys_write(f, "probe\n", 6); sys_close(f); }
        f = sys_open("/HOME/SQPROBE.TXT",   O_WRONLY | O_CREAT | O_TRUNC); bt("open-home",  f, 0); if (f >= 0) { sys_write(f, "probe\n", 6); sys_close(f); }
        f = sys_open("/NOTES/SQPROBE.TXT",  O_WRONLY | O_CREAT | O_TRUNC); bt("open-notes", f, 0); if (f >= 0) { sys_write(f, "probe\n", 6); sys_close(f); }
    }
#endif
    assets_load(); bt("assets", 0, 0);
    stars_init(); bt("stars", 0, 0);
    neb_init(); bt("neb", 0, 0);
    nebz_init(); bt("nebz", 0, 0);
    clust_init(); bt("clust", 0, 0);
    bgobj_init(); bt("bgobj", 0, 0);
    g_now = (unsigned)uptime_ms();
    snd_init(); bt("snd", (long)g_snd_started, 0);   /* #it20: starts the audio worker, or silently does nothing */
    /* #it10: any debug flag skips the GS_MENU screen and starts play directly,
     * so a scripted harness needs no synthetic keypress/click to get past the
     * menu before new_game() applies g_dbg_stage/g_dbg_boss (see new_game()). */
    if (g_dbg_stage > 0 || g_dbg_boss || g_dbg_god || g_dbg_lives > 0 || g_dbg_bosshp > 0 || g_dbg_drops) new_game();
    gui_event_t ev; int running = 1;
    bt("loop", 0, 0);
#ifdef SQ_BOOTTRACE
    unsigned bt_deadline = g_dbg_exitafter > 0 ? (unsigned)uptime_ms() + (unsigned)g_dbg_exitafter : 0;
    int bt_firstframe = 0;
#endif
    while (running) {
        /* Keep this fullscreen game frontmost + keyboard-focused every frame: a
         * background window-create (notification / dock / service) otherwise steals
         * focus and raises above us, which both drops our keys and lets the AI Chat
         * edge dock overlay the game. wm_focus_window() is a no-op in the kernel
         * when we already hold focus + front, so this is free until focus is lost. */
        wm_focus(g_win);
        int et = win_get_event(g_win, &ev, 16); g_now = (unsigned)uptime_ms();
        switch (et) {
        case EVENT_WINDOW_CLOSE: running = 0; break;
        case EVENT_RESIZE: { int nw, nh; if (win_get_size(g_win, &nw, &nh) == 0 && nw > 0 && nh > 0) { if (nw > MAXW) nw = MAXW; if (nh > MAXH) nh = MAXH;
            if ((long)nw * nh <= g_present_cap) { int oFBW = FBW, oFBH = FBH, oW = W, oH = H, oOX = PF_OX; FBW = nw; FBH = nh; compute_layout(); if ((long)W * H > g_blit_cap) { FBW = oFBW; FBH = oFBH; W = oW; H = oH; PF_OX = oOX; } } } } break;
        case EVENT_KEY_DOWN: {
            int k = (int)ev.keycode ? (int)ev.keycode : (int)(unsigned char)ev.key_char;
            if (g_state == GS_PLAYING) { key_set(&ev, 1); if (k == 0x1B || k == 'p' || k == 'P') g_state = GS_PAUSED; else if (k == 'b' || k == 'B') drop_bomb(); }
            else if (g_state == GS_PAUSED) { if (k == 0x1B) g_state = GS_MENU; else if (k == 'p' || k == 'P' || k == ' ' || k == '\n' || k == '\r') g_state = GS_PLAYING; }
            else if (g_state == GS_MENU) menu_key(k);
            else if (g_state == GS_GAMEOVER) {   /* #it18: two-option continue prompt */
                if (k == 0x80 + 0 || k == 0x48 || k == 'w' || k == 'W' ||
                    k == 0x80 + 1 || k == 0x50 || k == 's' || k == 'S') g_go_sel ^= 1;
                else if (k == ' ' || k == '\n' || k == '\r') gameover_confirm();
                else if (k == 0x1B) g_state = GS_MENU;
            }
        } break;
        case EVENT_KEY_UP: key_set(&ev, 0); break;
        case EVENT_MOUSE_MOVE: g_mx = ev.mouse_x; g_my = ev.mouse_y; g_have_mouse = 1; break;
        case EVENT_MOUSE_DOWN: {
            int right = (ev.mouse_buttons & MOUSE_BUTTON_RIGHT) != 0;
            g_mx = ev.mouse_x; g_my = ev.mouse_y; g_have_mouse = 1;
            if (g_state == GS_MENU) menu_confirm();
            else if (g_state == GS_PAUSED) g_state = GS_PLAYING;
            else if (g_state == GS_GAMEOVER) gameover_confirm();   /* #it18 */
            else if (g_state == GS_PLAYING) {
                /* RMB = bomb (momentary; the compositor never injects a RIGHT-button
                 * UP, so this must fire on DOWN, not be a held state). LMB = hold to
                 * fire (auto-fire removed); only the left button sets the fire flag. */
                if (right) drop_bomb(); else g_mouse_fire = 1;
            }
        } break;
        case EVENT_MOUSE_UP: g_mouse_fire = 0; break;
        default: break;
        }
        stars_update();
        clust_update();
        bgobj_update();
        g_bgpos += 0.35f;    /* slow, seamless background drift */
        if (g_state == GS_PLAYING) { update_input(); update_world(16); update_waves(); }
        else if (g_state == GS_STAGECLEAR) { update_world(16); if (g_now >= g_stageclear_until) { g_stage++; g_wave = 0; g_boss_active = 0; reset_entities(); g_px = (float)(W / 2); g_py = (float)(H + 60); begin_respawn(); /* #it17 */ g_invuln_until = g_now + INTRO_MS + 1200; g_wave_delay = g_now + INTRO_MS + 300; g_banner_until = 0; g_intro_until = g_now + INTRO_MS; sfx(SFX_STAGE); /* #it14 #it20 */ g_stage_spawned = 0; g_stage_killed = 0; g_stage_hit_taken = 0; g_state = GS_PLAYING; } }
        switch (g_state) {
        case GS_MENU: draw_menu(); break; case GS_PLAYING: draw_playfield(); break; case GS_PAUSED: draw_pause(); break;
        case GS_STAGECLEAR: draw_stageclear(); break; case GS_GAMEOVER: draw_gameover(); break;
        }
        present_frame();
        win_invalidate(g_win);
        fps_tick();   /* #475 measured FPS: drives the sidebar readout + FPSLOG.TXT */
#ifdef SQ_BOOTTRACE
        if (!bt_firstframe) { bt_firstframe = 1; bt("frame1", 0, 0); }
        if (bt_deadline && (unsigned)uptime_ms() >= bt_deadline) running = 0;
#endif
    }
    perf_summary();   /* #it20-fix */
    win_destroy(g_win); return 0;
}
