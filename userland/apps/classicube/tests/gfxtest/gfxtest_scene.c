/* gfxtest_scene.c - the scene + the exact-pixel checks (#28, gfx lane).
 *
 * PORTABLE. This file is compiled UNCHANGED into both the host harness and the
 * MayteraOS ELF, so what the host proves is the same code the OS runs.
 *
 * Design rule: every check compares a framebuffer word against a value that
 * comes from the scene definition, so a swapped channel, an off-by-one raster
 * rule, or a broken depth test all show up as a numeric failure. Nothing here
 * is verified by looking at a picture.
 */
#include "Core.h"
#include "Graphics.h"
#include "Bitmap.h"
#include "PackedCol.h"
#include "Vectors.h"
#include "ExtMath.h"
#include "Game.h"
#include "gfxtest.h"
#include "../../gfx/mos_gfx.h"

MOS_GFX_ASSERT_PIXEL_FORMAT();

extern struct IGameComponent Gfx_Component;

static struct gt_results* R;

/* ---------------------------------------------------------------------- */
/* Check plumbing                                                          */
/* ---------------------------------------------------------------------- */
static char msgbuf[192];

static char* put_str(char* p, const char* s) { while (*s) *p++ = *s++; return p; }

static char* put_hex(char* p, cc_uint32 v) {
	static const char* H = "0123456789ABCDEF";
	int i;
	*p++ = '0'; *p++ = 'x';
	for (i = 28; i >= 0; i -= 4) *p++ = H[(v >> i) & 0xF];
	return p;
}

static char* put_int(char* p, int v) {
	char tmp[12]; int n = 0;
	if (v < 0) { *p++ = '-'; v = -v; }
	if (!v) { *p++ = '0'; return p; }
	while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
	while (n) *p++ = tmp[--n];
	return p;
}

static BitmapCol px_at(int x, int y) { return gt_framebuffer()[y * GT_WIDTH + x]; }

/* Compare the RGB of a framebuffer word against an expected RGB. Alpha is
 * excluded deliberately: the rasteriser is free to leave any value there and
 * the compositor ignores it, so asserting on it would be asserting on noise. */
static void check_px(const char* what, int x, int y, cc_uint32 expect_rgb) {
	cc_uint32 got = px_at(x, y) & 0x00FFFFFFu;
	char* p = msgbuf;

	R->checks_run++;
	if (got == (expect_rgb & 0x00FFFFFFu)) return;

	R->checks_failed++;
	p = put_str(p, "FAIL ");
	p = put_str(p, what);
	p = put_str(p, " at (");
	p = put_int(p, x); *p++ = ',';
	p = put_int(p, y);
	p = put_str(p, ") expected ");
	p = put_hex(p, expect_rgb & 0x00FFFFFFu);
	p = put_str(p, " got ");
	p = put_hex(p, got);
	*p = '\0';
	gt_log(msgbuf);
}

static void check_int(const char* what, int got, int expect) {
	char* p = msgbuf;
	R->checks_run++;
	if (got == expect) return;

	R->checks_failed++;
	p = put_str(p, "FAIL ");
	p = put_str(p, what);
	p = put_str(p, " expected ");
	p = put_int(p, expect);
	p = put_str(p, " got ");
	p = put_int(p, got);
	*p = '\0';
	gt_log(msgbuf);
}

static void check_true(const char* what, int cond) { check_int(what, cond ? 1 : 0, 1); }

/* ---------------------------------------------------------------------- */
/* Scene constants                                                         */
/* ---------------------------------------------------------------------- */
/* EVERY colour is defined TWICE, from the same channel triple:
 *   V_*  = PackedCol,  what we hand the pipeline as a vertex/clear colour
 *   FB_* = BitmapCol,  what we require to come back out of the framebuffer
 * They are NOT the same bit layout in this build (see gfx/mos_gfx.h 1b), so
 * writing them separately is what makes the checks meaningful: a broken
 * PackedCol -> BitmapCol conversion anywhere in the backend shows up as a
 * numeric mismatch instead of quietly rendering a red world blue. */
#define CLEAR_R 0x20
#define CLEAR_G 0x30
#define CLEAR_B 0x40
#define V_CLEAR   MOS_PACKED_RGB(CLEAR_R, CLEAR_G, CLEAR_B)
#define FB_CLEAR  MOS_RGB(CLEAR_R, CLEAR_G, CLEAR_B)

