// drflac_config.h - MayteraOS build configuration for the vendored dr_flac.
//
// Included BEFORE dr_flac.h by both the implementation TU (dr_flac.c) and the
// wrapper (media/flac.c) so they agree on every ABI- and codepath-affecting
// macro. The choices below guarantee the dr_flac objects contain ZERO floating
// point / SIMD instructions under the kernel's -mno-sse / -mno-sse2 build (the
// same bar met by libmad and Tremor):
//
//   DR_FLAC_NO_STDIO  - no FILE I/O (kernel has none; we decode from memory).
//   DR_FLAC_NO_OGG    - native .flac only (no FLAC-in-Ogg).
//   DR_FLAC_NO_CRC    - drops the float-using binary-search seek. CRC only
//                       *verifies* the bitstream; it never affects decoded PCM.
//                       Seeking falls back to the seek table + integer brute
//                       force, both float-free.
//   DRFLAC_NO_SSE2/41/NEON - no SIMD intrinsics.
//   DRFLAC_NO_F32     - local patch (see dr_flac.h guards): excludes the float
//                       output API. We use ONLY the integer s16 path.
#ifndef MAYTERA_DRFLAC_CONFIG_H
#define MAYTERA_DRFLAC_CONFIG_H
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_CRC
#define DRFLAC_NO_SSE2
#define DRFLAC_NO_SSE41
#define DRFLAC_NO_NEON
#define DRFLAC_NO_F32
#ifndef DRFLAC_ASSERT
#define DRFLAC_ASSERT(expr)  ((void)0)
#endif
#endif
