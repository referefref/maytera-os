/* bsp_load.c - Maytera Arena: C integration of the #491 Stage 1 Rust GoldSrc
 * BSP v30 parser (bsp.rs). This layer:
 *   - reads the .bsp (and optional external .wad) from disk into a buffer,
 *   - calls the Rust bsp_parse() (which does ALL untrusted-input bounds
 *     checking; a hostile file returns error != 0, never an OOB),
 *   - computes per-vertex texture UVs from the texinfo vectors HERE, in C, with
 *     hardware SSE float (the Rust side passes only raw f32 bit patterns, so no
 *     soft-float ABI is ever crossed - the Stage 0 float caveat),
 *   - uploads each decoded texture through the existing polish img_bind_gl path
 *     (Image px pointer identity cache), and
 *   - draws the faces as textured triangles in the current TinyGL scene, a NEW
 *     render path parallel to draw_brushes (it does NOT use the 256-Brush cap).
 *
 * Freestanding: uses the shared libc (malloc/free/sys_open/sys_read/strtof).
 * No busy waits, no blocking beyond the one-shot file reads at load time.
 */
#include "game.h"
#include "polish.h"      /* Image + img_bind_gl                                 */
#include "bsp_load.h"
#include "stdlib.h"      /* O_RDONLY, malloc/free, strtof                       */
#include <GL/gl.h>

/* reinterpret a raw 32-bit pattern as an IEEE-754 float (the C side of the
 * integer-only FFI). This is the ONLY place map floats come to life.          */
static inline float f32b(uint32_t u) {
    union { uint32_t i; float f; } c;
    c.i = u;
    return c.f;
}
/* #491 Stage 2: the inverse - pack a hardware float back to its raw bit
 * pattern to cross the FFI into bsp_hull_trace(). */
static inline uint32_t b32f(float f) {
    union { uint32_t i; float f; } c;
    c.f = f;
    return c.i;
}

typedef struct { float x, y, z, u, v; } BspDrawVert;

static BspScene    *g_scene   = 0;
static BspDrawVert *g_dv      = 0;    /* one per face-vertex (== num_verts)      */
static Image       *g_tex_img = 0;    /* one Image per texture (into the pool)   */
static int          g_loaded  = 0;
static int          g_active  = 0;
static vec3         g_spawn   = {0, 0, 64};
static vec3         g_mins    = {-64, -64, 0};
static vec3         g_maxs    = { 64,  64, 128};
/* #491 Stage 3: the map's FULL info_player_* set (see collect_spawns). Capped at
 * game.h's MAX_SPAWNS because that is the table these ultimately fill; de_dust2
 * carries 40 and the first 32 are kept. Positions are FEET-space (BSP_SPAWN_Z_ADJ
 * already applied), yaw radians.                                              */
static vec3         g_spawns[MAX_SPAWNS];
static float        g_spawn_yaw[MAX_SPAWNS];
static int          g_nspawn = 0;

/* -------------------------------------------------- whole-file reader (heap) */
/* Reads an entire file into a freshly malloc'd buffer. *out_len gets the size.
 * Returns the buffer (caller frees) or NULL. Grows geometrically; bounded by a
 * hard cap so a bogus/huge file cannot exhaust the user heap.                  */
#define BSP_MAX_FILE (48u * 1024u * 1024u)
/* GoldSrc maps are multi-MB and are read into a fresh heap buffer. The kernel's
 * sys_read copy-to-user does NOT fault in user pages that are mapped but not yet
 * present (libc's heap is grown with one big sys_mmap and backed lazily): the
 * copy is silently dropped for those pages while sys_read still returns the full
 * byte count. The result is a buffer of the right LENGTH full of ZEROS, which the
 * BSP parser then correctly rejects as degenerate (error 6) -- de_dust2 loaded
 * "fine" at 2 MB yet produced 0 faces. Touching each page from Ring 3 first makes
 * it present, after which the kernel's copy lands. A read-modify-write through a
 * volatile pointer faults the page in for WRITING without disturbing any bytes
 * already read into the buffer. (Kernel-side fix tracked separately; this is the
 * userland-side guard.)                                                        */
