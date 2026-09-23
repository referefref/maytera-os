#include "zgl.h"

void glopClearColor(GLParam* p) {
	GLContext* c = gl_get_context();
	c->clear_color.v[0] = p[1].f;
	c->clear_color.v[1] = p[2].f;
	c->clear_color.v[2] = p[3].f;
	c->clear_color.v[3] = p[4].f;
}
void glopClearDepth(GLParam* p) {
	GLContext* c = gl_get_context();
	c->clear_depth = p[1].f;
}

void glopClear(GLParam* p) {
	GLContext* c = gl_get_context();
	GLint mask = p[1].i;
	/* #594 fix (was: "GLint z = 0;" with a "TODO : correct value of Z"
	 * comment left by the original TinyGL author, i.e. this was a KNOWN,
	 * never-finished bug, not something #582 or this session introduced).
	 * A hardcoded 0 clear value is only correct for the ORIGINAL hardcoded
	 * ">=" (GL_GEQUAL) depth compare this codebase shipped with for years
	 * (AssaultCube phase 2, ztriangle.c: zb->depth_func defaults to
	 * GL_GEQUAL specifically so callers that never touch glDepthFunc stay
	 * byte-identical to that old hardcoded behavior) - under GEQUAL
	 * ("pass if incoming z >= existing"), clearing to the MINIMUM (0) is
	 * correct: the first fragment at any pixel has z >= 0 and passes.
	 *
	 * OpenArena's renderer (renderer_oa/tr_backend.c) explicitly calls
	 * glDepthFunc(GL_LEQUAL) for real world/entity geometry - the OPPOSITE
	 * convention ("pass if incoming z <= existing", smaller-z-is-nearer,
	 * the standard OpenGL default). With the buffer cleared to 0 (the
	 * MINIMUM), a LEQUAL test needs incoming z <= 0, which a real
	 * (non-negative, non-zero) interpolated depth value essentially never
	 * satisfies - so EVERY fragment's per-pixel depth test fails, even
	 * though the rasterizer's outer scanline span is wide open and the
	 * PUT_PIXEL inner loop runs normally. MEASURED (#594, this session,
	 * VM 2611, instrumented ztriangle.c/ZB_oaTriDumpAndReset): thousands
	 * of triangles/frame entering the rasterizer, ~95-98% of them running
	 * the inner pixel loop ("drew"), ZERO rejected by the #582 bounds
	 * clamp, yet zb->pbuf measured 0 nonzero pixels on nearly every frame
	 * from frame ~100 through frame ~6440 of a live oa_dm1 session - this
	 * is that exact mechanism, not a rasterizer or clamp bug.
	 *
	 * Fix: pick the clear value from the ACTIVE depth_func's pass
	 * direction, not a single constant baked in for one convention.
	 * GL_LESS/GL_LEQUAL (smaller-is-nearer) need the buffer cleared to
	 * the FAR end (the zbuf storage's real max - zbuf entries are
	 * GLushort, so 0xFFFF, matching what PUT_PIXEL's `z >>
	 * ZB_POINT_Z_FRAC_BITS` produces and compares against). Every other
	 * compare direction (GEQUAL/GREATER/NOTEQUAL/ALWAYS/NEVER/EQUAL) keeps
	 * the original 0, so every existing TinyGL app that never calls
	 * glDepthFunc (gears, the texture/model demos, AssaultCube's own
	 * default state) is byte-identical to before this change. */
	GLint z = (c->zb->depth_func == GL_LESS || c->zb->depth_func == GL_LEQUAL) ? 0xFFFF : 0;
	GLint r = (GLint)(c->clear_color.v[0] * COLOR_MULT_MASK);
	GLint g = (GLint)(c->clear_color.v[1] * COLOR_MULT_MASK);
	GLint b = (GLint)(c->clear_color.v[2] * COLOR_MULT_MASK);

	ZB_clear(c->zb, mask & GL_DEPTH_BUFFER_BIT, z, mask & GL_COLOR_BUFFER_BIT, r, g, b);
}
