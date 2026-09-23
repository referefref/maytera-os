// gldemo_aquarium.c - hyper-real 3D reef aquarium render core (GLDEMO_AQUARIUM).
//
// A believable reef tank on TinyGL: five fish species with distinct bodies,
// colour patterns and behaviour (clownfish pairs, a blue tang, a tall striped
// angelfish, a sand-dwelling goby that darts and rests, and a boids school of
// neon cardinals with separation/alignment/cohesion), swimming with sinusoidal
// spine undulation, banking into turns and steering off the glass. Scene depth
// comes from a rippled sand heightfield with animated caustic shimmer, noise-
// deformed rocks, finger-coral clusters, swaying kelp ribbons, rising bubble
// streams and additive god-ray light shafts. Volumetric water is faked with
// per-vertex depth fog (TinyGL's glFog* are stubs, measured in stubs_ac.c, so
// every vertex is CPU-mixed toward the water colour by 1-exp(-d*dist)) plus a
// depth-graded background quad drawn in view space.
//
// TinyGL constraints honored here (measured in this tree, not assumed):
//   - Blending is ADDITIVE only (no GL_SRC_ALPHA path in zbuffer.h), so
//     bubbles, surface shimmer and god rays are additive passes over an
//     opaque scene, never alpha blending.
//   - glFog* are stubs; fog is computed per vertex on the CPU (aq_emit).
//   - GL_LINE_LOOP and GL_POLYGON are compiled out; only POINTS/LINES/
//     STRIPS/TRIANGLES/FANS/QUADS are used.
//   - glDepthMask IS real (api.c:203): additive passes keep the depth TEST
//     so bubbles hide behind rocks, but never write depth.
//   - One global context per process (gldemo.c owns init/teardown).
//
// Options (aquarium): population, species mix, water tint, light level,
// caustics, bubble density, camera mode, plant density and a quality toggle
// are all runtime-settable through gldemo_aquarium_option() (see gldemo.h);
// the aquarium app maps keyboard keys onto it, the screensaver path uses the
// defaults. Everything is procedural: no image assets, no licence questions.
//
// Performance: static scenery (sand/rocks/coral) is generated once per
// reseed into static arrays and only re-lit/fogged per frame; fish meshes
// are regenerated per frame (they deform) but are low-poly (a big fish is
// about 110 triangles, a schooling fish about 30); there are zero per-frame
// allocations, and the quality option trims ring counts, school size, rays
// and bubbles rather than the frame rate.
#include "../include/gldemo.h"
#include "../include/GL/gl.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif
#define AQ_2PI 6.2831853071795864f
#define AQ_D2R 0.01745329252f
#define AQ_R2D 57.295779513f

// ---------------------------------------------------------------------------
// Tank geometry (world units). Camera sits outside the front glass at +z.
// ---------------------------------------------------------------------------
#define TANK_X 4.0f          // half-width of swimmable volume
#define TANK_Y 2.7f          // water height above the sand plane
#define TANK_Z 1.7f          // half-depth of swimmable volume

// ---------------------------------------------------------------------------
// Options (defaults tuned to look good out of the box)
// ---------------------------------------------------------------------------
static int g_opt_pop      = 1;      // 0 sparse / 1 normal / 2 busy
static int g_opt_species  = 31;     // bitmask, bit n = species n present
static int g_opt_tint     = 0;      // 0 clear blue / 1 lagoon / 2 deep / 3 sunset
static int g_opt_light    = 1;      // 0 dim / 1 normal / 2 bright
static int g_opt_caustics = 1;      // 0/1 sand caustic shimmer
static int g_opt_bubbles  = 1;      // 0 off / 1 normal / 2 dense
static int g_opt_camera   = 0;      // 0 orbit / 1 fixed / 2 drift
static int g_opt_plants   = 1;      // 0 few / 1 normal / 2 lush
static int g_opt_quality  = 1;      // 0 performance / 1 quality
static int g_need_reseed  = 0;      // option changed something structural

// ---------------------------------------------------------------------------
// RNG (xorshift32, deterministic per reseed; same idiom as the other cores)
// ---------------------------------------------------------------------------
static unsigned int g_aqseed = 421771u;
static unsigned int aqrnd(void) {
    g_aqseed ^= g_aqseed << 13; g_aqseed ^= g_aqseed >> 17; g_aqseed ^= g_aqseed << 5;
    return g_aqseed;
}
static float aqrndf(void) { return (float)(aqrnd() & 0xFFFFFF) / (float)0x1000000; }
static float aqrndc(void) { return aqrndf() * 2.0f - 1.0f; }

// ---------------------------------------------------------------------------
// Water / lighting state, refreshed each frame from the options
// ---------------------------------------------------------------------------
static float g_t = 0.0f;             // animation clock (frames)
static int   g_aq_inited = 0;
static int   g_prj_w = 0, g_prj_h = 0;

static float g_eyex, g_eyey, g_eyez;         // camera position (for fog)
static float g_rightx, g_rightz;             // camera right vector (billboards)
static float g_fogr, g_fogg, g_fogb;         // water colour fog mixes toward
static float g_fogd;                         // fog density
static float g_light;                        // light intensity multiplier

// key light from above-front, normalized at compile time by construction
static const float LX = 0.29f, LY = 0.87f, LZ = 0.40f;

typedef struct { float cr, cg, cb, fr, fg, fb, dens; } aq_tint_t;
static const aq_tint_t AQ_TINTS[4] = {
    { 0.013f, 0.085f, 0.140f,  0.10f, 0.33f, 0.43f, 0.105f },  // clear blue
    { 0.016f, 0.095f, 0.085f,  0.11f, 0.36f, 0.29f, 0.120f },  // lagoon green
    { 0.004f, 0.030f, 0.075f,  0.03f, 0.14f, 0.25f, 0.160f },  // deep ocean
    { 0.055f, 0.040f, 0.090f,  0.27f, 0.17f, 0.25f, 0.110f },  // sunset
};
static const float AQ_LIGHTS[3] = { 0.72f, 1.0f, 1.28f };

// ---------------------------------------------------------------------------
// Vertex emit helpers: CPU lambert lighting + depth fog, then glColor/glVertex
// ---------------------------------------------------------------------------
static void aq_fog_mix(float x, float y, float z, float *r, float *g, float *b) {
    float dx = x - g_eyex, dy = y - g_eyey, dz = z - g_eyez;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz) - 3.2f;
    if (dist > 0.0f) {
        float f = 1.0f - expf(-g_fogd * dist);
        *r += (g_fogr - *r) * f;
        *g += (g_fogg - *g) * f;
        *b += (g_fogb - *b) * f;
    }
}

// lit + fogged vertex (n must be roughly unit length)
static void aq_emit(float x, float y, float z,
                    float r, float g, float b,
                    float nx, float ny, float nz) {
    float d = nx * LX + ny * LY + nz * LZ;
    if (d < 0.0f) d = 0.0f;
    float li = (0.40f + 0.66f * d) * g_light * (0.80f + 0.09f * y);
    r *= li; g *= li; b *= li;
    aq_fog_mix(x, y, z, &r, &g, &b);
    if (r > 1.0f) r = 1.0f; if (g > 1.0f) g = 1.0f; if (b > 1.0f) b = 1.0f;
    glColor3f(r, g, b);
    glVertex3f(x, y, z);
}