/* Fully-saturated single-channel colours: the ONLY colours that make a channel
 * swap unambiguous. A grey or a pastel would survive a swap and tell us
 * nothing, which is exactly why the swapped kernel FB_COLOR macro went
 * unnoticed for so long (its call sites are all greys). */
#define V_RED     MOS_PACKED_RGB(255,   0,   0)
#define FB_RED    MOS_RGB(       255,   0,   0)
#define V_GREEN   MOS_PACKED_RGB(  0, 255,   0)
#define FB_GREEN  MOS_RGB(         0, 255,   0)
#define V_BLUE    MOS_PACKED_RGB(  0,   0, 255)
#define FB_BLUE   MOS_RGB(         0,   0, 255)

/* An asymmetric colour: catches a swap of ANY two channels. Pure primaries
 * catch R<->B, but only a value with three distinct channels catches G<->B. */
#define V_ASYM    MOS_PACKED_RGB(0x11, 0x77, 0xEE)

/* ---- MODULATED expectations -------------------------------------------
 * MEASURED, then explained, then encoded. An untextured 2D rect is not drawn
 * as a flat fill: Gfx_Draw2DFlat routes through DrawSprite2D, which samples
 * the bound texture (the 1x1 white one, because we Gfx_BindTexture(NULL)) and
 * modulates it by the vertex colour at Graphics_SoftGPU.c:426-434 using
 *        R = (vertexR * texelR) >> 8
 * A >>8 is a divide by 256, not by 255, so a full-intensity channel comes out
 * one LSB low: (255*255)>>8 == 254. Every one of the seven initially-failing
 * checks was off by exactly this amount (0xFF->0xFE, 0x11->0x10, 0x77->0x76,
 * 0xEE->0xED), which is what identified the mechanism.
 *
 * This is upstream rasteriser behaviour, present on every platform upstream
 * ships SoftGPU on, NOT a MayteraOS porting defect, and 1/255 is invisible.
 * So we encode the real formula rather than loosening to a tolerance: the
 * check stays EXACT, and if the rasteriser's modulate ever changes the test
 * says so precisely instead of shrugging.
 *
 * Note line 426's guard: the modulate is SKIPPED when the vertex colour is
 * PACKEDCOL_WHITE. That is why the textured quad below, drawn with
 * PACKEDCOL_WHITE, is expected exact while these are not.
 * ---------------------------------------------------------------------- */
#define MOD8(a, b)             (((a) * (b)) >> 8)
#define FB_MOD(r, g, b)        MOS_RGB(MOD8(r, 255), MOD8(g, 255), MOD8(b, 255))
#define FB_RED_MOD             FB_MOD(255,   0,   0)
#define FB_GREEN_MOD           FB_MOD(  0, 255,   0)
#define FB_BLUE_MOD            FB_MOD(  0,   0, 255)
#define FB_ASYM_MOD            FB_MOD(0x11, 0x77, 0xEE)

/* Rect geometry, chosen so sample points are far from every edge and no
 * rasteriser fill-rule tie-break can change the answer. */
#define RA_X 40
#define RA_Y 40
#define RA_W 160
#define RA_H 100

static GfxResourceID tex_checker;

/* 4x4 texture: four solid 2x2 quadrants, so a sample anywhere inside a
 * quadrant has one unambiguous expected value even after bilinear-free
 * nearest sampling and UV rounding. */
static BitmapCol checker_px[16];
#define TQ_TL MOS_RGB(255,   0,   0)
#define TQ_TR MOS_RGB(  0, 255,   0)
#define TQ_BL MOS_RGB(  0,   0, 255)
#define TQ_BR MOS_RGB(255, 255,   0)

static void build_checker(void) {
	int x, y;
	for (y = 0; y < 4; y++) {
		for (x = 0; x < 4; x++) {
			BitmapCol c;
			if (y < 2) c = (x < 2) ? TQ_TL : TQ_TR;
			else       c = (x < 2) ? TQ_BL : TQ_BR;
			/* Textures carry full alpha; the rasteriser's alpha test rejects
			 * zero-alpha texels. */
			checker_px[y * 4 + x] = c | 0xFF000000u;
		}
	}
}

