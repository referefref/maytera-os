// gldemo_lavalamp.c - proper 3D lava lamp render core (GLDEMO_LAVALAMP).
//
// A classic rocket-style lava lamp: tapered glass vessel on a spun-metal
// base with a metal cap, filled with warm fluid and blobs of wax. The wax
// is a real METABALL isosurface: a Wyvill-style finite-support scalar field
// is accumulated on a coarse 3D grid every frame and polygonized with
// marching tetrahedra (6 tetrahedra per cell, no big case tables), so blobs
// genuinely merge, bridge, neck and pinch off like the real thing. Normals
// are ANALYTIC (gradient of the field summed per emitted vertex), so the
// surface shades smoothly regardless of grid resolution.
//
// Fluid dynamics is a buoyancy-driven convection loop per blob: a blob near
// the hot base warms toward the local environment temperature, gains
// buoyancy and rises; near the cool top it sheds heat, loses buoyancy and
// sinks. Vertical velocity stretches a blob into an ellipsoid (teardrop
// read while detaching), a molten pool blob sits over the bulb for risers
// to pinch off from, and slow per-blob wander phases keep the motion from
// ever looking periodic.
//
// TinyGL constraints honored here (measured in this tree, not assumed):
//   - Blending is ADDITIVE/SUBTRACTIVE only (zbuffer.h TGL_BLEND_FUNC has
//     no GL_SRC_ALPHA path), so the "glass" is composed as opaque interior
//     backdrop + additive fresnel/streak pass, never alpha blending.
//   - GL_LINE_LOOP and GL_POLYGON are compiled out (TGL_FEATURE_GL_POLYGON
//     0); only POINTS/LINES/STRIPS/TRIANGLES/FANS/QUADS are used.
//   - One global context per process (gldemo.c owns init/teardown).
//
// Performance: the field is only accumulated inside each blob's bounding
// box, cells are marched only inside the union bounding box and skipped by
// a corner min/max test, all lamp geometry (base, cap, backdrop, glass,
// glare, halo) is precomputed into static vertex/color arrays at init, and
// there are zero per-frame allocations. (lavalamp)
#include "../include/gldemo.h"
#include "../include/GL/gl.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
#define LL_FLUID_H   3.0f     // fluid column height (y = 0 .. LL_FLUID_H)
#define LL_ISO       0.25f    // isosurface threshold of the metaball field

// Field grid (points). Cells = (n-1) per axis. Sized so a blob spans about
// 8-10 cells: fine enough for smooth merges, coarse enough for software GL.
#define GX 27
#define GY 54
#define GZ 27
static const float X0 = -1.04f, X1 = 1.04f;
static const float Yb = -0.18f, Yt = 3.06f;   // includes the pool dome
static const float Z0 = -1.04f, Z1 = 1.04f;

#define LL_NBLOB 9            // blob 0 is the molten pool at the base
#define LL_NBUB  26           // tiny additive bubbles rising in the fluid

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef struct {
    float x, y, z;            // center
    float vx, vy, vz;
    float R;                  // base support radius
    float T;                  // temperature 0..1
    float sy;                 // vertical stretch (smoothed)
    float ph;                 // wander phase
    float wsp;                // wander speed
    float kmul;               // per-blob heat-exchange multiplier (desync)
    int   pool;               // 1 = fixed molten pool blob
    // per-frame derived (filled by ll_blob_derive):
    float ax2, ay2;           // 1/(R*sxz)^2, 1/(R*sy)^2
    float ext_xz, ext_y;      // support extents
} ll_blob_t;

static ll_blob_t g_bl[LL_NBLOB];

typedef struct { float x, y, z, vy, ph; } ll_bub_t;
static ll_bub_t g_bub[LL_NBUB];

static float g_fld[GY][GX][GZ];
static float g_gx[GX], g_gy[GY], g_gz[GZ];
static float g_t = 0.0f;      // frame counter (animation clock)
static int   g_ll_inited = 0;

static unsigned int g_llseed = 77031u;
static unsigned int llrnd(void) {
    g_llseed ^= g_llseed << 13; g_llseed ^= g_llseed >> 17; g_llseed ^= g_llseed << 5;
    return g_llseed;
}
static float llrndf(void) { return (float)(llrnd() & 0xFFFFFF) / (float)0x1000000; }

// ---------------------------------------------------------------------------
// Lamp silhouette
// ---------------------------------------------------------------------------
// Inner vessel radius at fluid height y (straight classic taper).
static float ll_vessel_r(float y) {
    float t = y / LL_FLUID_H;
    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    return 0.92f - 0.40f * t;
}

// ---------------------------------------------------------------------------
// Precomputed lamp geometry (static vertex/color arrays, built once)
// ---------------------------------------------------------------------------
#define SEG   30              // angular segments
#define VNR   13              // vessel profile rings
#define BNR   5               // base profile rings
#define CNR   4               // cap profile rings

static float g_cs[SEG + 1], g_sn[SEG + 1];

typedef struct { float x, y, z, r, g, b; } ll_vtx_t;