static void prefault_pages(unsigned char *p, unsigned long from, unsigned long to) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    for (unsigned long i = from; i < to; i += 4096u) v[i] = v[i];
    if (to > from) v[to - 1] = v[to - 1];
}

static unsigned char *read_whole_file(const char *path, unsigned long *out_len) {
    *out_len = 0;
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned long cap = 65536, len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) { sys_close(fd); return 0; }
    prefault_pages(buf, 0, cap);

    for (;;) {
        if (len == cap) {
            if (cap >= BSP_MAX_FILE) { free(buf); sys_close(fd); return 0; }
            unsigned long ncap = cap * 2;
            if (ncap > BSP_MAX_FILE) ncap = BSP_MAX_FILE;
            unsigned char *nb = (unsigned char *)realloc(buf, ncap);
            if (!nb) { free(buf); sys_close(fd); return 0; }
            buf = nb;
            /* realloc may hand back a fresh, never-touched block (or absorb an
             * untouched neighbour), so re-arm the whole capacity. */
            prefault_pages(buf, 0, ncap);
            cap = ncap;
        }
        long n = sys_read(fd, buf + len, cap - len);
        if (n < 0) { free(buf); sys_close(fd); return 0; }
        if (n == 0) break;                 /* EOF                               */
        len += (unsigned long)n;
    }
    sys_close(fd);
    *out_len = len;
    return buf;
}

/* --------------------------------------------------- entity-text spawn parse */
/* Collect EVERY info_player_start / info_player_deathmatch entity in the raw
 * entity text and read each one's "origin" "x y z" (+ facing). GoldSrc origins
 * are integers/decimals in world units. If no spawn is found the map centre
 * (computed from bounds) is used instead.
 *
 * #491 Stage 3: this was parse_spawn(), which stopped at the FIRST match and
 * filled only the single g_spawn that arena_start_bsp() uses to place the player
 * on ENTRY. That sufficed while nothing else read a spawn, but arena_start_bsp
 * deliberately spawns a bot, so the player dies, and death goes mode_update ->
 * mode_respawn_player -> spawn_player_entity -> pick_spawn, which reads
 * level.spawns[]/nspawn, NOT g_spawn. Those were left holding the previously
 * loaded BOX level's coordinates (arena_start_bsp zeroed nbrush/nprop/nitem but
 * not nspawn), so the first respawn teleported the player to a Longest-Yard
 * coordinate embedded in dust2's geometry, which looks exactly like the Stage 2
 * collision fix having failed. The whole set is collected here so main.c can
 * hand pick_spawn the map's own spawns.                                        */
static int find_sub(const char *hay, unsigned long hlen, unsigned long from,
                    const char *needle) {
    unsigned long nl = 0; while (needle[nl]) nl++;
    if (nl == 0 || hlen < nl) return -1;
    for (unsigned long i = from; i + nl <= hlen; i++) {
        unsigned long j = 0;
        while (j < nl && hay[i + j] == needle[j]) j++;
        if (j == nl) return (int)i;
    }
    return -1;
}

/* Read the quoted value of `key` (which INCLUDES its own quotes, e.g. "\"origin\"")
 * inside the entity block [lo,hi). Copies it out because the lump is not
 * NUL-terminated and strtof needs a bounded string. Returns 1 on success.
 * Note the closing quote in `key` is load-bearing: it is what stops a lookup of
 * "angle" from matching an "angles" key. */
static int ent_value(const char *ents, unsigned long lo, unsigned long hi,
                     const char *key, char *out, unsigned long outcap) {
    int k = find_sub(ents, hi, lo, key);
    if (k < 0) return 0;
    unsigned long kl = 0; while (key[kl]) kl++;
    unsigned long p = (unsigned long)k + kl;
    while (p < hi && ents[p] != '"') p++;    /* opening quote of the value       */
    if (p >= hi) return 0;
    p++;
    unsigned long n = 0;
    while (p < hi && ents[p] != '"' && n + 1 < outcap) out[n++] = ents[p++];
    out[n] = 0;
    return 1;
}

