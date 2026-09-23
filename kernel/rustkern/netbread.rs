// rustkern/netbread.rs - #imacnet: DURABLE network breadcrumbs + DNS resolver
// failover policy.
//
// THE TWO FAULTS THIS EXISTS FOR
// ==========================================================================
//
// FAULT 1: THE FETCH PATH IS INVISIBLE ON THE MACHINE IT FAILS ON.
//
// The owner reported "i can ping 1.1.1.1 but again browser not working, app
// store not working" on a real iMac14,4 booting golden 2334 from USB. His
// /BOOTLOG.TXT was pulled off the stick. It contains DHCP, an address, a
// resolved gateway ARP, and the [NETDIAG] traffic counters - and NOT ONE BYTE
// about DNS, TCP, TLS or HTTP.
//
// That is not an accident of that session. It is structural. MEASURED on this
// tree:
//
//     net/dns.c      18 kprintf,  4 bootlog_write
//     net/https.c    74 kprintf,  4 bootlog_write
//     net/wget.c     42 kprintf,  0 bootlog_write
//     net/tls/tls.c   ? kprintf,  4 bootlog_write
//
// and every single one of those bootlog_write() calls is a [RUST-DIFF] /
// [RUST-SEC] / [RUST-PERF] SELF-TEST line. There is not one durable log line
// anywhere in the DNS, HTTP, HTTPS or TLS stack about a real fetch. kprintf is
// SERIAL ONLY. The iMac has no serial port. So the entire operational fetch
// path writes its diagnosis to a wire that does not exist, and a user-visible
// failure leaves no evidence at all.
//
// THIS IS THE FOURTH TIME. net/dhcp.c already carries the lesson in prose
// ("Two diagnostics shipped this week reached only serial and were useless for
// the same reason"), and main.c's [NETDIAG] block carries it a third time
// ("A diagnostic that cannot reach the machine it was written for is not a
// diagnostic"), written when the same owner's golden-2277 log could not say
// whether a frame ever crossed his adapter. Each fix made ONE line durable.
// None of them made the FETCH path durable, so the next question asked on real
// hardware was unanswerable again.
//
// FAULT 2: A PINNED RESOLVER THAT NEVER ANSWERS IS PERMANENTLY FATAL, AND
//          LOOKS EXACTLY LIKE "PING WORKS, BROWSER DOES NOT".
//
// net/dns.c holds ONE server address. There is no secondary. If it does not
// answer, dns_resolve() exhausts its retries, soft-negative-caches, and every
// name lookup in the OS fails forever - while ICMP to a literal address keeps
// working perfectly, because ICMP needs no resolver. That is precisely the
// symptom reported.
//
// And the resolver cannot be corrected. #786 made an explicitly-chosen
// resolver PINNED so a DHCP lease can never overrule the user's choice, which
// is right. But the pin is absolute: dns_set_server_dhcp() logs "ignoring
// DHCP-offered x.x.x.x: resolver is pinned" and returns, forever, even when
// the pinned server has answered exactly zero queries since boot.
//
// MEASURED from the owner's log, and this chain is tight:
//   - [NETDIAG] at dhcp=unbound ip=0.0.0.0 already reads dns=1.1.1.1.
//   - dns_init() defaults to 8.8.8.8, not 1.1.1.1.
//   - the only setter that can run before DHCP is net_apply_static_config(),
//     which calls dns_set_server() -> pinned = 1.
//   - the same line reads cfg=dhcp, so g_net_static_configured == 0, i.e. the
//     file had a dns= line and NO ip= line (net.c's "#786 dns-only config"
//     branch).
//   => his stick carries a dns-only config pinning 1.1.1.1, and the resolver
//      his network actually advertised (192.0.2.1, the Windows ICS DNS
//      proxy, which on an ICS segment is the resolver the gateway expects to
//      serve) was refused.
//
// WHAT THIS MODULE DOES ABOUT IT
// ==========================================================================
// FAILOVER, NOT UNPINNING. The user's choice stays the PREFERRED resolver and
// is never overwritten. But if the preferred server produces DNS_FAILOVER_AFTER
// consecutive NO-ANSWER-AT-ALL outcomes, the ACTIVE server advances to the next
// candidate (the DHCP-offered resolver, then the gateway), and the change is
// ANNOUNCED DURABLY. A user whose pinned resolver is unreachable gets a working
// browser and a log line saying why, instead of silence.
//
// AN ERROR RCODE IS NOT A FAILOVER SIGNAL. NXDOMAIN/SERVFAIL means the server
// IS reachable and IS answering; it is a fact about the NAME, not the server.
// Only a total absence of any reply counts, and any reply at all - including
// NXDOMAIN - resets the counter. Failing over on NXDOMAIN would abandon a
// perfectly good resolver the first time a user typed a hostname wrong.
//
// FAILOVER IS STICKY FOR THE BOOT, DELIBERATELY. There is no background
// re-probe of the preferred server. A re-probe costs a query timeout on some
// unlucky future lookup to recover a resolver we already measured as silent,
// and it would make "which server answered this?" non-deterministic. The
// preferred server is restored by a reboot, or the moment the user sets it
// again in Settings (dns_set_server() re-arms this module).
//
// WHY RUST
// ==========================================================================
// New kernel code, so Rust per the 2026-07-16 rule. No C twin, no -DRUST_*
// strangler flag and no RUST_PORT_LEDGER row: none of this logic existed
// before in any language, so there is nothing to run a differential against.
// No performance argument for C is made or needed - this is a handful of
// atomic loads on a path that is about to spend milliseconds on the wire.
//
// IT NEVER BLOCKS, NEVER ALLOCATES, NEVER CALLS OUT. dns_send() can be reached
// from contexts where wait_event() would deadlock (#549), so everything here is
// static storage and atomics: no wait, no sleep, no lock, no kmalloc, no I/O.
// The C side does all printing and all persistence, in process context.

