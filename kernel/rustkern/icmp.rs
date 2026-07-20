// rustkern/icmp.rs - #404 Phase L / #494 incoming ICMP parse/validate
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// Phase L port (#404 / #494): incoming ICMP packet parse/validate.
//
// TIER 2 (untrusted wire input). This is the first non-checksum wire-parser
// folded to Rust: buf/len come straight off the network (attacker-controlled),
// so length validation and every field access must be bounds-checked. The seam
// is the PURE parse extracted out of net/icmp.c icmp_handle(); the reply-send /
// echo-reply construction stays in C. net/icmp.c routes the live parse here
// under -DRUST_ICMP (else icmp_parse_c).
//
// Two weaknesses in the verbatim C reference (icmp_parse_c) that this port
// removes BY CONSTRUCTION:
//   1. No upper bound on the attacker length. The C only checks len >= 8, then
//      feeds len unchecked into ip_checksum(buf, len) and (downstream) a
//      `uint8_t reply[length]` kernel-stack VLA. This port rejects len >
//      ICMP_MAX_LEN (1500, the Ethernet MTU) BEFORE any dereference. Live the
//      length is already MTU-bounded by ip.c, so this is defense-in-depth /
//      latent-bug confinement, not a currently-reachable OOB.
//   2. In-place mutation of the `const` untrusted RX buffer: the C zeroes the
//      checksum field, recomputes, and restores it (a transient corruption /
//      aliasing hazard on a shared or DMA RX buffer). This port computes the
//      identical verdict over an IMMUTABLE slice with zero writes, using the
//      ones-complement property that summing ALL 16-bit words (checksum field
//      included) folds to 0xFFFF for a valid packet, i.e. the fold returns 0.
//
// Field decode matches the C `icmp_header_t` struct reads on x86-64: type/code
// are single bytes; checksum/id/sequence are the raw packet bytes read as
// native (little-endian) u16 exactly as `*(uint16_t*)` did (NO htons here - the
// C stored the raw field values too; the C caller applies ntohs downstream).

// Parse return codes (mirror net/icmp.h).
const ICMP_PARSE_OK: i32 = 0;
const ICMP_PARSE_ETOOSHORT: i32 = -1;
const ICMP_PARSE_EOVERSIZE: i32 = -2;
const ICMP_HDR_LEN: u32 = 8;
const ICMP_MAX_LEN: u32 = 1500;

// #[repr(C)] mirror of icmp_parsed_t (net/icmp.h). Layout is asserted == 16
// bytes on the C side (_Static_assert in icmp.c) so this can never silently
// drift. Field order + primitive types match the C struct exactly, so on
// x86-64 the two layouts are identical (offsets 0,1,2,4,6,8,10,12,14; 1 tail
// pad byte; size 16).
#[repr(C)]
pub struct IcmpParsed {
    pub type_: u8,       // buf[0]
    pub code: u8,        // buf[1]
    pub checksum: u16,   // raw
    pub id: u16,         // raw
    pub sequence: u16,   // raw
    pub total_len: u16,
    pub payload_off: u16,
    pub payload_len: u16,
    pub checksum_ok: u8,
}

/// Ones-complement (RFC 1071) fold over a byte slice, native-endian 16-bit
/// words, identical to ip_checksum_rs. For a packet whose own checksum field is
/// correct this returns 0 (all words, field included, fold to 0xFFFF). Bounds-
/// checked: every access goes through the slice, so it can never read past it.
// Ones-complement fold with the 16-bit checksum field (bytes 2..3) treated as
// ZERO: byte-for-byte what icmp_parse_c computes after `header->checksum = 0`.
//
// #404 drift audit 2 (2026-07-16): the seam USED to verify the checksum with
// icmp_ones_complement(pkt) == 0, i.e. RFC 1071's "fold everything including the
// field and expect all-ones". That is NOT the C's verdict. The C computes
// ~fold(sum with the field zeroed) and compares it against the field. The two
// disagree on exactly one state, ones-complement NEGATIVE ZERO:
//     S == 0xFFFF && field == 0xFFFF
//       C   : ~S = 0x0000, 0x0000 == 0xFFFF -> REJECT
//       RFC : fold(0xFFFF + 0xFFFF) = 0xFFFF -> ACCEPT
// Proven exhaustively over 26,214,400 evaluations: the RFC form's accept set is
// a STRICT SUPERSET of the C's (132 states where Rust accepts and C rejects, 0
// the other way). Live effect: the original sends 0 echo replies where the
// shipped Rust sends 1.
//
// The RFC form is arguably more correct. It is still the wrong thing to ship
// HERE: this seam is labelled a CONFINEMENT and the port's contract is
// "identical behavior". A semantics change must be made deliberately and
// documented, not smuggled in under a memory-safety port. So the seam now
// reproduces the C exactly. Changing MayteraOS to the RFC-1071 verdict is a
// separate decision for both implementations at once.
fn icmp_ones_complement_zeroed(data: &[u8]) -> u16 {
    let mut sum: u32 = 0;
    let n = data.len();
    let mut i = 0usize;
    while i + 1 < n {
        // Bytes 2..3 are icmp_header_t.checksum; the C zeroes them first.
        // Callers reach here only with n >= 8, so this word is always whole.
        let w = if i == 2 {
            0u32
        } else {
            (data[i] as u32) | ((data[i + 1] as u32) << 8)
        };
        sum += w;
        i += 2;
    }
    if i < n {
        sum += data[i] as u32;
    }
    while (sum >> 16) != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    (!sum) as u16
}

