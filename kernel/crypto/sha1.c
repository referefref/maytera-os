// sha1.c - the C ABI seam for the Rust SHA-1 / HMAC-SHA1 primitive.
//
// THIS FILE CONTAINS NO ALGORITHM. SHA-1 lives in rustkern/sha1.rs and
// HMAC-SHA1 in rustkern/hmac.rs (through the one shared RFC 2104 construction,
// hmac_construct, that HMAC-SHA256/384/MD5 already use). Both are new code, so
// there is no C twin, no -DRUST_SHA1 strangler flag and nothing to switch
// between; see rustkern/sha1.rs's header for why SHA-1 is here at all and why
// that is not an endorsement of it.
//
// The file earns its place by doing two things that cannot be done from Rust or
// from a header alone:
//
//  1. IT GIVES sha1.h A TRANSLATION UNIT. The _Static_assert pair in sha1.h
//     pins sizeof(sha1_ctx_t) == 96 and its alignment against the #[repr(C)]
//     Sha1Ctx in rustkern/sha1.rs. A layout lock in a header that nothing
//     includes locks nothing: unlike a missing Rust export (rust-symbol-gate
//     catches that) or a missing symbol (the link catches that), a struct
//     layout divergence across the FFI produces no diagnostic at all, just
//     wrong memory. Until the first real consumer includes sha1.h, this is the
//     only place the assert is compiled, and it is compiled on every build.
//
//  2. It exposes hmac_sha1() under the ordinary un-suffixed name C callers
//     expect, exactly as crypto/hmac.c exposes hmac_md5() over hmac_md5_rs, so
//     a caller does not have to know or care which language the MAC is in.
//
// WHY THIS SEAM IS C, since new kernel code is Rust unless justified
// (CLAUDE.md): it is not new logic, it is the C side of the boundary itself.
// The _Static_assert is a C construct that only a C compiler can evaluate, and
// the dispatcher exists solely so the C-visible name matches the house
// convention. Writing either in Rust is not possible; writing them anywhere
// else means the layout lock is never compiled.

#include "sha1.h"

// The Rust HMAC-SHA1 one-shot (rustkern/hmac.rs). Same argument contract as
// every other hmac_*_rs: key/data may be any length, `mac` gets exactly
// SHA1_DIGEST_SIZE bytes.
extern void hmac_sha1_rs(const void *key, size_t key_len,
                         const void *data, size_t data_len, uint8_t *mac);

// Live HMAC-SHA1 one-shot. One line on purpose: there is no C implementation to
// fall back to, so this is a name, not a choice.
void hmac_sha1(const void *key, size_t key_len,
               const void *data, size_t data_len,
               uint8_t mac[SHA1_DIGEST_SIZE]) {
    hmac_sha1_rs(key, key_len, data, data_len, mac);
}
