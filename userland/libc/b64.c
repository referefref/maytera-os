// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// b64.c - shared standard base64 encoder. See b64.h for why it lives here.
#include "b64.h"

static const char B64A[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

long b64_encode(const unsigned char *in, long inlen, char *out, long outcap) {
    if (!out || outcap <= 0) return -1;
    if (inlen < 0 || (!in && inlen > 0)) return -1;
    long need = b64_encoded_len(inlen);
    // Fail closed on a short buffer. A truncated base64 body is a corrupt
    // request the far end rejects with an error that points nowhere near here.
    if (need + 1 > outcap) return -1;

    long o = 0, i = 0;
    while (i + 3 <= inlen) {
        unsigned int v = ((unsigned int)in[i] << 16) |
                         ((unsigned int)in[i + 1] << 8) |
                          (unsigned int)in[i + 2];
        out[o++] = B64A[(v >> 18) & 63];
        out[o++] = B64A[(v >> 12) & 63];
        out[o++] = B64A[(v >>  6) & 63];
        out[o++] = B64A[ v        & 63];
        i += 3;
    }
    long rem = inlen - i;
    if (rem == 1) {
        unsigned int v = (unsigned int)in[i] << 16;
        out[o++] = B64A[(v >> 18) & 63];
        out[o++] = B64A[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        unsigned int v = ((unsigned int)in[i] << 16) | ((unsigned int)in[i + 1] << 8);
        out[o++] = B64A[(v >> 18) & 63];
        out[o++] = B64A[(v >> 12) & 63];
        out[o++] = B64A[(v >>  6) & 63];
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}
