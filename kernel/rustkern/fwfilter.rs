// rustkern/fwfilter.rs - #238: THE packet filter. The whole decision engine,
// the rule store, the connection tracker, the config codec and the counters.
//
// NEW kernel code, so Rust per the 2026-07-16 rule. This is NOT a port: there
// is no C twin, no `_c` reference and no -DRUST_FWFILTER strangler flag,
// because THERE WAS NEVER ANY FIREWALL. `grep -rln firewall kernel/net`
// returned nothing. Settings has shipped an "Enable Firewall" toggle, two
// default policies and an iptables-style rule editor over a capability the
// kernel did not possess, persisting all of it perfectly to a file no code
// ever read. This module is that capability.
//
// WHY RUST AND NOT C, ON A HOT PATH. The measured cost is in fw_stats_t
// (cyc_tot/cyc_max, reported by `fw stats` on the serial shell and by
// SYS_NET_FW). It is a bounded rule walk (<= 12 comparisons) plus a 4-way
// conntrack probe, with no allocation, no loop over packet data, and no call
// out. There is no performance argument for C here and none is claimed. The
// one genuinely performance-shaped decision - not doing a linear scan of the
// conntrack table per packet - is addressed by the hash below, in either
// language.
//
// =========================================================================
// THE THREE RULES THIS FILE OBEYS, EACH FOR A RECORDED REASON
// =========================================================================
//
// 1. IT NEVER BLOCKS, NEVER ALLOCATES, NEVER CALLS OUT.
//    fw_filter_rs() runs inside eth_receive()'s RX drain, which executes
//    under net_lock with INTERRUPTS OFF, and inside ip_send(), which #549
//    records can be reached from a pre-scheduler / no-block context where
//    wait_event() DEADLOCKS. So this file contains no wait, no sleep, no
//    lock, no kmalloc and no I/O. Every structure below is statically sized.
//    wq_assert_may_block() cannot fire here because nothing here can reach
//    __wait_prepare(). Persistence (writing /CONFIG/FWRULES.CFG) is done by
//    the SYSCALL side in C, in process context, never from here.
//
// 2. IT FAILS CLOSED, AND "CLOSED" IS DEFINED AT COMPILE TIME.
//    DEFAULT_CFG below is the policy in force from the first packet of boot,
//    before any file is read and before userland exists. A config that is
//    absent, unreadable or unusable leaves DEFAULT_CFG in force - never "no
//    filtering". An install that fails validation changes NOTHING: the
//    previous policy stays. There is no code path that turns the filter off
//    by accident; only an explicit `enabled = 0` does that.
//
// 3. THE STATE IS SINGLE-DESCRIPTION.
//    The kernel owns the policy AND the file. Settings does not parse
//    FWRULES.CFG and does not write it; it reads and writes the kernel
//    through SYS_NET_FW, and the kernel serialises. There is exactly one
//    parser (cfg_parse below), one serialiser (cfg_format below) and one
//    authoritative copy of the rules. The previous arrangement - a userland
//    app writing a format with no reader - is precisely the fault #238
//    exists to remove, and adding a second parser in userland would rebuild
//    it one level down.
//
// =========================================================================
// THE MODEL, AND EXACTLY WHAT IS AND IS NOT FILTERED
// =========================================================================
//
// The rule model is the one the Settings UI already offered, unchanged, so
// there is one description and not two:
//
//     enabled              on / off
//     default inbound      ALLOW / DENY
//     default outbound     ALLOW / DENY
//     up to 12 rules       (direction, action, protocol, port)
//     direction            IN / OUT
//     action               ALLOW / DENY
//     protocol             TCP / UDP
//     port                 1..65535
//
// First match wins, in file order; if no rule matches, the default policy for
// that direction applies. That is the iptables-shaped semantics the UI claims.
//
// PORT SEMANTICS, stated once. A rule's port is matched against the
// DESTINATION port of the packet that OPENS a flow. So `allow in tcp 22` means
// "permit connections to my port 22" and `allow out tcp 443` means "permit me
// to connect to a remote port 443". This is what the UI's editor means and
// what a reader expects; it is written down because the alternative reading
// (match either port) silently permits far more.
//
// STATEFUL, BECAUSE THE STATELESS VERSION WOULD TAKE THE MACHINE OFFLINE.
// The UI's own default is inbound DENY. Applied to EVERY inbound packet, that
// drops every DNS response, every SYN-ACK for a connection we opened, and
// every HTTP reply - i.e. it would break all networking while looking like a
// working firewall. So the filter tracks flows: a packet belonging to a flow
// the filter has ALREADY ALLOWED passes without re-evaluation, and only
// FLOW-OPENING packets are evaluated. This is the standard NEW/ESTABLISHED
// model and it is the only design under which "default inbound: Deny" means
// what a person means by it: block unsolicited incoming connections.
//
// NOT FILTERED, DELIBERATELY, AND THE UI SAYS SO:
//
//   * ARP. It sits below IP and the hooks are in ip_send()/ip_handle(). If
//     ARP were filtered, a wrong rule would black-hole the machine's own
//     gateway resolution, and #380 (a dongle disrupting a real LAN through
//     bad gratuitous ARP) is a standing warning against touching that path.
//   * ICMP, and every IP protocol that is not TCP or UDP. The rule model has
//     no way to express them - it offers TCP and UDP only - so they are
//     passed and COUNTED (fw_stats_t.unfiltered). A filter that silently
//     dropped what its UI cannot describe would be worse than one that says
//     so. Ping therefore still works with inbound DENY, and that is a stated
//     limitation, not an oversight.
//   * The DHCP client exchange (UDP 68 <-> 67, either direction). Built in,
//     because the alternative is a machine that cannot obtain an address and
//     therefore cannot reach the UI that would fix it. Counted as `exempt`.
//     The residual exposure is narrow and stated: a LAN peer may send us UDP
//     from port 67 to port 68 regardless of policy, reaching only the DHCP
//     client parser (which is itself the Rust dhcp_parse_rs, #497).
//   * Loopback (127.0.0.0/8).
//
// DROPPED, DELIBERATELY:
//
//   * Non-first IP fragments, when the filter is enabled. A fragment past the
//     first carries no L4 header, so its ports CANNOT be known and it cannot
//     be classified; passing it is a documented firewall-evasion primitive.
//     This costs nothing in practice because this stack does not reassemble:
//     ip_handle() hands the fragment straight to a protocol handler that will
//     read the wrong bytes as a header. Counted as `frag`.
//   * A TCP or UDP packet too short to contain the header the filter must
//     read. Counted as `malformed`.
//
// =========================================================================
// CONCURRENCY, STATED HONESTLY
// =========================================================================
//
// The CONFIG is written from process context (the SYS_NET_FW handler and the
// boot-time load) and read on the packet path. Those are genuinely different
// contexts, so the config is DOUBLE-BUFFERED and published by one atomic
// store of an index. A reader takes the index once and reads a buffer nobody
// is writing. No lock, no torn policy, and an install is atomic: a packet
// sees the whole old ruleset or the whole new one.
//
// The CONNTRACK TABLE and the COUNTERS are packet-path-only state and are
// plain `static mut`. They rely on exactly the same serialisation ip_send()
// already relies on for its `static uint8_t packet[IP_MTU]` staging buffer -
// net_lock. That is not a new assumption introduced here; if it were false,
// ip_send() would already be corrupting outbound packets. The one cross-
// context write is the conntrack GENERATION, which an install bumps to flush
// the table in O(1); it is an atomic.
//
// FLUSH-ON-INSTALL IS PART OF THE SEMANTICS, not an implementation detail. A
// policy change re-evaluates everything: if you deny a port, flows already
// allowed through it stop at their next packet rather than surviving until
// eviction. It also makes the behaviour testable, because a deny takes effect
// immediately and visibly.
//
// NO TIMER IS USED ANYWHERE IN THIS FILE. Conntrack entries do not expire on
// a clock; they are reclaimed least-recently-used when their hash group is
// full, and TCP entries are removed on FIN/RST. This is deliberate:
// timer_ticks is NOT a wall clock (KVM replays lost ticks in BURSTS), so
// every `timer_ticks + N` deadline in this tree can fire instantly under vCPU
// starvation. An idle-timeout conntrack would therefore drop established
// connections in bursts under load, which is the hardest possible bug to
// attribute. A use-ordered table has no such failure mode.

