// rustkern/netstat.rs - #745 OOBE Network page: a STRUCTURED live network
// status accessor, plus a NON-BLOCKING network probe (ICMP echo + DHCP
// restart).
//
// NEW kernel code, so Rust per the 2026-07-16 rule. This is NOT a port: there
// is no C twin, no `_c` reference and no -DRUST_NETSTAT strangler flag,
// because there was never any C here to strangle.
//
// WHY THIS EXISTS AT ALL (the design decision, recorded where it is enforced)
// --------------------------------------------------------------------------
// The only way Ring 3 could learn its own IPv4 configuration was
// SYS_NET_INFO (243) -> net_format_info(), which emits a VERBOSE HUMAN REPORT:
//
//     "  IPv4 Address:  192.0.2.1/24\n"
//     "  Netmask:       255.255.255.0\n"
//
// Recovering four addresses from that means an app has to string-match kernel
// PROSE. That fails in the worst possible direction: reword one label, change
// one column of padding, and the parse silently yields nothing while the app
// keeps rendering a page that looks fine and says nothing. There is no compile
// error, no link error and no runtime error - the exact "renders and does
// nothing" class this tree keeps re-shipping. A struct of integers cannot fail
// that way: the sizeof lock below and the C mirror's _Static_assert in
// proc/syscall.c both go RED at BUILD TIME if the layout ever drifts.
//
// SYS_GET_NET_INFO (146) already exists and is NOT sufficient, which is why
// this is a new accessor rather than a caller of that one:
//   * every field is a PRE-FORMATTED STRING (char[16]), so the consumer still
//     cannot compute anything (is the gateway on-link? is the mask sane?);
//   * it carries no link state, no DHCP state and no fault flag, so it cannot
//     tell "no address yet" from "address but no route" - the exact
//     distinction the wizard has to draw;
//   * it collapses "the DHCP server offered a resolver" and "the stack fell
//     back to its 8.8.8.8 default" into one `dns` string, so a machine with a
//     DHCP lease that offered no DNS looks identical to one that was handed a
//     resolver. Both are reported separately here (dns_dhcp vs dns_active).
// Widening 146's struct was rejected: it is a published layout with live
// consumers, and growing it is an ABI break for a cosmetic saving.
//
// BYTE ORDER, ONCE, HERE. Every u32 address below is HOST byte order, i.e.
// exactly what ip_get_address()/dhcp_get_dns() store, i.e. (a<<24)|(b<<16)|
// (c<<8)|d for a.b.c.d. sys_get_net_info() byte-swaps because it feeds
// ip_to_str(), which reads bytes in memory order; nothing here does, so
// nothing here swaps. Callers render with (v>>24, v>>16, v>>8, v) and cannot
// get it backwards.

// ===========================================================================
// Live status.
// ===========================================================================

