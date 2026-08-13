/*

 * Z buffer: 16 bits Z / 16 bits color
 *
 */

#include <stdlib.h>
#include <string.h>

#include "../include/zbuffer.h"
#include "msghandling.h"

/* #578 T0(d): SSE2 intrinsics for the per-frame buffer-clear hot path
 * (ZB_clear -> memset_s/memset_l below). This is userland (unlike the
 * -mno-sse kernel), the top-level Makefile already forces -msse -msse2
 * unconditionally for every libgl translation unit, and the real/QEMU
 * targets this OS runs on both guarantee SSE2, so no runtime CPUID
 * dispatch is needed (matches the existing unconditional TGL_ALIGN /
 * -msse2 convention already in zfeatures.h). GCC's own <emmintrin.h>
 * pulls in mm_malloc.h, which wants libc's <stdlib.h> for
 * posix_memalign() - unavailable under -nostdinc/-nostdlib freestanding
 * userland. Pre-defining the header's own include guard skips that
 * unused _mm_malloc/_mm_free glue without needing any of it (verified:
 * this is the standard, documented guard macro in GCC 12's
 * mm_malloc.h, not a hack around a missing symbol). */
#if defined(__SSE2__)
#define _MM_MALLOC_H_INCLUDED
#include <emmintrin.h>
#endif
ZBuffer* ZB_open(GLint xsize, GLint ysize, GLint mode,

				 void* frame_buffer) {
	ZBuffer* zb;
	GLint size;

	zb = gl_malloc(sizeof(ZBuffer));
	if (zb == NULL)
		return NULL;

	zb->xsize = xsize & ~3;
	zb->ysize = ysize;

	/* #582: this used to be `xsize * PSZB` (the UNFLOORED caller arg), while
	 * zb->xsize just above is floored to a multiple of 4. Harmless for
	 * mult-4 widths (the only case exercised so far) since floor is a no-op
	 * there, but latent: fix it to use the floored zb->xsize like
	 * ZB_resize() already correctly does below, so linesize and xsize can
	 * never disagree regardless of caller-supplied width. */
	zb->linesize = (zb->xsize * PSZB);

	switch (mode) {
#if TGL_FEATURE_32_BITS == 1
	case ZB_MODE_RGBA:
		break;
#endif
#if TGL_FEATURE_16_BITS == 1
	case ZB_MODE_5R6G5B:
		break;
#endif

	default:
		goto error;
	}

	size = zb->xsize * zb->ysize * sizeof(GLushort);

	zb->zbuf = gl_malloc(size);
	if (zb->zbuf == NULL)
		goto error;

	if (frame_buffer == NULL) {
		zb->pbuf = gl_malloc(zb->ysize * zb->linesize);
		if (zb->pbuf == NULL) {
			gl_free(zb->zbuf);
			goto error;
		}
		zb->frame_buffer_allocated = 1;
	} else {
		zb->frame_buffer_allocated = 0;
		zb->pbuf = frame_buffer;
	}

	zb->current_texture = NULL;

	return zb;
error:
	gl_free(zb);
	return NULL;
}

void ZB_close(ZBuffer* zb) {

	if (zb->frame_buffer_allocated)
		gl_free(zb->pbuf);

	gl_free(zb->zbuf);
	gl_free(zb);
}

void ZB_resize(ZBuffer* zb, void* frame_buffer, GLint xsize, GLint ysize) {
	GLint size;

	/* xsize must be a multiple of 4 */
	xsize = xsize & ~3;

	zb->xsize = xsize;
	zb->ysize = ysize;
	zb->linesize = (xsize * PSZB);

	size = zb->xsize * zb->ysize * sizeof(GLushort);

	gl_free(zb->zbuf);
	zb->zbuf = gl_malloc(size);
	if (zb->zbuf == NULL)
		exit(1);
	if (zb->frame_buffer_allocated)
		gl_free(zb->pbuf);

	if (frame_buffer == NULL) {
		zb->pbuf = gl_malloc(zb->ysize * zb->linesize);
		if (!zb->pbuf)
			exit(1);
		zb->frame_buffer_allocated = 1;
	} else {
		zb->pbuf = frame_buffer;
		zb->frame_buffer_allocated = 0;
	}
}

#if TGL_FEATURE_32_BITS == 1
 PIXEL pxReverse32(PIXEL x) {
	return
		((x & 0xFF000000) >> 24) | /*______AA*/
		((x & 0x00FF0000) >> 8) |  /*____RR__*/
		((x & 0x0000FF00) << 8) |  /*__GG____*/
		((x & 0x000000FF) << 24);  /* BB______*/
}
#endif

