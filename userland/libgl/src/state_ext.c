/* state_ext.c - AssaultCube port phase 2 TinyGL gap-fill: glDepthFunc,
 * glAlphaFunc, glScissor, glGetString. See docs/ASSAULTCUBE_PORT_PLAN.md
 * and userland/apps/assaultcube/PORT-STATUS.md for what "real" means for
 * each of these (some are exact, some are documented approximations; none
 * are silent no-ops).
 *
 * glOrtho lives in matrix.c (next to glFrustum). glTexSubImage2D lives in
 * texture.c (next to glTexImage2D). Both for locality with the code they
 * extend. This file holds the state-only additions that did not have an
 * obvious existing home.
 *
 * No em-dashes per repo writing-style rule.
 */

#include "zgl.h"
#include "msghandling.h"

/* ---------------------------------------------------------------------
 * glDepthFunc: real. Was previously impossible to call at all (TinyGL's
 * depth test was hardcoded to "z >= zpix" everywhere in ztriangle.c). Now
 * zb->depth_func is consulted by zb_depth_test() (ztriangle.c), which
 * implements all eight standard GL compare funcs. Default is GL_GEQUAL
 * (init.c), reproducing the old hardcoded behavior for any app that never
 * calls this, so this is backward compatible by construction.
 * ------------------------------------------------------------------- */
void glDepthFunc(GLenum func) {
	GLParam p[2];
#include "error_check_no_context.h"
	p[0].op = OP_DepthFunc;
	p[1].i = func;
	gl_add_op(p);
}

void glopDepthFunc(GLParam* p) {
	GLContext* c = gl_get_context();
	c->zb->depth_func = p[1].i;
}

/* ---------------------------------------------------------------------
 * glAlphaFunc: real, but primitive-granularity, not per-fragment. See
 * clip.c gl_draw_triangle() for the actual test (applied once per triangle
 * using each vertex's color alpha; the whole triangle is rejected only if
 * every vertex fails). TinyGL's software rasterizer has no per-fragment
 * alpha channel to test against, and its textures carry no alpha channel
 * at all (texture.c only ever accepts GL_RGB / 3 components), so a true
 * per-texel GL alpha test (the common "cutout leaves/grass" use case) is
 * NOT implemented here: this state is stored and enforced at the vertex
 * level only. Enable/disable is GL_ALPHA_TEST in misc.c glopEnableDisable.
 * ------------------------------------------------------------------- */
void glAlphaFunc(GLenum func, GLclampf ref) {
	GLParam p[3];
#include "error_check_no_context.h"
	p[0].op = OP_AlphaFunc;
	p[1].i = func;
	p[2].f = ref;
	gl_add_op(p);
}

void glopAlphaFunc(GLParam* p) {
	GLContext* c = gl_get_context();
	c->alpha_test_func = p[1].i;
	c->alpha_test_ref = p[2].f;
}

/* ---------------------------------------------------------------------
 * glScissor: real, but whole-primitive bounding-box rejection, not exact
 * per-pixel clipping. See clip.c gl_draw_triangle() for the actual test:
 * a triangle fully outside the scissor rect never draws; a triangle that
 * straddles the scissor boundary still draws in full (including the part
 * outside the rect). Enable/disable is GL_SCISSOR_TEST in misc.c
 * glopEnableDisable. Coordinates are window-space, same convention as
 * glViewport (origin at the window's own (0,0), matching c->viewport).
 * ------------------------------------------------------------------- */
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
	GLParam p[5];
#include "error_check_no_context.h"
	p[0].op = OP_Scissor;
	p[1].i = x;
	p[2].i = y;
	p[3].i = width;
	p[4].i = height;
	gl_add_op(p);
}

void glopScissor(GLParam* p) {
	GLContext* c = gl_get_context();
	c->scissor_x = p[1].i;
	c->scissor_y = p[2].i;
	c->scissor_w = p[3].i;
	c->scissor_h = p[4].i;
}

/* ---------------------------------------------------------------------
 * glGetString: real, static strings. Not routed through the op/display-
 * list system (a query, like the rest of get.c, must be answerable
 * immediately even mid-list-compile; matches glGetIntegerv/glGetFloatv's
 * existing immediate-execution convention in get.c).
 * ------------------------------------------------------------------- */
static const GLubyte TGL_VENDOR_STRING[]     = "MayteraOS";
static const GLubyte TGL_RENDERER_STRING[]   = "TinyGL software rasterizer (#319)";
static const GLubyte TGL_VERSION_STRING[]    = "1.1 TinyGL";
static const GLubyte TGL_EXTENSIONS_STRING[] = ""; /* none advertised: multitexture, fog, stencil, clip planes are stubs, not real extensions */
static const GLubyte TGL_UNKNOWN_STRING[]    = "";

const GLubyte* glGetString(GLenum name) {
	switch (name) {
	case GL_VENDOR:     return TGL_VENDOR_STRING;
	case GL_RENDERER:   return TGL_RENDERER_STRING;
	case GL_VERSION:    return TGL_VERSION_STRING;
	case GL_EXTENSIONS: return TGL_EXTENSIONS_STRING;
	default:
		tgl_warning("glGetString: unsupported name 0x%X\n", (unsigned)name);
		return TGL_UNKNOWN_STRING;
	}
}
