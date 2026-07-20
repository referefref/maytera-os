// rustkern/dns.rs - #404 Phase N / #496 incoming DNS response parse/validate
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ---------------------------------------------------------------------------
// Phase N port (#404 / #496): incoming DNS response message parse/validate.
//
// TIER 2 (untrusted wire input). msg/msglen come straight off a spoofable UDP
// datagram (attacker-controlled). The seam is the PURE parse extracted out of
// net/dns.c dns_handle_response(): validate the QR bit + rcode, read ancount,
// skip the question name (compression-pointer-aware), then walk the answer
// records for the first A record and honor its TTL. The UDP receive, the
// transaction-id / QR-source match, the cache-put, and the dns_query bookkeeping
// stay in C. net/dns.c routes the live parse here under -DRUST_DNS (else _c).
//
// Like the ARP port, the verbatim C reference is ALREADY memory-safe: its
// dns_skip_name is bounded (pos strictly increases by >=1 each iteration, and a
// compression pointer returns immediately - so it can NEVER loop or read past
// msglen), and every record-field read is length-gated (pos+4 / pos+10 /
// pos+rdlength <= length) before the access. So there is NO reachable OOB or
// hang even on pointer loops, OOB pointers, oversized rdlength, or a label
// running past the end. This port removes the unchecked-wire-pointer-arithmetic
// class by construction, AND - the genuinely stronger point vs arp - it pre-
// confines the classic DNS compression-pointer-FOLLOW loop/OOB class that would
// become reachable the instant this parser is extended to actually DECODE a name
// (CNAME/PTR target extraction, logging the answer owner name). The Rust skip
// carries a strictly-decreasing visited budget so even a future pointer-follow
// could not loop; the raw-pointer C form could not bound it. Honest latent-not-
// reachable hardening + class elimination today, not a live-bug fix.
//
// Field decode matches the C reads on x86-64: the header flags/ancount are wire
// big-endian; ip/ttl are host order (assembled big-endian byte-by-byte exactly
// like the C `(buf[..]<<24)|...`). Output through DnsResult (status/rcode/ip/ttl).

// Parse status codes (mirror net/dns.h DNS_PARSE_*).
const DNS_PARSE_A_FOUND: i32 = 0;
const DNS_PARSE_NOT_RESPONSE: i32 = 1;
const DNS_PARSE_RCODE_ERR: i32 = 2;
const DNS_PARSE_NO_ANSWER: i32 = 3;
const DNS_PARSE_FORMAT_ERR: i32 = 4;
const DNS_PARSE_NO_A: i32 = 5;

const DNS_HDR_LEN: usize = 12;      // sizeof(dns_header_t)
const DNS_RCODE_OK_RS: i32 = 0;
const DNS_TYPE_A_RS: u16 = 1;       // DNS_TYPE_A

// #[repr(C)] mirror of dns_result_t (net/dns.h). Layout is asserted == 16 bytes
// on the C side (_Static_assert in dns.c) so this can never silently drift.
// Field order + primitive types match the C struct exactly (offsets 0,4,8,12).
#[repr(C)]
pub struct DnsResult {
    pub status: i32,   // DNS_PARSE_*
    pub rcode: i32,    // meaningful only for DNS_PARSE_RCODE_ERR
    pub ip: u32,       // host order (A_FOUND only)
    pub ttl: u32,      // seconds (A_FOUND only)
}

/// POD write of a parse result through the (possibly-null) C out pointer.
#[inline]
fn dns_write_result(out: *mut DnsResult, status: i32, rcode: i32, ip: u32, ttl: u32) {
    if !out.is_null() {
        // SAFETY: `out` is either null (checked) or a valid, writable DnsResult*
        // provided by the C caller (same contract as the C reference). POD only.
        unsafe {
            let o = &mut *out;
            o.status = status;
            o.rcode = rcode;
            o.ip = ip;
            o.ttl = ttl;
        }
    }
}

/// Skip a DNS name, verbatim-equivalent to net/dns.c dns_skip_name but every
/// access is a bounds-checked index into `buf`. Returns the new offset (>= 0) or
/// -1 if the name runs off the end. Does NOT follow the compression pointer (a
/// pointer TERMINATES the skip, matching the C), so it cannot loop; a strictly-
/// decreasing visited budget bounds the walk defensively (it can never fire
/// before `pos >= max_len` since `pos` grows by >= 1 each iteration, so it never
/// diverges from the C). `max_len <= buf.len()` is guaranteed by the caller.
fn dns_skip_name_rs(buf: &[u8], mut pos: usize, max_len: usize) -> i64 {
    let mut budget = max_len + 1;
    while pos < max_len {
        if budget == 0 {
            return -1;
        }
        budget -= 1;
        let len = buf[pos];
        if len == 0 {
            return (pos + 1) as i64;
        }
        if (len & 0xC0) == 0xC0 {
            return (pos + 2) as i64;   // pointer terminates the name (not followed)
        }
        pos += (len as usize) + 1;
    }
    -1
}

