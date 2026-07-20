/* faad_config.h - MayteraOS #331 fixed-point, no-FPU, AAC-LC build config for the
 * vendored faad2 2.7 (libfaad). Force-included via -include for both the kernel
 * build and the host cross-check harness so they are byte-identical.
 *
 * faad2 is GPLv2 (Copyright (c) Nero AG, www.nero.com). See media/faad2/COPYING.
 * Code from FAAD2 is copyright (c) Nero AG, www.nero.com.
 *
 * FIXED_POINT = integer-only DSP (real_t = int32_t); the kernel is -mno-sse /
 * -mno-sse2 (no runtime FPU). LC_ONLY_DECODER + our common.h guard drop the
 * float-heavy SBR / PS / SSR / MAIN / LTP paths, leaving a pure AAC-LC decoder
 * whose objects are objdump-verified to contain ZERO x87/SSE floating point. */
#ifndef MAYTERA_FAAD_CONFIG_H
#define MAYTERA_FAAD_CONFIG_H

#define FIXED_POINT      1   /* integer DSP, real_t = int32_t */
#define LC_ONLY_DECODER  1   /* pure AAC-LC: no SBR/PS/SSR/MAIN/LTP/LD/DRM */

/* Tell faad2's common.h which system headers exist so it pulls our compat
 * shims (-Imedia/faad2/compat) instead of self-typedef'ing uint32_t as
 * "unsigned long" (which is 64-bit on x86-64 and would corrupt everything). */
#define STDC_HEADERS   1
#define HAVE_STDINT_H  1
#define HAVE_STRING_H  1
#define HAVE_STDLIB_H  1

#endif
