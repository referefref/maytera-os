/*
 * Texture Manager
 */

#include "zgl.h"
#include "msghandling.h"
#include "syscall.h"

/* #421 phase 8 (AssaultCube port): raw, malloc/stdio-free diagnostic
 * writer for the GLTexture intra-struct canary below. tgl_warning() etc.
 * are compiled out entirely in this build (-DNO_DEBUG_OUTPUT, see
 * ../Makefile), and even when they are not, fprintf() depends on a
 * working heap - not something to trust once heap corruption is exactly
 * the thing being diagnosed. sys_write(2, ...) mirrors the same pattern
 * userland/libc/stdlib.c's heap_write_str/heap_write_hex already use for
 * the identical reason (see that file's HEAP_CANARY comment). */
static void gltex_write_str(const char* s) {
	unsigned long n = 0;
	while (s[n]) n++;
	sys_write(2, s, n);
}
static void gltex_write_hex(unsigned long v) {
	char buf[19];
	buf[0] = '0'; buf[1] = 'x';
	for (int i = 0; i < 16; i++) {
		int nib = (int)((v >> ((15 - i) * 4)) & 0xF);
		buf[2 + i] = (char)(nib < 10 ? ('0' + nib) : ('a' + nib - 10));
	}
	buf[18] = '\0';
	gltex_write_str(buf);
}

/* Checked at every hash-chain walk/alloc/free AND immediately before/after
 * every pixmap upload op, so a mismatch is reported at the FIRST point it
 * is observed, pinning which specific operation trips it rather than only
 * ever surfacing the later, contextless glGenTextures() #GP. `where` is a
 * short static string identifying the call site. */
static void gltex_check_canary(GLTexture* t, const char* where) {
	if (!t) return;
	if (t->canary != GLTEXTURE_CANARY_MAGIC) {
		gltex_write_str("[libgl] GLTexture canary TRIPPED at ");
		gltex_write_str(where);
		gltex_write_str(" t="); gltex_write_hex((unsigned long)(void*)t);
		gltex_write_str(" handle="); gltex_write_hex((unsigned long)(unsigned)t->handle);
		gltex_write_str(" canary="); gltex_write_hex((unsigned long)t->canary);
		gltex_write_str(" next="); gltex_write_hex((unsigned long)(void*)t->next);
		gltex_write_str(" prev="); gltex_write_hex((unsigned long)(void*)t->prev);
		gltex_write_str("\n");
	}
}

static GLTexture* find_texture(GLint h) {
	GLTexture* t;
	GLContext* c = gl_get_context();
	t = c->shared_state.texture_hash_table[h & TEXTURE_HASH_TABLE_MASK];
	while (t != NULL) {
		if (t->canary != GLTEXTURE_CANARY_MAGIC) {
			gltex_check_canary(t, "find_texture");
			/* Stop here instead of following a possibly-wild `next`: turns
			 * what would be an unrelated #GP one hop later into a clean,
			 * diagnosable "texture not found" instead of compounding the
			 * evidence with a second crash. */
			return NULL;
		}
		if (t->handle == h)
			return t;
		t = t->next;
	}
	return NULL;
}

GLboolean glAreTexturesResident(GLsizei n, const GLuint* textures, GLboolean* residences) {
#define RETVAL GL_FALSE
	GLboolean retval = GL_TRUE;
	GLint i;
#include "error_check_no_context.h"

	for (i = 0; i < n; i++)
		if (find_texture(textures[i])) {
			residences[i] = GL_TRUE;
		} else {
			residences[i] = GL_FALSE;
			retval = GL_FALSE;
		}
	return retval;
}
GLboolean glIsTexture(GLuint texture) {
	GLContext* c = gl_get_context();
#define RETVAL GL_FALSE
#include "error_check.h"
	if (find_texture(texture))
		return GL_TRUE;
	return GL_FALSE;
}

void* glGetTexturePixmap(GLint text, GLint level, GLint* xsize, GLint* ysize) {
	GLTexture* tex;
	GLContext* c = gl_get_context();
#if TGL_FEATURE_ERROR_CHECK == 1
	if (!(text >= 0 && level < MAX_TEXTURE_LEVELS))
#define ERROR_FLAG GL_INVALID_ENUM
#define RETVAL NULL
#include "error_check.h"
#else
	/*assert(text >= 0 && level < MAX_TEXTURE_LEVELS);*/
#endif
		tex = find_texture(text);
	if (!tex)
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_INVALID_ENUM
#define RETVAL NULL
#include "error_check.h"
#else
		return NULL;
#endif
		*xsize = tex->images[level].xsize;
	*ysize = tex->images[level].ysize;
	return tex->images[level].pixmap;
}