// unlit but fogged vertex (pre-shaded colours, e.g. sand with caustics)
static void aq_emitf(float x, float y, float z, float r, float g, float b) {
    aq_fog_mix(x, y, z, &r, &g, &b);
    if (r > 1.0f) r = 1.0f; if (g > 1.0f) g = 1.0f; if (b > 1.0f) b = 1.0f;
    glColor3f(r, g, b);
    glVertex3f(x, y, z);
}

// ---------------------------------------------------------------------------
// Sand heightfield
// ---------------------------------------------------------------------------
#define SAND_NX 23
#define SAND_NZ 13
static float g_sand_y[SAND_NZ][SAND_NX];
static float g_sand_c[SAND_NZ][SAND_NX];    // per-vertex mottle 0.85..1.1
static float g_sand_px[SAND_NX], g_sand_pz[SAND_NZ];

static float aq_sand_h(float x, float z) {
    return 0.055f * sinf(0.90f * x + 1.7f)
         + 0.050f * sinf(1.30f * z - 0.6f + 0.55f * x)
         + 0.035f * sinf(0.60f * (x + z) + 3.1f);
}

// ---------------------------------------------------------------------------
// Rocks: noise-deformed lat/long blobs, generated at reseed
// ---------------------------------------------------------------------------
#define ROCK_MAX 5
#define ROCK_LAT 6
#define ROCK_LON 11                          // last column repeats the first
typedef struct { float px, py, pz, nx, ny, nz, r, g, b; } aq_vtx_t;
static aq_vtx_t g_rock[ROCK_MAX][ROCK_LAT + 1][ROCK_LON];
static int g_nrock = 0;
static float g_rock_cx[ROCK_MAX], g_rock_cy[ROCK_MAX], g_rock_cz[ROCK_MAX], g_rock_R[ROCK_MAX];

// ---------------------------------------------------------------------------
// Coral: clusters of tapered 4-sided fingers, generated at reseed
// ---------------------------------------------------------------------------
#define CORAL_QMAX 260
static aq_vtx_t g_coral[CORAL_QMAX][4];     // quads
static int g_ncoral = 0;

// ---------------------------------------------------------------------------
// Kelp ribbons (regenerated per frame: they sway)
// ---------------------------------------------------------------------------
#define PLANT_MAX 16
#define PLANT_SEG 7
typedef struct { float x, z, h, w, ph, lean, hue; } aq_plant_t;
static aq_plant_t g_plant[PLANT_MAX];
static int g_nplant = 0;

// ---------------------------------------------------------------------------
// Fish
// ---------------------------------------------------------------------------
#define SP_CLOWN 0
#define SP_TANG  1
#define SP_ANGEL 2
#define SP_GOBY  3
#define SP_NEON  4

#define BIG_MAX 9
#define SCH_MAX 24
typedef struct {
    int   sp;
    float x, y, z;
    float heading;               // radians, world yaw
    float speed;                 // units/frame
    float vy;
    float phase;                 // tail beat phase
    float ph1, ph2;              // per-fish wander phases
    float size;                  // length in world units
    float turn;                  // smoothed turn rate (banking)
    float yband;                 // preferred cruising height
    // goby dart state
    int   state;                 // 0 rest, 1 dart
    float statet;
    float tx, tz;
} aq_fish_t;

static aq_fish_t g_big[BIG_MAX];
static int g_nbig = 0;
static aq_fish_t g_sch[SCH_MAX];
static int g_nsch = 0;
static float g_anchor_x, g_anchor_y, g_anchor_z;   // school wander anchor

// ---------------------------------------------------------------------------
// Bubbles
// ---------------------------------------------------------------------------
#define BUB_MAX 80
typedef struct { float x, y, z, vy, sz, ph; int em; } aq_bub_t;
static aq_bub_t g_bub[BUB_MAX];
static int g_nbub = 0;
#define EMIT_MAX 3
static float g_emx[EMIT_MAX], g_emy[EMIT_MAX], g_emz[EMIT_MAX];
static int g_nem = 0;

// ---------------------------------------------------------------------------
// God rays
// ---------------------------------------------------------------------------
#define RAY_MAX 5
static float g_ray_x[RAY_MAX], g_ray_ph[RAY_MAX];
static int g_nray = 0;

// ===========================================================================
// Reseed: build the whole scene from the current options
// ===========================================================================
static void aq_spawn_fish(aq_fish_t *f, int sp, float sizemul) {
    f->sp = sp;
    f->x = aqrndc() * (TANK_X - 0.8f);
    f->z = aqrndc() * (TANK_Z - 0.5f);
    f->heading = aqrndf() * AQ_2PI;
    f->vy = 0.0f;
    f->phase = aqrndf() * AQ_2PI;
    f->ph1 = aqrndf() * AQ_2PI;
    f->ph2 = aqrndf() * AQ_2PI;
    f->turn = 0.0f;
    f->state = 0; f->statet = 60.0f + aqrndf() * 120.0f;
    switch (sp) {
    case SP_CLOWN:
        f->size = (0.50f + 0.10f * aqrndf()) * sizemul;
        f->speed = 0.018f + 0.006f * aqrndf();
        f->yband = 1.2f + 0.7f * aqrndc();
        break;
    case SP_TANG:
        f->size = (0.72f + 0.10f * aqrndf()) * sizemul;
        f->speed = 0.024f + 0.007f * aqrndf();
        f->yband = 1.5f + 0.6f * aqrndc();
        break;
    case SP_ANGEL:
        f->size = (0.80f + 0.12f * aqrndf()) * sizemul;
        f->speed = 0.015f + 0.005f * aqrndf();
        f->yband = 1.6f + 0.5f * aqrndc();
        break;
    case SP_GOBY:
        f->size = (0.42f + 0.08f * aqrndf()) * sizemul;
        f->speed = 0.0f;
        f->yband = 0.1f;
        f->tx = f->x; f->tz = f->z;
        break;
    default: // SP_NEON
        f->size = (0.24f + 0.05f * aqrndf()) * sizemul;
        f->speed = 0.020f + 0.008f * aqrndf();
        f->yband = 1.6f;
        break;
    }
    f->y = (sp == SP_GOBY) ? aq_sand_h(f->x, f->z) + 0.05f
                           : 0.5f + f->yband + 0.2f * aqrndc();
    if (f->y > TANK_Y - 0.3f) f->y = TANK_Y - 0.3f;
    if (f->y < 0.3f && sp != SP_GOBY) f->y = 0.3f;
}