/// Structured IPv4 status. C mirror: `net_status_t` in proc/syscall.h, whose
/// size is locked by a `_Static_assert` in proc/syscall.c. Userland mirror:
/// `net_status_t` in userland/libc/syscall.h and `NetStatus` in
/// apps/setup/main.rs. Four copies of one layout, all size-checked or
/// hand-checked against the assert below; do not add a field without updating
/// every one of them.
///
/// All address fields are HOST byte order. 0 means "not configured", which is
/// a REAL state the consumer must render honestly, not a blank.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct NetStatus {
    /// ip_get_address(): what the IP layer will actually source packets from.
    /// This is the truth for both DHCP and static; 0 = no address.
    pub ip: u32,
    /// ip_get_netmask().
    pub netmask: u32,
    /// ip_get_gateway(); 0 = no default route.
    pub gateway: u32,
    /// dns_get_server(): the resolver the stack will ACTUALLY query. May be
    /// the stack's compiled-in 8.8.8.8 fallback (dns_init) rather than
    /// anything DHCP said.
    pub dns_active: u32,
    /// dhcp_get_dns(): what the DHCP server offered, 0 = it offered none.
    /// Deliberately separate from dns_active so "your DHCP server did not
    /// give you a resolver" is visible instead of being papered over.
    pub dns_dhcp: u32,
    /// dhcp_get_ip(): the offered/leased address. Cleared to 0 by
    /// dhcp_discover() while a DORA is in flight, so it is NOT a substitute
    /// for `ip`; it is here to distinguish "lease held" from "static config".
    pub dhcp_ip: u32,
    /// nic_link_up() as 0/1. Carrier. Re-reads the PHY on the USB dongle.
    pub link_up: u32,
    /// dhcp_get_state(): DHCP_STATE_IDLE 0 / DISCOVERING 1 / REQUESTING 2 /
    /// BOUND 3 (net/dhcp.h).
    pub dhcp_state: u32,
    /// g_net_static_configured as 0/1: the config came from
    /// /CONFIG/NETIP.CFG rather than DHCP.
    pub config_static: u32,
    /// net_is_faulty() as 0/1: #549 persistently-unreachable interface.
    pub faulty: u32,
    /// net_get_driver_type(): NET_DRIVER_NONE 0 / E1000 / VIRTIO / USB. 0
    /// means there is no NIC at all, which is a different failure from "no
    /// carrier" and the consumer should say so.
    pub driver: u32,
    /// Popcount of `netmask`, precomputed so no caller re-derives it. Note it
    /// is a POPCOUNT, not a validated CIDR prefix: a non-contiguous mask
    /// would report a length that no /n describes. That matches what
    /// net_format_info() already prints, and a non-contiguous mask is a
    /// broken configuration either way.
    pub prefix_len: u32,
}
const _: () = assert!(core::mem::size_of::<NetStatus>() == 48);
const _: () = assert!(core::mem::align_of::<NetStatus>() == 4);

extern "C" {
    // net/ip.c - the live IP layer configuration (host order).
    fn ip_get_address() -> u32;
    fn ip_get_netmask() -> u32;
    fn ip_get_gateway() -> u32;
    // net/dns.c
    fn dns_get_server() -> u32;
    // net/dhcp.c
    fn dhcp_get_dns() -> u32;
    fn dhcp_get_ip() -> u32;
    fn dhcp_get_state() -> i32;
    fn dhcp_reset();
    // net/net.c
    fn nic_link_up() -> i32;
    fn net_is_faulty() -> i32;
    fn net_get_driver_type() -> i32;
    static g_net_static_configured: i32;
    // net/icmp.c - consumes the latched echo reply (host-order source IP).
    fn icmp_get_ping_reply(src_ip: *mut u32, seq: *mut u16, time_ms: *mut u16) -> i32;

    // proc/syscall.c - the three thin C shims that own the ADDRESS-SPACE and
    // INTERRUPT discipline (cli + net_cr3_enter/net_cr3_exit) around NIC
    // MMIO/DMA. Those stay C for a stated reason: they are irreducibly
    // entangled with paging and inline asm, they already existed as the
    // static icmp_ping_kcr3()/net_rx_drain_kcr3() helpers behind SYS_PING,
    // and duplicating a CR3 switch in Rust would give the rule TWO
    // definitions instead of one. Everything ABOVE that window - when to
    // transmit, when to give up, what counts as an answer - is here.
    fn net_probe_tx_c(dest_ip: u32) -> i32;
    fn net_probe_rx_c();
    fn net_dhcp_restart_c() -> i32;
}

/// Fill `out` with the live status. Returns 0, or -1 for a NULL `out`.
///
/// The caller (proc/syscall.c SYS_NET_STATUS) passes a KERNEL-LOCAL
/// net_status_t and does the single copy_to_user itself, so no user pointer
/// is ever dereferenced in here.
///
/// # Safety
/// `out` must be NULL or point to a writable, correctly-aligned net_status_t.
#[no_mangle]
pub unsafe extern "C" fn net_status_build_rs(out: *mut NetStatus) -> i32 {
    if out.is_null() {
        return -1;
    }
    let nm = ip_get_netmask();
    let s = NetStatus {
        ip: ip_get_address(),
        netmask: nm,
        gateway: ip_get_gateway(),
        dns_active: dns_get_server(),
        dns_dhcp: dhcp_get_dns(),
        dhcp_ip: dhcp_get_ip(),
        link_up: (nic_link_up() != 0) as u32,
        dhcp_state: dhcp_get_state() as u32,
        config_static: (g_net_static_configured != 0) as u32,
        faulty: (net_is_faulty() != 0) as u32,
        driver: net_get_driver_type() as u32,
        prefix_len: nm.count_ones(),
    };
    core::ptr::write(out, s);
    0
}

