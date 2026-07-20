/* render.c - Maytera Arena: the full 3D renderer (r_*), drawn with the userland
 * TinyGL software rasterizer into an offscreen ZBuffer, then copied out to the
 * caller's ARGB blit buffer (0x00RRGGBB, which is exactly TinyGL's 32-bit
 * pixel format, so rows copy straight across).
 *
 * Scene, per frame:
 *   1. camera at local player's eye (pos + 40 up), z-up world, 90 deg FOV
 *   2. sky: fullscreen vertical gradient (level sky colors), pitch-shifted
 *   3. level brushes: axis-aligned boxes, 6 faces, directional shading +
 *      per-vertex distance fog; floors get a checker tiling for motion cues
 *   4. entities: enemies as shaded blocky humanoids (colored per player slot),
 *      items as spinning bobbing octahedra with additive halos, projectiles
 *      as glowing cores with additive billboards
 *   5. effects: additive particles (world pool), beam tracers (own pool),
 *      expanding explosion shells (own pool)
 *   6. first-person viewmodel per weapon + muzzle flash, drawn over a cleared
 *      depth buffer so it never clips into walls
 *
 * Notes on shared state:
 *   - render.c OWNS the tracer + explosion pools (static, here).
 *   - render.c also integrates and expires world->particles each frame (the
 *     contract says the renderer manages effect timing; physics only steps
 *     entities). r_spawn_particles() fills inactive slots in g_world.particles.
 *   - No busy waits, no blocking: everything here is pure computation.
 *
 * Freestanding: math from mathx.h (mx_*), memcpy/memset from the shared libc
 * via game.h. No system headers.
 */
#include "game.h"
#include <GL/gl.h>
#include <zbuffer.h>
#include "polish.h"
#include "world_art.h"
#include "bsp_load.h"    /* #491 Stage 1: BSP face draw path */

/* r_polish.c (graphics polish pass): init + camera accessor. Every polished
 * draw path below is guarded: if g_polish_ready is 0 or an asset getter
 * returns NULL, the original boxy/flat drawing runs instead, so a missing
 * asset can never blank the screen. */
void pol_init(void);

/* --------------------------------------------------------------- constants */
#define R_DT_MS        16          /* nominal frame delta for effect timing   */
#define R_EYE_HEIGHT   40.0f
#define R_NEAR         4.0f
#define R_FAR          4000.0f
#define RAD2DEG(r)     ((r) * 57.29577951f)
#define TWO_PI_F       6.28318530f

#define R_MAX_TRACERS  48
#define R_MAX_EXPL     16
#define EX_RINGS       4
#define EX_SEGS        8

typedef struct { vec3 a, b; float cr, cg, cb; int life, max_life; int active; } RTracer;
typedef struct { vec3 pos; float radius; int t, dur; int active; } RExpl;

/* ------------------------------------------------------------------ state */
static ZBuffer *g_zb = 0;
static int g_rw = 0, g_h = 0, g_reqw = 0;   /* g_rw = width rounded down to x4 */

static RTracer g_tracers[R_MAX_TRACERS];
static RExpl   g_expl[R_MAX_EXPL];

/* per-frame camera (world space) */
static vec3  g_eye, g_fwd, g_camr, g_camu;
static float g_fog_start, g_fog_end, g_fog_inv;
static float g_fogr, g_fogg, g_fogb;

/* bounded phase accumulators (kept small so mx_sinf range reduction is cheap) */
static float g_ph_spin  = 0.0f;   /* item rotation                            */
static float g_ph_bob   = 0.0f;   /* viewmodel walk bob                       */
static float g_ph_idle  = 0.0f;   /* generic idle wobble                      */
static float g_ph_death = 0.0f;   /* death-cam orbit                          */
static float g_ph_blade = 0.0f;   /* gauntlet blade spin (degrees)            */

/* unit sphere shell for explosions, built once in r_init */
static vec3 g_sph[EX_RINGS + 1][EX_SEGS + 1];
static int  g_sph_built = 0;

/* polish-pass animation state: a bounded sprite clock plus a tiny per-entity
 * damage tracker (pain flashes) so billboard characters can pick a state    */
static int g_anim_ms = 0;                     /* wraps at 1 hour, see below   */
static int g_prev_health[MAX_ENTITIES];
static int g_pain_ms[MAX_ENTITIES];

/* ------------------------------------------------------------- tiny utils */
static unsigned g_rng = 0x9E3779B9u;
static unsigned rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static float frand(void)  { return (float)(rnd() & 0xFFFF) * (1.0f / 65536.0f); }
static float frand2(void) { return frand() * 2.0f - 1.0f; }

static float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

static void u32_to_rgbf(uint32_t c, float *r, float *g, float *b) {
    *r = (float)((c >> 16) & 0xFF) * (1.0f / 255.0f);
    *g = (float)((c >>  8) & 0xFF) * (1.0f / 255.0f);
    *b = (float)( c        & 0xFF) * (1.0f / 255.0f);
}

/* levels store sky colors as vec3; accept either 0..1 floats or 0..255 */
static void skyvec_to_rgbf(vec3 c, float *r, float *g, float *b) {
    float s = (c.x > 1.5f || c.y > 1.5f || c.z > 1.5f) ? (1.0f / 255.0f) : 1.0f;
    *r = clamp01(c.x * s); *g = clamp01(c.y * s); *b = clamp01(c.z * s);
}

static void wrap_phase(float *p, float period) {
    while (*p >= period) *p -= period;
    while (*p < 0.0f)    *p += period;
}

/* fog amount 0 (near, clear) .. 1 (fully fogged) at world point p */
static float fog_at(vec3 p) {
    return clamp01((v3dist(p, g_eye) - g_fog_start) * g_fog_inv);
}

/* set a fogged smooth-shaded color then emit the vertex */
static void fog_vertex(vec3 p, float r, float g, float b) {
    float f = fog_at(p);
    glColor3f(r + (g_fogr - r) * f, g + (g_fogg - g) * f, b + (g_fogb - b) * f);
    glVertex3f(p.x, p.y, p.z);
}

/* Horizontal FOV in degrees (default 90). r_set_fov() lets the Settings menu
 * change it live; proj_setup() rebuilds the frustum from it.                  */
static int g_fov_deg = 90;
void r_set_fov(int fov_deg) {
    if (fov_deg < 60)  fov_deg = 60;
    if (fov_deg > 120) fov_deg = 120;
    g_fov_deg = fov_deg;
}

/* ------------------------------------------------------------- GL bring-up */
static void proj_setup(void) {
    float aspect = (g_h > 0) ? (float)g_rw / (float)g_h : 1.0f;
    /* half-width at the near plane = R_NEAR * tan(fov/2). 90deg -> tan45 = 1. */
    double hw = (double)R_NEAR * (double)mx_tanf((float)g_fov_deg * 0.5f
                                                 * 3.14159265f / 180.0f);
    double hh = hw / (double)aspect;
    glViewport(0, 0, g_rw, g_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-hw, hw, -hh, hh, (double)R_NEAR, (double)R_FAR);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void build_sphere(void) {
    if (g_sph_built) return;
    for (int i = 0; i <= EX_RINGS; i++) {
        float lat = -M_PI_F * 0.5f + M_PI_F * (float)i / (float)EX_RINGS;
        float cl = mx_cosf(lat), sl = mx_sinf(lat);
        for (int j = 0; j <= EX_SEGS; j++) {
            float lon = TWO_PI_F * (float)j / (float)EX_SEGS;
            g_sph[i][j] = v3(cl * mx_cosf(lon), cl * mx_sinf(lon), sl);
        }
    }
    g_sph_built = 1;
}

void r_init(int w, int h) {
    if (w < 16) w = 16;
    if (h < 16) h = 16;
    g_reqw = w;
    g_rw   = w & ~3;          /* ZB_open floors xsize to a multiple of 4      */
    if (g_rw < 16) g_rw = 16;
    g_h    = h;

    g_zb = ZB_open(g_rw, g_h, ZB_MODE_RGBA, 0);
    if (!g_zb) return;
    glInit(g_zb);
    glShadeModel(GL_SMOOTH);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.05f, 0.05f, 0.08f, 0.0f);
    glClearDepth(1.0);
    proj_setup();
    build_sphere();
    pol_init();                /* enable the polish art paths (r_polish.c)   */
}

/* Shared camera basis for r_polish.c billboards: single source of truth,
 * refreshed by camera_setup() every frame. */
void r_camera_basis(vec3 *eye, vec3 *right, vec3 *up, vec3 *fwd) {
    if (eye)   *eye   = g_eye;
    if (right) *right = g_camr;
    if (up)    *up    = g_camu;
    if (fwd)   *fwd   = g_fwd;
}

void r_resize(int w, int h) {
    if (g_zb && (w & ~3) == g_rw && h == g_h) { g_reqw = w; return; }
    if (g_zb) { glClose(); ZB_close(g_zb); g_zb = 0; }
    r_init(w, h);
}

void r_shutdown(void) {
    if (!g_zb) return;
    glClose();
    ZB_close(g_zb);
    g_zb = 0;
}