static void aq_reseed(void) {
    g_aqseed = 421771u + (unsigned)(g_opt_pop * 131 + g_opt_plants * 17 + g_opt_species);

    // sand
    for (int i = 0; i < SAND_NX; i++)
        g_sand_px[i] = -4.6f + 9.2f * (float)i / (float)(SAND_NX - 1);
    for (int k = 0; k < SAND_NZ; k++)
        g_sand_pz[k] = -2.1f + 4.4f * (float)k / (float)(SAND_NZ - 1);
    for (int k = 0; k < SAND_NZ; k++)
        for (int i = 0; i < SAND_NX; i++) {
            g_sand_y[k][i] = aq_sand_h(g_sand_px[i], g_sand_pz[k]);
            g_sand_c[k][i] = 0.86f + 0.22f * aqrndf();
        }

    // rocks along the back and one mid-tank
    g_nrock = (g_opt_quality ? 4 : 3);
    for (int r = 0; r < g_nrock; r++) {
        float cx = -3.0f + 2.1f * (float)r + 0.5f * aqrndc();
        float cz = -1.15f + 0.3f * aqrndc() + ((r == 2) ? 0.9f : 0.0f);
        float R = 0.45f + 0.30f * aqrndf();
        float cy = aq_sand_h(cx, cz) + R * 0.42f;
        g_rock_cx[r] = cx; g_rock_cy[r] = cy; g_rock_cz[r] = cz; g_rock_R[r] = R;
        float base_r = 0.34f + 0.10f * aqrndf();
        float base_g = 0.30f + 0.08f * aqrndf();
        float base_b = 0.27f + 0.06f * aqrndf();
        for (int la = 0; la <= ROCK_LAT; la++) {
            float v = (float)la / (float)ROCK_LAT;          // 0 top .. 1 bottom
            float th = v * (float)M_PI;
            for (int lo = 0; lo < ROCK_LON; lo++) {
                float u = (float)(lo % (ROCK_LON - 1)) / (float)(ROCK_LON - 1);
                float ph = u * AQ_2PI;
                float dx = sinf(th) * cosf(ph);
                float dy = cosf(th);
                float dz = sinf(th) * sinf(ph);
                float bump = 1.0f + 0.30f * sinf(3.0f * ph + 5.0f * v + (float)r * 2.2f)
                                  + 0.14f * sinf(7.0f * ph - 3.0f * v);
                float rr = R * bump;
                aq_vtx_t *vx = &g_rock[r][la][lo];
                vx->px = cx + dx * rr * 1.35f;              // squat, wide rocks
                vx->py = cy + dy * rr * 0.75f;
                vx->pz = cz + dz * rr;
                vx->nx = dx; vx->ny = dy; vx->nz = dz;
                float m = 0.85f + 0.3f * aqrndf();
                vx->r = base_r * m; vx->g = base_g * m; vx->b = base_b * m;
            }
        }
    }

    // coral fingers on the first two rocks
    g_ncoral = 0;
    for (int c = 0; c < 2 && c < g_nrock; c++) {
        int nb = 5 + (int)(aqrnd() % 3u);
        float palr = (c == 0) ? 0.85f : 0.95f;              // magenta vs orange
        float palg = (c == 0) ? 0.25f : 0.45f;
        float palb = (c == 0) ? 0.62f : 0.12f;
        for (int b = 0; b < nb; b++) {
            float bx = g_rock_cx[c] + 0.45f * aqrndc();
            float bz = g_rock_cz[c] + 0.30f * aqrndc();
            float by = g_rock_cy[c] + g_rock_R[c] * 0.45f;
            float ang = aqrndf() * AQ_2PI;
            float lean = 0.25f + 0.5f * aqrndf();
            float lx = cosf(ang) * lean, lz = sinf(ang) * lean;
            float len = 0.30f + 0.28f * aqrndf();
            float w0 = 0.075f + 0.025f * aqrndf();
            // 3 tapered segments of a 4-sided finger
            float px = bx, py = by, pz = bz;
            for (int s = 0; s < 3; s++) {
                float t0 = (float)s / 3.0f, t1 = (float)(s + 1) / 3.0f;
                float qx = bx + lx * len * t1, qz = bz + lz * len * t1;
                float qy = by + len * t1 * 1.25f;
                float r0 = w0 * (1.0f - 0.75f * t0), r1 = w0 * (1.0f - 0.75f * t1);
                for (int side = 0; side < 4 && g_ncoral < CORAL_QMAX; side++) {
                    float a0 = (float)side * (AQ_2PI / 4.0f);
                    float a1 = a0 + (AQ_2PI / 4.0f);
                    float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
                    aq_vtx_t *q = g_coral[g_ncoral++];
                    float shade = 0.8f + 0.35f * t0;
                    q[0].px = px + c0 * r0; q[0].py = py; q[0].pz = pz + s0 * r0;
                    q[1].px = px + c1 * r0; q[1].py = py; q[1].pz = pz + s1 * r0;
                    q[2].px = qx + c1 * r1; q[2].py = qy; q[2].pz = qz + s1 * r1;
                    q[3].px = qx + c0 * r1; q[3].py = qy; q[3].pz = qz + s0 * r1;
                    for (int v = 0; v < 4; v++) {
                        q[v].nx = cosf((a0 + a1) * 0.5f);
                        q[v].ny = 0.25f;
                        q[v].nz = sinf((a0 + a1) * 0.5f);
                        float sh = (v >= 2) ? shade + 0.3f : shade;
                        q[v].r = palr * sh; q[v].g = palg * sh; q[v].b = palb * sh;
                    }
                }
                px = qx; py = qy; pz = qz;
            }
        }
    }

    // kelp plants
    static const int PLN[3] = { 4, 9, 14 };
    g_nplant = PLN[g_opt_plants];
    for (int p = 0; p < g_nplant; p++) {
        aq_plant_t *pl = &g_plant[p];
        // cluster left and right, keep the mid-tank swim lane open
        float side = (p & 1) ? 1.0f : -1.0f;
        pl->x = side * (1.6f + 2.2f * aqrndf());
        pl->z = -1.4f + 1.6f * aqrndf();
        pl->h = 1.2f + 1.1f * aqrndf();
        pl->w = 0.07f + 0.05f * aqrndf();
        pl->ph = aqrndf() * AQ_2PI;
        pl->lean = 0.25f * aqrndc();
        pl->hue = aqrndf();
    }

    // fish population
    static const int CLOWN_N[3] = { 1, 2, 3 };
    static const int TANG_N[3]  = { 1, 1, 2 };
    static const int ANGEL_N[3] = { 0, 1, 2 };
    static const int GOBY_N[3]  = { 1, 1, 2 };
    static const int SCH_N[3]   = { 10, 16, 22 };
    g_nbig = 0;
    if (g_opt_species & (1 << SP_CLOWN))
        for (int i = 0; i < CLOWN_N[g_opt_pop] && g_nbig < BIG_MAX; i++)
            aq_spawn_fish(&g_big[g_nbig++], SP_CLOWN, 1.0f);
    if (g_opt_species & (1 << SP_TANG))
        for (int i = 0; i < TANG_N[g_opt_pop] && g_nbig < BIG_MAX; i++)
            aq_spawn_fish(&g_big[g_nbig++], SP_TANG, 1.0f);
    if (g_opt_species & (1 << SP_ANGEL))
        for (int i = 0; i < ANGEL_N[g_opt_pop] && g_nbig < BIG_MAX; i++)
            aq_spawn_fish(&g_big[g_nbig++], SP_ANGEL, 1.0f);
    if (g_opt_species & (1 << SP_GOBY))
        for (int i = 0; i < GOBY_N[g_opt_pop] && g_nbig < BIG_MAX; i++)
            aq_spawn_fish(&g_big[g_nbig++], SP_GOBY, 1.0f);
    g_nsch = 0;
    if (g_opt_species & (1 << SP_NEON)) {
        int n = SCH_N[g_opt_pop];
        if (!g_opt_quality && n > 14) n = 14;
        for (int i = 0; i < n && g_nsch < SCH_MAX; i++)
            aq_spawn_fish(&g_sch[g_nsch++], SP_NEON, 1.0f);
    }
    g_anchor_x = 0.0f; g_anchor_y = 1.6f; g_anchor_z = 0.0f;

    // bubble emitters on rock tops
    g_nem = (g_nrock >= 3) ? 3 : g_nrock;
    for (int e = 0; e < g_nem; e++) {
        g_emx[e] = g_rock_cx[e] + 0.2f * aqrndc();
        g_emy[e] = g_rock_cy[e] + g_rock_R[e] * 0.5f;
        g_emz[e] = g_rock_cz[e];
    }
    static const int BUBN[3] = { 0, 36, 78 };
    g_nbub = BUBN[g_opt_bubbles];
    if (!g_opt_quality && g_nbub > 40) g_nbub = 40;
    for (int b = 0; b < g_nbub; b++) {
        aq_bub_t *bb = &g_bub[b];
        bb->em = (g_nem > 0) ? (int)(aqrnd() % (unsigned)g_nem) : 0;
        bb->x = g_emx[bb->em]; bb->z = g_emz[bb->em];
        bb->y = g_emy[bb->em] + aqrndf() * (TANK_Y - g_emy[bb->em]);
        bb->vy = 0.012f + 0.010f * aqrndf();
        bb->sz = 0.016f + 0.020f * aqrndf();
        bb->ph = aqrndf() * AQ_2PI;
    }

    // god rays
    g_nray = g_opt_quality ? 5 : 3;
    for (int r = 0; r < g_nray; r++) {
        g_ray_x[r] = -3.2f + 6.4f * (float)r / (float)(g_nray - 1) + 0.4f * aqrndc();
        g_ray_ph[r] = aqrndf() * AQ_2PI;
    }
}