// ===========================================================================
// Non-blocking probe.
// ===========================================================================
//
// WHY NOT SYS_PING (66). sys_ping() BLOCKS the calling thread for up to
// timeout_ms, and it does so with two hand-rolled proc_sleep() poll loops
// (they are on the concurrency-lint allowlist as [DEBT], reviewed under
// #426 phase 2). Calling it from the setup wizard's event loop would freeze
// the wizard for up to a second per attempt on precisely the machine where
// the network is broken, which is the #211/#212 class of freeze this project
// treats as a defect.
//
// SO THE WAIT IS INVERTED. There is no wait in the kernel at all. START puts
// one echo request on the wire and returns immediately; each POLL drains the
// RX ring, answers "here is your RTT" or "still nothing", and returns
// immediately. Nothing sleeps, nothing spins, and there is no loop here for
// the concurrency lint to object to - because there is no loop.
//
// The DEADLINE therefore lives in the caller, which is correct: the caller is
// the one that knows how long a human will wait. The wizard polls from its
// existing 250ms win_get_event() timeout and gives up after its own bounded
// number of ticks. A remote host that never answers is exactly the case where
// a timeout is the right semantics rather than a hidden broken wake source
// (CLAUDE.md waiting rule, case 2): there is no wake to arm, because the peer
// may not exist.
//
// WHY POLLING IS SUFFICIENT HERE, and the trap it avoids: net_poll() is the
// ONLY receive path in this stack and e1000_irq_handler() has ZERO callers
// (see net/sntp.c's header comment). A bare wait on an ICMP reply would
// therefore never wake on a headless boot. POLL pumps the RX ring ITSELF via
// net_probe_rx_c(), the same "drive the engine yourself" shape sntp.c uses,
// so a reply is observed whether or not anything else in the system is
// pumping.

/// op codes for net_probe_rs. Mirrored as NET_PROBE_* in proc/syscall.h and
/// userland/libc/syscall.h.
const OP_PING_START: i32 = 0;
const OP_PING_POLL: i32 = 1;
const OP_PING_CANCEL: i32 = 2;
const OP_DHCP_RESTART: i32 = 3;

/// Return codes. A POLL that succeeds returns the RTT in ms, which is >= 0,
/// so every failure must be negative and distinguishable.
const PROBE_PENDING: i64 = -1; // no answer yet; poll again
const PROBE_EINVAL: i64 = -2; // bad op / zero destination
const PROBE_ENOTSTARTED: i64 = -3; // POLL with no START
const PROBE_ELINK: i64 = -4; // no carrier: nothing to probe

struct ProbeState {
    /// host-order destination of the probe in flight; 0 = idle.
    target: u32,
    /// 1 once an echo request actually reached ip_send. 0 means the first
    /// transmit was refused because ARP for the next hop was still pending -
    /// NOT a failure, and NOT a reason to give up (#333: the async fetcher
    /// must not give up on ARP-pending for a never-contacted LAN host; the
    /// same applies here, and a gateway is precisely a never-contacted LAN
    /// host on a freshly booted machine).
    sent: u32,
    /// latched round-trip time in ms once `have` is 1.
    rtt: u32,
    have: u32,
}

static mut PROBE: ProbeState = ProbeState { target: 0, sent: 0, rtt: 0, have: 0 };

