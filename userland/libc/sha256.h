// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// sha256.h - SHA-256 for MayteraOS userland (#559).
//
// This is a direct port of the kernel's audited crypto/sha256.c (FIPS 180-4),
// NOT a new implementation. The kernel copy carries a Rust strangler dispatcher
// (-DRUST_SHA256) and a boot-time NIST known-answer self-test; userland has no
// Rust toolchain wired in, so only the C compression leaf is ported here. The
// two must stay byte-identical in output: libc_sha256_selftest() runs the same
// NIST vectors the kernel uses so a divergence is caught in userland too.
//
// Used by the App Store / updater package-signature chain: the manifest digest
// fed to SYS_OTA_VERIFY_SIG and the per-package integrity check are both
// computed with this.

#ifndef _LIBC_SHA256_H
#define _LIBC_SHA256_H

#include "types.h"

#define SHA256_BLOCK_SIZE   64
#define SHA256_DIGEST_SIZE  32

typedef struct {
    uint32_t state[8];
    uint64_t count;             // bits processed
    uint8_t  buffer[64];
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t length);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]);
void sha256(const void *data, size_t length, uint8_t digest[32]);

// NIST known-answer self-test. Returns 0 if SHA-256 is correct on this build,
// -1 otherwise. An independent oracle: it does NOT compare against another copy
// of our own code (a differential cannot catch a bug both arms share), it
// compares against the published FIPS 180-4 digests.
int libc_sha256_selftest(void);

#endif // _LIBC_SHA256_H