// ===========================================================================
// Options API (see gldemo.h)
// ===========================================================================
int gldemo_aquarium_option(int opt, int val) {
    switch (opt) {
    case AQOPT_POPULATION:
        if (val < 0) val = 0; if (val > 2) val = 2;
        if (val != g_opt_pop) { g_opt_pop = val; g_need_reseed = 1; }
        return g_opt_pop;
    case AQOPT_SPECIES:
        val &= 31; if (val == 0) val = 31;
        if (val != g_opt_species) { g_opt_species = val; g_need_reseed = 1; }
        return g_opt_species;
    case AQOPT_TINT:
        if (val < 0) val = 0; if (val > 3) val = 3;
        g_opt_tint = val; return g_opt_tint;
    case AQOPT_LIGHT:
        if (val < 0) val = 0; if (val > 2) val = 2;
        g_opt_light = val; return g_opt_light;
    case AQOPT_CAUSTICS:
        g_opt_caustics = (val != 0); return g_opt_caustics;
    case AQOPT_BUBBLES:
        if (val < 0) val = 0; if (val > 2) val = 2;
        if (val != g_opt_bubbles) { g_opt_bubbles = val; g_need_reseed = 1; }
        return g_opt_bubbles;
    case AQOPT_CAMERA:
        if (val < 0) val = 0; if (val > 2) val = 2;
        g_opt_camera = val; return g_opt_camera;
    case AQOPT_PLANTS:
        if (val < 0) val = 0; if (val > 2) val = 2;
        if (val != g_opt_plants) { g_opt_plants = val; g_need_reseed = 1; }
        return g_opt_plants;
    case AQOPT_QUALITY:
        val = (val != 0);
        if (val != g_opt_quality) { g_opt_quality = val; g_need_reseed = 1; }
        return g_opt_quality;
    default:
        return -1;
    }
}

// ===========================================================================
// Behaviour
// ===========================================================================
static float aq_angdiff(float a, float b) {
    float d = a - b;
    while (d >  (float)M_PI) d -= AQ_2PI;
    while (d < -(float)M_PI) d += AQ_2PI;
    return d;
}

static void aq_update_big(aq_fish_t *f) {
    if (f->sp == SP_GOBY) {
        // rest-then-dart bottom dweller
        f->statet -= 1.0f;
        if (f->state == 0) {
            f->phase += 0.06f;                      // slow idle fin motion
            if (f->statet <= 0.0f) {
                f->state = 1;
                f->tx = aqrndc() * (TANK_X - 1.0f);
                f->tz = aqrndc() * (TANK_Z - 0.6f);
                f->statet = 30.0f + aqrndf() * 30.0f;
            }
        } else {
            float dx = f->tx - f->x, dz = f->tz - f->z;
            float d = sqrtf(dx * dx + dz * dz);
            float want = atan2f(-dz, dx);
            float ad = aq_angdiff(want, f->heading);
            f->heading += ad * 0.25f;
            float sp = (d > 0.4f) ? 0.055f : d * 0.14f;
            f->x += cosf(f->heading) * sp;
            f->z += -sinf(f->heading) * sp;
            f->phase += 0.9f;
            if (d < 0.05f || f->statet <= 0.0f) {
                f->state = 0;
                f->statet = 90.0f + aqrndf() * 200.0f;
            }
        }
        float sy = aq_sand_h(f->x, f->z) + 0.05f;
        f->y += (sy - f->y) * 0.2f;
        f->turn *= 0.8f;
        return;
    }

    // cruising fish: wander + wall avoidance + banking
    float wander = 0.55f * sinf(0.011f * g_t + f->ph1)
                 + 0.35f * sinf(0.0047f * g_t + f->ph2);
    float turn = wander * 0.016f;

    // predictive glass avoidance: look ahead ~40 frames
    float hx = cosf(f->heading), hz = -sinf(f->heading);
    float ax = f->x + hx * f->speed * 40.0f;
    float az = f->z + hz * f->speed * 40.0f;
    if (ax > TANK_X - 0.5f || ax < -(TANK_X - 0.5f) ||
        az > TANK_Z - 0.35f || az < -(TANK_Z - 0.35f)) {
        float want = atan2f(f->z * 0.6f, -f->x);   // roughly toward center
        turn += aq_angdiff(want, f->heading) * 0.045f;
    }
    if (turn > 0.05f) turn = 0.05f; if (turn < -0.05f) turn = -0.05f;
    f->heading += turn;
    f->turn += (turn - f->turn) * 0.08f;           // smoothed, drives banking

    float spd = f->speed * (0.85f + 0.30f * sinf(0.009f * g_t + f->ph2));
    f->x += cosf(f->heading) * spd;
    f->z += -sinf(f->heading) * spd;

    // gentle vertical wandering toward the species band
    float ytgt = 0.5f + f->yband * 0.55f + 0.45f * sinf(0.006f * g_t + f->ph1);
    f->vy += ((ytgt - f->y) * 0.002f - f->vy * 0.06f);
    f->y += f->vy;
    if (f->y > TANK_Y - 0.25f) { f->y = TANK_Y - 0.25f; f->vy = 0.0f; }
    if (f->y < 0.30f) { f->y = 0.30f; f->vy = 0.0f; }

    // hard clamp (fallback if avoidance loses)
    if (f->x >  TANK_X - 0.2f) f->x =  TANK_X - 0.2f;
    if (f->x < -TANK_X + 0.2f) f->x = -TANK_X + 0.2f;
    if (f->z >  TANK_Z - 0.2f) f->z =  TANK_Z - 0.2f;
    if (f->z < -TANK_Z + 0.2f) f->z = -TANK_Z + 0.2f;

    f->phase += 0.16f + spd * 6.0f;                // tail beat scales with speed
}