use core::sync::atomic::{AtomicU32, Ordering};

// ===========================================================================
// The wire/ABI structures. C mirrors in kernel/proc/syscall.h and
// userland/libc/syscall.h, both locked by _Static_assert on sizeof.
// ===========================================================================

/// Maximum explicit rules. Matches MAX_FW_RULES in the Settings editor.
pub const FW_MAX_RULES: usize = 12;
/// Bumped only on an incompatible layout change; an install carrying any
/// other value is refused rather than reinterpreted.
pub const FW_CFG_VERSION: u32 = 1;

pub const FW_DIR_IN: u32 = 0;
pub const FW_DIR_OUT: u32 = 1;

const ACT_ALLOW: u8 = 0;
const ACT_DENY: u8 = 1;

// Rule protocol selector (the UI's `proto` field), NOT an IP protocol number.
const RP_TCP: u8 = 0;
const RP_UDP: u8 = 1;

// IP protocol numbers.
const IPP_TCP: u8 = 6;
const IPP_UDP: u8 = 17;

const TCP_FIN: u8 = 0x01;
const TCP_RST: u8 = 0x04;

/// One rule. 8 bytes; the two reserved fields exist so the struct has no
/// implicit padding in any of its four copies and so a future field does not
/// change sizeof.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FwRule {
    pub dir: u8,
    pub action: u8,
    pub proto: u8,
    pub reserved: u8,
    pub port: u16,
    pub reserved2: u16,
}

/// The whole policy. 104 bytes.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FwConfig {
    pub version: u32,
    pub enabled: u8,
    pub pol_in: u8,
    pub pol_out: u8,
    pub rule_count: u8,
    pub rules: [FwRule; FW_MAX_RULES],
}

/// Counters. 136 bytes. Every one of these is a MEASUREMENT, not a debug aid:
/// `drop_in`/`drop_out` are the proof the filter is doing anything at all, and
/// cyc_tot/cyc_max are the cost this ticket is required to report.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FwStats {
    /// Total fw_filter_rs() invocations (including while disabled).
    pub calls: u64,
    /// Packets that matched a tracked flow and skipped rule evaluation.
    pub ct_hit: u64,
    /// Flow-opening packets evaluated against the rules, per direction.
    pub new_in: u64,
    pub new_out: u64,
    /// Evaluated and allowed.
    pub pass_in: u64,
    pub pass_out: u64,
    /// Evaluated and DROPPED. This is the number that proves the filter works.
    pub drop_in: u64,
    pub drop_out: u64,
    /// Built-in exemptions taken (DHCP client, loopback).
    pub exempt: u64,
    /// Passed because the rule model cannot express the protocol (ICMP, ...).
    pub unfiltered: u64,
    /// Dropped: L4 header too short to classify.
    pub malformed: u64,
    /// Dropped: non-first IP fragment, unclassifiable.
    pub frag: u64,
    /// Conntrack entries reclaimed because their hash group was full.
    pub ct_evict: u64,
    /// Total and worst-case TSC cycles spent inside fw_filter_rs().
    pub cyc_tot: u64,
    pub cyc_max: u64,
    /// Live conntrack entries at the moment of the read.
    pub ct_used: u32,
    pub enabled: u32,
    pub rule_count: u32,
    /// pol_in | (pol_out << 8), so a reader needs one field for both.
    pub pol: u32,
}