#[allow(dead_code)]
fn icmp_ones_complement(data: &[u8]) -> u16 {
    let mut sum: u32 = 0;
    let n = data.len();
    let mut i = 0usize;
    while i + 1 < n {
        let lo = data[i] as u32;
        let hi = data[i + 1] as u32;
        // native (little-endian) 16-bit word == *(uint16_t*)ptr on x86-64.
        sum += lo | (hi << 8);
        i += 2;
    }
    if i < n {
        sum += data[i] as u32;
    }
    while (sum >> 16) != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    (!sum) as u16
}

/// Rust port of net/icmp.c icmp_parse_c(): PURE parse/validate of an incoming
/// ICMP packet. Returns ICMP_PARSE_OK (0) when the packet is structurally valid
/// (then *out is filled and out.checksum_ok says whether the checksum verified),
/// ICMP_PARSE_ETOOSHORT (-1) for a truncated packet (len < 8), or
/// ICMP_PARSE_EOVERSIZE (-2) for an over-long claimed length (len > 1500). Never
/// sends, allocates, or mutates the input buffer. net/icmp.c routes the live
/// parse here under -DRUST_ICMP.
#[no_mangle]
pub extern "C" fn icmp_parse_rs(buf: *const u8, len: u32, out: *mut IcmpParsed) -> i32 {
    // Length gate FIRST, before any dereference. Lower bound matches the C
    // `if (len < 8)`; the upper bound is the confinement the C never had.
    if len < ICMP_HDR_LEN {
        return ICMP_PARSE_ETOOSHORT;
    }
    if len > ICMP_MAX_LEN {
        return ICMP_PARSE_EOVERSIZE;
    }

    // SAFETY: the caller (icmp_handle, via net/icmp.c) guarantees `buf` points
    // to at least `len` contiguous, readable bytes - the ICMP payload ip.c
    // handed down, whose extent is bounded by the IP total_length<=frame guard.
    // `len` is already proven to lie in [8, 1500] above, so the slice spans a
    // valid, in-bounds extent and EVERY access below (the 8-byte header + the
    // checksum fold) indexes only through it and is therefore bounds-checked by
    // Rust: no read can ever leave the slice. The buffer is used read-only and
    // is not retained past this call (unlike the C reference, which writes to
    // it to zero+restore the checksum field).
    let pkt: &[u8] = unsafe { core::slice::from_raw_parts(buf, len as usize) };

    // 8-byte header, all in-bounds (len >= 8). Native-endian u16 reads match the
    // C struct field reads.
    let type_ = pkt[0];
    let code = pkt[1];
    let checksum = u16::from_ne_bytes([pkt[2], pkt[3]]);
    let id = u16::from_ne_bytes([pkt[4], pkt[5]]);
    let sequence = u16::from_ne_bytes([pkt[6], pkt[7]]);

    // Verify the checksum WITHOUT mutating the buffer.
    // Reproduces icmp_parse_c's verdict EXACTLY (see icmp_ones_complement_zeroed):
    // ~fold(sum with the field zeroed) == field. Not RFC 1071's all-ones form,
    // which accepts a strict superset (ones-complement negative zero).
    let checksum_ok: u8 = if icmp_ones_complement_zeroed(pkt) == checksum { 1 } else { 0 };

    if !out.is_null() {
        // SAFETY: `out` is either null (checked) or a valid, writable
        // IcmpParsed* provided by the C caller (same contract as the C
        // reference, which writes the same fields). We write only POD fields.
        unsafe {
            let o = &mut *out;
            o.type_ = type_;
            o.code = code;
            o.checksum = checksum;
            o.id = id;
            o.sequence = sequence;
            o.total_len = len as u16;
            o.payload_off = ICMP_HDR_LEN as u16;
            o.payload_len = (len - ICMP_HDR_LEN) as u16;
            o.checksum_ok = checksum_ok;
        }
    }
    ICMP_PARSE_OK
}
