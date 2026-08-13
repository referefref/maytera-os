/* mos_gfx.h - MayteraOS <-> ClassiCube graphics contract (#28, graphics lane).
 *
 * This header is DOCUMENTATION THAT COMPILES. It declares nothing new: the
 * ClassiCube software rasteriser (vendor/ClassiCube/src/Graphics_SoftGPU.c) already
 * defines the whole Gfx_* contract. What this file pins down is the boundary
 * between that rasteriser and MayteraOS, so the window lane and the graphics
 * lane cannot drift.
 *
 * ===========================================================================
 * 1. PIXEL FORMAT - MEASURED, NOT ASSUMED
 * ===========================================================================
 * MayteraOS 32bpp pixel word (uint32, little-endian machine):
 *
 *      bit  31..24  23..16  15..8   7..0
 *            A/X      R       G      B
 *
 *      in memory:  byte[0]=B  byte[1]=G  byte[2]=R  byte[3]=A/X
 *
 * i.e. word order 0xAARRGGBB, byte order B,G,R,A ("BGRA8888" bytes /
 * "XRGB8888" words). FOUR independent in-tree sources agree:
 *
 *   (a) kernel/video/framebuffer.c:281 fb_put_pixel() stores the uint32 RAW
 *       into the framebuffer, and every consumer in that file decodes it as
 *       R=(c>>16)&0xFF, G=(c>>8)&0xFF, B=c&0xFF
 *       (fb_blend_pixel at :540-545, fb_fill_rect_alpha at :583-585).
 *   (b) kernel/gui/image.c:117-120 - the BMP decoder reads the 24bpp source
 *       triple in its on-disk order b=src[0], g=src[1], r=src[2] and packs
 *       (r<<16)|(g<<8)|b. BMP's on-disk order is fixed by the file format, so
 *       this is an EXTERNAL cross-check, not a self-consistent convention.
 *   (c) userland/libc/gui.h:50-61 - COLOR_RED 0x00FF0000, COLOR_GREEN
 *       0x0000FF00, COLOR_BLUE 0x000000FF.
 *   (d) userland/apps/compositor/draw.c:279-285 - CHAN_R(c) ((c>>16)&0xFF),
 *       described as "packed ARGB"; icons.c:579 "ARGB (0xAARRGGBB) pixels".
 *
 * KNOWN INCONSISTENCY, do not be misled by it: kernel/video/framebuffer.h:8-9
 * declares FB_COLOR(r,g,b) as ((b<<16)|(g<<8)|r) under a comment saying
 * "32-bit BGRA". That macro is CHANNEL-SWAPPED relative to every consumer
 * above. It survives only because its ten call sites (all in
 * kernel/exec/win16api.c) are black/white/grey stock brushes, where r==g==b
 * makes the swap invisible. Do not use FB_COLOR as evidence of anything, and
 * do not use it in new code.
 *
 * WHY THIS MATTERS AND WHY IT COSTS US NOTHING: upstream ClassiCube's DEFAULT
 * BitmapCol layout (vendor/ClassiCube/src/Bitmap.h:32-36) is
 *      B_SHIFT 0, G_SHIFT 8, R_SHIFT 16, A_SHIFT 24
 * which is bit-for-bit MayteraOS's layout, so the rasteriser writes pixels our
 * compositor can consume with a straight memcpy. Zero conversion, zero
 * per-pixel swizzle. This holds ONLY while our build avoids every platform
 * macro Bitmap.h special-cases; see gfx/cc_maytera_config.h.
 * MOS_GFX_ASSERT_PIXEL_FORMAT below makes a future violation a BUILD ERROR.
 *
 * ===========================================================================
 * 1b. PackedCol IS NOT BitmapCol. THEY DIFFER IN THIS BUILD.
 * ===========================================================================
 * ClassiCube has TWO 32-bit colour types and they do NOT share a layout:
 *
 *   BitmapCol  - image and framebuffer pixels. Our config: 0xAARRGGBB.
 *   PackedCol  - VERTEX colours handed to the 3D pipeline. Our config:
 *                0xAABBGGRR, i.e. R in the LOW byte (PackedCol.h:22-25, the
 *                little-endian #else branch; the 0xAARRGGBB branch at :12-15
 *                is gated on D3D9/Xbox/Dreamcast/Xbox360 only).
 *
 * This is correct upstream behaviour, not a bug: Graphics_SoftGPU.c converts
 * between them through the named accessors (e.g. Gfx_ClearColor at :172-179
 * pulls R/G/B/A out with PackedCol_R/G/B/A and rebuilds with BitmapCol_Make).
 *
 * THE TRAP: MayteraOS-side code that reaches for a familiar 0x00RRGGBB literal
 * when filling a VERTEX colour gets red and blue silently exchanged. Greys and
 * whites survive it, so the UI looks fine and only the world looks wrong. This
 * was caught here by MOS_GFX_ASSERT_PIXEL_FORMAT firing at compile time on the
 * first build of the harness, before a single pixel was rendered.
 *
 * THE RULE:
 *   - a value destined for a framebuffer/texture pixel -> MOS_RGB()
 *   - a value destined for a vertex colour or Gfx_ClearColor -> MOS_PACKED_RGB()
 * Never assign one to the other, and never write a raw hex colour literal for
 * either. Both macros are below and both are built from upstream's own shifts,
 * so they stay correct if upstream ever re-gates the branches.
 *
 * ===========================================================================
 * 2. THE INTERFACE THE WINDOW LANE MUST PROVIDE
 * ===========================================================================
 * Graphics_SoftGPU.c reaches outside itself for exactly three window symbols.
 * They are declared by upstream in vendor/ClassiCube/src/Window.h:180-187; their exact
 * required BEHAVIOUR is:
 *
 *   void Window_AllocFramebuffer(struct Bitmap* bmp, int width, int height)
 *       Called from Gfx_OnWindowResize (Graphics_SoftGPU.c:1050), once at
 *       startup and again on every resize.
 *       MUST set bmp->width  = width
 *                bmp->height = height
 *                bmp->scan0  = a writable buffer of >= width*height uint32.
 *
 *       *** HARD REQUIREMENT: THE BUFFER MUST BE TIGHTLY PACKED. ***
 *       Graphics_SoftGPU.c:1052-1053 does
 *              colorBuffer = fb_bmp.scan0;
 *              cb_stride   = fb_bmp.width;
 *       so the row stride IS width, in pixels, with no padding. If the
 *       compositor window buffer has a stride different from its width, you
 *       MUST allocate a separate packed buffer here and stride-convert inside
 *       Window_DrawFramebuffer. Handing back a padded buffer produces a
 *       progressively skewed image, which is the classic "nearly right" bug.
 *
 *       Allocate via malloc (heap), NOT a static array: user.ld links one RWX
 *       PT_LOAD and a large .bss breaks the ELF loader. 1280*1024*4 = 5 MB.
 *       On allocation failure this must fail loudly, not return a NULL scan0
 *       that the rasteriser will happily write through.
 *
 *   void Window_DrawFramebuffer(Rect2D r, struct Bitmap* bmp)
 *       Called once per frame from Gfx_EndFrame (Graphics_SoftGPU.c:1035-1038)
 *       with r = {0, 0, fb_width, fb_height}. Copy bmp->scan0 to the
 *       compositor back buffer and present. Because the formats are identical
 *       (section 1), a row-wise memcpy is correct and is the fast path; use a
 *       single memcpy when the destination stride also equals width.
 *       r may be honoured or ignored; SoftGPU always passes the full window.
 *
 *   void Window_FreeFramebuffer(struct Bitmap* bmp)
 *       Called from DestroyBuffers (Graphics_SoftGPU.c:51). Free scan0 and
 *       set it to NULL. Must tolerate being called with scan0 already NULL.
 *
 * That is the ENTIRE surface. The graphics lane needs nothing else from the
 * window lane: no GL context, no swap-interval, no visual selection.
 *
 * ===========================================================================
 * 3. INITIALISATION ORDER (what the app lane must call)
 * ===========================================================================
 *      Gfx_Create();                  // sets caps, marks Gfx.Created
 *      Gfx_OnWindowResize(w, h);      // allocates colour + depth buffers
 *      Gfx_Component.Init();          // creates default IB/VBs + white 1x1
 *   per frame:
 *      Gfx_BeginFrame();
 *      Gfx_ClearColor(col); Gfx_ClearBuffers(GFX_BUFFER_COLOR|GFX_BUFFER_DEPTH);
 *      ... draw ...
 *      Gfx_EndFrame();                // -> Window_DrawFramebuffer
 *
 * Gfx_OnWindowResize MUST be called before Gfx_Component.Init(): the latter
 * creates the 1x1 white texture, and the former is what allocates the colour
 * buffer the rasteriser writes into.
 */