static void ZB_copyBuffer(ZBuffer* zb, void* buf, GLint linesize) {
	GLint y, i;
	// #560: `linesize` is the CALLER's destination row stride in bytes (e.g. the
	// real framebuffer pitch), which is NOT guaranteed to equal the source row
	// size (zb->xsize * PSZB) - a caller whose destination is wider than the
	// zbuffer (letterboxing, or gldemo.c's MAXW/MAXH internal clamp leaving
	// gldemo's w/h smaller than the real screen) used to hand a larger
	// `linesize` straight to memcpy() as the copy LENGTH, over-reading past the
	// end of each source row (and, on the last row, past the end of the pbuf
	// heap allocation entirely - a real OOB read that could crash or leak
	// adjacent heap bytes onto the screen). Bound the actual bytes copied per
	// row to the smaller of the two; `linesize` still governs the destination
	// row-to-row stride so partial-width blits land in the right place.
	GLint rowbytes = zb->xsize * (GLint)sizeof(PIXEL);
	if (rowbytes > linesize) rowbytes = linesize;
#if TGL_FEATURE_MULTITHREADED_ZB_COPYBUFFER == 1
#ifdef _OPENMP
#pragma omp parallel for
#endif
	for (y = 0; y < zb->ysize; y++) {
		PIXEL* q;
		GLubyte* p1;
		q = zb->pbuf + y * zb->xsize;
		p1 = (GLubyte*)buf + y * linesize;
#if TGL_FEATURE_NO_COPY_COLOR == 1
		for (i = 0; i < zb->xsize; i++) {
			if ((*(q + i) & TGL_COLOR_MASK) != TGL_NO_COPY_COLOR)
				*(((PIXEL*)p1) + i) = *(q + i);
		}
#else
		memcpy(p1, q, rowbytes);
#endif


	}
#else
	for (y = 0; y < zb->ysize; y++) {
		PIXEL* q;
		GLubyte* p1;
		q = zb->pbuf + y * zb->xsize;
		p1 = (GLubyte*)buf + y * linesize;
#if TGL_FEATURE_NO_COPY_COLOR == 1
		for (i = 0; i < zb->xsize; i++) {
			if ((*(q + i) & TGL_COLOR_MASK) != TGL_NO_COPY_COLOR)
				*(((PIXEL*)p1) + i) = *(q + i);
		}
#else
		memcpy(p1, q, rowbytes);
#endif
	}
#endif
}

/* ------------------------------------------------------------------
 * T0(a) #578: shared half-resolution render support.
 *
 * A single float-free nearest-neighbor upscaler plus a global
 * render-scale (num/den) so the three heavy GL apps (openarena +
 * assaultcube sdlshim, arena) do NOT each hand-roll the same scale
 * math (CLAUDE.md reuse rule). The apps open their ZBuffer at
 * ZB_scaleDim(window_dim) instead of the raw window dim (1/4 the
 * pixels at 0.5x each axis = ~1/4 raster fillrate), then upscale the
 * small pbuf into their existing full-window blit buffer at present
 * time via ZB_upscaleNearest. The compositor + kernel SYS_FB_FLIP
 * path are byte-for-byte unchanged (still a full-window buffer).
 *
 * Kept int (fixed-point) for speed. This is userland, so float is
 * legal here, but nearest-neighbor needs no float and int is faster.
 * Default is 2/2 = 1.0 = OFF, so the exact current 1:1 present fast
 * path is preserved until a config file / menu opts in.
 * ------------------------------------------------------------------ */
static int g_tgl_scale_num = 2;
static int g_tgl_scale_den = 2;

void ZB_setRenderScale(int num, int den) {
	/* Clamp to [1/4 .. 1/1]. Reject nonsense (den<=0 or num<=0). */
	if (num <= 0 || den <= 0) {
		g_tgl_scale_num = 1;
		g_tgl_scale_den = 1;
		return;
	}
	/* num/den must be within [1/4, 1]. */
	if (num * 4 < den) {        /* < 1/4 -> clamp to 1/4 */
		num = 1;
		den = 4;
	}
	if (num > den) {            /* > 1/1 -> clamp to 1/1 */
		num = 1;
		den = 1;
	}
	g_tgl_scale_num = num;
	g_tgl_scale_den = den;
}

/* True when a non-1.0 render scale is in effect. Lets an app keep its
 * exact current 1:1 present path when scale is OFF (byte-identical),
 * and only diverge to the upscale path when half-res is active. */
int ZB_renderScaleActive(void) {
	return g_tgl_scale_num != g_tgl_scale_den;
}

