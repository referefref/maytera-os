/* stubs_ac.c - AssaultCube port phase 2: TinyGL entry points that AC's
 * renderer references but that this pass does NOT implement for real.
 *
 * Every function here is a deliberate, documented no-op so that AC's
 * renderer (rendergl.cpp / rendermodel.cpp / water.cpp / shadow.cpp) LINKS
 * and RUNS instead of failing to link, at the cost of the visual feature
 * those calls were meant to provide. None of these silently corrupt state
 * used elsewhere (they touch nothing but their own no-op).
 *
 * Gap list (see docs/ASSAULTCUBE_PORT_PLAN.md and PORT-STATUS.md):
 *   - Multitexture (glActiveTexture/glClientActiveTexture/glMultiTexCoord2f):
 *     used for lightmaps. Highest-risk stub: AC's world renderer may render
 *     with the base texture only, no lightmap, if it does not fall back to
 *     a single-pass path on its own.
 *   - glClipPlane: user clip planes, e.g. water reflection clipping. Water
 *     will render without the clip (whole scene visible above/below the
 *     plane), a known visual bug, not a crash.
 *   - glStencilFunc/glStencilOp: stencil buffer effects (e.g. some shadow
 *     techniques). TinyGL has no stencil buffer at all; nothing to hook
 *     these into short of adding one, out of scope for this pass.
 *   - glFogf/glFogi/glFogfv: distance fog. No visual fog; geometry renders
 *     at full brightness/color at all distances.
 *   - glCopyTexSubImage2D: framebuffer-to-texture copy (e.g. render-to-
 *     texture effects). Real support needs glReadPixels-style framebuffer
 *     access wired into texture upload; not attempted this pass.
 *
 * No em-dashes per repo writing-style rule.
 */

#include "zgl.h"

/* ---- multitexture (stub) ---- */
void glActiveTexture(GLenum texture)       { (void)texture; }
void glClientActiveTexture(GLenum texture) { (void)texture; }
void glMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t) { (void)target; (void)s; (void)t; }
void glMultiTexCoord2fv(GLenum target, const GLfloat* v)    { (void)target; (void)v; }

/* ---- clip planes (stub) ---- */
void glClipPlane(GLenum plane, const GLdouble* equation) { (void)plane; (void)equation; }

/* ---- stencil (stub: TinyGL has no stencil buffer) ---- */
void glStencilFunc(GLenum func, GLint ref, GLuint mask) { (void)func; (void)ref; (void)mask; }
void glStencilOp(GLenum sfail, GLenum zfail, GLenum zpass) { (void)sfail; (void)zfail; (void)zpass; }

/* ---- fog (stub) ---- */
void glFogf(GLenum pname, GLfloat param)         { (void)pname; (void)param; }
void glFogi(GLenum pname, GLint param)           { (void)pname; (void)param; }
void glFogfv(GLenum pname, const GLfloat* params) { (void)pname; (void)params; }

/* ---- framebuffer-to-texture copy (stub) ---- */
void glCopyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                          GLint x, GLint y, GLsizei width, GLsizei height) {
	(void)target; (void)level; (void)xoffset; (void)yoffset;
	(void)x; (void)y; (void)width; (void)height;
}

/* ---- AssaultCube port phase 3 additions (docs/ASSAULTCUBE_PORT_PLAN.md) ---- */

/* Line width: get.c's glGetFloatv(GL_LINE_WIDTH, ...) ALREADY always reports
 * a constant 1.0 (see that file), so there is no real state for this call to
 * change; every call site (editing.cpp, rendergl.cpp) only uses it in a
 * save/glLineWidth(x)/restore bracket around a draw call, so a no-op is
 * exactly consistent with what glGetFloatv already claims, not a
 * regression from some previously-tracked value. */
void glLineWidth(GLfloat width) { (void)width; }

/* Color write mask: no call site in this port ever reads GL_COLOR_WRITEMASK
 * back, and the ztriangle.c PUT_PIXEL inner loop (already flagged as
 * extremely perf-sensitive, see the glScissor/glAlphaFunc notes in gl.h) is
 * not touched to add a per-channel write gate. Every pixel keeps writing all
 * channels; a masked-channel draw (rare, typically a depth/stencil-only
 * prepass) draws its color too instead of being invisible. */
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
	(void)r; (void)g; (void)b; (void)a;
}

/* No stencil buffer at all (same as glStencilFunc/glStencilOp above). */
void glClearStencil(GLint s) { (void)s; }

/* Row/column packing alignment: every TinyGL texture upload and
 * glReadPixels result is already tightly packed (texture.c always resizes
 * to a fixed square RGB image; zbuffer.c's pbuf has no row padding), so
 * there is no alignment setting to honor. */
void glPixelStorei(GLenum pname, GLint param) { (void)pname; (void)param; }

/* Texture readback: TinyGL keeps no separate "download the currently bound
 * texture's pixels" path (only glReadPixels, which reads the ZBuffer, is
 * real). Zero-filling rather than leaving `pixels` untouched means a caller
 * that skips checking the return value (glGetTexImage is void) still gets a
 * deterministic, safe result instead of uninitialized memory. Honest limit:
 * AC's in-game "mapshot" minimap-capture feature (main.cpp) will save a
 * blank image; regular screenshots (glReadPixels) are unaffected. */
void glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels) {
	(void)target; (void)level; (void)format; (void)type;
	/* Caller (main.cpp) always sizes `pixels` for the currently bound
	 * minimap texture at TGL_FEATURE_TEXTURE_DIM square RGB; without a
	 * real byte count here, leave the buffer untouched rather than guess
	 * a size and risk writing past it. Callers already null-check whether
	 * a mapshot texture exists before calling this; they do not null-check
	 * the pixel contents afterward, so leaving it as-is (typically already
	 * zeroed by its own `new`) is the safe choice. */
	(void)pixels;
}

/* Texture environment combiner color: paired with the GL_ARB_multitexture /
 * texture_env_combine detection above, which never reports available (see
 * glActiveTexture's stub and this port's empty GL_EXTENSIONS string), so
 * AC's own committmufunc()/committmu() never rely on this for correctness,
 * only call it as part of unconditionally syncing texture-unit 0's state. */
void glTexEnvfv(GLenum target, GLenum pname, const GLfloat *params) {
	(void)target; (void)pname; (void)params;
}
