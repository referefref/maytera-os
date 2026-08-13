// gldemo.c - shared TinyGL demo render cores: spinning textured cube + 3D
// "matrix code rain". See gldemo.h. (#319)
#include "../include/gldemo.h"
#include "../include/GL/gl.h"
#include "../include/zbuffer.h"
#include <math.h>
#include "font8x8_basic.h"

// #560 audit: must match (or exceed) the compositor's own MAX_SCREEN_W/H
// (userland/apps/compositor/compositor.h). The previous 1600x1000 ceiling
// was BELOW that bound, so any real display wider/taller than 1600x1000
// (up to the compositor's supported 1920x1080) silently got its GL
// screensaver rendered into the top-left corner only, leaving the rest of
// the screen stale/black instead of filling it.
#define MAXW 1920
#define MAXH 1080

static ZBuffer *g_zb = 0;
static int g_mode = GLDEMO_CUBE;
static int g_w = 0, g_h = 0;
static int g_inited = 0;
static float g_ang = 0.0f;
static unsigned int g_seed = 2463534242u;

static unsigned int rnd(void) {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return g_seed;
}
static float rndf(void) { return (float)(rnd() & 0xFFFFFF) / (float)0x1000000; }

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ---------------------------------------------------------------------------
// #560: shared rainbow palette, precomputed once (not per-pixel/per-frame) so
// every new effect colors vertices from a 256-entry LUT lookup rather than
// running the HSV->RGB conversion live (see task #560 constraints).
// ---------------------------------------------------------------------------
static float g_hue_r[256], g_hue_g[256], g_hue_b[256];
static int   g_hue_built = 0;
static void build_hue_lut(void) {
    if (g_hue_built) return;
    for (int h = 0; h < 256; h++) {
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
        g_hue_r[h] = (float)r / 255.0f;
        g_hue_g[h] = (float)g / 255.0f;
        g_hue_b[h] = (float)b / 255.0f;
    }
    g_hue_built = 1;
}
#define HUE_R(h) g_hue_r[(h) & 0xFF]
#define HUE_G(h) g_hue_g[(h) & 0xFF]
#define HUE_B(h) g_hue_b[(h) & 0xFF]

// ---------------------------------------------------------------------------
// #560: low-poly Platonic solids, shared by the Platonic-morph effect and
// (icosahedron only) the Lava blob effect. Each solid is a vertex list plus a
// FACE list; wireframe/fill rendering walks the faces directly so no separate
// deduplicated edge list is needed.
// ---------------------------------------------------------------------------
typedef struct { float x, y, z; } gl_v3;

static const gl_v3 TETRA_V[4] = { {1,1,1}, {1,-1,-1}, {-1,1,-1}, {-1,-1,1} };
static const int   TETRA_F[4][3] = { {0,1,2}, {0,1,3}, {0,2,3}, {1,2,3} };

static const gl_v3 CUBE2_V[8] = {
    {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
    {-1,-1, 1}, {1,-1, 1}, {1,1, 1}, {-1,1, 1},
};
static const int   CUBE2_F[6][4] = {
    {0,1,2,3}, {4,5,6,7}, {0,1,5,4}, {3,2,6,7}, {0,3,7,4}, {1,2,6,5},
};

static const gl_v3 OCTA_V[6] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
static const int   OCTA_F[8][3] = {
    {0,2,4}, {0,4,3}, {0,3,5}, {0,5,2}, {1,4,2}, {1,3,4}, {1,5,3}, {1,2,5},
};

#define PHI_CONST 1.618034f
static const gl_v3 ICO_V[12] = {
    {-1, PHI_CONST, 0}, {1, PHI_CONST, 0}, {-1,-PHI_CONST, 0}, {1,-PHI_CONST, 0},
    {0,-1, PHI_CONST}, {0, 1, PHI_CONST}, {0,-1,-PHI_CONST}, {0, 1,-PHI_CONST},
    {PHI_CONST, 0,-1}, {PHI_CONST, 0, 1}, {-PHI_CONST, 0,-1}, {-PHI_CONST, 0, 1},
};
static const int ICO_F[20][3] = {
    {0,11,5}, {0,5,1}, {0,1,7}, {0,7,10}, {0,10,11},
    {1,5,9},  {5,11,4},{11,10,2},{10,7,6},{7,1,8},
    {3,9,4},  {3,4,2}, {3,2,6}, {3,6,8}, {3,8,9},
    {4,9,5},  {2,4,11},{6,2,10},{8,6,7}, {9,8,1},
};

// #570: GL_LINE_LOOP is compiled entirely out of vertex.c's glopVertex()
// switch in this build (TGL_FEATURE_GL_POLYGON=0, zfeatures.h) - it falls
// into `default: break;` and draws NOTHING, regardless of vertex count. The
// #560 session ruled out an overflow here (3/4 verts fit inside the 4-slot
// c->vertex[] cache "by construction"), which is true, but missed that the
// primitive itself is a silent no-op in this config: draw_platonic() never
// crashed, it just never drew a single line, so Settings "Test" showed a
// black screen. Fixed the same way as draw_tunnel() (#560/f5ee702):
// GL_LINE_STRIP (bounded correctly regardless of vertex count) with the
// first vertex repeated at the end to close the loop.
static void draw_solid_wire3(const gl_v3 *v, const int f[][3], int nf, float scale, int hueBase) {
    for (int i = 0; i < nf; i++) {
        int hue = (hueBase + i * 23) & 0xFF;
        glColor3f(HUE_R(hue), HUE_G(hue), HUE_B(hue));
        glBegin(GL_LINE_STRIP);
        for (int k = 0; k <= 3; k++) {
            gl_v3 p = v[f[i][k % 3]];
            glVertex3f(p.x * scale, p.y * scale, p.z * scale);
        }
        glEnd();
    }
}
static void draw_solid_wire4(const gl_v3 *v, const int f[][4], int nf, float scale, int hueBase) {
    for (int i = 0; i < nf; i++) {
        int hue = (hueBase + i * 23) & 0xFF;
        glColor3f(HUE_R(hue), HUE_G(hue), HUE_B(hue));
        glBegin(GL_LINE_STRIP);
        for (int k = 0; k <= 4; k++) {
            gl_v3 p = v[f[i][k % 4]];
            glVertex3f(p.x * scale, p.y * scale, p.z * scale);
        }
        glEnd();
    }
}

// ---------------------------------------------------------------------------
// Cube texture (colored checkerboard)
// ---------------------------------------------------------------------------
static unsigned char g_cubetex[256*256*3];
static void build_cube_texture(void) {
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            int c = ((x >> 5) ^ (y >> 5)) & 1;
            int r, g, b;
            if (c) { r = 235; g = 200; b = 70; } else { r = 30; g = 90; b = 175; }
            int grad = (x + y) >> 3;
            r = r - 16 + (grad & 31); if (r < 0) r = 0; if (r > 255) r = 255;
            g = g - 16 + (grad & 31); if (g < 0) g = 0; if (g > 255) g = 255;
            int i = (y * 256 + x) * 3;
            g_cubetex[i] = r; g_cubetex[i+1] = g; g_cubetex[i+2] = b;
        }
}

