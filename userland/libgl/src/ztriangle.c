#include "../include/zbuffer.h"
#include "msghandling.h"
#include <stdlib.h>




/* TODO: Switch from scanline rasterizer to easily parallelized cross product rasterizer.*/
static GLfloat edgeFunction(GLfloat ax, GLfloat ay, GLfloat bx, GLfloat by, GLfloat cx, GLfloat cy) {
	return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

#if TGL_FEATURE_RENDER_BITS == 32
#elif TGL_FEATURE_RENDER_BITS == 16
#else
#error "WRONG MODE!!!"
#endif

#if TGL_FEATURE_POLYGON_STIPPLE == 1

#define TGL_STIPPLEVARS                                                                                                                                        \
	GLubyte* zbstipplepattern = zb->stipplepattern;                                                                                                            \
	GLubyte zbdostipple = zb->dostipple;
#define THE_X ((GLint)(pp - pp1))
#define XSTIP(_a) ((THE_X + _a) & TGL_POLYGON_STIPPLE_MASK_X)
#define YSTIP (the_y & TGL_POLYGON_STIPPLE_MASK_Y)
/* NOTES                                                           Divide by 8 to get the byte        Get the actual bit*/
#define STIPBIT(_a) (zbstipplepattern[(XSTIP(_a) | (YSTIP << TGL_POLYGON_STIPPLE_POW2_WIDTH)) >> 3] & (1 << (XSTIP(_a) & 7)))
#define STIPTEST(_a) &&(!(zbdostipple && !STIPBIT(_a)))

#else

#define TGL_STIPPLEVARS /* a comment */
#define STIPTEST(_a)	/* a comment*/

#endif

#if TGL_FEATURE_NO_DRAW_COLOR == 1
#define NODRAWTEST(c) &&((c & TGL_COLOR_MASK) != TGL_NO_DRAW_COLOR)
#else
#define NODRAWTEST(c) /* a comment */
#endif

/* AssaultCube port phase 2: real glDepthFunc. Previously this whole file
   hardcoded the depth comparison to "z >= zpix" (GL_GEQUAL-equivalent) with
   no way to change it. zb->depth_func defaults to GL_GEQUAL (see init.c),
   so any caller that never calls glDepthFunc gets byte-identical behavior
   to before this change. One definition point, used by every rasterizer
   variant in this file via ZCMP/ZCMPSIMP, so no per-variant duplication. */
static inline GLint zb_depth_test(GLenum func, GLuint z, GLuint zpix) {
	switch (func) {
	case GL_NEVER:    return 0;
	case GL_LESS:     return z < zpix;
	case GL_EQUAL:    return z == zpix;
	case GL_LEQUAL:   return z <= zpix;
	case GL_GREATER:  return z > zpix;
	case GL_NOTEQUAL: return z != zpix;
	case GL_ALWAYS:   return 1;
	case GL_GEQUAL:
	default:          return z >= zpix; /* original TinyGL behavior */
	}
}

#define ZCMP(z, zpix, _a, c) (((!zbdt) || zb_depth_test(zb->depth_func, (z), (zpix))) STIPTEST(_a) NODRAWTEST(c))
#define ZCMPSIMP(z, zpix, _a, crabapple) (((!zbdt) || zb_depth_test(zb->depth_func, (z), (zpix))) STIPTEST(_a))

void ZB_fillTriangleFlat(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	GLubyte zbdt = zb->depth_test;
	GLubyte zbdw = zb->depth_write;
	GLuint color;
	TGL_BLEND_VARS
	TGL_STIPPLEVARS

#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ

#define INTERP_Z


#define DRAW_INIT()                                                                                                                                            \
	{ color = RGB_TO_PIXEL(p2->r, p2->g, p2->b); }

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, color)) {                                                                                                             \
				TGL_BLEND_FUNC(color, (pp[_a])) /*pp[_a] = color;*/                                                                                            \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
	}

#include "ztriangle.h"
}

void ZB_fillTriangleFlatNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL color = RGB_TO_PIXEL(p2->r, p2->g, p2->b);
	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS
#undef INTERP_Z
#undef INTERP_RGB
#undef INTERP_ST
#undef INTERP_STZ
#define INTERP_Z

