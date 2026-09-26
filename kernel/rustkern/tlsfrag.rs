// rustkern/tlsfrag.rs - the OUTGOING TLS record fragmentation plan.
//
// WHY THIS EXISTS (measured 2026-09-25, golden 2474).
//
// net/tls/tls.c tls_send() handed its ENTIRE payload to one record:
//
//     int tls_send(ctx, data, length) {
//         ... tls13_encrypt_record(..., data, length, enc, &enc_len, ...);
//         ret = tls_send_record(ctx, TLS_CONTENT_APPLICATION, enc, enc_len);
//     }
//
// with no fragmentation anywhere. RFC 5246 6.2.1 and RFC 8446 5.1 both cap
// TLSPlaintext.length at 2^14 = 16384 bytes, and a conforming server answers an
// oversized record with a FATAL record_overflow alert (22) and drops the
// connection. tls.h has carried the right constant, TLS_MAX_PLAINTEXT_SIZE,
// since #497 and had ZERO users in the whole tree: the receive side was bounded,
// the send side never was.
//
// Effect: every HTTPS request whose request line + headers + body exceeded
// 16384 bytes died as an opaque "net/TLS error". A Maytera Flow posting a
// 7701-byte JPEG to a vision endpoint (about 11 KB of body after base64 + JSON,
// so about 11.5 KB on the wire) SUCCEEDED; the same flow with a 15639-byte JPEG
// (about 23 KB of body) FAILED with `Network error (POST returned -1)`. Neither
// declared limit explains that: aiclient.c's BODY_MAX is 65536 and the kernel
// POST cap is 128 KB. The real ceiling was 16384 bytes of whole-request, which
// is a number nothing in the tree ever named on the send path.
//
// SECOND, WORSE DEFECT closed by the same seam. tls_send_record() takes a
// size_t length and passes it to tls_build_record_header(..., uint16_t length),
// a silent narrowing that no warning in the build catches (-Wall -Wextra does
// not imply -Wconversion). At a legal BODY_MAX-sized body the ciphertext record
// body exceeds 65535, the declared length wraps to a small number while the full
// ciphertext is still written to the socket, and the peer's record framing
// desyncs against a garbage stream. Never observed, because nothing got that far
// before the 16 KB ceiling killed it first, but it was loaded and waiting.
//
// WHY RUST. New kernel logic defaults to Rust (CLAUDE.md). This is pure,
// leaf-level length arithmetic with no float, no allocation and no entanglement
// with paging or asm, so there is no performance justification for C. Keeping it
// here also makes it host-testable: tools/tls-frag-test `include!`s THIS file, so
// the test cannot drift from what ships.

// MUST stay equal to net/tls/tls.h TLS_MAX_PLAINTEXT_SIZE. A _Static_assert on
// the C side (net/tls/tls.c) locks the two together.
pub const TLS_MAX_PLAINTEXT_RS: u32 = 16384; // 2^14, RFC 5246 6.2.1 / RFC 8446 5.1
// MUST stay equal to net/tls/tls.h TLS_MAX_RECORD_SIZE (2^14 + 2048): the
// largest CIPHERTEXT record body this stack will emit or accept.
pub const TLS_MAX_RECORD_RS: u32 = 18432;

/// How many plaintext bytes the NEXT outgoing record may carry.
///
/// `remaining` is how much of the caller's payload is still unsent;
/// `max_plaintext` is the caller's cap, clamped into 1..=TLS_MAX_PLAINTEXT_RS so
/// a caller can never widen the protocol limit and can never pass 0 and spin.
///
/// Returns 0 only when `remaining` is 0, so a `while (off < len)` loop driven by
/// this function always makes forward progress and always terminates.
#[no_mangle]
pub extern "C" fn tls_out_fragment_rs(remaining: u64, max_plaintext: u32) -> u32 {
    let cap: u32 = if max_plaintext == 0 || max_plaintext > TLS_MAX_PLAINTEXT_RS {
        TLS_MAX_PLAINTEXT_RS
    } else {
        max_plaintext
    };
    let n: u32 = if remaining >= cap as u64 { cap } else { remaining as u32 };
    n
}

/// Is `body_len` a legal length for the body of ONE outgoing TLS record?
///
/// Returns 1 for legal, 0 for illegal. This is the last gate before the 5-byte
/// record header is built, and it is what makes the uint16_t narrowing in
/// tls_build_record_header() unreachable rather than merely unlikely: anything
/// that would not fit the 2-byte length field is refused long before it gets
/// there, with a named limit instead of a corrupted stream.
#[no_mangle]
pub extern "C" fn tls_out_record_ok_rs(body_len: u64) -> i32 {
    if body_len > TLS_MAX_RECORD_RS as u64 {
        0
    } else {
        1
    }
}

/// The plaintext ceiling, for C callers and for the log line that names it.
#[no_mangle]
pub extern "C" fn tls_out_max_plaintext_rs() -> u32 {
    TLS_MAX_PLAINTEXT_RS
}