// ---------------------------------------------------------------------------
// Matrix glyph atlas: 256x256, 8x8 grid of 32x32 cells (64 glyphs). White on
// black, scaled 8x8 font glyphs. Tinted green at draw time, blended additively.
// ---------------------------------------------------------------------------
static unsigned char g_glyphtex[256*256*3];
static const char *GLYPHSET =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:.=*+-<>|/\\!?#$%&@^~()[]{}";
static void build_glyph_atlas(void) {
    for (int i = 0; i < 256*256*3; i++) g_glyphtex[i] = 0;
    int n = 0; while (GLYPHSET[n]) n++;
    for (int cell = 0; cell < 64; cell++) {
        int cc = cell & 7, cr = cell >> 3;
        unsigned char ch = (cell < n) ? (unsigned char)GLYPHSET[cell] : 32;
        GLbyte *gl = font8x8_basic[ch];
        // place an 8x8 glyph scaled x3 (24x24) centered in the 32x32 cell
        int ox = cc * 32 + 4, oy = cr * 32 + 4;
        for (int gy = 0; gy < 8; gy++) {
            int bits = (unsigned char)gl[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (bits & (1 << gx)) {
                    for (int sy = 0; sy < 3; sy++)
                        for (int sx = 0; sx < 3; sx++) {
                            int px = ox + gx*3 + sx;
                            int py = oy + gy*3 + sy;
                            int idx = (py * 256 + px) * 3;
                            g_glyphtex[idx] = 255;
                            g_glyphtex[idx+1] = 255;
                            g_glyphtex[idx+2] = 255;
                        }
                }
            }
        }
    }
}

// matrix stream state
#define MAXSTREAMS 120
#define MAXTRAIL   16
typedef struct { float x, y, z, speed; int len; unsigned char cell[MAXTRAIL]; } stream_t;
static stream_t g_streams[MAXSTREAMS];
static int g_nstreams = 0;

static void respawn_stream(stream_t *s, int top) {
    s->z = -3.5f - rndf() * 16.0f;       // depth
    float spread = -s->z * 1.3f;
    s->x = (rndf() * 2.0f - 1.0f) * spread;
    float ytop = -s->z * 0.95f;
    s->y = top ? (ytop + rndf() * ytop) : ((rndf() * 2.0f - 1.0f) * ytop);
    s->speed = 0.04f + rndf() * 0.10f + (-s->z) * 0.004f;
    s->len = 6 + (int)(rndf() * (MAXTRAIL - 6));
    for (int k = 0; k < MAXTRAIL; k++) s->cell[k] = (unsigned char)(rnd() % 64);
}
static void init_streams(void) {
    g_nstreams = MAXSTREAMS;
    for (int i = 0; i < g_nstreams; i++) respawn_stream(&g_streams[i], 0);
}

// ===========================================================================
// #560: ten psychedelic/geometric screensaver cores. Each keeps its own
// bounded static state (this .c is built -mcmodel=large, see libgl/Makefile,
// so large .bss arrays here are safe against the user.ld disp32 pitfall).
// g_ang (declared above) is the shared "time" driver, reset to 0 by
// gldemo_init and wrapped so it never grows unbounded across an
// hours-long unattended run.
// ===========================================================================