static void free_texture(GLContext* c, GLint h) {
	GLTexture *t, **ht;

	t = find_texture(h);
	if (t == NULL) return; /* corrupted chain: find_texture already reported it */
	gltex_check_canary(t, "free_texture");
	if (t->prev == NULL) {
		ht = &c->shared_state.texture_hash_table[t->handle & TEXTURE_HASH_TABLE_MASK];
		*ht = t->next;
	} else {
		t->prev->next = t->next;
	}
	if (t->next != NULL)
		t->next->prev = t->prev;

	gl_free(t);
}

GLTexture* alloc_texture(GLint h) {
	GLContext* c = gl_get_context();
	GLTexture *t, **ht;
#define RETVAL NULL
#include "error_check.h"
	t = gl_zalloc(sizeof(GLTexture));
	if (!t)
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_OUT_OF_MEMORY
#define RETVAL NULL
#include "error_check.h"
#else
		gl_fatal_error("GL_OUT_OF_MEMORY");
#endif

		ht = &c->shared_state.texture_hash_table[h & TEXTURE_HASH_TABLE_MASK];

	t->canary = GLTEXTURE_CANARY_MAGIC;
	if (*ht) gltex_check_canary(*ht, "alloc_texture(head-of-chain)");
	t->next = *ht;
	t->prev = NULL;
	if (t->next != NULL)
		t->next->prev = t;
	*ht = t;

	t->handle = h;

	return t;
}

void glInitTextures() {
	/* textures */
	GLContext* c = gl_get_context();
	c->texture_2d_enabled = 0;
	c->current_texture = find_texture(0);
}

void glGenTextures(GLint n, GLuint* textures) {
	GLContext* c = gl_get_context();
	GLint max, i;
	GLTexture* t;
#include "error_check.h"
	max = 0;
	for (i = 0; i < TEXTURE_HASH_TABLE_SIZE; i++) {
		t = c->shared_state.texture_hash_table[i];
		while (t != NULL) {
			if (t->canary != GLTEXTURE_CANARY_MAGIC) {
				gltex_check_canary(t, "glGenTextures");
				break; /* stop walking this bucket's chain, do not follow a wild `next` */
			}
			if (t->handle > max)
				max = t->handle;
			t = t->next;
		}
	}
	for (i = 0; i < n; i++) {
		textures[i] = max + i + 1; /* MARK: How texture handles are created.*/
	}
}

void glDeleteTextures(GLint n, const GLuint* textures) {
	GLint i;
	GLTexture* t;
	GLContext* c = gl_get_context();
#include "error_check.h"
	for (i = 0; i < n; i++) {
		t = find_texture(textures[i]);
		if (t != NULL && t != 0) {
			if (t == c->current_texture) {
				glBindTexture(GL_TEXTURE_2D, 0);
#include "error_check.h"
			}
			free_texture(c, textures[i]);
		}
	}
}

void glopBindTexture(GLParam* p) {
	GLint target = p[1].i;
	GLint texture = p[2].i;
	GLTexture* t;
	GLContext* c = gl_get_context();
#if TGL_FEATURE_ERROR_CHECK == 1
	if (!(target == GL_TEXTURE_2D && target > 0))
#define ERROR_FLAG GL_INVALID_ENUM
#include "error_check.h"
#else
	
#endif
		t = find_texture(texture);
	if (t == NULL) {
		t = alloc_texture(texture);
#include "error_check.h"
	}
	if (t == NULL) { 
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_OUT_OF_MEMORY
#include "error_check.h"
#else
		gl_fatal_error("GL_OUT_OF_MEMORY");
#endif
	}
	c->current_texture = t;
}