/// The single transfer struct for SYS_NET_FW. ONE size for every op, so the
/// argtab.rs descriptor is one fixed-size write and there is no op-dependent
/// pointer contract for the user-pointer validator to get wrong.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FwXfer {
    pub cfg: FwConfig,
    pub stats: FwStats,
}

// ===========================================================================
// The compiled-in fail-safe policy.
//
// THIS IS THE ANSWER TO "what happens when the config is absent". It is in
// force from the first packet of boot and it is what an unreadable, absent or
// unusable /CONFIG/FWRULES.CFG leaves in place. It is deliberately IDENTICAL
// to the first-run defaults the Settings editor used to seed, so a virgin
// machine and a configured-with-defaults machine behave the same and there is
// no second description of "default".
//
//   inbound  DENY  (unsolicited only - replies to our own traffic still pass)
//   outbound ALLOW
//   NO inbound allowance at all - see below.
//   allow out tcp 80 / 443, allow out udp 53 - so an outbound DENY policy set
//                         later still leaves a working browser and resolver.
//
// THE `allow in tcp 22` RULE IS GONE (2026-08-23, #<config-ref>). It was here "because
// sshd is the only inbound listener the OS ships", and the sentence right
// underneath it stated the correct rule for its own removal: a default rule
// opening a port with no service behind it is a description of something that
// does not exist. That was written to justify NOT adding a rule for port 2323
// after #566 deleted the RC-2323 shell, and it applied word for word to port
// 22 the whole time it sat above this list. The sshd is compiled in only under
// MAYTERA_SSHD=1 (build/build-golden.sh), which defaults to 0, so no shipped
// golden has ever contained the server this rule was holding a port open for.
// A default install therefore lost nothing when this went: there was no remote
// debug path to lose, only an open port.
//
// WHEN THE SERVER COMES BACK, THIS RULE COMES BACK WITH IT, and the two moves
// belong in one commit. The condition is MAYTERA_SSHD=1 plus /CONFIG/SSHD.CFG
// enable=1 (see build/assets/SSHD.CFG, which spells out both halves); the
// inbound policy is DENY, so until then port 22 is closed by policy rather
// than open by description.
// ===========================================================================

const fn rule(dir: u8, action: u8, proto: u8, port: u16) -> FwRule {
    FwRule { dir, action, proto, reserved: 0, port, reserved2: 0 }
}
const NORULE: FwRule = rule(0, 0, 0, 0);

const DEFAULT_CFG: FwConfig = FwConfig {
    version: FW_CFG_VERSION,
    enabled: 1,
    pol_in: ACT_DENY,
    pol_out: ACT_ALLOW,
    rule_count: 3,
    rules: [
        rule(FW_DIR_OUT as u8, ACT_ALLOW, RP_TCP, 80),
        rule(FW_DIR_OUT as u8, ACT_ALLOW, RP_TCP, 443),
        rule(FW_DIR_OUT as u8, ACT_ALLOW, RP_UDP, 53),
        NORULE, NORULE, NORULE, NORULE, NORULE, NORULE, NORULE, NORULE, NORULE,
    ],
};

// ===========================================================================
// Published policy: double-buffered, one atomic index.
// ===========================================================================

static mut CFG: [FwConfig; 2] = [DEFAULT_CFG, DEFAULT_CFG];
static CFG_ACTIVE: AtomicU32 = AtomicU32::new(0);

#[inline(always)]
fn cfg_read() -> &'static FwConfig {
    let i = CFG_ACTIVE.load(Ordering::Acquire) as usize & 1;
    unsafe { &*(&raw const CFG[i]) }
}

// ===========================================================================
// Connection tracking.
//
// A 4-way set-associative table of 256 slots. Lookup probes exactly 4 slots,
// so the per-packet cost is a constant regardless of how many flows are live.
// Direct-mapped (1-way) was rejected: with only 10 live flows the chance that
// some pair collides is ~16%, and a collision silently evicts an ESTABLISHED
// flow, which under inbound DENY kills a working connection. 4-way makes that
// require five simultaneous flows in one group.
// ===========================================================================

const CT_SLOTS: usize = 256;
const CT_WAYS: usize = 4;

#[derive(Clone, Copy)]
struct Ct {
    peer: u32,
    lport: u16,
    rport: u16,
    proto: u8,
    valid: u8,
    gen: u16,
    lru: u32,
}
const CT_EMPTY: Ct = Ct { peer: 0, lport: 0, rport: 0, proto: 0, valid: 0, gen: 0, lru: 0 };

static mut CT: [Ct; CT_SLOTS] = [CT_EMPTY; CT_SLOTS];
static mut CT_CLOCK: u32 = 0;
/// Bumped by an install; every entry from an older generation is invisible.
/// This is the O(1) flush - no memset on a policy change, and nothing on the
/// packet path has to walk the table.
static CT_GEN: AtomicU32 = AtomicU32::new(1);

#[inline(always)]
fn ct_group(peer: u32, lport: u16, rport: u16, proto: u8) -> usize {
    let mut h = peer.wrapping_mul(0x9E37_79B1);
    h ^= ((lport as u32) << 16) | (rport as u32);
    h = h.wrapping_mul(0x85EB_CA6B);
    h ^= h >> 15;
    h = h.wrapping_add(proto as u32).wrapping_mul(0xC2B2_AE35);
    h ^= h >> 13;
    ((h as usize) % CT_SLOTS) & !(CT_WAYS - 1)
}

