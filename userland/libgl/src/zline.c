#include "../include/zbuffer.h"
#include <stdlib.h>

#define ZCMP(z, zpix) (!(zbdt) || z >= (zpix))

/* TODO: Implement point size. */
/* TODO: Implement blending for lines and points. */

void ZB_plot(ZBuffer* zb, ZBufferPoint* p) {

	GLint zz, y, x;
	GLubyte zbdw = zb->depth_write;
	GLubyte zbdt = zb->depth_test;
	GLfloat zbps = zb->pointsize;
	TGL_BLEND_VARS
	zz = p->z >> ZB_POINT_Z_FRAC_BITS;
	
	if (zbps == 1) {
		GLushort* pz;
		PIXEL* pp;
		// #560-followup: a point whose projected (x,y) lands outside the
		// buffer (near-clip W close to zero, or a rounding edge case at the
		// viewport boundary) used to be written with NO bounds check at all,
		// corrupting whatever heap/bss memory follows zb->zbuf/zb->pbuf -
		// the same missing-bounds-check class as the ZB_copyBuffer bug fixed
		// earlier (#560 audit). Reject it here instead of walking off the
		// end of the buffer.
		if (p->x < 0 || p->x >= zb->xsize || p->y < 0 || p->y >= zb->ysize)
			return;
		pz = zb->zbuf + (p->y * zb->xsize + p->x);
		pp = (PIXEL*)((GLbyte*)zb->pbuf + zb->linesize * p->y + p->x * PSZB);

		if (ZCMP(zz, *pz)) {
#if TGL_FEATURE_BLEND == 1
			if (!zb->enable_blend)
				*pp = RGB_TO_PIXEL(p->r, p->g, p->b);
			else
				TGL_BLEND_FUNC_RGB(p->r, p->g, p->b, (*pp))
#else
			*pp = RGB_TO_PIXEL(p->r, p->g, p->b);
#endif
			if (zbdw)
				*pz = zz;
		}
	} else {
		PIXEL col = RGB_TO_PIXEL(p->r, p->g, p->b);
		GLfloat hzbps = zbps / 2.0f;
		GLint bx = (GLfloat)p->x - hzbps;
		GLint ex = (GLfloat)p->x + hzbps;
		GLint by = (GLfloat)p->y - hzbps;
		GLint ey = (GLfloat)p->y + hzbps;
		bx = (bx < 0) ? 0 : bx;
		by = (by < 0) ? 0 : by;
		ex = (ex > zb->xsize) ? zb->xsize : ex;
		ey = (ey > zb->ysize) ? zb->ysize : ey;
		for (y = by; y < ey; y++)
			for (x = bx; x < ex; x++) {
				GLushort* pz = zb->zbuf + (y * zb->xsize + x);
				PIXEL* pp = (PIXEL*)((GLbyte*)zb->pbuf + zb->linesize * y + x * PSZB);
				
				if (ZCMP(zz, *pz)) {
#if TGL_FEATURE_BLEND == 1
					if (!zb->enable_blend)
						*pp = col;
					else
						TGL_BLEND_FUNC_RGB(p->r, p->g, p->b, (*pp))
#else
					*pp = col;
#endif
					if (zbdw)
						*pz = zz;
				}
			}
	}
}

#define INTERP_Z
static void ZB_line_flat_z(ZBuffer* zb, ZBufferPoint* p1, ZBufferPoint* p2, GLint color) {
	
	GLubyte zbdt = zb->depth_test;
	GLubyte zbdw = zb->depth_write;
#include "zline.h"
}

/* line with color GLinterpolation */
#define INTERP_Z
#define INTERP_RGB
static void ZB_line_interp_z(ZBuffer* zb, ZBufferPoint* p1, ZBufferPoint* p2) {
	
	GLubyte zbdt = zb->depth_test;
	GLubyte zbdw = zb->depth_write;
#include "zline.h"
}

/* no Z GLinterpolation */

static void ZB_line_flat(ZBuffer* zb, ZBufferPoint* p1, ZBufferPoint* p2, GLint color) {
	
	
#include "zline.h"
}

#define INTERP_RGB
static void ZB_line_interp(ZBuffer* zb, ZBufferPoint* p1, ZBufferPoint* p2) {

#include "zline.h"
}

void ZB_line_z(ZBuffer* zb, ZBufferPoint* p1, ZBufferPoint* p2) {
	GLint color1, color2;
	
	color1 = RGB_TO_PIXEL(p1->r, p1->g, p1->b);
	color2 = RGB_TO_PIXEL(p2->r, p2->g, p2->b);

	/* choose if the line should have its color GLinterpolated or not */
	if (color1 == color2) {
		ZB_line_flat_z(zb, p1, p2, color1);
	} else {
		ZB_line_interp_z(zb, p1, p2);
	}
}

void ZB_line(ZBuffer* zb, ZBufferPoint* p1, ZBufferPoint* p2) {
	GLint color1, color2;

	color1 = RGB_TO_PIXEL(p1->r, p1->g, p1->b);
	color2 = RGB_TO_PIXEL(p2->r, p2->g, p2->b);

	/* choose if the line should have its color GLinterpolated or not */
	if (color1 == color2) {
		ZB_line_flat(zb, p1, p2, color1);
	} else {
		ZB_line_interp(zb, p1, p2);
	}
}