// --- 1. Rainbow Tunnel: flight through a colour-cycling ring tunnel --------
#define TUN_RINGS 16
#define TUN_SEG   16
static float g_tun_scroll = 0.0f;
static void draw_tunnel(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    const float spacing = 1.2f;
    g_tun_scroll += 0.12f;
    g_tun_scroll = (float)((double)g_tun_scroll - (double)((int)(g_tun_scroll / spacing)) * spacing); // wrap (#560: bound the accumulator)
    for (int i = 0; i < TUN_RINGS; i++) {
        float z = -((float)i * spacing) + g_tun_scroll - 1.5f;
        float radius = 1.6f + 0.35f * sinf(z * 0.6f - g_ang * 0.03f);
        int hue = (int)(g_ang * 1.3f + i * 14) & 0xFF;
        glColor3f(HUE_R(hue), HUE_G(hue), HUE_B(hue));
        // #560 ROOT CAUSE FIX: this used to be glBegin(GL_LINE_LOOP) with
        // TUN_SEG=16 vertices. TinyGL is built with TGL_FEATURE_GL_POLYGON=0
        // (zfeatures.h), which shrinks the per-context vertex cache
        // (GLContext.vertex[], zgl.h) to POLYGON_MAX_VERTEX=4 AND compiles
        // GL_LINE_LOOP out of vertex.c's glopVertex() switch entirely - with
        // the feature off, GL_LINE_LOOP falls through to the `default: break;`
        // case, which does NOT reset/bound the vertex index the way every
        // other primitive type does. So each of the 16 glVertex3f() calls
        // walked one slot further past c->vertex[3], overflowing the fixed
        // 4-slot array into whatever GLContext fields sit right after it
        // (matrix_model_view_inv, matrix_model_projection, ..., and then the
        // matrix_stack_ptr[3] pointers) - stomping matrix_stack_ptr[1] with
        // stray 1.0f vertex data. THAT stray pointer is what gl_M4_Mul()
        // (zmath.c:65) faulted on: CR2=0x3f800000 is exactly the IEEE-754 bit
        // pattern of 1.0f, i.e. the corrupted pointer's own bytes, not a
        // write of 1.0f through a good pointer. Measured/proven with a clean
        // TESTHOOK rebuild (see CHANGELOG); the fix is to never call
        // GL_LINE_LOOP with more than POLYGON_MAX_VERTEX vertices in this
        // build. GL_LINE_STRIP IS bounded correctly regardless of vertex
        // count (vertex.c's GL_LINE_STRIP case rotates through only
        // c->vertex[0..2]), so draw a strip and manually repeat the first
        // vertex at the end to close the ring.
        glBegin(GL_LINE_STRIP);
        for (int s = 0; s <= TUN_SEG; s++) {
            float a = (float)(s % TUN_SEG) / (float)TUN_SEG * 2.0f * M_PI;
            glVertex3f(cosf(a) * radius, sinf(a) * radius, z);
        }
        glEnd();
    }
    glBegin(GL_LINES);   // radial spokes: turns the rings into a tube
    for (int s = 0; s < TUN_SEG; s += 2) {
        float a = (float)s / (float)TUN_SEG * 2.0f * M_PI;
        for (int i = 0; i < TUN_RINGS - 1; i++) {
            float z0 = -((float)i * spacing) + g_tun_scroll - 1.5f;
            float z1 = -((float)(i + 1) * spacing) + g_tun_scroll - 1.5f;
            float r0 = 1.6f + 0.35f * sinf(z0 * 0.6f - g_ang * 0.03f);
            float r1 = 1.6f + 0.35f * sinf(z1 * 0.6f - g_ang * 0.03f);
            int hue = (int)(g_ang * 1.3f + i * 14) & 0xFF;
            glColor3f(HUE_R(hue), HUE_G(hue), HUE_B(hue));
            glVertex3f(cosf(a) * r0, sinf(a) * r0, z0);
            glVertex3f(cosf(a) * r1, sinf(a) * r1, z1);
        }
    }
    glEnd();
    g_ang += 1.2f; if (g_ang > 36000.0f) g_ang -= 36000.0f;
}

// --- 2. Kaleidoscope: a small wedge pattern mirrored 8-fold ---------------
#define KAL_FOLD 8
static void draw_kaleido(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -5.0f);
    glRotatef(g_ang * 0.15f, 0.0f, 0.0f, 1.0f);
    for (int slice = 0; slice < KAL_FOLD; slice++) {
        glPushMatrix();
        glRotatef(360.0f / KAL_FOLD * (float)slice, 0.0f, 0.0f, 1.0f);
        if (slice & 1) glScalef(1.0f, -1.0f, 1.0f);   // mirror alternate wedges
        for (int k = 0; k < 5; k++) {
            float t = g_ang * 0.02f + (float)k * 0.7f;
            float r0 = 0.4f + 0.15f * (float)k;
            float r1 = r0 + 0.9f + 0.3f * sinf(t);
            float a0 = 0.10f + 0.05f * sinf(t * 1.3f);
            int hue = (int)(g_ang * 0.8f + k * 35 + slice * 10) & 0xFF;
            glColor3f(HUE_R(hue), HUE_G(hue), HUE_B(hue));
            // #570: GL_LINE_LOOP is a no-op in this build (see the comment
            // above draw_solid_wire3()/draw_solid_wire4()) - was silently
            // drawing nothing, hence "Test" showing a black screen for
            // Kaleidoscope. GL_LINE_STRIP + repeat the first vertex closes
            // the same quad outline and is bounded regardless of vertex count.
            glBegin(GL_LINE_STRIP);
            glVertex3f(r0 * cosf(-a0), r0 * sinf(-a0), 0.0f);
            glVertex3f(r1 * cosf(-a0 * 0.6f), r1 * sinf(-a0 * 0.6f), 0.0f);
            glVertex3f(r1 * cosf(a0 * 0.6f), r1 * sinf(a0 * 0.6f), 0.0f);
            glVertex3f(r0 * cosf(a0), r0 * sinf(a0), 0.0f);
            glVertex3f(r0 * cosf(-a0), r0 * sinf(-a0), 0.0f);
            glEnd();
        }
        glPopMatrix();
    }
    g_ang += 1.0f; if (g_ang > 36000.0f) g_ang -= 36000.0f;
}