/// Discard any latched echo reply so a stale one from an earlier probe can
/// never be mistaken for an answer to this one.
///
/// # Safety
/// Calls into net/icmp.c, which only touches its own file-static state.
unsafe fn drain_stale_reply() {
    let mut src: u32 = 0;
    let mut seq: u16 = 0;
    let mut ms: u16 = 0;
    icmp_get_ping_reply(&mut src, &mut seq, &mut ms);
}

/// Non-blocking network probe. See the op/return constants above.
///
/// `arg` is the host-order destination IPv4 for OP_PING_START and is ignored
/// otherwise. It arrives as a u64 from the syscall register, so it is range
/// checked before the narrowing cast rather than truncated silently.
///
/// # Safety
/// FFI entry point. Touches PROBE (single-threaded with respect to itself:
/// see the note below) and the net/icmp.c latch.
///
/// CONCURRENCY, STATED HONESTLY: PROBE is one global, so two processes
/// probing at once share it and the second START steals the first's target.
/// The underlying net/icmp.c latch (ping_pending / ping_reply_received) is a
/// single global too and has ALWAYS had that property, so this adds no new
/// sharing - SYS_PING has the same behaviour today. It is acceptable here
/// because the consumer is the first-boot wizard, which is modal and alone on
/// the machine. It is NOT a general-purpose ping API and must not be promoted
/// to one without per-process state.
#[no_mangle]
pub unsafe extern "C" fn net_probe_rs(op: i32, arg: u64) -> i64 {
    match op {
        OP_PING_START => {
            if arg == 0 || arg > 0xFFFF_FFFF {
                return PROBE_EINVAL;
            }
            if nic_link_up() == 0 {
                // No carrier is an INSTANT no-op (#381), never a wait.
                return PROBE_ELINK;
            }
            let dest = arg as u32;
            drain_stale_reply();
            PROBE.target = dest;
            PROBE.sent = 0;
            PROBE.rtt = 0;
            PROBE.have = 0;
            if net_probe_tx_c(dest) >= 0 {
                PROBE.sent = 1;
            }
            0
        }

        OP_PING_POLL => {
            if PROBE.target == 0 {
                return PROBE_ENOTSTARTED;
            }
            if PROBE.have != 0 {
                return PROBE.rtt as i64; // latched: repeat polls are idempotent
            }
            // (a) drive the RX engine ourselves, then (b) look, then (c)
            // retransmit. That ORDER matters: transmitting first would clear
            // net/icmp.c's ping_reply_received latch and throw away a reply
            // that had just landed.
            net_probe_rx_c();
            let mut src: u32 = 0;
            let mut seq: u16 = 0;
            let mut ms: u16 = 0;
            if icmp_get_ping_reply(&mut src, &mut seq, &mut ms) == 1 && src == PROBE.target {
                PROBE.rtt = ms as u32;
                PROBE.have = 1;
                return PROBE.rtt as i64;
            }
            // Retransmit on every poll while unanswered. Two reasons, neither
            // optional: the FIRST transmit is usually refused outright while
            // ARP for the gateway resolves, and a single lost echo request on
            // a lossy link would otherwise be reported as "gateway
            // unreachable" forever. The caller's poll cadence bounds the
            // packet rate (the wizard polls at 250ms), and its own deadline
            // bounds the total count.
            if net_probe_tx_c(PROBE.target) >= 0 {
                PROBE.sent = 1;
            }
            PROBE_PENDING
        }

        OP_PING_CANCEL => {
            PROBE.target = 0;
            PROBE.sent = 0;
            PROBE.rtt = 0;
            PROBE.have = 0;
            drain_stale_reply();
            0
        }

        OP_DHCP_RESTART => {
            if nic_link_up() == 0 {
                return PROBE_ELINK;
            }
            // dhcp_reset() alone would leave the state machine IDLE with no
            // DORA in flight; dhcp_discover() alone refuses outright when the
            // state is already BOUND. Both, in this order, are what makes a
            // retry actually retry (#431 carrier-edge path does the same).
            dhcp_reset();
            net_dhcp_restart_c() as i64
        }

        _ => PROBE_EINVAL,
    }
}
