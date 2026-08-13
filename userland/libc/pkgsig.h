// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// pkgsig.h - App Store / updater package trust chain (#559).
//
// THREAT MODEL (read this before changing anything here).
//
// What this defends against: a hostile or compromised package repository, a
// hostile DNS answer for updates.maytera.net, and anyone able to modify traffic
// in flight (the transport is plain HTTP by deliberate design). None of those
// can get code installed, because none of them hold the signing private key.
//
// What this does NOT defend against: a hostile Ring-3 binary already on the
// image. The digest is computed in userland, so a malicious local app could
// simply not call these functions at all, and SYS_PKG_WRITE is not itself
// authenticated. That is out of scope here and is a real, separate gap; do not
// describe this layer as protecting against a compromised local app.
//
// DESIGN: the Debian apt model.
//   1. manifest.json is signed with a DETACHED signature (manifest.json.sig).
//      Verifying it makes the manifest the trust anchor.
//   2. The manifest carries a sha256 for every package, so that one signature
//      transitively covers every .mpkg without per-package signatures.
//   3. A downloaded .mpkg is checked against its manifest sha256 before a
//      single byte of it is unpacked or written.
//
// The signature is RSA-2048 PKCS#1 v1.5 over SHA-256, matching
// `openssl dgst -sha256 -sign`. It is NOT verified here: the digest is handed
// to the kernel via SYS_OTA_VERIFY_SIG, which checks it against a public key
// baked into the kernel image (kernel/proc/ota_pubkey.h). That matters, because
// it means the public key is not a file on the filesystem that Ring-3 could
// swap out; substituting it requires replacing the kernel.
//
// FAIL CLOSED. Every function here returns 0 only on a positive verification.
// Any error, short read, malformed input, or missing signature returns non-zero
// and the caller MUST abort the install. There is deliberately no "warn and
// continue" path and no unverified fallback.

#ifndef _LIBC_PKGSIG_H
#define _LIBC_PKGSIG_H

#include "types.h"

// Raw detached signature size: RSA-2048 => 256 bytes. The signature file is
// exactly this many bytes of raw binary (no armor, no container, nothing to
// parse). Anything else is rejected on length alone.
#define PKGSIG_SIG_LEN 256

// Result codes. 0 == verified, everything else == refuse to install.
enum {
    PKGSIG_OK             =  0,
    PKGSIG_ERR_ARG        = -1,  // null/degenerate arguments
    PKGSIG_ERR_SIGLEN     = -2,  // signature not exactly PKGSIG_SIG_LEN bytes
    PKGSIG_ERR_BADSIG     = -3,  // kernel rejected the signature
    PKGSIG_ERR_NOHASH     = -4,  // manifest entry carried no/!64-char sha256
    PKGSIG_ERR_BADHEX     = -5,  // sha256 field was not valid hex
    PKGSIG_ERR_HASH_MISMATCH = -6 // content hash != manifest hash (TAMPERED)
};

// Verify a detached signature over an in-memory manifest.
// `sig` must be exactly PKGSIG_SIG_LEN bytes of raw signature.
// Returns PKGSIG_OK only if the kernel confirms the signature over
// sha256(manifest) against the baked-in public key.
int pkgsig_verify_manifest(const void *manifest, size_t manifest_len,
                           const uint8_t *sig, size_t sig_len);

// Verify downloaded package bytes against the 64-char lowercase hex sha256
// taken from the (already signature-verified) manifest.
// Returns PKGSIG_OK only on an exact digest match.
int pkgsig_verify_package(const void *data, size_t len, const char *want_hex);

// #570: streaming variant. Verify an ALREADY-COMPUTED sha256 digest against the
// 64-char lowercase hex sha256 from the (signature-verified) manifest. This is
// for the large-package download path, which cannot hold the whole package in
// one contiguous buffer: it hashes the bytes incrementally (sha256_update over
// each chunk) while streaming them to disk, then verifies the final digest here.
// The trust contract is identical to pkgsig_verify_package(); the ONLY
// difference is where the digest was computed. Returns PKGSIG_OK only on an
// exact match. `digest` must be SHA256_DIGEST_SIZE (32) bytes.
int pkgsig_verify_package_digest(const uint8_t *digest, const char *want_hex);

// Human-readable reason for a failure code, for status lines and logs.
const char *pkgsig_strerror(int rc);

#endif // _LIBC_PKGSIG_H
