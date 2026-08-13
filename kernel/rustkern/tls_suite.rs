// rustkern/tls_suite.rs - the client's offered cipher suite list, and the
// acceptance test for what the server selects.
//
// THE DEFECT
//
// net/tls/tls.c built its ClientHello from a private `static const uint16_t
// suites[]`, and then, on the ServerHello path, did:
//
//     ctx->tls13_cipher_suite = ctx->cipher_suite;      // tls.c:1955
//
// with nothing in between. The server's choice was adopted verbatim. There are
// three separate ways that hurts, and the harness case exercises all three:
//
//   * A NON-TLS-1.3 code point selected alongside supported_versions=1.3. Pick
//     0x002f (TLS_RSA_WITH_AES_128_CBC_SHA) and the client's own key schedule
//     falls through to `key_bits = 128` AES-GCM, because every downstream test
//     in tls13.c is written as "is it 0x1302? is it 0x1303? otherwise
//     AES-128-GCM". So a suite that is neither AEAD nor forward-secret was
//     silently treated as one that is.
//   * A GREASE value. We emit one at the head of the list for fingerprint
//     shape (RFC 8701). It exists precisely to be ignored, and a server
//     selecting it was being obeyed.
//   * Any suite we never offered at all, including ones we would have refused.
//
// WHY THE LIST LIVES HERE AND NOT IN tls.c
//
// The obvious fix is a whitelist in the ServerHello handler. That creates two
// lists that must agree forever, and this tree's recurring failure is exactly
// that: a description and an artifact drifting apart until the check is
// checking something nobody offers. So the ClientHello builder now ITERATES
// THIS ARRAY, and the acceptance test reads THE SAME ARRAY. Adding a suite to
// the offer automatically makes it acceptable; removing one automatically makes
// it refused. There is no second place to forget.

/// The cipher suites the ClientHello offers, in order.
///
/// GREASE is deliberately NOT in here. tls.c emits it separately at the head of
/// the list because it must never be acceptable, and putting it in the shared
/// array would make it acceptable by construction, which is the exact bug this
/// module exists to prevent.
const OFFERED: [u16; 15] = [
    // TLS 1.3
    0x1301, // TLS_AES_128_GCM_SHA256
    0x1302, // TLS_AES_256_GCM_SHA384
    0x1303, // TLS_CHACHA20_POLY1305_SHA256
    // TLS 1.2 ECDHE. Offered for shape; tls.c forces 1.3 via key_share plus
    // supported_versions, and separately restricts what 1.2 may negotiate.
    0xc02b, 0xc02f, 0xc02c, 0xc030, 0xcca9, 0xcca8, 0xc013, 0xc014,
    // TLS 1.2 non-ECDHE, offered for shape only.
    0x009c, 0x009d, 0x002f, 0x0035,
];

/// The subset of OFFERED that is a TLS 1.3 cipher suite. RFC 8446 B.4 defines
/// the 0x13xx block; nothing outside it can key a 1.3 connection, whatever the
/// server says.
fn is_tls13_code_point(suite: u16) -> bool {
    matches!(suite, 0x1301 | 0x1302 | 0x1303)
}

/// How many suites the ClientHello should emit.
#[no_mangle]
pub extern "C" fn tls_suite_offered_count_rs() -> u32 {
    OFFERED.len() as u32
}

/// The i'th offered suite, or 0 if `i` is out of range. 0x0000 is
/// TLS_NULL_WITH_NULL_NULL and is never a legal selection, so an out-of-range
/// read cannot accidentally produce a usable suite.
#[no_mangle]
pub extern "C" fn tls_suite_offered_at_rs(i: u32) -> u16 {
    match OFFERED.get(i as usize) {
        Some(&s) => s,
        None => 0,
    }
}

/// Was `suite` in the list we actually offered?
#[no_mangle]
pub extern "C" fn tls_suite_was_offered_rs(suite: u16) -> i32 {
    if OFFERED.contains(&suite) {
        1
    } else {
        0
    }
}

/// May the server select `suite` for a TLS 1.3 connection?
///
/// Both halves are required. "Offered" alone would admit 0x002f, which we do
/// offer but only for a 1.2 negotiation. "Is a 1.3 code point" alone would
/// admit a 1.3 suite we had chosen not to offer. Fail closed on either.
#[no_mangle]
pub extern "C" fn tls13_suite_acceptable_rs(suite: u16) -> i32 {
    if is_tls13_code_point(suite) && OFFERED.contains(&suite) {
        1
    } else {
        0
    }
}
