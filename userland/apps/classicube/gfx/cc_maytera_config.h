/* cc_maytera_config.h - MayteraOS build configuration for ClassiCube (#28).
 *
 * OWNED BY: the graphics lane (agent/cc-gfx).
 *
 * ===========================================================================
 * LANE BOUNDARY: THIS FILE DOES NOT SELECT THE PLATFORM. READ THIS FIRST.
 * ===========================================================================
 * Upstream Core.h offers two mutually exclusive ways to describe a platform,
 * and picking both silently loses one of them:
 *
 *   (a) add a branch INSIDE Core.h's auto-detect block, or
 *   (b) define CC_BUILD_MANUAL, which makes Core.h:189 skip that whole block.
 *
 * The APP BUILD owns (a), and applies it to a STAGED COPY, never to vendor/:
 * the Makefile copies vendor/ClassiCube/src into build/src and runs
 * engine-patches/coreh-maytera.py on build/src/Core.h. That branch sets
 * CC_BUILD_MAYTERA, the CC_BUILD_FREETYPE / CC_BUILD_PLUGINS undefs,
 * DEFAULT_NET_BACKEND=BUILTIN, DEFAULT_SSL_BACKEND=NONE,
 * DEFAULT_AUD_BACKEND=NULL, DEFAULT_GFX_BACKEND=CC_GFX_BACKEND_SOFTGPU and
 * DEFAULT_WIN_BACKEND=CC_WIN_BACKEND_MAYTERA. Staging is what keeps vendor/
 * byte-identical to the pin, which is what keeps build/repo-guard.sh green.
 *
 * So in the REAL APP BUILD this header must NOT define CC_BUILD_MANUAL: doing
 * so would skip the platform lane's branch entirely and quietly revert every
 * one of those settings to upstream defaults. That failure has no symptom at
 * build time. It is only defined for the self-contained offscreen graphics
 * harness, which must build with NO patches applied to vendor/ at all, and
 * then only because the harness deliberately owns its whole environment.
 *
 * THE GRAPHICS LANE HAS NEVER EDITED A VENDORED BYTE. Verified, not asserted:
 * all 540 files under vendor/ sha256-match `git archive` of the pinned
 * upstream commit 4016a0918ba5c127d5203a4940e76b79b229d51f, Core.h included.
 * If this lane ever needs a Core.h change, it goes in an additional numbered
 * engine-patches/ script that asserts its own anchors, never a hand edit.
 */
#ifndef CC_MAYTERA_CONFIG_H
#define CC_MAYTERA_CONFIG_H

/* ---------------------------------------------------------------------------
 * STANDALONE HARNESS MODE ONLY.
 *
 * Defined solely by userland/apps/classicube/tests/gfxtest/Makefile, so the
 * offscreen graphics test can build straight from a pristine vendor tree with
 * no engine patches applied. The app build must NEVER define this.
 * -------------------------------------------------------------------------*/
#ifdef MOS_CC_STANDALONE_GFXTEST
	/* Take over platform detection for the harness only. */
	#define CC_BUILD_MANUAL
	#define CC_BUILD_MAYTERA

	/* SOFTGPU is upstream's own bring-up backend: DEFAULT_GFX_BACKEND on seven
	 * upstream platforms (Core.h:219,268,274,293,356,461,626). Its only
	 * outside-world dependencies are Mem_*, Math_* and the three
	 * Window_*Framebuffer entry points. No GL, no driver, no context.
	 * The platform lane independently selected the same backend. */
	#define DEFAULT_GFX_BACKEND CC_GFX_BACKEND_SOFTGPU

	/* Freestanding libc: no dlopen, so no runtime plugin loading. */
	#undef CC_BUILD_PLUGINS
#endif

/* ---------------------------------------------------------------------------
 * PIXEL FORMAT - VERIFIED, NOT ASSUMED. Evidence and macros: gfx/mos_gfx.h.
 *
 * Applies in BOTH modes, and deliberately contains no platform selection.
 *
 * MayteraOS framebuffer and compositor window buffers are 32bpp words of the
 * form 0xAARRGGBB (little-endian bytes B,G,R,A). Upstream Bitmap.h's DEFAULT
 * branch (vendor/ClassiCube/src/Bitmap.h:32-36) is B=0, G=8, R=16, A=24, i.e. bit-for-bit
 * identical, so the rasteriser's output blits with no conversion. This holds
 * only while the build avoids every platform macro Bitmap.h special-cases
 * (CC_BUILD_WEB / ANDROID / PSP / PSVITA / PS2 / PS4 / 3DS / N64 / WIIU / PS1 /
 * SATURN / NDS / 32X / GBA / ATARIOS).
 *
 * NOTE that PackedCol, the VERTEX colour type, is a DIFFERENT layout in this
 * configuration (0xAABBGGRR). That is correct upstream behaviour and is the
 * single easiest way to render a red world blue. See gfx/mos_gfx.h section 1b.
 *
 * MOS_GFX_ASSERT_PIXEL_FORMAT() in gfx/mos_gfx.h turns a future violation into
 * a BUILD ERROR rather than a shipped channel swap. It has already fired once
 * in anger.
 * -------------------------------------------------------------------------*/

/* MayteraOS is a full-fat desktop target: 32-bit colour, and hardware FPU in
 * Ring 3. Measured, not assumed: the soft-float / -mno-sse rule is a KERNEL
 * constraint (kernel/Makefile:142-143); compiling a float expression with the
 * plain userland flags and NO -msse still emits xmm, because SSE2 is the
 * x86-64 ABI baseline. So CC_BUILD_FPU_MODE's default of CC_FPU_MODE_NORMAL
 * is correct for us and is deliberately not overridden here. */

#endif /* CC_MAYTERA_CONFIG_H */