#define DRAW_INIT()                                                                                                                                            \
	{}

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = color;                                                                                                                                \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
	}

#include "ztriangle.h"
}

/*
 * Smooth filled triangle.
 * The code below is very tricky :)
 */

void ZB_fillTriangleSmooth(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_BLEND_VARS
	TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define SAR_RND_TO_ZERO(v, n) (v / (1 << n))

#if TGL_FEATURE_RENDER_BITS == 32
#define DRAW_INIT()                                                                                                                                            \
	{}
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				/*pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);*/                                                                                                      \
				TGL_BLEND_FUNC_RGB(or1, og1, ob1, (pp[_a]));                                                                                                   \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}


#elif TGL_FEATURE_RENDER_BITS == 16

#define DRAW_INIT()                                                                                                                                            \
	{}

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				/*pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);*/                                                                                                      \
				TGL_BLEND_FUNC_RGB(or1, og1, ob1, (pp[_a]));                                                                                                   \
                                                                                                                                                               \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}

#endif

#include "ztriangle.h"
} 

void ZB_fillTriangleSmoothNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {

	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS

#define INTERP_Z
#define INTERP_RGB

#define SAR_RND_TO_ZERO(v, n) (v / (1 << n))

#if TGL_FEATURE_RENDER_BITS == 32
#define DRAW_INIT()                                                                                                                                            \
	{}

#if TGL_FEATURE_NO_DRAW_COLOR != 1
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);                                                                                                          \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			/*c = RGB_TO_PIXEL(or1, og1, ob1);*/                                                                                                               \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);                                                                                                          \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}
#endif

#elif TGL_FEATURE_RENDER_BITS == 16

#define DRAW_INIT()                                                                                                                                            \
	{}

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_TO_PIXEL(or1, og1, ob1);                                                                                                          \
                                                                                                                                                               \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		og1 += dgdx;                                                                                                                                           \
		or1 += drdx;                                                                                                                                           \
		ob1 += dbdx;                                                                                                                                           \
	}

#endif
/* End of 16 bit mode stuff*/
#include "ztriangle.h"
} 

/*


			TEXTURE MAPPED TRIANGLES
               Section_Header




*/
void ZB_setTexture(ZBuffer* zb, PIXEL* texture) { zb->current_texture = texture; }


#if 1