/* ------------------------------------------------------------ effect API -- */
void r_add_tracer(vec3 a, vec3 b, uint32_t rgb) {
    int best = 0, best_life = 0x7FFFFFFF;
    for (int i = 0; i < R_MAX_TRACERS; i++) {
        if (!g_tracers[i].active) { best = i; best_life = -1; break; }
        if (g_tracers[i].life < best_life) { best_life = g_tracers[i].life; best = i; }
    }
    RTracer *t = &g_tracers[best];
    t->a = a; t->b = b;
    u32_to_rgbf(rgb, &t->cr, &t->cg, &t->cb);
    t->life = t->max_life = 280;
    t->active = 1;
}

void r_spawn_particles(vec3 pos, uint32_t rgb, int count, float speed) {
    if (count <= 0) return;
    if (count > 64) count = 64;
    int spawned = 0;
    for (int i = 0; i < MAX_PARTICLES && spawned < count; i++) {
        Particle *p = &g_world.particles[i];
        if (p->active && p->life_ms > 0) continue;
        vec3 d = v3(frand2(), frand2(), frand2() * 0.9f + 0.35f);
        d = v3norm(d);
        p->pos = pos;
        p->vel = v3scale(d, speed * (0.35f + 0.65f * frand()));
        p->rgb = rgb;
        p->max_life = 350 + (int)(frand() * 450.0f);
        p->life_ms  = p->max_life;
        p->size = 2.0f + frand() * 2.5f;
        p->active = 1;
        spawned++;
    }
}

void r_explosion(vec3 pos, float radius) {
    int best = 0, best_t = -1;
    for (int i = 0; i < R_MAX_EXPL; i++) {
        if (!g_expl[i].active) { best = i; best_t = -2; break; }
        if (g_expl[i].t > best_t) { best_t = g_expl[i].t; best = i; }
    }
    RExpl *e = &g_expl[best];
    e->pos = pos;
    e->radius = (radius > 8.0f) ? radius : 8.0f;
    e->t = 0;
    e->dur = 420;
    e->active = 1;
    /* debris sparks fly out of the blast */
    r_spawn_particles(pos, 0xFFB040u, 18, radius * 2.2f);
    r_spawn_particles(pos, 0xFF5010u, 10, radius * 1.2f);
}

/* advance all effect timers by one nominal frame */
static void fx_update(World *w) {
    for (int i = 0; i < R_MAX_TRACERS; i++)
        if (g_tracers[i].active && (g_tracers[i].life -= R_DT_MS) <= 0)
            g_tracers[i].active = 0;
    for (int i = 0; i < R_MAX_EXPL; i++)
        if (g_expl[i].active && (g_expl[i].t += R_DT_MS) >= g_expl[i].dur)
            g_expl[i].active = 0;
    /* world particle pool: integrate + expire (renderer owns effect timing) */
    const float dt = (float)R_DT_MS * 0.001f;
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &w->particles[i];
        if (!p->active) continue;
        p->life_ms -= R_DT_MS;
        if (p->life_ms <= 0) { p->active = 0; continue; }
        p->pos = v3add(p->pos, v3scale(p->vel, dt));
        p->vel.z -= 380.0f * dt;      /* light gravity pull                  */
        p->vel = v3scale(p->vel, 0.985f);
    }
}

/* ------------------------------------------------------------------ camera */
static void camera_setup(World *w, float *out_yaw, float *out_pitch) {
    float yaw = 0.0f, pitch = 0.0f;
    Entity *pe = 0;
    if (w->local_player >= 0 && w->local_player < MAX_PLAYERS) {
        int ei = w->players[w->local_player].entity;
        if (ei >= 0 && ei < MAX_ENTITIES && w->ents[ei].alive) pe = &w->ents[ei];
    }
    if (pe && pe->health > 0) {
        g_eye = v3add(pe->pos, v3(0, 0, R_EYE_HEIGHT));
        yaw = pe->yaw; pitch = pe->pitch;
    } else {
        /* death / no-player cam: slow orbit around the arena center */
        vec3 c = v3scale(v3add(w->level.world_mins, w->level.world_maxs), 0.5f);
        float diag = v3dist(w->level.world_mins, w->level.world_maxs);
        if (diag < 100.0f) diag = 1200.0f;
        float dist = diag * 0.35f + 150.0f;
        yaw = g_ph_death + M_PI_F;        /* look back toward the center      */
        pitch = -0.35f;
        vec3 back = v3fromangles(g_ph_death, 0.30f);
        g_eye = v3add(c, v3scale(back, dist));
    }
    g_fwd  = v3fromangles(yaw, pitch);
    g_camr = v3norm(v3cross(g_fwd, v3(0, 0, 1)));
    if (v3len(g_camr) < 0.001f) g_camr = v3(0, -1, 0);
    g_camu = v3cross(g_camr, g_fwd);
    *out_yaw = yaw; *out_pitch = pitch;

    /* fog range scales with the arena size; fog color is the low sky color */
    float diag = v3dist(w->level.world_mins, w->level.world_maxs);
    if (diag < 100.0f) diag = 2500.0f;
    g_fog_end = diag * 1.15f;
    if (g_fog_end < 1200.0f) g_fog_end = 1200.0f;
    if (g_fog_end > 3900.0f) g_fog_end = 3900.0f;
    g_fog_start = g_fog_end * 0.35f;
    g_fog_inv = 1.0f / (g_fog_end - g_fog_start);
    skyvec_to_rgbf(w->level.sky_rgb_bot, &g_fogr, &g_fogg, &g_fogb);
    if (g_polish_ready) {
        /* world_art grading: fog colour matched to the staged sky panorama */
        unsigned int fc = world_fog_rgb(w->level_index);
        if (fc) u32_to_rgbf(fc, &g_fogr, &g_fogg, &g_fogb);
    }

    /* world -> GL view matrix. GL cameras look down -Z with Y up; our world is
     * z-up with yaw about Z. Classic Quake axis conversion, then view angles. */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glRotatef(-90.0f, 1, 0, 0);           /* put world Z up in view space     */
    glRotatef( 90.0f, 0, 0, 1);           /* make world +X the view forward   */
    glRotatef(RAD2DEG(pitch),  0, 1, 0);
    glRotatef(-RAD2DEG(yaw),   0, 0, 1);
    glTranslatef(-g_eye.x, -g_eye.y, -g_eye.z);
}

/* --------------------------------------------------------------------- sky */
static void draw_sky(World *w, float pitch) {
    float tr, tg, tb, br, bg, bb;
    skyvec_to_rgbf(w->level.sky_rgb_top, &tr, &tg, &tb);
    skyvec_to_rgbf(w->level.sky_rgb_bot, &br, &bg, &bb);

    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glDisable(GL_DEPTH_TEST);

    /* horizon line slides with pitch so the sky feels attached to the world */
    float hor = -pitch * 1.5f;
    if (hor >  0.95f) hor =  0.95f;
    if (hor < -0.95f) hor = -0.95f;

    glBegin(GL_QUADS);
    /* upper band: deep sky down to the horizon color */
    glColor3f(tr, tg, tb); glVertex3f(-1,  1, 0); glVertex3f( 1,  1, 0);
    glColor3f(br, bg, bb); glVertex3f( 1, hor, 0); glVertex3f(-1, hor, 0);
    /* lower band: horizon glow fading into darker ground haze */
    glColor3f(br, bg, bb);
    glVertex3f(-1, hor, 0); glVertex3f( 1, hor, 0);
    glColor3f(br * 0.45f, bg * 0.45f, bb * 0.45f);
    glVertex3f( 1, -1, 0); glVertex3f(-1, -1, 0);
    glEnd();

    glEnable(GL_DEPTH_TEST);
    glClear(GL_DEPTH_BUFFER_BIT);   /* fresh depth for the 3D pass            */
    proj_setup();                    /* restore the perspective projection     */
}

/* ----------------------------------------------------------- level brushes */
/* emit one fogged quad (call inside glBegin(GL_QUADS)) */
static void quad4(vec3 a, vec3 b, vec3 c, vec3 d, float r, float g, float bl) {
    fog_vertex(a, r, g, bl); fog_vertex(b, r, g, bl);
    fog_vertex(c, r, g, bl); fog_vertex(d, r, g, bl);
}