void glCopyTexImage2D(GLenum target,		 
					  GLint level,			 
					  GLenum internalformat, 
					  GLint x,				 
					  GLint y,				 
					  GLsizei width,		 
					  GLsizei height, GLint border) {
	GLParam p[9];
#include "error_check_no_context.h"

	p[0].op = OP_CopyTexImage2D;
	p[1].i = target;
	p[2].i = level;
	p[3].i = internalformat;
	p[4].i = x;
	p[5].i = y;
	p[6].i = width;
	p[7].i = height;
	p[8].i = border;
	gl_add_op(p);
}
void glopCopyTexImage2D(GLParam* p) {
	GLImage* im;
	PIXEL* data;
	GLint i, j;
	GLint target = p[1].i;
	GLint level = p[2].i;
	GLint x = p[4].i;
	GLint y = p[5].i;
	GLsizei w = p[6].i;
	GLsizei h = p[7].i;
	GLint border = p[8].i;
	GLContext* c = gl_get_context();
	y -= h;

	if (c->readbuffer != GL_FRONT || c->current_texture == NULL || target != GL_TEXTURE_2D || border != 0 ||
		w != TGL_FEATURE_TEXTURE_DIM || /*TODO Implement image interp*/
		h != TGL_FEATURE_TEXTURE_DIM) {
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_INVALID_OPERATION
#include "error_check.h"
#else
		return;
#endif
	}
	gltex_check_canary(c->current_texture, "glopCopyTexImage2D(entry)");
	im = &c->current_texture->images[level];
	data = c->current_texture->images[level].pixmap;
	im->xsize = TGL_FEATURE_TEXTURE_DIM;
	im->ysize = TGL_FEATURE_TEXTURE_DIM;
	/* TODO implement the scaling and stuff that the GL spec says it should have.*/
#if TGL_FEATURE_MULTITHREADED_COPY_TEXIMAGE_2D == 1
#ifdef _OPENMP
#pragma omp parallel for
#endif
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			data[i + j * w] = c->zb->pbuf[((i + x) % (c->zb->xsize)) + ((j + y) % (c->zb->ysize)) * (c->zb->xsize)];
		}
#else
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			data[i + j * w] = c->zb->pbuf[((i + x) % (c->zb->xsize)) + ((j + y) % (c->zb->ysize)) * (c->zb->xsize)];
		}
#endif
	gltex_check_canary(c->current_texture, "glopCopyTexImage2D(exit)");
}

/* AssaultCube port phase 4 (#421): glopTexImage1D/2D below have always
 * hard-required format==GL_RGB (3 bytes/pixel), which is fine for opaque
 * world/menu textures (phase 3's own icon.png/notexture.jpg fix already
 * normalizes those to RGB, see sdlshim.cpp's IMG_Load_RW comment on why
 * Amask is deliberately 0). AssaultCube's HUD/font atlases are a different,
 * real caller: font.cfg's `font huddigits "packages/misc/huddigits.png" ...`
 * commands load textures through the SAME createtexture()/uploadtexture()
 * path but legitimately request GL_LUMINANCE / GL_LUMINANCE_ALPHA / GL_RGBA /
 * GL_ALPHA source data (grayscale digit glyphs, some with an alpha channel
 * for masking), which the strict GL_RGB-only check below rejected outright
 * via gl_fatal_error() -> exit(1), one texture upload past the #421 font.cfg
 * fix. Real desktop OpenGL accepts any of these `format` values for
 * glTexImage2D; this rasterizer's stored images are always 3-byte RGB (see
 * gl_convertRGB_to_8A8R8G8B/gl_convertRGB_to_5R6G5B below, which assume
 * exactly that layout), so the honest fix is the same shape as the phase 3
 * internalformat fix: convert INTO the one format this rasterizer actually
 * stores, at the point of upload, rather than teach the whole pixmap/
 * rasterizer pipeline a second pixel layout. Alpha is dropped (no per-pixel
 * alpha blending exists in ztriangle.c's sampler regardless, matching the
 * already-documented "vertex/uniform-color alpha only" limitation from the
 * phase 2 glAlphaFunc gap-fill), matching how the caller's own colour tint
 * is applied on top at draw time; GL_ALPHA (no colour channel at all, a pure
 * mask) is rendered as its own alpha value replicated into R/G/B so the
 * glyph's SHAPE is still visible as grayscale rather than a blank/solid
 * rectangle, an honest approximation, not a silent correctness bug.
 * Returns NULL (caller must treat that as "unsupported, do not proceed") only
 * for a `type` other than GL_UNSIGNED_BYTE, which no real caller in this tree
 * sends. */