#define DRAW_LINE_TRI_TEXTURED()                                                                                                                               \
	{                                                                                                                                                          \
		register GLushort* pz;                                                                                                                                 \
		register PIXEL* pp;                                                                                                                                    \
		register GLuint s, t, z;                                                                                                                               \
		register GLint n;                                                                                                                                      \
		OR1OG1OB1DECL                                                                                                                                          \
		GLfloat sz, tz, fzl, zinv;                                                                                                                             \
		n = (x2 >> 16) - x1;                                                                                                                                   \
		fzl = (GLfloat)z1;                                                                                                                                     \
		zinv = 1.0 / fzl;                                                                                                                                      \
		pp = (PIXEL*)((GLbyte*)pp1 + x1 * PSZB);                                                                                                               \
		pz = pz1 + x1;                                                                                                                                         \
		z = z1;                                                                                                                                                \
		sz = sz1;                                                                                                                                              \
		tz = tz1;                                                                                                                                              \
		while (n >= (NB_INTERP - 1)) {                                                                                                                         \
			register GLint dsdx, dtdx;                                                                                                                         \
			{                                                                                                                                                  \
				GLfloat ss, tt;                                                                                                                                \
				ss = (sz * zinv);                                                                                                                              \
				tt = (tz * zinv);                                                                                                                              \
				s = (GLint)ss;                                                                                                                                 \
				t = (GLint)tt;                                                                                                                                 \
				dsdx = (GLint)((dszdx - ss * fdzdx) * zinv);                                                                                                   \
				dtdx = (GLint)((dtzdx - tt * fdzdx) * zinv);                                                                                                   \
			}                                                                                                                                                  \
			fzl += fndzdx;                                                                                                                                     \
			zinv = 1.0 / fzl;                                                                                                                                  \
			PUT_PIXEL(0); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(1); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(2); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(3); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(4); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(5); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(6); /*the_x++;*/                                                                                                                         \
			PUT_PIXEL(7); /*the_x-=7;*/                                                                                                                        \
			pz += NB_INTERP;                                                                                                                                   \
			pp += NB_INTERP; /*the_x+=NB_INTERP * PSZB;*/                                                                                                      \
			n -= NB_INTERP;                                                                                                                                    \
			sz += ndszdx;                                                                                                                                      \
			tz += ndtzdx;                                                                                                                                      \
		}                                                                                                                                                      \
		{                                                                                                                                                      \
			register GLint dsdx, dtdx;                                                                                                                         \
			{                                                                                                                                                  \
				GLfloat ss, tt;                                                                                                                                \
				ss = (sz * zinv);                                                                                                                              \
				tt = (tz * zinv);                                                                                                                              \
				s = (GLint)ss;                                                                                                                                 \
				t = (GLint)tt;                                                                                                                                 \
				dsdx = (GLint)((dszdx - ss * fdzdx) * zinv);                                                                                                   \
				dtdx = (GLint)((dtzdx - tt * fdzdx) * zinv);                                                                                                   \
			}                                                                                                                                                  \
			while (n >= 0) {                                                                                                                                   \
				PUT_PIXEL(0);                                                                                                                                  \
				pz += 1;                                                                                                                                       \
				/*pp = (PIXEL*)((GLbyte*)pp + PSZB);*/                                                                                                         \
				pp++;                                                                                                                                          \
				n -= 1;                                                                                                                                        \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
	} 

void ZB_fillTriangleMappingPerspective(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL* texture;

	// task #578: real, measured crash (a Page Fault, CR2 near NULL, live in
	// OpenArena rendering the oa_dm1 world geometry) proved this function
	// gets called with zb->current_texture == NULL - the caller selected the
	// textured/perspective fill path for a surface whose shader stage has no
	// bound image (e.g. a missing texture; this port's reduced pak subset
	// does not include pak4-textures.pk3), but nothing here ever checked
	// for that before TEXTURE_SAMPLE(texture, s, t) indexed off a NULL
	// pointer. Every OTHER fill routine in this file (Flat/Smooth) draws a
	// solid color with no texture involved at all, so they were never at
	// risk; only the two texture-mapped variants (this one and its NOBLEND
	// sibling below) read through `texture`. Bail out (skip the triangle,
	// draw nothing) rather than crash - this is a shared TinyGL primitive
	// used by every ported game (AssaultCube, Quake, OpenArena), so the fix
	// belongs here, not forked into any one port. A deeper fix (make the
	// caller fall back to flat-shaded white instead of skipping) is a
	// reasonable follow-up but not required to stop the crash.
	if (!zb->current_texture) return;

	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_BLEND_VARS
	TGL_STIPPLEVARS
#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB


#define NB_INTERP 8

#define DRAW_INIT()                                                                                                                                            \
	{                                                                                                                                                          \
		texture = zb->current_texture;                                                                                                                         \
		fdzdx = (GLfloat)dzdx;                                                                                                                                 \
		fndzdx = NB_INTERP * fdzdx;                                                                                                                            \
		ndszdx = NB_INTERP * dszdx;                                                                                                                            \
		ndtzdx = NB_INTERP * dtzdx;                                                                                                                            \
	}
#if TGL_FEATURE_LIT_TEXTURES == 1
#define OR1OG1OB1DECL                                                                                                                                          \
	register GLint or1, og1, ob1;                                                                                                                              \
	or1 = r1;                                                                                                                                                  \
	og1 = g1;                                                                                                                                                  \
	ob1 = b1;
#define OR1G1B1INCR                                                                                                                                            \
	og1 += dgdx;                                                                                                                                               \
	or1 += drdx;                                                                                                                                               \
	ob1 += dbdx;
#else
#define OR1OG1OB1DECL /*A comment*/
#define OR1G1B1INCR   /*Another comment*/
#define or1 COLOR_MULT_MASK
#define og1 COLOR_MULT_MASK
#define ob1 COLOR_MULT_MASK
#endif
#if TGL_FEATURE_NO_DRAW_COLOR != 1

#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				/*pp[_a] = RGB_MIX_FUNC(or1, og1, ob1, TEXTURE_SAMPLE(texture, s, t));*/                                                                       \
				TGL_BLEND_FUNC(RGB_MIX_FUNC(or1, og1, ob1, (TEXTURE_SAMPLE(texture, s, t))), (pp[_a]));                                                        \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			PIXEL c = TEXTURE_SAMPLE(texture, s, t);                                                                                                           \
			if (ZCMP(zz, pz[_a], _a, c)) {                                                                                                                     \
				TGL_BLEND_FUNC(RGB_MIX_FUNC(or1, og1, ob1, c), (pp[_a]));                                                                                      \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#endif
#define DRAW_LINE()                                                                                                                                            \
	{ DRAW_LINE_TRI_TEXTURED() }

#include "ztriangle.h"
}

void ZB_fillTriangleMappingPerspectiveNOBLEND(ZBuffer* zb, ZBufferPoint* p0, ZBufferPoint* p1, ZBufferPoint* p2) {
	PIXEL* texture;

	// task #578: same NULL-texture guard as ZB_fillTriangleMappingPerspective
	// above (see that comment); this sibling reads through `texture` the
	// same unchecked way.
	if (!zb->current_texture) return;

	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	TGL_STIPPLEVARS
#define INTERP_Z
#define INTERP_STZ
#define INTERP_RGB

#define NB_INTERP 8

#define DRAW_INIT()                                                                                                                                            \
	{                                                                                                                                                          \
		texture = zb->current_texture;                                                                                                                         \
		fdzdx = (GLfloat)dzdx;                                                                                                                                 \
		fndzdx = NB_INTERP * fdzdx;                                                                                                                            \
		ndszdx = NB_INTERP * dszdx;                                                                                                                            \
		ndtzdx = NB_INTERP * dtzdx;                                                                                                                            \
	}
#if TGL_FEATURE_LIT_TEXTURES == 1
#define OR1OG1OB1DECL                                                                                                                                          \
	register GLint or1, og1, ob1;                                                                                                                              \
	or1 = r1;                                                                                                                                                  \
	og1 = g1;                                                                                                                                                  \
	ob1 = b1;
#define OR1G1B1INCR                                                                                                                                            \
	og1 += dgdx;                                                                                                                                               \
	or1 += drdx;                                                                                                                                               \
	ob1 += dbdx;
#else
#define OR1OG1OB1DECL /*A comment*/
#define OR1G1B1INCR   /*Another comment*/
#define or1 COLOR_MULT_MASK
#define og1 COLOR_MULT_MASK
#define ob1 COLOR_MULT_MASK
#endif
#if TGL_FEATURE_NO_DRAW_COLOR != 1
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			if (ZCMPSIMP(zz, pz[_a], _a, 0)) {                                                                                                                 \
				pp[_a] = RGB_MIX_FUNC(or1, og1, ob1, TEXTURE_SAMPLE(texture, s, t));                                                                           \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#else
#define PUT_PIXEL(_a)                                                                                                                                          \
	{                                                                                                                                                          \
		{                                                                                                                                                      \
			register GLuint zz = z >> ZB_POINT_Z_FRAC_BITS;                                                                                                    \
			PIXEL c = TEXTURE_SAMPLE(texture, s, t);                                                                                                           \
			if (ZCMP(zz, pz[_a], _a, c)) {                                                                                                                     \
				pp[_a] = RGB_MIX_FUNC(or1, og1, ob1, c);                                                                                                       \
				/*TGL_BLEND_FUNC(RGB_MIX_FUNC(or1, og1, ob1, c), (pp[_a]));*/                                                                                  \
				if (zbdw)                                                                                                                                      \
					pz[_a] = zz;                                                                                                                               \
			}                                                                                                                                                  \
		}                                                                                                                                                      \
		z += dzdx;                                                                                                                                             \
		s += dsdx;                                                                                                                                             \
		t += dtdx;                                                                                                                                             \
		OR1G1B1INCR                                                                                                                                            \
	}
#endif
#define DRAW_LINE()                                                                                                                                            \
	{ DRAW_LINE_TRI_TEXTURED() }
#include "ztriangle.h"
}

#endif 