/// Returns the slot index of a live entry for this flow, or None.
#[inline(always)]
unsafe fn ct_find(peer: u32, lport: u16, rport: u16, proto: u8) -> Option<usize> {
    let gen = CT_GEN.load(Ordering::Relaxed) as u16;
    let base = ct_group(peer, lport, rport, proto);
    let t = &*(&raw const CT);
    for w in 0..CT_WAYS {
        let e = &t[base + w];
        if e.valid != 0 && e.gen == gen && e.peer == peer && e.lport == lport
            && e.rport == rport && e.proto == proto
        {
            return Some(base + w);
        }
    }
    None
}

#[inline(always)]
unsafe fn ct_touch(i: usize) {
    CT_CLOCK = CT_CLOCK.wrapping_add(1);
    (*(&raw mut CT))[i].lru = CT_CLOCK;
}

unsafe fn ct_insert(peer: u32, lport: u16, rport: u16, proto: u8) {
    let gen = CT_GEN.load(Ordering::Relaxed) as u16;
    let base = ct_group(peer, lport, rport, proto);
    let t = &mut *(&raw mut CT);
    // Prefer a free or stale-generation slot; otherwise reclaim the
    // least-recently-used member of the group.
    let mut victim = base;
    let mut best = u32::MAX;
    let mut found_free = false;
    for w in 0..CT_WAYS {
        let e = &t[base + w];
        if e.valid == 0 || e.gen != gen {
            victim = base + w;
            found_free = true;
            break;
        }
        if e.lru <= best {
            best = e.lru;
            victim = base + w;
        }
    }
    if !found_free {
        STATS.ct_evict += 1;
    }
    CT_CLOCK = CT_CLOCK.wrapping_add(1);
    t[victim] = Ct { peer, lport, rport, proto, valid: 1, gen, lru: CT_CLOCK };
}

#[inline(always)]
unsafe fn ct_drop(i: usize) {
    (*(&raw mut CT))[i].valid = 0;
}

unsafe fn ct_live() -> u32 {
    let gen = CT_GEN.load(Ordering::Relaxed) as u16;
    let t = &*(&raw const CT);
    let mut n = 0u32;
    for e in t.iter() {
        if e.valid != 0 && e.gen == gen {
            n += 1;
        }
    }
    n
}

// ===========================================================================
// Counters.
// ===========================================================================

const STATS_ZERO: FwStats = FwStats {
    calls: 0, ct_hit: 0, new_in: 0, new_out: 0, pass_in: 0, pass_out: 0,
    drop_in: 0, drop_out: 0, exempt: 0, unfiltered: 0, malformed: 0, frag: 0,
    ct_evict: 0, cyc_tot: 0, cyc_max: 0,
    ct_used: 0, enabled: 0, rule_count: 0, pol: 0,
};
static mut STATS: FwStats = STATS_ZERO;

// ===========================================================================
// The decision.
// ===========================================================================

const PASS: i32 = 1;
const DROP: i32 = 0;

#[inline(always)]
unsafe fn be16(p: *const u8, off: usize) -> u16 {
    ((*p.add(off) as u16) << 8) | (*p.add(off + 1) as u16)
}

/// The whole filter. `dir` is FW_DIR_IN/FW_DIR_OUT, `peer_ip` is the REMOTE
/// address in HOST byte order (source for inbound, destination for outbound),
/// `frag_off` is the IPv4 fragment offset field in 8-byte units (0 for a
/// first or only fragment; always 0 outbound, this stack sets DF), and
/// `payload`/`len` are the L4 header and body.
///
/// Returns 1 to pass the packet, 0 to drop it.
unsafe fn fw_decide(
    dir: u32,
    peer_ip: u32,
    proto: u8,
    frag_off: u16,
    payload: *const u8,
    len: u16,
) -> i32 {
    let cfg = cfg_read();

    if cfg.enabled == 0 {
        return PASS;
    }

    // Loopback is never filtered: a rule cannot usefully apply to ourselves
    // and dropping it would break local services for no security gain.
    if (peer_ip >> 24) == 127 {
        STATS.exempt += 1;
        return PASS;
    }

    // The rule model describes TCP and UDP only. Anything else is passed and
    // counted, NOT silently dropped: see the header block.
    if proto != IPP_TCP && proto != IPP_UDP {
        STATS.unfiltered += 1;
        return PASS;
    }

    // A fragment past the first has no L4 header, so it cannot be classified.
    if (frag_off & 0x1FFF) != 0 {
        STATS.frag += 1;
        return DROP;
    }

    // We must be able to read the header we are about to classify on. For TCP
    // that includes the flags byte at offset 13.
    let need: usize = if proto == IPP_TCP { 14 } else { 8 };
    if payload.is_null() || (len as usize) < need {
        STATS.malformed += 1;
        return DROP;
    }

    let sport = be16(payload, 0);
    let dport = be16(payload, 2);
    let (lport, rport) = if dir == FW_DIR_IN { (dport, sport) } else { (sport, dport) };

    // DHCP client exchange, both directions. Built in; see the header block.
    if proto == IPP_UDP
        && ((lport == 68 && rport == 67) || (lport == 67 && rport == 68))
    {
        STATS.exempt += 1;
        return PASS;
    }

    // ESTABLISHED: a flow this filter has already allowed.
    if let Some(i) = ct_find(peer_ip, lport, rport, proto) {
        STATS.ct_hit += 1;
        ct_touch(i);
        if proto == IPP_TCP {
            let flags = *payload.add(13);
            if (flags & (TCP_FIN | TCP_RST)) != 0 {
                ct_drop(i);
            }
        }
        return PASS;
    }

    // NEW. The rule port is the DESTINATION port of the opening packet, in
    // both directions - see PORT SEMANTICS in the header block.
    let rp = if proto == IPP_TCP { RP_TCP } else { RP_UDP };
    let mut action = if dir == FW_DIR_IN { cfg.pol_in } else { cfg.pol_out };
    let n = cfg.rule_count as usize;
    let n = if n > FW_MAX_RULES { FW_MAX_RULES } else { n };
    for i in 0..n {
        let r = &cfg.rules[i];
        if r.dir as u32 == dir && r.proto == rp && r.port == dport {
            action = r.action;
            break; // first match wins
        }
    }

    if dir == FW_DIR_IN {
        STATS.new_in += 1;
    } else {
        STATS.new_out += 1;
    }

    if action == ACT_DENY {
        if dir == FW_DIR_IN { STATS.drop_in += 1; } else { STATS.drop_out += 1; }
        return DROP;
    }

    ct_insert(peer_ip, lport, rport, proto);
    if dir == FW_DIR_IN { STATS.pass_in += 1; } else { STATS.pass_out += 1; }
    PASS
}