static void collect_spawns(const char *ents, unsigned long elen) {
    /* #491 Stage 2: a GoldSrc info_player_* "origin" IS the player's hull
     * ORIGIN, but Arena's Entity.pos (and g_spawn, which feeds it) is the
     * player's FEET (render.c puts the eye at pos.z + R_EYE_HEIGHT). The
     * conversion is therefore origin -> feet = origin + hull1 mins.z = -36.
     *
     * This was +24 through Stage 1, when g_spawn only ever positioned a NOCLIP
     * FREE-FLY CAMERA ("lift a touch so the eye is inside") and nothing ever
     * collided with it. Stage 1's own comment says as much. Feeding that same
     * eye-space lift into Stage 2's feet-space collider placed every player 60
     * units too high; hull_test/dust2_test.c measures that against de_dust2's
     * own 40 info_player_* origins and finds +24 embeds 4 of the 40 (the two
     * 544,*,-64 and two -928,*,176 spawns) INSIDE geometry, while -36 embeds
     * 0/40. Keep this in sync with hull_test/dust2_test.c's SPAWN_Z_ADJ.      */
#define BSP_SPAWN_Z_ADJ (-36.0f)
    g_nspawn = 0;
    unsigned long i = 0;
    while (i < elen && g_nspawn < MAX_SPAWNS) {
        while (i < elen && ents[i] != '{') i++;      /* block start             */
        if (i >= elen) break;
        unsigned long lo = i, hi = i;
        while (hi < elen && ents[hi] != '}') hi++;
        if (hi >= elen) break;                       /* unterminated: stop      */

        /* Match the classname VALUE (quoted) so a key/targetname that merely
         * contains the substring cannot masquerade as a spawn. dust2 writes the
         * block as origin, angles, classname, so the whole block is searched. */
        if (find_sub(ents, hi, lo, "\"info_player_start\"") >= 0 ||
            find_sub(ents, hi, lo, "\"info_player_deathmatch\"") >= 0) {
            char val[64];
            if (ent_value(ents, lo, hi, "\"origin\"", val, sizeof val)) {
                char *end = val;
                float x = strtof(end, &end);
                float y = strtof(end, &end);
                float z = strtof(end, &end);
                /* Facing: de_dust2's info_player_* carry "angles" "pitch yaw roll"
                 * (yaw is the SECOND value, verified against the map's own entity
                 * lump: 44 "angles" keys, 1 "angle"); the older single-value
                 * "angle" "yaw" form is accepted as a fallback for maps using it.
                 * Arena's yaw is radians CCW from +X and GoldSrc's is degrees CCW
                 * from +X, so this is a straight deg->rad conversion. Facing only
                 * decides which way a fresh spawn LOOKS, never where it stands. */
                float yaw = 0.0f;
                char av[64];
                if (ent_value(ents, lo, hi, "\"angles\"", av, sizeof av)) {
                    char *ae = av;
                    (void)strtof(ae, &ae);           /* pitch (discarded)       */
                    yaw = strtof(ae, &ae) * (M_PI_F / 180.0f);
                } else if (ent_value(ents, lo, hi, "\"angle\"", av, sizeof av)) {
                    char *ae = av;
                    yaw = strtof(ae, &ae) * (M_PI_F / 180.0f);
                }
                g_spawns[g_nspawn]    = v3(x, y, z + BSP_SPAWN_Z_ADJ);
                g_spawn_yaw[g_nspawn] = yaw;
                g_nspawn++;
            }
        }
        i = hi + 1;
    }
    /* Entry spawn stays the map's first info_player_*, exactly as before.       */
    if (g_nspawn > 0) g_spawn = g_spawns[0];
}

/* ----------------------------------------------------------- bounds from verts */
static void compute_bounds(void) {
    if (!g_scene || g_scene->num_verts == 0) return;
    float minx = 1e30f, miny = 1e30f, minz = 1e30f;
    float maxx = -1e30f, maxy = -1e30f, maxz = -1e30f;
    for (uint32_t i = 0; i < g_scene->num_verts; i++) {
        float x = g_dv[i].x, y = g_dv[i].y, z = g_dv[i].z;
        if (x < minx) minx = x; if (x > maxx) maxx = x;
        if (y < miny) miny = y; if (y > maxy) maxy = y;
        if (z < minz) minz = z; if (z > maxz) maxz = z;
    }
    /* pad so the noclip player never clamps exactly onto a wall */
    g_mins = v3(minx - 32.0f, miny - 32.0f, minz - 32.0f);
    g_maxs = v3(maxx + 32.0f, maxy + 32.0f, maxz + 32.0f);
}