use core::sync::atomic::{AtomicU32, Ordering};

// Consecutive no-answer outcomes on the ACTIVE server before advancing to the
// next candidate. Three is one full dns_resolve() call: DNS_MAX_RETRIES is 3,
// so this is "one entire lookup produced not a single packet back", which is
// the weakest evidence that is still unambiguous. Two would fire on a single
// unlucky lookup that lost two datagrams; ten would make the user wait through
// several dead lookups before the OS helped itself.
const DNS_FAILOVER_AFTER: u32 = 3;

// ---------------------------------------------------------------------------
// THE CANDIDATE LADDER, AND WHY IT NOW HAS PUBLIC RESOLVERS ON IT (#dnsfallback)
// ---------------------------------------------------------------------------
// MEASURED, from the owner's real iMac14,4 running golden 2353
// (the build host:/root/imac-logs-dns/BOOTLOG.TXT, 2026-08-31):
//
//   [NETDIAG] ... dns=192.0.2.1 dnspref=192.0.2.1 gw=192.0.2.1
//             ... dnsq=43/1/41/0/0
//
// Read the last field: sent=43, ok=1, timeout=41, rcode=0, FAILOVERS=0. This
// module's failover NEVER FIRED, through forty-one unanswered queries, and the
// hedge in dns.c never sent a single hedged datagram. Not because either was
// broken, but because THE LADDER HAD NOWHERE TO GO.
//
// His gateway is a Windows 11 box running Internet Connection Sharing. On an
// ICS segment the DHCP-offered resolver, the gateway and (once DHCP had
// applied it) the preferred resolver are ALL 192.0.2.1. The old candidate
// list was exactly {DHCP-offered, gateway}, and both were equal to the server
// already in use, so `netbread_dns_note_timeout_rs` fell into its "no alternate
// exists" branch every time and dns.c's hedge computed hedge_to = 0.
//
// So the previous fix was correct and inapplicable: it could hedge FROM a
// pinned resolver TO the DHCP one, and his dead half WAS the DHCP one. The
// escalation has to be able to leave the local network entirely, because on
// this topology every locally-endorsed candidate is the same dead host.
//
// WHY THE WINDOWS BOX IS SILENT ON 53 BUT NOT ON 67/68: the same log shows DHCP
// DISCOVER/OFFER/ACK completing and a lease granted from that host. One
// protocol served, one not, same host, same interface. That points at Windows
// Firewall or the ICS DNS proxy, and it is the owner's side to fix. Ours is to
// cope with it, which is what this ladder does.
//
// PRIVACY / BEHAVIOUR NOTE. Escalating to a public resolver sends the user's
// queries to a third party, so it is gated, not automatic: see LOCAL_ANSWERED
// below. A machine whose own network resolves names never sends a query to any
// of these addresses.
const PUB_A: u32 = 0x0101_0101;   // 1.1.1.1   Cloudflare
const PUB_B: u32 = 0x0808_0808;   // 8.8.8.8   Google
const PUB_C: u32 = 0x0909_0909;   // 9.9.9.9   Quad9

// Ladder stages, in escalation order:
//   0 PREFERRED      the user's / config's / DHCP-applied choice
//   1 DHCP_OFFERED   what the lease advertised
//   2 GATEWAY        on most home routers and on ICS, the gateway IS the resolver
//   3..5 PUB_A/B/C   off-network last resort
// Escalation is monotone within a lookup and wraps once, so a machine that has
// walked the whole ladder returns to its preferred resolver rather than being
// parked forever on the last public one.
const LADDER_LEN: u32 = 6;

