// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// jpegenc.h - shared baseline JPEG encoder (ITU T.81, 8-bit, JFIF 1.1 APP0).
//
// WHERE THIS CAME FROM AND WHY IT MOVED (#469 AI-VISION). The encoder was
// written for Maytera Studio and lived at userland/apps/paint/jpegenc.c. It was
// already a PURE FUNCTION over an ARGB buffer (no document, no UI, no file I/O),
// and imgconv was already compiling that file BY PATH out of another app's
// directory to get at it. The AI-vision capture path needs it too, which makes
// three consumers, so it moved here rather than being copied a third time: the
// project rule is to share the primitive, never to fork it.
//
// The implementation is unchanged by the move. It writes baseline sequential
// DCT, YCbCr, Annex K.1 quantisation tables scaled by the IJG quality formula,
// the Annex K.3 Huffman tables written INTO the file (so any decoder derives
// exactly the codes used), one interleaved scan, no restart intervals. Chroma is
// 4:2:0 (2x2 box-averaged) by default or 4:4:4. The kernel decoder
// (kernel/gui/jpeg.c) reads both layouts.
#ifndef MAYTERA_JPEGENC_H
#define MAYTERA_JPEGENC_H

#include "types.h"

// Encode `src` (w*h ARGB8888, alpha ignored: the caller flattens) to a malloc'd
// JPEG in *out / *outlen. quality is 1..100; chroma444 != 0 selects 4:4:4
// sampling (0 = 4:2:0). Returns 0 on success, -1 on bad args or out of memory.
// THE CALLER FREES *out.
int jpeg_encode_argb(const uint32_t *src, int w, int h, int quality, int chroma444,
                     unsigned char **out, long *outlen);

#endif // MAYTERA_JPEGENC_H