/* ---------------------------------------------------------------------- */
/* Frame 1: clear + 2D primitives, exact-pixel verified                    */
/* ---------------------------------------------------------------------- */
static void frame_2d(void) {
	struct Texture t;

	Gfx_BeginFrame();
	Gfx_ClearColor(V_CLEAR);
	Gfx_ClearBuffers(GFX_BUFFER_COLOR | GFX_BUFFER_DEPTH);

	Gfx_Begin2D(GT_WIDTH, GT_HEIGHT);

	/* Untextured flat rects. Bind NULL so the 1x1 white texture is sampled
	 * and the result is the vertex colour verbatim. */
	Gfx_BindTexture(NULL);
	Gfx_Draw2DFlat(RA_X,       RA_Y, RA_W, RA_H, V_RED);
	Gfx_Draw2DFlat(RA_X + 200, RA_Y, RA_W, RA_H, V_GREEN);
	Gfx_Draw2DFlat(RA_X + 400, RA_Y, RA_W, RA_H, V_BLUE);
	Gfx_Draw2DFlat(RA_X,       200,  RA_W, RA_H, V_ASYM);

	/* Textured quad, stretched 4x4 -> 160x160 so each texel quadrant covers
	 * an 80x80 screen block. */
	t.ID     = tex_checker;
	t.x      = 260; t.y      = 200;
	t.width  = 160; t.height = 160;
	t.uv.u1  = 0.0f; t.uv.v1 = 0.0f;
	t.uv.u2  = 1.0f; t.uv.v2 = 1.0f;
	Gfx_BindTexture(tex_checker);
	Gfx_Draw2DTexture(&t, PACKEDCOL_WHITE);

	Gfx_End2D();
	Gfx_EndFrame();
	R->frames_2d++;
}

static void verify_2d(void) {
	/* Background survives where nothing was drawn. Also proves the
	 * PackedCol -> BitmapCol conversion inside Gfx_ClearColor is correct. */
	check_px("clear-topleft",     2,           2,           FB_CLEAR);
	check_px("clear-bottomright", GT_WIDTH - 3, GT_HEIGHT - 3, FB_CLEAR);

	/* THE CHANNEL-ORDER PROOF. If our buffer were RGBA-byte-order instead of
	 * BGRA, red would read back as 0x0000FF and this fails. */
	check_px("flat-red",   RA_X + RA_W / 2,       RA_Y + RA_H / 2, FB_RED_MOD);
	check_px("flat-green", RA_X + 200 + RA_W / 2, RA_Y + RA_H / 2, FB_GREEN_MOD);
	check_px("flat-blue",  RA_X + 400 + RA_W / 2, RA_Y + RA_H / 2, FB_BLUE_MOD);
	check_px("flat-asym",  RA_X + RA_W / 2,       200 + RA_H / 2,  FB_ASYM_MOD);

	/* Rect coverage: just inside each edge is filled, just outside is clear.
	 * This catches an off-by-one fill rule, which a centre sample cannot. */
	check_px("rect-inside-left",  RA_X + 1,          RA_Y + RA_H / 2, FB_RED_MOD);
	check_px("rect-inside-right", RA_X + RA_W - 2,   RA_Y + RA_H / 2, FB_RED_MOD);
	check_px("rect-inside-top",   RA_X + RA_W / 2,   RA_Y + 1,        FB_RED_MOD);
	check_px("rect-outside-left", RA_X - 2,          RA_Y + RA_H / 2, FB_CLEAR);
	check_px("rect-outside-above",RA_X + RA_W / 2,   RA_Y - 2,        FB_CLEAR);

	/* Texture sampling: the four quadrant centres of the stretched quad. */
	check_px("tex-TL", 260 + 40,  200 + 40,  TQ_TL);
	check_px("tex-TR", 260 + 120, 200 + 40,  TQ_TR);
	check_px("tex-BL", 260 + 40,  200 + 120, TQ_BL);
	check_px("tex-BR", 260 + 120, 200 + 120, TQ_BR);

	/* Presentation actually happened. */
	check_int("presents-after-2d-frame", gt_present_count(), 1);
}

/* ---------------------------------------------------------------------- */
/* Frame 2: 3D transform + depth buffer                                    */
/* ---------------------------------------------------------------------- */
static GfxResourceID vb_quads;

/* One axis-aligned quad at a fixed z, filling most of the view. */
static void fill_quad(struct VertexColoured* v, float half, float z, PackedCol c) {
	v[0].x = -half; v[0].y =  half; v[0].z = z; v[0].Col = c;
	v[1].x =  half; v[1].y =  half; v[1].z = z; v[1].Col = c;
	v[2].x =  half; v[2].y = -half; v[2].z = z; v[2].Col = c;
	v[3].x = -half; v[3].y = -half; v[3].z = z; v[3].Col = c;
}