// Durable [NETFETCH] line budget for the whole boot. bootlog_write() rewrites
// the growing file, so this is a real cost and not a formality (see the COST
// note in main.c's [NETDIAG] block). 32 lines is enough to cover a browser
// page load plus an App Store listing plus their retries, and small enough that
// a pathological retry loop cannot turn the log into the fault.
const FETCH_LOG_BUDGET: u32 = 32;

// ---------------------------------------------------------------------------
// Resolver candidates. PREFERRED is the user's/config's choice and is written
// ONLY by dns_set_server()/dns_init(). ACTIVE is what dns.c actually sends to.
// ---------------------------------------------------------------------------
static PREFERRED: AtomicU32 = AtomicU32::new(0);
static DHCP_OFFERED: AtomicU32 = AtomicU32::new(0);
static GATEWAY: AtomicU32 = AtomicU32::new(0);
static ACTIVE: AtomicU32 = AtomicU32::new(0);

// Query outcome counters, surfaced on the ALREADY-DURABLE [NETDIAG] line so
// "was DNS ever even attempted, and did anything ever answer" is readable off a
// stick with no serial port and no extra log budget at all.
static N_SENT: AtomicU32 = AtomicU32::new(0);
static N_OK: AtomicU32 = AtomicU32::new(0);
static N_TIMEOUT: AtomicU32 = AtomicU32::new(0);
static N_RCODE: AtomicU32 = AtomicU32::new(0);
static N_FAILOVER: AtomicU32 = AtomicU32::new(0);
static CONSEC_SILENT: AtomicU32 = AtomicU32::new(0);

// Current ladder stage. Advanced ONLY by netbread_dns_note_timeout_rs().
static STAGE: AtomicU32 = AtomicU32::new(0);

// RUNTIME STATE, NOT CONFIG. THIS DISTINCTION IS THE WHOLE POINT (#dnsfallback).
//
// LEARNED is the last resolver that actually put a datagram back on the wire.
// It is remembered so a machine does not re-probe a host it has already
// measured as dead on every single lookup. It lives in a static atomic, is
// never written to /CONFIG, is never consulted by dns_init(), and is gone at
// the next power cycle.
//
// That is precisely what a PIN is not. #786 made an explicitly-chosen resolver
// persistent, which outlived the network it was chosen on and left this owner's
// iMac pointed at a resolver his ICS segment did not serve. Removing that pin
// was right. Re-adding one under a new name would be the same bug: a learned
// resolver must not survive a reboot, because the next network may be a
// different one.
static LEARNED: AtomicU32 = AtomicU32::new(0);

// Has any NON-PUBLIC resolver answered anything at all this boot? Gates
// whether a public resolver may be offered as a HEDGE candidate. On a network
// whose own DNS works, this is set by the first successful lookup and no query
// is ever hedged off-network. It deliberately does NOT gate the slow failover:
// three consecutive silent attempts is evidence that the resolver which used to
// work has stopped, and at that point leaving the network is the correct move.
static LOCAL_ANSWERED: AtomicU32 = AtomicU32::new(0);

// [NETFETCH] budget + consecutive-duplicate suppression.
static FETCH_LOGGED: AtomicU32 = AtomicU32::new(0);
static FETCH_SUPPRESSED: AtomicU32 = AtomicU32::new(0);
static LAST_FETCH_KEY: AtomicU32 = AtomicU32::new(0);

fn is_public(ip: u32) -> bool {
    ip == PUB_A || ip == PUB_B || ip == PUB_C
}

fn ladder_at(stage: u32) -> u32 {
    match stage {
        0 => PREFERRED.load(Ordering::SeqCst),
        1 => DHCP_OFFERED.load(Ordering::SeqCst),
        2 => GATEWAY.load(Ordering::SeqCst),
        3 => PUB_A,
        4 => PUB_B,
        5 => PUB_C,
        _ => 0,
    }
}

// The first ladder entry strictly AFTER `from_stage` (wrapping once) that is
// non-zero, different from `cur`, and permitted. Returns (stage, ip).
//
// `allow_public` is false for hedge candidates on a network whose own resolver
// has already answered something: see LOCAL_ANSWERED.
fn ladder_next(from_stage: u32, cur: u32, allow_public: bool) -> Option<(u32, u32)> {
    let mut i: u32 = 1;
    while i <= LADDER_LEN {
        let s = (from_stage + i) % LADDER_LEN;
        let ip = ladder_at(s);
        if ip != 0 && ip != cur && (allow_public || !is_public(ip)) {
            return Some((s, ip));
        }
        i += 1;
    }
    None
}

