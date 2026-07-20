// dr_flac.c - MayteraOS vendored dr_flac (FLAC decoder) implementation unit.
//
// dr_flac is a single-file public-domain / MIT-0 FLAC decoder by David Reid
// (mackron). FLAC is lossless and therefore integer; this build uses ONLY the
// integer output APIs (drflac_read_pcm_frames_s16). The float (f32) output path
// is excluded and SSE/NEON disabled (see drflac_config.h) so the resulting
// object contains ZERO floating-point / SIMD instructions under the kernel's
// -mno-sse / -mno-sse2 (no-FPU) build, exactly as for libmad and Tremor.
//
// License: public domain (Unlicense) or MIT-0, at your option. See
// media/dr_flac/COPYING and the license block at the end of dr_flac.h.
// Source: dr_flac v0.13.4 from github.com/mackron/dr_libs (dr_flac.h).
#define DR_FLAC_IMPLEMENTATION
#include "drflac_config.h"
#include "dr_flac.h"