/* Return the render dimension for a given full window dimension,
 * floored to a multiple of 4 so it agrees with ZB_open's own
 * `xsize & ~3` and the ztriangle inner-loop stride. Never returns 0
 * (a zero-size ZBuffer would divide-by-zero in the upscaler). */
int ZB_scaleDim(int full) {
	int r;
	if (full <= 0)
		return 4;
	r = (full * g_tgl_scale_num) / g_tgl_scale_den;
	r &= ~3;
	if (r < 4)
		r = 4;
	return r;
}

/* Nearest-neighbor upscale of a src (sw x sh) PIXEL image into dst
 * (dw x dh) with destination row stride `dstride` PIXELs. All-int
 * fixed-point (xr/yr in 16.16). Forces opaque alpha (|0xFF000000)
 * to match every present path's convention. Cannot over-read src:
 * sx<sw and sy<sh are derived from dst counters via >>16, so the
 * index sy*sw+sx is always in [0, sw*sh). Guards div-by-zero. */
void ZB_upscaleNearest(const PIXEL *src, int sw, int sh,
					   PIXEL *dst, int dw, int dh, int dstride) {
	int x, y, xr, yr;
	if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return;
	xr = (sw << 16) / dw;
	yr = (sh << 16) / dh;
	for (y = 0; y < dh; y++) {
		int sy = (y * yr) >> 16;
		const PIXEL *srow;
		PIXEL *drow = dst + (GLint)y * dstride;
		if (sy >= sh) sy = sh - 1;
		srow = src + (GLint)sy * sw;
		for (x = 0; x < dw; x++) {
			int sx = (x * xr) >> 16;
			if (sx >= sw) sx = sw - 1;
			drow[x] = srow[sx] | 0xFF000000u;
		}
	}
}

#if TGL_FEATURE_RENDER_BITS == 16

/* 32 bpp copy */
/*

#ifdef TGL_FEATURE_32_BITS

#define RGB16_TO_RGB32(p0,p1,v)\
{\
	GLuint g,b,gb;\
	g = (v & 0x07E007E0) << 5;\
	b = (v & 0x001F001F) << 3;\
	gb = g | b;\
	p0 = (gb & 0x0000FFFF) | ((v & 0x0000F800) << 8);\
	p1 = (gb >> 16) | ((v & 0xF8000000) >> 8);\
}


static void ZB_copyFrameBufferRGB32(ZBuffer * zb,
									void *buf,
									GLint linesize)
{
	GLushort *q;
	GLuint *p, *p1, v, w0, w1;
	GLint y, n;

	q = zb->pbuf;
	p1 = (GLuint *) buf;
	
	for (y = 0; y < zb->ysize; y++) {
	p = p1;
	n = zb->xsize >> 2;
	do {
		v = *(GLuint *) q;
		RGB16_TO_RGB32(w1, w0, v);
		p[0] = w0;
		p[1] = w1;
		v = *(GLuint *) (q + 2);
		RGB16_TO_RGB32(w1, w0, v);
		p[2] = w0;
		p[3] = 0;

		q += 4;
		p += 4;
	} while (--n > 0);

	p1 += linesize;
	}
}
*/
#endif

/* 24 bit packed pixel handling */

#ifdef TGL_FEATURE_24_BITS

/* order: RGBR GBRG BRGB */