static void aq_update_school(void) {
    // wandering anchor the school loosely follows
    g_anchor_x = 2.2f * sinf(0.0031f * g_t) + 0.9f * sinf(0.0013f * g_t + 2.0f);
    g_anchor_y = 1.5f + 0.6f * sinf(0.0021f * g_t + 1.0f);
    g_anchor_z = 0.8f * sinf(0.0026f * g_t + 4.0f);

    for (int i = 0; i < g_nsch; i++) {
        aq_fish_t *f = &g_sch[i];
        float sepx = 0, sepy = 0, sepz = 0;
        float alix = 0, aliy = 0, aliz = 0;
        float cohx = 0, cohy = 0, cohz = 0;
        int n = 0;
        for (int j = 0; j < g_nsch; j++) {
            if (j == i) continue;
            aq_fish_t *o = &g_sch[j];
            float dx = o->x - f->x, dy = o->y - f->y, dz = o->z - f->z;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > 1.44f) continue;              // neighbourhood 1.2
            n++;
            cohx += o->x; cohy += o->y; cohz += o->z;
            alix += cosf(o->heading); aliz += -sinf(o->heading); aliy += o->vy;
            if (d2 < 0.09f && d2 > 1e-6f) {        // separation 0.3
                float inv = 1.0f / d2;
                sepx -= dx * inv; sepy -= dy * inv; sepz -= dz * inv;
            }
        }
        // desired direction = alignment + cohesion + separation + anchor pull
        float dx = cosf(f->heading), dz = -sinf(f->heading), dy = f->vy * 8.0f;
        if (n > 0) {
            cohx = cohx / (float)n - f->x;
            cohy = cohy / (float)n - f->y;
            cohz = cohz / (float)n - f->z;
            dx += alix * 0.10f + cohx * 0.16f + sepx * 0.05f;
            dy += aliy * 0.10f + cohy * 0.16f + sepy * 0.05f;
            dz += aliz * 0.10f + cohz * 0.16f + sepz * 0.05f;
        }
        dx += (g_anchor_x - f->x) * 0.05f;
        dy += (g_anchor_y - f->y) * 0.05f;
        dz += (g_anchor_z - f->z) * 0.05f;
        // glass avoidance dominates near the walls
        if (f->x >  TANK_X - 0.7f) dx -= (f->x - (TANK_X - 0.7f)) * 1.5f;
        if (f->x < -TANK_X + 0.7f) dx += ((-TANK_X + 0.7f) - f->x) * 1.5f;
        if (f->z >  TANK_Z - 0.5f) dz -= (f->z - (TANK_Z - 0.5f)) * 1.5f;
        if (f->z < -TANK_Z + 0.5f) dz += ((-TANK_Z + 0.5f) - f->z) * 1.5f;
        if (f->y >  TANK_Y - 0.4f) dy -= (f->y - (TANK_Y - 0.4f)) * 1.5f;
        if (f->y < 0.5f)           dy += (0.5f - f->y) * 1.5f;

        float want = atan2f(-dz, dx);
        float ad = aq_angdiff(want, f->heading);
        if (ad > 0.09f) ad = 0.09f; if (ad < -0.09f) ad = -0.09f;
        f->heading += ad;
        f->turn += (ad - f->turn) * 0.15f;
        f->vy += (dy * 0.010f - f->vy * 0.08f);
        if (f->vy > 0.02f) f->vy = 0.02f; if (f->vy < -0.02f) f->vy = -0.02f;

        f->x += cosf(f->heading) * f->speed;
        f->z += -sinf(f->heading) * f->speed;
        f->y += f->vy;
        if (f->y > TANK_Y - 0.25f) f->y = TANK_Y - 0.25f;
        if (f->y < 0.35f) f->y = 0.35f;
        if (f->x >  TANK_X - 0.15f) f->x =  TANK_X - 0.15f;
        if (f->x < -TANK_X + 0.15f) f->x = -TANK_X + 0.15f;
        if (f->z >  TANK_Z - 0.12f) f->z =  TANK_Z - 0.12f;
        if (f->z < -TANK_Z + 0.12f) f->z = -TANK_Z + 0.12f;
        f->phase += 0.34f + f->speed * 8.0f;
    }
}

// ===========================================================================
// Fish rendering
// ===========================================================================
// species colour pattern: u = 0 tail .. 1 nose, ang = ring angle (0 = top)
static void aq_pattern(int sp, float u, float ang, float mottle,
                       float *r, float *g, float *b) {
    float ca = cosf(ang);                          // 1 top .. -1 belly
    switch (sp) {
    case SP_CLOWN: {
        *r = 1.00f; *g = 0.44f; *b = 0.07f;
        if (ca < -0.5f) { *r = 1.0f; *g = 0.58f; *b = 0.22f; }   // paler belly
        static const float band[3] = { 0.24f, 0.55f, 0.86f };
        for (int i = 0; i < 3; i++) {
            float d = u - band[i]; if (d < 0) d = -d;
            if (d < 0.055f) { *r = 0.96f; *g = 0.95f; *b = 0.90f; }
            else if (d < 0.085f) { *r = 0.06f; *g = 0.05f; *b = 0.05f; }
        }
        break;
    }
    case SP_TANG:
        *r = 0.07f; *g = 0.16f; *b = 0.80f;
        if (u > 0.30f && u < 0.85f && ca > -0.2f)  // dark palette marking
            { *r = 0.02f; *g = 0.05f; *b = 0.38f; }
        if (u < 0.16f) { *r = 0.95f; *g = 0.85f; *b = 0.10f; }   // yellow tail
        break;
    case SP_ANGEL: {
        *r = 0.90f; *g = 0.82f; *b = 0.45f;
        float s = sinf(u * 21.0f);
        if (s > 0.45f) { *r = 0.16f; *g = 0.19f; *b = 0.28f; }   // vertical bars
        if (u > 0.92f) { *r = 0.25f; *g = 0.30f; *b = 0.42f; }   // dark face
        break;
    }
    case SP_GOBY:
        *r = 0.74f * mottle; *g = 0.64f * mottle; *b = 0.44f * mottle;
        if (ca < -0.4f) { *r = 0.85f; *g = 0.80f; *b = 0.66f; }  // pale belly
        break;
    default: // SP_NEON
        if (ca > 0.05f) { *r = 0.15f; *g = 0.72f; *b = 0.88f; }  // cyan back
        else if (u < 0.55f) { *r = 0.88f; *g = 0.14f; *b = 0.18f; } // red rear
        else { *r = 0.80f; *g = 0.85f; *b = 0.90f; }             // silver belly
        break;
    }
}

// species body proportions
static void aq_proportions(int sp, float *H, float *W, float *dorsal, float *tail) {
    switch (sp) {
    case SP_CLOWN: *H = 0.32f; *W = 0.14f; *dorsal = 0.10f; *tail = 0.26f; break;
    case SP_TANG:  *H = 0.40f; *W = 0.10f; *dorsal = 0.13f; *tail = 0.24f; break;
    case SP_ANGEL: *H = 0.55f; *W = 0.09f; *dorsal = 0.26f; *tail = 0.22f; break;
    case SP_GOBY:  *H = 0.20f; *W = 0.17f; *dorsal = 0.07f; *tail = 0.22f; break;
    default:       *H = 0.26f; *W = 0.12f; *dorsal = 0.06f; *tail = 0.30f; break;
    }
}

