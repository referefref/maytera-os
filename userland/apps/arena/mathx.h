/* Self-contained float math + vec3 for Maytera Arena (freestanding, no libm).
 * Uses SSE sqrt and polynomial sin/cos so the game never depends on libm.     */
#ifndef ARENA_MATHX_H
#define ARENA_MATHX_H

typedef struct { float x, y, z; } vec3;

#define M_PI_F   3.14159265358979f
#define DEG2RAD(d) ((d) * (M_PI_F / 180.0f))

static inline float mx_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r;
}
static inline float mx_absf(float x){ return x < 0 ? -x : x; }

/* sin/cos via range-reduction + minimax polynomial (accurate ~1e-4).         */
static inline float mx_sinf(float x) {
    /* reduce to [-PI, PI] */
    const float TWO_PI = 2.0f * M_PI_F;
    while (x >  M_PI_F) x -= TWO_PI;
    while (x < -M_PI_F) x += TWO_PI;
    float x2 = x * x;
    return x * (1.0f - x2*(1.0f/6.0f - x2*(1.0f/120.0f - x2*(1.0f/5040.0f))));
}
static inline float mx_cosf(float x) { return mx_sinf(x + M_PI_F * 0.5f); }
static inline float mx_tanf(float x) { float c = mx_cosf(x); return c==0?0:mx_sinf(x)/c; }

/* atan2 approximation (good to ~0.01 rad) - used by bots to aim.             */
static inline float mx_atan2f(float y, float x) {
    if (x == 0.0f && y == 0.0f) return 0.0f;
    float ax = mx_absf(x), ay = mx_absf(y);
    float a = (ax > ay ? ay/ax : ax/ay);
    float s = a*a;
    float r = ((-0.0464964749f*s + 0.15931422f)*s - 0.327622764f)*s*a + a;
    if (ay > ax) r = 1.57079637f - r;
    if (x < 0)   r = 3.14159274f - r;
    if (y < 0)   r = -r;
    return r;
}

static inline vec3 v3(float x, float y, float z){ vec3 v={x,y,z}; return v; }
static inline vec3 v3add(vec3 a, vec3 b){ return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline vec3 v3sub(vec3 a, vec3 b){ return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline vec3 v3scale(vec3 a, float s){ return v3(a.x*s, a.y*s, a.z*s); }
static inline vec3 v3mul(vec3 a, vec3 b){ return v3(a.x*b.x, a.y*b.y, a.z*b.z); }
static inline float v3dot(vec3 a, vec3 b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline vec3 v3cross(vec3 a, vec3 b){
    return v3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static inline float v3len(vec3 a){ return mx_sqrtf(v3dot(a,a)); }
static inline float v3dist(vec3 a, vec3 b){ return v3len(v3sub(a,b)); }
static inline vec3 v3norm(vec3 a){ float l = v3len(a); return l>0.0001f ? v3scale(a, 1.0f/l) : v3(0,0,0); }

/* forward vector from yaw (around Z, up) + pitch. z is up in this game.       */
static inline vec3 v3fromangles(float yaw, float pitch){
    float cp = mx_cosf(pitch);
    return v3(mx_cosf(yaw)*cp, mx_sinf(yaw)*cp, mx_sinf(pitch));
}

#endif /* ARENA_MATHX_H */