/* XXX: packed pixel 24 bit support not tested */
/* XXX: big endian case not optimised */
/*
#if BYTE_ORDER == BIG_ENDIAN

#define RGB16_TO_RGB24(p0,p1,p2,v1,v2)\
{\
	GLuint r1,g1,b1,gb1,g2,b2,gb2;\
	v1 = (v1 << 16) | (v1 >> 16);\
	v2 = (v2 << 16) | (v2 >> 16);\
	r1 = (v1 & 0xF800F800);\
	g1 = (v1 & 0x07E007E0) << 5;\
	b1 = (v1 & 0x001F001F) << 3;\
	gb1 = g1 | b1;\
	p0 = ((gb1 & 0x0000FFFF) << 8) | (r1 << 16) | (r1 >> 24);\
	g2 = (v2 & 0x07E007E0) << 5;\
	b2 = (v2 & 0x001F001F) << 3;\
	gb2 = g2 | b2;\
	p1 = (gb1 & 0xFFFF0000) | (v2 & 0xF800) | ((gb2 >> 8) & 0xff);\
	p2 = (gb2 << 24) | ((v2 & 0xF8000000) >> 8) | (gb2 >> 16);\
}

#else

#define RGB16_TO_RGB24(p0,p1,p2,v1,v2)\
{\
	GLuint r1,g1,b1,gb1,g2,b2,gb2;\
	r1 = (v1 & 0xF800F800);\
	g1 = (v1 & 0x07E007E0) << 5;\
	b1 = (v1 & 0x001F001F) << 3;\
	gb1 = g1 | b1;\
	p0 = ((gb1 & 0x0000FFFF) << 8) | (r1 << 16) | (r1 >> 24);\
	g2 = (v2 & 0x07E007E0) << 5;\
	b2 = (v2 & 0x001F001F) << 3;\
	gb2 = g2 | b2;\
	p1 = (gb1 & 0xFFFF0000) | (v2 & 0xF800) | ((gb2 >> 8) & 0xff);\
	p2 = (gb2 << 24) | ((v2 & 0xF8000000) >> 8) | (gb2 >> 16);\
}

#endif
*/
/*
static void ZB_copyFrameBufferRGB24(ZBuffer * zb,
									void *buf,
									GLint linesize)
{
	GLushort *q;
	GLuint *p, *p1, w0, w1, w2, v0, v1;
	GLint y, n;

	q = zb->pbuf;
	p1 = (GLuint *) buf;
	linesize = linesize * 3;

	for (y = 0; y < zb->ysize; y++) {
	p = p1;
	n = zb->xsize >> 2;
	do {
		v0 = *(GLuint *) q;
		v1 = *(GLuint *) (q + 2);
		RGB16_TO_RGB24(w0, w1, w2, v0, v1);
		p[0] = w0;
		p[1] = w1;
		p[2] = w2;

		q += 4;
		p += 3;
	} while (--n > 0);

	*((GLbyte *) p1) += linesize;
	}
}
*/
#endif

#if TGL_FEATURE_RENDER_BITS == 16

void ZB_copyFrameBuffer(ZBuffer* zb, void* buf, GLint linesize) {

	ZB_copyBuffer(zb, buf, linesize);
}

#endif 
/*^ TGL_FEATURE_RENDER_BITS == 16 */


#if TGL_FEATURE_RENDER_BITS == 32

#define RGB32_TO_RGB16(v) (((v >> 8) & 0xf800) | (((v) >> 5) & 0x07e0) | (((v)&0xff) >> 3))


void ZB_copyFrameBuffer(ZBuffer* zb, void* buf, GLint linesize) {
	ZB_copyBuffer(zb, buf, linesize);
}

#endif 
/* ^TGL_FEATURE_RENDER_BITS == 32 */

/*
 * adr must be aligned on an 'int'
 *
 * #578 T0(d): this is the z-buffer clear, called once per glClear() (i.e.
 * once per rendered frame in every GL app: glcube/glmatrix/the ten gldemo
 * screensaver cores/openarena/assaultcube/arena) over the FULL zbuffer
 * (zb->xsize * zb->ysize GLushorts), so it is pure O(w*h) unconditional
 * work with no data-dependent branch - the single safest possible SIMD
 * target in this file (unlike the triangle rasterizer's per-pixel z-test,
 * which this change does NOT touch; see ztriangle.c/.h comments on how
 * delicate that code is). The scalar loop below already got compiled to
 * one 16-byte SSE `movups` per unrolled group of 4 by GCC's -O2 SLP
 * vectorizer (verified by disassembly during this task), so hand-writing
 * intrinsics for the SAME 16-byte width would have been a no-op; the win
 * here comes from explicitly processing 4 x 16 bytes (64 bytes) per loop
 * iteration, which cuts loop-branch/pointer-increment overhead 4x versus
 * relying on the compiler to pick an unroll factor, and makes the
 * vectorization deliberate rather than an accident of `-O2 -ftree-slp-vectorize`
 * defaults that could regress under a future compiler/flag change.
 * `_mm_storeu_si128` (unaligned) is used throughout even though every
 * call site in this file happens to be 16-byte aligned today (gl_malloc
 * -> libc malloc guarantees 16-byte alignment, and zb->linesize /
 * zb->xsize are always multiples of 4 GLuints / 16 bytes per ZB_open's
 * `xsize & ~3`) - unaligned stores cost nothing extra on any CPU this OS
 * targets (real iMac14,4 / QEMU kvm64, both well past the Core2-era
 * unaligned-SSE penalty) and removes a whole class of alignment-assumption
 * bugs if that invariant ever changes. Pixel-for-pixel output is
 * unchanged: every byte written and its value is identical to the
 * scalar path, just fewer/wider store instructions.
 */