static void frame_depth(void) {
	struct Matrix proj, view;
	struct VertexColoured* v;

	Gfx_BeginFrame();
	Gfx_ClearColor(V_CLEAR);
	Gfx_ClearBuffers(GFX_BUFFER_COLOR | GFX_BUFFER_DEPTH);

	Gfx_CalcPerspectiveMatrix(&proj, 70.0f * MATH_DEG2RAD, (float)GT_WIDTH / GT_HEIGHT, 512.0f);
	Matrix_Translate(&view, 0.0f, 0.0f, -6.0f);
	Gfx_LoadMatrix(MATRIX_PROJ, &proj);
	Gfx_LoadMatrix(MATRIX_VIEW, &view);

	Gfx_SetDepthTest(true);
	Gfx_SetDepthWrite(true);
	Gfx_SetFaceCulling(false);
	Gfx_BindTexture(NULL);
	Gfx_SetVertexFormat(VERTEX_FORMAT_COLOURED);

	/* Draw the FAR quad SECOND. If the depth test is broken (or absent) the
	 * far green quad paints over the near red one and the centre reads green.
	 * Painter's order alone would give the wrong answer, so this check can
	 * only pass with a working depth buffer. */
	v = (struct VertexColoured*)Gfx_LockDynamicVb(vb_quads, VERTEX_FORMAT_COLOURED, 8);
	fill_quad(v,     1.5f,  0.0f, V_RED);    /* near */
	fill_quad(v + 4, 2.5f, -3.0f, V_GREEN);  /* far  */
	Gfx_UnlockDynamicVb(vb_quads);

	Gfx_BindDynamicVb(vb_quads);
	Gfx_DrawVb_IndexedTris(8);

	Gfx_EndFrame();
	R->frames_3d++;
}

static void verify_depth(void) {
	/* Centre: near red quad wins over the later-drawn far green quad. */
	check_px("depth-near-wins", GT_WIDTH / 2, GT_HEIGHT / 2, FB_RED);

	/* The far quad is larger in world units but further away; find a point
	 * that only it covers. Scan outwards from centre along the horizontal
	 * midline and require the sequence red -> green -> clear. */
	{
		int x, seen_red = 0, seen_green = 0, seen_clear = 0;
		int order_ok = 1, stage = 0;
		for (x = GT_WIDTH / 2; x < GT_WIDTH; x++) {
			cc_uint32 c = px_at(x, GT_HEIGHT / 2) & 0x00FFFFFFu;
			int s;
			if      (c == FB_RED)   { s = 0; seen_red = 1; }
			else if (c == FB_GREEN) { s = 1; seen_green = 1; }
			else if (c == FB_CLEAR) { s = 2; seen_clear = 1; }
			else                    { s = -1; }
			if (s < 0) { order_ok = 0; break; }
			if (s < stage) { order_ok = 0; break; }  /* never goes backwards */
			stage = s;
		}
		check_true("depth-band-red-present",   seen_red);
		check_true("depth-band-green-present", seen_green);
		check_true("depth-band-clear-present", seen_clear);
		check_true("depth-band-order-red-green-clear", order_ok);
	}

	/* Perspective sanity: the near quad's half-width in pixels must match the
	 * projection maths, not merely "be non-zero". half=1.5 at distance 6 with
	 * a 70 degree vertical fov:
	 *     c   = cot(35 deg)                 (Gfx_CalcPerspectiveMatrix)
	 *     ndc = 1.5 * (c/aspect) / 6
	 *     px  = (GT_WIDTH/2) * ndc
	 * Computed here from the same formula the backend uses, then compared to
	 * the measured edge with a 2px tolerance for the fill rule. */
	{
		float c      = Math_CosF(0.5f * 70.0f * MATH_DEG2RAD) / Math_SinF(0.5f * 70.0f * MATH_DEG2RAD);
		float aspect = (float)GT_WIDTH / GT_HEIGHT;
		float ndc    = 1.5f * (c / aspect) / 6.0f;
		int   expect = (int)((GT_WIDTH / 2) * ndc);
		int   x, measured = 0;
		for (x = GT_WIDTH / 2; x < GT_WIDTH; x++) {
			if ((px_at(x, GT_HEIGHT / 2) & 0x00FFFFFFu) != FB_RED) break;
			measured++;
		}
		{
			int diff = measured - expect;
			if (diff < 0) diff = -diff;
			check_true("perspective-halfwidth-matches-projection", diff <= 2);
			if (diff > 2) {
				char* p = msgbuf;
				p = put_str(p, "  perspective: expected halfwidth ");
				p = put_int(p, expect);
				p = put_str(p, " px, measured ");
				p = put_int(p, measured);
				*p = '\0';
				gt_log(msgbuf);
			}
		}
	}
}

