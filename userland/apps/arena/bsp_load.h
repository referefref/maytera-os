/* bsp_load.h - C side of the #491 Stage 1 GoldSrc BSP v30 import.
 *
 * Two layers:
 *   1. The RAW Rust FFI surface (bsp_parse / bsp_free + the #[repr(C)] structs),
 *      mirrored here EXACTLY. The _Static_assert()s below lock the struct sizes
 *      to the same const asserts in bsp.rs, so neither side can silently drift.
 *      This FFI is INTEGER / BIT-PATTERN ONLY: vertex coords + texinfo vectors
 *      cross as raw u32 IEEE-754 bit patterns (see f32b() in bsp_load.c); Rust
 *      does no float math (the Stage 0 soft-float caveat).
 *   2. The Arena integration API (bsp_load_file / bsp_is_active /
 *      bsp_draw_faces / bsp_get_spawn / bsp_get_bounds), implemented in
 *      bsp_load.c and called by main.c + render.c.
 */
#ifndef ARENA_BSP_LOAD_H
#define ARENA_BSP_LOAD_H

#include <stdint.h>
#include "mathx.h"
/* #491 Stage 2: bsp_hull_sweep() below returns a `Trace` (game.h's shared
 * trace-result type, same convention as map_trace_ray/map_move_entity), so
 * this header needs game.h's declaration too. game.h does NOT include this
 * header back (no cycle) and is idempotent (its own include guard).          */
#include "game.h"

/* ---- raw Rust FFI (mirror of bsp.rs; keep in lockstep) -------------------- */
typedef struct { uint32_t x, y, z; } BspVec3;          /* raw f32 bit patterns  */

typedef struct {
    uint32_t first_vertex;
    uint32_t num_vertices;
    int32_t  tex_id;
    uint32_t s_vec[4];       /* texinfo vecs[0] as f32 bit patterns             */
    uint32_t t_vec[4];       /* texinfo vecs[1] as f32 bit patterns             */
    uint32_t tex_w, tex_h;
} BspFace;

typedef struct {
    uint32_t width, height;
    uint32_t pixel_offset;   /* offset into the ARGB pixel pool (u32 units)     */
    uint32_t has_pixels;
} BspTexture;

/* #491 Stage 2: hull-trace collision types (mirror bsp.rs BspPlane/BspClipnode
 * exactly). Consumed only via bsp_hull_trace()/bsp_hull_box() below; C never
 * walks these arrays directly (the recursive trace stays entirely in Rust). */
typedef struct {
    BspVec3 normal;   /* raw f32 bit patterns */
    uint32_t dist;    /* raw f32 bit pattern  */
    int32_t  ptype;   /* plane axis-type: 0/1/2 axis-aligned, >=3 general      */
} BspPlane;

typedef struct {
    int32_t planenum;
    int32_t children[2];  /* negative = CONTENTS_* leaf, not a further index    */
} BspClipnode;

typedef struct {
    BspVec3    *verts;
    BspFace    *faces;
    BspTexture *textures;
    uint32_t   *pixels;      /* 0xAARRGGBB pool                                 */
    uint8_t    *entities;    /* raw entity text lump                            */
    uint32_t    num_verts;
    uint32_t    num_faces;
    uint32_t    num_textures;
    uint32_t    num_pixels;
    uint32_t    entities_len;
    BspVec3     spawn;
    uint32_t    has_spawn;
    int32_t     error;       /* 0 = ok; nonzero = clean parse error (see bsp.rs)*/
    /* #491 Stage 2 fields, appended at the end (mirrors bsp.rs's own append). */
    BspPlane    *planes;
    BspClipnode *clipnodes;
    uint32_t     num_planes;
    uint32_t     num_clipnodes;
    int32_t      hull_headnode[4];  /* per-hull clipnode-tree root, model 0     */
    uint32_t     hull_ok;           /* 1 = real collision data present          */
} BspScene;

/* Lock the ABI: identical to the const asserts in bsp.rs. */
_Static_assert(sizeof(BspVec3)     == 12, "BspVec3 size drift vs bsp.rs");
_Static_assert(sizeof(BspFace)     == 52, "BspFace size drift vs bsp.rs");
_Static_assert(sizeof(BspTexture)  == 16, "BspTexture size drift vs bsp.rs");
_Static_assert(sizeof(BspPlane)    == 20, "BspPlane size drift vs bsp.rs");
_Static_assert(sizeof(BspClipnode) == 12, "BspClipnode size drift vs bsp.rs");
_Static_assert(sizeof(BspScene)    == 128, "BspScene size drift vs bsp.rs");

/* Implemented in Rust (bsp.rs). data/wad may be NULL with len 0. Returns a heap
 * scene to be released with bsp_free(); NEVER reads out of bounds on a hostile
 * input (returns error != 0 instead). */
BspScene *bsp_parse(const uint8_t *data, unsigned long len,
                    const uint8_t *wad,  unsigned long wad_len);
void      bsp_free(BspScene *scene);

