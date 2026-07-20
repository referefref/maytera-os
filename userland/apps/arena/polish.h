/* Maytera Arena - GRAPHICS POLISH contract. Replaces the boxy programmer-art
 * with real (open-source, CC0/GPL) textures + sprites.
 *
 * SPLIT so 5 agents never edit the same file:
 *   textures.c   - CC0 wall/floor/ceiling/sky textures        (tx_*)      [agent 1]
 *   characters.c - CC0 enemy + item + projectile + fx sprites (ch_/it_/pj_/fx_) [agent 2]
 *   weapons_art.c- CC0 first-person weapon viewmodel sprites   (wv_*)      [agent 3]
 *   world_art.c  - CC0 skybox + decals/blood/smoke + colour    (sky_/decal)[agent 4]
 *   r_polish.c + render.c edits - the ACTUAL rendering that consumes the
 *                  above (img_bind_gl/billboard_draw/screen_sprite) and the
 *                  integration + build.                        [agent 5, integrator]
 *
 * Asset delivery: SMALL images may be baked as `static const unsigned int[]`
 * pixel arrays inside your .c. LARGE sets SHOULD ship as image FILES under
 * /ARENA/ on the disk and be lazy-decoded via the kernel image decoder
 * (SYS_DECODE_IMAGE, see how apps/paint/imgio.c or apps/files use it). If you
 * ship files, LIST them (name + source URL + licence) in your final report so
 * the integrator can add them to the boot image. Everything must be CC0 or a
 * GPL-compatible licence; record attribution in an ASSET_CREDITS block.
 */
#ifndef ARENA_POLISH_H
#define ARENA_POLISH_H
#include "game.h"

/* A decoded image. px points at w*h pixels, 0xAARRGGBB (alpha in the top byte;
 * alpha 0 == fully transparent, used for sprite cut-outs). Storage is owned by
 * the asset module (static / lazily-decoded cache). Returns NULL if missing. */
typedef struct { int w, h; const unsigned int *px; } Image;

/* -------- asset packs (agents 1-4 implement these; agent 5 only CALLS them) -- */
/* Level textures. `theme` = level index 0..NUM_LEVELS-1 (pick a fitting set per
 * level); `id` selects among a few variants. Power-of-two sizes preferred. */
const Image *tx_wall (int theme, int id);
const Image *tx_floor(int theme, int id);
const Image *tx_ceil (int theme, int id);
const Image *tx_sky  (int theme);

/* Characters/enemies as Doom-style billboards. skin 0..N-1 (map from
 * players[].color or player_slot); state: 0 idle,1 run,2 fire,3 pain,4 die;
 * angle 0..7 = enemy facing relative to the viewer (0 = facing you); frame =
 * animation frame within the state. */
const Image *ch_frame(int skin, int state, int angle, int frame);
int          ch_state_frames(int state);

/* Pickups, projectiles, effects (all billboards). */
const Image *it_sprite(int item_kind, int value);   /* IT_* (+weapon id for IT_WEAPON) */
const Image *pj_sprite(int proj_kind, int frame);   /* PROJ_* */
const Image *fx_sprite(int fx_id, int frame);       /* 0 explosion,1 blood,2 smoke,3 spark,... */

/* First-person weapon viewmodels (2D screen sprites). weapon = W_*; frame 0 =
 * idle, 1..wv_fire_frames-1 = firing animation. */
const Image *wv_sprite(int weapon, int frame);
int          wv_fire_frames(int weapon);

/* Skybox faces (agent 4). face 0..5 (+X,-X,+Y,-Y,+Z,-Z) or a single panorama at
 * face 0; return NULL for faces you don't provide. */
const Image *sky_face(int theme, int face);

/* -------- render helpers (agent 5 implements in r_polish.c) ------------------ */
/* Upload `im` as a GL texture (cached by pointer identity) and bind it. */
void img_bind_gl(const Image *im);
/* Draw `im` as a camera-facing textured quad of world height `size` at worldpos,
 * inside the current TinyGL 3D scene (alpha-tested). `tint` multiplies colour. */
void billboard_draw(const Image *im, vec3 worldpos, float size, unsigned int tint);
/* Blit `im` (alpha-composited) into the ARGB `blit` buffer at (x,y), integer
 * `scale`. Used for the weapon viewmodel + 2D HUD art. */
void screen_sprite(const Image *im, unsigned int *blit, int bw, int bh,
                   int x, int y, int scale);
/* Lazy-decode + cache an image file from disk (/ARENA/...). Native path for
 * uncompressed 24/32-bit BMP; magenta (0xFF00FF) is the transparency color key.
 * Returns NULL if the file is missing (callers fall back gracefully).         */
const Image *pol_load_bmp(const char *path);

/* Runtime flag: integrator sets this if assets loaded OK; if 0, render.c falls
 * back to the original boxy drawing so the game never ships broken. */
extern int g_polish_ready;

#endif /* ARENA_POLISH_H */
