// screensaver.c - Screensaver effects for MayteraOS userland compositor.
// Supports blank, starfield, bouncing lines, and expanding bubbles modes.
// No malloc. All state is static. Idle detection uses monotonic uptime_ms()
// (kernel-reported milliseconds), so it is independent of the timer rate.

#include "compositor.h"
#include "../../libc/syscall.h"
#include "gldemo.h"   // #319 TinyGL demo render cores (reconciled #336)
#include "planet_art.h"  // real CC0 planet sprites (2D Planet Pack 2, SBS)

// ============================================================================
// Static state
// ============================================================================

static screensaver_type_t g_ss_type    = SS_PLASMA;   // default Plasma (UIPROFIL screensaver:7); starfield selectable
static int                g_ss_timeout = SS_DEFAULT_TIMEOUT; // seconds
static uint32_t           g_ss_frame;
static uint32_t           g_ss_seed    = 12345;

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
        // Smooth animated plasma field (#282): summed sine waves -> hue.
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

    // -----------------------------------------------------------------------
    case SS_GLCUBE:    // #319 TinyGL spinning textured cube (reconciled #336)
        ss_gl_render(GLDEMO_CUBE);
        break;

    // -----------------------------------------------------------------------
    case SS_GLMATRIX:  // #319 TinyGL 3D matrix code rain (reconciled #336)
        ss_gl_render(GLDEMO_MATRIX);
        break;
    }

    g_ss_frame++;
}

// ============================================================================
// screensaver_on_input
// ============================================================================

void screensaver_on_input(void) {
    // Reset the idle timer to the current monotonic millisecond.
    g_idle_ms = uptime_ms();

    // Dismiss an active screensaver on any user input.
    if (g_screensaver_active) {
        g_screensaver_active = false;
        g_needs_redraw       = true;
    }
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
        return true;
    }

    return false;
}


// Change the active screensaver effect at runtime (Settings selection).
void screensaver_set_type(int t) {
    if (t < 0 || t > SS_GLMATRIX) return;   // #319 allow GL saver ids (reconciled #336)
    if ((screensaver_type_t)t == g_ss_type) return;
    // Free the TinyGL context when switching to a non-GL effect.
    if (t != SS_GLCUBE && t != SS_GLMATRIX)
        ss_gl_teardown();
    g_ss_type = (screensaver_type_t)t;
    screensaver_init();   // re-seed state for the new effect
}