#define FISH_NSMAX 9
#define FISH_NRMAX 8
static void aq_draw_fish(const aq_fish_t *f, int ns, int nr) {
    float H, W, dorsal, tailL;
    aq_proportions(f->sp, &H, &W, &dorsal, &tailL);
    float L = f->size;
    float amp = 0.16f;                              // undulation amplitude
    float pitch = f->vy * 14.0f;
    if (pitch > 0.5f) pitch = 0.5f; if (pitch < -0.5f) pitch = -0.5f;
    float roll = -f->turn * 9.0f;
    if (roll > 0.7f) roll = 0.7f; if (roll < -0.7f) roll = -0.7f;

    // rotation: world = Ry(heading) * Rz(pitch) * Rx(roll), fish nose = +x
    float cy = cosf(f->heading), sy = sinf(f->heading);
    float cp = cosf(pitch), sp = sinf(pitch);
    float cr = cosf(roll), sr = sinf(roll);
    // rows of M = Ry(yaw) * Rz(pitch) * Rx(roll); heading 0 faces +x and
    // increasing heading turns toward -z (matches aq_update_*'s x += cos,
    // z += -sin step).
    float m00 = cy * cp, m01 = -cy * sp * cr + sy * sr, m02 = cy * sp * sr + sy * cr;
    float m10 = sp,      m11 = cp * cr,                 m12 = -cp * sr;
    float m20 = -sy * cp, m21 = sy * sp * cr + cy * sr, m22 = -sy * sp * sr + cy * cr;

#define XFORM(lx, ly, lz, ox, oy, oz) do {                              \
        (ox) = f->x + m00 * (lx) + m01 * (ly) + m02 * (lz);             \
        (oy) = f->y + m10 * (lx) + m11 * (ly) + m12 * (lz);             \
        (oz) = f->z + m20 * (lx) + m21 * (ly) + m22 * (lz);             \
    } while (0)
#define NXFORM(lx, ly, lz, ox, oy, oz) do {                             \
        (ox) = m00 * (lx) + m01 * (ly) + m02 * (lz);                    \
        (oy) = m10 * (lx) + m11 * (ly) + m12 * (lz);                    \
        (oz) = m20 * (lx) + m21 * (ly) + m22 * (lz);                    \
    } while (0)

    // ring cache for strip emission
    static float rp[2][FISH_NRMAX + 1][3], rn[2][FISH_NRMAX + 1][3], rc[2][FISH_NRMAX + 1][3];
    float zoff_tail = 0.0f;

    for (int i = 0; i <= ns; i++) {
        int cur = i & 1;
        float u = 1.0f - (float)i / (float)ns;      // 1 nose .. 0 tail
        float xl = (u - 0.5f) * L;
        // spine undulation: grows toward the tail; add turn-induced curve
        float uu = 1.0f - u;
        float zoff = L * amp * (0.05f + 0.85f * uu * uu) * sinf(f->phase - uu * 4.4f)
                   + f->turn * L * 5.0f * uu * uu;
        if (i == ns) zoff_tail = zoff;
        // body profile
        float prof = sinf((float)M_PI * powf(u, 0.62f));
        if (prof < 0.02f) prof = 0.02f;
        float ry = L * H * prof * (1.0f + 0.15f * sinf(u * 3.0f));
        float rz = L * W * prof;
        float mot = 0.8f + 0.4f * sinf(u * 37.0f + f->ph1 * 9.0f);   // goby mottle
        for (int j = 0; j <= nr; j++) {
            float ang = AQ_2PI * (float)j / (float)nr;
            float cay = cosf(ang), saz = sinf(ang);
            float ly = cay * ry, lz = saz * rz + zoff;
            float cr_, cg_, cb_;
            aq_pattern(f->sp, u, ang, mot, &cr_, &cg_, &cb_);
            // ellipse normal (unnormalized is fine for lambert-approx)
            float nyl = cay * rz, nzl = saz * ry;
            float nl = sqrtf(nyl * nyl + nzl * nzl);
            if (nl < 1e-5f) nl = 1.0f;
            nyl /= nl; nzl /= nl;
            XFORM(xl, ly, lz, rp[cur][j][0], rp[cur][j][1], rp[cur][j][2]);
            NXFORM(0.0f, nyl, nzl, rn[cur][j][0], rn[cur][j][1], rn[cur][j][2]);
            rc[cur][j][0] = cr_; rc[cur][j][1] = cg_; rc[cur][j][2] = cb_;
        }
        if (i > 0) {
            int prv = cur ^ 1;
            glBegin(GL_QUADS);
            for (int j = 0; j < nr; j++) {
                aq_emit(rp[prv][j][0], rp[prv][j][1], rp[prv][j][2],
                        rc[prv][j][0], rc[prv][j][1], rc[prv][j][2],
                        rn[prv][j][0], rn[prv][j][1], rn[prv][j][2]);
                aq_emit(rp[prv][j+1][0], rp[prv][j+1][1], rp[prv][j+1][2],
                        rc[prv][j+1][0], rc[prv][j+1][1], rc[prv][j+1][2],
                        rn[prv][j+1][0], rn[prv][j+1][1], rn[prv][j+1][2]);
                aq_emit(rp[cur][j+1][0], rp[cur][j+1][1], rp[cur][j+1][2],
                        rc[cur][j+1][0], rc[cur][j+1][1], rc[cur][j+1][2],
                        rn[cur][j+1][0], rn[cur][j+1][1], rn[cur][j+1][2]);
                aq_emit(rp[cur][j][0], rp[cur][j][1], rp[cur][j][2],
                        rc[cur][j][0], rc[cur][j][1], rc[cur][j][2],
                        rn[cur][j][0], rn[cur][j][1], rn[cur][j][2]);
            }
            glEnd();
        }
    }

    // tail fin: fan behind the tail, swinging harder than the spine
    float tswing = zoff_tail * 1.9f + L * amp * 0.35f * sinf(f->phase);
    float tr, tg, tb;
    aq_pattern(f->sp, 0.02f, 0.0f, 1.0f, &tr, &tg, &tb);
    float tipx, tipy, tipz;
    XFORM(-0.5f * L, 0.0f, zoff_tail, tipx, tipy, tipz);
    glBegin(GL_TRIANGLE_FAN);
    aq_emit(tipx, tipy, tipz, tr * 0.9f, tg * 0.9f, tb * 0.9f, 0, 0.3f, 0.95f);
    for (int k = 0; k <= 4; k++) {
        float a = -1.0f + 2.0f * (float)k / 4.0f;   // -1..1 spread
        float fx = -0.5f * L - tailL * L * (0.75f + 0.25f * (1.0f - a * a));
        float fy = a * L * H * 1.05f;
        float px, py, pz;
        XFORM(fx, fy, tswing, px, py, pz);
        aq_emit(px, py, pz, tr, tg, tb, 0, 0.3f, 0.95f);
    }
    glEnd();

    // dorsal fin: sail along the back, tall on the angelfish
    if (dorsal > 0.01f) {
        glBegin(GL_TRIANGLE_STRIP);
        for (int k = 0; k <= 4; k++) {
            float u = 0.28f + 0.46f * (float)k / 4.0f;
            float uu = 1.0f - u;
            float xl = (u - 0.5f) * L;
            float zoff = L * amp * (0.05f + 0.85f * uu * uu) * sinf(f->phase - uu * 4.4f);
            float prof = sinf((float)M_PI * powf(u, 0.62f));
            float topy = L * H * prof;
            float finh = L * dorsal * sinf((float)M_PI * (float)k / 4.0f);
            float bx, by, bz, tx, ty, tz;
            XFORM(xl, topy * 0.96f, zoff, bx, by, bz);
            XFORM(xl - finh * 0.35f, topy + finh, zoff * 1.05f, tx, ty, tz);
            float dr, dg, db;
            aq_pattern(f->sp, u, 0.0f, 1.0f, &dr, &dg, &db);
            aq_emit(bx, by, bz, dr, dg, db, 0, 0.4f, 0.9f);
            aq_emit(tx, ty, tz, dr * 0.75f, dg * 0.75f, db * 0.75f, 0, 0.6f, 0.8f);
        }
        glEnd();
        // angelfish gets a mirrored anal fin for the tall rhombic silhouette
        if (f->sp == SP_ANGEL) {
            glBegin(GL_TRIANGLE_STRIP);
            for (int k = 0; k <= 4; k++) {
                float u = 0.30f + 0.40f * (float)k / 4.0f;
                float uu = 1.0f - u;
                float xl = (u - 0.5f) * L;
                float zoff = L * amp * (0.05f + 0.85f * uu * uu) * sinf(f->phase - uu * 4.4f);
                float prof = sinf((float)M_PI * powf(u, 0.62f));
                float boty = -L * H * prof;
                float finh = L * dorsal * 0.8f * sinf((float)M_PI * (float)k / 4.0f);
                float bx, by, bz, tx, ty, tz;
                XFORM(xl, boty * 0.96f, zoff, bx, by, bz);
                XFORM(xl - finh * 0.3f, boty - finh, zoff * 1.05f, tx, ty, tz);
                float dr, dg, db;
                aq_pattern(f->sp, u, (float)M_PI, 1.0f, &dr, &dg, &db);
                aq_emit(bx, by, bz, dr, dg, db, 0, -0.4f, 0.9f);
                aq_emit(tx, ty, tz, dr * 0.7f, dg * 0.7f, db * 0.7f, 0, -0.6f, 0.8f);
            }
            glEnd();
        }
    }