// ring strips: [ring][seg] vertex + color, drawn as QUADS between rings
static ll_vtx_t g_backdrop[VNR][SEG + 1];   // interior fluid backdrop
static ll_vtx_t g_rim[2][VNR][3];      // additive glass edge rims (billboards)
static ll_vtx_t g_streak[2][VNR][3];   // additive vertical reflection streaks
static ll_vtx_t g_base[BNR][SEG + 1];       // metal base
static ll_vtx_t g_cap[CNR][SEG + 1];        // metal cap

static const float BASE_Y[BNR] = { -1.06f, -0.92f, -0.55f, -0.18f, 0.02f };
static const float BASE_R[BNR] = {  0.60f,  0.98f,  1.10f,  1.00f, 0.93f };
static const float CAP_Y[CNR]  = { LL_FLUID_H + 0.00f, LL_FLUID_H + 0.16f,
                                   LL_FLUID_H + 0.42f, LL_FLUID_H + 0.66f };
static const float CAP_R[CNR]  = { 0.56f, 0.47f, 0.30f, 0.19f };

static float ll_clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Brushed-metal vertex color: angular diffuse + two baked specular streaks.
static void ll_metal_color(float th, float vfade, float *r, float *g, float *b) {
    float diff = cosf(th - 0.55f); if (diff < 0.0f) diff = 0.0f;
    float shade = 0.085f + 0.26f * diff;
    float s1 = cosf(th - 0.45f); if (s1 < 0.0f) s1 = 0.0f;
    s1 = powf(s1, 26.0f) * 0.55f;
    float s2 = cosf(th - 3.70f); if (s2 < 0.0f) s2 = 0.0f;
    s2 = powf(s2, 16.0f) * 0.22f;
    float m = (shade + s1 + s2) * vfade;
    *r = ll_clamp01(m * 0.86f + 0.03f);
    *g = ll_clamp01(m * 0.88f + 0.03f);
    *b = ll_clamp01(m * 0.96f + 0.04f);
}

// Fluid backdrop gradient: bulb-bright amber at the bottom of the column
// fading to deep red-brown at the top, dimmer toward the cylinder edges.
static void ll_fluid_color(float y, float edge, float *r, float *g, float *b) {
    float t = powf(ll_clamp01(y / LL_FLUID_H), 0.72f);
    float rr = 0.94f + (0.13f - 0.94f) * t;
    float gg = 0.46f + (0.025f - 0.46f) * t;
    float bb = 0.10f + (0.035f - 0.10f) * t;
    float fac = 0.55f + 0.45f * edge;
    *r = rr * fac; *g = gg * fac; *b = bb * fac;
}