// Is this address one of the public last-resort resolvers? Exposed so the C
// side can word its durable log line honestly ("off-network") rather than
// pretending every failover target is a local one.
#[no_mangle]
pub extern "C" fn netbread_dns_is_public_rs(ip: u32) -> i32 {
    if is_public(ip) { 1 } else { 0 }
}

// The last resolver that actually replied to anything this boot, or 0.
// Runtime state only; see the LEARNED comment above.
#[no_mangle]
pub extern "C" fn netbread_dns_learned_rs() -> u32 {
    LEARNED.load(Ordering::SeqCst)
}

// The server a hedged query should ALSO go to, given the one we are already
// using. Does NOT advance any state: a hedge is a guess, and a guess must not
// move the machine's idea of which resolver it is on. 0 = do not hedge.
#[no_mangle]
pub extern "C" fn netbread_dns_hedge_candidate_rs(cur: u32) -> u32 {
    let allow_public = LOCAL_ANSWERED.load(Ordering::SeqCst) == 0;
    let st = STAGE.load(Ordering::SeqCst);
    match ladder_next(st, cur, allow_public) {
        Some((_, ip)) => ip,
        None => 0,
    }
}

// Set the PREFERRED resolver. Called from dns_init() (compiled-in default) and
// from dns_set_server() (explicit, pinned choice). Re-arms failover: an
// explicit new choice deserves to be tried on its own merits, not to inherit
// the previous server's failure count.
#[no_mangle]
pub extern "C" fn netbread_dns_set_preferred_rs(ip: u32) {
    PREFERRED.store(ip, Ordering::SeqCst);
    ACTIVE.store(ip, Ordering::SeqCst);
    CONSEC_SILENT.store(0, Ordering::SeqCst);
    // Back to the top of the ladder, and FORGET what was learned. An explicit
    // new choice has to be tried on its own merits; leaving LEARNED set would
    // let the previous boot's discovery silently override the thing the user
    // just asked for on the very next lookup, which is indistinguishable from
    // the Settings panel not working.
    STAGE.store(0, Ordering::SeqCst);
    LEARNED.store(0, Ordering::SeqCst);
}

// Record the resolver a DHCP lease offered. This is remembered EVEN WHEN THE
// PIN REFUSES IT, which is the whole point: it is the only candidate we know
// the local network actually endorses, and on an ICS segment it is the only
// one the gateway is guaranteed to serve.
#[no_mangle]
pub extern "C" fn netbread_dns_set_dhcp_rs(ip: u32) {
    DHCP_OFFERED.store(ip, Ordering::SeqCst);
    // If nothing has been chosen at all, adopt it as the active server.
    let _ = ACTIVE.compare_exchange(0, ip, Ordering::SeqCst, Ordering::SeqCst);
}

#[no_mangle]
pub extern "C" fn netbread_dns_set_gateway_rs(ip: u32) {
    GATEWAY.store(ip, Ordering::SeqCst);
}

// The server dns.c should send to right now.
#[no_mangle]
pub extern "C" fn netbread_dns_active_rs() -> u32 {
    let a = ACTIVE.load(Ordering::SeqCst);
    if a != 0 { a } else { PREFERRED.load(Ordering::SeqCst) }
}

#[no_mangle]
pub extern "C" fn netbread_dns_preferred_rs() -> u32 {
    PREFERRED.load(Ordering::SeqCst)
}

#[no_mangle]
pub extern "C" fn netbread_dns_note_sent_rs() {
    N_SENT.fetch_add(1, Ordering::SeqCst);
}

// A reply arrived. rcode 0 = an answer, anything else = an error reply. EITHER
// WAY the server is alive, so the silence counter resets. See the header note.
#[no_mangle]
pub extern "C" fn netbread_dns_note_answer_rs(rcode: i32, from: u32) {
    if rcode == 0 {
        N_OK.fetch_add(1, Ordering::SeqCst);
    } else {
        N_RCODE.fetch_add(1, Ordering::SeqCst);
    }
    CONSEC_SILENT.store(0, Ordering::SeqCst);
    // REMEMBER WHAT WORKED. `from` is the source address of the reply, in host
    // order, taken straight off the wire by dns.c. An NXDOMAIN counts here for
    // the same reason it resets the silence run: it proves the host is alive
    // and serving DNS, which is the only property this record is about.
    if from != 0 {
        LEARNED.store(from, Ordering::SeqCst);
        if !is_public(from) {
            LOCAL_ANSWERED.store(1, Ordering::SeqCst);
        }
    }
}