/// C entry point. Wraps the decision in a TSC measurement so the cost of the
/// filter is a number this ticket can report rather than an assertion.
#[no_mangle]
pub unsafe extern "C" fn fw_filter_rs(
    dir: u32,
    peer_ip: u32,
    proto: u8,
    frag_off: u16,
    payload: *const u8,
    len: u16,
) -> i32 {
    let t0 = core::arch::x86_64::_rdtsc();
    let v = fw_decide(dir, peer_ip, proto, frag_off, payload, len);
    let dt = core::arch::x86_64::_rdtsc().wrapping_sub(t0);
    STATS.calls += 1;
    STATS.cyc_tot = STATS.cyc_tot.wrapping_add(dt);
    if dt > STATS.cyc_max {
        STATS.cyc_max = dt;
    }
    v
}

// ===========================================================================
// Validation and installation.
// ===========================================================================

/// Every field checked. A ruleset containing ANY invalid rule is refused
/// WHOLE: there is no partial install, because a policy that is half of what
/// was asked for is a policy nobody decided on. The caller keeps the previous
/// policy, which is the fail-closed outcome.
fn cfg_validate(c: &FwConfig) -> bool {
    if c.version != FW_CFG_VERSION {
        return false;
    }
    if c.enabled > 1 || c.pol_in > 1 || c.pol_out > 1 {
        return false;
    }
    if c.rule_count as usize > FW_MAX_RULES {
        return false;
    }
    for i in 0..c.rule_count as usize {
        let r = &c.rules[i];
        if r.dir > 1 || r.action > 1 || r.proto > 1 {
            return false;
        }
        if r.port == 0 {
            return false;
        }
        if r.reserved != 0 || r.reserved2 != 0 {
            return false;
        }
    }
    true
}

/// Install a validated policy and flush the flow table.
///
/// Returns 0 on success, -1 if the config is invalid (NOTHING is changed).
/// Must be called from process context, never from the packet path.
#[no_mangle]
pub unsafe extern "C" fn fw_install_rs(cfg: *const FwConfig) -> i32 {
    if cfg.is_null() {
        return -1;
    }
    let c = *cfg;
    if !cfg_validate(&c) {
        return -1;
    }
    // Write the inactive buffer, then publish it with one atomic store. A
    // packet-path reader sees either the whole old policy or the whole new
    // one, never a mixture.
    let cur = CFG_ACTIVE.load(Ordering::Relaxed) as usize & 1;
    let next = cur ^ 1;
    (*(&raw mut CFG))[next] = c;
    CFG_ACTIVE.store(next as u32, Ordering::Release);
    // Flush every tracked flow: a policy change re-evaluates everything.
    CT_GEN.fetch_add(1, Ordering::Relaxed);
    0
}

#[no_mangle]
pub unsafe extern "C" fn fw_get_config_rs(out: *mut FwConfig) -> i32 {
    if out.is_null() {
        return -1;
    }
    *out = *cfg_read();
    0
}

#[no_mangle]
pub unsafe extern "C" fn fw_get_stats_rs(out: *mut FwStats) -> i32 {
    if out.is_null() {
        return -1;
    }
    let mut s = STATS;
    let c = cfg_read();
    s.ct_used = ct_live();
    s.enabled = c.enabled as u32;
    s.rule_count = c.rule_count as u32;
    s.pol = (c.pol_in as u32) | ((c.pol_out as u32) << 8);
    *out = s;
    0
}

#[no_mangle]
pub unsafe extern "C" fn fw_reset_stats_rs() {
    STATS = STATS_ZERO;
}

/// The compiled-in fail-safe, for a caller that needs to fall back to it.
#[no_mangle]
pub unsafe extern "C" fn fw_default_config_rs(out: *mut FwConfig) -> i32 {
    if out.is_null() {
        return -1;
    }
    *out = DEFAULT_CFG;
    0
}

// ===========================================================================
// The config codec. ONE parser and ONE serialiser for /CONFIG/FWRULES.CFG,
// both here, both in the kernel, because the kernel owns the file.
//
// The text format is UNCHANGED from what the Settings app used to write, so
// an existing file still parses:
//
//     on                 or  off
//     pin  <0|1>             default inbound policy  (0 = allow, 1 = deny)
//     pout <0|1>             default outbound policy
//     r <dir> <action> <proto> <port>     0..12 of these
//
// Blank lines and lines beginning '#' are comments.
// ===========================================================================