#undef XFORM
#undef NXFORM
}

// ===========================================================================
// Scene rendering
// ===========================================================================
static void aq_draw_background(float aspect) {
    // view-space gradient quad at the far plane: lighter toward the surface
    glDisable(GL_DEPTH_TEST);
    glDepthMask(0);
    glPushMatrix();
    glLoadIdentity();
    const aq_tint_t *tt = &AQ_TINTS[g_opt_tint];
    float li = g_light;
    float z = -70.0f;
    float hh = 0.55f * 70.0f / 0.9f;                // frustum nh=0.55 near=0.9
    float ww = hh * aspect;
    float topr = tt->fr * 1.55f * li, topg = tt->fg * 1.55f * li, topb = tt->fb * 1.45f * li;
    float botr = tt->cr * li, botg = tt->cg * li, botb = tt->cb * li;
    glBegin(GL_QUADS);
    glColor3f(botr, botg, botb); glVertex3f(-ww, -hh, z);
    glColor3f(botr, botg, botb); glVertex3f( ww, -hh, z);
    glColor3f(topr, topg, topb); glVertex3f( ww,  hh, z);
    glColor3f(topr, topg, topb); glVertex3f(-ww,  hh, z);
    glEnd();
    glPopMatrix();
    glDepthMask(1);
    glEnable(GL_DEPTH_TEST);
}

static void aq_draw_sand(void) {
    float ca = g_opt_caustics ? 0.20f * g_light : 0.0f;
    for (int k = 0; k < SAND_NZ - 1; k++) {
        glBegin(GL_QUADS);
        for (int i = 0; i < SAND_NX - 1; i++) {
            for (int q = 0; q < 4; q++) {
                static const int di[4] = { 0, 1, 1, 0 };
                static const int dk[4] = { 0, 0, 1, 1 };
                int ii = i + di[q], kk = k + dk[q];
                float x = g_sand_px[ii], z = g_sand_pz[kk], y = g_sand_y[kk][ii];
                float caus = 1.0f + ca * sinf(1.7f * x + 2.3f * z + 0.05f * g_t
                                              + 1.5f * sinf(0.9f * x - 0.031f * g_t))
                                       * sinf(2.2f * z - 1.1f * x + 0.043f * g_t);
                float m = g_sand_c[kk][ii] * caus * g_light * (0.55f + 0.25f * g_light);
                aq_emitf(x, y, z, 0.78f * m, 0.70f * m, 0.52f * m);
            }
        }
        glEnd();
    }
}

static void aq_draw_rocks(void) {
    for (int r = 0; r < g_nrock; r++) {
        for (int la = 0; la < ROCK_LAT; la++) {
            glBegin(GL_QUADS);
            for (int lo = 0; lo < ROCK_LON - 1; lo++) {
                const aq_vtx_t *a = &g_rock[r][la][lo];
                const aq_vtx_t *b = &g_rock[r][la][lo + 1];
                const aq_vtx_t *c = &g_rock[r][la + 1][lo + 1];
                const aq_vtx_t *d = &g_rock[r][la + 1][lo];
                aq_emit(a->px, a->py, a->pz, a->r, a->g, a->b, a->nx, a->ny, a->nz);
                aq_emit(b->px, b->py, b->pz, b->r, b->g, b->b, b->nx, b->ny, b->nz);
                aq_emit(c->px, c->py, c->pz, c->r, c->g, c->b, c->nx, c->ny, c->nz);
                aq_emit(d->px, d->py, d->pz, d->r, d->g, d->b, d->nx, d->ny, d->nz);
            }
            glEnd();
        }
    }
}

static void aq_draw_coral(void) {
    glBegin(GL_QUADS);
    for (int q = 0; q < g_ncoral; q++)
        for (int v = 0; v < 4; v++) {
            const aq_vtx_t *p = &g_coral[q][v];
            aq_emit(p->px, p->py, p->pz, p->r, p->g, p->b, p->nx, p->ny, p->nz);
        }
    glEnd();
}

static void aq_draw_plants(void) {
    for (int p = 0; p < g_nplant; p++) {
        const aq_plant_t *pl = &g_plant[p];
        float base_y = aq_sand_h(pl->x, pl->z);
        float gr = 0.10f + 0.10f * pl->hue;
        float gg = 0.42f + 0.22f * pl->hue;
        float gb = 0.12f + 0.08f * (1.0f - pl->hue);
        glBegin(GL_TRIANGLE_STRIP);
        for (int s = 0; s <= PLANT_SEG; s++) {
            float t = (float)s / (float)PLANT_SEG;
            float sw = t * sqrtf(t);
            float sx = pl->x + pl->lean * t
                     + 0.30f * sw * sinf(0.021f * g_t + pl->ph + t * 2.6f);
            float sz = pl->z
                     + 0.18f * sw * sinf(0.017f * g_t + pl->ph * 1.7f + t * 2.1f);
            float sy = base_y + pl->h * t;
            float w = pl->w * (1.0f - 0.75f * t);
            float sh = 0.75f + 0.5f * t;            // brighter toward the tip
            aq_emit(sx - w, sy, sz, gr * sh, gg * sh, gb * sh, 0.1f, 0.35f, 0.93f);
            aq_emit(sx + w, sy, sz, gr * sh * 0.8f, gg * sh * 0.8f, gb * sh * 0.8f,
                    -0.1f, 0.35f, 0.93f);
        }
        glEnd();
    }
}

