/* Maytera Arena - world art extras (agent 4, world_art.c).
 *
 * sky_face() itself is declared in polish.h (the contract). This header adds
 * the OPTIONAL extras the integrator may use if present:
 *   - per-theme fog + ambient colour grading
 *   - environmental decal sprites (blood splats, scorch marks, smoke, sparks)
 *
 * Decals deliberately do NOT reuse the fx_sprite() name: that symbol belongs
 * to characters.c (agent 2). If both exist the integrator can prefer either.
 */
#ifndef ARENA_WORLD_ART_H
#define ARENA_WORLD_ART_H

#include "polish.h"

/* Suggested fog colour for the theme, 0x00RRGGBB. Matches the horizon band
 * baked into the staged /ARENA/SKY/SKYn.BMP panoramas, so geometry fog and
 * the sky meet seamlessly. */
unsigned int world_fog_rgb(int theme);

/* Ambient light tint for the theme, 0x00RRGGBB (multiply against surface
 * colours; 0xFFFFFF = neutral). Gives each arena a distinct colour grade. */
unsigned int world_ambient_rgb(int theme);

/* Environmental decal / particle sprites (procedural, lazily baked, cached).
 * All are 64x64 0xAARRGGBB with real alpha cut-outs. `frame` selects the
 * variant (blood/scorch) or the animation frame (smoke/spark). Out-of-range
 * frames clamp; returns NULL only on allocation failure. */
enum {
    WA_DECAL_BLOOD  = 0,   /* frames = random splat variants               */
    WA_DECAL_SCORCH = 1,   /* frames = scorch mark variants                */
    WA_DECAL_SMOKE  = 2,   /* frames = expanding, fading puff animation    */
    WA_DECAL_SPARK  = 3,   /* frames = bright star flash, decaying         */
    WA_DECAL_KINDS  = 4
};
const Image *decal_sprite(int kind, int frame);
int          decal_frames(int kind);

#endif /* ARENA_WORLD_ART_H */
