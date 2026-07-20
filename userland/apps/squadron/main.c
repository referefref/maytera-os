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
 * disk. Backgrounds: L<lvl>BG1..5.BMP (opaque). LOGO.BMP is the title.
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
 */
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "stdlib.h"    /* O_RDONLY, malloc/free, exit */

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
/* Vertical-scroll blit of an opaque bg, stretched to fill W and scrolled by `scroll`. */
static void bg_fill_scroll(const Sprite *s, int scroll) {
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
 * progress, but only ONE BMP is decoded per stage - 5x less load time and I/O. */
static void bgset_load(int level) {
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

/* bullet kinds (shared struct for player + enemy). */
enum { BK_STRAIGHT, BK_AIMED, BK_WAVE, BK_PLASMA, BK_MISSILE,   /* enemy */
       PK_SHOT, PK_LASER, PK_MISSILE, PK_WAVE };                /* player */
typedef struct { int alive, kind; float x, y, vx, vy; float t; int life; } Bullet;

/* enemy archetypes */
enum { ET_DRONE, ET_WEAVER, ET_DIVER, ET_STRAFER, ET_GUNSHIP, ET_TANK,
       ET_KAMIKAZE, ET_MINELAYER, ET_TURRET, ET_SILO, ET_STATION };
/* movement patterns */
enum { PT_STRAIGHT, PT_SINE, PT_SCURVE, PT_FIG8, PT_DIVE, PT_SWEEP, PT_CIRCLE, PT_HOVER, PT_STATIONARY };

typedef struct {
    int alive, type, boss, pattern, stationary;
    float x, y, vx, vy, t, basex, basey, amp;
    int   hp, maxhp, fire_ms, score, boss_phase;
    unsigned col;         /* tint for fallback sprite / glow */
} Enemy;
typedef struct { int alive, kind; float x, y, vy; } Powerup;   /* kind: see PUK_ */
typedef struct { int alive; float x, y; int frame, t, big; unsigned col; } Explosion;
typedef struct { float x, y, speed; int size; uint32_t col; } Star;

static Bullet   g_pb[MAX_PB];
static Bullet   g_eb[MAX_EB];
static Enemy    g_en[MAX_EN];
static Powerup  g_pu[MAX_PU];
static Explosion g_ex[MAX_EX];
static Star     g_stars[MAX_STARS];

/* ============================================================ game state == */
enum { GS_MENU, GS_PLAYING, GS_PAUSED, GS_STAGECLEAR, GS_GAMEOVER };
static int g_state = GS_MENU;

/* player weapons */
enum { WP_SINGLE, WP_TWIN, WP_TRI, WP_WIDE, WP_LASER, WP_MISSILE, WP_WAVE, WP_COUNT };
/* powerup kinds */
enum { PUK_RAPID, PUK_TWIN, PUK_WIDE, PUK_LASER, PUK_MISSILE, PUK_WAVE, PUK_SHIELD, PUK_BOMB, PUK_LIFE, PUK_COUNT };

static float g_px, g_py;
static int   g_lives, g_stage, g_wave;
static long  g_score;
static int   g_weapon;            /* WP_* */
static int   g_wlevel;            /* 1..3 power level within weapon */
static int   g_bombs;
static unsigned g_rapid_until, g_shield_until, g_invuln_until, g_fire_cd, g_wave_delay;
static int      g_boss_active;
static unsigned g_stageclear_until, g_banner_until, g_pu_label_until;
static const char *g_pu_label = 0;
static int      g_difficulty = 1;
static int      g_menu_sel = 0;
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
static long  g_highscore = 0;         /* max g_score seen this app session  */
static int   g_stage_spawned = 0;     /* enemies (incl boss) queued this stage */
static int   g_stage_killed = 0;      /* of those, how many are dead        */

static void add_score(long amt) { g_score += amt; if (g_score > g_highscore) g_highscore = g_score; }
static int  combo_mult(void) { int m = 1 + g_combo / 8; return m > 5 ? 5 : m; }

#define WAVES_PER_STAGE 5
#define PLAYER_W 52
#define PLAYER_H 44

/* mouse + keyboard input */
static int  g_have_mouse, g_mx, g_my, g_mouse_fire;
#define KEY_GRACE_MS 90
static unsigned char g_keys[256];
static unsigned      g_key_rel[256];
static unsigned      g_now;

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
static void neb_init(void) {
    unsigned char lat[NEB_LY][NEB_LX];
    for (int y = 0; y < NEB_LY; y++) for (int x = 0; x < NEB_LX; x++) lat[y][x] = (unsigned char)(rnd() & 0xFF);
    for (int y = 0; y < NEB_H; y++) {
        int ly = y * (NEB_LY - 1) / (NEB_H - 1); if (ly > NEB_LY - 2) ly = NEB_LY - 2;
        int fy = (y * (NEB_LY - 1) * 256 / (NEB_H - 1)) & 255;
        for (int x = 0; x < NEB_W; x++) {
            int lx = x * (NEB_LX - 1) / (NEB_W - 1); if (lx > NEB_LX - 2) lx = NEB_LX - 2;
            int fx = (x * (NEB_LX - 1) * 256 / (NEB_W - 1)) & 255;
            int v00 = lat[ly][lx], v10 = lat[ly][lx + 1], v01 = lat[ly + 1][lx], v11 = lat[ly + 1][lx + 1];
            int top = v00 + ((v10 - v00) * fx) / 256;
            int bot = v01 + ((v11 - v01) * fx) / 256;
            g_neb[y][x] = (unsigned char)(top + ((bot - top) * fy) / 256);
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
static float diff_enemy_speed(void) { return g_difficulty == 0 ? 0.8f : (g_difficulty == 2 ? 1.35f : 1.0f); }
static int   diff_fire_ms(void)     { return g_difficulty == 0 ? 1600 : (g_difficulty == 2 ? 750 : 1100); }
static float diff_ebullet_speed(void){ return g_difficulty == 0 ? 3.0f : (g_difficulty == 2 ? 5.0f : 4.0f); }

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

static void spawn_formation(int idx) {
    int pat = (g_stage * 3 + idx) % 7;
    int extra = g_stage > 3 ? 3 : g_stage;
    if (pat == 0) { int n = 5 + extra; int gap = W / (n + 1); for (int i = 0; i < n; i++) { Enemy *e = en_alloc(); if (e) enemy_reset(e, ET_DRONE, (float)(gap * (i + 1)), (float)(-40 - i * 10)); } }
    else if (pat == 1) { int n = 6; for (int i = 0; i < n; i++) { Enemy *e = en_alloc(); if (!e) continue; float x = (float)(W / 2 + (i - n / 2) * 90); enemy_reset(e, ET_WEAVER, x, (float)(-40 - iabs(i - n / 2) * 34)); } }
    else if (pat == 2) { for (int i = 0; i < 4; i++) { Enemy *l = en_alloc(); if (l) enemy_reset(l, ET_DIVER, 90.0f, (float)(-40 - i * 70)); Enemy *r = en_alloc(); if (r) enemy_reset(r, ET_DIVER, (float)(W - 90), (float)(-40 - i * 70)); } }
    else if (pat == 3) { int n = 5; for (int i = 0; i < n; i++) { Enemy *e = en_alloc(); if (!e) continue; enemy_reset(e, ET_STRAFER, (float)(120 + i * (W - 240) / (n - 1)), (float)(-50 - i * 24)); } }
    else if (pat == 4) { Enemy *g1 = en_alloc(); if (g1) enemy_reset(g1, ET_GUNSHIP, (float)(W / 3), -60.0f); Enemy *g2 = en_alloc(); if (g2) enemy_reset(g2, ET_GUNSHIP, (float)(2 * W / 3), -110.0f); for (int i = 0; i < 3; i++) { Enemy *e = en_alloc(); if (e) enemy_reset(e, ET_KAMIKAZE, (float)(W / 4 + i * W / 4), (float)(-160 - i * 40)); } }
    else if (pat == 5) { /* station wall of stationary turrets scrolling down */ Enemy *st = en_alloc(); if (st) enemy_reset(st, ET_STATION, (float)(W / 2), -180.0f); Enemy *t1 = en_alloc(); if (t1) enemy_reset(t1, ET_TURRET, (float)(W / 4), -120.0f); Enemy *t2 = en_alloc(); if (t2) enemy_reset(t2, ET_TURRET, (float)(3 * W / 4), -120.0f); Enemy *s1 = en_alloc(); if (s1) enemy_reset(s1, ET_SILO, 120.0f, -220.0f); Enemy *s2 = en_alloc(); if (s2) enemy_reset(s2, ET_SILO, (float)(W - 120), -220.0f); }
    else { Enemy *tk = en_alloc(); if (tk) enemy_reset(tk, ET_TANK, (float)(W / 2), -70.0f); for (int i = 0; i < 4; i++) { Enemy *e = en_alloc(); if (e) enemy_reset(e, ET_MINELAYER, (float)(100 + i * (W - 200) / 3), (float)(-40 - i * 30)); } }
}

static void spawn_boss(void) {
    Enemy *e = en_alloc(); if (!e) return;
    e->alive = 1; e->boss = 1; e->type = ET_DRONE; e->stationary = 0; e->pattern = PT_SINE;
    e->x = (float)(W / 2); e->y = -140.0f; e->basex = e->x; e->basey = 110.0f;
    e->vx = 2.0f * diff_enemy_speed(); e->vy = 1.2f; e->t = 0; e->amp = (float)(W / 2 - 130);
    e->hp = 340 + g_stage * 130; e->maxhp = e->hp; e->col = 0x00FF60A0;   /* far tankier + steeper scaling */
    e->fire_ms = 500; e->score = 3000 + g_stage * 1000; e->boss_phase = 0;
    g_boss_active = 1; g_banner_until = g_now + 2200;
    g_stage_spawned++;   /* boss counts as the final unit of "% COMPLETE" */
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
    g_px = (float)(W / 2); g_py = (float)(H - 90);
    g_invuln_until = g_now + 1500; g_fire_cd = 0; g_wave_delay = g_now + 600; g_boss_active = 0;
    g_banner_until = g_now + 2000; g_hit_flash = 0; g_bgpos = 0; bgset_load(1);
    g_kills = 0; g_shots_fired = 0; g_shots_hit = 0; g_combo = 0;
    g_stage_spawned = 0; g_stage_killed = 0;   /* g_highscore deliberately NOT reset: it is per-session */
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
    g_fire_cd = g_now + cd; g_shots_fired++;
    int lv = g_wlevel;
    switch (g_weapon) {
        case WP_SINGLE: spawn_pb(g_px, top, 0, -16, PK_SHOT); if (lv >= 2) spawn_pb(g_px, top - 10, 0, -16, PK_SHOT); break;
        case WP_TWIN:   spawn_pb(g_px - 12, top, 0, -16, PK_SHOT); spawn_pb(g_px + 12, top, 0, -16, PK_SHOT); if (lv >= 3) spawn_pb(g_px, top - 8, 0, -16, PK_SHOT); break;
        case WP_TRI:    spawn_pb(g_px, top, 0, -16, PK_SHOT); spawn_pb(g_px - 14, top, -3.0f, -15, PK_SHOT); spawn_pb(g_px + 14, top, 3.0f, -15, PK_SHOT); break;
        case WP_WIDE:   { int n = 3 + lv; for (int i = 0; i < n; i++) { float a = ((float)i / (float)(n - 1) - 0.5f) * 1.1f; spawn_pb(g_px, top, fsinf(a) * 8.0f, -15.0f, PK_SHOT); } } break;
        case WP_LASER:  spawn_pb(g_px, top, 0, -22, PK_LASER); if (lv >= 2) { spawn_pb(g_px - 10, top, 0, -22, PK_LASER); spawn_pb(g_px + 10, top, 0, -22, PK_LASER); } break;
        case WP_MISSILE:spawn_pb(g_px - 16, top, -1.5f, -8, PK_MISSILE); spawn_pb(g_px + 16, top, 1.5f, -8, PK_MISSILE); if (lv >= 3) spawn_pb(g_px, top, 0, -9, PK_MISSILE); break;
        case WP_WAVE:   spawn_pb(g_px, top, 0, -12, PK_WAVE); if (lv >= 2) { spawn_pb(g_px - 8, top, 0, -12, PK_WAVE); spawn_pb(g_px + 8, top, 0, -12, PK_WAVE); } break;
    }
}
static int pb_damage(int kind) { return kind == PK_LASER ? 1 : kind == PK_MISSILE ? 3 : kind == PK_WAVE ? 2 : 1; }

static void drop_bomb(void) {
    if (g_bombs <= 0 || g_state != GS_PLAYING) return;
    g_bombs--; g_bomb_flash = 14;
    for (int i = 0; i < MAX_EB; i++) g_eb[i].alive = 0;                 /* clear enemy bullets */
    for (int i = 0; i < MAX_EN; i++) { Enemy *e = &g_en[i]; if (!e->alive) continue; if (e->boss) { e->hp -= 40; if (e->hp <= 0) e->hp = 1; } else { spawn_explosion_c(e->x, e->y, 0, e->col); e->hp -= 6; if (e->hp <= 0) { add_score(e->score); g_kills++; g_stage_killed++; e->alive = 0; } } }
}

/* ============================================================ boss fire === */
static void boss_fire(Enemy *e) {
    float sp = diff_ebullet_speed();
    int hard = g_difficulty == 2;
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
    switch (k) { case PUK_RAPID: return "RAPID FIRE"; case PUK_TWIN: return "TWIN SHOT"; case PUK_WIDE: return "WIDE SPREAD";
        case PUK_LASER: return "LASER BEAM"; case PUK_MISSILE: return "HOMING MISSILES"; case PUK_WAVE: return "WAVE BEAM";
        case PUK_SHIELD: return "SHIELD"; case PUK_BOMB: return "SMART BOMB"; case PUK_LIFE: return "EXTRA LIFE"; default: return ""; }
}
static void maybe_drop_powerup(float x, float y, int guaranteed) {
    int roll = (int)(rnd() % 100);
    if (!guaranteed && roll >= 16) return;
    Powerup *p = pu_alloc(); if (!p) return;
    int kind = (int)(rnd() % PUK_COUNT);
    p->alive = 1; p->kind = kind; p->x = x; p->y = y; p->vy = 2.0f;
}
static void set_weapon(int w) { if (g_weapon == w) { if (g_wlevel < 3) g_wlevel++; else add_score(400); } else { g_weapon = w; g_wlevel = 1; } }
static void apply_powerup(int kind) {
    switch (kind) {
        case PUK_RAPID:   g_rapid_until = g_now + 10000; break;
        case PUK_TWIN:    set_weapon(WP_TWIN); break;
        case PUK_WIDE:    set_weapon(WP_WIDE); break;
        case PUK_LASER:   set_weapon(WP_LASER); break;
        case PUK_MISSILE: set_weapon(WP_MISSILE); break;
        case PUK_WAVE:    set_weapon(WP_WAVE); break;
        case PUK_SHIELD:  g_shield_until = g_now + 8000; break;
        case PUK_BOMB:    if (g_bombs < 5) g_bombs++; else add_score(300); break;
        case PUK_LIFE:    if (g_lives < 6) g_lives++; else add_score(500); break;
    }
    g_pu_label = puk_name(kind); g_pu_label_until = g_now + 1400; add_score(60);
}

/* ============================================================ collisions == */
static int aabb(float ax, float ay, int aw, int ah, float bx, float by, int bw, int bh) { return iabs((int)ax - (int)bx) * 2 < (aw + bw) && iabs((int)ay - (int)by) * 2 < (ah + bh); }
static int enemy_box_w(Enemy *e) { return e->boss ? 190 : (e->type == ET_STATION ? 120 : (e->type == ET_TANK ? 60 : 46)); }
static int enemy_box_h(Enemy *e) { return e->boss ? 140 : (e->type == ET_STATION ? 100 : (e->type == ET_TANK ? 52 : 40)); }

static void player_hit(void) {
    if (g_now < g_shield_until || g_now < g_invuln_until) return;
    spawn_explosion(g_px, g_py, 1); g_lives--; g_hit_flash = 8; g_combo = 0;
    if (g_weapon != WP_SINGLE) { g_weapon = WP_SINGLE; g_wlevel = 1; } else if (g_wlevel > 1) g_wlevel = 1;
    g_invuln_until = g_now + 2000; g_shield_until = 0;
    if (g_lives <= 0) { g_state = GS_GAMEOVER; g_banner_until = 0; }
    else { g_px = (float)(W / 2); g_py = (float)(H - 90); }
}
static void enemy_killed(Enemy *e) {
    spawn_explosion_c(e->x, e->y, e->boss, e->col); add_score(e->score * combo_mult()); g_combo++;
    g_kills++; g_stage_killed++;
    if (e->boss) { g_boss_active = 0; for (int i = 0; i < 4; i++) maybe_drop_powerup(e->x + (i - 2) * 44, e->y, 1); g_state = GS_STAGECLEAR; g_stageclear_until = g_now + 2600; }
    else maybe_drop_powerup(e->x, e->y, 0);
    e->alive = 0;
}

/* ============================================================ update ====== */
static void update_input(void) {
    if (g_state != GS_PLAYING) return;
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
            else { float rate = 0.9f + (float)e->boss_phase * 0.55f;
                   float amp  = e->amp * (1.0f + (float)e->boss_phase * 0.22f);
                   e->x = e->basex + fsinf(e->t * rate) * amp; }
            int lo = e->maxhp / 3, mid = 2 * e->maxhp / 3;
            e->boss_phase = e->hp > mid ? 0 : (e->hp > lo ? 1 : 2);
            e->fire_ms -= dt;
            if (e->fire_ms <= 0) { boss_fire(e);
                int base = e->boss_phase == 2 ? 300 : (e->boss_phase == 1 ? 360 : 520);
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
    if (!(inv && ((g_now / 80) & 1))) {
        if (A_player.ok) spr_blit_c(&A_player, (int)g_px, (int)g_py);
        else draw_ship_fallback((int)g_px, (int)g_py, 0x0080C8FF, 1);
    }
    if (shielded) { int r = 42 + (int)(fsinf((float)g_now * 0.01f) * 3.0f);
        for (int a = 0; a < 360; a += 10) { float rad = (float)a * 0.01745f; add_glow((int)(g_px + fcosf(rad) * r), (int)(g_py + fsinf(rad) * r), 4, 0x0040FFFF, 120); } }
}
static void draw_enemy(Enemy *e) {
    if (e->boss) {
        add_glow((int)e->x, (int)e->y, 40, 0x00602040, 60);
        if (A_boss.ok) spr_blit_scaled_c(&A_boss, (int)e->x, (int)e->y, 210, 158);
        else if (A_enemy[0].ok) spr_blit_scaled_c(&A_enemy[0], (int)e->x, (int)e->y, 210, 158);
        else { for (int i = 0; i < 3; i++) draw_ship_fallback((int)e->x + (i - 1) * 64, (int)e->y, e->col, 0); }
        int bw = W / 2, bx = (int)(W / 2 - bw / 2), by = 24; fill_rect(bx - 2, by - 2, bw + 4, 12, 0x00202028); fill_rect(bx, by, bw * (e->hp > 0 ? e->hp : 0) / e->maxhp, 8, 0x00FF4060);
        const char *ph = e->boss_phase == 0 ? "PHASE 1" : e->boss_phase == 1 ? "PHASE 2" : "PHASE 3 - CORE"; sq_text_sh(bx, by + 14, ph, 0x00FFC0C0, 1);
        return;
    }
    if (e->stationary) {
        /* draw as a metallic emplacement box + gun barrel + glow */
        int hw = enemy_box_w(e) / 2, hh = enemy_box_h(e) / 2;
        fill_rect((int)e->x - hw, (int)e->y - hh, hw * 2, hh * 2, 0x00404860);
        fill_rect((int)e->x - hw, (int)e->y - hh, hw * 2, 3, 0x00808CA8);
        if (e->type == ET_STATION) { for (int g = -2; g <= 2; g++) fill_rect((int)e->x + g * 22 - 3, (int)e->y + hh - 4, 6, 12, 0x00303848); add_glow((int)e->x, (int)e->y, 26, e->col, 70); }
        else { fill_rect((int)e->x - 4, (int)e->y, 8, hh + 10, 0x00303848); add_glow((int)e->x, (int)e->y, 14, e->col, 90); }
        int barw = hw * 2 * (e->hp > 0 ? e->hp : 0) / e->maxhp; fill_rect((int)e->x - hw, (int)e->y - hh - 6, barw, 3, 0x00FFA040);
        return;
    }
    Sprite *s = &A_enemy[e->type % 4];
    if (s->ok) spr_blit_c(s, (int)e->x, (int)e->y);
    else draw_ship_fallback((int)e->x, (int)e->y, e->col, 0);
    add_glow((int)e->x, (int)(e->y - 6), 8, e->col, 60);
    if (e->type == ET_TANK) { int barw = 60 * (e->hp > 0 ? e->hp : 0) / e->maxhp; fill_rect((int)e->x - 30, (int)e->y - 30, barw, 3, 0x00FFA040); }
}
static void draw_powerup(Powerup *p) {
    uint32_t col = hsv((int)(g_now / 8) % 360, 255, 255);
    add_glow((int)p->x, (int)p->y, 16, col, 130);
    fill_rect((int)p->x - 13, (int)p->y - 13, 26, 26, 0x00101018);
    fill_rect((int)p->x - 11, (int)p->y - 11, 22, 22, col);
    const char *l = p->kind == PUK_RAPID ? "R" : p->kind == PUK_TWIN ? "II" : p->kind == PUK_WIDE ? "W" :
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

/* Procedural rainbow / themed background fallback (per level). */
static void proc_bg(int level, int scroll) {
    int set = (level - 1) % 3;
    for (int y = 0; y < H; y++) {
        int yy = (y + scroll) % (H * 2);
        uint32_t c;
        if (set == 0) c = hsv((yy * 300 / H + scroll / 4) % 360, 200, 40 + (yy % 60));       /* rainbow aurora */
        else if (set == 1) { int t = (yy * 80 / H) % 90; c = (uint32_t)((6) << 16) | ((10 + t / 3) << 8) | (30 + t); }  /* deep blue */
        else { int t = (yy * 80 / H) % 90; c = (uint32_t)((30 + t) << 16) | ((6 + t / 4) << 8) | (10); }                /* fiery red */
        fill_rect(0, y, W, 1, c);
    }
}
__attribute__((unused)) static uint32_t proc_bg_col(int level, int seg) {
    int set = (level - 1) % 3;
    if (set == 0) return hsv((seg * 72 + 40) % 360, 200, 90);   /* rainbow */
    if (set == 1) return 0x00102040;                            /* blue */
    return 0x00401010;                                          /* red */
}
static void draw_background(void) {
    int level = g_state == GS_MENU ? 1 : g_stage;
    bgset_load(level);
    /* Slow, seamless vertical drift (bg_fill_scroll tiles by modulo). The scroll
     * is deliberately gentle so it reads as flying over a base / through space,
     * not a fast rush; the starfield adds the faster parallax layer. */
    int pos = (int)g_bgpos;
    if (A_bgset[0].ok) bg_fill_scroll(&A_bgset[0], pos);
    else proc_bg(level, pos);
    /* #476 multi-layer parallax, slowest to fastest: nebula wash, far
     * background art (planets/asteroid/cloud), then the 3-layer starfield.
     * Everything here is deliberately slower than any enemy's vy. */
    nebula_draw(level, g_bgpos);
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

static void fps_append(const char *s) { while (*s && g_fps_log_len < FPS_LOG_CAP - 1) g_fps_log[g_fps_log_len++] = *s++; }
static void fps_append_num(long v) { char b[24]; num_to_str(v, b); fps_append(b); }
static void fps_flush(void) {
    int fd = sys_open(FPS_LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    sys_write(fd, g_fps_log, (unsigned long)g_fps_log_len); sys_close(fd);
}
static void fps_tick(void) {
    g_fps_win_frames++;
    if (g_fps_win_start == 0) g_fps_win_start = g_now;
    unsigned el = g_now - g_fps_win_start;
    if (el < 1000) return;
    g_perf_fps = (int)((long)g_fps_win_frames * 1000 / (el ? el : 1));
    if (g_fps_samples < FPS_MAX_SAMPLES) {
        fps_append("t="); fps_append_num((long)g_now); fps_append(" fps="); fps_append_num(g_perf_fps); fps_append("\n");
        g_fps_samples++; fps_flush();
    }
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
 * re-blitting the whole (static) panel every frame. Malloc failure degrades
 * gracefully: box_restore() then no-ops and old text may persist under new
 * text rather than crashing - rare (12 small allocations) and non-fatal. */
static void box_snapshot(HudBox *b) {
    if (!b->clean) return;
    for (int y = 0; y < b->h; y++) {
        int py = b->y + y; uint32_t *dst = b->clean + (long)y * b->w;
        if ((unsigned)py >= (unsigned)FBH) { for (int x = 0; x < b->w; x++) dst[x] = 0; continue; }
        const uint32_t *src = g_present + (long)py * FBW;
        for (int x = 0; x < b->w; x++) { int px = b->x + x; dst[x] = ((unsigned)px < (unsigned)FBW) ? src[px] : 0; }
    }
}
static void box_restore(HudBox *b) {
    if (!b->clean) return;
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
/* label (small, top) + value (larger, auto-fit) centered in a box. */
static void box_label_value(HudBox *b, const char *label, const char *value, uint32_t lcol, uint32_t vcol) {
    int cx = b->x + b->w / 2;
    sq_text_abs_center(cx, b->y + 5, label, lcol, 1);
    int sc = fit_scale(value, b->w - 8, 2);
    sq_text_abs_center(cx, b->y + 5 + 10 + (sc == 2 ? 3 : 1), value, vcol, sc);
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
    box_restore(&g_boxR[0]);
    { int cx = g_boxR[0].x + g_boxR[0].w / 2;
      sq_text_abs_center(cx, g_boxR[0].y + 5, "LIVES", 0x0090C0FF, 1);
      num_to_str(g_lives, buf); strcpy(val, "x "); strcat(val, buf);
      sq_text_abs_center(cx, g_boxR[0].y + 18, val, 0x00FFFFFF, 2);
      int n = g_lives > 6 ? 6 : (g_lives < 0 ? 0 : g_lives);
      for (int i = 0; i < n; i++) ship_icon_abs(cx - n * 8 + i * 16 + 8, g_boxR[0].y + g_boxR[0].h - 12, 0x0080C8FF); }
    box_restore(&g_boxR[1]);
    { int cx = g_boxR[1].x + g_boxR[1].w / 2;
      sq_text_abs_center(cx, g_boxR[1].y + 5, "BOMBS", 0x0090C0FF, 1);
      num_to_str(g_bombs, buf); strcpy(val, "x "); strcat(val, buf);
      sq_text_abs_center(cx, g_boxR[1].y + 18, val, 0x00FF80FF, 2);
      /* discoverability: the actual trigger, right under the count (#475) */
      sq_text_abs_center(cx, g_boxR[1].y + g_boxR[1].h - 12, "[RMB]/B", 0x0090A0B0, 1); }
    box_restore(&g_boxR[2]);
    { num_to_str((long)g_perf_fps, buf); box_label_value(&g_boxR[2], "FPS", buf, 0x0090C0FF, g_perf_fps > 0 && g_perf_fps < 30 ? 0x00FF6060 : 0x0060FF90); }
    box_restore(&g_boxR[3]);
    { strcpy(val, "x"); char d[4]; num_to_str(combo_mult(), d); strcat(val, d);
      box_label_value(&g_boxR[3], "MULTIPLIER", val, 0x0090C0FF, 0x00FFE060); }
    box_restore(&g_boxR[4]);
    { long fired = g_shots_fired, hit = g_shots_hit > fired ? fired : g_shots_hit;
      int acc = fired > 0 ? (int)(hit * 100 / fired) : 0;
      num_to_str(acc, buf); strcat(buf, "%"); box_label_value(&g_boxR[4], "ACCURACY", buf, 0x0090C0FF, 0x0080D0FF); }
    box_restore(&g_boxR[5]);
    { Enemy *boss = find_boss(); int cx = g_boxR[5].x + g_boxR[5].w / 2;
      int bw = g_boxR[5].w - 16, bx = g_boxR[5].x + 8, by = g_boxR[5].y + g_boxR[5].h / 2;
      if (boss) {
          sq_text_abs_center(cx, g_boxR[5].y + 5, "BOSS", 0x00FF9090, 1);
          fill_rect_abs(bx - 1, by - 1, bw + 2, 10, 0x00202028);
          int fillw = boss->maxhp > 0 ? bw * (boss->hp > 0 ? boss->hp : 0) / boss->maxhp : 0;
          fill_rect_abs(bx, by, fillw, 8, 0x00FF4060);
      } else if (g_now < g_shield_until) {
          sq_text_abs_center(cx, g_boxR[5].y + 5, "SHIELD", 0x0090C0FF, 1);
          num_to_str((g_shield_until - g_now) / 1000 + 1, buf); strcat(buf, "s");
          sq_text_abs_center(cx, g_boxR[5].y + 18, buf, 0x0040C0FF, 2);
      } else {
          sq_text_abs_center(cx, g_boxR[5].y + 5, "HULL", 0x0090C0FF, 1);
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
    /* HUD is rendered onto the sidebars at present time (draw_side_hud). */
    if (g_now < g_banner_until) { char b[24], line[40]; num_to_str(g_stage, b); strcpy(line, "STAGE "); strcat(line, b); if (g_boss_active) strcpy(line, "WARNING - BOSS"); sq_text_center(W / 2, H / 2 - 40, line, g_boss_active ? 0x00FF5050 : 0x00FFFFFF, 3); }
}

/* ============================================================ menus ======= */
static const char *diff_name(void) { return g_difficulty == 0 ? "EASY" : g_difficulty == 2 ? "HARD" : "NORMAL"; }
static void draw_menu(void) {
    draw_background();
    if (A_logo.ok) { int lw = A_logo.w, lh = A_logo.h; if (lw > W - 80) { lh = lh * (W - 80) / lw; lw = W - 80; } spr_blit_scaled_c(&A_logo, W / 2, H / 4, lw, lh); }
    else { sq_text_center(W / 2, H / 6, "MAYTERA", hsv((int)(g_now / 10) % 360, 255, 255), 5); sq_text_center(W / 2, H / 6 + 54, "SQUADRON", 0x00FFE060, 5); }
    int y = H / 2 + 40; char line[40]; const char *items[3];
    items[0] = "PLAY"; strcpy(line, "DIFFICULTY: "); strcat(line, diff_name()); items[1] = line; items[2] = "QUIT";
    for (int i = 0; i < 3; i++) { uint32_t c = (i == g_menu_sel) ? 0x00FFFF80 : 0x00A0A0B0; const char *txt = i == 1 ? line : items[i]; int tw = text_w(txt, 3);
        if (i == g_menu_sel) sq_text_sh(W / 2 - tw / 2 - 40, y + i * 44, ">", 0x00FFFF80, 3); sq_text_center(W / 2, y + i * 44, txt, c, 3); }
    sq_text_center(W / 2, H - 90, "MOUSE MOVES SHIP   HOLD LMB/SPACE TO FIRE   RMB/B = BOMB", 0x0090A0B0, 1);
    sq_text_center(W / 2, H - 68, "ENTER/CLICK SELECT   ESC QUITS", 0x0070808F, 1);
}
static void draw_pause(void) { draw_playfield(); blend_rect(0, 0, W, H, 0x00000010, 150); sq_text_center(W / 2, H / 2 - 40, "PAUSED", 0x00FFFFFF, 5); sq_text_center(W / 2, H / 2 + 30, "P OR CLICK TO RESUME   ESC FOR MENU", 0x00B0B0C0, 2); }
static void draw_stageclear(void) { draw_playfield(); blend_rect(0, 0, W, H, 0x00001000, 120); sq_text_center(W / 2, H / 2 - 40, "STAGE CLEAR", 0x0060FF80, 3); char b[24], line[40]; num_to_str(g_score, b); strcpy(line, "SCORE "); strcat(line, b); sq_text_center(W / 2, H / 2 + 30, line, 0x00FFFFFF, 2); }
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

    if (A_txt_tryagain.ok) spr_blit_fit_c(&A_txt_tryagain, W / 2, H - 155, mw - 30, 60);
    else sq_text_stylized(W / 2, H - 150, "TRY AGAIN?", 0x00C0FFD0, 0x00105030, 0x0030D070, 3);

    sq_text_center(W / 2, H - 108, "SPACE OR CLICK TO PLAY AGAIN", 0x00C0C0D0, 2);

    if (A_txt_escmenu.ok) spr_blit_fit_c(&A_txt_escmenu, W / 2, H - 72, mw - 60, 34);
    else sq_text_stylized(W / 2, H - 74, "ESC FOR MENU", 0x00C0E0FF, 0x00203850, 0x0040A0FF, 2);
}

static void menu_confirm(void) { if (g_menu_sel == 0) new_game(); else if (g_menu_sel == 1) g_difficulty = (g_difficulty + 1) % 3; else { win_destroy(g_win); exit(0); } }
static void menu_key(int k) {
    if (k == 0x80 + 0 || k == 0x48 || k == 'w' || k == 'W') g_menu_sel = (g_menu_sel + 2) % 3;
    else if (k == 0x80 + 1 || k == 0x50 || k == 's' || k == 'S') g_menu_sel = (g_menu_sel + 1) % 3;
    else if (k == 0x80 + 3 || k == 0x4B) { if (g_menu_sel == 1) g_difficulty = (g_difficulty + 2) % 3; }
    else if (k == 0x80 + 2 || k == 0x4D) { if (g_menu_sel == 1) g_difficulty = (g_difficulty + 1) % 3; }
    else if (k == '\n' || k == '\r' || k == ' ') menu_confirm();
    else if (k == 0x1B) { win_destroy(g_win); exit(0); }
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
    for (int y = 0; y < H && y < FBH; y++) {
        const uint32_t *src = g_blit + y * W;
        uint32_t *dst = g_present + y * FBW + PF_OX;
        for (int x = 0; x < W; x++) dst[x] = src[x];
    }
    draw_side_hud(lw, rx, rw);
    syscall5(SYS_WIN_BLIT, g_win, 0, 0, (FBW & 0xFFFF) | ((FBH & 0xFFFF) << 16), (long)g_present);
}

/* ============================================================ main ======== */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    g_rng ^= (uint32_t)uptime_ms() * 2654435761u; if (!g_rng) g_rng = 1;
    fb_info_t fi; int sw = 1024, sh = 768;
    if (fb_info(&fi) == 0 && fi.width > 0 && fi.height > 0) { sw = (int)fi.width; sh = (int)fi.height; }
    if (sw > MAXW) sw = MAXW; if (sh > MAXH) sh = MAXH;
    g_win = win_create("Maytera Squadron", 0, 0, sw, sh); if (g_win < 0) return 1;
    win_set_nochrome(g_win); wm_focus(g_win);
    { gui_event_t pe; win_get_event(g_win, &pe, 60); (void)pe; }
    FBW = sw; FBH = sh;
    if (win_get_size(g_win, &FBW, &FBH) != 0 || FBW <= 0 || FBH <= 0) { FBW = sw; FBH = sh; }
    if (FBW > MAXW) FBW = MAXW; if (FBH > MAXH) FBH = MAXH;
    compute_layout();
    g_present_cap = (long)FBW * FBH; g_present = (uint32_t *)malloc((unsigned long)g_present_cap * 4);
    g_blit_cap   = (long)W   * H;   g_blit   = (uint32_t *)malloc((unsigned long)g_blit_cap   * 4);
    if (!g_present || !g_blit) {
        if (g_present) free(g_present);
        if (g_blit) free(g_blit);
        FBW = 1024; FBH = 768; compute_layout();
        g_present_cap = (long)FBW * FBH; g_present = (uint32_t *)malloc((unsigned long)g_present_cap * 4);
        g_blit_cap   = (long)W   * H;   g_blit   = (uint32_t *)malloc((unsigned long)g_blit_cap   * 4);
        if (!g_present || !g_blit) { win_destroy(g_win); return 1; }
    }
    assets_load(); bgset_load(1); stars_init(); neb_init(); bgobj_init(); g_now = (unsigned)uptime_ms();
    gui_event_t ev; int running = 1;
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
            else if (g_state == GS_GAMEOVER) { if (k == ' ' || k == '\n' || k == '\r') new_game(); else if (k == 0x1B) g_state = GS_MENU; }
        } break;
        case EVENT_KEY_UP: key_set(&ev, 0); break;
        case EVENT_MOUSE_MOVE: g_mx = ev.mouse_x; g_my = ev.mouse_y; g_have_mouse = 1; break;
        case EVENT_MOUSE_DOWN: {
            int right = (ev.mouse_buttons & MOUSE_BUTTON_RIGHT) != 0;
            g_mx = ev.mouse_x; g_my = ev.mouse_y; g_have_mouse = 1;
            if (g_state == GS_MENU) menu_confirm();
            else if (g_state == GS_PAUSED) g_state = GS_PLAYING;
            else if (g_state == GS_GAMEOVER) new_game();
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
        bgobj_update();
        g_bgpos += 0.35f;    /* slow, seamless background drift */
        if (g_state == GS_PLAYING) { update_input(); update_world(16); update_waves(); }
        else if (g_state == GS_STAGECLEAR) { update_world(16); if (g_now >= g_stageclear_until) { g_stage++; g_wave = 0; g_boss_active = 0; reset_entities(); bgset_load(g_stage); g_px = (float)(W / 2); g_py = (float)(H - 90); g_invuln_until = g_now + 1500; g_wave_delay = g_now + 800; g_banner_until = g_now + 2000; g_stage_spawned = 0; g_stage_killed = 0; g_state = GS_PLAYING; } }
        switch (g_state) {
        case GS_MENU: draw_menu(); break; case GS_PLAYING: draw_playfield(); break; case GS_PAUSED: draw_pause(); break;
        case GS_STAGECLEAR: draw_stageclear(); break; case GS_GAMEOVER: draw_gameover(); break;
        }
        present_frame();
        win_invalidate(g_win);
        fps_tick();   /* #475 measured FPS: drives the sidebar readout + FPSLOG.TXT */
    }
    win_destroy(g_win); return 0;
}