/* ----------------------------------------------------------------- teardown */
static void bsp_unload(void) {
    if (g_scene) { bsp_free(g_scene); g_scene = 0; }
    if (g_dv)      { free(g_dv);      g_dv = 0; }
    if (g_tex_img) { free(g_tex_img); g_tex_img = 0; }
    g_loaded = 0;
    g_active = 0;
}

/* --------------------------------------------------------------- public load */
int bsp_load_file(const char *bsp_path, const char *wad_path) {
    bsp_unload();

    unsigned long blen = 0, wlen = 0;
    unsigned char *bbuf = read_whole_file(bsp_path, &blen);
    if (!bbuf) return 0;
    unsigned char *wbuf = wad_path ? read_whole_file(wad_path, &wlen) : 0;

    BspScene *sc = bsp_parse(bbuf, blen, wbuf, wlen);
    /* The Rust parser copies everything it needs into its own heap allocation,
     * so the file buffers can be released now regardless of outcome.           */
    free(bbuf);
    if (wbuf) free(wbuf);

    if (!sc || sc->error != 0 || sc->num_faces == 0 || sc->num_verts == 0) {
        if (sc) bsp_free(sc);
        return 0;
    }
    g_scene = sc;

    /* Build the C draw list: reinterpret vertex bit patterns to float and
     * compute per-vertex UVs from each face's texinfo vectors (hardware float). */
    g_dv = (BspDrawVert *)malloc((size_t)sc->num_verts * sizeof(BspDrawVert));
    if (!g_dv) { bsp_unload(); return 0; }

    for (uint32_t fi = 0; fi < sc->num_faces; fi++) {
        BspFace *f = &sc->faces[fi];
        float sx = f32b(f->s_vec[0]), sy = f32b(f->s_vec[1]);
        float sz = f32b(f->s_vec[2]), sd = f32b(f->s_vec[3]);
        float tx = f32b(f->t_vec[0]), ty = f32b(f->t_vec[1]);
        float tz = f32b(f->t_vec[2]), td = f32b(f->t_vec[3]);
        float iw = (f->tex_w > 0) ? 1.0f / (float)f->tex_w : 1.0f / 64.0f;
        float ih = (f->tex_h > 0) ? 1.0f / (float)f->tex_h : 1.0f / 64.0f;
        uint32_t start = f->first_vertex;
        for (uint32_t k = 0; k < f->num_vertices; k++) {
            uint32_t vi = start + k;
            if (vi >= sc->num_verts) break;    /* defensive (Rust guarantees)   */
            BspVec3 *bv = &sc->verts[vi];
            float x = f32b(bv->x), y = f32b(bv->y), z = f32b(bv->z);
            g_dv[vi].x = x; g_dv[vi].y = y; g_dv[vi].z = z;
            g_dv[vi].u = (x * sx + y * sy + z * sz + sd) * iw;
            g_dv[vi].v = (x * tx + y * ty + z * tz + td) * ih;
        }
    }

    /* Build one Image per texture, aliasing the Rust ARGB pixel pool. */
    if (sc->num_textures > 0) {
        g_tex_img = (Image *)malloc((size_t)sc->num_textures * sizeof(Image));
        if (!g_tex_img) { bsp_unload(); return 0; }
        for (uint32_t i = 0; i < sc->num_textures; i++) {
            BspTexture *t = &sc->textures[i];
            if (t->has_pixels && t->width > 0 && t->height > 0 &&
                (unsigned long)t->pixel_offset + (unsigned long)t->width * t->height
                    <= sc->num_pixels) {
                g_tex_img[i].w = (int)t->width;
                g_tex_img[i].h = (int)t->height;
                g_tex_img[i].px = (const unsigned int *)(sc->pixels + t->pixel_offset);
            } else {
                g_tex_img[i].w = 0; g_tex_img[i].h = 0; g_tex_img[i].px = 0;
            }
        }
    }

    compute_bounds();
    /* default spawn = map centre, then override from entity text if present */
    g_spawn = v3((g_mins.x + g_maxs.x) * 0.5f,
                 (g_mins.y + g_maxs.y) * 0.5f,
                 (g_mins.z + g_maxs.z) * 0.5f);
    g_nspawn = 0;                 /* a reload must not inherit the last map's set */
    if (sc->entities && sc->entities_len > 0)
        collect_spawns((const char *)sc->entities, sc->entities_len);

    g_loaded = 1;
    return 1;
}