static void draw_brushes(World *w) {
    Level *lv = &w->level;
    glBegin(GL_QUADS);
    for (int i = 0; i < lv->nbrush && i < MAX_BRUSHES; i++) {
        Brush *br = &lv->brushes[i];
        vec3 mn = br->mins, mx = br->maxs;
        vec3 c  = v3scale(v3add(mn, mx), 0.5f);
        float rad = v3dist(mn, mx) * 0.5f;
        /* coarse cull: fully behind the camera */
        if (v3dot(v3sub(c, g_eye), g_fwd) < -rad) continue;

        float cr, cg, cb;
        u32_to_rgbf(br->rgb, &cr, &cg, &cb);

        /* exact per-face visibility for an AABB: a face can only be seen from
         * its own side, so each face costs nothing unless it faces the eye  */
        if (g_eye.z > mx.z) {                       /* top face (+Z)          */
            float s = 1.0f;
            if (br->is_floor) {
                /* checker tiling so player motion reads clearly              */
                float dx = mx.x - mn.x, dy = mx.y - mn.y;
                int nx = (int)(dx * (1.0f / 96.0f)) + 1; if (nx > 12) nx = 12;
                int ny = (int)(dy * (1.0f / 96.0f)) + 1; if (ny > 12) ny = 12;
                float tsx = dx / (float)nx, tsy = dy / (float)ny;
                for (int ix = 0; ix < nx; ix++)
                    for (int iy = 0; iy < ny; iy++) {
                        float x0 = mn.x + tsx * ix, x1 = x0 + tsx;
                        float y0 = mn.y + tsy * iy, y1 = y0 + tsy;
                        float sh = ((ix + iy) & 1) ? 0.84f : 1.0f;
                        quad4(v3(x0,y0,mx.z), v3(x1,y0,mx.z),
                              v3(x1,y1,mx.z), v3(x0,y1,mx.z),
                              cr*s*sh, cg*s*sh, cb*s*sh);
                    }
            } else {
                quad4(v3(mn.x,mn.y,mx.z), v3(mx.x,mn.y,mx.z),
                      v3(mx.x,mx.y,mx.z), v3(mn.x,mx.y,mx.z), cr, cg, cb);
            }
        }
        if (g_eye.z < mn.z) {                       /* bottom face (-Z)       */
            float s = 0.38f;
            quad4(v3(mn.x,mn.y,mn.z), v3(mx.x,mn.y,mn.z),
                  v3(mx.x,mx.y,mn.z), v3(mn.x,mx.y,mn.z), cr*s, cg*s, cb*s);
        }
        if (g_eye.x > mx.x) {                       /* +X side                */
            float s = 0.62f;
            quad4(v3(mx.x,mn.y,mn.z), v3(mx.x,mx.y,mn.z),
                  v3(mx.x,mx.y,mx.z), v3(mx.x,mn.y,mx.z), cr*s, cg*s, cb*s);
        }
        if (g_eye.x < mn.x) {                       /* -X side                */
            float s = 0.70f;
            quad4(v3(mn.x,mn.y,mn.z), v3(mn.x,mx.y,mn.z),
                  v3(mn.x,mx.y,mx.z), v3(mn.x,mn.y,mx.z), cr*s, cg*s, cb*s);
        }
        if (g_eye.y > mx.y) {                       /* +Y side                */
            float s = 0.84f;
            quad4(v3(mn.x,mx.y,mn.z), v3(mx.x,mx.y,mn.z),
                  v3(mx.x,mx.y,mx.z), v3(mn.x,mx.y,mx.z), cr*s, cg*s, cb*s);
        }
        if (g_eye.y < mn.y) {                       /* -Y side                */
            float s = 0.76f;
            quad4(v3(mn.x,mn.y,mn.z), v3(mx.x,mn.y,mn.z),
                  v3(mx.x,mn.y,mx.z), v3(mn.x,mn.y,mx.z), cr*s, cg*s, cb*s);
        }
    }
    glEnd();
}

/* -------------------------------------------- textured level (polish pass) */
/* textured vertex: uv + fogged shade times the theme's ambient colour grade;
 * TinyGL's LIT_TEXTURES modulates the texel by this vertex color, so
 * lighting, grading and fog keep working on textured faces                  */
static float g_amb_r = 1.0f, g_amb_g = 1.0f, g_amb_b = 1.0f;
/* Per-brush tint: modulates the shared wall/floor texture by that brush's own
 * level colour (levels.c gives every structure a distinct rgb). This gives
 * pillars/crates/tiers/catwalks visibly different materials without needing a
 * separate baked texture per structure (fixes the "too samey" walls).        */
static float g_tint_r = 1.0f, g_tint_g = 1.0f, g_tint_b = 1.0f;
static void tvert(vec3 p, float u, float v, float shade) {
    glTexCoord2f(u, v);
    fog_vertex(p, shade * g_amb_r * g_tint_r,
                  shade * g_amb_g * g_tint_g,
                  shade * g_amb_b * g_tint_b);
}
/* Blend a brush colour toward neutral grey so the texture stays legible, then
 * expose it as an RGB multiplier centred near 1.0.                            */
static void set_brush_tint(uint32_t rgb) {
    float r, g, b;
    u32_to_rgbf(rgb, &r, &g, &b);
    /* normalise brightness so dark level colours don't crush the texture, then
     * pull 55% toward white to keep it a tint not a repaint.                  */
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (mx < 0.20f) mx = 0.20f;
    float inv = 0.85f / mx;
    g_tint_r = 0.55f + 0.45f * (r * inv);
    g_tint_g = 0.55f + 0.45f * (g * inv);
    g_tint_b = 0.55f + 0.45f * (b * inv);
}

#define TEX_TILE (1.0f / 128.0f)   /* one texture repeat per 128 world units */

/* one textured axis-aligned face (own begin/end; texture already bound) */
static void tface(vec3 a, vec3 b, vec3 c, vec3 d,
                  float ua, float va, float ub, float vb,
                  float uc, float vc, float ud, float vd, float shade) {
    glBegin(GL_QUADS);
    tvert(a, ua, va, shade); tvert(b, ub, vb, shade);
    tvert(c, uc, vc, shade); tvert(d, ud, vd, shade);
    glEnd();
}

/* /ARENA art texture for a brush material (de_dust props/terrain). Cached by
 * pol_load_bmp; returns NULL if the BMP is missing (caller falls back).        */
static const Image *mat_art(int mat) {
    const char *p = 0;
    switch (mat) {
    case MAT_CRATE:  p = "/ARENA/CRATE.BMP";  break;
    case MAT_WOOD:   p = "/ARENA/WOOD.BMP";   break;
    case MAT_SANDST: p = "/ARENA/SANDST.BMP"; break;
    case MAT_WINDOW: p = "/ARENA/SANDST.BMP"; break; /* window = dark-tinted sandstone */
    case MAT_SAND:   p = "/ARENA/SAND.BMP";   break;
    default: return 0;
    }
    return pol_load_bmp(p);
}

/* Textured replacement for draw_brushes(). Returns 0 when the theme has no
 * texture set yet, in which case the caller runs the classic flat version. */
static int draw_brushes_tex(World *w) {
    if (!g_polish_ready) return 0;
    int theme = w->level_index;
    const Image *twall  = tx_wall(theme, 0);
    const Image *tfloor = tx_floor(theme, 0);
    const Image *tceil  = tx_ceil(theme, 0);
    if (!twall || !tfloor || !tceil) return 0;

    unsigned int amb = world_ambient_rgb(theme);
    if (amb) u32_to_rgbf(amb, &g_amb_r, &g_amb_g, &g_amb_b);
    else     g_amb_r = g_amb_g = g_amb_b = 1.0f;

    /* Resolve the /ARENA material textures once per frame (pol_load_bmp caches).*/
    const Image *matx[6];
    for (int m = 1; m < 6; m++) matx[m] = mat_art(m);
    matx[0] = 0;

    Level *lv = &w->level;
    for (int i = 0; i < lv->nbrush && i < MAX_BRUSHES; i++) {
        Brush *br = &lv->brushes[i];
        vec3 mn = br->mins, mx = br->maxs;
        vec3 c  = v3scale(v3add(mn, mx), 0.5f);
        float rad = v3dist(mn, mx) * 0.5f;
        if (v3dot(v3sub(c, g_eye), g_fwd) < -rad) continue;

        int id = i & 3;                       /* per-brush texture variant   */
        const Image *vwall  = tx_wall(theme, id);  if (!vwall)  vwall  = twall;
        const Image *vfloor = tx_floor(theme, id); if (!vfloor) vfloor = tfloor;
        const Image *vceil  = tx_ceil(theme, id);  if (!vceil)  vceil  = tceil;
        /* de_dust materials: crate/wood/sandstone/window/sand art overrides.   */
        if (br->mat > 0 && br->mat < 6 && matx[br->mat]) {
            vwall = vfloor = matx[br->mat];
        }
        set_brush_tint(br->rgb);              /* material variety per structure */

        if (g_eye.z > mx.z) {                 /* top face: walkable surface  */
            img_bind_gl(vfloor);
            tface(v3(mn.x,mn.y,mx.z), v3(mx.x,mn.y,mx.z),
                  v3(mx.x,mx.y,mx.z), v3(mn.x,mx.y,mx.z),
                  mn.x*TEX_TILE, mn.y*TEX_TILE, mx.x*TEX_TILE, mn.y*TEX_TILE,
                  mx.x*TEX_TILE, mx.y*TEX_TILE, mn.x*TEX_TILE, mx.y*TEX_TILE,
                  br->is_floor ? 1.0f : 0.92f);
        }
        if (g_eye.z < mn.z) {                 /* bottom face: ceiling        */
            img_bind_gl(vceil);
            tface(v3(mn.x,mn.y,mn.z), v3(mx.x,mn.y,mn.z),
                  v3(mx.x,mx.y,mn.z), v3(mn.x,mx.y,mn.z),
                  mn.x*TEX_TILE, mn.y*TEX_TILE, mx.x*TEX_TILE, mn.y*TEX_TILE,
                  mx.x*TEX_TILE, mx.y*TEX_TILE, mn.x*TEX_TILE, mx.y*TEX_TILE,
                  0.52f);
        }
        if (g_eye.x > mx.x) {                 /* +X side                     */
            img_bind_gl(vwall);
            tface(v3(mx.x,mn.y,mn.z), v3(mx.x,mx.y,mn.z),
                  v3(mx.x,mx.y,mx.z), v3(mx.x,mn.y,mx.z),
                  mn.y*TEX_TILE, -mn.z*TEX_TILE, mx.y*TEX_TILE, -mn.z*TEX_TILE,
                  mx.y*TEX_TILE, -mx.z*TEX_TILE, mn.y*TEX_TILE, -mx.z*TEX_TILE,
                  0.62f);
        }
        if (g_eye.x < mn.x) {                 /* -X side                     */
            img_bind_gl(vwall);
            tface(v3(mn.x,mn.y,mn.z), v3(mn.x,mx.y,mn.z),
                  v3(mn.x,mx.y,mx.z), v3(mn.x,mn.y,mx.z),
                  mn.y*TEX_TILE, -mn.z*TEX_TILE, mx.y*TEX_TILE, -mn.z*TEX_TILE,
                  mx.y*TEX_TILE, -mx.z*TEX_TILE, mn.y*TEX_TILE, -mx.z*TEX_TILE,
                  0.70f);
        }
        if (g_eye.y > mx.y) {                 /* +Y side                     */
            img_bind_gl(vwall);
            tface(v3(mn.x,mx.y,mn.z), v3(mx.x,mx.y,mn.z),
                  v3(mx.x,mx.y,mx.z), v3(mn.x,mx.y,mx.z),
                  mn.x*TEX_TILE, -mn.z*TEX_TILE, mx.x*TEX_TILE, -mn.z*TEX_TILE,
                  mx.x*TEX_TILE, -mx.z*TEX_TILE, mn.x*TEX_TILE, -mx.z*TEX_TILE,
                  0.84f);
        }
        if (g_eye.y < mn.y) {                 /* -Y side                     */
            img_bind_gl(vwall);
            tface(v3(mn.x,mn.y,mn.z), v3(mx.x,mn.y,mn.z),
                  v3(mx.x,mn.y,mx.z), v3(mn.x,mn.y,mx.z),
                  mn.x*TEX_TILE, -mn.z*TEX_TILE, mx.x*TEX_TILE, -mn.z*TEX_TILE,
                  mx.x*TEX_TILE, -mx.z*TEX_TILE, mn.x*TEX_TILE, -mx.z*TEX_TILE,
                  0.76f);
        }
    }
    g_tint_r = g_tint_g = g_tint_b = 1.0f;    /* reset for other passes         */
    glDisable(GL_TEXTURE_2D);
    return 1;
}

