/* pixelprobe.c - THE FORMAT TRAP, measured under the REAL app configuration.
 *
 * #800 ClassiCube port. Run by `make pixelformat` in the app directory.
 *
 * WHY THIS EXISTS AS A SEPARATE, RUNNABLE THING.
 * MayteraOS framebuffer words are 0xAARRGGBB. ClassiCube's VERTEX colour type
 * PackedCol is 0xAABBGGRR in this configuration. Red and blue are exchanged
 * between the two, and GREYS SURVIVE THE SWAP: a UI made of greys, whites and
 * blacks renders perfectly while the world renders with red and blue swapped.
 * So looking at a menu proves nothing, and the only honest check is a
 * deliberately NON-GREY, NON-SYMMETRIC colour whose word is read back.
 *
 * The graphics lane already proved the rendered path with 26 exact-pixel
 * checks in tests/gfxtest, including an asymmetric vertex colour. But that
 * harness compiles under MOS_CC_STANDALONE_GFXTEST, which sets CC_BUILD_MANUAL
 * and takes over platform detection. This probe closes the remaining gap: it
 * compiles against the STAGED Core.h the real app build uses, with the real
 * -DPLAT_MAYTERA, so what it measures is the configuration that ships.
 *
 * The matching COMPILE-TIME gate lives in gfx/mos_gfx.h and is invoked from
 * Window_Maytera.c, so a future platform macro that changes either layout
 * stops the app build. This probe is the runtime half: it prints the words so
 * a human can see the swap rather than take it on trust.
 */
#include <stdio.h>
#include "Core.h"
#include "Bitmap.h"
#include "PackedCol.h"

/* A deliberately asymmetric, non-grey colour: every channel different, and
 * R and B far apart so a swap cannot hide in rounding. */
#define PR 0x11
#define PG 0x77
#define PB 0xEE

/* A grey, present only to DEMONSTRATE that greys cannot detect the fault. */
#define GR 0x80
#define GG 0x80
#define GB 0x80

static int fails;

static void expect(const char* what, unsigned int got, unsigned int want)
{
	if (got == want) {
		printf("  OK   %-34s 0x%08X\n", what, got);
	} else {
		printf("  FAIL %-34s got 0x%08X want 0x%08X\n", what, got, want);
		fails++;
	}
}

int main(void)
{
	PackedCol v, vg;
	BitmapCol f, fg;

	printf("ClassiCube <-> MayteraOS pixel format probe (#800)\n");
	printf("  configuration: PLAT_MAYTERA, staged Core.h, the shipping app build\n");
	printf("  BITMAPCOLOR shifts  R=%d G=%d B=%d A=%d  (framebuffer / texture)\n",
		BITMAPCOLOR_R_SHIFT, BITMAPCOLOR_G_SHIFT,
		BITMAPCOLOR_B_SHIFT, BITMAPCOLOR_A_SHIFT);
	printf("  PACKEDCOL   shifts  R=%d G=%d B=%d A=%d  (vertex colour)\n",
		PACKEDCOL_R_SHIFT, PACKEDCOL_G_SHIFT,
		PACKEDCOL_B_SHIFT, PACKEDCOL_A_SHIFT);

	/* 1. The two layouts, spelled out as words. */
	v = PackedCol_Make(PR, PG, PB, 0xFF);
	f = BitmapCol_Make(PR, PG, PB, 0xFF);
	expect("vertex  PackedCol(11,77,EE)",  v, 0xFFEE7711u);
	expect("pixel   BitmapCol(11,77,EE)",  f, 0xFF1177EEu);

	/* 2. THE POINT: for a non-grey colour the two words DIFFER. If they ever
	 *    stop differing, either a platform macro re-gated a branch or someone
	 *    "helpfully" unified the types, and every rule about which macro to
	 *    use has silently become meaningless. */
	if (v == f) {
		printf("  FAIL vertex and pixel words are identical for a non-grey colour;\n"
		       "       the layouts have been unified and the rules are now noise\n");
		fails++;
	} else {
		printf("  OK   vertex != pixel word for a non-grey colour (R and B exchanged)\n");
	}

	/* 3. WHY A MENU PROVES NOTHING: the same two macros on a grey agree. */
	vg = PackedCol_Make(GR, GG, GB, 0xFF);
	fg = BitmapCol_Make(GR, GG, GB, 0xFF);
	if (vg == fg) {
		printf("  OK   grey 0x%02X is IDENTICAL in both layouts (0x%08X) - which is\n"
		       "       exactly why a grey UI cannot detect a channel swap\n", GR, vg);
	} else {
		printf("  FAIL grey differs between layouts; the shifts are not a pure R/B swap\n");
		fails++;
	}

	/* 4. The conversion the rasteriser actually performs (Graphics_SoftGPU.c
	 *    Gfx_ClearColor): pull channels out of a PackedCol with the named
	 *    accessors, rebuild a BitmapCol. A correct implementation returns the
	 *    framebuffer word; a raw cast returns the swapped one. */
	{
		BitmapCol conv = BitmapCol_Make(PackedCol_R(v), PackedCol_G(v),
		                                PackedCol_B(v), PackedCol_A(v));
		expect("SoftGPU vertex->pixel conversion", conv, 0xFF1177EEu);
		if ((BitmapCol)v == conv) {
			printf("  FAIL a raw cast would also have passed; this test proves nothing\n");
			fails++;
		}
	}

	printf("%s\n", fails ? "PIXEL FORMAT PROBE: FAIL" : "PIXEL FORMAT PROBE: PASS");
	return fails ? 1 : 0;
}