static void ll_build_geometry(void) {
    for (int s = 0; s <= SEG; s++) {
        float th = (float)s * (2.0f * M_PI / (float)SEG);
        g_cs[s] = cosf(th);
        g_sn[s] = sinf(th);
    }

    // Vessel rings: backdrop uses the inner radius, glass sits just outside.
    for (int i = 0; i < VNR; i++) {
        float y = LL_FLUID_H * (float)i / (float)(VNR - 1);
        float ri = ll_vessel_r(y);
        float rg = ri + 0.055f;
        for (int s = 0; s <= SEG; s++) {
            float th = (float)s * (2.0f * M_PI / (float)SEG);
            // backdrop: brightness peaks at the far (-z) center as seen from +z
            float edge = -g_sn[s]; if (edge < 0.0f) edge = 0.0f;
            ll_vtx_t *bd = &g_backdrop[i][s];
            bd->x = ri * g_cs[s]; bd->y = y; bd->z = ri * g_sn[s];
            ll_fluid_color(y, edge, &bd->r, &bd->g, &bd->b);

        }
    }

    // Glass billboards: TinyGL smears near-degenerate screen triangles,
    // and a 3D shell's silhouette quads are exactly that (measured as
    // horizontal dash artifacts in the harness render). Screen-aligned
    // ribbons never degenerate: two edge rims hugging the profile plus two
    // vertical reflection streaks give the glass read without the shell.
    for (int i = 0; i < VNR; i++) {
        float y = LL_FLUID_H * (float)i / (float)(VNR - 1);
        float r = ll_vessel_r(y) + 0.055f;
        float tv = y / LL_FLUID_H;
        float vmod = 0.55f + 0.45f * sinf(tv * M_PI);
        for (int side = 0; side < 2; side++) {
            float sx = side ? 1.0f : -1.0f;
            ll_vtx_t *c0 = &g_rim[side][i][0], *c1 = &g_rim[side][i][1], *c2 = &g_rim[side][i][2];
            c0->x = sx * (r - 0.16f); c0->y = y; c0->z = 0.0f;
            c0->r = c0->g = c0->b = 0.0f;
            c1->x = sx * (r + 0.02f); c1->y = y; c1->z = 0.0f;
            c1->r = 0.10f * vmod; c1->g = 0.125f * vmod; c1->b = 0.150f * vmod;
            c2->x = sx * (r + 0.10f); c2->y = y; c2->z = 0.0f;
            c2->r = c2->g = c2->b = 0.0f;
        }
        float wob = 0.85f + 0.15f * sinf(y * 5.3f + 1.0f);
        float sA = 0.26f * vmod * wob;
        float sB = 0.10f * vmod;
        float cxA = -0.34f * r, wA = 0.09f * r;
        float cxB =  0.44f * r, wB = 0.06f * r;
        float zf = 0.62f * r;
        ll_vtx_t *a0 = &g_streak[0][i][0], *a1 = &g_streak[0][i][1], *a2 = &g_streak[0][i][2];
        a0->x = cxA - wA; a0->y = y; a0->z = zf; a0->r = a0->g = a0->b = 0.0f;
        a1->x = cxA;      a1->y = y; a1->z = zf; a1->r = sA; a1->g = sA; a1->b = sA * 0.95f;
        a2->x = cxA + wA; a2->y = y; a2->z = zf; a2->r = a2->g = a2->b = 0.0f;
        ll_vtx_t *b0 = &g_streak[1][i][0], *b1 = &g_streak[1][i][1], *b2 = &g_streak[1][i][2];
        b0->x = cxB - wB; b0->y = y; b0->z = zf; b0->r = b0->g = b0->b = 0.0f;
        b1->x = cxB;      b1->y = y; b1->z = zf; b1->r = sB; b1->g = sB; b1->b = sB * 0.95f;
        b2->x = cxB + wB; b2->y = y; b2->z = zf; b2->r = b2->g = b2->b = 0.0f;
    }

    for (int i = 0; i < BNR; i++) {
        float vf = 0.55f + 0.45f * ((float)i / (float)(BNR - 1));
        for (int s = 0; s <= SEG; s++) {
            float th = (float)s * (2.0f * M_PI / (float)SEG);
            ll_vtx_t *v = &g_base[i][s];
            v->x = BASE_R[i] * g_cs[s]; v->y = BASE_Y[i]; v->z = BASE_R[i] * g_sn[s];
            ll_metal_color(th, vf, &v->r, &v->g, &v->b);
            // warm bulb spill on the upper lip of the base
            if (i >= BNR - 2) { v->r = ll_clamp01(v->r + 0.10f); v->g = ll_clamp01(v->g + 0.045f); }
        }
    }
    for (int i = 0; i < CNR; i++) {
        float vf = 1.0f - 0.35f * ((float)i / (float)(CNR - 1));
        for (int s = 0; s <= SEG; s++) {
            float th = (float)s * (2.0f * M_PI / (float)SEG);
            ll_vtx_t *v = &g_cap[i][s];
            v->x = CAP_R[i] * g_cs[s]; v->y = CAP_Y[i]; v->z = CAP_R[i] * g_sn[s];
            ll_metal_color(th, vf, &v->r, &v->g, &v->b);
        }
    }

    for (int i = 0; i < GX; i++) g_gx[i] = X0 + (X1 - X0) * (float)i / (float)(GX - 1);
    for (int i = 0; i < GY; i++) g_gy[i] = Yb + (Yt - Yb) * (float)i / (float)(GY - 1);
    for (int i = 0; i < GZ; i++) g_gz[i] = Z0 + (Z1 - Z0) * (float)i / (float)(GZ - 1);
}

static void ll_strip_pass(ll_vtx_t rings[][SEG + 1], int nr, int flip, float k) {
    // strip centroid height, so the shell pass scales about the middle
    float cy = 0.5f * (rings[0][0].y + rings[nr - 1][0].y);
    glBegin(GL_QUADS);
    for (int i = 0; i < nr - 1; i++) {
        for (int s = 0; s < SEG; s++) {
            ll_vtx_t *a = &rings[i][s],     *b = &rings[i][s + 1];
            ll_vtx_t *c = &rings[i + 1][s + 1], *d = &rings[i + 1][s];
            if (flip) { ll_vtx_t *t = b; b = d; d = t; }
            glColor3f(a->r, a->g, a->b); glVertex3f(a->x * k, cy + (a->y - cy) * k, a->z * k);
            glColor3f(b->r, b->g, b->b); glVertex3f(b->x * k, cy + (b->y - cy) * k, b->z * k);
            glColor3f(c->r, c->g, c->b); glVertex3f(c->x * k, cy + (c->y - cy) * k, c->z * k);
            glColor3f(d->r, d->g, d->b); glVertex3f(d->x * k, cy + (d->y - cy) * k, d->z * k);
        }
    }
    glEnd();
}

static void ll_draw_strip(ll_vtx_t rings[][SEG + 1], int nr, int flip) {
    // Two passes, a 2.5% shell copy + the exact surface: TinyGL leaves
    // pinhole cracks along shared quad seams (measured with the flat-color
    // harness render), and a crack in either pass then reveals the
    // identically shaded other pass instead of whatever lies behind. The
    // shells are separated enough in depth that they never z-fight.
    ll_strip_pass(rings, nr, flip, 1.025f);
    ll_strip_pass(rings, nr, flip, 1.0f);
}