// No reply at all. Returns the NEW active server if this tipped a failover, or
// 0 if nothing changed. The caller announces; this function never prints.
//
// Candidate order after the preferred server: the DHCP-offered resolver first
// (the network told us to use it), then the gateway (on a home router and on a
// Windows ICS segment the gateway IS the resolver). A candidate equal to the
// current active one, or zero, is skipped.
#[no_mangle]
pub extern "C" fn netbread_dns_note_timeout_rs(cur: u32) -> u32 {
    N_TIMEOUT.fetch_add(1, Ordering::SeqCst);
    let n = CONSEC_SILENT.fetch_add(1, Ordering::SeqCst) + 1;
    if n < DNS_FAILOVER_AFTER {
        return 0;
    }
    // `cur` is passed IN rather than read from ACTIVE, and that is a deliberate
    // correction. ACTIVE was this module's private copy of "which resolver are
    // we on", and NOTHING OUTSIDE THIS FILE EVER READ IT: dns.c keeps its own
    // `dns_server` and changes it in three places (hedge win, slow failover,
    // Settings). Two copies of one fact, one of which nobody reads, is a fact
    // that can drift, and a failover decision made against a stale copy of the
    // current server is a failover to the server we are already using. The
    // caller's value is the only authoritative one.
    let cur = if cur != 0 { cur } else { ACTIVE.load(Ordering::SeqCst) };
    let st = STAGE.load(Ordering::SeqCst);
    match ladder_next(st, cur, true) {
        Some((stage, next)) => {
            STAGE.store(stage, Ordering::SeqCst);
            ACTIVE.store(next, Ordering::SeqCst);
            CONSEC_SILENT.store(0, Ordering::SeqCst);
            N_FAILOVER.fetch_add(1, Ordering::SeqCst);
            next
        }
        None => {
            // Every candidate is zero or is the server we are already on.
            // Reset the counter so we do not re-evaluate on every subsequent
            // timeout, and change NOTHING - in particular do not zero ACTIVE,
            // which would turn "no better resolver" into "no resolver at all".
            CONSEC_SILENT.store(0, Ordering::SeqCst);
            0
        }
    }
}

// Counters for the [NETDIAG] line. Any pointer may be null.
#[no_mangle]
pub extern "C" fn netbread_dns_counters_rs(
    sent: *mut u32,
    ok: *mut u32,
    timeout: *mut u32,
    rcode: *mut u32,
    failovers: *mut u32,
) {
    unsafe {
        if !sent.is_null() { *sent = N_SENT.load(Ordering::SeqCst); }
        if !ok.is_null() { *ok = N_OK.load(Ordering::SeqCst); }
        if !timeout.is_null() { *timeout = N_TIMEOUT.load(Ordering::SeqCst); }
        if !rcode.is_null() { *rcode = N_RCODE.load(Ordering::SeqCst); }
        if !failovers.is_null() { *failovers = N_FAILOVER.load(Ordering::SeqCst); }
    }
}

// Ladder state for the [NETDIAG] line: which stage we are on, and the learned
// working resolver. Both are the questions a log reader asks next after seeing
// a non-zero failover count, and neither was answerable off the owner's stick.
#[no_mangle]
pub extern "C" fn netbread_dns_ladder_rs(stage: *mut u32, learned: *mut u32) {
    unsafe {
        if !stage.is_null() { *stage = STAGE.load(Ordering::SeqCst); }
        if !learned.is_null() { *learned = LEARNED.load(Ordering::SeqCst); }
    }
}

// FNV-1a over a NUL-terminated C string, bounded. Used to key the [NETFETCH]
// duplicate suppressor without holding a copy of the URL in this module.
// `len_cap` bounds the walk so a non-terminated buffer cannot run away.
#[no_mangle]
pub extern "C" fn netbread_strhash_rs(s: *const u8, len_cap: u32) -> u32 {
    if s.is_null() { return 0; }
    let mut h: u32 = 0x811c_9dc5;
    let mut i: u32 = 0;
    while i < len_cap {
        let c = unsafe { *s.add(i as usize) };
        if c == 0 { break; }
        h ^= c as u32;
        h = h.wrapping_mul(0x0100_0193);
        i += 1;
    }
    h
}