// --- 3. Platonic morph: tetra/cube/octa/icosa cycle via grow/shrink pulse --
static int   g_plat_shape = 0;
static float g_plat_t = 0.0f;
static void draw_platonic(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -6.0f);
    glRotatef(g_ang * 0.6f, 1.0f, 0.4f, 0.0f);
    glRotatef(g_ang, 0.0f, 1.0f, 0.3f);

    g_plat_t += 0.0045f;
    if (g_plat_t >= 1.0f) { g_plat_t = 0.0f; g_plat_shape = (g_plat_shape + 1) & 3; }
    float env;
    if (g_plat_t < 0.15f)       env = g_plat_t / 0.15f;
    else if (g_plat_t > 0.85f)  env = (1.0f - g_plat_t) / 0.15f;
    else                        env = 1.0f;
    int hueBase = (int)(g_ang * 1.4f) & 0xFF;
    switch (g_plat_shape) {
        case 0: draw_solid_wire3(TETRA_V, TETRA_F, 4, env * 1.0f,  hueBase); break;
        case 1: draw_solid_wire4(CUBE2_V, CUBE2_F, 6, env * 0.85f, hueBase); break;
        case 2: draw_solid_wire3(OCTA_V,  OCTA_F,  8, env * 1.5f,  hueBase); break;
        default: draw_solid_wire3(ICO_V,  ICO_F,  20, env * 1.05f, hueBase); break;
    }
    g_ang += 1.0f; if (g_ang > 36000.0f) g_ang -= 36000.0f;
}

// --- 4. Lorenz attractor: classic strange attractor, rainbow trail --------
#define LORENZ_N 2000
static float g_lorenz_x[LORENZ_N], g_lorenz_y[LORENZ_N], g_lorenz_z[LORENZ_N];
static int   g_lorenz_count = 0, g_lorenz_head = 0;
static float g_lx = 0.1f, g_ly = 0.0f, g_lz = 0.0f;
static void reset_lorenz(void) {
    g_lx = 0.1f; g_ly = 0.0f; g_lz = 0.0f;
    g_lorenz_count = 0; g_lorenz_head = 0;
}
static void draw_lorenz(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -30.0f);
    glRotatef(g_ang * 0.5f, 0.0f, 1.0f, 0.0f);
    glRotatef(20.0f, 1.0f, 0.0f, 0.0f);
    glScalef(0.55f, 0.55f, 0.55f);

    const float dt = 0.006f, sigma = 10.0f, rho = 28.0f, beta = 8.0f / 3.0f;
    for (int s = 0; s < 6; s++) {
        float dx = sigma * (g_ly - g_lx) * dt;
        float dy = (g_lx * (rho - g_lz) - g_ly) * dt;
        float dz = (g_lx * g_ly - beta * g_lz) * dt;
        g_lx += dx; g_ly += dy; g_lz += dz;
        g_lorenz_x[g_lorenz_head] = g_lx;
        g_lorenz_y[g_lorenz_head] = g_ly;
        g_lorenz_z[g_lorenz_head] = g_lz - 25.0f;   // recenter (Lorenz z is ~0..50)
        g_lorenz_head = (g_lorenz_head + 1) % LORENZ_N;
        if (g_lorenz_count < LORENZ_N) g_lorenz_count++;
    }
    int n = g_lorenz_count;
    int start = (g_lorenz_head - n + LORENZ_N) % LORENZ_N;
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % LORENZ_N;
        int hue = (i * 255 / (n > 1 ? n - 1 : 1) + (int)g_ang) & 0xFF;
        glColor3f(HUE_R(hue) * 0.8f, HUE_G(hue) * 0.8f, HUE_B(hue) * 0.8f);
        glVertex3f(g_lorenz_x[idx], g_lorenz_y[idx], g_lorenz_z[idx]);
    }
    glEnd();
    g_ang += 0.6f; if (g_ang > 36000.0f) g_ang -= 36000.0f;
}

// --- 5. Mobius strip: twisting band, hue gradient along its length --------
#define MOB_USEG 60
#define MOB_VSEG 5
static void mobius_point(float u, float v, float *x, float *y, float *z) {
    const float R = 1.6f;
    float cu2 = cosf(u * 0.5f), su2 = sinf(u * 0.5f);
    float r = R + v * cu2;
    *x = r * cosf(u);
    *y = r * sinf(u);
    *z = v * su2;
}
static void draw_mobius(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -6.5f);
    glRotatef(g_ang * 0.5f, 1.0f, 0.3f, 0.0f);
    glRotatef(g_ang, 0.0f, 1.0f, 0.0f);
    const float halfw = 0.55f;
    for (int vi = 0; vi < MOB_VSEG; vi++) {
        float v = -halfw + (2.0f * halfw) * (float)vi / (float)(MOB_VSEG - 1);
        glBegin(GL_LINE_STRIP);
        for (int ui = 0; ui <= MOB_USEG; ui++) {
            float u = 2.0f * M_PI * (float)ui / (float)MOB_USEG;
            float x, y, z; mobius_point(u, v, &x, &y, &z);
            int hue = ((int)(u * 40.7f) + (int)g_ang + vi * 15) & 0xFF;
            glColor3f(HUE_R(hue), HUE_G(hue), HUE_B(hue));
            glVertex3f(x, y, z);
        }
        glEnd();
    }
    glBegin(GL_LINES);   // a few cross-rungs so the twist reads clearly
    for (int ui = 0; ui < MOB_USEG; ui += 5) {
        float u = 2.0f * M_PI * (float)ui / (float)MOB_USEG;
        int hue = ((int)(u * 40.7f) + (int)g_ang) & 0xFF;
        glColor3f(HUE_R(hue), HUE_G(hue), HUE_B(hue));
        float x0, y0, z0, x1, y1, z1;
        mobius_point(u, -halfw, &x0, &y0, &z0);
        mobius_point(u,  halfw, &x1, &y1, &z1);
        glVertex3f(x0, y0, z0); glVertex3f(x1, y1, z1);
    }
    glEnd();
    g_ang += 0.8f; if (g_ang > 36000.0f) g_ang -= 36000.0f;
}