/// Rust port of net/dns.c dns_parse_response_c(): PURE parse/validate of an
/// incoming DNS response message. Returns DNS_PARSE_A_FOUND (0) with *out.ip/ttl
/// set when the first answer A record is found; DNS_PARSE_NOT_RESPONSE (1) when
/// the QR bit is clear; DNS_PARSE_RCODE_ERR (2) with *out.rcode set for a non-zero
/// rcode; DNS_PARSE_NO_ANSWER (3) for ancount==0; DNS_PARSE_FORMAT_ERR (4) for a
/// short header or malformed question name; DNS_PARSE_NO_A (5) when answers are
/// present but none is an A record. Never sends, allocates, touches a global, or
/// mutates the input. net/dns.c routes the live parse here under -DRUST_DNS.
#[no_mangle]
pub extern "C" fn dns_parse_response_rs(msg: *const u8, msglen: u32, out: *mut DnsResult) -> i32 {
    let mlen = msglen as usize;

    // Header gate FIRST, before any dereference (matches the C
    // `if (msglen < sizeof(dns_header_t))`).
    if mlen < DNS_HDR_LEN {
        dns_write_result(out, DNS_PARSE_FORMAT_ERR, 0, 0, 0);
        return DNS_PARSE_FORMAT_ERR;
    }

    // SAFETY: the caller (dns_handle_response, via net/dns.c) guarantees `msg`
    // points to at least `msglen` contiguous, readable bytes - the UDP payload
    // handed down by the DNS-port bind. `mlen` is proven >= 12 above, so the
    // slice spans a valid, in-bounds extent and EVERY access below (header +
    // label walk + record fields) indexes only through it and is therefore
    // bounds-checked by Rust: no read can ever leave the slice. Read-only; the
    // buffer is not retained past this call.
    let buf: &[u8] = unsafe { core::slice::from_raw_parts(msg, mlen) };

    // Flags (buf[2..3], big-endian == ntohs(hdr->flags)).
    let flags: u16 = ((buf[2] as u16) << 8) | (buf[3] as u16);
    if (flags & 0x8000) == 0 {
        dns_write_result(out, DNS_PARSE_NOT_RESPONSE, 0, 0, 0);
        return DNS_PARSE_NOT_RESPONSE;
    }

    let rcode = (flags & 0x0F) as i32;
    if rcode != DNS_RCODE_OK_RS {
        dns_write_result(out, DNS_PARSE_RCODE_ERR, rcode, 0, 0);
        return DNS_PARSE_RCODE_ERR;
    }

    let ancount: u16 = ((buf[6] as u16) << 8) | (buf[7] as u16);
    if ancount == 0 {
        dns_write_result(out, DNS_PARSE_NO_ANSWER, 0, 0, 0);
        return DNS_PARSE_NO_ANSWER;
    }

    let max_len = mlen;   // == the C `length`

    // Skip the question QNAME.
    let sk = dns_skip_name_rs(buf, DNS_HDR_LEN, max_len);
    if sk < 0 || (sk as usize) + 4 > max_len {
        dns_write_result(out, DNS_PARSE_FORMAT_ERR, 0, 0, 0);
        return DNS_PARSE_FORMAT_ERR;
    }
    let mut pos = (sk as usize) + 4;   // + QTYPE + QCLASS

    // Walk answers for the first A record; honor its TTL.
    let mut i: u32 = 0;
    while i < ancount as u32 && pos < max_len {
        let sk2 = dns_skip_name_rs(buf, pos, max_len);
        if sk2 < 0 || (sk2 as usize) + 10 > max_len {
            break;
        }
        let p = sk2 as usize;
        let rtype: u16 = ((buf[p] as u16) << 8) | (buf[p + 1] as u16);
        let rttl: u32 = ((buf[p + 4] as u32) << 24)
            | ((buf[p + 5] as u32) << 16)
            | ((buf[p + 6] as u32) << 8)
            | (buf[p + 7] as u32);
        let rdlength: u16 = ((buf[p + 8] as u16) << 8) | (buf[p + 9] as u16);
        pos = p + 10;

        if pos + rdlength as usize > max_len {
            break;
        }

        if rtype == DNS_TYPE_A_RS && rdlength == 4 {
            let a: u32 = ((buf[pos] as u32) << 24)
                | ((buf[pos + 1] as u32) << 16)
                | ((buf[pos + 2] as u32) << 8)
                | (buf[pos + 3] as u32);
            dns_write_result(out, DNS_PARSE_A_FOUND, 0, a, rttl);
            return DNS_PARSE_A_FOUND;
        }

        pos += rdlength as usize;
        i += 1;
    }

    dns_write_result(out, DNS_PARSE_NO_A, 0, 0, 0);
    DNS_PARSE_NO_A
}
