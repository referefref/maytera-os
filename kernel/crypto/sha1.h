// sha1.h - SHA-1 (FIPS 180-4) and HMAC-SHA1, the kernel's SHARED primitives.
//
// The implementation is RUST: rustkern/sha1.rs (the hash) and rustkern/hmac.rs
// (HMAC-SHA1 through the one shared RFC 2104 construction every other kernel
// HMAC already uses). There is deliberately NO C implementation of SHA-1 in
// this tree and there must not be a second one of anything below: this header
// is the only C-visible SHA-1 surface.
//
// WHY SHA-1 EXISTS HERE AT ALL: RFC 6238 TOTP specifies HMAC-SHA1 (via RFC
// 4226) and it is what every authenticator app in the field speaks, so the
// kernel-side secrets store needs it for interoperability. SHA-1 IS
// COLLISION-BROKEN (SHAttered 2017, chosen-prefix 2020) and must NEVER be used
// here for a signature, a certificate, a code-signing digest or a content
// address; use sha256()/sha512() from crypto.h for those. HMAC-SHA1 is NOT
// affected by the collision attacks (HMAC rests on the keyed compression
// function as a PRF, not on collision resistance), which is the whole and only
// reason this is acceptable. rustkern/sha1.rs's header states the same at
// length; if you are about to add a caller, read it.
#ifndef SHA1_H
#define SHA1_H

#include "../types.h"

#define SHA1_BLOCK_SIZE   64
#define SHA1_DIGEST_SIZE  20

// Mirror of the #[repr(C)] struct Sha1Ctx in rustkern/sha1.rs. The FIELD ORDER
// is the FFI contract, chosen so the layout has no padding at all:
//   len 8 @0, h 20 @8, buf_len 4 @28, buf 64 @32  =>  96 bytes, align 8.
// `len` counts MESSAGE BYTES; the padding's bit length is len * 8 big-endian.
typedef struct {
    uint64_t len;
    uint32_t h[5];
    uint32_t buf_len;
    uint8_t  buf[SHA1_BLOCK_SIZE];
} sha1_ctx_t;

// The layout lock (the house sizeof-lock convention). rustkern/sha1.rs pins the
// same two numbers independently with const asserts, so a field reordering on
// either side fails a compile instead of silently mismatching across the FFI.
// This is the gate nothing else provides: unlike a missing Rust export, a
// layout divergence produces no link error, just wrong memory.
_Static_assert(sizeof(sha1_ctx_t) == 96, "sha1_ctx_t must match rustkern/sha1.rs Sha1Ctx (96 bytes)");
_Static_assert(__alignof__(sha1_ctx_t) == 8, "sha1_ctx_t must be 8-aligned to match Rust Sha1Ctx");

// And the OTHER half of the contract, which crypto/hmac.c makes for the three
// hash contexts that existed before this one: HMAC-SHA1 runs through the shared
// hmac_construct(), which keeps each hash context in an OPAQUE 256-byte,
// 16-aligned buffer rather than duplicating any struct layout in Rust. That
// buffer must be at least as large as this ctx. 96 <= 256 is obvious today and
// that is exactly why it is worth writing down: the assert is what makes a
// future field growth a build failure instead of a silent Rust-side overflow.
_Static_assert(sizeof(sha1_ctx_t) <= 256, "sha1_ctx_t exceeds the Rust HMAC opaque ctx buffer");

// Streaming SHA-1 (rustkern/sha1.rs). `out` needs SHA1_DIGEST_SIZE bytes.
void sha1_init(sha1_ctx_t *ctx);
void sha1_update(sha1_ctx_t *ctx, const void *data, size_t len);
void sha1_final(sha1_ctx_t *ctx, uint8_t out[SHA1_DIGEST_SIZE]);

// One-shot SHA-1, same shape as md5()/sha256().
void sha1(const void *data, size_t len, uint8_t out[SHA1_DIGEST_SIZE]);

// HMAC-SHA1 one-shot (RFC 2104), B=64 D=20. The live, un-suffixed name C
// callers use; crypto/sha1.c routes it to hmac_sha1_rs, mirroring how
// crypto/hmac.c exposes hmac_md5() over hmac_md5_rs.
void hmac_sha1(const void *key, size_t key_len,
               const void *data, size_t data_len,
               uint8_t mac[SHA1_DIGEST_SIZE]);

// Boot-time known-answer self-test (rustkern/sha1.rs sha1_selftest_rs). Returns
// the number of FAILED checks, 0 on pass, -1 if zero checks ran. main.c prints
// the verdict to serial and to /BOOTLOG.TXT on every boot.
int32_t sha1_selftest_rs(uint32_t *out_checks);

#endif // SHA1_H
