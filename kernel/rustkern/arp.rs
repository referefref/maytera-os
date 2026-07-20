// rustkern/arp.rs - #404 Phase M / #495 incoming ARP frame parse/validate
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ---------------------------------------------------------------------------
// Phase M port (#404 / #495): incoming ARP frame parse/validate.
//
// TIER 2 (untrusted wire input). buf/len come straight off the network
// (attacker-controlled). The seam is the PURE parse extracted out of
// net/arp.c arp_handle(); the DAD check, cache-add, and reply-send stay in C.
// net/arp.c routes the live parse here under -DRUST_ARP (else arp_parse_c).
//
// Unlike the ICMP port, the C reference here is ALREADY memory-safe: it
// length-gates the frame (len >= 28) before any field read, reads every field
// at a FIXED offset inside those 28 bytes (hw_len/proto_len are validated == 6/4
// but never used to compute an offset), and never mutates the buffer. So there
// is NO reachable OOB. This port's value is DEFENSE-IN-DEPTH: it removes the
// unchecked-wire-pointer-arithmetic CLASS by construction (every access is a
// bounds-checked index into a slice of exactly len bytes), so if the fixed-offset
// assumption were ever broken (struct change, or a future variable-length HW/proto
// address keyed off hw_len/proto_len) the C would silently gain an OOB while this
// Rust could not. Honest latent-not-reachable hardening, not a live-bug fix.
//
// Field decode matches the C arp_header_t reads on x86-64: hw_type/proto_type/
// operation are the wire big-endian fields decoded to host order (ntohs);
// sender_ip/target_ip are the wire IPs decoded to host order (ntohl); sender_mac
// and sender_ip_be keep the RAW wire bytes (the C reply path copies sender_ip_be
// verbatim into reply.target_ip).

// Parse return codes (mirror net/arp.h).
const ARP_PARSE_OK: i32 = 0;
const ARP_PARSE_ETOOSHORT: i32 = -1;
const ARP_PARSE_EINVAL: i32 = -2;
const ARP_HDR_LEN: u32 = 28;
const ARP_HW_ETHERNET: u16 = 1;
const ARP_ETH_TYPE_IPV4: u16 = 0x0800;

// #[repr(C)] mirror of arp_parsed_t (net/arp.h). Layout is asserted == 28 bytes
// on the C side (_Static_assert in arp.c) so this can never silently drift.
// Field order + primitive types match the C struct exactly; on x86-64 the two
// layouts are identical (offsets 0,2,4,6,7,8,12,16,22; size 28, align 4).
#[repr(C)]
pub struct ArpParsed {
    pub hw_type: u16,          // 0: host order
    pub proto_type: u16,       // 2: host order
    pub operation: u16,        // 4: host order
    pub hw_len: u8,            // 6
    pub proto_len: u8,         // 7
    pub sender_ip: u32,        // 8: host order
    pub target_ip: u32,        // 12: host order
    pub sender_mac: [u8; 6],   // 16
    pub sender_ip_be: [u8; 4], // 22: raw network bytes
}

/// Rust port of net/arp.c arp_parse_c(): PURE parse/validate of an incoming ARP
/// frame. Returns ARP_PARSE_OK (0) for a structurally valid Ethernet/IPv4 ARP
/// (then *out is filled), ARP_PARSE_ETOOSHORT (-1) for a truncated frame
/// (len < 28), or ARP_PARSE_EINVAL (-2) when the hw/proto type or hlen/plen is
/// not Ethernet/IPv4. Never sends, mutates the cache, or writes the input buffer.
/// net/arp.c routes the live parse here under -DRUST_ARP.
#[no_mangle]
pub extern "C" fn arp_parse_rs(buf: *const u8, len: u32, out: *mut ArpParsed) -> i32 {
    // Length gate FIRST, before any dereference (matches the C `if (len < 28)`).
    if len < ARP_HDR_LEN {
        return ARP_PARSE_ETOOSHORT;
    }

    // SAFETY: the caller (arp_handle, via net/arp.c) guarantees `buf` points to at
    // least `len` contiguous, readable bytes - the ARP frame eth_receive handed
    // down. `len` is proven >= 28 above, so the slice spans a valid, in-bounds
    // extent and EVERY access below (the fixed 28-byte header) indexes only through
    // it and is therefore bounds-checked by Rust: no read can leave the slice. The
    // buffer is used read-only and is not retained past this call.
    let pkt: &[u8] = unsafe { core::slice::from_raw_parts(buf, len as usize) };

    // Fixed 28-byte header, all in-bounds (len >= 28). Big-endian wire fields
    // decoded to host order == ntohs/ntohl exactly as the C struct reads did.
    let hw_type = u16::from_be_bytes([pkt[0], pkt[1]]);
    let proto_type = u16::from_be_bytes([pkt[2], pkt[3]]);
    let hw_len = pkt[4];
    let proto_len = pkt[5];
    let operation = u16::from_be_bytes([pkt[6], pkt[7]]);

    // Validate exactly as the C header check.
    if hw_type != ARP_HW_ETHERNET
        || proto_type != ARP_ETH_TYPE_IPV4
        || hw_len != 6
        || proto_len != 4
    {
        return ARP_PARSE_EINVAL;
    }

    if !out.is_null() {
        let sender_ip = u32::from_be_bytes([pkt[14], pkt[15], pkt[16], pkt[17]]);
        let target_ip = u32::from_be_bytes([pkt[24], pkt[25], pkt[26], pkt[27]]);
        // SAFETY: `out` is either null (checked) or a valid, writable ArpParsed*
        // provided by the C caller (same contract as the C reference). POD writes.
        unsafe {
            let o = &mut *out;
            o.hw_type = hw_type;
            o.proto_type = proto_type;
            o.operation = operation;
            o.hw_len = hw_len;
            o.proto_len = proto_len;
            o.sender_ip = sender_ip;
            o.target_ip = target_ip;
            o.sender_mac.copy_from_slice(&pkt[8..14]);
            o.sender_ip_be.copy_from_slice(&pkt[14..18]);
        }
    }
    ARP_PARSE_OK
}