/* Skybox / sky panorama (world_art.c). Drawn right after the gradient with
 * depth writes off, so it layers over the gradient and behind everything
 * else; if no sky art exists the gradient simply stays visible.            */
static void draw_sky_art(World *w) {
    if (!g_polish_ready) return;
    int theme = w->level_index;
    const Image *f[6];
    int have_cube = 0;
    for (int i = 0; i < 6; i++) {
        f[i] = sky_face(theme, i);
        if (i > 0 && f[i]) have_cube = 1;
    }
    const Image *pano = 0;
    if (!have_cube) pano = f[0] ? f[0] : tx_sky(theme);
    if (!have_cube && !pano) return;

    glDepthMask(0);
    if (have_cube) {
        /* cube faces at +-S around the eye; corners stay inside the far
         * plane (S*sqrt(3) < R_FAR) */
        const float S = 1800.0f;
        float ex = g_eye.x, ey = g_eye.y, ez = g_eye.z;
        /* face order per polish.h: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z */
        for (int i = 0; i < 6; i++) {
            if (!f[i]) continue;
            img_bind_gl(f[i]);
            glColor3f(1, 1, 1);
            glBegin(GL_QUADS);
            switch (i) {
            case 0:  /* +X */
                glTexCoord2f(0,0); glVertex3f(ex+S, ey+S, ez+S);
                glTexCoord2f(1,0); glVertex3f(ex+S, ey-S, ez+S);
                glTexCoord2f(1,1); glVertex3f(ex+S, ey-S, ez-S);
                glTexCoord2f(0,1); glVertex3f(ex+S, ey+S, ez-S);
                break;
            case 1:  /* -X */
                glTexCoord2f(0,0); glVertex3f(ex-S, ey-S, ez+S);
                glTexCoord2f(1,0); glVertex3f(ex-S, ey+S, ez+S);
                glTexCoord2f(1,1); glVertex3f(ex-S, ey+S, ez-S);
                glTexCoord2f(0,1); glVertex3f(ex-S, ey-S, ez-S);
                break;
            case 2:  /* +Y */
                glTexCoord2f(0,0); glVertex3f(ex-S, ey+S, ez+S);
                glTexCoord2f(1,0); glVertex3f(ex+S, ey+S, ez+S);
                glTexCoord2f(1,1); glVertex3f(ex+S, ey+S, ez-S);
                glTexCoord2f(0,1); glVertex3f(ex-S, ey+S, ez-S);
                break;
            case 3:  /* -Y */
                glTexCoord2f(0,0); glVertex3f(ex+S, ey-S, ez+S);
                glTexCoord2f(1,0); glVertex3f(ex-S, ey-S, ez+S);
                glTexCoord2f(1,1); glVertex3f(ex-S, ey-S, ez-S);
                glTexCoord2f(0,1); glVertex3f(ex+S, ey-S, ez-S);
                break;
            case 4:  /* +Z (up) */
                glTexCoord2f(0,0); glVertex3f(ex-S, ey+S, ez+S);
                glTexCoord2f(1,0); glVertex3f(ex+S, ey+S, ez+S);
                glTexCoord2f(1,1); glVertex3f(ex+S, ey-S, ez+S);
                glTexCoord2f(0,1); glVertex3f(ex-S, ey-S, ez+S);
                break;
            default: /* -Z (down) */
                glTexCoord2f(0,0); glVertex3f(ex-S, ey-S, ez-S);
                glTexCoord2f(1,0); glVertex3f(ex+S, ey-S, ez-S);
                glTexCoord2f(1,1); glVertex3f(ex+S, ey+S, ez-S);
                glTexCoord2f(0,1); glVertex3f(ex-S, ey+S, ez-S);
                break;
            }
            glEnd();
        }
    } else {
        /* 360 degree panorama band on a cylinder around the eye; the
         * gradient already painted the zenith and the ground haze          */
        const float RADIUS = 1400.0f, ZTOP = 1250.0f, ZBOT = -680.0f;
        const int SEGS = 16;
        img_bind_gl(pano);
        glColor3f(1, 1, 1);
        glBegin(GL_QUADS);
        for (int s = 0; s < SEGS; s++) {
            float a0 = TWO_PI_F * (float)s / (float)SEGS;
            float a1 = TWO_PI_F * (float)(s + 1) / (float)SEGS;
            float x0 = g_eye.x + mx_cosf(a0) * RADIUS, y0 = g_eye.y + mx_sinf(a0) * RADIUS;
            float x1 = g_eye.x + mx_cosf(a1) * RADIUS, y1 = g_eye.y + mx_sinf(a1) * RADIUS;
            float u0 = (float)s / (float)SEGS, u1 = (float)(s + 1) / (float)SEGS;
            glTexCoord2f(u0, 0); glVertex3f(x0, y0, g_eye.z + ZTOP);
            glTexCoord2f(u1, 0); glVertex3f(x1, y1, g_eye.z + ZTOP);
            glTexCoord2f(u1, 1); glVertex3f(x1, y1, g_eye.z + ZBOT);
            glTexCoord2f(u0, 1); glVertex3f(x0, y0, g_eye.z + ZBOT);
        }
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);
    glDepthMask(1);
}

/* --------------------------------------------------------------- 3D shapes */
/* axis-aligned box in the CURRENT modelview space, flat directional shading,
 * colors premixed with a constant fog amount (fog computed once per model)  */
static void draw_box(float x0, float y0, float z0, float x1, float y1, float z1,
                     float cr, float cg, float cb, float fog) {
    static const float sh[6] = { 1.0f, 0.40f, 0.62f, 0.70f, 0.84f, 0.76f };
    float fr[6], fg[6], fb[6];
    for (int i = 0; i < 6; i++) {
        float r = cr * sh[i], g = cg * sh[i], b = cb * sh[i];
        fr[i] = r + (g_fogr - r) * fog;
        fg[i] = g + (g_fogg - g) * fog;
        fb[i] = b + (g_fogb - b) * fog;
    }
    glBegin(GL_QUADS);
    glColor3f(fr[0], fg[0], fb[0]);   /* +Z */
    glVertex3f(x0,y0,z1); glVertex3f(x1,y0,z1); glVertex3f(x1,y1,z1); glVertex3f(x0,y1,z1);
    glColor3f(fr[1], fg[1], fb[1]);   /* -Z */
    glVertex3f(x0,y0,z0); glVertex3f(x1,y0,z0); glVertex3f(x1,y1,z0); glVertex3f(x0,y1,z0);
    glColor3f(fr[2], fg[2], fb[2]);   /* +X */
    glVertex3f(x1,y0,z0); glVertex3f(x1,y1,z0); glVertex3f(x1,y1,z1); glVertex3f(x1,y0,z1);
    glColor3f(fr[3], fg[3], fb[3]);   /* -X */
    glVertex3f(x0,y0,z0); glVertex3f(x0,y1,z0); glVertex3f(x0,y1,z1); glVertex3f(x0,y0,z1);
    glColor3f(fr[4], fg[4], fb[4]);   /* +Y */
    glVertex3f(x0,y1,z0); glVertex3f(x1,y1,z0); glVertex3f(x1,y1,z1); glVertex3f(x0,y1,z1);
    glColor3f(fr[5], fg[5], fb[5]);   /* -Y */
    glVertex3f(x0,y0,z0); glVertex3f(x1,y0,z0); glVertex3f(x1,y0,z1); glVertex3f(x0,y0,z1);
    glEnd();
}