// Narrow 3-column billboard ribbon (glass rims / reflection streaks).
static void ll_draw_ribbon(ll_vtx_t rib[][3], int nr) {
    glBegin(GL_QUADS);
    for (int i = 0; i < nr - 1; i++) {
        for (int c = 0; c < 2; c++) {
            ll_vtx_t *a = &rib[i][c], *b = &rib[i][c + 1];
            ll_vtx_t *d = &rib[i + 1][c + 1], *e2 = &rib[i + 1][c];
            glColor3f(a->r, a->g, a->b); glVertex3f(a->x, a->y, a->z);
            glColor3f(b->r, b->g, b->b); glVertex3f(b->x, b->y, b->z);
            glColor3f(d->r, d->g, d->b); glVertex3f(d->x, d->y, d->z);
            glColor3f(e2->r, e2->g, e2->b); glVertex3f(e2->x, e2->y, e2->z);
        }
    }
    glEnd();
}

// Radial gradient disc (triangle fan), used for the bulb glare and the halo.
static void ll_draw_glow(float cx, float cy, float cz, float rad,
                         float r, float g, float b) {
    glBegin(GL_TRIANGLE_FAN);
    glColor3f(r, g, b);
    glVertex3f(cx, cy, cz);
    glColor3f(0.0f, 0.0f, 0.0f);
    for (int s = 0; s <= SEG; s++)
        glVertex3f(cx + rad * g_cs[s], cy + rad * g_sn[s], cz);
    glEnd();
}

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------
static void ll_reset_sim(void) {
    // blob 0: the molten pool over the bulb; risers pinch off its dome.
    g_bl[0].x = 0.0f; g_bl[0].y = -0.10f; g_bl[0].z = 0.0f;
    g_bl[0].vx = g_bl[0].vy = g_bl[0].vz = 0.0f;
    g_bl[0].R = 1.15f; g_bl[0].T = 1.0f; g_bl[0].sy = 0.50f;
    g_bl[0].ph = 0.0f; g_bl[0].wsp = 0.0f; g_bl[0].kmul = 1.0f; g_bl[0].pool = 1;

    for (int i = 1; i < LL_NBLOB; i++) {
        ll_blob_t *b = &g_bl[i];
        b->pool = 0;
        b->R  = 0.40f + llrndf() * 0.20f;
        // seed mid-cycle: spread over the column with matching temperatures
        b->y  = 0.25f + llrndf() * (LL_FLUID_H - 0.6f);
        float rmax = ll_vessel_r(b->y) - b->R * 0.72f;
        if (rmax < 0.05f) rmax = 0.05f;
        float a = llrndf() * 2.0f * M_PI, rr = llrndf() * rmax;
        b->x = rr * cosf(a); b->z = rr * sinf(a);
        b->vx = b->vy = b->vz = 0.0f;
        b->T  = 1.0f - b->y / LL_FLUID_H * (0.6f + 0.3f * llrndf());
        b->sy = 1.0f;
        b->ph = llrndf() * 2.0f * M_PI;
        b->wsp = 0.0018f + llrndf() * 0.0022f;
        b->kmul = 0.70f + llrndf() * 0.60f;
    }

    for (int i = 0; i < LL_NBUB; i++) {
        g_bub[i].x = (llrndf() * 2.0f - 1.0f) * 0.5f;
        g_bub[i].z = (llrndf() * 2.0f - 1.0f) * 0.5f;
        g_bub[i].y = llrndf() * LL_FLUID_H;
        g_bub[i].vy = 0.005f + llrndf() * 0.009f;
        g_bub[i].ph = llrndf() * 2.0f * M_PI;
    }
    g_t = 0.0f;
}