static void memset_s(void* adr, GLint val, GLint count) {
#if defined(__SSE2__)
	GLuint* p = (GLuint*)adr;
	GLuint v = (GLuint)val | ((GLuint)val << 16);
	__m128i vv = _mm_set1_epi32((int)v);
	/* count is in GLushort units; one __m128i store covers 16 bytes = 8
	 * GLushorts, matching the original "n = count >> 3" chunking. */
	GLint n16 = count >> 3;
	GLint groups = n16 >> 2;   /* 4 stores (64 bytes) per loop iteration */
	GLint i;
	for (i = 0; i < groups; i++) {
		_mm_storeu_si128((__m128i*)(p + 0), vv);
		_mm_storeu_si128((__m128i*)(p + 4), vv);
		_mm_storeu_si128((__m128i*)(p + 8), vv);
		_mm_storeu_si128((__m128i*)(p + 12), vv);
		p += 16;
	}
	GLint rem16 = n16 & 3;
	for (i = 0; i < rem16; i++) {
		_mm_storeu_si128((__m128i*)p, vv);
		p += 4;
	}
	{
		GLushort* q = (GLushort*)p;
		GLint tail = count & 7;
		for (i = 0; i < tail; i++)
			*q++ = (GLushort)val;
	}
#else
	GLint i, n, v;
	GLuint* p;
	GLushort* q;

	p = adr;
	v = val | (val << 16);

	n = count >> 3;
	for (i = 0; i < n; i++) {
		p[0] = v;
		p[1] = v;
		p[2] = v;
		p[3] = v;
		p += 4;
	}

	q = (GLushort*)p;
	n = count & 7;
	for (i = 0; i < n; i++)
		*q++ = val;
#endif
}

/* Used in 32 bit mode. Same treatment as memset_s above (ZB_clear's
 * color-buffer clear path, one row of zb->xsize GLuints, called
 * zb->ysize times per frame -> the full framebuffer, once per frame). */
static void memset_l(void* adr, GLint val, GLint count) {
#if defined(__SSE2__)
	GLuint* p = (GLuint*)adr;
	__m128i vv = _mm_set1_epi32((int)val);
	/* count is in GLuint units; one __m128i store covers 4 of them,
	 * matching the original "n = count >> 2" chunking. */
	GLint n4 = count >> 2;
	GLint groups = n4 >> 2;    /* 4 stores (64 bytes) per loop iteration */
	GLint i;
	for (i = 0; i < groups; i++) {
		_mm_storeu_si128((__m128i*)(p + 0), vv);
		_mm_storeu_si128((__m128i*)(p + 4), vv);
		_mm_storeu_si128((__m128i*)(p + 8), vv);
		_mm_storeu_si128((__m128i*)(p + 12), vv);
		p += 16;
	}
	GLint rem4 = n4 & 3;
	for (i = 0; i < rem4; i++) {
		_mm_storeu_si128((__m128i*)p, vv);
		p += 4;
	}
	{
		GLint tail = count & 3;
		for (i = 0; i < tail; i++)
			*p++ = val;
	}
#else
	GLint i, n, v;
	GLuint* p;
	p = adr;
	v = val;
	n = count >> 2;
	for (i = 0; i < n; i++) {
		p[0] = v;
		p[1] = v;
		p[2] = v;
		p[3] = v;
		p += 4;
	}
	n = count & 3;
	for (i = 0; i < n; i++)
		*p++ = val;
#endif
}

void ZB_clear(ZBuffer* zb, GLint clear_z, GLint z, GLint clear_color, GLint r, GLint g, GLint b) {
	GLuint color;
	GLint y;
	PIXEL* pp;
	if (clear_z) {
		memset_s(zb->zbuf, z, zb->xsize * zb->ysize);
	}
	if (clear_color) {
		pp = zb->pbuf;
		for (y = 0; y < zb->ysize; y++) {
#if TGL_FEATURE_RENDER_BITS == 15 || TGL_FEATURE_RENDER_BITS == 16
			// color = RGB_TO_PIXEL(r, g, b);
#if TGL_FEATURE_FORCE_CLEAR_NO_COPY_COLOR
			color = TGL_NO_COPY_COLOR;
#else
			color = RGB_TO_PIXEL(r, g, b);
#endif
			memset_s(pp, color, zb->xsize);
#elif TGL_FEATURE_RENDER_BITS == 32
#if TGL_FEATURE_FORCE_CLEAR_NO_COPY_COLOR
			color = TGL_NO_COPY_COLOR;
#else
			color = RGB_TO_PIXEL(r, g, b);
#endif
			memset_l(pp, color, zb->xsize);
#else
#error BADJUJU
#endif
			pp = (PIXEL*)((GLbyte*)pp + zb->linesize);
		}
	}
}