/* spinning octahedron in world space (items, projectile cores) */
static void draw_octa(vec3 c, float rad, float ang,
                      float cr, float cg, float cb, float fog) {
    float ca = mx_cosf(ang) * rad, sa = mx_sinf(ang) * rad;
    vec3 ring[4] = {
        v3(c.x + ca, c.y + sa, c.z), v3(c.x - sa, c.y + ca, c.z),
        v3(c.x - ca, c.y - sa, c.z), v3(c.x + sa, c.y - ca, c.z)
    };
    vec3 top = v3(c.x, c.y, c.z + rad * 1.35f);
    vec3 bot = v3(c.x, c.y, c.z - rad * 1.35f);
    static const float sht[4] = { 1.00f, 0.82f, 0.92f, 0.74f };
    static const float shb[4] = { 0.62f, 0.48f, 0.56f, 0.42f };
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        float s = sht[i];
        float r = cr*s, g = cg*s, b = cb*s;
        glColor3f(r + (g_fogr-r)*fog, g + (g_fogg-g)*fog, b + (g_fogb-b)*fog);
        glVertex3f(top.x,top.y,top.z);
        glVertex3f(ring[i].x,ring[i].y,ring[i].z);
        glVertex3f(ring[j].x,ring[j].y,ring[j].z);
        s = shb[i];
        r = cr*s; g = cg*s; b = cb*s;
        glColor3f(r + (g_fogr-r)*fog, g + (g_fogg-g)*fog, b + (g_fogb-b)*fog);
        glVertex3f(bot.x,bot.y,bot.z);
        glVertex3f(ring[j].x,ring[j].y,ring[j].z);
        glVertex3f(ring[i].x,ring[i].y,ring[i].z);
    }
    glEnd();
}

/* ---------------------------------------------------------------- entities */
static const float g_itemcol[5][3] = {
    { 0.25f, 1.00f, 0.35f },   /* IT_HEALTH  green                            */
    { 1.00f, 0.85f, 0.20f },   /* IT_ARMOR   gold                             */
    { 1.00f, 0.55f, 0.15f },   /* IT_AMMO    orange                           */
    { 0.30f, 0.75f, 1.00f },   /* IT_WEAPON  cyan (tinted by weapon below)    */
    { 1.00f, 0.30f, 0.90f },   /* IT_MEGA    magenta                          */
};

static void item_color(Entity *e, float *r, float *g, float *b) {
    int k = e->item_kind;
    if (k < 0 || k > 4) k = 0;
    if (k == IT_WEAPON && e->item_value >= 0 && e->item_value < NUM_WEAPONS) {
        u32_to_rgbf(g_weapons[e->item_value].tracer_rgb, r, g, b);
        /* keep it bright enough to read as a pickup */
        if (*r + *g + *b < 0.6f) { *r += 0.3f; *g += 0.3f; *b += 0.3f; }
        return;
    }
    *r = g_itemcol[k][0]; *g = g_itemcol[k][1]; *b = g_itemcol[k][2];
}

static void proj_color(Entity *e, float *r, float *g, float *b, float *rad) {
    switch (e->proj_kind) {
    case PROJ_ROCKET:  *r=1.00f; *g=0.55f; *b=0.20f; *rad=5.0f; break;
    case PROJ_GRENADE: *r=0.35f; *g=0.90f; *b=0.30f; *rad=4.0f; break;
    case PROJ_PLASMA:  *r=0.30f; *g=0.60f; *b=1.00f; *rad=3.5f; break;
    case PROJ_BFG:     *r=0.40f; *g=1.00f; *b=0.30f; *rad=9.0f; break;
    case PROJ_NAIL:    *r=0.80f; *g=0.80f; *b=0.90f; *rad=2.5f; break;
    default:           *r=1.00f; *g=1.00f; *b=1.00f; *rad=3.0f; break;
    }
}

/* ------------------------------------------- billboard entities (polish) -- */
/* frame-count probe with a tiny cache (0 = not probed yet)                  */
static int probe_frames(const Image *(*get)(int, int), int kind, signed char *cache) {
    if (*cache == 0) {
        int n = 0;
        while (n < 8 && get(kind, n)) n++;
        *cache = (signed char)(n > 0 ? n : 1);
    }
    return *cache;
}
static signed char g_pj_nf[8], g_fx_nf[8];

/* fog darkens billboards: sprites cannot lerp toward the fog color, so they
 * fade toward black with distance instead (reads fine in practice)         */
static unsigned int fog_tint(vec3 p) {
    float k = 1.0f - fog_at(p) * 0.78f;
    unsigned int c = (unsigned int)(k * 255.0f);
    return (c << 16) | (c << 8) | c;
}

/* Doom-style enemy billboard. Returns 0 when no sprite art exists so the
 * caller can fall back to the boxy humanoid.                               */
static int draw_enemy_sprite(World *w, Entity *e, int idx) {
    if (!g_polish_ready) return 0;

    int skin = (e->player_slot >= 0 ? e->player_slot : idx) & 7;

    /* state: 0 idle, 1 run, 2 fire, 3 pain (approx from vel + recent dmg)  */
    int state = 0;
    float speed = v3len(v3(e->vel.x, e->vel.y, 0));
    if (speed > 30.0f) state = 1;
    if (e->fire_cooldown > 0) state = 2;
    if (g_pain_ms[idx] > 0) state = 3;

    /* angle 0..7: enemy facing relative to the viewer (0 = facing you)     */
    float to_view = mx_atan2f(g_eye.y - e->pos.y, g_eye.x - e->pos.x);
    float rel = e->yaw - to_view;
    while (rel < 0.0f)       rel += TWO_PI_F;
    while (rel >= TWO_PI_F)  rel -= TWO_PI_F;
    int angle = (int)(rel * (8.0f / TWO_PI_F) + 0.5f) & 7;

    int nf = ch_state_frames(state);
    if (nf < 1) nf = 1;
    int frame = (g_anim_ms / 120 + idx * 3) % nf;

    const Image *im = ch_frame(skin, state, angle, frame);
    if (!im) im = ch_frame(skin, 0, angle, 0);
    if (!im) return 0;

    (void)w;
    vec3 c = v3add(e->pos, v3(0, 0, 29.0f));
    billboard_draw(im, c, 58.0f, fog_tint(e->pos));
    return 1;
}

static int draw_item_sprite(Entity *e, vec3 c) {
    if (!g_polish_ready) return 0;
    const Image *im = it_sprite(e->item_kind, e->item_value);
    if (!im) return 0;
    float size = (e->item_kind == IT_MEGA || e->item_kind == IT_WEAPON) ? 30.0f : 24.0f;
    billboard_draw(im, c, size, fog_tint(c));
    return 1;
}

static int draw_proj_sprite(Entity *e, float rad) {
    if (!g_polish_ready) return 0;
    int kind = (e->proj_kind >= 0 && e->proj_kind < 8) ? e->proj_kind : 0;
    int nf = probe_frames(pj_sprite, kind, &g_pj_nf[kind]);
    const Image *im = pj_sprite(kind, (g_anim_ms / 80) % nf);
    if (!im) im = pj_sprite(kind, 0);
    if (!im) return 0;
    billboard_draw(im, e->pos, rad * 4.0f, 0xFFFFFFu);
    return 1;
}

static void draw_enemy(World *w, Entity *e) {
    float br, bg, bb;
    if (e->player_slot >= 0 && e->player_slot < MAX_PLAYERS)
        u32_to_rgbf(w->players[e->player_slot].color, &br, &bg, &bb);
    else { br = bg = bb = 0.55f; }
    float fog = fog_at(e->pos);

    glPushMatrix();
    glTranslatef(e->pos.x, e->pos.y, e->pos.z);
    glRotatef(RAD2DEG(e->yaw), 0, 0, 1);

    float dr = br * 0.5f, dg = bg * 0.5f, db = bb * 0.5f;   /* darker limbs   */
    float hr = br + (1.0f - br) * 0.35f;                    /* lighter head   */
    float hg = bg + (1.0f - bg) * 0.35f;
    float hb = bb + (1.0f - bb) * 0.35f;

    /* legs (model faces +X after the yaw rotation) */
    draw_box(-3, -6, 0, 3, -1, 24, dr, dg, db, fog);
    draw_box(-3,  1, 0, 3,  6, 24, dr, dg, db, fog);
    /* torso */
    draw_box(-5, -8, 24, 5, 8, 46, br, bg, bb, fog);
    /* head */
    draw_box(-4, -4, 46, 4, 4, 56, hr, hg, hb, fog);
    /* held gun pointing forward */
    draw_box(5, -1.5f, 36, 20, 1.5f, 41, 0.22f, 0.22f, 0.25f, fog);
    glPopMatrix();
}