/* ---------------------------------------------------------------------- */
/* Timing scene: textured rotating cube, the representative workload       */
/* ---------------------------------------------------------------------- */
static void cube_face(struct VertexTextured* v, Vec3 a, Vec3 b, Vec3 c, Vec3 d, PackedCol col) {
	v[0].x = a.x; v[0].y = a.y; v[0].z = a.z; v[0].Col = col; v[0].U = 0.0f; v[0].V = 0.0f;
	v[1].x = b.x; v[1].y = b.y; v[1].z = b.z; v[1].Col = col; v[1].U = 1.0f; v[1].V = 0.0f;
	v[2].x = c.x; v[2].y = c.y; v[2].z = c.z; v[2].Col = col; v[2].U = 1.0f; v[2].V = 1.0f;
	v[3].x = d.x; v[3].y = d.y; v[3].z = d.z; v[3].Col = col; v[3].U = 0.0f; v[3].V = 1.0f;
}

static GfxResourceID vb_cube;

static void build_cube(float t) {
	struct VertexTextured* v;
	float cs = Math_CosF(t), sn = Math_SinF(t);
	Vec3 p[8];
	int i;
	static const float base[8][3] = {
		{-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
		{-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}
	};

	for (i = 0; i < 8; i++) {
		float x = base[i][0], y = base[i][1], z = base[i][2];
		float x2 = x * cs - z * sn;
		float z2 = x * sn + z * cs;
		float y2 = y * cs - z2 * sn * 0.5f;
		p[i].x = x2; p[i].y = y2; p[i].z = z2;
	}

	v = (struct VertexTextured*)Gfx_LockDynamicVb(vb_cube, VERTEX_FORMAT_TEXTURED, 24);
	cube_face(v +  0, p[0], p[1], p[2], p[3], PACKEDCOL_WHITE);
	cube_face(v +  4, p[5], p[4], p[7], p[6], PACKEDCOL_WHITE);
	cube_face(v +  8, p[4], p[0], p[3], p[7], PACKEDCOL_WHITE);
	cube_face(v + 12, p[1], p[5], p[6], p[2], PACKEDCOL_WHITE);
	cube_face(v + 16, p[3], p[2], p[6], p[7], PACKEDCOL_WHITE);
	cube_face(v + 20, p[4], p[5], p[1], p[0], PACKEDCOL_WHITE);
	Gfx_UnlockDynamicVb(vb_cube);
}

int gt_render_3d_frames(int frames) {
	struct Matrix proj, view;
	int i;

	Gfx_CalcPerspectiveMatrix(&proj, 70.0f * MATH_DEG2RAD, (float)GT_WIDTH / GT_HEIGHT, 512.0f);
	Matrix_Translate(&view, 0.0f, 0.0f, -4.0f);
	Gfx_LoadMatrix(MATRIX_PROJ, &proj);
	Gfx_LoadMatrix(MATRIX_VIEW, &view);

	Gfx_SetDepthTest(true);
	Gfx_SetDepthWrite(true);
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_BindTexture(tex_checker);

	for (i = 0; i < frames; i++) {
		Gfx_BeginFrame();
		Gfx_ClearColor(V_CLEAR);
		Gfx_ClearBuffers(GFX_BUFFER_COLOR | GFX_BUFFER_DEPTH);

		build_cube(0.02f * i);
		Gfx_BindDynamicVb(vb_cube);
		Gfx_DrawVb_IndexedTris(24);

		Gfx_EndFrame();
	}
	return frames;
}

/* ---------------------------------------------------------------------- */
/* Fill-rate benchmark: the HONEST number                                  */
/*                                                                          */
/* The rotating cube covers ~17% of the surface, which flatters the frame    */
/* time. A real ClassiCube world frame covers the whole screen and overdraws */
/* it several times. This draws `overdraw` screen-filling TEXTURED,          */
/* DEPTH-TESTED quads per frame through the same 3D path the world uses, so  */
/* the resulting number is a defensible fill-rate figure rather than a       */
/* best case.                                                                */
/* ---------------------------------------------------------------------- */
static GfxResourceID vb_fill;

int gt_render_fill_frames(int frames, int overdraw) {
	struct Matrix proj, view;
	struct VertexTextured* v;
	int i, k;
	/* At distance 6 with the projection below, this half-extent more than
	 * covers the viewport, so every quad touches every pixel. */
	const float H = 7.0f;

	if (overdraw > 8) overdraw = 8;
	if (!vb_fill) vb_fill = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, 8 * 4);

	Gfx_CalcPerspectiveMatrix(&proj, 70.0f * MATH_DEG2RAD, (float)GT_WIDTH / GT_HEIGHT, 512.0f);
	Matrix_Translate(&view, 0.0f, 0.0f, -6.0f);
	Gfx_LoadMatrix(MATRIX_PROJ, &proj);
	Gfx_LoadMatrix(MATRIX_VIEW, &view);

	Gfx_SetDepthTest(true);
	Gfx_SetDepthWrite(true);
	Gfx_SetFaceCulling(false);
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_BindTexture(tex_checker);

	for (i = 0; i < frames; i++) {
		Gfx_BeginFrame();
		Gfx_ClearColor(V_CLEAR);
		Gfx_ClearBuffers(GFX_BUFFER_COLOR | GFX_BUFFER_DEPTH);

		v = (struct VertexTextured*)Gfx_LockDynamicVb(vb_fill, VERTEX_FORMAT_TEXTURED, overdraw * 4);
		for (k = 0; k < overdraw; k++) {
			/* Back to front, so the depth test never rejects: worst case. */
			float z = -3.0f + 0.5f * k;
			struct VertexTextured* q = v + k * 4;
			q[0].x = -H; q[0].y =  H; q[0].z = z; q[0].Col = PACKEDCOL_WHITE; q[0].U = 0.0f; q[0].V = 0.0f;
			q[1].x =  H; q[1].y =  H; q[1].z = z; q[1].Col = PACKEDCOL_WHITE; q[1].U = 4.0f; q[1].V = 0.0f;
			q[2].x =  H; q[2].y = -H; q[2].z = z; q[2].Col = PACKEDCOL_WHITE; q[2].U = 4.0f; q[2].V = 4.0f;
			q[3].x = -H; q[3].y = -H; q[3].z = z; q[3].Col = PACKEDCOL_WHITE; q[3].U = 0.0f; q[3].V = 4.0f;
		}
		Gfx_UnlockDynamicVb(vb_fill);

		Gfx_BindDynamicVb(vb_fill);
		Gfx_DrawVb_IndexedTris(overdraw * 4);
		Gfx_EndFrame();
	}
	return frames;
}

