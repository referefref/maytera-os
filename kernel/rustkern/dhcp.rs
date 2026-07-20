// rustkern/dhcp.rs - #404 Phase O / #497 incoming DHCP reply parse/validate
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ---------------------------------------------------------------------------
// Phase O port (#404 / #497): incoming DHCP reply parse/validate (option TLV walk).
//
// TIER 2 (untrusted wire input). buf/len are an attacker-spoofable DHCP
// OFFER/ACK/NAK UDP payload off the LAN. The seam is the PURE parse extracted out
// of net/dhcp.c dhcp_handle(): the BOOTP-header check (op==2 reply + magic
// cookie) + the option Type-Length-Value walk. The socket / state machine
// (xid match, DISCOVER/REQUEST send, DAD, bind) stays in C. net/dhcp.c routes
// the live parse here under -DRUST_DHCP (else dhcp_parse_c).
//
// UNLIKE the arp/dns ports (whose C references were already fully length-gated),
// the DHCP C reference has a REACHABLE over-read: its option walk runs over the
// FIXED 308-byte options[] array driven ONLY by the attacker's option LENGTH
// bytes, WITHOUT consulting `len` (dhcp_handle only gates the 240-byte header).
// So a runt / non-END-terminated / lying-length OFFER makes the C read PAST the
// received bytes (opt[i+1] at i==307 is one past options[]; a 4-byte option value
// is read after checking only olen>=4, not i+2+4<=308). Live it usually lands
// inside the >=1514-byte RX buffer (reading STALE / adjacent bytes and deriving
// config from non-packet memory) rather than faulting, but with a tight buffer
// the tail reads past the allocation (offline ASan proves it). This port CONFINES
// it: an immutable slice of exactly `len` bytes, the option walk bounded to
// [240, min(len, 548)) == the C's own fixed window intersected with the received
// bytes, and every read bounds-checked against `len` so a length running past the
// packet TERMINATES the walk instead of reading past it.
//
// CONFINEMENT ONLY, both directions (corrected 2026-07-16 by the #404 3-way drift
// audit; the b806 walk was NOT a pure confinement and this comment overstated it).
// The port must never read a byte the C did not read AND never honor an option the
// C ignored. Bounding to [240, len) alone violated the second half: the C's window
// is FIXED at 308 bytes regardless of len, so for len > 548 the old port honored
// options the C skipped entirely. See the walk body for both fixes.
//
// Field decode matches the C on x86-64: op/msg_type are single bytes; the magic
// cookie is compared as the 4 wire bytes 63 82 53 63; xid/yiaddr and the 4-byte
// option values are assembled big-endian == ntohl exactly as the C produced.

// Parse return codes (mirror net/dhcp.h).
const DHCP_PARSE_OK: i32 = 0;
const DHCP_PARSE_ETOOSHORT: i32 = -1;
const DHCP_PARSE_ENOTREPLY: i32 = -2;
const DHCP_PARSE_EBADMAGIC: i32 = -3;

const DHCP_HDR_LEN: usize = 240;      // sizeof(dhcp_packet_t) - 308 (BOOTP + magic)
const DHCP_OPTIONS_OFF: usize = 240;  // options[] start offset
const DHCP_MAGIC_OFF: usize = 236;    // magic cookie offset

// DHCP option codes (mirror net/dhcp.h).
const DHCP_OPT_PAD: u8 = 0;
const DHCP_OPT_SUBNET_MASK: u8 = 1;
const DHCP_OPT_ROUTER: u8 = 3;
const DHCP_OPT_DNS: u8 = 6;
const DHCP_OPT_LEASE_TIME: u8 = 51;
const DHCP_OPT_MSG_TYPE: u8 = 53;
const DHCP_OPT_SERVER_ID: u8 = 54;
const DHCP_OPT_END: u8 = 255;

// #[repr(C)] mirror of dhcp_parsed_t (net/dhcp.h). Layout is asserted == 36 bytes
// on the C side (_Static_assert in dhcp.c) so this can never silently drift.
// Field order + primitive types match the C struct exactly; on x86-64 the two
// layouts are identical (7 u32 at 0..28, 6 u8 at 28..34, 2 pad; size 36, align 4).
#[repr(C)]
pub struct DhcpParsed {
    pub xid: u32,             // host order
    pub yiaddr: u32,          // host order
    pub subnet: u32,          // option 1  (host order)
    pub router: u32,          // option 3  (host order)
    pub dns: u32,             // option 6  (host order)
    pub lease: u32,           // option 51 (host order)
    pub server_id: u32,       // option 54 (host order)
    pub msg_type: u8,         // option 53 (0 if absent)
    pub have_subnet: u8,
    pub have_router: u8,
    pub have_dns: u8,
    pub have_lease: u8,
    pub have_server_id: u8,
    pub _pad: [u8; 2],
}