// Should this fetch outcome get a durable [NETFETCH] line?
//
// 1 = yes (and the budget is charged), 0 = no.
//
// Two gates. First, CONSECUTIVE DUPLICATE SUPPRESSION on (host-hash, rc,
// status): a browser retrying the same dead URL in a loop is one fact, not
// forty. Second, the boot budget. A FAILURE always outranks a success for the
// remaining budget, because the successes are not what anyone reads this log
// to find out; so once the budget is half gone, only failures are still
// admitted. The suppressed count is reported by netbread_fetch_stats_rs() so
// the line the log DOES carry can say how many it stands for.
#[no_mangle]
pub extern "C" fn netbread_fetch_should_log_rs(host_hash: u32, rc: i32, status: i32) -> i32 {
    let key = host_hash
        ^ ((rc as u32) << 1).rotate_left(7)
        ^ (status as u32).rotate_left(19);
    // A key of 0 would collide with "nothing logged yet"; nudge it.
    let key = if key == 0 { 1 } else { key };
    if LAST_FETCH_KEY.load(Ordering::SeqCst) == key {
        FETCH_SUPPRESSED.fetch_add(1, Ordering::SeqCst);
        return 0;
    }
    let used = FETCH_LOGGED.load(Ordering::SeqCst);
    if used >= FETCH_LOG_BUDGET {
        FETCH_SUPPRESSED.fetch_add(1, Ordering::SeqCst);
        return 0;
    }
    let failed = rc < 0 || status < 200 || status >= 400;
    if used >= FETCH_LOG_BUDGET / 2 && !failed {
        // Second half of the budget is reserved for failures.
        FETCH_SUPPRESSED.fetch_add(1, Ordering::SeqCst);
        return 0;
    }
    LAST_FETCH_KEY.store(key, Ordering::SeqCst);
    FETCH_LOGGED.fetch_add(1, Ordering::SeqCst);
    1
}

#[no_mangle]
pub extern "C" fn netbread_fetch_stats_rs(logged: *mut u32, suppressed: *mut u32) {
    unsafe {
        if !logged.is_null() { *logged = FETCH_LOGGED.load(Ordering::SeqCst); }
        if !suppressed.is_null() { *suppressed = FETCH_SUPPRESSED.load(Ordering::SeqCst); }
    }
}

