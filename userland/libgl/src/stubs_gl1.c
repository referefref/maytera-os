/* stubs_gl1.c - TinyGL entry points the OpenArena port (task #568) needs
 * that stubs_ac.c (the AssaultCube-era gap file) does not cover. Same
 * policy as that file: every function here is a deliberate, documented
 * choice (real forwarding wrapper where TinyGL already has the underlying
 * primitive, honest no-op where it genuinely does not), not a guess.
 *
 * glTexEnvf is a REAL implementation (not a stub): TinyGL's api.c already
 * implements glTexEnvi for the exact same GL_TEXTURE_ENV state, so the
 * float variant is a correct, no-precision-loss forward (every value Q3's
 * renderer passes through this call is a small integer-valued GL enum like
 * GL_MODULATE/GL_REPLACE, never a fractional float).
 *
 * glDepthRange/glPushClientAttrib/glPopClientAttrib are honest no-ops:
 * TinyGL has a single fixed depth range and no vertex-array attribute
 * stack. glGetBooleanv is a real-but-narrow implementation: it only
 * answers GL_COLOR_WRITEMASK correctly (matching this build's glColorMask,
 * itself already a documented no-op in stubs_ac.c: color writes are never
 * masked, so reporting all four channels as GL_TRUE is the actually-true
 * answer, not a fabricated one); any other pname reports GL_FALSE rather
 * than silently returning uninitialized memory.
 *
 * No em-dashes per repo writing-style rule.
 */
#include "zgl.h"

void glTexEnvf(GLenum target, GLenum pname, GLfloat param) {
	glTexEnvi((GLint)target, (GLint)pname, (GLint)param);
}

void glDepthRange(double zNear, double zFar) {
	(void)zNear; (void)zFar;
}

void glGetBooleanv(GLenum pname, GLboolean *params) {
	if (!params) return;
	if (pname == GL_COLOR_WRITEMASK) {
		params[0] = params[1] = params[2] = params[3] = 1; /* GL_TRUE */
		return;
	}
	params[0] = 0; /* GL_FALSE: no other pname is tracked */
}

void glPushClientAttrib(unsigned int mask) {
	(void)mask;
}

void glPopClientAttrib(void) {
}

/* REAL: same rationale as glTexEnvf - TinyGL's glTexParameteri already
 * implements every GL_TEXTURE_2D pname the renderer sets through the float
 * entry point (GL_TEXTURE_MIN_FILTER/MAG_FILTER/WRAP_S/WRAP_T/
 * MAX_ANISOTROPY_EXT/COMPARE_MODE, all integer-valued enums). */
void glTexParameterf(GLenum target, GLenum pname, GLfloat param) {
	glTexParameteri((GLint)target, (GLint)pname, (GLint)param);
}

/* STUB: GL_TEXTURE_BORDER_COLOR (a 4-float RGBA array, the only pname this
 * port's renderer ever passes here, see tr_image.c's R_CreateFogImage) has
 * no TinyGL equivalent - TinyGL's texture wrap modes do not support
 * GL_CLAMP_TO_BORDER at all. No-op: the fog edge texel uses TinyGL's
 * regular clamp-to-edge behavior instead of a true border color, a
 * cosmetic-only limitation (visible only as a slightly different fog edge
 * pixel, not a crash or wrong geometry). */
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params) {
	(void)target; (void)pname; (void)params;
}

/* STUB: no stencil buffer at all (same as the pre-existing
 * glStencilFunc/glStencilOp no-ops in stubs_ac.c). */
void glStencilMask(unsigned int mask) {
	(void)mask;
}
