/* gfxtest.h - offscreen ClassiCube graphics-backend harness (#28, gfx lane).
 *
 * Proves the software rasteriser renders CORRECTLY into a plain 32bpp buffer
 * we own, with NO window, NO compositor and NO VM. Every check is an exact
 * pixel-value comparison against a value derived from the scene, never an
 * eyeball of a dumped image. The BMP dump exists so a human can look, but the
 * pass/fail verdict does not depend on anyone looking.
 */
#ifndef GFXTEST_H
#define GFXTEST_H

#include "Core.h"
#include "Bitmap.h"

#define GT_WIDTH  640
#define GT_HEIGHT 480

struct gt_results {
	int checks_run;
	int checks_failed;
	int frames_2d;
	int frames_3d;
};

/* Runs the whole scene suite. Returns 0 if every check passed. */
int  gt_run(struct gt_results* res);

/* The colour buffer the rasteriser wrote into (packed, GT_WIDTH stride). */
BitmapCol* gt_framebuffer(void);

/* Re-renders the 2D conformance frame only (for a separate BMP dump). */
void gt_render_2d_frame(void);

/* Renders `frames` rotating-cube frames for timing. Returns frames drawn. */
int  gt_render_3d_frames(int frames);

/* Fill-rate benchmark: `overdraw` screen-filling textured depth-tested quads
 * per frame, through the same 3D path the world uses. */
int  gt_render_fill_frames(int frames, int overdraw);

/* Provided by the platform-specific main: single line of output. */
void gt_log(const char* msg);

/* Offscreen window backing store (tests/gfxtest/gfxtest_support.c). */
void gt_offscreen_reset(void);
int  gt_present_count(void);

#endif /* GFXTEST_H */