static void ll_sim_step(void) {
    for (int i = 1; i < LL_NBLOB; i++) {
        ll_blob_t *b = &g_bl[i];

        // Convection as a relaxation oscillator, the way the real lamp
        // works: strong heat exchange only near the bulb (bottom) and the
        // cool cap (top), near-adiabatic in between. A blob therefore
        // KEEPS its temperature while traversing the column, overshoots,
        // lingers, and turns around, instead of settling at the neutral
        // height (the overdamped hover the first cut of this model had).
        float tenv = (b->y < 1.0f) ? 1.05f : 0.12f;
        float k = 0.0090f * expf(-3.0f * b->y)
                + 0.0075f * expf(3.0f * (b->y - LL_FLUID_H))
                + 0.0003f;
        b->T += (tenv - b->T) * k * b->kmul;

        // buoyancy vs. slight per-blob neutral point, plus viscous drag
        float neutral = 0.50f + 0.06f * sinf(b->ph * 3.1f);
        b->vy += 0.00020f * (b->T - neutral);
        b->vy *= 0.988f;
        if (b->vy >  0.013f) b->vy =  0.013f;
        if (b->vy < -0.011f) b->vy = -0.011f;

        // slow horizontal wander (never periodic: per-blob phase and speed)
        b->ph += b->wsp;
        float reff = b->R * (0.90f + 0.22f * b->T);
        float rmax = ll_vessel_r(b->y) - reff * 0.68f;
        if (rmax < 0.04f) rmax = 0.04f;
        float txw = rmax * 0.62f * sinf(b->ph);
        float tzw = rmax * 0.62f * cosf(b->ph * 0.83f + 1.7f);
        b->vx += (txw - b->x) * 0.00035f;
        b->vz += (tzw - b->z) * 0.00035f;
        b->vx *= 0.982f; b->vz *= 0.982f;

        b->x += b->vx; b->y += b->vy; b->z += b->vz;

        // stay inside the tapered vessel
        float rd = sqrtf(b->x * b->x + b->z * b->z);
        if (rd > rmax && rd > 1e-5f) {
            float pull = (rd - rmax) * 0.10f / rd;
            b->x -= b->x * pull; b->z -= b->z * pull;
        }
        float ylo = 0.02f + reff * 0.30f;
        float yhi = LL_FLUID_H - reff * 0.55f;
        if (b->y < ylo) { b->y = ylo; if (b->vy < 0.0f) b->vy *= -0.25f; }
        if (b->y > yhi) { b->y = yhi; if (b->vy > 0.0f) b->vy *= -0.25f; }

        // vertical velocity stretches the blob into a teardrop
        float st = 1.0f + fabsf(b->vy) * 42.0f;
        if (st > 1.55f) st = 1.55f;
        b->sy += (st - b->sy) * 0.06f;

        // rare gentle kick so long-run motion never settles into a loop
        if ((llrnd() & 1023u) == 0u) {
            b->vx += (llrndf() - 0.5f) * 0.004f;
            b->vz += (llrndf() - 0.5f) * 0.004f;
        }
    }

    // the pool breathes a little with the bulb heat
    g_bl[0].R = 1.15f + 0.05f * sinf(g_t * 0.013f);

    for (int i = 0; i < LL_NBUB; i++) {
        ll_bub_t *u = &g_bub[i];
        u->y += u->vy;
        u->ph += 0.04f;
        u->x += 0.0016f * sinf(u->ph);
        if (u->y > LL_FLUID_H - 0.15f) {
            u->y = 0.10f;
            float rmax = 0.7f;
            u->x = (llrndf() * 2.0f - 1.0f) * rmax * 0.6f;
            u->z = (llrndf() * 2.0f - 1.0f) * rmax * 0.6f;
            u->vy = 0.005f + llrndf() * 0.009f;
        }
    }
    g_t += 1.0f;
}

// ---------------------------------------------------------------------------
// Metaball field + marching tetrahedra
// ---------------------------------------------------------------------------
static void ll_blob_derive(ll_blob_t *b) {
    float reff = b->pool ? b->R : b->R * (0.90f + 0.22f * b->T);
    float sy = b->sy;
    // volume-ish conservation for free blobs; the pool flattens vertically
    // only, so its dome can never widen through the vessel wall
    float sxz = b->pool ? 0.72f : 1.0f / sqrtf(sy);
    float axz = 1.0f / (reff * sxz);
    float ay  = 1.0f / (reff * sy);
    b->ax2 = axz * axz;
    b->ay2 = ay * ay;
    b->ext_xz = reff * sxz;
    b->ext_y  = reff * sy;
}

static int ll_gx_lo(float v) { int i = (int)((v - X0) / (X1 - X0) * (float)(GX - 1)); return i < 0 ? 0 : (i >= GX ? GX - 1 : i); }
static int ll_gy_lo(float v) { int i = (int)((v - Yb) / (Yt - Yb) * (float)(GY - 1)); return i < 0 ? 0 : (i >= GY ? GY - 1 : i); }
static int ll_gz_lo(float v) { int i = (int)((v - Z0) / (Z1 - Z0) * (float)(GZ - 1)); return i < 0 ? 0 : (i >= GZ ? GZ - 1 : i); }

static void ll_accumulate_field(int *bx0, int *bx1, int *by0, int *by1, int *bz0, int *bz1) {
    // zero the field (contiguous static array)
    float *f = &g_fld[0][0][0];
    for (long i = 0; i < (long)GY * GX * GZ; i++) f[i] = 0.0f;

    int ux0 = GX, ux1 = -1, uy0 = GY, uy1 = -1, uz0 = GZ, uz1 = -1;
    for (int n = 0; n < LL_NBLOB; n++) {
        ll_blob_t *b = &g_bl[n];
        ll_blob_derive(b);
        int x0 = ll_gx_lo(b->x - b->ext_xz), x1 = ll_gx_lo(b->x + b->ext_xz) + 1;
        int y0 = ll_gy_lo(b->y - b->ext_y),  y1 = ll_gy_lo(b->y + b->ext_y) + 1;
        int z0 = ll_gz_lo(b->z - b->ext_xz), z1 = ll_gz_lo(b->z + b->ext_xz) + 1;
        if (x1 >= GX) x1 = GX - 1; if (y1 >= GY) y1 = GY - 1; if (z1 >= GZ) z1 = GZ - 1;
        if (x0 < ux0) ux0 = x0; if (x1 > ux1) ux1 = x1;
        if (y0 < uy0) uy0 = y0; if (y1 > uy1) uy1 = y1;
        if (z0 < uz0) uz0 = z0; if (z1 > uz1) uz1 = z1;
        for (int iy = y0; iy <= y1; iy++) {
            float dy = g_gy[iy] - b->y;
            float qy = dy * dy * b->ay2;
            if (qy >= 1.0f) continue;
            for (int ix = x0; ix <= x1; ix++) {
                float dx = g_gx[ix] - b->x;
                float qxy = qy + dx * dx * b->ax2;
                if (qxy >= 1.0f) continue;
                float *row = &g_fld[iy][ix][0];
                for (int iz = z0; iz <= z1; iz++) {
                    float dz = g_gz[iz] - b->z;
                    float q = qxy + dz * dz * b->ax2;
                    if (q < 1.0f) {
                        float w = 1.0f - q;
                        row[iz] += w * w;
                    }
                }
            }
        }
    }
    *bx0 = ux0; *bx1 = ux1; *by0 = uy0; *by1 = uy1; *bz0 = uz0; *bz1 = uz1;
}