/// Rust port of net/dhcp.c dhcp_parse_c(): PURE parse/validate of an incoming
/// DHCP reply. Returns DHCP_PARSE_OK (0) for a structurally valid reply (op==2 +
/// magic cookie; then *out is filled), DHCP_PARSE_ETOOSHORT (-1) for len < 240,
/// DHCP_PARSE_ENOTREPLY (-2) for op != 2, or DHCP_PARSE_EBADMAGIC (-3) for a bad
/// magic cookie. Never sends, allocates, touches a global, or mutates the input.
///
/// The option walk is a PURE CONFINEMENT of dhcp_parse_c: it walks the C's own
/// FIXED window [240, 548) intersected with the received bytes, i.e.
/// [240, min(len, 548)), and gates every read on the bytes ACTUALLY read against
/// `len`. Where the C would run past the received bytes into stale/adjacent
/// memory (a runt or non-END-terminated reply) this walk stops: that is the
/// reachable over-read the verbatim C never confined, and the only intended
/// behavior difference. An option with a lying/oversized declared length whose
/// value bytes ARE present is still extracted, exactly as the C does; the walk is
/// never aborted early on account of the declared length.
///
/// net/dhcp.c routes the live parse here under -DRUST_DHCP.
#[no_mangle]
pub extern "C" fn dhcp_parse_rs(buf: *const u8, len: u32, out: *mut DhcpParsed) -> i32 {
    let n = len as usize;

    // Header gate FIRST, before any dereference (matches the C
    // `if (length < sizeof(dhcp_packet_t) - 308)` == len < 240).
    if n < DHCP_HDR_LEN {
        return DHCP_PARSE_ETOOSHORT;
    }

    // SAFETY: the caller (dhcp_handle, via net/dhcp.c) guarantees `buf` points to
    // at least `len` contiguous, readable bytes - the DHCP UDP payload udp.c
    // handed down. `n` is proven >= 240 above, so the slice spans a valid,
    // in-bounds extent and EVERY access below (the fixed header fields at offsets
    // <240 + the option walk over [240, n)) indexes only through it and is
    // therefore bounds-checked by Rust: no read can ever leave the slice. The
    // buffer is used read-only and is not retained past this call.
    let pkt: &[u8] = unsafe { core::slice::from_raw_parts(buf, n) };

    // op == 2 (BOOTP reply); matches the C `if (pkt->op != 2) return;`.
    if pkt[0] != 2 {
        return DHCP_PARSE_ENOTREPLY;
    }
    // Magic cookie in WIRE (network) order at offset 236: 63 82 53 63. Comparing
    // the 4 wire bytes is exactly the C `pkt->magic != htonl(DHCP_MAGIC)` test.
    if pkt[DHCP_MAGIC_OFF] != 0x63
        || pkt[DHCP_MAGIC_OFF + 1] != 0x82
        || pkt[DHCP_MAGIC_OFF + 2] != 0x53
        || pkt[DHCP_MAGIC_OFF + 3] != 0x63
    {
        return DHCP_PARSE_EBADMAGIC;
    }

    // Fixed-offset header fields, all in-bounds (n >= 240). Big-endian == ntohl.
    let xid = u32::from_be_bytes([pkt[4], pkt[5], pkt[6], pkt[7]]);
    let yiaddr = u32::from_be_bytes([pkt[16], pkt[17], pkt[18], pkt[19]]);

    let mut msg_type: u8 = 0;
    let mut subnet: u32 = 0;
    let mut router: u32 = 0;
    let mut dns: u32 = 0;
    let mut lease: u32 = 0;
    let mut server_id: u32 = 0;
    let mut have_subnet: u8 = 0;
    let mut have_router: u8 = 0;
    let mut have_dns: u8 = 0;
    let mut have_lease: u8 = 0;
    let mut have_server_id: u8 = 0;

    // Option TLV walk. This is a PURE CONFINEMENT of the C: it visits exactly the
    // window the C visits, INTERSECTED with the bytes actually received, and it
    // never reads a byte the C did not read nor honors an option the C ignored.
    //
    // DRIFT FIX (#404 3-way drift audit, 2026-07-16). The shipped b806 walk was
    // NOT a pure confinement and diverged from the C on the LIVE path in two
    // undocumented ways (264,738 non-excusable divergences per 1M vectors). Both
    // hid from the 2-way rs-vs-c differential because the boot self-test builder
    // only emits ~250-290 byte packets and both need len > 548:
    //
    //  (A) WALK-WINDOW WIDENING (the serious one; it read MORE, not less, which
    //      is the OPPOSITE of a confinement despite the comment calling it one).
    //      The C walks the FIXED 308-byte options[] field, `while (i < 308 ...)`,
    //      i.e. absolute [240, 548), REGARDLESS of len. The old Rust walked
    //      [240, n), so for len > 548 it HONORED options the C ignores entirely.
    //      PoC: a 600-byte OFFER with 308 PADs then msg_type + router/dns/
    //      server_id past offset 548 -> C ignores the packet (msg_type=0), old
    //      Rust adopted gateway/dns/server_id from it. Fix: bound the walk to
    //      min(n, 240+308).
    //  (B) LYING-LENGTH STRICTNESS. The C checks only `olen >= 4`, extracts the
    //      value, then advances `i += 2 + olen`. The old Rust did
    //      `if vstart + olen > n { break }`, which REJECTED the option and ABORTED
    //      the whole walk even when the 4 bytes it actually reads were present in
    //      the packet. That is a stricter parse, not a confinement. Fix: gate each
    //      read on the bytes ACTUALLY READ against `n` (`vstart + 4 <= n`), and
    //      never break early; advance by 2+olen exactly as the C does and let the
    //      window bound terminate the walk, as the C's `i < 308` does.
    //
    // What REMAINS an intended confinement (advisory 0002, unchanged): where the
    // C runs off the received bytes into stale/adjacent memory (a runt or
    // non-END-terminated reply), this walk stops at `n`. Those cases are
    // determined by bytes that were never received, so no correct parse can
    // depend on them.
    let win = DHCP_OPTIONS_OFF + 308;
    let end = if n < win { n } else { win };
    let mut i = DHCP_OPTIONS_OFF;
    while i < end {
        let t = pkt[i];
        if t == DHCP_OPT_PAD {
            i += 1;
            continue;
        }
        if t == DHCP_OPT_END {
            break;
        }
        // Length byte: the C reads opt[i+1] with only `i < 308` proven. Gate it on
        // the PACKET bound so we never read a byte that was not received.
        if i + 1 >= n {
            break;
        }
        let olen = pkt[i + 1] as usize;
        let vstart = i + 2;
        match t {
            DHCP_OPT_MSG_TYPE => {
                if olen >= 1 && vstart < n {
                    msg_type = pkt[vstart];
                }
            }
            DHCP_OPT_SUBNET_MASK => {
                if olen >= 4 && vstart + 4 <= n {
                    subnet = u32::from_be_bytes([pkt[vstart], pkt[vstart + 1], pkt[vstart + 2], pkt[vstart + 3]]);
                    have_subnet = 1;
                }
            }
            DHCP_OPT_ROUTER => {
                if olen >= 4 && vstart + 4 <= n {
                    router = u32::from_be_bytes([pkt[vstart], pkt[vstart + 1], pkt[vstart + 2], pkt[vstart + 3]]);
                    have_router = 1;
                }
            }
            DHCP_OPT_DNS => {
                if olen >= 4 && vstart + 4 <= n {
                    dns = u32::from_be_bytes([pkt[vstart], pkt[vstart + 1], pkt[vstart + 2], pkt[vstart + 3]]);
                    have_dns = 1;
                }
            }
            DHCP_OPT_LEASE_TIME => {
                if olen >= 4 && vstart + 4 <= n {
                    lease = u32::from_be_bytes([pkt[vstart], pkt[vstart + 1], pkt[vstart + 2], pkt[vstart + 3]]);
                    have_lease = 1;
                }
            }
            DHCP_OPT_SERVER_ID => {
                if olen >= 4 && vstart + 4 <= n {
                    server_id = u32::from_be_bytes([pkt[vstart], pkt[vstart + 1], pkt[vstart + 2], pkt[vstart + 3]]);
                    have_server_id = 1;
                }
            }
            _ => {}
        }
        i += 2 + olen;
    }

    if !out.is_null() {
        // SAFETY: `out` is either null (checked) or a valid, writable DhcpParsed*
        // provided by the C caller (same contract as the C reference). POD writes.
        unsafe {
            let o = &mut *out;
            o.xid = xid;
            o.yiaddr = yiaddr;
            o.subnet = subnet;
            o.router = router;
            o.dns = dns;
            o.lease = lease;
            o.server_id = server_id;
            o.msg_type = msg_type;
            o.have_subnet = have_subnet;
            o.have_router = have_router;
            o.have_dns = have_dns;
            o.have_lease = have_lease;
            o.have_server_id = have_server_id;
            o._pad = [0, 0];
        }
    }
    DHCP_PARSE_OK
}