/// Result of a parse, returned to C packed in one i32:
///   >= 0  : usable config, value = number of MALFORMED LINES IGNORED
///   -1    : bad arguments
///   -2    : the text carried no usable directive at all. The caller must
///           fall back to DEFAULT_CFG, NOT to an empty ruleset - an empty
///           ruleset under inbound DENY would silently lock out sshd, which
///           is a fail-open-looking change that is actually a fail-shut one,
///           and either way it is not a policy anyone chose.
#[no_mangle]
pub unsafe extern "C" fn fw_parse_cfg_rs(text: *const u8, len: u32, out: *mut FwConfig) -> i32 {
    if text.is_null() || out.is_null() {
        return -1;
    }
    let buf = core::slice::from_raw_parts(text, len as usize);

    let mut c = FwConfig {
        version: FW_CFG_VERSION,
        enabled: 1,
        pol_in: ACT_DENY,
        pol_out: ACT_ALLOW,
        rule_count: 0,
        rules: [NORULE; FW_MAX_RULES],
    };
    let mut bad: i32 = 0;
    let mut good: i32 = 0;

    let mut i = 0usize;
    while i < buf.len() {
        let mut j = i;
        while j < buf.len() && buf[j] != b'\n' {
            j += 1;
        }
        let mut line = &buf[i..j];
        i = j + 1;
        // strip \r and surrounding spaces
        while !line.is_empty() && (line[line.len() - 1] == b'\r' || line[line.len() - 1] == b' ') {
            line = &line[..line.len() - 1];
        }
        while !line.is_empty() && line[0] == b' ' {
            line = &line[1..];
        }
        if line.is_empty() || line[0] == b'#' {
            continue;
        }

        if eqs(line, b"on") {
            c.enabled = 1;
            good += 1;
        } else if eqs(line, b"off") {
            c.enabled = 0;
            good += 1;
        } else if starts(line, b"pin ") {
            match one_num(&line[4..], 0, 1) {
                Some(v) => { c.pol_in = v as u8; good += 1; }
                None => bad += 1,
            }
        } else if starts(line, b"pout ") {
            match one_num(&line[5..], 0, 1) {
                Some(v) => { c.pol_out = v as u8; good += 1; }
                None => bad += 1,
            }
        } else if starts(line, b"r ") {
            match parse_rule(&line[2..]) {
                Some(r) => {
                    if (c.rule_count as usize) < FW_MAX_RULES {
                        c.rules[c.rule_count as usize] = r;
                        c.rule_count += 1;
                        good += 1;
                    } else {
                        // More rules than the model holds. Refusing the extra
                        // is the only honest outcome: silently keeping 12 of
                        // 15 while the file says 15 is exactly the
                        // description-that-disagrees fault this ticket exists
                        // to remove, so it is counted as ignored and the
                        // caller reports it.
                        bad += 1;
                    }
                }
                None => bad += 1,
            }
        } else {
            bad += 1;
        }
    }

    if good == 0 {
        return -2;
    }
    if !cfg_validate(&c) {
        return -2;
    }
    *out = c;
    bad
}

fn eqs(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && {
        let mut k = 0;
        let mut ok = true;
        while k < a.len() {
            if a[k] != b[k] { ok = false; break; }
            k += 1;
        }
        ok
    }
}

fn starts(a: &[u8], b: &[u8]) -> bool {
    a.len() >= b.len() && eqs(&a[..b.len()], b)
}

/// A whole field must be digits and the value must be in range. "1x", "" and
/// "99" for a 0..1 field are all rejected rather than silently truncated: a
/// malformed rule that becomes a PERMISSIVE rule is the worst outcome
/// available, so nothing is coerced.
fn one_num(s: &[u8], lo: u32, hi: u32) -> Option<u32> {
    let mut t = s;
    while !t.is_empty() && t[0] == b' ' {
        t = &t[1..];
    }
    while !t.is_empty() && t[t.len() - 1] == b' ' {
        t = &t[..t.len() - 1];
    }
    if t.is_empty() || t.len() > 5 {
        return None;
    }
    let mut v: u32 = 0;
    for &ch in t {
        if !(b'0'..=b'9').contains(&ch) {
            return None;
        }
        v = v * 10 + (ch - b'0') as u32;
    }
    if v < lo || v > hi {
        return None;
    }
    Some(v)
}

fn parse_rule(s: &[u8]) -> Option<FwRule> {
    let mut f: [&[u8]; 4] = [&[], &[], &[], &[]];
    let mut n = 0usize;
    let mut i = 0usize;
    while i < s.len() && n < 4 {
        while i < s.len() && s[i] == b' ' {
            i += 1;
        }
        let st = i;
        while i < s.len() && s[i] != b' ' {
            i += 1;
        }
        if i > st {
            f[n] = &s[st..i];
            n += 1;
        }
    }
    // Trailing junk after the fourth field is a malformed line, not a rule
    // with extras: accepting it would accept "r 0 0 0 22 nonsense".
    while i < s.len() && s[i] == b' ' {
        i += 1;
    }
    if n != 4 || i != s.len() {
        return None;
    }
    let dir = one_num(f[0], 0, 1)?;
    let act = one_num(f[1], 0, 1)?;
    let pro = one_num(f[2], 0, 1)?;
    let port = one_num(f[3], 1, 65535)?;
    Some(rule(dir as u8, act as u8, pro as u8, port as u16))
}

/// Serialise a config to the text format above. Returns bytes written, or -1
/// if the buffer is too small (nothing partial is written to the file: the C
/// caller checks and refuses to write).
#[no_mangle]
pub unsafe extern "C" fn fw_format_cfg_rs(cfg: *const FwConfig, out: *mut u8, max: u32) -> i32 {
    if cfg.is_null() || out.is_null() {
        return -1;
    }
    let c = &*cfg;
    let dst = core::slice::from_raw_parts_mut(out, max as usize);
    let mut w = Writer { d: dst, n: 0, of: false };

    w.s(b"# /CONFIG/FWRULES.CFG - written by the kernel packet filter (#238).\n");
    w.s(b"# The KERNEL owns this file. Settings edits it through SYS_NET_FW;\n");
    w.s(b"# nothing in userland parses or writes it. Hand edits are read at\n");
    w.s(b"# boot and by `fw reload`; a malformed line is ignored and counted.\n");
    w.s(if c.enabled != 0 { b"on\n" as &[u8] } else { b"off\n" as &[u8] });
    w.s(b"pin ");
    w.u(c.pol_in as u32);
    w.s(b"\n");
    w.s(b"pout ");
    w.u(c.pol_out as u32);
    w.s(b"\n");
    let n = c.rule_count as usize;
    let n = if n > FW_MAX_RULES { FW_MAX_RULES } else { n };
    for i in 0..n {
        let r = &c.rules[i];
        w.s(b"r ");
        w.u(r.dir as u32);
        w.s(b" ");
        w.u(r.action as u32);
        w.s(b" ");
        w.u(r.proto as u32);
        w.s(b" ");
        w.u(r.port as u32);
        w.s(b"\n");
    }
    if w.of {
        -1
    } else {
        w.n as i32
    }
}