// Analytic field gradient + heat at a surface point -> shaded wax vertex.
// Fills sv with position, unit normal and final color; ll_emit_tri then
// emits every triangle twice (an inner shell displaced along the normal,
// then the exact surface), so a TinyGL seam crack in the outer pass
// reveals the like-shaded shell instead of the bright backdrop. Shading
// is computed once per vertex, so the second copy is nearly free.
typedef struct { float x, y, z, nx, ny, nz, r, g, b; } ll_wax_vtx_t;

static void ll_shade_wax_vertex(float x, float y, float z, ll_wax_vtx_t *sv) {
    float nx = 0.0f, ny = 0.0f, nz = 0.0f, wsum = 0.0f, tsum = 0.0f;
    for (int n = 0; n < LL_NBLOB; n++) {
        ll_blob_t *b = &g_bl[n];
        float dx = x - b->x, dy = y - b->y, dz = z - b->z;
        float q = dx * dx * b->ax2 + dy * dy * b->ay2 + dz * dz * b->ax2;
        if (q >= 1.0f) continue;
        float w = 1.0f - q;
        nx += w * dx * b->ax2; ny += w * dy * b->ay2; nz += w * dz * b->ax2;
        w *= w;
        wsum += w; tsum += w * b->T;
    }
    float nl = sqrtf(nx * nx + ny * ny + nz * nz);
    if (nl > 1e-6f) { nx /= nl; ny /= nl; nz /= nl; }
    else { nx = 0.0f; ny = 1.0f; nz = 0.0f; }
    // two-sided: a pinhole crack in the mesh then reveals a like-colored
    // back face instead of a contrasting dot (TinyGL seam artifact)
    if (nz < 0.0f) { nx = -nx; ny = -ny; nz = -nz; }
    float heatmix = (wsum > 1e-6f) ? tsum / wsum : 0.5f;   // blob temperature

    // bulb light (point light under the pool) with wrap diffuse: the lower
    // hemisphere gets direct amber light, the upper hemisphere a dimmer
    // transmitted glow, which is what sells the subsurface wax read.
    float bx = x, by = y + 0.34f, bz = z;
    float d2 = bx * bx + by * by + bz * bz;
    float inv = 1.0f / sqrtf(d2 + 1e-6f);
    float ndl = -(nx * bx + ny * by + nz * bz) * inv;
    float wrap = 0.5f * (ndl + 1.0f);
    float att = (0.65f + 0.5f * heatmix) * 1.75f / (1.0f + 0.60f * d2);
    float heat = wrap * att;
    float sss  = (1.0f - wrap) * att * 0.50f;

    float rr = 0.30f + heat * 0.85f + sss * 0.60f;
    float gg = 0.040f + heat * 0.36f + sss * 0.085f;
    float bb = 0.018f + heat * 0.065f + sss * 0.016f;

    // soft key light for form
    float kd = nx * 0.42f + ny * 0.66f + nz * 0.62f;
    if (kd < 0.0f) kd = 0.0f;
    rr += kd * 0.15f; gg += kd * 0.085f; bb += kd * 0.075f;

    // rim glow from the surrounding lit fluid
    float nv = nz; if (nv < 0.0f) nv = 0.0f;
    float rim = 1.0f - nv; rim = rim * rim * rim;
    rr += rim * 0.30f; gg += rim * 0.10f; bb += rim * 0.025f;

    // tight waxy specular from the key light half-vector
    float hd = nx * 0.24f + ny * 0.38f + nz * 0.89f;
    if (hd < 0.0f) hd = 0.0f;
    hd *= hd; hd *= hd; hd *= hd; hd *= hd;    // ^16
    rr += hd * 0.42f; gg += hd * 0.30f; bb += hd * 0.20f;

    sv->x = x; sv->y = y; sv->z = z;
    sv->nx = nx; sv->ny = ny; sv->nz = nz;
    sv->r = ll_clamp01(rr); sv->g = ll_clamp01(gg); sv->b = ll_clamp01(bb);
}

static void ll_edge_lerp(const float *va, const float *pa,
                         const float *vb, const float *pb, float *out) {
    float t = (LL_ISO - *va) / (*vb - *va);
    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    out[0] = pa[0] + t * (pb[0] - pa[0]);
    out[1] = pa[1] + t * (pb[1] - pa[1]);
    out[2] = pa[2] + t * (pb[2] - pa[2]);
}