static void draw_entities(World *w) {
    int local_ent = -1;
    if (w->local_player >= 0 && w->local_player < MAX_PLAYERS)
        local_ent = w->players[w->local_player].entity;

    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *e = &w->ents[i];
        if (!e->alive) continue;
        /* coarse behind-camera cull for models */
        if (v3dot(v3sub(e->pos, g_eye), g_fwd) < -80.0f) continue;

        if ((e->type == ET_PLAYER || e->type == ET_BOT) &&
            i != local_ent && e->health > 0) {
            if (!draw_enemy_sprite(w, e, i))       /* sprite art, else boxy  */
                draw_enemy(w, e);
        } else if (e->type == ET_ITEM && e->respawn_ms <= 0) {
            float ph = g_ph_spin + (float)(i & 15) * 0.7f;
            float bob = mx_sinf(ph) * 4.0f;
            vec3 c = v3add(e->pos, v3(0, 0, 16.0f + bob));
            if (!draw_item_sprite(e, c)) {         /* sprite art, else octa  */
                float r, g, b;
                item_color(e, &r, &g, &b);
                float rad = (e->item_kind == IT_MEGA || e->item_kind == IT_WEAPON)
                            ? 11.0f : 8.0f;
                draw_octa(c, rad, ph * 1.7f, r, g, b, fog_at(c));
            }
        } else if (e->type == ET_PROJECTILE) {
            float r, g, b, rad;
            proj_color(e, &r, &g, &b, &rad);
            if (!draw_proj_sprite(e, rad))         /* sprite art, else octa  */
                draw_octa(e->pos, rad, g_ph_spin * 3.0f, r, g, b, 0.0f);
        } else if (e->type == ET_PROP && e->prop_kind == PROP_BARREL) {
            const Image *im = g_polish_ready ? pol_load_bmp("/ARENA/BARREL.BMP") : 0;
            if (im) {
                billboard_draw(im, v3add(e->pos, v3(0, 0, 22.0f)), 50.0f,
                               fog_tint(e->pos));
            } else {                                /* fallback: red steel drum */
                float fog = fog_at(e->pos);
                glPushMatrix();
                glTranslatef(e->pos.x, e->pos.y, e->pos.z);
                draw_box(-18,-18,0,  18,18,44, 0.66f,0.16f,0.10f, fog);
                draw_box(-19,-19,5,  19,19,11, 0.28f,0.28f,0.30f, fog);
                draw_box(-19,-19,33, 19,19,39, 0.28f,0.28f,0.30f, fog);
                glPopMatrix();
            }
        }
    }
}

/* Static world props (trees + lamps) drawn straight from the level list. Cheap:
 * one billboard (or a small box fallback) each, no per-frame allocation.       */
static void draw_level_props(World *w) {
    const Image *tree = g_polish_ready ? pol_load_bmp("/ARENA/TREE.BMP") : 0;
    const Image *glow = g_polish_ready ? decal_sprite(WA_DECAL_SPARK, 0) : 0;
    for (int i = 0; i < w->level.nprop; i++) {
        PropDef *p = &w->level.props[i];
        if (v3dot(v3sub(p->pos, g_eye), g_fwd) < -120.0f) continue;  /* behind cam */
        float fog = fog_at(p->pos);
        if (p->kind == PROP_TREE) {
            float s = p->size > 0 ? p->size : 200.0f;
            if (tree) {
                billboard_draw(tree, v3add(p->pos, v3(0, 0, s * 0.5f)), s,
                               fog_tint(p->pos));
            } else {                                /* fallback: trunk + canopy  */
                glPushMatrix(); glTranslatef(p->pos.x, p->pos.y, p->pos.z);
                draw_box(-8,-8,0, 8,8,s*0.5f, 0.35f,0.22f,0.10f, fog);
                draw_box(-s*0.22f,-s*0.22f,s*0.42f, s*0.22f,s*0.22f,s,
                         0.20f,0.45f,0.18f, fog);
                glPopMatrix();
            }
        } else if (p->kind == PROP_LAMP) {
            float s = p->size > 0 ? p->size : 120.0f;
            /* post */
            glPushMatrix(); glTranslatef(p->pos.x, p->pos.y, p->pos.z);
            draw_box(-4,-4,0, 4,4,s, 0.20f,0.18f,0.14f, fog);
            glPopMatrix();
            /* warm glow at the top (emissive billboard, or a bright box)        */
            vec3 top = v3add(p->pos, v3(0, 0, s));
            if (glow) billboard_draw(glow, top, 70.0f, p->rgb);
            else {
                glPushMatrix(); glTranslatef(top.x, top.y, top.z);
                draw_box(-8,-8,-8, 8,8,8, 1.0f,0.82f,0.55f, 0.0f);
                glPopMatrix();
            }
        }
    }
}

/* ----------------------------------------------------------------- effects */
/* camera-facing quad, call inside glBegin(GL_QUADS) */
static void bb_quad(vec3 c, float s, float r, float g, float b) {
    vec3 R = v3scale(g_camr, s), U = v3scale(g_camu, s);
    glColor3f(r, g, b);
    glVertex3f(c.x-R.x-U.x, c.y-R.y-U.y, c.z-R.z-U.z);
    glVertex3f(c.x+R.x-U.x, c.y+R.y-U.y, c.z+R.z-U.z);
    glVertex3f(c.x+R.x+U.x, c.y+R.y+U.y, c.z+R.z+U.z);
    glVertex3f(c.x-R.x+U.x, c.y-R.y+U.y, c.z-R.z+U.z);
}

/* beam as two crossed quads along the segment, call inside glBegin(GL_QUADS) */
static void beam_quads(vec3 a, vec3 b, float w, float r, float g, float bl) {
    vec3 d = v3sub(b, a);
    float len = v3len(d);
    if (len < 1.0f) return;
    d = v3scale(d, 1.0f / len);
    vec3 mid = v3scale(v3add(a, b), 0.5f);
    vec3 te  = v3norm(v3sub(g_eye, mid));
    vec3 s1  = v3cross(d, te);
    if (v3len(s1) < 0.01f) s1 = g_camu;
    s1 = v3scale(v3norm(s1), w);
    vec3 s2 = v3scale(v3norm(v3cross(d, s1)), w);
    glColor3f(r, g, bl);
    glVertex3f(a.x+s1.x, a.y+s1.y, a.z+s1.z);
    glVertex3f(b.x+s1.x, b.y+s1.y, b.z+s1.z);
    glVertex3f(b.x-s1.x, b.y-s1.y, b.z-s1.z);
    glVertex3f(a.x-s1.x, a.y-s1.y, a.z-s1.z);
    glVertex3f(a.x+s2.x, a.y+s2.y, a.z+s2.z);
    glVertex3f(b.x+s2.x, b.y+s2.y, b.z+s2.z);
    glVertex3f(b.x-s2.x, b.y-s2.y, b.z-s2.z);
    glVertex3f(a.x-s2.x, a.y-s2.y, a.z-s2.z);
}

/* is this particle a blood droplet? (weapons.c spawns 0xCC2020/0xCC3030)    */
static int particle_is_blood(uint32_t rgb) {
    unsigned r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return r >= 0x80 && r > 2 * g && r > 2 * b;
}