int  bsp_available(void) { return g_loaded; }
int  bsp_is_active(void) { return g_loaded && g_active; }
void bsp_set_active(int on) { if (g_loaded) g_active = on ? 1 : 0; }

void bsp_get_spawn(vec3 *out) { if (out) *out = g_spawn; }
int  bsp_get_spawn_count(void) { return g_nspawn; }
int  bsp_get_spawn_n(int i, vec3 *out_pos, float *out_yaw) {
    if (i < 0 || i >= g_nspawn) return 0;
    if (out_pos) *out_pos = g_spawns[i];
    if (out_yaw) *out_yaw = g_spawn_yaw[i];
    return 1;
}
void bsp_get_bounds(vec3 *out_mins, vec3 *out_maxs) {
    if (out_mins) *out_mins = g_mins;
    if (out_maxs) *out_maxs = g_maxs;
}

/* ---------------------------------------------------------------- draw path */
static inline void emit_dv(const BspDrawVert *d, int textured) {
    if (textured) glTexCoord2f(d->u, d->v);
    glVertex3f(d->x, d->y, d->z);
}

void bsp_draw_faces(void) {
    if (!g_loaded || !g_scene || !g_dv) return;
    glDisable(GL_CULL_FACE);          /* BSP faces are single-sided; show both  */
    glEnable(GL_DEPTH_TEST);
    for (uint32_t fi = 0; fi < g_scene->num_faces; fi++) {
        BspFace *f = &g_scene->faces[fi];
        uint32_t n = f->num_vertices;
        if (n < 3) continue;
        if ((unsigned long)f->first_vertex + n > g_scene->num_verts) continue;

        int textured = 0;
        if (f->tex_id >= 0 && (uint32_t)f->tex_id < g_scene->num_textures &&
            g_tex_img && g_tex_img[f->tex_id].px) {
            img_bind_gl(&g_tex_img[f->tex_id]);
            glColor3f(1.0f, 1.0f, 1.0f);
            textured = 1;
        } else {
            img_bind_gl(0);           /* no texture: flat mid-grey wall         */
            glColor3f(0.60f, 0.60f, 0.66f);
        }

        const BspDrawVert *base = &g_dv[f->first_vertex];
        glBegin(GL_TRIANGLES);        /* fan-triangulate the convex polygon     */
        for (uint32_t k = 1; k + 1 < n; k++) {
            emit_dv(&base[0], textured);
            emit_dv(&base[k], textured);
            emit_dv(&base[k + 1], textured);
        }
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);
}

/* ======================================================================== */
/* #491 Stage 2: hull-trace collision (world.c dispatches to these when a BSP
 * map is the active world - see bsp_is_active()). The recursive clipnode walk
 * itself lives entirely in Rust (bsp.rs bsp_hull_trace); this layer only does
 * the feet<->hull-origin conversion and f32<->bit-pattern packing, in
 * hardware float, per the Stage-0/1 integer-only-FFI rule.
 *
 * HULL CHOICE: Arena's player box is radius 16 (half-extent X/Y), height 56
 * (Z, feet-to-head). GoldSrc's stock hulls are point(0)/32x32x72(1)/
 * 64x64x64(2)/32x32x36(3) - see bsp_hull_box()'s doc comment. Arena's XY
 * EXACTLY matches hull 1 (and hull 3)'s +-16 half-extent; Z (56) matches
 * neither hull exactly, but is closer to hull 1's 72 (|72-56|=16) than hull
 * 3's 36 (|56-36|=20). Hull 1 is used: besides being numerically closer, it
 * is also the SAFER mismatch direction - tracing a slightly TALLER box than
 * Arena's actual player cannot let the player fit through a gap the real
 * GoldSrc geometry (and any human playing the source game) would consider
 * too tight, whereas tracing too SHORT a box could let the player's model
 * visibly clip into a low ceiling the hull reports as clear. This is a
 * disclosed approximation, not a silent rescale of the map: the world
 * geometry itself is untouched, only which of the map's OWN precomputed clip
 * hulls we query against.                                                    */