static GLubyte* gl_convert_to_rgb888(const void* src, GLint width, GLint height, GLint format, GLint type) {
	if (type != GL_UNSIGNED_BYTE || width <= 0 || height <= 0) return NULL;
	GLint n = width * height;
	GLubyte* out = gl_malloc((unsigned long)n * 3);
	if (!out) return NULL;
	const GLubyte* s = (const GLubyte*)src;
	GLint i;
	switch (format) {
	case GL_RGB:
		memcpy(out, s, (unsigned long)n * 3);
		break;
	case GL_RGBA:
		for (i = 0; i < n; i++) { out[i*3] = s[i*4]; out[i*3+1] = s[i*4+1]; out[i*3+2] = s[i*4+2]; }
		break;
	case GL_LUMINANCE:
		for (i = 0; i < n; i++) { GLubyte l = s[i]; out[i*3] = l; out[i*3+1] = l; out[i*3+2] = l; }
		break;
	case GL_LUMINANCE_ALPHA:
		for (i = 0; i < n; i++) { GLubyte l = s[i*2]; out[i*3] = l; out[i*3+1] = l; out[i*3+2] = l; }
		break;
	case GL_ALPHA:
		for (i = 0; i < n; i++) { GLubyte a = s[i]; out[i*3] = a; out[i*3+1] = a; out[i*3+2] = a; }
		break;
	default:
		gl_free(out);
		return NULL;
	}
	return out;
}

