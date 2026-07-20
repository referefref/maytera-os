// rustkern/checksum.rs - #404 IP / TCP / UDP ones-complement checksums (RFC 1071)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

/// Faithful Rust port of net/ip.c : ip_checksum() (RFC 1071 ones-complement
/// Internet checksum). Byte-for-byte equivalent to the C version on x86-64:
/// 16-bit words are read in NATIVE (little-endian) order exactly as the C
/// read `*(const uint16_t*)ptr` does, a trailing odd byte is added as its raw
/// value, the carries are folded, and the ones-complement of the low 16 bits
/// is returned. No htons / endianness change is applied (matching the C).
/// This is the PoC-proven port (207,507 vectors byte-identical, live HTTP 200);
/// net/ip.c routes the real ip_checksum symbol here under -DRUST_IP_CHECKSUM.
#[no_mangle]
pub extern "C" fn ip_checksum_rs(data: *const u8, length: u16) -> u16 {
    let mut sum: u32 = 0;
    let mut len: u16 = length;
    let mut off: isize = 0;

    // SAFETY: The caller guarantees that `data` points to at least `length`
    // contiguous, readable bytes (the same contract the C `ip_checksum` relies
    // on). We never read past `length` bytes: the main loop consumes 2 bytes
    // per iteration while len > 1, and the final branch reads exactly 1 more
    // byte only when len == 1. `data` is used read-only and is not retained.
    unsafe {
        while len > 1 {
            let lo = *data.offset(off) as u32;
            let hi = *data.offset(off + 1) as u32;
            // Little-endian 16-bit word == native *(uint16_t*)ptr on x86-64.
            sum += lo | (hi << 8);
            off += 2;
            len -= 2;
        }
        if len > 0 {
            sum += *data.offset(off) as u32;
        }
    }

    // Fold carries into the low 16 bits (C: while (sum >> 16) ...).
    while (sum >> 16) != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    // C returns ~sum truncated to uint16_t.
    (!sum) as u16
}

// ===========================================================================
// Phase D port (#404 / #486): IP-layer transport checksums (TCP + UDP).
//
// Faithful, memory-safe Rust drop-ins for the ones-complement (RFC 1071)
// transport checksums. Both prepend the standard IPv4 pseudo-header (src IP,
// dst IP, zero + protocol, transport length) to the payload, then fold exactly
// like ip_checksum_rs (reusing the proven native-endian 16-bit word read). The
// src_ip / dest_ip arguments arrive ALREADY in network byte order (the C
// callers pass htonl(ip)), so we sum their high and low 16-bit halves directly,
// byte-for-byte matching the C reference tcp_checksum_c / udp_checksum_c.

/// Rust port of net/tcp.c tcp_checksum_c(). Byte-for-byte equivalent: pseudo-
/// header (src/dst IP halves in network order, htons(IP_PROTO_TCP=6)=0x0600,
/// htons(length)) then the native-endian 16-bit fold over the TCP segment, a
/// trailing odd byte added raw, carry fold, ones-complement truncated to u16.
/// net/tcp.c routes the live tcp_checksum symbol here under -DRUST_TCP_CHECKSUM.
#[no_mangle]
pub extern "C" fn tcp_checksum_rs(src_ip: u32, dest_ip: u32, data: *const u8, length: u16) -> u16 {
    let mut sum: u32 = 0;

    // Pseudo-header. src_ip / dest_ip are already network byte order; add both
    // 16-bit halves (order is irrelevant to a ones-complement sum).
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dest_ip >> 16) & 0xFFFF;
    sum += dest_ip & 0xFFFF;
    // htons(IP_PROTO_TCP), IP_PROTO_TCP == 6 -> byte-swapped == 0x0600.
    sum += 0x0600u32;
    // htons(length): swap the two bytes of the 16-bit length (computed in u32
    // to avoid any intermediate overflow, result identical to the C htons()).
    let len_be = (((length as u32) & 0xFF) << 8) | (((length as u32) >> 8) & 0xFF);
    sum += len_be;

    let mut len: u16 = length;
    let mut off: isize = 0;
    // SAFETY: The caller guarantees `data` points to at least `length`
    // contiguous readable bytes (the exact contract net/tcp.c tcp_checksum_c
    // relies on). We never read past `length`: the loop consumes 2 bytes while
    // len > 1, and the final branch reads exactly 1 more byte only when len==1.
    // `data` is read-only and not retained.
    unsafe {
        while len > 1 {
            let lo = *data.offset(off) as u32;
            let hi = *data.offset(off + 1) as u32;
            sum += lo | (hi << 8); // native little-endian u16, == C *(uint16_t*)ptr
            off += 2;
            len -= 2;
        }
        if len > 0 {
            sum += *data.offset(off) as u32;
        }
    }

    while (sum >> 16) != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    (!sum) as u16
}

/// Rust port of net/udp.c udp_checksum_c(). Same pseudo-header fold as TCP but
/// with htons(IP_PROTO_UDP=17)=0x1100 and the RFC 768 special case: a computed
/// checksum of 0x0000 is transmitted as 0xFFFF (0x0000 on the wire means "no
/// checksum"). Byte-for-byte equivalent to udp_checksum_c. NOTE: this is a
/// staged/proven port; the live udp_send() path currently emits checksum 0
/// (IPv4-optional, unchanged behavior), so this symbol is proven by the boot
/// [RUST-DIFF] self-test but is NOT yet on the live transmit path. It routes
/// through udp_checksum() under -DRUST_UDP_CHECKSUM for a future opt-in.
#[no_mangle]
pub extern "C" fn udp_checksum_rs(src_ip: u32, dest_ip: u32, data: *const u8, length: u16) -> u16 {
    let mut sum: u32 = 0;

    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dest_ip >> 16) & 0xFFFF;
    sum += dest_ip & 0xFFFF;
    // htons(IP_PROTO_UDP), IP_PROTO_UDP == 17 -> byte-swapped == 0x1100.
    sum += 0x1100u32;
    let len_be = (((length as u32) & 0xFF) << 8) | (((length as u32) >> 8) & 0xFF);
    sum += len_be;

    let mut len: u16 = length;
    let mut off: isize = 0;
    // SAFETY: identical contract to tcp_checksum_rs above: `data` has at least
    // `length` readable bytes; we read exactly `length` bytes, read-only.
    unsafe {
        while len > 1 {
            let lo = *data.offset(off) as u32;
            let hi = *data.offset(off + 1) as u32;
            sum += lo | (hi << 8);
            off += 2;
            len -= 2;
        }
        if len > 0 {
            sum += *data.offset(off) as u32;
        }
    }

    while (sum >> 16) != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    let cs = (!sum) as u16;
    // RFC 768: a computed 0 is sent as 0xFFFF (0 means "no checksum").
    if cs == 0 { 0xFFFF } else { cs }
}