struct Writer<'a> {
    d: &'a mut [u8],
    n: usize,
    of: bool,
}
impl<'a> Writer<'a> {
    fn s(&mut self, b: &[u8]) {
        for &ch in b {
            if self.n >= self.d.len() {
                self.of = true;
                return;
            }
            self.d[self.n] = ch;
            self.n += 1;
        }
    }
    fn u(&mut self, v: u32) {
        let mut t = [0u8; 10];
        let mut k = 0;
        let mut x = v;
        if x == 0 {
            t[0] = b'0';
            k = 1;
        }
        while x > 0 {
            t[k] = b'0' + (x % 10) as u8;
            x /= 10;
            k += 1;
        }
        while k > 0 {
            k -= 1;
            let c = t[k];
            self.s(&[c]);
        }
    }
}

// ===========================================================================
// Boot-time self-test.
//
// A filter nobody has watched DROP a packet is indistinguishable from one
// that is not wired up (#514/#665). This drives the real decision function
// over a vector table covering every branch that matters and reports one
// [FW] line. `make FWTESTFAIL=1` makes it assert a WRONG expectation so the
// line can be SEEN to say FAIL on an otherwise healthy machine.
//
// It runs BEFORE the boot-time config load and restores the previous policy
// and counters when it finishes, so it cannot perturb the running system.
// ===========================================================================

/// The expected verdict of a self-test vector. `make FWTESTFAIL=1` inverts
/// every one of them, so the [FW] boot line can be SEEN to say FAIL on an
/// otherwise healthy machine.
#[cfg(not(fw_test_fail))]
#[inline(always)]
fn want(v: i32) -> i32 { v }
#[cfg(fw_test_fail)]
#[inline(always)]
fn want(v: i32) -> i32 { 1 - v }