// Emit one triangle unless it is (near-)degenerate: TinyGL scanline
// rasterization of sliver triangles can smear a whole horizontal run
// (measured as stray dash artifacts in the harness render), and marching
// tetrahedra naturally produces slivers where the isosurface grazes a
// grid vertex.
static void ll_emit_tri(const float *e0, const float *e1, const float *e2) {
    float ux = e1[0] - e0[0], uy = e1[1] - e0[1], uz = e1[2] - e0[2];
    float vx = e2[0] - e0[0], vy = e2[1] - e0[1], vz = e2[2] - e0[2];
    float cx = uy * vz - uz * vy;
    float cy = uz * vx - ux * vz;
    float cz = ux * vy - uy * vx;
    if (cx * cx + cy * cy + cz * cz < 1.0e-8f) return;
    ll_wax_vtx_t sv[3];
    ll_shade_wax_vertex(e0[0], e0[1], e0[2], &sv[0]);
    ll_shade_wax_vertex(e1[0], e1[1], e1[2], &sv[1]);
    ll_shade_wax_vertex(e2[0], e2[1], e2[2], &sv[2]);
    const float off = -0.045f;   // inner healing shell
    for (int k = 0; k < 3; k++) {
        glColor3f(sv[k].r, sv[k].g, sv[k].b);
        glVertex3f(sv[k].x + sv[k].nx * off,
                   sv[k].y + sv[k].ny * off,
                   sv[k].z + sv[k].nz * off);
    }
    for (int k = 0; k < 3; k++) {
        glColor3f(sv[k].r, sv[k].g, sv[k].b);
        glVertex3f(sv[k].x, sv[k].y, sv[k].z);
    }
}

// One tetrahedron: values v[4], positions p[4][3]. Emits 0, 1 or 2 tris.
static void ll_tetra(const float v[4], const float p[4][3]) {
    int in[4], n = 0;
    for (int i = 0; i < 4; i++) { in[i] = v[i] > LL_ISO; n += in[i]; }
    if (n == 0 || n == 4) return;

    int a[4], bq[4], na = 0, nb = 0;
    for (int i = 0; i < 4; i++) { if (in[i]) a[na++] = i; else bq[nb++] = i; }

    float e0[3], e1[3], e2[3], e3[3];
    if (n == 1 || n == 3) {
        int s = (n == 1) ? a[0] : bq[0];
        int o0 = (n == 1) ? bq[0] : a[0];
        int o1 = (n == 1) ? bq[1] : a[1];
        int o2 = (n == 1) ? bq[2] : a[2];
        ll_edge_lerp(&v[s], p[s], &v[o0], p[o0], e0);
        ll_edge_lerp(&v[s], p[s], &v[o1], p[o1], e1);
        ll_edge_lerp(&v[s], p[s], &v[o2], p[o2], e2);
        ll_emit_tri(e0, e1, e2);
    } else {  // n == 2: quad -> two triangles
        ll_edge_lerp(&v[a[0]], p[a[0]], &v[bq[0]], p[bq[0]], e0);
        ll_edge_lerp(&v[a[0]], p[a[0]], &v[bq[1]], p[bq[1]], e1);
        ll_edge_lerp(&v[a[1]], p[a[1]], &v[bq[1]], p[bq[1]], e2);
        ll_edge_lerp(&v[a[1]], p[a[1]], &v[bq[0]], p[bq[0]], e3);
        ll_emit_tri(e0, e1, e2);
        ll_emit_tri(e0, e2, e3);
    }
}

// 6 tetrahedra around the c0-c6 cube diagonal.
static const int LL_TET[6][4] = {
    {0,5,1,6}, {0,1,2,6}, {0,2,3,6}, {0,3,7,6}, {0,7,4,6}, {0,4,5,6},
};

static void ll_march(int x0, int x1, int y0, int y1, int z0, int z1) {
    if (x1 <= x0 || y1 <= y0 || z1 <= z0) return;
    // one-cell margin so surfaces never clip at the union bbox edge
    if (x0 > 0) x0--; if (y0 > 0) y0--; if (z0 > 0) z0--;
    if (x1 < GX - 1) x1++; if (y1 < GY - 1) y1++; if (z1 < GZ - 1) z1++;

    glBegin(GL_TRIANGLES);
    for (int iy = y0; iy < y1; iy++) {
        for (int ix = x0; ix < x1; ix++) {
            for (int iz = z0; iz < z1; iz++) {
                // cube corners c0..c7 (x+, then y+, standard ring order)
                float cv[8];
                cv[0] = g_fld[iy][ix][iz];
                cv[1] = g_fld[iy][ix + 1][iz];
                cv[2] = g_fld[iy + 1][ix + 1][iz];
                cv[3] = g_fld[iy + 1][ix][iz];
                cv[4] = g_fld[iy][ix][iz + 1];
                cv[5] = g_fld[iy][ix + 1][iz + 1];
                cv[6] = g_fld[iy + 1][ix + 1][iz + 1];
                cv[7] = g_fld[iy + 1][ix][iz + 1];
                int above = 0, below = 0;
                for (int k = 0; k < 8; k++) {
                    if (cv[k] > LL_ISO) above++; else below++;
                }
                if (above == 0 || below == 0) continue;   // cell not on surface

                float cp[8][3];
                float xa = g_gx[ix], xb2 = g_gx[ix + 1];
                float ya = g_gy[iy], yb2 = g_gy[iy + 1];
                float za = g_gz[iz], zb2 = g_gz[iz + 1];
                cp[0][0] = xa;  cp[0][1] = ya;  cp[0][2] = za;
                cp[1][0] = xb2; cp[1][1] = ya;  cp[1][2] = za;
                cp[2][0] = xb2; cp[2][1] = yb2; cp[2][2] = za;
                cp[3][0] = xa;  cp[3][1] = yb2; cp[3][2] = za;
                cp[4][0] = xa;  cp[4][1] = ya;  cp[4][2] = zb2;
                cp[5][0] = xb2; cp[5][1] = ya;  cp[5][2] = zb2;
                cp[6][0] = xb2; cp[6][1] = yb2; cp[6][2] = zb2;
                cp[7][0] = xa;  cp[7][1] = yb2; cp[7][2] = zb2;

                for (int t = 0; t < 6; t++) {
                    float tv[4]; float tp[4][3];
                    for (int k = 0; k < 4; k++) {
                        int ci = LL_TET[t][k];
                        tv[k] = cv[ci];
                        tp[k][0] = cp[ci][0]; tp[k][1] = cp[ci][1]; tp[k][2] = cp[ci][2];
                    }
                    ll_tetra(tv, tp);
                }
            }
        }
    }
    glEnd();
}

