// screensaver.c - Screensaver effects for MayteraOS userland compositor.
// Supports blank, starfield, bouncing lines, and expanding bubbles modes.
// No malloc. All state is static. Idle detection uses monotonic uptime_ms()
// (kernel-reported milliseconds), so it is independent of the timer rate.

#include "compositor.h"
#include "../../libc/syscall.h"
#include "gldemo.h"   // #319 TinyGL demo render cores (reconciled #336)
#include "planet_art.h"  // real CC0 planet sprites (2D Planet Pack 2, SBS)
#include "screensaver_gfx.h"  // psychedelic redesign shared pipeline (docs/SCREENSAVER_PSYCHEDELIC_DESIGN.md)

// ============================================================================
// Static state
// ============================================================================

// #124: default is the CLASSIC plasma (id 22). NOTE this initializer is NOT
// what decides the default on a running system: main.c syncs g_ss_type from
// get_screensaver() (the kernel-held setting) on the FIRST main-loop pass
// (s_cur_ss starts at -1), so the kernel's g_screensaver_type initializer in
// kernel/proc/syscall.c wins on a fresh profile and UIPROFIL.YML's
// screensaver:<n> wins on an existing one. Kept in agreement with both so the
// three copies cannot disagree (the #652 lesson, applied to the TYPE default
// rather than the DELAY default that commit was actually about).
static screensaver_type_t g_ss_type    = SS_PLASMACLASSIC;   // default: Plasma (Classic), kernel id 22
static int                g_ss_timeout = SS_DEFAULT_TIMEOUT; // seconds
static uint32_t           g_ss_frame;
static uint32_t           g_ss_seed    = 12345;
// #570: uptime_ms() at the moment the screensaver last became active. Used by
// screensaver_on_input() to ignore wake-on-input for a short grace window
// right after activation (the click that activates it via Settings "Test"
// still has its button-UP in flight).
static uint64_t           g_ss_active_ms = 0;

// Starfield
static ss_star_t   g_stars[SS_MAX_STARS];

// Lines
static ss_line_t   g_lines[SS_MAX_LINES];

// Bubbles
static ss_bubble_t g_bubbles[SS_MAX_BUBBLES];

// Deep-space objects for the starfield
static ss_obj_t g_objs[SS_MAX_OBJS];

// Real planet sprites drifting through the starfield with depth parallax.
#define SS_MAX_PLANETS 5
typedef struct { int32_t x, y, sz, vx, vy, img; } ss_planet_t;
static ss_planet_t g_planets[SS_MAX_PLANETS];

// ============================================================================
// PRNG
// ============================================================================

static uint32_t ss_rand(void) {
    g_ss_seed = g_ss_seed * 1103515245 + 12345;
    return (g_ss_seed >> 16) & 0x7FFF;
}

// Fixed-point sine table (amplitude 4096), 256 steps per full turn.
static const short g_sin[256] = {
        0,   101,   201,   301,   401,   501,   601,   700,   799,   897,   995,  1092,  1189,  1285,  1380,  1474,
     1567,  1660,  1751,  1842,  1931,  2019,  2106,  2191,  2276,  2359,  2440,  2520,  2598,  2675,  2751,  2824,
     2896,  2967,  3035,  3102,  3166,  3229,  3290,  3349,  3406,  3461,  3513,  3564,  3612,  3659,  3703,  3745,
     3784,  3822,  3857,  3889,  3920,  3948,  3973,  3996,  4017,  4036,  4052,  4065,  4076,  4085,  4091,  4095,
     4096,  4095,  4091,  4085,  4076,  4065,  4052,  4036,  4017,  3996,  3973,  3948,  3920,  3889,  3857,  3822,
     3784,  3745,  3703,  3659,  3612,  3564,  3513,  3461,  3406,  3349,  3290,  3229,  3166,  3102,  3035,  2967,
     2896,  2824,  2751,  2675,  2598,  2520,  2440,  2359,  2276,  2191,  2106,  2019,  1931,  1842,  1751,  1660,
     1567,  1474,  1380,  1285,  1189,  1092,   995,   897,   799,   700,   601,   501,   401,   301,   201,   101,
        0,  -101,  -201,  -301,  -401,  -501,  -601,  -700,  -799,  -897,  -995, -1092, -1189, -1285, -1380, -1474,
    -1567, -1660, -1751, -1842, -1931, -2019, -2106, -2191, -2276, -2359, -2440, -2520, -2598, -2675, -2751, -2824,
    -2896, -2967, -3035, -3102, -3166, -3229, -3290, -3349, -3406, -3461, -3513, -3564, -3612, -3659, -3703, -3745,
    -3784, -3822, -3857, -3889, -3920, -3948, -3973, -3996, -4017, -4036, -4052, -4065, -4076, -4085, -4091, -4095,
    -4096, -4095, -4091, -4085, -4076, -4065, -4052, -4036, -4017, -3996, -3973, -3948, -3920, -3889, -3857, -3822,
    -3784, -3745, -3703, -3659, -3612, -3564, -3513, -3461, -3406, -3349, -3290, -3229, -3166, -3102, -3035, -2967,
    -2896, -2824, -2751, -2675, -2598, -2520, -2440, -2359, -2276, -2191, -2106, -2019, -1931, -1842, -1751, -1660,
    -1567, -1474, -1380, -1285, -1189, -1092,  -995,  -897,  -799,  -700,  -601,  -501,  -401,  -301,  -201,  -101,
};
#define SS_SIN(a) ((int)g_sin[(int)(a) & 0xFF])
#define SS_COS(a) ((int)g_sin[((int)(a) + 64) & 0xFF])