/* #491 Stage 2: the recursive clipnode hull trace, implemented ENTIRELY in
 * Rust (bsp.rs, SV_RecursiveHullCheck shape). p1/p2 are HULL-ORIGIN-space raw
 * f32 bit patterns (see bsp_hull_box() for the feet<->origin offset); hull
 * selects model 0's headnode[0..3]. Returns 0 on success (result in *out), a
 * negative code if no usable clip data / bad hull index exists (caller must
 * not trust *out then). */
typedef struct {
    uint32_t frac;        /* raw f32 bit pattern, 0..1                        */
    BspVec3  end;          /* raw f32 bit pattern, hull-origin space           */
    BspVec3  normal;        /* raw f32 bit pattern, impact plane normal        */
    uint32_t start_solid;  /* 1 if p1 itself was embedded in solid             */
    uint32_t all_solid;    /* 1 if the WHOLE segment stayed in solid           */
} HullTrace;
_Static_assert(sizeof(HullTrace) == 36, "HullTrace size drift vs bsp.rs");

int32_t bsp_hull_trace(const BspScene *scene, int32_t hull,
                       uint32_t p1x, uint32_t p1y, uint32_t p1z,
                       uint32_t p2x, uint32_t p2y, uint32_t p2z,
                       HullTrace *out);

/* The fixed GoldSrc/Quake hull box half-extents (NOT stored in the file; see
 * bsp.rs bsp_hull_box() doc comment). Returns 0 on success, -1 for hull<0||>3.*/
int32_t bsp_hull_box(int32_t hull, BspVec3 *out_mins, BspVec3 *out_maxs);

/* ---- Arena integration (bsp_load.c) -------------------------------------- */
/* Load a BSP file from disk (and optional external WAD), decode + upload its
 * textures, and build the C-side draw list (computes per-vertex UVs from the
 * texinfo vectors, HARDWARE float, C-side). Returns 1 on success, 0 on any
 * failure (missing file, parse error). Safe to call repeatedly (reloads).     */
int  bsp_load_file(const char *bsp_path, const char *wad_path);

/* 1 once a BSP map is loaded AND selected as the active world. render.c draws
 * BSP faces instead of box brushes while this is set. */
int  bsp_is_active(void);
void bsp_set_active(int on);
int  bsp_available(void);          /* 1 if a map is loaded (may be inactive)    */

/* Draw the loaded BSP faces as textured triangles in the current TinyGL scene
 * (own glBegin/glEnd, binds each face's texture). No-op if nothing loaded. */
void bsp_draw_faces(void);

/* Player spawn (from info_player_* in the entity text, else map centre) and the
 * world AABB (from the parsed vertices). Fill z-up world-space vec3s.          */
void bsp_get_spawn(vec3 *out);
void bsp_get_bounds(vec3 *out_mins, vec3 *out_maxs);

/* #491 Stage 3: the map's FULL info_player_start/-deathmatch set. bsp_get_spawn()
 * above remains the single ENTRY spawn (== index 0); these expose the whole set
 * so the level's spawns[]/nspawn table (and therefore pick_spawn() and every
 * RESPAWN) is driven by the map's own entities instead of the stale coordinates
 * of whichever box level was loaded before. Positions are FEET-space
 * (BSP_SPAWN_Z_ADJ applied), yaw is radians CCW from +X. bsp_get_spawn_n()
 * returns 0 for an out-of-range index and leaves the outputs untouched. */
int  bsp_get_spawn_count(void);
int  bsp_get_spawn_n(int i, vec3 *out_pos, float *out_yaw);

/* #491 Stage 2: hull-trace collision (world.c dispatches to these when a BSP
 * map is the active world). All feet<->hull-origin conversion + f32<->bits
 * packing happens HERE in C (hardware float), never in Rust (see bsp.rs file
 * header). Arena's player box (radius 16, height 56) is mapped onto GoldSrc
 * HULL 1 (standing player, 32x32x72): the XY half-extent matches Arena's
 * PLAYER_RADIUS exactly (16), so wall/doorway clearance is exact; Z does not
 * match any stock hull (56 vs hull1's 72 / hull3's 36) and hull 1 was picked
 * as the closer AND safer of the two (a taller probe cannot let the player
 * fit through a gap real GoldSrc geometry would block, whereas an
 * under-tall probe could clip into low ceilings) - see bsp_load.c. */
int  bsp_hull_available(void);     /* 1 if the loaded map has real clip data    */
/* Sweep the mapped hull from feet_start to feet_end (both FEET-space, Arena's
 * normal convention). Returns a Trace exactly like map_trace_ray/-move's own
 * convention: frac 0..1, hit 1 if blocked before reaching feet_end, normal at
 * the impact plane, end = the resulting feet position, hit_entity = -1. */
Trace bsp_hull_sweep(vec3 feet_start, vec3 feet_end);
/* True if the mapped hull is solid at this FEET position (zero-length sweep). */
int  bsp_hull_point_solid(vec3 feet);

#endif /* ARENA_BSP_LOAD_H */