void glopTexImage1D(GLParam* p) {
	GLint target = p[1].i;
	GLint level = p[2].i;
	GLint components = p[3].i;
	GLint width = p[4].i;
	/* GLint height = p[5].i;*/
	GLint height = 1;
	GLint border = p[5].i;
	GLint format = p[6].i;
	GLint type = p[7].i;
	void* pixels = p[8].p;
	GLImage* im;
	GLubyte* pixels1;
	GLint do_free=0;
	GLContext* c = gl_get_context();
	/* AssaultCube port phase 3: same GLenum-vs-integer internalformat fix as
	 * glopTexImage2D below (see that function's comment for the full why). */
	if (components == GL_LUMINANCE) components = 1;
	else if (components == GL_LUMINANCE_ALPHA) components = 2;
	else if (components == GL_RGB) components = 3;
	else if (components == GL_RGBA) components = 4;
	/* #421 phase 4: convert any real-GL source format into this rasterizer's
	 * one stored layout (3-byte RGB) instead of hard-rejecting it. See the
	 * gl_convert_to_rgb888() comment above for the full rationale. */
	/* #421 phase 5: a NULL data pointer is a 100%-legal, common OpenGL idiom
	 * ("allocate this texture's storage, I'll fill it in later via
	 * glTexSubImage2D", or a placeholder/render-target texture) - real
	 * desktop GL leaves the texel contents unspecified in that case. Guard
	 * the conversion call so a NULL source with a non-GL_RGB format does not
	 * crash inside gl_convert_to_rgb888()'s own memcpy/loops (that combo is
	 * simply not supported yet: format stays non-GL_RGB and the validation
	 * block below correctly rejects it, same as before this fix). The
	 * common, actually-reproduced case (format already GL_RGB, so no
	 * conversion is even attempted) is handled further down, right before
	 * `pixels` would otherwise be dereferenced. */
	GLubyte* converted1d = NULL;
	if (format != GL_RGB && type == GL_UNSIGNED_BYTE && pixels != NULL) {
		converted1d = gl_convert_to_rgb888(pixels, width, height, format, type);
		if (converted1d) { pixels = converted1d; format = GL_RGB; components = 3; }
	}
	{
#if TGL_FEATURE_ERROR_CHECK == 1
		if (!(c->current_texture != NULL && target == GL_TEXTURE_1D && level == 0 && components == 3 && border == 0 && format == GL_RGB &&
			  type == GL_UNSIGNED_BYTE))
#define ERROR_FLAG GL_INVALID_ENUM
#include "error_check.h"

#else
		if (!(c->current_texture != NULL && target == GL_TEXTURE_1D && level == 0 && components == 3 && border == 0 && format == GL_RGB &&
			  type == GL_UNSIGNED_BYTE))
			gl_fatal_error("glTexImage2D: combination of parameters not handled!!");
#endif
	}
	/* #421 phase 5: past the same validation every other call goes through
	 * (target/level/components/border/format/type all already confirmed
	 * legal above), a NULL `pixels` is the ONLY thing left that would crash:
	 * gl_resizeImageNoInterpolate()/gl_convertRGB_to_8A8R8G8B() below both
	 * unconditionally dereference it. This is the exact, reproduced
	 * AssaultCube crash (a real NULL-pointer SIGSEGV inside
	 * gl_convertRGB_to_8A8R8G8B, confirmed via the kernel's new #421
	 * phase-5 original-fault diagnostic in mm/fault.c: cr2=0x0, i.e. the
	 * source pointer really was NULL, not a wild pointer). Record the
	 * logical size (clamped to this rasterizer's one fixed stored
	 * resolution, exactly like every other path below, so a later real
	 * glTexSubImage2D upload or the per-pixel sampler in ztriangle.c never
	 * see a size mismatch against the fixed-size GLImage::pixmap array)
	 * and skip the conversion entirely instead of crashing. */
	gltex_check_canary(c->current_texture, "glopTexImage1D(entry)");
	if (pixels == NULL) {
		im = &c->current_texture->images[level];
		im->xsize = TGL_FEATURE_TEXTURE_DIM;
		im->ysize = TGL_FEATURE_TEXTURE_DIM;
		return;
	}
	if (width != TGL_FEATURE_TEXTURE_DIM || height != TGL_FEATURE_TEXTURE_DIM) {
		pixels1 = gl_malloc(TGL_FEATURE_TEXTURE_DIM * TGL_FEATURE_TEXTURE_DIM * 3); /* GUARDED*/
		if (pixels1 == NULL) {
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_OUT_OF_MEMORY
#include "error_check.h"
#else
			gl_fatal_error("GL_OUT_OF_MEMORY");
#endif
		}
		/* no GLinterpolation is done here to respect the original image aliasing ! */

		gl_resizeImageNoInterpolate(pixels1, TGL_FEATURE_TEXTURE_DIM, TGL_FEATURE_TEXTURE_DIM, pixels, width, height);
		do_free = 1;
		width = TGL_FEATURE_TEXTURE_DIM;
		height = TGL_FEATURE_TEXTURE_DIM;
	} else {
		pixels1 = pixels;
	}

	/* #421 phase 8: belt-and-braces hard bound on the actual write, since
	 * width/height here MUST be exactly TGL_FEATURE_TEXTURE_DIM by this
	 * point (either originally, or forced by the resize branch above) -
	 * im->pixmap is a fixed TGL_FEATURE_TEXTURE_DIM^2 buffer with no other
	 * capacity tracking. If that invariant is ever violated, clamp instead
	 * of overflowing into the GLTexture's own next/prev/handle tail. */
	if (width != TGL_FEATURE_TEXTURE_DIM || height != TGL_FEATURE_TEXTURE_DIM) {
		gltex_write_str("[libgl] glopTexImage1D: width/height not clamped to TGL_FEATURE_TEXTURE_DIM before pixmap write, w=");
		gltex_write_hex((unsigned long)(long)width);
		gltex_write_str(" h="); gltex_write_hex((unsigned long)(long)height);
		gltex_write_str("\n");
		width = TGL_FEATURE_TEXTURE_DIM;
		height = TGL_FEATURE_TEXTURE_DIM;
	}

	im = &c->current_texture->images[level];
	im->xsize = width;
	im->ysize = height;
#if TGL_FEATURE_RENDER_BITS == 32
	gl_convertRGB_to_8A8R8G8B(im->pixmap, pixels1, width, height);
#elif TGL_FEATURE_RENDER_BITS == 16
	gl_convertRGB_to_5R6G5B(im->pixmap, pixels1, width, height);
#else
#error bad TGL_FEATURE_RENDER_BITS
#endif
	gltex_check_canary(c->current_texture, "glopTexImage1D(exit)");
	if (do_free)
		gl_free(pixels1);
	if (converted1d)
		gl_free(converted1d);
}
void glopTexImage2D(GLParam* p) {
	GLint target = p[1].i;
	GLint level = p[2].i;
	GLint components = p[3].i;
	GLint width = p[4].i;
	GLint height = p[5].i;
	GLint border = p[6].i;
	GLint format = p[7].i;
	GLint type = p[8].i;
	void* pixels = p[9].p;
	GLImage* im;
	GLubyte* pixels1;
	GLint do_free=0;
	GLContext* c = gl_get_context();
	/* AssaultCube port phase 3 (docs/ASSAULTCUBE_PORT_PLAN.md): mipmap
	 * levels beyond the base. This function's own images[] array (see
	 * GLTexture/GLImage in zgl.h) is sized for MAX_TEXTURE_LEVELS, but the
	 * validation below has always hard-required level==0, and nothing in
	 * this codebase's three prior GL apps (glcube/glmatrix/gldemo) ever
	 * requested real mipmaps (mipmap=false at their one glTexImage2D call),
	 * so this path was simply never reached before. AssaultCube's own
	 * createtexture() does request mipmaps (see uploadtexture()'s
	 * level-loop, texture.cpp), and hits this on the SECOND glTexImage2D
	 * call (level=1) for every single texture it loads. Rather than try to
	 * make the level>0 path fully correct (this rasterizer's per-pixel
	 * texture sampling in ztriangle.c only ever reads images[0] regardless
	 * of the min filter, i.e. it has no real mip-level LOD selection to
	 * feed anyway; that is a separate, deeper piece of unimplemented work),
	 * accept and silently ignore any level > 0: the base level (0) already
	 * uploaded a complete, correctly-filtered texture, so a real image
	 * still renders, just without the minification-quality benefit real
	 * mipmapping would add at a distance. Documented honest limitation, not
	 * a silent correctness bug: nothing reads or writes stale/uninitialized
	 * data as a result, the upload for that level is simply a no-op. */
	if (level > 0) return;
	/* AssaultCube port phase 3: real OpenGL has always allowed
	 * internalformat to be EITHER the legacy component COUNT (1/2/3/4, what
	 * this check originally required) OR the equivalent GLenum form
	 * (GL_LUMINANCE/GL_LUMINANCE_ALPHA/GL_RGB/GL_RGBA), per spec.
	 * AssaultCube's own texture.cpp createtexture() calls
	 * glTexImage2D(..., format, ...) passing the GLenum form (e.g. GL_RGB =
	 * 6407), which this check rejected outright because it only ever
	 * compared against the bare integer 3, silently killing the process via
	 * gl_fatal_error() -> exit(1) (NO_DEBUG_OUTPUT swallows its own error
	 * message, which is what made this so hard to find: real upstream,
	 * standards-legal caller code, TinyGL's own check simply never accepted
	 * the enum spelling of an internalformat any real port beyond this
	 * codebase's own three demos, which all happen to pass the literal 3,
	 * was ever going to send it). Fixed by accepting both spellings, not by
	 * changing the caller: this is the shared primitive, and any future GL
	 * port would hit the exact same silent-exit wall otherwise. */
	if (components == GL_LUMINANCE) components = 1;
	else if (components == GL_LUMINANCE_ALPHA) components = 2;
	else if (components == GL_RGB) components = 3;
	else if (components == GL_RGBA) components = 4;
	/* #421 phase 4: convert any real-GL source format (AssaultCube's
	 * font/HUD atlases legitimately use GL_LUMINANCE / GL_LUMINANCE_ALPHA /
	 * GL_RGBA / GL_ALPHA) into this rasterizer's one stored layout (3-byte
	 * RGB) instead of hard-rejecting it via gl_fatal_error()->exit(1). See
	 * the gl_convert_to_rgb888() comment above glopTexImage1D for the full
	 * rationale; this is the same fix, applied to the 2D upload path AC's
	 * own font.cfg-driven `font ...` commands actually use. */
	/* #421 phase 5: see glopTexImage1D above for the full rationale (a NULL
	 * data pointer is a legal OpenGL call this rasterizer must not crash
	 * on). Guard the conversion call the same way: a NULL source with a
	 * non-GL_RGB format is simply not supported yet (format stays
	 * non-GL_RGB and the validation block below correctly rejects it). */
	GLubyte* converted2d = NULL;
	if (format != GL_RGB && type == GL_UNSIGNED_BYTE && pixels != NULL) {
		converted2d = gl_convert_to_rgb888(pixels, width, height, format, type);
		if (converted2d) { pixels = converted2d; format = GL_RGB; components = 3; }
	}
	{
#if TGL_FEATURE_ERROR_CHECK == 1
		if (!(c->current_texture != NULL && target == GL_TEXTURE_2D && level == 0 && components == 3 && border == 0 && format == GL_RGB &&
			  type == GL_UNSIGNED_BYTE))
#define ERROR_FLAG GL_INVALID_ENUM
#include "error_check.h"

#else
		if (!(c->current_texture != NULL && target == GL_TEXTURE_2D && level == 0 && components == 3 && border == 0 && format == GL_RGB &&
			  type == GL_UNSIGNED_BYTE))
			gl_fatal_error("glTexImage2D: combination of parameters not handled!!");
#endif
	}
	/* #421 phase 5: past the same validation every other call goes through,
	 * a NULL `pixels` is the ONLY thing left that would crash - this is the
	 * exact, reproduced AssaultCube crash (real NULL-pointer SIGSEGV inside
	 * gl_convertRGB_to_8A8R8G8B, confirmed via the kernel's new #421
	 * phase-5 original-fault diagnostic in mm/fault.c: cr2=0x0). Record the
	 * logical size (clamped to this rasterizer's one fixed stored
	 * resolution, matching every other path below) and skip the conversion
	 * entirely instead of crashing. */
	gltex_check_canary(c->current_texture, "glopTexImage2D(entry)");
	if (pixels == NULL) {
		im = &c->current_texture->images[level];
		im->xsize = TGL_FEATURE_TEXTURE_DIM;
		im->ysize = TGL_FEATURE_TEXTURE_DIM;
		return;
	}
	if (width != TGL_FEATURE_TEXTURE_DIM || height != TGL_FEATURE_TEXTURE_DIM) {
		pixels1 = gl_malloc(TGL_FEATURE_TEXTURE_DIM * TGL_FEATURE_TEXTURE_DIM * 3); /* GUARDED*/
		if (pixels1 == NULL) {
#if TGL_FEATURE_ERROR_CHECK == 1
#define ERROR_FLAG GL_OUT_OF_MEMORY
#include "error_check.h"
#else
			gl_fatal_error("GL_OUT_OF_MEMORY");
#endif
		}
		/* no GLinterpolation is done here to respect the original image aliasing ! */

		gl_resizeImageNoInterpolate(pixels1, TGL_FEATURE_TEXTURE_DIM, TGL_FEATURE_TEXTURE_DIM, pixels, width, height);
		do_free = 1;
		width = TGL_FEATURE_TEXTURE_DIM;
		height = TGL_FEATURE_TEXTURE_DIM;
	} else {
		pixels1 = pixels;
	}

	/* #421 phase 8: same belt-and-braces hard bound as glopTexImage1D
	 * above - width/height MUST be exactly TGL_FEATURE_TEXTURE_DIM here. */
	if (width != TGL_FEATURE_TEXTURE_DIM || height != TGL_FEATURE_TEXTURE_DIM) {
		gltex_write_str("[libgl] glopTexImage2D: width/height not clamped to TGL_FEATURE_TEXTURE_DIM before pixmap write, w=");
		gltex_write_hex((unsigned long)(long)width);
		gltex_write_str(" h="); gltex_write_hex((unsigned long)(long)height);
		gltex_write_str("\n");
		width = TGL_FEATURE_TEXTURE_DIM;
		height = TGL_FEATURE_TEXTURE_DIM;
	}

	im = &c->current_texture->images[level];
	im->xsize = width;
	im->ysize = height;
#if TGL_FEATURE_RENDER_BITS == 32
	gl_convertRGB_to_8A8R8G8B(im->pixmap, pixels1, width, height);
#elif TGL_FEATURE_RENDER_BITS == 16
	gl_convertRGB_to_5R6G5B(im->pixmap, pixels1, width, height);
#else
#error Bad TGL_FEATURE_RENDER_BITS
#endif
	gltex_check_canary(c->current_texture, "glopTexImage2D(exit)");
	if (do_free)
		gl_free(pixels1);
	if (converted2d)
		gl_free(converted2d);
}