#[no_mangle]
pub unsafe extern "C" fn fw_selftest_rs() -> i32 {
    let saved = *cfg_read();
    let saved_stats = STATS;
    let mut fails = 0i32;

    // Policy under test: inbound deny with 22 allowed, outbound allow with
    // 25 denied. Exactly the shape a person configures.
    let mut c = FwConfig {
        version: FW_CFG_VERSION,
        enabled: 1,
        pol_in: ACT_DENY,
        pol_out: ACT_ALLOW,
        rule_count: 2,
        rules: [NORULE; FW_MAX_RULES],
    };
    c.rules[0] = rule(FW_DIR_IN as u8, ACT_ALLOW, RP_TCP, 22);
    c.rules[1] = rule(FW_DIR_OUT as u8, ACT_DENY, RP_TCP, 25);
    if fw_install_rs(&c) != 0 {
        return 99;
    }

    // A TCP SYN header: sport, dport, seq, ack, off/flags.
    let mut tcp = [0u8; 20];
    let mk = |b: &mut [u8; 20], sp: u16, dp: u16, flags: u8| {
        b[0] = (sp >> 8) as u8; b[1] = sp as u8;
        b[2] = (dp >> 8) as u8; b[3] = dp as u8;
        b[13] = flags;
    };
    let mut udp = [0u8; 8];
    let mku = |b: &mut [u8; 8], sp: u16, dp: u16| {
        b[0] = (sp >> 8) as u8; b[1] = sp as u8;
        b[2] = (dp >> 8) as u8; b[3] = dp as u8;
    };

    let peer: u32 = 0xC0A8_0105; // 192.0.2.1

    // 1. inbound SYN to 22 -> allowed by the rule.
    mk(&mut tcp, 40000, 22, 0x02);
    if fw_decide(FW_DIR_IN, peer, IPP_TCP, 0, tcp.as_ptr(), 20) != want(PASS) { fails += 1; }
    // 2. inbound SYN to 23 -> no rule, inbound policy DENY.
    mk(&mut tcp, 40001, 23, 0x02);
    if fw_decide(FW_DIR_IN, peer, IPP_TCP, 0, tcp.as_ptr(), 20) != want(DROP) { fails += 1; }
    // 3. outbound SYN to 25 -> denied by the rule despite policy ALLOW.
    mk(&mut tcp, 50000, 25, 0x02);
    if fw_decide(FW_DIR_OUT, peer, IPP_TCP, 0, tcp.as_ptr(), 20) != want(DROP) { fails += 1; }
    // 4. outbound SYN to 443 -> no rule, outbound policy ALLOW.
    mk(&mut tcp, 50001, 443, 0x02);
    if fw_decide(FW_DIR_OUT, peer, IPP_TCP, 0, tcp.as_ptr(), 20) != want(PASS) { fails += 1; }
    // 5. THE STATEFUL CASE. The reply to test 4 arrives inbound from 443 to
    //    our port 50001. Inbound policy is DENY and no rule permits it; it
    //    must pass because it belongs to a flow we opened. Without this, a
    //    default-deny-inbound machine could not browse.
    mk(&mut tcp, 443, 50001, 0x12);
    if fw_decide(FW_DIR_IN, peer, IPP_TCP, 0, tcp.as_ptr(), 20) != want(PASS) { fails += 1; }
    // 6. An UNRELATED inbound segment from 443 to a port we never used is
    //    still evaluated, and denied. (Proves 5 was a flow match, not a
    //    protocol-wide hole.)
    mk(&mut tcp, 443, 50002, 0x12);
    if fw_decide(FW_DIR_IN, peer, IPP_TCP, 0, tcp.as_ptr(), 20) != want(DROP) { fails += 1; }
    // 7. RST tears the flow down; the next packet on it is re-evaluated and
    //    now denied.
    mk(&mut tcp, 443, 50001, TCP_RST);
    if fw_decide(FW_DIR_IN, peer, IPP_TCP, 0, tcp.as_ptr(), 20) != want(PASS) { fails += 1; }
    mk(&mut tcp, 443, 50001, 0x10);
    if fw_decide(FW_DIR_IN, peer, IPP_TCP, 0, tcp.as_ptr(), 20) != want(DROP) { fails += 1; }
    // 9. DHCP client exchange is exempt under inbound DENY.
    mku(&mut udp, 67, 68);
    if fw_decide(FW_DIR_IN, peer, IPP_UDP, 0, udp.as_ptr(), 8) != want(PASS) { fails += 1; }
    // 10. DNS reply with NO prior query is evaluated and denied.
    mku(&mut udp, 53, 33333);
    if fw_decide(FW_DIR_IN, peer, IPP_UDP, 0, udp.as_ptr(), 8) != want(DROP) { fails += 1; }
    // 11. ...but after our query goes out, the reply passes.
    mku(&mut udp, 33333, 53);
    if fw_decide(FW_DIR_OUT, peer, IPP_UDP, 0, udp.as_ptr(), 8) != want(PASS) { fails += 1; }
    mku(&mut udp, 53, 33333);
    if fw_decide(FW_DIR_IN, peer, IPP_UDP, 0, udp.as_ptr(), 8) != want(PASS) { fails += 1; }
    // 13. A truncated TCP header is dropped, not guessed at.
    mk(&mut tcp, 1, 22, 0x02);
    if fw_decide(FW_DIR_IN, peer, IPP_TCP, 0, tcp.as_ptr(), 8) != want(DROP) { fails += 1; }
    // 14. A non-first fragment is dropped.
    mk(&mut tcp, 40002, 22, 0x02);
    if fw_decide(FW_DIR_IN, peer, IPP_TCP, 37, tcp.as_ptr(), 20) != want(DROP) { fails += 1; }
    // 15. ICMP is outside the rule model and passes, counted.
    if fw_decide(FW_DIR_IN, peer, 1, 0, tcp.as_ptr(), 20) != want(PASS) { fails += 1; }
    // 16. Disabled means disabled: the port-23 SYN that was dropped in test 2
    //     now passes.
    c.enabled = 0;
    if fw_install_rs(&c) != 0 {
        fails += 1;
    }
    mk(&mut tcp, 40003, 23, 0x02);
    if fw_decide(FW_DIR_IN, peer, IPP_TCP, 0, tcp.as_ptr(), 20) != want(PASS) { fails += 1; }

    // 17. Validation refuses a malformed ruleset WHOLE and changes nothing.
    let mut badc = c;
    badc.enabled = 1;
    badc.rule_count = 1;
    badc.rules[0] = FwRule { dir: 7, action: 0, proto: 0, reserved: 0, port: 22, reserved2: 0 };
    if fw_install_rs(&badc) != -1 {
        fails += 1;
    }
    if cfg_read().enabled != 0 {
        fails += 1; // the refused install must not have taken effect
    }

    // 18. The codec round-trips, and a malformed line is counted not accepted.
    let mut txt = [0u8; 512];
    let mut c2 = DEFAULT_CFG;
    c2.rule_count = 2;
    c2.rules[0] = rule(FW_DIR_IN as u8, ACT_ALLOW, RP_TCP, 22);
    c2.rules[1] = rule(FW_DIR_OUT as u8, ACT_DENY, RP_UDP, 161);
    let wrote = fw_format_cfg_rs(&c2, txt.as_mut_ptr(), txt.len() as u32);
    if wrote <= 0 {
        fails += 1;
    } else {
        let mut back = DEFAULT_CFG;
        let bad = fw_parse_cfg_rs(txt.as_ptr(), wrote as u32, &mut back);
        if bad != 0 { fails += 1; }
        if back.enabled != c2.enabled || back.pol_in != c2.pol_in
            || back.pol_out != c2.pol_out || back.rule_count != 2
            || back.rules[1].port != 161 || back.rules[1].action != ACT_DENY {
            fails += 1;
        }
    }
    let junk = b"on\npin 1\nr 0 0 0 22\nr 9 0 0 80\nr 0 0 0 abc\nnonsense\nr 0 1 1 161\n";
    let mut jc = DEFAULT_CFG;
    let bad = fw_parse_cfg_rs(junk.as_ptr(), junk.len() as u32, &mut jc);
    if bad != 3 { fails += 1; }               // three malformed lines counted
    if jc.rule_count != 2 { fails += 1; }     // two good rules kept
    if jc.rules[1].port != 161 { fails += 1; }
    // A file with nothing usable in it must report -2 so the caller falls back
    // to the compiled-in policy rather than to an empty ruleset.
    let empty = b"garbage\n\n# only a comment\n";
    let mut ec = DEFAULT_CFG;
    if fw_parse_cfg_rs(empty.as_ptr(), empty.len() as u32, &mut ec) != -2 {
        fails += 1;
    }

    // Restore. The self-test must leave no trace on the live machine.
    let _ = fw_install_rs(&saved);
    STATS = saved_stats;
    fails
}