// ---------------------------------------------------------------------------
// Public entry points (called from gldemo.c)
// ---------------------------------------------------------------------------
void gldemo_lavalamp_setup(void) {
    glDisable(GL_TEXTURE_2D);
    glShadeModel(GL_SMOOTH);
    glClearColor(0.012f, 0.006f, 0.016f, 0.0f);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);
    ll_build_geometry();
    ll_reset_sim();
    g_ll_inited = 1;
}

void gldemo_lavalamp_frame(void) {
    if (!g_ll_inited) return;
    ll_sim_step();

    // Tight depth range around the lamp. The shared gldemo projection
    // (near 1.5, far 60) leaves TinyGL's 16-bit z-buffer too coarse here:
    // every grazing wax surface speckled against what sat just behind it
    // (measured in the harness render). The lamp occupies z 6.5..11, so
    // near 4.4 / far 15 spends the precision where the wax is.
    {
        float aspect = (float)gldemo_width() / (float)gldemo_height();
        float nh = 1.62f;
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glFrustum(-nh * aspect, nh * aspect, -nh, nh, 4.4, 15.0);
        glMatrixMode(GL_MODELVIEW);
    }

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -8.8f);
    // gentle camera drift so the lamp never reads as a still image
    glRotatef(2.0f * sinf(g_t * 0.0017f), 1.0f, 0.0f, 0.0f);
    glRotatef(7.0f * sinf(g_t * 0.0031f), 0.0f, 1.0f, 0.0f);
    glTranslatef(0.0f, -1.32f, 0.0f);

    // 1. ambient halo behind the lamp (additive, no depth)
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    ll_draw_glow(0.0f, 1.35f, -1.6f, 3.4f, 0.075f, 0.038f, 0.018f);
    ll_draw_glow(0.0f, 0.10f, -1.6f, 2.2f, 0.11f, 0.055f, 0.02f);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);

    // 2. opaque pass: metal base + cap (both faces: the far side heals
    //    TinyGL seam pinholes), then the fluid backdrop with back-face
    //    culling so only the far interior wall of the vessel renders.
    glDisable(GL_CULL_FACE);
    ll_draw_strip(g_base, BNR, 0);
    ll_draw_strip(g_cap, CNR, 0);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    ll_draw_strip(g_backdrop, VNR, 0);
    glDisable(GL_CULL_FACE);

    // (a bulb-glare TRIANGLE_FAN used to be drawn here; TinyGL rendered a
    // dark artifact tower at the fan center vertex, measured in the
    // harness, so the bulb glow lives in the backdrop gradient instead)

    // 3. the wax: metaball field -> marching tetrahedra isosurface
    int x0, x1, y0, y1, z0, z1;
    ll_accumulate_field(&x0, &x1, &y0, &y1, &z0, &z1);
    ll_march(x0, x1, y0, y1, z0, z1);

    // 4. additive overlays: bubbles (depth-tested so wax occludes them),
    //    then the glass rims + reflection streaks with depth off: those
    //    reflections sit ON the glass, in front of everything inside it.
    glEnable(GL_BLEND);
    glPointSize(2.0f);
    glBegin(GL_POINTS);
    for (int i = 0; i < LL_NBUB; i++) {
        float fade = sinf(g_bub[i].y / LL_FLUID_H * M_PI);
        glColor3f(0.30f * fade, 0.17f * fade, 0.07f * fade);
        glVertex3f(g_bub[i].x, g_bub[i].y, g_bub[i].z);
    }
    glEnd();

    glDisable(GL_DEPTH_TEST);
    ll_draw_ribbon(g_rim[0], VNR);
    ll_draw_ribbon(g_rim[1], VNR);
    ll_draw_ribbon(g_streak[0], VNR);
    ll_draw_ribbon(g_streak[1], VNR);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}