/* Counts pixels that are not the clear colour. Used to prove the timing scene
 * actually drew something rather than measuring the cost of an empty loop. */
int gt_count_drawn_pixels(void) {
	BitmapCol* fb = gt_framebuffer();
	int i, n = 0, total = GT_WIDTH * GT_HEIGHT;
	for (i = 0; i < total; i++) {
		if ((fb[i] & 0x00FFFFFFu) != FB_CLEAR) n++;
	}
	return n;
}

/* ---------------------------------------------------------------------- */
/* Re-renders just the 2D conformance frame, so the driver can dump it as a
 * separate BMP without re-running the whole suite. */
void gt_render_2d_frame(void) { frame_2d(); }

int gt_run(struct gt_results* res) {
	struct Bitmap bmp;

	R = res;
	res->checks_run = 0; res->checks_failed = 0;
	res->frames_2d  = 0; res->frames_3d     = 0;

	build_checker();

	/* Init order documented in gfx/mos_gfx.h section 3. */
	Gfx_Create();
	Gfx_OnWindowResize(GT_WIDTH, GT_HEIGHT);
	Gfx_Component.Init();

	check_true("gfx-created", Gfx.Created);
	check_true("framebuffer-allocated", gt_framebuffer() != NULL);

	Bitmap_Init(bmp, 4, 4, checker_px);
	tex_checker = Gfx_CreateTexture(&bmp, 0, false);
	check_true("texture-created", tex_checker != NULL);

	vb_quads = Gfx_CreateDynamicVb(VERTEX_FORMAT_COLOURED, 8);
	vb_cube  = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, 24);
	check_true("vb-created", vb_quads != NULL && vb_cube != NULL);

	gt_offscreen_reset();
	frame_2d();
	verify_2d();

	gt_offscreen_reset();
	frame_depth();
	verify_depth();

	return res->checks_failed == 0 ? 0 : 1;
}