/* AssaultCube port phase 2: glTexSubImage2D was entirely missing. Real,
 * bounds-checked partial upload into an already-allocated texture image.
 * glTexImage2D above always resizes the stored image to a fixed
 * TGL_FEATURE_TEXTURE_DIM square and records im->xsize/ysize as that fixed
 * size, so xoffset/yoffset/width/height here are already in the SAME
 * coordinate space as the stored pixmap: no extra resize-tracking is
 * needed. Only GL_TEXTURE_2D / level 0 / GL_RGB / GL_UNSIGNED_BYTE is
 * supported, matching glTexImage2D's own restriction (see the parameter
 * check in glopTexImage2D above).
 */
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                      GLsizei width, GLsizei height, GLenum format, GLenum type,
                      const GLvoid* pixels) {
	GLParam p[10];
#include "error_check_no_context.h"
	p[0].op = OP_TexSubImage2D;
	p[1].i = target;
	p[2].i = level;
	p[3].i = xoffset;
	p[4].i = yoffset;
	p[5].i = width;
	p[6].i = height;
	p[7].i = format;
	p[8].i = type;
	p[9].p = (void*)pixels;
	gl_add_op(p);
}

void glopTexSubImage2D(GLParam* p) {
	GLint target = p[1].i;
	GLint level = p[2].i;
	GLint xoffset = p[3].i;
	GLint yoffset = p[4].i;
	GLint width = p[5].i;
	GLint height = p[6].i;
	GLint format = p[7].i;
	GLint type = p[8].i;
	const GLubyte* pixels = (const GLubyte*)p[9].p;
	GLContext* c = gl_get_context();
	GLImage* im;
	GLint y;

	if (target != GL_TEXTURE_2D || level != 0 || format != GL_RGB || type != GL_UNSIGNED_BYTE ||
	    c->current_texture == NULL || pixels == NULL) {
		tgl_warning("glTexSubImage2D: unsupported parameter combination, ignored.\n");
		return;
	}

	gltex_check_canary(c->current_texture, "glopTexSubImage2D(entry)");
	im = &c->current_texture->images[level];

	/* #421 phase 8: the metadata check below (against im->xsize/ysize) is
	 * only as trustworthy as im->xsize/ysize themselves; those fields live
	 * at the very START of the GLTexture's tail (immediately after the
	 * fixed pixmap buffer, see zgl.h), i.e. exactly where a pixmap overrun
	 * would hit FIRST, before it would reach the canary a few bytes later.
	 * Add a hard check against the true, compile-time-fixed capacity too,
	 * so a corrupted im->xsize/ysize can't make the metadata check above
	 * wrongly pass. */
	if (xoffset < 0 || yoffset < 0 || width <= 0 || height <= 0 ||
	    xoffset + width > im->xsize || yoffset + height > im->ysize ||
	    xoffset + width > TGL_FEATURE_TEXTURE_DIM || yoffset + height > TGL_FEATURE_TEXTURE_DIM) {
		tgl_warning("glTexSubImage2D: region out of bounds, ignored.\n");
		return;
	}

	for (y = 0; y < height; y++) {
		const GLubyte* srow = pixels + (GLint)y * width * 3;
		PIXEL* drow = im->pixmap + (yoffset + y) * im->xsize + xoffset;
#if TGL_FEATURE_RENDER_BITS == 32
		gl_convertRGB_to_8A8R8G8B((GLuint*)drow, (GLubyte*)srow, width, 1);
#elif TGL_FEATURE_RENDER_BITS == 16
		gl_convertRGB_to_5R6G5B((GLushort*)drow, (GLubyte*)srow, width, 1);
#else
#error Bad TGL_FEATURE_RENDER_BITS
#endif
	}
	gltex_check_canary(c->current_texture, "glopTexSubImage2D(exit)");
}