static void draw_effects(World *w) {
    /* sprite-art availability decides what the additive batch must skip    */
    const Image *blood0 = g_polish_ready ? fx_sprite(1, 0) : 0;
    const Image *expl0  = g_polish_ready ? fx_sprite(0, 0) : 0;

    glDepthMask(0);                 /* effects test depth but never write it  */
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);    /* additive glow                          */
    glBlendEquation(GL_FUNC_ADD);

    glBegin(GL_QUADS);

    /* particles from the shared world pool */
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &w->particles[i];
        if (!p->active || p->life_ms <= 0) continue;
        if (blood0 && particle_is_blood(p->rgb)) continue;  /* sprite pass   */
        float fr = (p->max_life > 0) ? (float)p->life_ms / (float)p->max_life : 1.0f;
        float r, g, b;
        u32_to_rgbf(p->rgb, &r, &g, &b);
        float sz = (p->size > 0.1f) ? p->size : 2.5f;
        bb_quad(p->pos, sz, r * fr, g * fr, b * fr);
    }

    /* projectile glows (cores were drawn solid in the entity pass) */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *e = &w->ents[i];
        if (!e->alive || e->type != ET_PROJECTILE) continue;
        float r, g, b, rad;
        proj_color(e, &r, &g, &b, &rad);
        bb_quad(e->pos, rad * 2.4f, r * 0.55f, g * 0.55f, b * 0.55f);
        /* rockets and BFG shots leave a faint smoke/energy trail */
        if ((e->proj_kind == PROJ_ROCKET || e->proj_kind == PROJ_BFG) && (rnd() & 1))
            r_spawn_particles(e->pos, e->proj_kind == PROJ_ROCKET ? 0x806040u
                                                                  : 0x40A030u,
                              1, 20.0f);
    }

    /* item halos (soft pulsing glow so pickups pop from a distance) */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *e = &w->ents[i];
        if (!e->alive || e->type != ET_ITEM || e->respawn_ms > 0) continue;
        float r, g, b;
        item_color(e, &r, &g, &b);
        float pulse = 0.20f + 0.10f * mx_sinf(g_ph_spin * 2.0f + (float)(i & 7));
        bb_quad(v3add(e->pos, v3(0, 0, 16)), 15.0f, r * pulse, g * pulse, b * pulse);
    }

    /* tracers: bright wide beam + hot white core */
    for (int i = 0; i < R_MAX_TRACERS; i++) {
        RTracer *t = &g_tracers[i];
        if (!t->active) continue;
        float f = (float)t->life / (float)t->max_life;
        f *= f;                                        /* quadratic fade      */
        beam_quads(t->a, t->b, 2.8f, t->cr * f, t->cg * f, t->cb * f);
        beam_quads(t->a, t->b, 0.9f, 0.9f * f, 0.9f * f, 0.9f * f);
    }

    /* explosions: expanding additive shell + hot center flash */
    for (int i = 0; i < R_MAX_EXPL; i++) {
        RExpl *ex = &g_expl[i];
        if (!ex->active) continue;
        if (expl0) continue;                      /* fireball sprite pass    */
        float fr = (float)ex->t / (float)ex->dur;      /* 0..1                */
        float R  = ex->radius * (0.25f + 0.75f * fr);
        float br = 1.0f - fr;
        float sr = 1.00f * br * 0.55f, sg = 0.45f * br * 0.55f, sb = 0.10f * br * 0.55f;
        for (int ri = 0; ri < EX_RINGS; ri++)
            for (int sj = 0; sj < EX_SEGS; sj++) {
                vec3 p0 = v3add(ex->pos, v3scale(g_sph[ri][sj],     R));
                vec3 p1 = v3add(ex->pos, v3scale(g_sph[ri][sj+1],   R));
                vec3 p2 = v3add(ex->pos, v3scale(g_sph[ri+1][sj+1], R));
                vec3 p3 = v3add(ex->pos, v3scale(g_sph[ri+1][sj],   R));
                glColor3f(sr, sg, sb);
                glVertex3f(p0.x,p0.y,p0.z); glVertex3f(p1.x,p1.y,p1.z);
                glVertex3f(p2.x,p2.y,p2.z); glVertex3f(p3.x,p3.y,p3.z);
            }
        bb_quad(ex->pos, R * 0.9f, 1.0f * br, 0.85f * br, 0.5f * br);
    }

    glEnd();

    /* fireball sprites (additive, still depth-tested but never written)    */
    if (expl0) {
        int nf = probe_frames(fx_sprite, 0, &g_fx_nf[0]);
        for (int i = 0; i < R_MAX_EXPL; i++) {
            RExpl *ex = &g_expl[i];
            if (!ex->active) continue;
            float fr = (float)ex->t / (float)ex->dur;          /* 0..1       */
            int frame = (int)(fr * (float)nf);
            if (frame >= nf) frame = nf - 1;
            const Image *im = fx_sprite(0, frame);
            if (!im) im = expl0;
            unsigned int k = (unsigned int)((1.0f - fr * 0.55f) * 255.0f);
            billboard_draw(im, ex->pos, ex->radius * (1.1f + 1.3f * fr),
                           (k << 16) | (k << 8) | k);
        }
    }

    glDisable(GL_BLEND);

    /* blood sprites (cutout, not additive; magenta key handles the shape)  */
    if (blood0) {
        int nf = probe_frames(fx_sprite, 1, &g_fx_nf[1]);
        for (int i = 0; i < MAX_PARTICLES; i++) {
            Particle *p = &w->particles[i];
            if (!p->active || p->life_ms <= 0) continue;
            if (!particle_is_blood(p->rgb)) continue;
            float age = 1.0f - ((p->max_life > 0)
                        ? (float)p->life_ms / (float)p->max_life : 0.0f);
            int frame = (int)(age * (float)nf);
            if (frame >= nf) frame = nf - 1;
            const Image *im = fx_sprite(1, frame);
            if (!im) im = blood0;
            float sz = ((p->size > 0.1f) ? p->size : 2.5f) * 3.0f;
            billboard_draw(im, p->pos, sz, fog_tint(p->pos));
        }
    }

    glDepthMask(1);
}

/* --------------------------------------------------------------- viewmodel */
typedef struct { float blen, brad; int twin; float body[3]; } VMDef;
static const VMDef g_vm[NUM_WEAPONS] = {
    /* W_GAUNTLET   */ { 0.0f, 0.00f, 0, { 0.55f, 0.50f, 0.45f } },
    /* W_MACHINEGUN */ { 7.0f, 0.45f, 0, { 0.42f, 0.42f, 0.46f } },
    /* W_SHOTGUN    */ { 6.0f, 0.55f, 1, { 0.46f, 0.32f, 0.20f } },
    /* W_GRENADE    */ { 4.0f, 0.95f, 0, { 0.26f, 0.44f, 0.26f } },
    /* W_ROCKET     */ { 8.0f, 1.30f, 0, { 0.50f, 0.23f, 0.18f } },
    /* W_LIGHTNING  */ { 7.5f, 0.60f, 0, { 0.70f, 0.72f, 0.82f } },
    /* W_RAILGUN    */ {10.0f, 0.50f, 0, { 0.20f, 0.55f, 0.55f } },
    /* W_PLASMA     */ { 5.0f, 0.90f, 0, { 0.26f, 0.36f, 0.66f } },
    /* W_BFG        */ { 6.5f, 1.80f, 0, { 0.26f, 0.55f, 0.22f } },
    /* W_NAILGUN    */ { 8.0f, 0.40f, 1, { 0.45f, 0.30f, 0.50f } },
};