// Map a hue 0..255 to a fully-saturated RGB color (ARGB).
static uint32_t ss_hue(int h) {
    h &= 0xFF;
    int region = h / 43;
    int rem = (h % 43) * 6;            // 0..252
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

// ============================================================================
// Perf logging for the psychedelic effects (MEASURE, don't guess - see
// docs/SCREENSAVER_PSYCHEDELIC_DESIGN.md §9). There is no userland syscall
// for writing straight to the serial console, so this follows the same
// established file-log convention main.c already uses for screensaver debug
// (the /SSDEBUG.TXT frame-counter pattern): one fd opened once and kept open
// (so repeated writes append via the advancing file offset, no O_APPEND
// dance needed), a small per-call decimal formatter, and a hard cap on
// total writes so an hours-long unattended run never grows this file
// without bound.
// ============================================================================
// #650: ss_perf_log() DELETED. It opened /SSPERF.TXT and wrote one line per
// rendered frame, from inside the screensaver render loop. It had NEVER
// emitted a single line: the sys_open() failed (no such file), the function
// latched fd = -1, and it disabled itself permanently for the life of the
// process. Verified by running the saver to completion on a throwaway VM and
// then mounting the ext2 root: no /SSPERF.TXT exists.
//
// That is the same zero-readers class as blk_stale_skips() and
// g_msc_noblock_spins - an instrument everyone assumes is watching and which
// has never produced data. Here it was strictly worse than useless, because
// the version that DID open the file would have put a synchronous per-frame
// disk write in the hot render path. Deleted rather than repaired: a
// per-frame file write is not the right shape for this measurement, and the
// numbers it was meant to produce are obtainable from the [HB] top= and
// [FLIPPROF] cpy= fields that already exist and already work.

// ============================================================================
// Helpers
// ============================================================================

// Draw a line using Bresenham's algorithm.
static void ss_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                         uint32_t color) {
    int32_t dx  =  (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int32_t dy  = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int32_t sx  =  (x0 < x1) ? 1 : -1;
    int32_t sy  =  (y0 < y1) ? 1 : -1;
    int32_t err =  dx + dy;

    while (1) {
        draw_putpixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = err * 2;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Draw the outline of a circle using the midpoint circle algorithm.
static void ss_draw_circle_outline(int32_t cx, int32_t cy, int32_t r,
                                   uint32_t color) {
    if (r <= 0) return;
    int32_t x = 0;
    int32_t y = r;
    int32_t d = 3 - 2 * r;

    while (x <= y) {
        draw_putpixel(cx + x, cy + y, color);
        draw_putpixel(cx - x, cy + y, color);
        draw_putpixel(cx + x, cy - y, color);
        draw_putpixel(cx - x, cy - y, color);
        draw_putpixel(cx + y, cy + x, color);
        draw_putpixel(cx - y, cy + x, color);
        draw_putpixel(cx + y, cy - x, color);
        draw_putpixel(cx - y, cy - x, color);
        if (d < 0) {
            d += 4 * x + 6;
        } else {
            d += 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

// Filled circle (small, for cores/heads/stars).
static void ss_fill_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color) {
    if (r < 0) return;
    for (int32_t dy = -r; dy <= r; dy++) {
        for (int32_t dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r)
                draw_putpixel(cx + dx, cy + dy, color);
        }
    }
}

// Apply inclination (squash) + position-angle rotation to a disk-plane offset.
static void ss_xform(int dx, int dy, int incl, int pa, int *ox, int *oy) {
    dy = dy * incl / 16;
    *ox = (dx * SS_COS(pa) - dy * SS_SIN(pa)) / 4096;
    *oy = (dx * SS_SIN(pa) + dy * SS_COS(pa)) / 4096;
}

// Spawn one deep-space object with randomized appearance.
static void ss_obj_spawn(ss_obj_t *o, int far) {
    o->x = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
    o->y = (int32_t)(ss_rand() % (uint32_t)g_fb_height);
    o->z = far ? (int32_t)(200 + ss_rand() % 200) : (int32_t)(1 + ss_rand() % 400);
    o->type   = (int32_t)(ss_rand() % 5);
    o->color  = ss_hue((int)(ss_rand() & 0xFF));
    o->color2 = ss_hue((int)(ss_rand() & 0xFF));
    o->arms   = (int16_t)(2 + ss_rand() % 4);          // 2..5
    o->incl   = (int16_t)(2 + ss_rand() % 15);         // 2..16 edge..face
    o->pa     = (int16_t)(ss_rand() & 0xFF);
    o->spin   = (int16_t)((int)(ss_rand() % 7) - 3);   // -3..3
    o->sizem  = (int16_t)(6 + ss_rand() % 16);         // 0.75x..2.6x
}

// Draw a deep-space object projected at (sx,sy) with apparent radius rad.
static void ss_obj_draw(ss_obj_t *o, int32_t sx, int32_t sy, int32_t rad) {
    if (rad < 2) rad = 2;
    int ox, oy;
    switch (o->type) {
    case 0: { // Galaxy: o->arms spiral arms, spun + inclined
        int steps = 44;
        int rot = (int)g_ss_frame * o->spin / 2;
        for (int arm = 0; arm < o->arms; arm++) {
            int abase = arm * (256 / o->arms) + rot;
            for (int t = 1; t < steps; t++) {
                int a = t * 9 + abase;
                int rr = t * rad / steps;
                ss_xform((rr * SS_COS(a)) / 4096, (rr * SS_SIN(a)) / 4096,
                         o->incl, o->pa, &ox, &oy);
                uint32_t c = (t < 6) ? 0xFFFFFFFF : o->color;
                draw_putpixel(sx + ox, sy + oy, c);
                if (rad > 30) draw_putpixel(sx + ox + 1, sy + oy, c);
            }
        }
        ss_fill_circle(sx, sy, rad / 8 + 1, 0xFFFFF0C0);
        break;
    }
    case 1: { // Black hole: inclined accretion disk (ellipse) + event horizon
        for (int a = 0; a < 256; a += 2) {
            ss_xform((rad * SS_COS(a)) / 4096, (rad * SS_SIN(a)) / 4096,
                     o->incl, o->pa, &ox, &oy);
            draw_putpixel(sx + ox, sy + oy, o->color);
            ss_xform(((rad - 2) * SS_COS(a)) / 4096, ((rad - 2) * SS_SIN(a)) / 4096,
                     o->incl, o->pa, &ox, &oy);
            draw_putpixel(sx + ox, sy + oy, 0xFFFF8020);
        }
        ss_fill_circle(sx, sy, rad / 3 + 1, 0xFF000000);
        break;
    }
    case 2: { // Comet: head + tail along position angle, length from size
        int tl = rad * 3 + (int)o->sizem;
        int tdx = SS_COS(o->pa), tdy = SS_SIN(o->pa);
        for (int t = tl; t > 0; t--) {
            int px = sx - (tdx * t) / 4096;
            int py = sy - (tdy * t) / 4096;
            int b = 220 - (t * 200 / (tl + 1));
            if (b < 40) b = 40;
            draw_putpixel(px, py, 0xFF000000 | ((uint32_t)b << 16) | ((uint32_t)b << 8) | 0xFF);
        }
        ss_fill_circle(sx, sy, rad / 4 + 1, 0xFFFFFFFF);
        break;
    }
    case 3: { // Nebula: colored cloud, elongated by inclination along pa
        uint32_t seed = (uint32_t)(o->x * 131 + o->y * 977 + 7);
        uint32_t cc = o->color;
        uint32_t r = ((cc >> 16) & 0xFF) / 2, g = ((cc >> 8) & 0xFF) / 2, b = (cc & 0xFF) / 2;
        uint32_t col = 0xFF000000 | (r << 16) | (g << 8) | b;
        for (int i = 0; i < 80; i++) {
            seed = seed * 1103515245 + 12345; int dx = (int)((seed >> 16) % (uint32_t)(2 * rad + 1)) - rad;
            seed = seed * 1103515245 + 12345; int dy = (int)((seed >> 16) % (uint32_t)(2 * rad + 1)) - rad;
            if (dx * dx + dy * dy > rad * rad) continue;
            ss_xform(dx, dy, o->incl, o->pa, &ox, &oy);
            draw_putpixel(sx + ox, sy + oy, col);
        }
        break;
    }
    default: { // Double star: two stars (random colors) separated along pa
        int off = rad / 2 + 3 + o->sizem / 3;
        int ddx = (SS_COS(o->pa) * off) / 4096;
        int ddy = (SS_SIN(o->pa) * off) / 4096;
        ss_fill_circle(sx - ddx, sy - ddy, rad / 3 + 1, o->color);
        ss_fill_circle(sx + ddx, sy + ddy, rad / 3 + 1, o->color2);
        break;
    }
    }
}

// Place a planet at a random spot with a slow parallax drift. Planets are
// real textured sprites, so they use fixed on-screen sizes (48..138 px) and
// gently drift + wrap rather than the stars' fly-past perspective (which would
// throw the large near ones off-screen and leave only sub-pixel far dots).
static void ss_planet_spawn(ss_planet_t *p, int unused) {
    (void)unused;
    p->x   = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
    p->y   = (int32_t)(ss_rand() % (uint32_t)g_fb_height);
    p->sz  = (int32_t)(48 + ss_rand() % 90);
    p->vx  = (int32_t)(ss_rand() % 3) - 1;   // -1..1
    p->vy  = (int32_t)(ss_rand() % 3) - 1;
    if (p->vx == 0 && p->vy == 0) p->vx = 1;
    p->img = (int)(ss_rand() % PLANET_COUNT);
}

// Alpha-blit a planet sprite (nearest-scaled to `sz`, centered at cx,cy) over
// the framebuffer. Straight over g_fb like ss_gl_render; the starfield redraws
// the whole screen each frame so no dirty-rect bookkeeping is needed here.
static void ss_blit_planet(int img, int cx, int cy, int sz) {
    if (img < 0 || img >= PLANET_COUNT) return;
    if (sz < 3) return;
    if (sz > 480) sz = 480;
    const unsigned int *src = g_planet_px[img];
    int x0 = cx - sz / 2, y0 = cy - sz / 2;
    // NOTE: pixels go out through draw_putpixel(), not a raw g_fb[] store.
    // The starfield draws with draw_putpixel and only that path reaches the
    // presented surface here; a direct g_fb write from this module never
    // showed up. The space backdrop is solid black, so edge antialiasing is
    // done by pre-multiplying the sprite alpha against black (no read needed).
    for (int j = 0; j < sz; j++) {
        int dy = y0 + j;
        if (dy < 0 || dy >= g_fb_height) continue;
        int sj = (j * PLANET_SZ) / sz;
        const unsigned int *srow = src + sj * PLANET_SZ;
        for (int i = 0; i < sz; i++) {
            int dx = x0 + i;
            if (dx < 0 || dx >= g_fb_width) continue;
            unsigned int s = srow[(i * PLANET_SZ) / sz];
            unsigned int a = s >> 24;
            if (!a) continue;
            unsigned int sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
            if (a < 255) { sr = sr * a / 255; sg = sg * a / 255; sb = sb * a / 255; }
            draw_putpixel(dx, dy, 0xFF000000u | (sr << 16) | (sg << 8) | sb);
        }
    }
}

// ============================================================================
// screensaver_init
// ============================================================================

void screensaver_init(void) {
    int32_t i;

    // Psychedelic redesign shared pipeline (palettes etc.) - idempotent, so
    // safe to call every time this runs (screensaver_set_type() calls
    // screensaver_init() on every effect switch, not just once at boot).
    ss_gfx_init();

    // Starfield: spread stars across the virtual 3-D volume.
    for (i = 0; i < SS_MAX_STARS; i++) {
        g_stars[i].x = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
        g_stars[i].y = (int32_t)(ss_rand() % (uint32_t)g_fb_height);
        g_stars[i].z = (int32_t)((ss_rand() % 256) + 1);
    }

    // Lines: random start and end points with random bounce velocities.
    for (i = 0; i < SS_MAX_LINES; i++) {
        g_lines[i].x1 = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
        g_lines[i].y1 = (int32_t)(ss_rand() % (uint32_t)g_fb_height);
        g_lines[i].x2 = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
        g_lines[i].y2 = (int32_t)(ss_rand() % (uint32_t)g_fb_height);

        // Velocities in range [-4, 4], excluding 0.
        int32_t d1x = (int32_t)(ss_rand() % 4) + 1;
        int32_t d1y = (int32_t)(ss_rand() % 4) + 1;
        int32_t d2x = (int32_t)(ss_rand() % 4) + 1;
        int32_t d2y = (int32_t)(ss_rand() % 4) + 1;
        g_lines[i].dx1 = (ss_rand() & 1) ? d1x : -d1x;
        g_lines[i].dy1 = (ss_rand() & 1) ? d1y : -d1y;
        g_lines[i].dx2 = (ss_rand() & 1) ? d2x : -d2x;
        g_lines[i].dy2 = (ss_rand() & 1) ? d2y : -d2y;

        // Random saturated color with full alpha.
        uint32_t r = (ss_rand() % 128) + 64;
        uint32_t g = (ss_rand() % 128) + 64;
        uint32_t b = (ss_rand() % 128) + 64;
        g_lines[i].color = 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    // Bubbles: random center, radius starting at 1, random max and color.
    for (i = 0; i < SS_MAX_BUBBLES; i++) {
        g_bubbles[i].x          = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
        g_bubbles[i].y          = (int32_t)(ss_rand() % (uint32_t)g_fb_height);
        g_bubbles[i].radius     = (int32_t)((ss_rand() % 30) + 1);
        g_bubbles[i].max_radius = (int32_t)((ss_rand() % 80) + 20);
        g_bubbles[i].dr         = 1; // growing

        uint32_t r = (ss_rand() % 128) + 64;
        uint32_t g = (ss_rand() % 128) + 64;
        uint32_t b = (ss_rand() % 128) + 64;
        g_bubbles[i].color = 0xFF000000 | (r << 16) | (g << 8) | b;
    }

    // Deep-space objects spread through the volume.
    for (i = 0; i < SS_MAX_OBJS; i++) {
        ss_obj_spawn(&g_objs[i], 0);
    }

    // Drifting planets spread through the volume.
    for (i = 0; i < SS_MAX_PLANETS; i++) {
        ss_planet_spawn(&g_planets[i], 0);
    }

    g_ss_frame = 0;
}

// ============================================================================
// #319 GL screensavers (TinyGL): render the shared gldemo cores straight into
// the compositor framebuffer. One TinyGL context per process, (re)initialized
// lazily when a GL saver becomes active or the screen size changes.
// (reconciled #336)
// ============================================================================
static int  g_gl_mode = -1;   // currently-initialized gldemo mode, -1 = none
static int  g_gl_w = 0, g_gl_h = 0;

static void ss_gl_render(int mode) {
    if (g_gl_mode != mode || g_gl_w != g_fb_width || g_gl_h != g_fb_height) {
        gldemo_init(mode, g_fb_width, g_fb_height);
        g_gl_mode = mode;
        g_gl_w = g_fb_width;
        g_gl_h = g_fb_height;
    }
    gldemo_frame(g_fb, g_fb_pitch);
}

static void ss_gl_teardown(void) {
    if (g_gl_mode != -1) {
        gldemo_shutdown();
        g_gl_mode = -1;
    }
}

// #560: true for every GL-backed screensaver type (the original two GLCUBE/
// GLMATRIX plus the ten added in #560). Relies on the enum block in
// compositor.h staying contiguous from SS_GLCUBE through SS_GLLAVA.
static int ss_is_gl_type(int t) {
    return t >= SS_GLCUBE && t <= SS_GLLAVA;
}

// ============================================================================
// screensaver_render
// ============================================================================

void screensaver_render(void) {
    int32_t i;

    switch (g_ss_type) {

    // -----------------------------------------------------------------------
    case SS_NONE:
        // No screensaver, nothing to draw.
        break;

    // -----------------------------------------------------------------------
    case SS_BLANK:
        draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF000000);
        break;

    // -----------------------------------------------------------------------
    case SS_STARFIELD: {
        draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF000000);

        int32_t cx = g_fb_width  / 2;
        int32_t cy = g_fb_height / 2;

        for (i = 0; i < SS_MAX_STARS; i++) {
            // Move star closer to the camera (lower z = nearer).
            g_stars[i].z -= 2;

            // Recycle stars that fly past the camera.
            if (g_stars[i].z <= 0) {
                g_stars[i].x = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
                g_stars[i].y = (int32_t)(ss_rand() % (uint32_t)g_fb_height);
                g_stars[i].z = 255;
            }

            int32_t z = g_stars[i].z;

            // Project onto the 2-D screen.
            int32_t sx = cx + (g_stars[i].x - cx) * 256 / z;
            int32_t sy = cy + (g_stars[i].y - cy) * 256 / z;

            // Clip to framebuffer bounds before drawing.
            if (sx < 0 || sx >= g_fb_width || sy < 0 || sy >= g_fb_height) {
                continue;
            }

            // Brightness: stars with small z are close, therefore bright.
            // Map z in [1,255] linearly to brightness in [255,20].
            uint32_t bright = (uint32_t)(255 - (z * 235 / 255));
            uint32_t color  = 0xFF000000 | (bright << 16) | (bright << 8) | bright;

            if (z < 64) {
                // Draw a 2x2 block for nearby stars.
                draw_putpixel(sx,     sy,     color);
                draw_putpixel(sx + 1, sy,     color);
                draw_putpixel(sx,     sy + 1, color);
                draw_putpixel(sx + 1, sy + 1, color);
            } else {
                draw_putpixel(sx, sy, color);
            }
        }

        // Drifting planets: real textured sprites at fixed sizes with a slow
        // parallax drift (wrap at the edges). Drawn before the deep-space
        // objects so a galaxy/comet can pass in front. Uses draw_putpixel (the
        // only draw path that reaches the presented surface from this module).
        for (i = 0; i < SS_MAX_PLANETS; i++) {
            int32_t sz = g_planets[i].sz;
            g_planets[i].x += g_planets[i].vx;
            g_planets[i].y += g_planets[i].vy;
            if (g_planets[i].x < -sz)                   g_planets[i].x = g_fb_width  + sz;
            else if (g_planets[i].x > g_fb_width + sz)  g_planets[i].x = -sz;
            if (g_planets[i].y < -sz)                   g_planets[i].y = g_fb_height + sz;
            else if (g_planets[i].y > g_fb_height + sz) g_planets[i].y = -sz;
            ss_blit_planet(g_planets[i].img, g_planets[i].x, g_planets[i].y, sz);
        }

        // Deep-space objects (galaxies, black holes, comets, nebulae, doubles).
        for (i = 0; i < SS_MAX_OBJS; i++) {
            g_objs[i].z -= 1;
            if (g_objs[i].z <= 0) ss_obj_spawn(&g_objs[i], 1);
            int32_t oz = g_objs[i].z;
            int32_t osx = cx + (g_objs[i].x - cx) * 256 / oz;
            int32_t osy = cy + (g_objs[i].y - cy) * 256 / oz;
            if (osx < -80 || osx >= g_fb_width + 80 ||
                osy < -80 || osy >= g_fb_height + 80) continue;
            static const int base[5] = {30, 14, 7, 34, 6};
            int32_t rad = base[g_objs[i].type] * 200 / oz;
            rad = rad * g_objs[i].sizem / 8;   // per-object size variation
            if (rad > 110) rad = 110;
            ss_obj_draw(&g_objs[i], osx, osy, rad);
        }

        break;
    }

    // -----------------------------------------------------------------------
    case SS_LINES:
        draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF000000);

        for (i = 0; i < SS_MAX_LINES; i++) {
            // Update endpoints and bounce off screen edges.
            g_lines[i].x1 += g_lines[i].dx1;
            g_lines[i].y1 += g_lines[i].dy1;
            g_lines[i].x2 += g_lines[i].dx2;
            g_lines[i].y2 += g_lines[i].dy2;

            if (g_lines[i].x1 < 0 || g_lines[i].x1 >= g_fb_width) {
                g_lines[i].dx1 = -g_lines[i].dx1;
                g_lines[i].x1 += g_lines[i].dx1 * 2;
            }
            if (g_lines[i].y1 < 0 || g_lines[i].y1 >= g_fb_height) {
                g_lines[i].dy1 = -g_lines[i].dy1;
                g_lines[i].y1 += g_lines[i].dy1 * 2;
            }
            if (g_lines[i].x2 < 0 || g_lines[i].x2 >= g_fb_width) {
                g_lines[i].dx2 = -g_lines[i].dx2;
                g_lines[i].x2 += g_lines[i].dx2 * 2;
            }
            if (g_lines[i].y2 < 0 || g_lines[i].y2 >= g_fb_height) {
                g_lines[i].dy2 = -g_lines[i].dy2;
                g_lines[i].y2 += g_lines[i].dy2 * 2;
            }

            ss_draw_line(g_lines[i].x1, g_lines[i].y1,
                         g_lines[i].x2, g_lines[i].y2,
                         g_lines[i].color);
        }
        break;

    // -----------------------------------------------------------------------
    case SS_BUBBLES:
        draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF000000);

        for (i = 0; i < SS_MAX_BUBBLES; i++) {
            // Grow or shrink the radius.
            g_bubbles[i].radius += g_bubbles[i].dr;

            if (g_bubbles[i].radius >= g_bubbles[i].max_radius) {
                // Reached maximum: start shrinking.
                g_bubbles[i].dr = -1;
            } else if (g_bubbles[i].radius <= 0) {
                // Collapsed: respawn at a new position with a new max radius.
                g_bubbles[i].x          = (int32_t)(ss_rand() % (uint32_t)g_fb_width);
                g_bubbles[i].y          = (int32_t)(ss_rand() % (uint32_t)g_fb_height);
                g_bubbles[i].max_radius = (int32_t)((ss_rand() % 80) + 20);
                g_bubbles[i].radius     = 1;
                g_bubbles[i].dr         = 1;

                uint32_t r = (ss_rand() % 128) + 64;
                uint32_t g = (ss_rand() % 128) + 64;
                uint32_t b = (ss_rand() % 128) + 64;
                g_bubbles[i].color = 0xFF000000 | (r << 16) | (g << 8) | b;
            }

            ss_draw_circle_outline(g_bubbles[i].x, g_bubbles[i].y,
                                   g_bubbles[i].radius, g_bubbles[i].color);
        }
        break;

    // -----------------------------------------------------------------------
    case SS_FLUX: {
        // Fade the previous frame (~13/16) so the spiral arms leave trails.
        for (int32_t y = 0; y < g_fb_height; y++) {
            uint32_t *row = &g_fb[y * g_fb_pitch];
            for (int32_t x = 0; x < g_fb_width; x++) {
                uint32_t c = row[x];
                uint32_t r = (((c >> 16) & 0xFF) * 13) >> 4;
                uint32_t g = (((c >> 8) & 0xFF) * 13) >> 4;
                uint32_t b = ((c & 0xFF) * 13) >> 4;
                row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
        int32_t cx = g_fb_width / 2, cy = g_fb_height / 2;
        int32_t t = (int32_t)g_ss_frame;
        int32_t maxr = (g_fb_height < g_fb_width ? g_fb_height : g_fb_width) * 2 / 5;
        const int arms = 7;
        for (int a = 0; a < arms; a++) {
            for (int p = 0; p < 70; p++) {
                int ang = t * 2 + a * (256 / arms) + p * 3;
                int rad = (p * maxr) / 70;
                rad += (SS_SIN(t * 3 + p * 6) * (maxr / 6)) / 4096;
                int x = cx + (rad * SS_COS(ang)) / 4096;
                int y = cy + (rad * SS_SIN(ang)) / 4096;
                uint32_t col = ss_hue(a * 36 + p * 2 + t);
                draw_putpixel(x,     y,     col);
                draw_putpixel(x + 1, y,     col);
                draw_putpixel(x,     y + 1, col);
                draw_putpixel(x + 1, y + 1, col);
            }
        }
        break;
    }

    // -----------------------------------------------------------------------
    case SS_MATRIX: {
        // Digital rain (#282). Persistent column heads; the green channel is
        // faded each frame so glyphs leave trailing tails.
        #define MX_MAX 256
        static int     s_mx_init = 0;
        static int32_t mx_y[MX_MAX];
        static int32_t mx_sp[MX_MAX];
        const int cw = 12;                       // column cell width
        int ncol = g_fb_width / cw;
        if (ncol > MX_MAX) ncol = MX_MAX;
        if (!s_mx_init) {
            for (int c = 0; c < MX_MAX; c++) {
                int hh = g_fb_height ? g_fb_height : 1;
                mx_y[c]  = -(int32_t)(ss_rand() % (uint32_t)hh);
                mx_sp[c] = 4 + (int32_t)(ss_rand() % 10);
            }
            s_mx_init = 1;
        }
        for (int32_t y = 0; y < g_fb_height; y++) {
            uint32_t *row = &g_fb[y * g_fb_pitch];
            for (int32_t x = 0; x < g_fb_width; x++) {
                uint32_t cc = row[x];
                uint32_t gg = (((cc >> 8) & 0xFF) * 13) >> 4;
                row[x] = 0xFF000000u | (gg << 8);
            }
        }
        for (int c = 0; c < ncol; c++) {
            mx_y[c] += mx_sp[c];
            if (mx_y[c] - cw * 18 > g_fb_height) {
                mx_y[c]  = -(int32_t)(ss_rand() % 240);
                mx_sp[c] = 4 + (int32_t)(ss_rand() % 10);
            }
            int x = c * cw + 1;
            int head = mx_y[c];
            for (int k = 0; k < 18; k++) {
                int gy = head - k * cw;
                if (gy < 0 || gy + 7 >= g_fb_height) continue;
                uint32_t col;
                if (k == 0) col = 0xFFE8FFE8;
                else { int b = 230 - k * 12; if (b < 40) b = 40;
                       col = 0xFF000000u | ((uint32_t)b << 8); }
                uint32_t bits = ss_rand();
                for (int yy = 0; yy < 7; yy++)
                    for (int xx = 0; xx < 5; xx++)
                        if ((bits >> ((yy * 5 + xx) & 31)) & 1)
                            draw_putpixel(x + xx, gy + yy, col);
            }
        }
        #undef MX_MAX
        break;
    }

    // -----------------------------------------------------------------------
    case SS_PLASMA: {
        // "Plasma Reborn" (psychedelic redesign §4.3): direct upgrade of the
        // classic plasma - same shared-sine family the old code already
        // used, now rendered into the low-res HI buffer and bilinearly
        // upscaled (smooth, not the old blocky step=4 squares), indexed
        // through a curated palette LUT instead of the raw HSV wheel, with
        // an added radial term for organic curvature, shared bloom, and
        // slow palette cycling. This slot is a strict superset of the old
        // SS_PLASMA, replaced in place (design doc §11 Q2).
        uint32_t *buf = ss_lores_buf(SS_LORES_HI_W, SS_LORES_HI_H);
        if (!buf) break;   // never dereference NULL; just skip this frame

        int t  = (int)g_ss_frame;
        int cx = SS_LORES_HI_W / 2, cy = SS_LORES_HI_H / 2;
        for (int y = 0; y < SS_LORES_HI_H; y++) {
            uint32_t *row = &buf[y * SS_LORES_HI_W];
            int dy = y - cy;
            for (int x = 0; x < SS_LORES_HI_W; x++) {
                int dx   = x - cx;
                int dist = ss_isqrt((uint32_t)(dx * dx + dy * dy));
                int v = SS_SIN(x / 5 + t)
                      + SS_SIN(y / 7 - t)
                      + SS_SIN((x + y) / 9 + t)
                      + SS_SIN((x - y) / 13 - t / 2)
                      + SS_SIN(dist * 3 / 2 - t * 2);   // radial term: organic curvature
                int idx = ((v >> 7) + t / 3 + ss_palette_phase) & 0xFF;
                row[x] = ss_pal(SS_PAL_ACID, idx);
            }
        }
        ss_palette_tick();
        ss_lores_bloom(buf, SS_LORES_HI_W, SS_LORES_HI_H, 3, 45);
        ss_lores_upscale_to_fb(buf, SS_LORES_HI_W, SS_LORES_HI_H);
        break;
    }

    // -----------------------------------------------------------------------
    case SS_GLCUBE:    // #319 TinyGL spinning textured cube (reconciled #336)
        ss_gl_render(GLDEMO_CUBE);
        break;

    // -----------------------------------------------------------------------
    case SS_GLMATRIX:  // #319 TinyGL 3D matrix code rain (reconciled #336)
        ss_gl_render(GLDEMO_MATRIX);
        break;

    // -----------------------------------------------------------------------
    // #560: ten psychedelic/geometric TinyGL screensavers.
    case SS_GLTUNNEL:     ss_gl_render(GLDEMO_TUNNEL);     break;
    case SS_GLKALEIDO:    ss_gl_render(GLDEMO_KALEIDO);    break;
    case SS_GLPLATONIC:   ss_gl_render(GLDEMO_PLATONIC);   break;
    case SS_GLLORENZ:     ss_gl_render(GLDEMO_LORENZ);     break;
    case SS_GLMOBIUS:     ss_gl_render(GLDEMO_MOBIUS);     break;
    case SS_GLWAVEMESH:   ss_gl_render(GLDEMO_WAVEMESH);   break;
    case SS_GLSPIROGRAPH: ss_gl_render(GLDEMO_SPIROGRAPH); break;
    case SS_GLHYPERCUBE:  ss_gl_render(GLDEMO_HYPERCUBE);  break;
    case SS_GLVORTEX:     ss_gl_render(GLDEMO_VORTEX);     break;
    case SS_GLLAVA:       ss_gl_render(GLDEMO_LAVA);       break;

    // -----------------------------------------------------------------------
    // Psychedelic redesign lead trio (docs/SCREENSAVER_PSYCHEDELIC_DESIGN.md).
    // New IDs (>= 20, direct-pixel, NOT TinyGL - design doc principle 7).
    // -----------------------------------------------------------------------

    case SS_FLAME: {
        // Fractal Flame "Bloom Garden" (design doc §4.1): IFS chaos-game
        // point plot accumulated into a histogram over many frames (the
        // "long exposure" look), tone-mapped through an isqrt gamma into
        // the Ultraviolet palette, then bloomed. Detail BUILDS UP over
        // time by design; a subtle ambient backdrop (also Ultraviolet,
        // low brightness) keeps the buffer full-screen color from frame
        // one rather than looking sparse before the histogram fills in.
        uint16_t *density; uint8_t *hue;
        if (!ss_flame_hist(&density, &hue)) break;

        int t = (int)g_ss_frame;   // used by the ambient backdrop drift below

        static int      s_fl_init     = 0;
        static int32_t  s_fx, s_fy;        // chaos-game point, Q12 fixed point (-4096..4096)
        static uint32_t s_fl_reset_at = 0;

        // Five contractive affine transforms in Q12 fixed point (|coeff| <
        // 4096 = 1.0, so the linear part alone is already contractive),
        // each tagged with a hue (0..255) so "which transform last painted
        // this cell" gives free, cheap per-region coloring. Blended 50/50
        // below with a sinusoidal variation bounded to +-4096 by SS_SIN's
        // own amplitude, so the point can never leave the [-4096,4096]
        // square - no per-step clamp needed for stability.
        static const int32_t xf[5][6] = {
            /*  a,     b,     c,    d,     e,     f */
            {  2048,     0,     0,     0,  2048,     0 },
            {  2048,     0,  2048,     0,  2048,     0 },
            {  2048,     0,  1024,     0,  2048,  2048 },
            {  1229, -1229,   819,  1229,  1229,  -819 },
            { -1229,  1229,  -819, -1229, -1229,   819 },
        };
        static const uint8_t xf_hue[5]    = { 0, 51, 102, 153, 204 };
        static const int     xf_weight[5] = { 25, 25, 20, 15, 15 };   // sums to 100

        if (!s_fl_init) {
            s_fx = 0; s_fy = 0;
            for (int i = 0; i < SS_FLAME_W * SS_FLAME_H; i++) { density[i] = 0; hue[i] = 0; }
            s_fl_init = 1;
            s_fl_reset_at = g_ss_frame + 9000;   // rare full reset (~5 min at 30Hz)
        }
        if (g_ss_frame >= s_fl_reset_at) {
            for (int i = 0; i < SS_FLAME_W * SS_FLAME_H; i++) { density[i] = 0; hue[i] = 0; }
            s_fx = 0; s_fy = 0;
            s_fl_reset_at = g_ss_frame + 9000;
        }

        // Decay (~253/256, design doc §4.1) so the flame stays a living,
        // slowly evolving structure rather than monotonically saturating.
        for (int i = 0; i < SS_FLAME_W * SS_FLAME_H; i++) {
            density[i] = (uint16_t)(((uint32_t)density[i] * 253) >> 8);
        }

        // Chaos-game point plot: bounded iteration count (design principle
        // 6 - never an unbounded per-frame loop). NOT per-pixel.
        {
            const int ITERS = 5000;
            for (int it = 0; it < ITERS; it++) {
                int r = (int)(ss_rand() % 100);
                int pick = 4, acc = 0;
                for (int k = 0; k < 5; k++) { acc += xf_weight[k]; if (r < acc) { pick = k; break; } }
                const int32_t *c = xf[pick];
                int32_t nx = ((c[0] * s_fx + c[1] * s_fy) >> 12) + c[2];
                int32_t ny = ((c[3] * s_fx + c[4] * s_fy) >> 12) + c[5];
                int32_t vx = SS_SIN((nx * 128) / 4096);
                int32_t vy = SS_SIN((ny * 128) / 4096);
                nx = (nx + vx) / 2;
                ny = (ny + vy) / 2;
                s_fx = nx; s_fy = ny;

                if (it < 20) continue;   // discard the IFS settle-in

                int32_t px = SS_FLAME_W / 2 + (s_fx * (SS_FLAME_W / 2 - 4)) / 4096;
                int32_t py = SS_FLAME_H / 2 + (s_fy * (SS_FLAME_H / 2 - 4)) / 4096;
                if (px < 0 || px >= SS_FLAME_W || py < 0 || py >= SS_FLAME_H) continue;
                int32_t cell = py * SS_FLAME_W + px;
                if (density[cell] < 65535) density[cell]++;
                hue[cell] = xf_hue[pick];
            }
        }

        // Tone-map + ambient backdrop + composite into the shared HI color
        // buffer (bounded 320x200 = 64000 cells, same cost class as
        // Plasma's per-cell work).
        uint32_t *buf = ss_lores_buf(SS_LORES_HI_W, SS_LORES_HI_H);
        if (buf) {
            for (int y = 0; y < SS_FLAME_H; y++) {
                for (int x = 0; x < SS_FLAME_W; x++) {
                    int i = y * SS_FLAME_W + x;

                    // MEASURED FIX (the build host VM <vmid> live screendump): the first
                    // cut of this ambient term used x*2/y*3 (a MULTIPLY, not
                    // the divide every other low-res effect in this file
                    // uses), which wraps the 256-entry SS_SIN table 2-3
                    // times across the 320x200 buffer and reads as a small
                    // repeating tiled-oval pattern, not one smooth wash -
                    // plus a 40/255 (~16%) brightness scale that stayed too
                    // close to the palette's near-black low end, so the
                    // whole buffer looked mostly dark with sparse dots (the
                    // literal "lines on black" failure this redesign exists
                    // to fix). Low frequency (divide, under one full cycle
                    // across the buffer - the same idiom Plasma Reborn
                    // already uses) plus a +128 index offset (stays in
                    // Ultraviolet's violet/hot-pink/white range, away from
                    // its dark indigo end) plus a much stronger brightness
                    // scale fixes both at once.
                    int av   = SS_SIN(x / 40 + t / 3) + SS_SIN(y / 30 - t / 4);
                    int aidx = (128 + (av >> 5) + t / 5 + ss_palette_phase) & 0xFF;
                    uint32_t ac = ss_pal(SS_PAL_ULTRAVIOLET, aidx);
                    int ar = (int)((ac >> 16) & 0xFF) * 150 / 255;
                    int ag = (int)((ac >> 8) & 0xFF) * 150 / 255;
                    int ab = (int)(ac & 0xFF) * 150 / 255;

                    // Gain bumped 64 -> 220: MEASURED live, the classic
                    // 3-vertex-contraction transforms below (T0-T2) are
                    // structurally a Sierpinski-triangle-family IFS, which
                    // is a genuinely thin/low-area fractal (Hausdorff dim
                    // ~1.58) even after millions of accumulated points, so
                    // per-cell density stays low; a bigger gain makes that
                    // real (not sparse-by-bug) filament actually visible as
                    // a bright accent on top of the ambient wash above,
                    // instead of reading as near-invisible pixel dust.
                    int bright = ss_isqrt((uint32_t)density[i] * 220u);
                    if (bright > 255) bright = 255;
                    uint32_t base = ss_pal(SS_PAL_ULTRAVIOLET, hue[i]);
                    int br = (int)((base >> 16) & 0xFF) * bright / 255;
                    int bg = (int)((base >> 8) & 0xFF) * bright / 255;
                    int bb = (int)(base & 0xFF) * bright / 255;

                    int r = ar + br; if (r > 255) r = 255;
                    int g = ag + bg; if (g > 255) g = 255;
                    int b = ab + bb; if (b > 255) b = 255;
                    buf[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                }
            }
            ss_palette_tick();
            ss_lores_bloom(buf, SS_LORES_HI_W, SS_LORES_HI_H, 4, 90);
            ss_lores_upscale_to_fb(buf, SS_LORES_HI_W, SS_LORES_HI_H);
        }
        break;
    }

    case SS_STAINEDGLASS: {
        // Stained-Glass Warp (design doc §4.6): N drifting feature points,
        // a brute-force nearest/second-nearest Voronoi scan per low-res
        // cell. Hard ink-black edges near cell boundaries; smooth per-cell
        // radial gradient interiors elsewhere - the literal "smooth
        // gradients AND hard dark-edged shapes in the same frame" brief.
        uint32_t *buf = ss_lores_buf(SS_LORES_LO_W, SS_LORES_LO_H);
        if (!buf) break;

        #define SG_N 10
        typedef struct { int32_t x, y, vx, vy; uint8_t hue; } sg_pt_t;
        static sg_pt_t s_sg[SG_N];
        static int     s_sg_init = 0;
        if (!s_sg_init) {
            for (int i = 0; i < SG_N; i++) {
                s_sg[i].x = (int32_t)(ss_rand() % (uint32_t)SS_LORES_LO_W);
                s_sg[i].y = (int32_t)(ss_rand() % (uint32_t)SS_LORES_LO_H);
                int32_t vx = (int32_t)(ss_rand() % 3) - 1; if (vx == 0) vx = 1;
                int32_t vy = (int32_t)(ss_rand() % 3) - 1; if (vy == 0) vy = 1;
                s_sg[i].vx  = vx;
                s_sg[i].vy  = vy;
                s_sg[i].hue = (uint8_t)((255 * i) / SG_N);
            }
            s_sg_init = 1;
        }
        for (int i = 0; i < SG_N; i++) {
            s_sg[i].x += s_sg[i].vx;
            s_sg[i].y += s_sg[i].vy;
            if (s_sg[i].x < 0 || s_sg[i].x >= SS_LORES_LO_W) { s_sg[i].vx = -s_sg[i].vx; s_sg[i].x += s_sg[i].vx * 2; }
            if (s_sg[i].y < 0 || s_sg[i].y >= SS_LORES_LO_H) { s_sg[i].vy = -s_sg[i].vy; s_sg[i].y += s_sg[i].vy * 2; }
        }

        for (int y = 0; y < SS_LORES_LO_H; y++) {
            uint32_t *row = &buf[y * SS_LORES_LO_W];
            for (int x = 0; x < SS_LORES_LO_W; x++) {
                int32_t d1 = 0x7FFFFFFF, d2 = 0x7FFFFFFF;
                int best = 0;
                for (int i = 0; i < SG_N; i++) {
                    int32_t ddx = x - s_sg[i].x, ddy = y - s_sg[i].y;
                    int32_t ds  = ddx * ddx + ddy * ddy;
                    if (ds < d1) { d2 = d1; d1 = ds; best = i; }
                    else if (ds < d2) { d2 = ds; }
                }
                int dist1 = ss_isqrt((uint32_t)d1);
                int dist2 = ss_isqrt((uint32_t)d2);
                int edge  = dist2 - dist1;
                if (edge < 3) {
                    // Hard ink-black boundary.
                    int e2 = edge < 0 ? 0 : edge;
                    int shade = e2 * 60 / 3;   // ramps 0..~60 as the boundary widens to the threshold
                    row[x] = 0xFF000000u | ((uint32_t)shade << 16) | ((uint32_t)shade << 8) | (uint32_t)shade;
                } else {
                    // Smooth radial gradient interior, own hue per pane.
                    uint32_t base = ss_pal(SS_PAL_DEEPSEA, s_sg[best].hue + ss_palette_phase);
                    int bright = 255 - dist1 * 5;
                    if (bright < 60) bright = 60;
                    if (bright > 255) bright = 255;
                    int r = (int)((base >> 16) & 0xFF) * bright / 255;
                    int g = (int)((base >> 8) & 0xFF) * bright / 255;
                    int b = (int)(base & 0xFF) * bright / 255;
                    row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                }
            }
        }
        ss_palette_tick();
        ss_lores_bloom(buf, SS_LORES_LO_W, SS_LORES_LO_H, 1, 18);
        ss_lores_upscale_to_fb(buf, SS_LORES_LO_W, SS_LORES_LO_H);
        #undef SG_N
        break;
    }

    // -----------------------------------------------------------------------
    case SS_PLASMACLASSIC: {
        // #124: the ORIGINAL plasma (#282), restored verbatim from ff3ca5f^.
        // ff3ca5f replaced SS_PLASMA in place with "Plasma Reborn", which is a
        // superset in capability but a DIFFERENT LOOK: smooth bilinear upscale
        // vs these hard 4px blocks, a curated palette LUT vs the raw HSV
        // wheel, plus a radial term, bloom and slow palette cycling this one
        // deliberately has none of. Both ship now so the two are selectable
        // side by side.
        //
        // #650/#652 NOTE: this predates both perf fixes but cannot reintroduce
        // them. The frame cap (SS_FRAME_MIN_MS) and the blank-after stage
        // (SS_BLANK_AFTER_MS) are type-independent gates in main.c's
        // render_frame(), applied BEFORE screensaver_render() is called at
        // all. #650's per-output-pixel divide lived in
        // ss_lores_upscale_to_fb(), which this effect does not use: its
        // divides are per 4x4 CELL (one sixteenth of the output pixel count),
        // not per pixel, and there is no bloom pass.
        int t = (int)g_ss_frame;
        const int step = 4;
        for (int y = 0; y < g_fb_height; y += step) {
            for (int x = 0; x < g_fb_width; x += step) {
                int v = SS_SIN(x / 6 + t)
                      + SS_SIN(y / 8 - t)
                      + SS_SIN((x + y) / 10 + t)
                      + SS_SIN((x - y) / 14 - t / 2);
                int hue = ((v >> 7) + t) & 0xFF;
                draw_fill_rect(x, y, step, step, ss_hue(hue));
            }
        }
        break;
    }
    }

    g_ss_frame++;
}

// ============================================================================
// screensaver_on_input
// ============================================================================

void screensaver_on_input(void) {
    // Reset the idle timer to the current monotonic millisecond.
    g_idle_ms = uptime_ms();

    // Dismiss an active screensaver on any user input, except for a brief
    // grace window right after activation (#570). Without this, the very
    // click that activates the saver (Settings "Test", or a stray motion
    // from that same click's button-UP) instantly wakes it again, so it
    // flashes for a frame and vanishes before anyone (or a screenshot) can
    // see it. uptime_ms() is the same monotonic kernel clock used for the
    // idle timeout below, so this needs no extra polling or busy-wait.
    if (g_screensaver_active) {
        uint64_t now = uptime_ms();
        if (now - g_ss_active_ms < SS_ACTIVATE_GRACE_MS) {
            return;   // still within the activation grace window: ignore
        }
        g_screensaver_active = false;
        g_needs_redraw       = true;
    }
}

// Record the activation timestamp. Called from every place that flips
// g_screensaver_active to true: the idle timeout below, the Settings "Test"
// one-shot trigger, and the TESTHOOK SAVER command (main.c / testhook.c).
void screensaver_note_activated(void) {
    g_ss_active_ms = uptime_ms();
}

// #652: expose the last-activation timestamp so main.c's render_frame() can
// tell how long the screensaver has been running CONTINUOUSLY (as opposed to
// g_idle_ms, which tracks idle time BEFORE activation and keeps advancing
// only on input). Used to gate the blank-after stage (SS_BLANK_AFTER_MS).
uint64_t screensaver_active_since_ms(void) {
    return g_ss_active_ms;
}

// ============================================================================
// screensaver_check_timeout
// ============================================================================

bool screensaver_check_timeout(void) {
    // Already active: keep it running.
    if (g_screensaver_active) {
        return true;
    }

    // SS_NONE means the feature is disabled entirely.
    if (g_ss_type == SS_NONE) {
        return false;
    }

    // #596: fullscreen presenting-app exemption. "Idle" used to mean "no
    // keyboard/mouse INPUT", which is wrong for a fullscreen game or GL app in
    // a no-input phase: it is rendering every frame, yet after the timeout the
    // screensaver activated and fully occluded it until real input arrived.
    // That also broke every headless verification run (plasma on screen was
    // mistaken for the desktop while an app was actually running underneath).
    // main.c stamps g_fs_present_ms whenever a TRUE-FULLSCREEN window presents
    // a frame; while that stamp is fresh, treat it as activity: hold the idle
    // timer at "now" and never activate. When the app stops presenting (or is
    // closed) the stamp goes stale and the full idle timeout starts from that
    // moment, so plain idle-desktop behavior is completely unchanged.
    if (g_fs_present_ms != 0 &&
        (uptime_ms() - g_fs_present_ms) < SS_FS_PRESENT_GRACE_MS) {
        g_idle_ms = uptime_ms();
        return false;
    }

    // Activation delay is read live from the kernel (#115) so a Settings
    // change applies without restarting the compositor; guard with default.
    int delay = get_ss_delay();
    if (delay < 5) delay = SS_DEFAULT_TIMEOUT;
    g_ss_timeout = delay;
    // Compare real elapsed time in milliseconds (uptime_ms is kernel-
    // reported, ticks*1000/g_timer_hz). No tick-rate divisor in userland,
    // so this can never drift if the timer frequency changes.
    uint64_t elapsed_ms = uptime_ms() - g_idle_ms;

    if (elapsed_ms >= (uint64_t)g_ss_timeout * 1000ULL) {
        g_screensaver_active = true;
        screensaver_note_activated();
        return true;
    }

    return false;
}


// Change the active screensaver effect at runtime (Settings selection).
void screensaver_set_type(int t) {
    // #319 allow GL saver ids, extended #560; extended again for the
    // psychedelic redesign's two new direct-pixel IDs (SS_FLAME,
    // SS_STAINEDGLASS, both >= 20, both NOT TinyGL).
    // #124: upper bound follows the last enumerator, now SS_PLASMACLASSIC.
    if (t < 0 || t > SS_PLASMACLASSIC) return;
    // #560/#571 GATE REMOVED (was: `if (t >= SS_GLTUNNEL && t <= SS_GLLAVA)
    // return;`). History: GLTUNNEL was measured to crash COMPOSIT (page
    // fault in gl_M4_Mul, zmath.c:65) from a GL_LINE_LOOP vertex-cache
    // overflow; fixed in f5ee702 (GL_LINE_STRIP + a defensive clamp in
    // vertex.c's glopVertex()). GLKALEIDO/GLPLATONIC separately rendered
    // BLACK from the same GL_LINE_LOOP-is-a-no-op issue; fixed in 69072be.
    // A later re-verification session (2026-07-21, see CHANGELOG) found a
    // THIRD, different-looking defect after those two fixes landed: all ten
    // effects rendered as a full-screen soft rainbow gradient instead of
    // their geometry, gate was put back on (this block) pending root-cause.
    //
    // #571 (this session): reproduced the original gradient screenshots
    // first (confirmed the historical report was real), then bisected on a
    // throwaway TESTHOOK build (compositor.h gate bypassed there only,
    // never in this tree): disabling all glVertex/glBegin/glEnd emission in
    // gldemo.c's draw_hypercube() left the correct solid glClearColor with
    // no gradient, proving the fault was in primitive rendering, not
    // glClear/ZB_open/the framebuffer copy. Progressively restoring real
    // geometry (fixed color -> per-line alternating hard colors -> the real
    // 256-entry hue LUT with no time offset -> the byte-for-byte unmodified
    // shipped file) rendered CORRECTLY at every step, including the last
    // (stock, unmodified) one. MEASURED: all ten effects (GLTUNNEL,
    // GLKALEIDO, GLPLATONIC, GLLORENZ, GLMOBIUS, GLWAVEMESH, GLSPIROGRAPH,
    // GLHYPERCUBE, GLVORTEX, GLLAVA) were boot-tested via testhook.c's
    // `SAVER <id>` on VM <vmid> against this exact commit, two screendumps
    // (~4s apart, differing md5 proving live animation) per effect, all
    // rendering their intended geometry with no gradient, no crash, no
    // hang, and returning cleanly to the desktop on dismiss. INFERRED: the
    // gradient bug itself was most likely fixed incidentally by 69072be or
    // 93af69e (shared TinyGL bounds-check work) between the 07-21 session
    // and now; the exact commit was not bisected since current HEAD is
    // what matters for shipping. The gate was leftover from before that
    // fix, reintroduced by ff3ca5f's merge of an older base. See blame.md.
    if ((screensaver_type_t)t == g_ss_type) return;
    // Free the TinyGL context when switching to a non-GL effect.
    if (!ss_is_gl_type(t))
        ss_gl_teardown();
    g_ss_type = (screensaver_type_t)t;
    screensaver_init();   // re-seed state for the new effect
}