#ifndef MOS_GFX_H
#define MOS_GFX_H

#include "Core.h"
#include "Bitmap.h"
#include "PackedCol.h"

/* ---------------------------------------------------------------------------
 * BUILD-TIME PIXEL FORMAT GATE.
 *
 * A guessed constant does not fail loudly, so make this one fail loudly. If
 * anybody ever selects a ClassiCube platform whose BitmapCol/PackedCol channel
 * order differs from the MayteraOS framebuffer word, the build STOPS here
 * rather than shipping a render that looks nearly right.
 * -------------------------------------------------------------------------*/
#define MOS_PIXEL_B_SHIFT  0
#define MOS_PIXEL_G_SHIFT  8
#define MOS_PIXEL_R_SHIFT 16
#define MOS_PIXEL_A_SHIFT 24

#define MOS_GFX_ASSERT_PIXEL_FORMAT()                                          \
	_Static_assert(BITMAPCOLOR_SIZE == 4,                                      \
		"MayteraOS framebuffer is 32bpp; BitmapCol must be 4 bytes");          \
	_Static_assert(BITMAPCOLOR_B_SHIFT == MOS_PIXEL_B_SHIFT &&                 \
	               BITMAPCOLOR_G_SHIFT == MOS_PIXEL_G_SHIFT &&                 \
	               BITMAPCOLOR_R_SHIFT == MOS_PIXEL_R_SHIFT &&                 \
	               BITMAPCOLOR_A_SHIFT == MOS_PIXEL_A_SHIFT,                   \
		"BitmapCol channel order != MayteraOS 0xAARRGGBB - a platform macro "  \
		"changed Bitmap.h's layout. Fix cc_maytera_config.h, do NOT swizzle "  \
		"per pixel.");                                                         \
	_Static_assert(PACKEDCOL_A_SHIFT == 24,                                    \
		"PackedCol looks big-endian (CC_BIG_ENDIAN branch). MayteraOS is "     \
		"x86-64 little-endian; check cc_maytera_config.h.");                   \
	/* The A_SHIFT check above catches a big-endian branch but NOT the trap    \
	   this header exists for: the D3D9/Xbox/Dreamcast/Xbox360 branch of       \
	   PackedCol.h:11-15 puts R at 16 and B at 0, which would make PackedCol   \
	   identical to BitmapCol, MOS_PACKED_RGB identical to MOS_RGB, and every  \
	   rule below meaningless with no symptom. Pin all three. */               \
	_Static_assert(PACKEDCOL_R_SHIFT ==  0 &&                                  \
	               PACKEDCOL_G_SHIFT ==  8 &&                                  \
	               PACKEDCOL_B_SHIFT == 16,                                    \
		"PackedCol is no longer 0xAABBGGRR. Something re-gated PackedCol.h. "  \
		"Do NOT 'fix' this by swizzling: find which macro moved the branch.");  \
	_Static_assert(sizeof(PackedCol) == 4, "PackedCol must be 32-bit")

/* Build a MayteraOS FRAMEBUFFER / TEXTURE pixel word from 8-bit channels.
 * Deliberately spelled out rather than reusing kernel FB_COLOR, which is
 * channel-swapped (see the note above). */
#define MOS_RGB(r, g, b) (((cc_uint32)(r) << MOS_PIXEL_R_SHIFT) | \
                          ((cc_uint32)(g) << MOS_PIXEL_G_SHIFT) | \
                          ((cc_uint32)(b) << MOS_PIXEL_B_SHIFT))

/* Build a VERTEX colour / Gfx_ClearColor argument. Uses upstream's own shifts,
 * so it is correct whichever PackedCol branch is active. Opaque alpha. */
#define MOS_PACKED_RGB(r, g, b) PackedCol_Make((r), (g), (b), 255)

#endif /* MOS_GFX_H */