// ---------------------------------------------------------------------------
// Boot self-test. Returns a bitmask of FAILING checks (0 = all passed) and
// writes the number of checks run through `checks`.
//
// IT IS DISCRIMINATING, which is the only property that makes a self-test worth
// shipping: every case below is one that the OBVIOUS WRONG implementation would
// fail. Specifically it proves (a) an error rcode does NOT cause failover,
// which is the single most tempting bug in this module, (b) failover needs
// DNS_FAILOVER_AFTER silences and not fewer, (c) with no alternate candidate
// nothing changes rather than the active server being zeroed, and (d) the
// duplicate suppressor actually suppresses and actually stops suppressing when
// the outcome changes.
//
// It saves and restores every static it touches, so it cannot perturb the live
// resolver state, and it runs before net_init() binds anything anyway.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn netbread_selftest_rs(checks: *mut u32) -> u32 {
    // Save.
    let s_pref = PREFERRED.load(Ordering::SeqCst);
    let s_dhcp = DHCP_OFFERED.load(Ordering::SeqCst);
    let s_gw = GATEWAY.load(Ordering::SeqCst);
    let s_act = ACTIVE.load(Ordering::SeqCst);
    let s_sent = N_SENT.load(Ordering::SeqCst);
    let s_ok = N_OK.load(Ordering::SeqCst);
    let s_to = N_TIMEOUT.load(Ordering::SeqCst);
    let s_rc = N_RCODE.load(Ordering::SeqCst);
    let s_fo = N_FAILOVER.load(Ordering::SeqCst);
    let s_cs = CONSEC_SILENT.load(Ordering::SeqCst);
    let s_fl = FETCH_LOGGED.load(Ordering::SeqCst);
    let s_fs = FETCH_SUPPRESSED.load(Ordering::SeqCst);
    let s_fk = LAST_FETCH_KEY.load(Ordering::SeqCst);
    let s_st = STAGE.load(Ordering::SeqCst);
    let s_ln = LEARNED.load(Ordering::SeqCst);
    let s_la = LOCAL_ANSWERED.load(Ordering::SeqCst);

    let mut fail: u32 = 0;
    let mut n: u32 = 0;

    // NOTE ON THE CONSTANTS. PREF must NOT be one of PUB_A/B/C or half these
    // cases would be testing the public ladder against itself. 10.9.9.9 is a
    // deliberately unroutable RFC1918 stand-in for "the user's own choice".
    const PREF: u32 = 0x0A09_0909;  // 10.9.9.9, a private, non-public address
    const DHCPS: u32 = 0xC0A8_8901; // 192.0.2.1, the owner's ICS host
    const GWS: u32 = 0xC0A8_8901;   // ...which is also his gateway

    // (a) an error rcode must NOT advance failover, however many arrive.
    LOCAL_ANSWERED.store(0, Ordering::SeqCst);
    netbread_dns_set_preferred_rs(PREF);
    netbread_dns_set_dhcp_rs(DHCPS);
    netbread_dns_set_gateway_rs(GWS);
    for _ in 0..(DNS_FAILOVER_AFTER * 4) {
        netbread_dns_note_answer_rs(3, PREF); // NXDOMAIN, from the server itself
    }
    n += 1;
    if netbread_dns_active_rs() != PREF { fail |= 1 << 0; }

    // (b) one silence short of the threshold must NOT fail over...
    LOCAL_ANSWERED.store(0, Ordering::SeqCst);
    netbread_dns_set_preferred_rs(PREF);
    for _ in 0..(DNS_FAILOVER_AFTER - 1) {
        let _ = netbread_dns_note_timeout_rs(PREF);
    }
    n += 1;
    if netbread_dns_active_rs() != PREF { fail |= 1 << 1; }
    // ...and the threshold'th must, landing on the DHCP-offered server, which
    // is still tried BEFORE any public one. Escalating straight off-network
    // would send queries to a third party while the local resolver was fine.
    let moved = netbread_dns_note_timeout_rs(PREF);
    n += 1;
    if moved != DHCPS { fail |= 1 << 2; }
    n += 1;
    if netbread_dns_active_rs() != DHCPS { fail |= 1 << 3; }
    // ...and PREFERRED must be untouched: failover never overwrites the choice.
    n += 1;
    if netbread_dns_preferred_rs() != PREF { fail |= 1 << 4; }

    // (c) a single answer resets the silence run.
    LOCAL_ANSWERED.store(0, Ordering::SeqCst);
    netbread_dns_set_preferred_rs(PREF);
    let _ = netbread_dns_note_timeout_rs(PREF);
    let _ = netbread_dns_note_timeout_rs(PREF);
    netbread_dns_note_answer_rs(0, PREF);
    let _ = netbread_dns_note_timeout_rs(PREF);
    n += 1;
    if netbread_dns_active_rs() != PREF { fail |= 1 << 5; }

    // (d) THE OWNER'S EXACT TOPOLOGY, AND THE REGRESSION TEST FOR THIS FIX.
    //
    // Windows ICS: the preferred resolver, the DHCP-offered resolver and the
    // gateway are ALL the same silent host. Every locally-endorsed candidate
    // equals the one already in use. The OLD ladder had literally nowhere to
    // go and reported zero failovers through forty-one unanswered queries on
    // real hardware; the new one must leave the network.
    LOCAL_ANSWERED.store(0, Ordering::SeqCst);
    netbread_dns_set_preferred_rs(DHCPS);
    netbread_dns_set_dhcp_rs(DHCPS);
    netbread_dns_set_gateway_rs(GWS);
    let esc = netbread_dns_note_timeout_rs(DHCPS);
    let esc = if esc != 0 { esc } else { netbread_dns_note_timeout_rs(DHCPS) };
    let esc = if esc != 0 { esc } else { netbread_dns_note_timeout_rs(DHCPS) };
    n += 1;
    if netbread_dns_is_public_rs(esc) != 1 { fail |= 1 << 6; }
    n += 1;
    if esc == DHCPS { fail |= 1 << 7; }

    // (d2) the active server is NEVER zeroed, however long the silence runs.
    // Getting this wrong turns "no better resolver" into "no network at all".
    for _ in 0..(DNS_FAILOVER_AFTER * (LADDER_LEN + 3)) {
        let _ = netbread_dns_note_timeout_rs(netbread_dns_active_rs());
    }
    n += 1;
    if netbread_dns_active_rs() == 0 { fail |= 1 << 8; }
    n += 1;
    if netbread_dns_preferred_rs() != DHCPS { fail |= 1 << 9; }

    // (d3) THE HEDGE GATE. On the owner's topology, with nothing having
    // answered, the hedge must offer a public resolver on the FIRST lookup:
    // that is what turns a twelve-second failure into a 400ms one, and it is
    // the difference between "the browser works" and "the browser eventually
    // works after I gave up".
    LOCAL_ANSWERED.store(0, Ordering::SeqCst);
    netbread_dns_set_preferred_rs(DHCPS);
    netbread_dns_set_dhcp_rs(DHCPS);
    netbread_dns_set_gateway_rs(GWS);
    n += 1;
    if netbread_dns_is_public_rs(netbread_dns_hedge_candidate_rs(DHCPS)) != 1 {
        fail |= 1 << 13;
    }
    // ...and on a network whose OWN resolver has answered, it must offer
    // nothing rather than spraying that user's queries at a third party.
    netbread_dns_note_answer_rs(0, DHCPS);
    n += 1;
    if netbread_dns_hedge_candidate_rs(DHCPS) != 0 { fail |= 1 << 14; }

    // (d4) REMEMBER WHAT WORKED, and remember only what actually replied.
    LOCAL_ANSWERED.store(0, Ordering::SeqCst);
    netbread_dns_set_preferred_rs(PREF);
    n += 1;
    if netbread_dns_learned_rs() != 0 { fail |= 1 << 15; }   // cleared by a new choice
    netbread_dns_note_answer_rs(0, PUB_A);
    n += 1;
    if netbread_dns_learned_rs() != PUB_A { fail |= 1 << 16; }
    // A PUBLIC answer must not set LOCAL_ANSWERED: the local resolver is still
    // unproven, so the hedge must stay armed.
    n += 1;
    if LOCAL_ANSWERED.load(Ordering::SeqCst) != 0 { fail |= 1 << 17; }
    // An explicit new choice forgets it. A learned resolver is runtime state;
    // if it could outlive the choice that replaced it, it would be a pin.
    netbread_dns_set_preferred_rs(PREF);
    n += 1;
    if netbread_dns_learned_rs() != 0 { fail |= 1 << 18; }
    n += 1;
    if STAGE.load(Ordering::SeqCst) != 0 { fail |= 1 << 19; }

    // (e) the duplicate suppressor: same triple twice = one line, and a changed
    // outcome is admitted again.
    FETCH_LOGGED.store(0, Ordering::SeqCst);
    FETCH_SUPPRESSED.store(0, Ordering::SeqCst);
    LAST_FETCH_KEY.store(0, Ordering::SeqCst);
    n += 1;
    if netbread_fetch_should_log_rs(0xABCD, -1, 0) != 1 { fail |= 1 << 20; }
    n += 1;
    if netbread_fetch_should_log_rs(0xABCD, -1, 0) != 0 { fail |= 1 << 21; }
    n += 1;
    if netbread_fetch_should_log_rs(0xABCD, 0, 200) != 1 { fail |= 1 << 22; }

    // (f) the budget is real: a long run of DISTINCT failures stops being
    // logged, and never exceeds FETCH_LOG_BUDGET lines in total.
    FETCH_LOGGED.store(0, Ordering::SeqCst);
    LAST_FETCH_KEY.store(0, Ordering::SeqCst);
    let mut allowed: u32 = 0;
    for i in 0..(FETCH_LOG_BUDGET * 3) {
        if netbread_fetch_should_log_rs(i, -1, 0) == 1 { allowed += 1; }
    }
    n += 1;
    if allowed != FETCH_LOG_BUDGET { fail |= 1 << 23; }

    // (g) the hash must actually distinguish and must stop at the NUL.
    let a = b"example.com\0";
    let b = b"example.net\0";
    n += 1;
    if netbread_strhash_rs(a.as_ptr(), 64) == netbread_strhash_rs(b.as_ptr(), 64) {
        fail |= 1 << 24;
    }
    n += 1;
    if netbread_strhash_rs(a.as_ptr(), 64) != netbread_strhash_rs(a.as_ptr(), 11) {
        fail |= 1 << 25;
    }

    // Restore.
    PREFERRED.store(s_pref, Ordering::SeqCst);
    DHCP_OFFERED.store(s_dhcp, Ordering::SeqCst);
    GATEWAY.store(s_gw, Ordering::SeqCst);
    ACTIVE.store(s_act, Ordering::SeqCst);
    N_SENT.store(s_sent, Ordering::SeqCst);
    N_OK.store(s_ok, Ordering::SeqCst);
    N_TIMEOUT.store(s_to, Ordering::SeqCst);
    N_RCODE.store(s_rc, Ordering::SeqCst);
    N_FAILOVER.store(s_fo, Ordering::SeqCst);
    CONSEC_SILENT.store(s_cs, Ordering::SeqCst);
    FETCH_LOGGED.store(s_fl, Ordering::SeqCst);
    FETCH_SUPPRESSED.store(s_fs, Ordering::SeqCst);
    LAST_FETCH_KEY.store(s_fk, Ordering::SeqCst);
    STAGE.store(s_st, Ordering::SeqCst);
    LEARNED.store(s_ln, Ordering::SeqCst);
    LOCAL_ANSWERED.store(s_la, Ordering::SeqCst);

    unsafe { if !checks.is_null() { *checks = n; } }
    fail
}