#define ARENA_HULL 1

int bsp_hull_available(void) {
    return g_loaded && g_scene && g_scene->hull_ok;
}

/* feet (Arena convention: pos.z is the FEET, the box spans [z, z+height]) ->
 * hull-origin (GoldSrc convention: the traced point is the box's own local
 * origin, with the box defined by bsp_hull_box()'s mins/maxs RELATIVE to it -
 * for hull 1, mins.z=-36/maxs.z=36, so the origin sits at mid-height). x/y
 * need no offset: Arena's pos is already the box's horizontal center, which
 * matches hull 1's symmetric +-16 exactly. */
static vec3 feet_to_origin(vec3 feet) {
    BspVec3 mn;
    bsp_hull_box(ARENA_HULL, &mn, 0);
    return v3(feet.x, feet.y, feet.z - f32b(mn.z));   /* mins.z is negative: this LIFTS  */
}
static vec3 origin_to_feet(vec3 origin) {
    BspVec3 mn;
    bsp_hull_box(ARENA_HULL, &mn, 0);
    return v3(origin.x, origin.y, origin.z + f32b(mn.z));
}

/* Sweep the mapped hull from feet_start to feet_end (Arena's normal
 * feet-space convention). No usable clip data (bsp_hull_available()==0, or
 * the underlying trace call itself reports an error) is reported as a clean
 * miss (frac=1, hit=0) - the caller (world.c) then falls back to whatever it
 * does when nothing is solid, never a crash or an undefined position.        */
Trace bsp_hull_sweep(vec3 feet_start, vec3 feet_end) {
    Trace tr;
    tr.frac = 1.0f; tr.hit = 0; tr.end = feet_end; tr.normal = v3(0, 0, 0); tr.hit_entity = -1;
    if (!bsp_hull_available()) return tr;

    vec3 o1 = feet_to_origin(feet_start);
    vec3 o2 = feet_to_origin(feet_end);

    HullTrace ht;
    int32_t rc = bsp_hull_trace(g_scene, ARENA_HULL,
                                b32f(o1.x), b32f(o1.y), b32f(o1.z),
                                b32f(o2.x), b32f(o2.y), b32f(o2.z),
                                &ht);
    if (rc != 0) return tr;   /* no usable clip data: clean miss, see above    */

    float frac = f32b(ht.frac);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    tr.frac = frac;
    tr.hit  = (frac < 1.0f) ? 1 : 0;
    tr.end  = origin_to_feet(v3(f32b(ht.end.x), f32b(ht.end.y), f32b(ht.end.z)));
    tr.normal = v3(f32b(ht.normal.x), f32b(ht.normal.y), f32b(ht.normal.z));
    tr.hit_entity = -1;
    return tr;
}

/* True if the mapped hull is solid at this FEET position (a zero-length
 * sweep; the underlying recursive trace handles p1==p2 correctly - it just
 * walks straight to the containing leaf, no plane-crossing split needed).    */
int bsp_hull_point_solid(vec3 feet) {
    if (!bsp_hull_available()) return 0;
    vec3 o = feet_to_origin(feet);
    HullTrace ht;
    int32_t rc = bsp_hull_trace(g_scene, ARENA_HULL,
                                b32f(o.x), b32f(o.y), b32f(o.z),
                                b32f(o.x), b32f(o.y), b32f(o.z),
                                &ht);
    if (rc != 0) return 0;
    return ht.start_solid ? 1 : 0;
}