// --- 6. Wave mesh: grid deformed by interfering sine waves, hue by height -
#define WM_N 28
static float g_wm_h[WM_N][WM_N];
static void draw_wavemesh(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.3f, -7.5f);
    glRotatef(35.0f, 1.0f, 0.0f, 0.0f);
    glRotatef(g_ang * 0.3f, 0.0f, 1.0f, 0.0f);
    float t = g_ang * 0.05f;
    const float extent = 3.0f;
    for (int j = 0; j < WM_N; j++) {
        float gy = -extent + 2.0f * extent * (float)j / (float)(WM_N - 1);
        for (int i = 0; i < WM_N; i++) {
            float gx = -extent + 2.0f * extent * (float)i / (float)(WM_N - 1);
            g_wm_h[j][i] = 0.5f * sinf(gx * 1.3f + t) + 0.5f * sinf(gy * 1.7f - t * 1.3f)
                         + 0.3f * sinf((gx + gy) * 0.9f + t * 0.7f);
        }
    }
    for (int j = 0; j < WM_N; j++) {
        glBegin(GL_LINE_STRIP);
        for (int i = 0; i < WM_N; i++) {
            float gx = -extent + 2.0f * extent * (float)i / (float)(WM_N - 1);
            float gy = -extent + 2.0f * extent * (float)j / (float)(WM_N - 1);
            int hue = (int)((g_wm_h[j][i] + 1.3f) * 90.0f + g_ang) & 0xFF;
            glColor3f(HUE_R(hue), HUE_G(hue), HUE_B(hue));
            glVertex3f(gx, g_wm_h[j][i], gy);
        }
        glEnd();
    }
    for (int i = 0; i < WM_N; i++) {
        glBegin(GL_LINE_STRIP);
        for (int j = 0; j < WM_N; j++) {
            float gx = -extent + 2.0f * extent * (float)i / (float)(WM_N - 1);
            float gy = -extent + 2.0f * extent * (float)j / (float)(WM_N - 1);
            int hue = (int)((g_wm_h[j][i] + 1.3f) * 90.0f + g_ang) & 0xFF;
            glColor3f(HUE_R(hue), HUE_G(hue), HUE_B(hue));
            glVertex3f(gx, g_wm_h[j][i], gy);
        }
        glEnd();
    }
    g_ang += 1.0f; if (g_ang > 36000.0f) g_ang -= 36000.0f;
}

// --- 7. Spirograph 3D: epitrochoid sweep with a z wobble, rainbow trail ---
#define SPIRO_N 2000
static float g_spiro_x[SPIRO_N], g_spiro_y[SPIRO_N], g_spiro_z[SPIRO_N];
static int   g_spiro_count = 0, g_spiro_head = 0;
static float g_spiro_t = 0.0f;
static void reset_spiro(void) { g_spiro_count = 0; g_spiro_head = 0; g_spiro_t = 0.0f; }
static void draw_spirograph(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -9.0f);
    glRotatef(g_ang * 0.4f, 0.0f, 1.0f, 0.0f);
    glRotatef(20.0f, 1.0f, 0.0f, 0.0f);
    const float R = 5.0f;
    float r = 1.3f + 0.6f * sinf(g_spiro_t * 0.017f);
    float d = 2.2f + 0.9f * sinf(g_spiro_t * 0.011f);
    for (int s = 0; s < 4; s++) {
        g_spiro_t += 0.04f;
        float k = (R - r) / r;
        float x = (R - r) * cosf(g_spiro_t) + d * cosf(k * g_spiro_t);
        float y = (R - r) * sinf(g_spiro_t) - d * sinf(k * g_spiro_t);
        float z = 1.4f * sinf(g_spiro_t * 0.37f);
        g_spiro_x[g_spiro_head] = x * 0.42f;
        g_spiro_y[g_spiro_head] = y * 0.42f;
        g_spiro_z[g_spiro_head] = z;
        g_spiro_head = (g_spiro_head + 1) % SPIRO_N;
        if (g_spiro_count < SPIRO_N) g_spiro_count++;
    }
    int n = g_spiro_count;
    int start = (g_spiro_head - n + SPIRO_N) % SPIRO_N;
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < n; i++) {
        int idx = (start + i) % SPIRO_N;
        int hue = (i * 255 / (n > 1 ? n - 1 : 1) + (int)g_ang) & 0xFF;
        glColor3f(HUE_R(hue) * 0.8f, HUE_G(hue) * 0.8f, HUE_B(hue) * 0.8f);
        glVertex3f(g_spiro_x[idx], g_spiro_y[idx], g_spiro_z[idx]);
    }
    glEnd();
    g_ang += 0.7f; if (g_ang > 36000.0f) g_ang -= 36000.0f;
}

