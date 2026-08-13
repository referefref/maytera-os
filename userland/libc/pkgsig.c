// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// pkgsig.c - App Store / updater package trust chain (#559).
// See pkgsig.h for the threat model and the fail-closed contract.

#include "pkgsig.h"
#include "sha256.h"
#include "string.h"
#include "syscall.h"

// Verify a detached RSA-2048 PKCS#1 v1.5 SHA-256 signature over a 32-byte
// digest against the kernel's baked-in OTA public key. Returns 0 if valid.
// This is the #492 kernel primitive, reused rather than reimplemented: the
// private key, the key format, and the openssl-compatible signing convention
// are all already established for the kernel OTA path.
#ifndef SYS_OTA_VERIFY_SIG
#define SYS_OTA_VERIFY_SIG 314
#endif
#ifndef SYS_APP_VERIFY_SIG
#define SYS_APP_VERIFY_SIG 334
#endif

// #563 key split: authenticate an APP manifest with the DEDICATED app key via
// SYS_APP_VERIFY_SIG. The app-verify path in the kernel trusts the app key (and,
// during migration, the legacy OTA key), and CANNOT be used to authorize a
// kernel image. We fall back to SYS_OTA_VERIFY_SIG for two transition cases:
//   - an OLD kernel that predates SYS_APP_VERIFY_SIG (unknown syscall), and
//   - a manifest still signed only with the legacy OTA key.
// Both fallbacks disappear at the final cutover (app-key-only manifests + a
// kernel built without APP_VERIFY_ACCEPT_LEGACY_OTA_KEY). Verified == 0.
static inline int app_verify_sig(const uint8_t *digest,
                                 const uint8_t *sig, unsigned sig_len) {
    if ((int)syscall3(SYS_APP_VERIFY_SIG, (long)digest, (long)sig, (long)sig_len) == 0)
        return 0;
    return (int)syscall3(SYS_OTA_VERIFY_SIG, (long)digest, (long)sig, (long)sig_len);
}

int pkgsig_verify_manifest(const void *manifest, size_t manifest_len,
                           const uint8_t *sig, size_t sig_len) {
    if (!manifest || manifest_len == 0 || !sig) return PKGSIG_ERR_ARG;

    // Length is the entire "parse" of the signature container. A detached raw
    // signature has no structure to get wrong, which is the point: there is no
    // packet parser here for a hostile server to attack.
    if (sig_len != PKGSIG_SIG_LEN) return PKGSIG_ERR_SIGLEN;

    uint8_t digest[32];
    sha256(manifest, manifest_len, digest);

    if (app_verify_sig(digest, sig, (unsigned)PKGSIG_SIG_LEN) != 0)
        return PKGSIG_ERR_BADSIG;

    return PKGSIG_OK;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// #570: shared core. Compare a computed 32-byte digest against the manifest's
// hex sha256. pkgsig_verify_package() (whole-buffer) and
// pkgsig_verify_package_digest() (streamed) both funnel through here so the hex
// parse and the constant-time compare have exactly ONE implementation.
int pkgsig_verify_package_digest(const uint8_t *digest, const char *want_hex) {
    if (!digest) return PKGSIG_ERR_ARG;

    // A package entry with no hash is NOT installable. An absent hash must be
    // a refusal, never a skip: "no hash means nothing to check" is exactly how
    // an attacker would ask to be trusted.
    if (!want_hex || strlen(want_hex) != 64) return PKGSIG_ERR_NOHASH;

    uint8_t want[32];
    for (int i = 0; i < 32; i++) {
        int hi = hexval(want_hex[i * 2]);
        int lo = hexval(want_hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return PKGSIG_ERR_BADHEX;
        want[i] = (uint8_t)((hi << 4) | lo);
    }

    // Fixed-time compare. The values here are public, so this is hygiene rather
    // than a required property, but it costs nothing and stops the habit of
    // early-exit compares leaking into places where it does matter.
    volatile uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(digest[i] ^ want[i]);
    if (diff != 0) return PKGSIG_ERR_HASH_MISMATCH;

    return PKGSIG_OK;
}

int pkgsig_verify_package(const void *data, size_t len, const char *want_hex) {
    if (!data || len == 0) return PKGSIG_ERR_ARG;

    uint8_t got[32];
    sha256(data, len, got);
    return pkgsig_verify_package_digest(got, want_hex);
}

const char *pkgsig_strerror(int rc) {
    switch (rc) {
        case PKGSIG_OK:               return "verified";
        case PKGSIG_ERR_ARG:          return "bad arguments";
        case PKGSIG_ERR_SIGLEN:       return "signature wrong size";
        case PKGSIG_ERR_BADSIG:       return "SIGNATURE INVALID";
        case PKGSIG_ERR_NOHASH:       return "no sha256 in manifest";
        case PKGSIG_ERR_BADHEX:       return "malformed sha256";
        case PKGSIG_ERR_HASH_MISMATCH:return "PACKAGE TAMPERED (hash mismatch)";
        default:                      return "unknown verification error";
    }
}
