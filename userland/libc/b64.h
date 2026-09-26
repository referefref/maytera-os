// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// b64.h - shared standard base64 encoder (RFC 4648 alphabet, '=' padded).
//
// WHY IT IS HERE AND NOT IN ONE APP (#469 AI-VISION). Nothing in the tree had a
// base64 encoder: the only hits for the word were comments. The AI-vision path
// needs one to put a JPEG into a `data:image/jpeg;base64,...` URL, and the next
// consumer (an HTTP Basic header, a MIME attachment, an OGG
// METADATA_BLOCK_PICTURE) would otherwise write a second one. The project rule
// is one shared primitive, so it starts shared.
//
// NO DECODER YET, deliberately: nothing needs one, and an untested decoder
// sitting in libc is a liability, not a feature. Add it here, with its own test,
// when there is a caller.
#ifndef MAYTERA_B64_H
#define MAYTERA_B64_H

// Bytes of base64 output (EXCLUDING the NUL) produced for `inlen` input bytes.
// Use it to size a buffer: the caller needs b64_encoded_len(n) + 1.
static inline long b64_encoded_len(long inlen) {
    if (inlen <= 0) return 0;
    return ((inlen + 2) / 3) * 4;
}

// Encode `inlen` bytes of `in` into `out` as NUL-terminated base64.
// Returns the number of characters written (excluding the NUL), or -1 if the
// arguments are bad or `outcap` cannot hold the result AND its NUL. It NEVER
// writes a truncated encoding: a short buffer is an error, not a silent
// half-image (a truncated base64 payload would be a corrupt request body that
// the server rejects with an opaque error a long way from the cause).
long b64_encode(const unsigned char *in, long inlen, char *out, long outcap);

#endif // MAYTERA_B64_H
