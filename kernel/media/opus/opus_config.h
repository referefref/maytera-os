/* opus_config.h - shared fixed-point, no-FPU, decode-oriented config for the
 * vendored libopus, used by BOTH the host cross-check harness and the kernel
 * build (force-included via -include / -DHAVE_CONFIG_H is NOT used). */
#ifndef MAYTERA_OPUS_CONFIG_H
#define MAYTERA_OPUS_CONFIG_H

#define OPUS_BUILD          1
#define FIXED_POINT         1   /* integer-only DSP path (no FPU) */
#define DISABLE_FLOAT_API   1   /* drop opus_decode_float / opus_encode_float */
#define VAR_ARRAYS          1   /* temp allocation via C99 VLAs (no alloca/malloc) */
#define OPUS_VERSION        "1.3.1-maytera-fixed"
#define PACKAGE_VERSION     OPUS_VERSION

/* No runtime CPU detection / SIMD: pure portable C only. */
#undef OPUS_HAVE_RTCD
#undef OPUS_X86_MAY_HAVE_SSE
#undef OPUS_X86_MAY_HAVE_SSE2
#undef OPUS_X86_MAY_HAVE_SSE4_1
#undef OPUS_X86_MAY_HAVE_AVX
#undef OPUS_X86_PRESUME_SSE
#undef OPUS_X86_PRESUME_SSE2
#undef OPUS_X86_PRESUME_SSE4_1
#undef OPUS_ARM_ASM
#undef OPUS_ARM_MAY_HAVE_NEON
#undef OPUS_ARM_MAY_HAVE_NEON_INTR

#endif