// --- 8. Hypercube: rotating tesseract, 4D->3D perspective then 3D->2D -----
typedef struct { float x, y, z, w; } gl_v4;
static gl_v4 g_hc_v[16];
static int   g_hc_built = 0;
static void build_hypercube(void) {
    if (g_hc_built) return;
    for (int i = 0; i < 16; i++) {
        g_hc_v[i].x = (i & 1) ? 1.0f : -1.0f;
        g_hc_v[i].y = (i & 2) ? 1.0f : -1.0f;
        g_hc_v[i].z = (i & 4) ? 1.0f : -1.0f;
        g_hc_v[i].w = (i & 8) ? 1.0f : -1.0f;
    }
    g_hc_built = 1;
}
static void draw_hypercube(void) {
    build_hypercube();
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -6.0f);
    glScalef(1.7f, 1.7f, 1.7f);
    float a1 = g_ang * 0.021f, a2 = g_ang * 0.013f, a3 = g_ang * 0.009f;
    float ca1 = cosf(a1), sa1 = sinf(a1), ca2 = cosf(a2), sa2 = sinf(a2), ca3 = cosf(a3), sa3 = sinf(a3);
    float px[16], py[16], pz[16];
    for (int i = 0; i < 16; i++) {
        float x = g_hc_v[i].x, y = g_hc_v[i].y, z = g_hc_v[i].z, w = g_hc_v[i].w;
        float x1 = x * ca1 - w * sa1;
        float w1 = x * sa1 + w * ca1;
        float y1 = y * ca2 - z * sa2;
        float z1 = y * sa2 + z * ca2;
        float z2 = z1 * ca3 - w1 * sa3;
        float w2 = z1 * sa3 + w1 * ca3;
        const float distw = 2.6f;
        float wfac = distw / (distw - w2);
        px[i] = x1 * wfac; py[i] = y1 * wfac; pz[i] = z2 * wfac;
    }
    glBegin(GL_LINES);
    for (int i = 0; i < 16; i++) {
        for (int b = 0; b < 4; b++) {
            int j = i ^ (1 << b);
            if (j > i) {
                int hue = (i * 16 + j + (int)g_ang) & 0xFF;
                glColor3f(HUE_R(hue), HUE_G(hue), HUE_B(hue));
                glVertex3f(px[i], py[i], pz[i]);
                glVertex3f(px[j], py[j], pz[j]);
            }
        }
    }
    glEnd();
    g_ang += 1.0f; if (g_ang > 36000.0f) g_ang -= 36000.0f;
}

// --- 9. Particle vortex: swirling point sprites, colour by speed ----------
#define VTX_N 500
typedef struct { float r, a, y, va, vr, hue0; } vtx_particle_t;
static vtx_particle_t g_vtx[VTX_N];
static void reset_vortex(void) {
    for (int i = 0; i < VTX_N; i++) {
        g_vtx[i].r    = 0.3f + rndf() * 2.6f;
        g_vtx[i].a    = rndf() * 2.0f * M_PI;
        g_vtx[i].y    = (rndf() * 2.0f - 1.0f) * 2.2f;
        g_vtx[i].va   = 0.02f + rndf() * 0.05f;
        g_vtx[i].vr   = (rndf() * 2.0f - 1.0f) * 0.004f;
        g_vtx[i].hue0 = rndf() * 255.0f;
    }
}
static void draw_vortex(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -8.0f);
    glRotatef(20.0f, 1.0f, 0.0f, 0.0f);
    glRotatef(g_ang * 0.3f, 0.0f, 1.0f, 0.0f);
    glPointSize(3.0f);
    glBegin(GL_POINTS);
    for (int i = 0; i < VTX_N; i++) {
        vtx_particle_t *p = &g_vtx[i];
        float speed = p->va / (0.4f + p->r * 0.5f);   // faster spin near the core
        p->a += speed;
        p->r += p->vr;
        if (p->r < 0.2f || p->r > 3.1f) {
            p->r  = 0.3f + rndf() * 0.6f;
            p->a  = rndf() * 2.0f * M_PI;
            p->y  = (rndf() * 2.0f - 1.0f) * 2.2f;
            p->vr = (rndf() * 2.0f - 1.0f) * 0.004f;
        }
        float x = p->r * cosf(p->a), z = p->r * sinf(p->a);
        int hue = ((int)(p->hue0 + speed * 900.0f) + (int)g_ang) & 0xFF;
        glColor3f(HUE_R(hue), HUE_G(hue), HUE_B(hue));
        glVertex3f(x, p->y, z);
    }
    glEnd();
    g_ang += 1.0f; if (g_ang > 36000.0f) g_ang -= 36000.0f;
}