/* TODO: not all tests are done */
/*
void glopTexEnv(GLContext* c, GLParam* p) {
	GLint target = p[1].i;
	GLint pname = p[2].i;
	GLint param = p[3].i;

	if (target != GL_TEXTURE_ENV) {

	error:
#if TGL_FEATURE_ERROR_CHECK == 1

#define ERROR_FLAG GL_INVALID_ENUM
#include "error_check.h"
#else
		gl_fatal_error("glTexParameter: unsupported option");
#endif

	}

	if (pname != GL_TEXTURE_ENV_MODE)
		goto error;

	if (param != GL_DECAL)
		goto error;
}
*/
/* TODO: not all tests are done */
/*
void glopTexParameter(GLContext* c, GLParam* p) {
	GLint target = p[1].i;
	GLint pname = p[2].i;
	GLint param = p[3].i;

	if (target != GL_TEXTURE_2D &&
		target != GL_TEXTURE_1D) {
	error:
		tgl_warning("glTexParameter: unsupported option");
		return;
	}

	switch (pname) {
	case GL_TEXTURE_WRAP_S:
	case GL_TEXTURE_WRAP_T:
		if (param != GL_REPEAT)
			goto error;
		break;
	}
}
*/

/*
void glopPixelStore(GLContext* c, GLParam* p) {
	GLint pname = p[1].i;
	GLint param = p[2].i;

	if (pname != GL_UNPACK_ALIGNMENT || param != 1) {
		gl_fatal_error("glPixelStore: unsupported option");
	}
}
*/