static void aq_update_draw_bubbles(void) {
    if (g_nbub <= 0 || g_nem <= 0) return;
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);
    glDepthMask(0);                                 // read depth, never write
    glBegin(GL_QUADS);
    for (int b = 0; b < g_nbub; b++) {
        aq_bub_t *bb = &g_bub[b];
        bb->y += bb->vy;
        bb->ph += 0.09f;
        float wob = 0.05f * sinf(bb->ph);
        if (bb->y > TANK_Y - 0.05f) {
            bb->em = (int)(aqrnd() % (unsigned)g_nem);
            bb->x = g_emx[bb->em]; bb->z = g_emz[bb->em];
            bb->y = g_emy[bb->em];
            bb->vy = 0.012f + 0.010f * aqrndf();
            bb->sz = 0.016f + 0.020f * aqrndf();
        }
        float x = bb->x + wob, y = bb->y, z = bb->z;
        float s = bb->sz * (1.0f + 0.5f * (y / TANK_Y));
        float br = 0.10f + 0.10f * g_light;
        // camera-facing quad via the camera right vector
        float rx = g_rightx * s, rz = g_rightz * s;
        glColor3f(br, br * 1.15f, br * 1.25f);
        glVertex3f(x - rx, y - s, z - rz);
        glVertex3f(x + rx, y - s, z + rz);
        glColor3f(br * 1.5f, br * 1.6f, br * 1.7f);
        glVertex3f(x + rx, y + s, z + rz);
        glVertex3f(x - rx, y + s, z - rz);
    }
    glEnd();
    glDepthMask(1);
    glDisable(GL_BLEND);
}

static void aq_draw_rays_and_surface(void) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);
    glDepthMask(0);

    // surface shimmer: faint additive strip at the waterline
    glDisable(GL_DEPTH_TEST);
    glBegin(GL_QUADS);
    for (int i = 0; i < 10; i++) {
        float x0 = -4.4f + 0.88f * (float)i;
        float x1 = x0 + 0.88f;
        float w0 = 0.05f + 0.045f * sinf(0.061f * g_t + (float)i * 1.7f);
        float w1 = 0.05f + 0.045f * sinf(0.061f * g_t + (float)(i + 1) * 1.7f);
        float br = 0.055f * g_light;
        glColor3f(br, br * 1.4f, br * 1.5f);
        glVertex3f(x0, TANK_Y - w0, -1.0f);
        glVertex3f(x1, TANK_Y - w1, -1.0f);
        glColor3f(br * 2.2f, br * 2.6f, br * 2.7f);
        glVertex3f(x1, TANK_Y + w1, -1.0f);
        glVertex3f(x0, TANK_Y + w0, -1.0f);
    }
    glEnd();

    // god rays: swaying additive shafts from the surface down to the sand
    if (g_opt_caustics) {
        glBegin(GL_QUADS);
        for (int r = 0; r < g_nray; r++) {
            float sway = 0.8f * sinf(0.004f * g_t + g_ray_ph[r]);
            float pulse = 0.65f + 0.35f * sinf(0.0093f * g_t + g_ray_ph[r] * 2.0f);
            float br = 0.030f * g_light * pulse;
            float topx = g_ray_x[r], botx = g_ray_x[r] + sway;
            float z = -0.9f;
            glColor3f(br * 1.4f, br * 1.7f, br * 1.8f);
            glVertex3f(topx - 0.22f, TANK_Y + 0.15f, z);
            glVertex3f(topx + 0.22f, TANK_Y + 0.15f, z);
            glColor3f(br * 0.3f, br * 0.4f, br * 0.45f);
            glVertex3f(botx + 0.55f, 0.05f, z);
            glVertex3f(botx - 0.55f, 0.05f, z);
        }
        glEnd();
    }
    glEnable(GL_DEPTH_TEST);
    glDepthMask(1);
    glDisable(GL_BLEND);
}

// ===========================================================================
// Camera
// ===========================================================================
static void aq_camera(void) {
    float ex, ey, ez, tx, ty, tz;
    switch (g_opt_camera) {
    default:
    case 0: {                                        // slow orbital sweep
        float a = 0.42f * sinf(0.0035f * g_t);
        ex = 6.0f * sinf(a);
        ey = 1.45f + 0.25f * sinf(0.0021f * g_t + 1.0f);
        ez = 6.0f * cosf(a);
        tx = 0.0f; ty = 1.05f; tz = 0.0f;
        break;
    }
    case 1:                                          // fixed front view
        ex = 0.0f; ey = 1.35f; ez = 6.0f;
        tx = 0.0f; ty = 1.2f; tz = 0.0f;
        break;
    case 2:                                          // lazy drifting glide
        ex = 1.6f * sinf(0.0023f * g_t) + 0.7f * sinf(0.0011f * g_t + 2.0f);
        ey = 1.35f + 0.45f * sinf(0.0017f * g_t + 0.7f);
        ez = 6.0f + 0.5f * sinf(0.0013f * g_t + 4.0f);
        tx = 0.9f * sinf(0.0019f * g_t + 3.0f);
        ty = 1.25f + 0.25f * sinf(0.0012f * g_t);
        tz = 0.0f;
        break;
    }
    g_eyex = ex; g_eyey = ey; g_eyez = ez;
    float fx = tx - ex, fy = ty - ey, fz = tz - ez;
    float fl = sqrtf(fx * fx + fy * fy + fz * fz);
    fx /= fl; fy /= fl; fz /= fl;
    // camera right (for billboards): cross(forward, up)
    g_rightx = -fz; g_rightz = fx;
    float rl = sqrtf(g_rightx * g_rightx + g_rightz * g_rightz);
    if (rl > 1e-5f) { g_rightx /= rl; g_rightz /= rl; }
    float yaw = atan2f(-fx, -fz) * AQ_R2D;
    float pitch = asinf(fy) * AQ_R2D;
    glLoadIdentity();
    glRotatef(-pitch, 1, 0, 0);
    glRotatef(-yaw, 0, 1, 0);
    glTranslatef(-ex, -ey, -ez);
}

// ===========================================================================
// Entry points (called from gldemo.c)
// ===========================================================================
void gldemo_aquarium_setup(void) {
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glShadeModel(GL_SMOOTH);
    g_prj_w = 0; g_prj_h = 0;
    g_t = 0.0f;
    aq_reseed();
    g_need_reseed = 0;
    g_aq_inited = 1;
}

void gldemo_aquarium_frame(void) {
    if (!g_aq_inited) return;
    if (g_need_reseed) { aq_reseed(); g_need_reseed = 0; }
    g_t += 1.0f;
    if (g_t > 1e8f) g_t = 0.0f;

    const aq_tint_t *tt = &AQ_TINTS[g_opt_tint];
    g_light = AQ_LIGHTS[g_opt_light];
    g_fogr = tt->fr * g_light; g_fogg = tt->fg * g_light; g_fogb = tt->fb * g_light;
    g_fogd = tt->dens;

    int w = gldemo_width(), h = gldemo_height();
    float aspect = (h > 0) ? (float)w / (float)h : 1.333f;
    if (w != g_prj_w || h != g_prj_h) {
        glViewport(0, 0, w, h);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glFrustum(-0.55 * aspect, 0.55 * aspect, -0.55, 0.55, 0.9, 80.0);
        glMatrixMode(GL_MODELVIEW);
        g_prj_w = w; g_prj_h = h;
    }
    glClearColor(tt->cr * g_light, tt->cg * g_light, tt->cb * g_light, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // simulate
    for (int i = 0; i < g_nbig; i++) aq_update_big(&g_big[i]);
    aq_update_school();
    aq_camera();

    // draw
    aq_draw_background(aspect);
    aq_draw_sand();
    aq_draw_rocks();
    aq_draw_coral();
    aq_draw_plants();
    int ns = g_opt_quality ? 8 : 6;
    int nr = g_opt_quality ? 7 : 5;
    for (int i = 0; i < g_nbig; i++) aq_draw_fish(&g_big[i], ns, nr);
    for (int i = 0; i < g_nsch; i++) aq_draw_fish(&g_sch[i], 4, 4);
    aq_update_draw_bubbles();
    aq_draw_rays_and_surface();
}