// --- 10. Lava blobs: drifting low-poly icospheres, additive glow ----------
// NOTE (#560): this is a deliberate simplification, not true metaballs. A
// real isosurface (marching cubes over a scalar field) is a per-voxel cost
// this software rasterizer cannot hold a frame rate on; low-poly translucent
// spheres blended additively give the same "blobby lava lamp" read at a
// vertex cost of a few hundred per frame instead of a volumetric one.
#define LAVA_N 7
typedef struct { float x, y, z, vx, vy, vz, scale, hue; } lava_blob_t;
static lava_blob_t g_lava[LAVA_N];
static void reset_lava(void) {
    for (int i = 0; i < LAVA_N; i++) {
        g_lava[i].x = (rndf() * 2.0f - 1.0f) * 1.6f;
        g_lava[i].y = (rndf() * 2.0f - 1.0f) * 1.1f;
        g_lava[i].z = (rndf() * 2.0f - 1.0f) * 1.0f;
        g_lava[i].vx = (rndf() * 2.0f - 1.0f) * 0.006f;
        g_lava[i].vy = (rndf() * 2.0f - 1.0f) * 0.006f;
        g_lava[i].vz = (rndf() * 2.0f - 1.0f) * 0.006f;
        g_lava[i].scale = 0.55f + rndf() * 0.5f;
        g_lava[i].hue = rndf() * 255.0f;
    }
}
static void draw_lava(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -6.5f);
    glRotatef(g_ang * 0.2f, 0.0f, 1.0f, 0.0f);
    for (int b = 0; b < LAVA_N; b++) {
        lava_blob_t *p = &g_lava[b];
        p->x += p->vx; p->y += p->vy; p->z += p->vz;
        if (p->x < -1.8f || p->x > 1.8f) p->vx = -p->vx;
        if (p->y < -1.3f || p->y > 1.3f) p->vy = -p->vy;
        if (p->z < -1.2f || p->z > 1.2f) p->vz = -p->vz;
        glPushMatrix();
        glTranslatef(p->x, p->y, p->z);
        float wobble = 1.0f + 0.08f * sinf(g_ang * 0.05f + (float)b * 1.7f);
        glScalef(p->scale * wobble, p->scale * wobble, p->scale * wobble);
        int hue = ((int)p->hue + (int)(g_ang * 0.6f)) & 0xFF;
        glColor3f(HUE_R(hue) * 0.5f, HUE_G(hue) * 0.5f, HUE_B(hue) * 0.5f);
        glBegin(GL_TRIANGLES);
        for (int f = 0; f < 20; f++) {
            for (int k = 0; k < 3; k++) {
                gl_v3 v = ICO_V[ICO_F[f][k]];
                const float len = 1.0f / 1.902f;   // ~normalize icosahedron radius to 1
                glVertex3f(v.x * len, v.y * len, v.z * len);
            }
        }
        glEnd();
        glPopMatrix();
    }
    g_ang += 1.0f; if (g_ang > 36000.0f) g_ang -= 36000.0f;
}

// ---------------------------------------------------------------------------
// GL context setup
// ---------------------------------------------------------------------------
static void setup_projection(void) {
    glViewport(0, 0, g_w, g_h);
    double aspect = (double)g_w / (double)g_h;
    double nh = 0.55;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-nh * aspect, nh * aspect, -nh, nh, 1.5, 60.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

int gldemo_init(int mode, int w, int h) {
    if (w < 64) w = 64; if (h < 64) h = 64;
    if (w > MAXW) w = MAXW; if (h > MAXH) h = MAXH;
    if (g_inited) gldemo_shutdown();
    g_mode = mode; g_w = w; g_h = h;
    g_zb = ZB_open(w, h, ZB_MODE_RGBA, 0);
    if (!g_zb) return 0;
    glInit(g_zb);
    g_inited = 1;
    glShadeModel(GL_SMOOTH);
    build_hue_lut();   // #560: shared palette for the new non-textured cores

    if (mode == GLDEMO_CUBE || mode == GLDEMO_MATRIX) {
        GLuint tid;
        glEnable(GL_TEXTURE_2D);
        glGenTextures(1, &tid);
        glBindTexture(GL_TEXTURE_2D, tid);
        if (mode == GLDEMO_CUBE) {
            build_cube_texture();
            glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, g_cubetex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glClearColor(0.04f, 0.04f, 0.09f, 0.0f);
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
        } else {
            build_glyph_atlas();
            glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, g_glyphtex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glClearColor(0.0f, 0.02f, 0.0f, 0.0f);
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glBlendEquation(GL_FUNC_ADD);
            init_streams();
        }
    } else {
        // #560: the ten new cores are all untextured (flat/wireframe), so
        // no texture unit is bound for them at all.
        glDisable(GL_TEXTURE_2D);
        switch (mode) {
            case GLDEMO_TUNNEL:
            case GLDEMO_KALEIDO:
            case GLDEMO_PLATONIC:
            case GLDEMO_MOBIUS:
            case GLDEMO_WAVEMESH:
            case GLDEMO_HYPERCUBE:
                // Solid/wireframe geometry: depth test on, no blending.
                glClearColor(0.02f, 0.01f, 0.06f, 0.0f);
                glEnable(GL_DEPTH_TEST);
                glDisable(GL_BLEND);
                break;
            default:
                // GLDEMO_LORENZ / SPIROGRAPH / VORTEX / LAVA: glow trails on
                // black, additive blend (same proven blend mode as GLDEMO_MATRIX).
                glClearColor(0.0f, 0.0f, 0.02f, 0.0f);
                glDisable(GL_DEPTH_TEST);
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE);
                glBlendEquation(GL_FUNC_ADD);
                break;
        }
        switch (mode) {
            case GLDEMO_TUNNEL:     g_tun_scroll = 0.0f; break;
            case GLDEMO_PLATONIC:   g_plat_shape = 0; g_plat_t = 0.0f; break;
            case GLDEMO_LORENZ:     reset_lorenz(); break;
            case GLDEMO_SPIROGRAPH: reset_spiro(); break;
            case GLDEMO_VORTEX:     reset_vortex(); break;
            case GLDEMO_LAVA:       reset_lava(); break;
            default: break;
        }
    }
    setup_projection();
    g_ang = 0.0f;
    return 1;
}

void gldemo_resize(int w, int h) {
    int m = g_mode;
    gldemo_init(m, w, h);
}

void gldemo_shutdown(void) {
    if (!g_inited) return;
    glClose();
    ZB_close(g_zb);
    g_zb = 0;
    g_inited = 0;
}

int gldemo_width(void)  { return g_w; }
int gldemo_height(void) { return g_h; }