static void draw_viewmodel(World *w) {
    Entity *pe = 0;
    if (w->local_player >= 0 && w->local_player < MAX_PLAYERS) {
        int ei = w->players[w->local_player].entity;
        if (ei >= 0 && ei < MAX_ENTITIES && w->ents[ei].alive) pe = &w->ents[ei];
    }
    if (!pe || pe->health <= 0) return;

    int wi = pe->weapon;
    if (wi < 0 || wi >= NUM_WEAPONS) wi = 0;
    const VMDef *vm = &g_vm[wi];
    float ar, ag, ab;
    u32_to_rgbf(g_weapons[wi].tracer_rgb, &ar, &ag, &ab);

    /* muzzle flash intensity: cooldown is close to its max right after a shot */
    float flash = 0.0f;
    int fms = g_weapons[wi].fire_ms;
    if (fms > 0 && pe->fire_cooldown > 0) {
        float cf = (float)pe->fire_cooldown / (float)fms;
        if (cf > 0.55f) flash = (cf - 0.55f) * (1.0f / 0.45f);
        if (fms <= 120) flash = 0.8f;    /* rapid weapons: near-constant glow */
    }

    /* the gun never clips into world geometry: it gets a fresh depth buffer */
    glClear(GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* walk bob + fire kick, in view space (x right, y up, gun points -z) */
    float speed = v3len(v3(pe->vel.x, pe->vel.y, 0));
    float amp = speed * (1.0f / 320.0f);
    if (amp > 1.0f) amp = 1.0f;
    float bobx = mx_sinf(g_ph_bob) * 0.45f * amp + mx_sinf(g_ph_idle) * 0.06f;
    float boby = mx_absf(mx_cosf(g_ph_bob)) * -0.35f * amp
               + mx_cosf(g_ph_idle * 0.7f) * 0.05f;
    float kick = flash * 1.6f;

    /* keep old fog color from tinting the gun: viewmodel uses fog = 0 */
    float sf = g_fogr, sg2 = g_fogg, sb2 = g_fogb;
    g_fogr = g_fogg = g_fogb = 0.0f;

    glPushMatrix();
    glTranslatef(6.5f + bobx, -5.5f + boby, -16.0f + kick);
    glRotatef(-4.0f, 0, 1, 0);       /* angle the gun slightly inward         */

    float br2 = vm->body[0], bg2 = vm->body[1], bb2 = vm->body[2];
    /* receiver/body */
    draw_box(-1.5f, -2.4f, -5.0f, 1.5f, 1.0f, 3.0f, br2, bg2, bb2, 0.0f);
    /* grip */
    draw_box(-0.9f, -4.6f, 0.4f, 0.9f, -2.2f, 2.6f,
             br2 * 0.6f, bg2 * 0.6f, bb2 * 0.6f, 0.0f);
    /* weapon-colored accent strip on top so weapons read at a glance */
    draw_box(-1.6f, 1.0f, -4.4f, 1.6f, 1.5f, 2.4f, ar, ag, ab, 0.0f);

    float tipz = -5.0f - vm->blen;
    if (wi == W_GAUNTLET) {
        /* spinning saw blade instead of a barrel */
        glPushMatrix();
        glTranslatef(0.0f, 0.2f, -6.0f);
        glRotatef(g_ph_blade, 0, 0, 1);
        draw_box(-2.6f, -0.5f, -0.35f, 2.6f, 0.5f, 0.35f, 0.75f, 0.75f, 0.78f, 0.0f);
        draw_box(-0.5f, -2.6f, -0.35f, 0.5f, 2.6f, 0.35f, 0.75f, 0.75f, 0.78f, 0.0f);
        glPopMatrix();
        tipz = -7.0f;
    } else if (vm->twin) {
        float off = vm->brad + 0.25f;
        draw_box(-off - vm->brad, -vm->brad + 0.2f, tipz,
                 -off + vm->brad,  vm->brad + 0.2f, -4.5f,
                 0.28f, 0.28f, 0.30f, 0.0f);
        draw_box( off - vm->brad, -vm->brad + 0.2f, tipz,
                  off + vm->brad,  vm->brad + 0.2f, -4.5f,
                 0.28f, 0.28f, 0.30f, 0.0f);
    } else {
        draw_box(-vm->brad, -vm->brad + 0.2f, tipz,
                  vm->brad,  vm->brad + 0.2f, -4.5f,
                 0.30f, 0.30f, 0.33f, 0.0f);
        if (wi == W_PLASMA || wi == W_BFG)   /* energy bulb behind the barrel */
            draw_box(-vm->brad - 0.5f, -vm->brad - 0.3f, -4.8f,
                      vm->brad + 0.5f,  vm->brad + 0.7f, -2.2f,
                      ar * 0.7f, ag * 0.7f, ab * 0.7f, 0.0f);
    }

    /* muzzle flash: additive star at the barrel tip */
    if (flash > 0.05f && wi != W_GAUNTLET) {
        float s = 1.4f + flash * 2.2f + frand() * 0.5f;
        float mr = 0.5f + ar * 0.5f, mg = 0.45f + ag * 0.5f, mb = 0.3f + ab * 0.5f;
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glBlendEquation(GL_FUNC_ADD);
        glDepthMask(0);
        glBegin(GL_QUADS);
        glColor3f(mr * flash, mg * flash, mb * flash);
        glVertex3f(-s, 0.2f - s, tipz - 0.5f); glVertex3f( s, 0.2f - s, tipz - 0.5f);
        glVertex3f( s, 0.2f + s, tipz - 0.5f); glVertex3f(-s, 0.2f + s, tipz - 0.5f);
        glColor3f(mr * flash * 0.7f, mg * flash * 0.7f, mb * flash * 0.7f);
        glVertex3f(-s * 1.6f, 0.2f - s * 0.4f, tipz - 0.6f);
        glVertex3f( s * 1.6f, 0.2f - s * 0.4f, tipz - 0.6f);
        glVertex3f( s * 1.6f, 0.2f + s * 0.4f, tipz - 0.6f);
        glVertex3f(-s * 1.6f, 0.2f + s * 0.4f, tipz - 0.6f);
        glEnd();
        glDepthMask(1);
        glDisable(GL_BLEND);
    }
    glPopMatrix();

    g_fogr = sf; g_fogg = sg2; g_fogb = sb2;

    /* advance viewmodel phases (bounded, see wrap below) */
    g_ph_bob += 0.011f * (float)R_DT_MS * (0.3f + amp);
    wrap_phase(&g_ph_bob, 2.0f * TWO_PI_F);
    if (wi == W_GAUNTLET) {
        g_ph_blade += (pe->fire_cooldown > 0) ? 34.0f : 7.0f;
        while (g_ph_blade >= 360.0f) g_ph_blade -= 360.0f;
    }
}

/* ------------------------------------------- 2D viewmodel (polish pass) -- */
/* Returns the local player entity if it is alive, else NULL. */
static Entity *local_alive_ent(World *w) {
    if (w->local_player < 0 || w->local_player >= MAX_PLAYERS) return 0;
    int ei = w->players[w->local_player].entity;
    if (ei < 0 || ei >= MAX_ENTITIES || !w->ents[ei].alive) return 0;
    return &w->ents[ei];
}

/* Does weapon sprite art exist for the local player's current gun?         */
static int viewmodel_sprite_ready(World *w) {
    if (!g_polish_ready) return 0;
    Entity *pe = local_alive_ent(w);
    if (!pe || pe->health <= 0) return 0;
    int wi = pe->weapon;
    if (wi < 0 || wi >= NUM_WEAPONS) wi = 0;
    return wv_sprite(wi, 0) != 0;
}

/* First-person weapon as a 2D sprite composited over the finished frame,
 * anchored bottom-center. Fire animation frames double as the muzzle flash
 * (frame 0 = idle, 1..wv_fire_frames-1 = firing).                          */
static void draw_viewmodel_sprite(World *w, uint32_t *blit, int stride) {
    Entity *pe = local_alive_ent(w);
    if (!pe || pe->health <= 0 || !blit || stride <= 0) return;
    int wi = pe->weapon;
    if (wi < 0 || wi >= NUM_WEAPONS) wi = 0;
    const Image *idle = wv_sprite(wi, 0);
    if (!idle) return;

    int nf = wv_fire_frames(wi);
    if (nf < 1) nf = 1;
    int frame = 0, fms = g_weapons[wi].fire_ms;
    if (pe->fire_cooldown > 0 && fms > 0 && nf > 1) {
        float prog = 1.0f - (float)pe->fire_cooldown / (float)fms;
        if (prog < 0.0f) prog = 0.0f;
        if (prog > 1.0f) prog = 1.0f;
        frame = 1 + (int)(prog * (float)(nf - 1));
        if (frame >= nf) frame = nf - 1;
    }
    const Image *im = wv_sprite(wi, frame);
    if (!im) im = idle;

    /* sprite height ~= 45% of the screen, integer scaled                   */
    int scale = ((g_h * 9) / 20) / (im->h > 0 ? im->h : 1);
    if (scale < 1) scale = 1;

    /* walk bob shares the phase the 3D gun used; fire adds a small kick    */
    float speed = v3len(v3(pe->vel.x, pe->vel.y, 0));
    float amp = speed * (1.0f / 320.0f);
    if (amp > 1.0f) amp = 1.0f;
    int bobx = (int)(mx_sinf(g_ph_bob) * 6.0f * amp) * scale;
    int boby = (int)(mx_absf(mx_cosf(g_ph_bob)) * 4.0f * amp) * scale;
    int kick = frame ? 3 * scale : 0;

    int x = stride / 2 - (im->w * scale) / 2 + bobx;
    int y = g_h - im->h * scale + boby + kick;
    screen_sprite(im, blit, stride, g_h, x, y, scale);

    g_ph_bob += 0.011f * (float)R_DT_MS * (0.3f + amp);
    wrap_phase(&g_ph_bob, 2.0f * TWO_PI_F);
}

/* ---------------------------------------------------------------- present */
static void present(uint32_t *blit, int stride) {
    if (!g_zb || !blit || stride <= 0) return;
    const uint32_t *src = (const uint32_t *)g_zb->pbuf;
    int copyw = (g_rw < stride) ? g_rw : stride;
    int padw  = (g_reqw < stride) ? g_reqw : stride;
    for (int y = 0; y < g_h; y++) {
        uint32_t *dst = blit + (long)y * stride;
        memcpy(dst, src + (long)y * g_rw, (unsigned long)copyw * 4);
        /* the ZBuffer width was rounded down to x4: pad the last columns */
        for (int x = copyw; x < padw; x++) dst[x] = dst[copyw - 1];
    }
}

/* ------------------------------------------------------------------ frame */
void r_frame(World *world, uint32_t *blit, int stride) {
    if (!g_zb) return;
    if (!world) {                       /* defensive: black frame              */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        present(blit, stride);
        return;
    }

    /* effect timing runs at the nominal frame rate */
    fx_update(world);
    g_ph_spin  += 0.045f; wrap_phase(&g_ph_spin,  TWO_PI_F);
    g_ph_idle  += 0.060f; wrap_phase(&g_ph_idle,  2.0f * TWO_PI_F);
    g_ph_death += 0.006f; wrap_phase(&g_ph_death, TWO_PI_F);
    g_anim_ms  += R_DT_MS;
    if (g_anim_ms >= 3600000) g_anim_ms = 0;   /* bounded sprite clock       */

    /* pain tracking for billboard characters: a health drop flags the pain
     * state for a few frames (approximation contract-blessed in polish.h)  */
    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *e = &world->ents[i];
        if (e->alive && (e->type == ET_PLAYER || e->type == ET_BOT)) {
            if (e->health < g_prev_health[i]) g_pain_ms[i] = 320;
            else if (g_pain_ms[i] > 0)        g_pain_ms[i] -= R_DT_MS;
            g_prev_health[i] = e->health;
        } else {
            g_prev_health[i] = 0;
            g_pain_ms[i] = 0;
        }
    }

    float yaw, pitch;
    camera_setup(world, &yaw, &pitch);  /* also sets the modelview matrix     */
    (void)yaw;

    /* the sky pass resets the matrices, so rebuild the camera after it */
    draw_sky(world, pitch);
    camera_setup(world, &yaw, &pitch);

    draw_sky_art(world);                /* skybox/panorama over the gradient  */
    if (bsp_is_active()) {              /* #491: imported GoldSrc BSP faces    */
        bsp_draw_faces();
    } else if (!draw_brushes_tex(world)) { /* textured level, else classic flat */
        draw_brushes(world);
    }
    draw_entities(world);
    draw_level_props(world);
    draw_effects(world);

    /* weapon in hand: 2D sprite art composited after present() when it
     * exists; otherwise the classic 3D boxy gun inside the scene            */
    int vm_sprite = viewmodel_sprite_ready(world);
    if (!vm_sprite) draw_viewmodel(world);

    present(blit, stride);
    if (vm_sprite) draw_viewmodel_sprite(world, blit, stride);
}