// ---------------------------------------------------------------------------
// Cube frame
// ---------------------------------------------------------------------------
static void face(float sh,
                 float ax,float ay,float az, float bx,float by,float bz,
                 float cx,float cy,float cz, float dx,float dy,float dz) {
    glColor3f(sh, sh, sh);
    glBegin(GL_QUADS);
    glTexCoord2f(0,0); glVertex3f(ax,ay,az);
    glTexCoord2f(1,0); glVertex3f(bx,by,bz);
    glTexCoord2f(1,1); glVertex3f(cx,cy,cz);
    glTexCoord2f(0,1); glVertex3f(dx,dy,dz);
    glEnd();
}
static void draw_cube(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -6.0f);
    glRotatef(g_ang * 0.7f, 1.0f, 0.0f, 0.0f);
    glRotatef(g_ang,        0.0f, 1.0f, 0.0f);
    glRotatef(g_ang * 0.3f, 0.0f, 0.0f, 1.0f);
    float s = 1.3f;
    face(1.00f,-s,-s, s,  s,-s, s,  s, s, s, -s, s, s);
    face(0.55f, s,-s,-s, -s,-s,-s, -s, s,-s,  s, s,-s);
    face(0.80f, s,-s, s,  s,-s,-s,  s, s,-s,  s, s, s);
    face(0.65f,-s,-s,-s, -s,-s, s, -s, s, s, -s, s,-s);
    face(0.90f,-s, s, s,  s, s, s,  s, s,-s, -s, s,-s);
    face(0.45f,-s,-s,-s,  s,-s,-s,  s,-s, s, -s,-s, s);
    g_ang += 1.6f;
    if (g_ang > 36000.0f) g_ang -= 36000.0f;
}

// ---------------------------------------------------------------------------
// Matrix frame
// ---------------------------------------------------------------------------
static void glyph_quad(float x, float y, float z, float sz, unsigned char cell,
                       float r, float g, float b) {
    float u0 = (float)(cell & 7) / 8.0f;
    float v0 = (float)(cell >> 3) / 8.0f;
    float u1 = u0 + 1.0f/8.0f;
    float v1 = v0 + 1.0f/8.0f;
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v1); glVertex3f(x - sz, y - sz, z);
    glTexCoord2f(u1, v1); glVertex3f(x + sz, y - sz, z);
    glTexCoord2f(u1, v0); glVertex3f(x + sz, y + sz, z);
    glTexCoord2f(u0, v0); glVertex3f(x - sz, y + sz, z);
    glEnd();
}
static void draw_matrix(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    float gap = 0.62f;     // vertical spacing between glyphs (world units)
    float sz  = 0.30f;     // glyph half-size
    for (int i = 0; i < g_nstreams; i++) {
        stream_t *s = &g_streams[i];
        s->y -= s->speed;
        float ybot = s->z * 0.95f; // negative; bottom limit
        if (s->y < ybot) respawn_stream(s, 1);
        // occasionally mutate a glyph for shimmer
        if ((rnd() & 31) == 0) s->cell[rnd() % s->len] = (unsigned char)(rnd() % 64);
        for (int k = 0; k < s->len; k++) {
            float gy = s->y + (float)k * gap;   // trail extends upward
            float t = 1.0f - (float)k / (float)s->len;  // 1 at head .. ->0 tail
            float r, g, b;
            if (k == 0) { r = 0.75f; g = 1.0f; b = 0.80f; }   // bright head
            else { r = 0.0f; g = 0.30f + 0.70f * t; b = 0.10f * t; }
            glyph_quad(s->x, gy, s->z, sz, s->cell[k], r, g, b);
        }
    }
}

// ---------------------------------------------------------------------------
void gldemo_frame(uint32_t *dst, int dst_pitch) {
    if (!g_inited) return;
    switch (g_mode) {
        case GLDEMO_CUBE:       draw_cube();        break;
        case GLDEMO_MATRIX:     draw_matrix();      break;
        case GLDEMO_TUNNEL:     draw_tunnel();      break;
        case GLDEMO_KALEIDO:    draw_kaleido();     break;
        case GLDEMO_PLATONIC:   draw_platonic();    break;
        case GLDEMO_LORENZ:     draw_lorenz();      break;
        case GLDEMO_MOBIUS:     draw_mobius();      break;
        case GLDEMO_WAVEMESH:   draw_wavemesh();    break;
        case GLDEMO_SPIROGRAPH: draw_spirograph();  break;
        case GLDEMO_HYPERCUBE:  draw_hypercube();   break;
        case GLDEMO_VORTEX:     draw_vortex();      break;
        case GLDEMO_LAVA:       draw_lava();        break;
        default:                draw_cube();        break;
    }
    // #560 audit fix: copy row-by-row instead of ZB_copyFrameBuffer(), which
    // does memcpy(dst_row, src_row, linesize) using the CALLER-supplied
    // linesize for BOTH the destination row stride and the number of bytes
    // read from the source row. That is only safe when the destination
    // pitch exactly equals the ZBuffer's own row width. It does not on real
    // hardware: a UEFI GOP framebuffer commonly pads PixelsPerScanLine
    // beyond the visible width, and ZB_open() itself rounds its internal
    // xsize down to a multiple of 4. Either mismatch made the old call read
    // past the end of g_zb->pbuf on the last row (heap over-read) and shear
    // every row before it. Copy only the columns both buffers actually
    // have, walking each buffer with its own real stride.
    int copyw = (g_zb->xsize < g_w) ? g_zb->xsize : g_w;
    for (int y = 0; y < g_h; y++) {
        const uint32_t *srow = (const uint32_t *)g_zb->pbuf + (long)y * g_zb->xsize;
        uint32_t *drow = dst + (long)y * dst_pitch;
        for (int x = 0; x < copyw; x++) drow[x] = srow[x] | 0xFF000000u;
    }
}
