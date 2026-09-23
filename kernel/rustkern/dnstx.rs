// rustkern/dnstx.rs - #httpdns: the DNS transaction table.
//
// THE DEFECT THIS EXISTS FOR, MEASURED 2026-09-03
// ==========================================================================
//
// net/dns.c held ONE in-flight query for the whole machine:
//
//     static struct { uint16_t pending_id; uint32_t result_ip; int result_code;
//                     int complete; char hostname[128]; } dns_query;
//
// with no lock, no ownership and no way for a caller to say "this answer is
// mine". Every resolver in the OS shared it: the six `httpfetch` worker
// threads that sys_http_fetch_start() spawns (one per async fetch, so a
// browser page or an App Store listing has several live at once), the sync
// https_get/wget_fetch path, SMB, NFS, SNTP, netfs, and the userland
// SYS_DNS_START / SYS_DNS_POLL pair.
//
// Two overlapping lookups therefore did this:
//
//   1. A builds a query for "example.com": pending_id = X, complete = 0.
//   2. B builds a query for "other.host":  pending_id = Y, complete = 0,
//      hostname overwritten. A's transaction no longer exists anywhere.
//   3. The reply for X arrives. `id != dns_query.pending_id`, so it is
//      DROPPED. A can never be answered, for its whole retry budget.
//   4. The reply for Y arrives and sets complete = 1. BOTH A and B wake, and
//      BOTH return dns_query.result_ip - which is B's address.
//
// So A connects to somebody else's server with A's Host header, and gets a
// 4xx, a wrong page, or a TLS certificate that does not match. From the
// outside this is "the browser and the App Store do not work" on a machine
// where `ping 1.1.1.1` is perfect, because ping does exactly one lookup at a
// time and never overlaps anything.
//
// MEASURED on VM 2977 (golden 2346, e1000, lab LAN, resolver 192.0.2.1
// answering in ~1 ms - i.e. the FRIENDLIEST possible case). Six concurrent
// fetches to six distinct hosts, driven by /APPS/DNSRACE:
//
//     CLOBBER      5   (each new lookup destroyed a live one)
//     id-mismatch  5   (replies dropped because the id had moved on)
//     CROSS-WIRE   8   (a caller returned another hostname's address)
//     result       2 of 6 fetches failed; one took 7 minutes to give up
//
// with, verbatim from that boot:
//
//     [DNSRACE] CROSS-WIRE: asked example.com, answer belongs to
//               detectportal.firefox.com
//
// The window is the round-trip time, so it gets WORSE, not better, on the
// machines that report the fault: a USB Ethernet dongle whose every send
// busy-polls the xHCI for up to 40 ms, and an ICS gateway's DNS proxy.
//
// WHAT THIS MODULE IS
// ==========================================================================
//
// A fixed table of DNS transactions. Each lookup owns a slot for its whole
// life; the transaction id is unique across every LIVE slot, so an incoming
// reply is delivered to the one lookup that asked for it, and to nobody else.
// A reply that matches no live slot is counted and dropped, which is the
// correct handling of a straggler from an abandoned query and is no longer
// indistinguishable from "somebody else stole my transaction".
//
// IT NEVER BLOCKS, NEVER ALLOCATES, NEVER CALLS OUT. dns_handle_response()
// runs from the UDP receive path, and dns_send() is reachable from #549
// no-block contexts, so this is static storage and atomics only: no wait, no
// sleep, no lock, no kmalloc, no I/O, no printing. The C side does all
// logging, in process context.
//
// WHY A TABLE AND NOT A LOCK. Serialising DNS behind a mutex would make every
// concurrent fetch wait out the one in front of it, turning a six-image page
// into six sequential round trips - and it would still need per-caller result
// storage to be correct. The table is both correct AND concurrent.
//
// SLOT EXHAUSTION IS AN HONEST FAILURE. With no free slot, dns_resolve()
// returns a soft failure and the caller retries, rather than silently
// clobbering a live transaction (which is exactly what the old code did every
// time). NSLOT is comfortably above the six async fetch workers plus the
// handful of sync kernel callers, and DNSTX_ALLOC_FAIL counts any occurrence
// so the number can never be a guess.

use core::sync::atomic::{AtomicI32, AtomicU32, AtomicU64, Ordering};
use core::cell::UnsafeCell;

// Six async fetch workers (ASYNC_FETCH_MAX) + the sync GET path + SNTP + SMB +
// NFS + netfs + one userland SYS_DNS_START per process. 16 leaves headroom for
// every one of those to be live simultaneously.
const NSLOT: usize = 16;
const HOSTMAX: usize = 128;

const ST_FREE: u32 = 0;
const ST_LIVE: u32 = 1;
const ST_DONE: u32 = 2;

struct HostBuf(UnsafeCell<[u8; HOSTMAX]>);
// SAFETY: a slot's host bytes are written only by the thread that owns the
// slot (between a successful ST_FREE->ST_LIVE claim and the store that
// publishes ST_DONE), and read only after that claim succeeded or after
// ST_DONE was observed. The atomic state word is the synchronisation edge.
unsafe impl Sync for HostBuf {}

const HOST_NEW: HostBuf = HostBuf(UnsafeCell::new([0u8; HOSTMAX]));
const AU32_0: AtomicU32 = AtomicU32::new(0);
const AI32_0: AtomicI32 = AtomicI32::new(0);
const AU64_0: AtomicU64 = AtomicU64::new(0);

static STATE: [AtomicU32; NSLOT] = [AU32_0; NSLOT];
static TXID: [AtomicU32; NSLOT] = [AU32_0; NSLOT];     // 16-bit id in the low bits
static OWNER: [AtomicU32; NSLOT] = [AU32_0; NSLOT];    // tgid for start/poll; 0 = kernel
static DEADLINE: [AtomicU64; NSLOT] = [AU64_0; NSLOT]; // ms; 0 = never reaped
static RESIP: [AtomicU32; NSLOT] = [AU32_0; NSLOT];
static RESRC: [AtomicI32; NSLOT] = [AI32_0; NSLOT];
static HLEN: [AtomicU32; NSLOT] = [AU32_0; NSLOT];
static HOSTS: [HostBuf; NSLOT] = [HOST_NEW; NSLOT];

// #netfix2's hedge bookkeeping, per TRANSACTION rather than per machine. A
// hedged lookup asks the SAME question of two resolvers at once and the answer
// is attributed to whoever replied first, which decides whether the configured
// resolver gets dropped for the rest of the boot. On the single global those
// three fields belonged to whichever lookup wrote them last, so a concurrent
// lookup could make that decision on another lookup's evidence.
static SRV0: [AtomicU32; NSLOT] = [AU32_0; NSLOT];
static SRV1: [AtomicU32; NSLOT] = [AU32_0; NSLOT];
static NSRV: [AtomicU32; NSLOT] = [AU32_0; NSLOT];
static ANSBY: [AtomicU32; NSLOT] = [AU32_0; NSLOT];

// Counters, surfaced on the ALREADY-DURABLE [NETDIAG] line so a machine with
// no serial port can still answer "did DNS run out of slots" and "how many
// replies arrived for a question nobody is still asking".
static PEAK: AtomicU32 = AtomicU32::new(0);
static ALLOC_FAIL: AtomicU32 = AtomicU32::new(0);
static NOMATCH: AtomicU32 = AtomicU32::new(0);
static REAPED: AtomicU32 = AtomicU32::new(0);

#[inline]
fn live_count() -> u32 {
    let mut n = 0u32;
    for s in STATE.iter() {
        if s.load(Ordering::Acquire) != ST_FREE {
            n += 1;
        }
    }
    n
}

// Is `id` already the transaction id of some LIVE slot? Ids must be unique
// across live slots or delivery is ambiguous again, which is the whole bug.
#[inline]
fn id_taken(id: u32) -> bool {
    for i in 0..NSLOT {
        if STATE[i].load(Ordering::Acquire) != ST_FREE && TXID[i].load(Ordering::Acquire) == id {
            return true;
        }
    }
    false
}

// Resolve an id collision that only a CONCURRENT allocation can create, and
// that SMP makes real rather than theoretical (AP user scheduling is the
// shipping default).
//
// id_taken() is read BEFORE this slot's id is published, so two threads
// claiming two different slots at the same instant can both see the same value
// as free and both take it. Two live slots with one id is the original defect
// back again, narrowed to a few instructions.
//
// The candidate must therefore be PUBLISHED first and checked second, and the
// tie is broken by SLOT INDEX: the higher-indexed slot yields. That asymmetry
// is what stops the two from ping-ponging onto each other's replacement, which
// is what "both re-roll on a clash" would do. Bounded by NSLOT, because each
// pass can only clash with a distinct live slot.
fn settle_id(i: usize, id_in: u32) -> u32 {
    let mut id = id_in;
    TXID[i].store(id, Ordering::Release);
    for _ in 0..NSLOT {
        let mut clash = false;
        for j in 0..NSLOT {
            if j >= i {
                continue;               // only a LOWER-indexed slot wins the tie
            }
            if STATE[j].load(Ordering::Acquire) != ST_FREE
                && TXID[j].load(Ordering::Acquire) == id
            {
                clash = true;
                break;
            }
        }
        if !clash {
            return id;
        }
        id = (id + 1) & 0xFFFF;
        if id == 0 {
            id = 1;
        }
        TXID[i].store(id, Ordering::Release);
    }
    id
}

// Release any slot whose deadline has passed. A userland process that calls
// SYS_DNS_START and then exits without ever polling would otherwise hold its
// slot forever; the kernel paths free explicitly, and their deadline is only a
// backstop. now_ms == 0 disables reaping (used by the self-test).
fn reap(now_ms: u64) {
    if now_ms == 0 {
        return;
    }
    for i in 0..NSLOT {
        let d = DEADLINE[i].load(Ordering::Acquire);
        if d != 0 && now_ms > d && STATE[i].load(Ordering::Acquire) != ST_FREE {
            DEADLINE[i].store(0, Ordering::Release);
            STATE[i].store(ST_FREE, Ordering::Release);
            REAPED.fetch_add(1, Ordering::SeqCst);
        }
    }
}

// Claim a slot for one lookup. `id_seed` supplies randomness for the
// transaction id (anti-spoofing); the id actually used is the first value from
// that seed that no live slot already holds, and is read back with
// dnstx_id_rs(). Returns the slot index, or -1 if the table is full.
#[no_mangle]
pub extern "C" fn dnstx_alloc_rs(
    host: *const u8,
    hlen: u32,
    id_seed: u32,
    owner: u32,
    now_ms: u64,
    ttl_ms: u32,
) -> i32 {
    reap(now_ms);
    for i in 0..NSLOT {
        if STATE[i]
            .compare_exchange(ST_FREE, ST_LIVE, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            // The slot is ours before anything else is written to it, so no
            // other claimant can ever observe a half-built record.
            let mut id = id_seed & 0xFFFF;
            if id == 0 {
                id = 1;
            }
            let mut tries = 0;
            while id_taken(id) && tries <= NSLOT {
                id = (id + 1) & 0xFFFF;
                if id == 0 {
                    id = 1;
                }
                tries += 1;
            }
            OWNER[i].store(owner, Ordering::Release);
            RESIP[i].store(0, Ordering::Release);
            RESRC[i].store(-1, Ordering::Release);
            SRV0[i].store(0, Ordering::Release);
            SRV1[i].store(0, Ordering::Release);
            NSRV[i].store(0, Ordering::Release);
            ANSBY[i].store(0, Ordering::Release);
            DEADLINE[i].store(
                if ttl_ms == 0 { 0 } else { now_ms + ttl_ms as u64 },
                Ordering::Release,
            );
            let n = if hlen as usize >= HOSTMAX { HOSTMAX - 1 } else { hlen as usize };
            // SAFETY: this slot is exclusively ours (the CAS above succeeded)
            // and `host` is a kernel buffer of at least `hlen` bytes.
            unsafe {
                let dst = &mut *HOSTS[i].0.get();
                if !host.is_null() {
                    for k in 0..n {
                        dst[k] = *host.add(k);
                    }
                }
                dst[n] = 0;
            }
            HLEN[i].store(n as u32, Ordering::Release);
            // PUBLISH THE ID LAST. settle_id() stores it, and the id is what
            // makes this slot findable by dnstx_match_rs(); everything a
            // matched reply reads (the hostname, the server list, the result
            // fields) is therefore already written by the time anything can
            // find it. There is no window in which a reply lands on a
            // half-built transaction.
            let _ = settle_id(i, id);
            let live = live_count();
            let _ = PEAK.fetch_update(Ordering::SeqCst, Ordering::SeqCst, |p| {
                if live > p { Some(live) } else { None }
            });
            return i as i32;
        }
    }
    ALLOC_FAIL.fetch_add(1, Ordering::SeqCst);
    -1
}

// The transaction id this slot must put on the wire.
#[no_mangle]
pub extern "C" fn dnstx_id_rs(slot: u32) -> u32 {
    if slot as usize >= NSLOT {
        return 0;
    }
    TXID[slot as usize].load(Ordering::Acquire)
}

// Give a live slot a FRESH transaction id and clear any completion. Used when
// the resolver fails over mid-lookup: a straggler from the server we just
// abandoned must not be able to answer the question we are now asking someone
// else. Returns the new id.
#[no_mangle]
pub extern "C" fn dnstx_rearm_rs(slot: u32, id_seed: u32, now_ms: u64, ttl_ms: u32) -> u32 {
    let i = slot as usize;
    if i >= NSLOT || STATE[i].load(Ordering::Acquire) == ST_FREE {
        return 0;
    }
    STATE[i].store(ST_LIVE, Ordering::Release);
    RESIP[i].store(0, Ordering::Release);
    RESRC[i].store(-1, Ordering::Release);
    // A fresh id is a fresh question, asked of nobody yet.
    SRV0[i].store(0, Ordering::Release);
    SRV1[i].store(0, Ordering::Release);
    NSRV[i].store(0, Ordering::Release);
    ANSBY[i].store(0, Ordering::Release);
    let mut id = id_seed & 0xFFFF;
    if id == 0 {
        id = 1;
    }
    let mut tries = 0;
    while id_taken(id) && tries <= NSLOT {
        id = (id + 1) & 0xFFFF;
        if id == 0 {
            id = 1;
        }
        tries += 1;
    }
    let id = settle_id(i, id);
    DEADLINE[i].store(
        if ttl_ms == 0 { 0 } else { now_ms + ttl_ms as u64 },
        Ordering::Release,
    );
    id
}

// Push a live slot's reap deadline out. Each retransmit is evidence the caller
// is still there.
#[no_mangle]
pub extern "C" fn dnstx_touch_rs(slot: u32, now_ms: u64, ttl_ms: u32) {
    let i = slot as usize;
    if i >= NSLOT || STATE[i].load(Ordering::Acquire) == ST_FREE {
        return;
    }
    DEADLINE[i].store(
        if ttl_ms == 0 { 0 } else { now_ms + ttl_ms as u64 },
        Ordering::Release,
    );
}

// Which lookup does this reply belong to? Only a LIVE slot can match: an
// already-completed slot has had its answer, and a second reply for it is a
// duplicate, not a new result. Returns the slot, or -1 (counted).
#[no_mangle]
pub extern "C" fn dnstx_match_rs(id: u32) -> i32 {
    let id = id & 0xFFFF;
    if id == 0 {
        // The retired-slot marker. No live transaction ever carries it.
        NOMATCH.fetch_add(1, Ordering::SeqCst);
        return -1;
    }
    for i in 0..NSLOT {
        if STATE[i].load(Ordering::Acquire) == ST_LIVE && TXID[i].load(Ordering::Acquire) == id {
            return i as i32;
        }
    }
    NOMATCH.fetch_add(1, Ordering::SeqCst);
    -1
}

// Copy the hostname this slot asked about. The response handler needs it to
// key the cache entry, and reading it from the SLOT rather than from a global
// is precisely what stops one lookup's answer being filed under another
// lookup's name.
#[no_mangle]
pub extern "C" fn dnstx_host_copy_rs(slot: u32, out: *mut u8, cap: u32) -> u32 {
    let i = slot as usize;
    if i >= NSLOT || out.is_null() || cap == 0 {
        return 0;
    }
    let n0 = HLEN[i].load(Ordering::Acquire) as usize;
    let n = if n0 >= cap as usize { cap as usize - 1 } else { n0 };
    // SAFETY: the host bytes of a claimed slot are stable for its lifetime,
    // and `out` has `cap` writable bytes.
    unsafe {
        let src = &*HOSTS[i].0.get();
        for k in 0..n {
            *out.add(k) = src[k];
        }
        *out.add(n) = 0;
    }
    n as u32
}

// Publish this lookup's answer. Written by the UDP receive path; the ST_DONE
// store is the release edge that makes ip/rcode visible to the waiter.
// Publish this lookup's answer, but ONLY if the slot is still the transaction
// that asked. Between dnstx_match_rs() and here, the waiting thread can time
// out, free the slot and let another lookup claim it; landing the answer then
// would hand one caller another caller's address and file it in the cache under
// the wrong name, which is the whole defect this module removes. Returns 1 if
// the answer landed, 0 if the transaction had gone.
#[no_mangle]
pub extern "C" fn dnstx_complete_rs(slot: u32, id: u32, rcode: i32, ip: u32) -> i32 {
    let i = slot as usize;
    let id = id & 0xFFFF;
    if i >= NSLOT || id == 0 {
        return 0;
    }
    if STATE[i].load(Ordering::Acquire) != ST_LIVE || TXID[i].load(Ordering::Acquire) != id {
        NOMATCH.fetch_add(1, Ordering::SeqCst);
        return 0;
    }
    RESIP[i].store(ip, Ordering::Release);
    RESRC[i].store(rcode, Ordering::Release);
    STATE[i].store(ST_DONE, Ordering::Release);
    1
}

#[no_mangle]
pub extern "C" fn dnstx_done_rs(slot: u32) -> i32 {
    let i = slot as usize;
    if i >= NSLOT {
        return 0;
    }
    if STATE[i].load(Ordering::Acquire) == ST_DONE { 1 } else { 0 }
}

// Read the answer. Returns the DNS result code (0 = an A record was found,
// negative = the resolver's rcode) and writes the address.
#[no_mangle]
pub extern "C" fn dnstx_result_rs(slot: u32, ip_out: *mut u32) -> i32 {
    let i = slot as usize;
    if i >= NSLOT {
        return -1;
    }
    if !ip_out.is_null() {
        // SAFETY: kernel out-parameter supplied by the caller.
        unsafe { *ip_out = RESIP[i].load(Ordering::Acquire) };
    }
    RESRC[i].load(Ordering::Acquire)
}

// Record a resolver this transaction's query was sent to. Up to two, the
// hedge's own limit; duplicates are ignored. Returns the count afterwards.
#[no_mangle]
pub extern "C" fn dnstx_note_server_rs(slot: u32, server: u32) -> u32 {
    let i = slot as usize;
    if i >= NSLOT || server == 0 {
        return 0;
    }
    let n = NSRV[i].load(Ordering::Acquire);
    if n == 0 {
        SRV0[i].store(server, Ordering::Release);
        NSRV[i].store(1, Ordering::Release);
        return 1;
    }
    if SRV0[i].load(Ordering::Acquire) == server {
        return n;
    }
    if n == 1 {
        SRV1[i].store(server, Ordering::Release);
        NSRV[i].store(2, Ordering::Release);
        return 2;
    }
    n
}

// The resolvers this transaction asked, for the one-per-boot [DNSSRC] note.
#[no_mangle]
pub extern "C" fn dnstx_servers_rs(slot: u32, s0: *mut u32, s1: *mut u32) -> u32 {
    let i = slot as usize;
    if i >= NSLOT {
        return 0;
    }
    // SAFETY: kernel out-parameters, each checked for null.
    unsafe {
        if !s0.is_null() { *s0 = SRV0[i].load(Ordering::Acquire); }
        if !s1.is_null() { *s1 = SRV1[i].load(Ordering::Acquire); }
    }
    NSRV[i].load(Ordering::Acquire)
}

// Who actually replied. Recorded only while the transaction is still live and
// still carries this id, for the same reason dnstx_complete_rs() re-checks it.
#[no_mangle]
pub extern "C" fn dnstx_note_answered_by_rs(slot: u32, id: u32, from: u32) {
    let i = slot as usize;
    let id = id & 0xFFFF;
    if i >= NSLOT || id == 0 {
        return;
    }
    if STATE[i].load(Ordering::Acquire) == ST_LIVE && TXID[i].load(Ordering::Acquire) == id {
        ANSBY[i].store(from, Ordering::Release);
    }
}

#[no_mangle]
pub extern "C" fn dnstx_answered_by_rs(slot: u32) -> u32 {
    let i = slot as usize;
    if i >= NSLOT {
        return 0;
    }
    ANSBY[i].load(Ordering::Acquire)
}

#[no_mangle]
pub extern "C" fn dnstx_free_rs(slot: u32) {
    let i = slot as usize;
    if i >= NSLOT {
        return;
    }
    // RETIRE THE ID FIRST. If a reply for this transaction is in flight right
    // now, it must find nothing rather than find whatever question the slot is
    // recycled to next. dnstx_match_rs() never matches id 0 and
    // dnstx_alloc_rs() never issues id 0, so 0 is the retired marker.
    TXID[i].store(0, Ordering::Release);
    DEADLINE[i].store(0, Ordering::Release);
    OWNER[i].store(0, Ordering::Release);
    STATE[i].store(ST_FREE, Ordering::Release);
}

// The slot belonging to a userland process, for the SYS_DNS_START /
// SYS_DNS_POLL pair, which has no handle in its ABI. owner 0 never matches, so
// a kernel-internal slot can never be claimed by a Ring-3 poll.
#[no_mangle]
pub extern "C" fn dnstx_owner_slot_rs(owner: u32) -> i32 {
    if owner == 0 {
        return -1;
    }
    for i in 0..NSLOT {
        if STATE[i].load(Ordering::Acquire) != ST_FREE && OWNER[i].load(Ordering::Acquire) == owner {
            return i as i32;
        }
    }
    -1
}

#[no_mangle]
pub extern "C" fn dnstx_stats_rs(
    live: *mut u32,
    peak: *mut u32,
    allocfail: *mut u32,
    nomatch: *mut u32,
    reaped: *mut u32,
) {
    // SAFETY: kernel out-parameters; each is checked for null.
    unsafe {
        if !live.is_null() { *live = live_count(); }
        if !peak.is_null() { *peak = PEAK.load(Ordering::SeqCst); }
        if !allocfail.is_null() { *allocfail = ALLOC_FAIL.load(Ordering::SeqCst); }
        if !nomatch.is_null() { *nomatch = NOMATCH.load(Ordering::SeqCst); }
        if !reaped.is_null() { *reaped = REAPED.load(Ordering::SeqCst); }
    }
}

// ---------------------------------------------------------------------------
// Boot self-test. Returns 0 on pass, otherwise a bitmask naming the check that
// failed. EVERY CHECK IS ONE THE OLD SINGLE-GLOBAL IMPLEMENTATION FAILS - that
// is the point of it. Runs before net_init(), restores the table it touched,
// and asserts the table is empty on the way out so it cannot leave a booby trap
// for the first real lookup.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn dnstx_selftest_rs(extra: *mut u32) -> u32 {
    let mut bad: u32 = 0;
    // A SECOND mask, because the first one is full. Bits are cheaper than
    // merging two unrelated checks into one and losing which of them failed.
    let mut bad2: u32 = 0;
    let save_peak = PEAK.load(Ordering::SeqCst);
    let save_af = ALLOC_FAIL.load(Ordering::SeqCst);
    let save_nm = NOMATCH.load(Ordering::SeqCst);
    let save_rp = REAPED.load(Ordering::SeqCst);

    let a = b"example.com";
    let b = b"other.host";

    // 1. Two lookups get DIFFERENT slots. (The old code had one.)
    let sa = dnstx_alloc_rs(a.as_ptr(), a.len() as u32, 0x1234, 0, 1000, 30_000);
    let sb = dnstx_alloc_rs(b.as_ptr(), b.len() as u32, 0x5678, 0, 1000, 30_000);
    if sa < 0 || sb < 0 || sa == sb { bad |= 1 << 0; }

    // 2. Each slot remembers ITS OWN hostname. (The old code kept one string,
    //    so B's query overwrote A's name and A's answer was filed under B.)
    let mut buf = [0u8; HOSTMAX];
    if sa >= 0 {
        let n = dnstx_host_copy_rs(sa as u32, buf.as_mut_ptr(), HOSTMAX as u32);
        if n as usize != a.len() || &buf[..a.len()] != &a[..] { bad |= 1 << 1; }
    }
    if sb >= 0 {
        let n = dnstx_host_copy_rs(sb as u32, buf.as_mut_ptr(), HOSTMAX as u32);
        if n as usize != b.len() || &buf[..b.len()] != &b[..] { bad |= 1 << 2; }
    }

    // 3. Transaction ids are UNIQUE across live slots, even from a colliding
    //    seed. Two live slots sharing an id would re-create the ambiguity.
    let sc = dnstx_alloc_rs(a.as_ptr(), a.len() as u32, 0x1234, 0, 1000, 30_000);
    if sc < 0 { bad |= 1 << 3; }
    if sa >= 0 && sc >= 0 && dnstx_id_rs(sa as u32) == dnstx_id_rs(sc as u32) { bad |= 1 << 4; }

    // 4. A reply is delivered to the slot that asked, and to no other. This is
    //    the whole defect: the old code delivered every reply to whichever
    //    query happened to be last.
    if sb >= 0 {
        let idb = dnstx_id_rs(sb as u32);
        if dnstx_match_rs(idb) != sb { bad |= 1 << 5; }
        let idb_now = dnstx_id_rs(sb as u32);
        if dnstx_complete_rs(sb as u32, idb_now, 0, 0x0A0B0C0D) != 1 { bad |= 1 << 20; }
        if dnstx_done_rs(sb as u32) != 1 { bad |= 1 << 6; }
        // A must be UNAFFECTED by B completing.
        if sa >= 0 && dnstx_done_rs(sa as u32) != 0 { bad |= 1 << 7; }
        let mut ip: u32 = 0;
        if dnstx_result_rs(sb as u32, &mut ip) != 0 || ip != 0x0A0B0C0D { bad |= 1 << 8; }
    }

    // 5. A reply for an id nobody is waiting on is dropped, not misdelivered.
    //    A completed slot no longer matches, so a duplicate cannot re-complete
    //    a slot that has been recycled to a different question.
    if sb >= 0 && dnstx_match_rs(dnstx_id_rs(sb as u32)) >= 0 { bad |= 1 << 9; }
    if dnstx_match_rs(0) >= 0 { bad |= 1 << 10; }

    // 5b. A FREED slot retires its id, and a late answer carrying that id
    //     cannot land on whatever question the slot is recycled to next.
    if sa >= 0 {
        let ida = dnstx_id_rs(sa as u32);
        dnstx_free_rs(sa as u32);
        if dnstx_match_rs(ida) >= 0 { bad |= 1 << 21; }
        let sre = dnstx_alloc_rs(b.as_ptr(), b.len() as u32, 0x4242, 0, 1000, 30_000);
        if sre < 0 { bad |= 1 << 22; }
        // the recycled slot must refuse the OLD transaction's answer
        if sre >= 0 && dnstx_complete_rs(sre as u32, ida, 0, 0xDEADBEEF) != 0 { bad |= 1 << 23; }
        if sre >= 0 && dnstx_done_rs(sre as u32) != 0 { bad |= 1 << 24; }
        if sre >= 0 { dnstx_free_rs(sre as u32); }
    }

    // 5c. Hedge bookkeeping is PER TRANSACTION. Two live lookups that each hedge
    //     to a different second resolver must not read each other's
    //     "who answered", which is what decides whether the configured resolver
    //     is dropped for the rest of the boot.
    {
        let h1 = dnstx_alloc_rs(a.as_ptr(), a.len() as u32, 0x1111, 0, 1000, 30_000);
        let h2 = dnstx_alloc_rs(b.as_ptr(), b.len() as u32, 0x2222, 0, 1000, 30_000);
        if h1 < 0 || h2 < 0 { bad2 |= 1 << 0; }
        if h1 >= 0 && h2 >= 0 {
            if dnstx_note_server_rs(h1 as u32, 0x0101_0101) != 1 { bad2 |= 1 << 1; }
            if dnstx_note_server_rs(h1 as u32, 0x0101_0101) != 1 { bad2 |= 1 << 2; } // dup
            if dnstx_note_server_rs(h1 as u32, 0x0202_0202) != 2 { bad2 |= 1 << 3; }
            if dnstx_note_server_rs(h2 as u32, 0x0303_0303) != 1 { bad2 |= 1 << 4; }
            dnstx_note_answered_by_rs(h1 as u32, dnstx_id_rs(h1 as u32), 0x0202_0202);
            if dnstx_answered_by_rs(h1 as u32) != 0x0202_0202 { bad2 |= 1 << 5; }
            if dnstx_answered_by_rs(h2 as u32) != 0 { bad2 |= 1 << 6; }
            dnstx_free_rs(h1 as u32);
            dnstx_free_rs(h2 as u32);
        }
    }

    // 5d. The concurrent-allocation tie-break. Two live slots cannot keep the
    //     same id, and it is the HIGHER-indexed one that moves, so two racing
    //     allocators converge instead of chasing each other. Simulated by
    //     planting the collision the race would produce.
    {
        let lo = dnstx_alloc_rs(a.as_ptr(), a.len() as u32, 0x7000, 0, 1000, 30_000);
        let hi = dnstx_alloc_rs(b.as_ptr(), b.len() as u32, 0x8000, 0, 1000, 30_000);
        if lo < 0 || hi < 0 || lo >= hi { bad2 |= 1 << 8; }   // alloc order is index order
        if lo >= 0 && hi >= 0 && lo < hi {
            let idlo = dnstx_id_rs(lo as u32);
            // The higher slot lands on the lower slot's id, which is exactly
            // what the unsynchronised window produces.
            let settled = settle_id(hi as usize, idlo);
            if settled == idlo { bad2 |= 1 << 9; }            // it must have moved
            if dnstx_id_rs(lo as u32) != idlo { bad2 |= 1 << 10; }  // the low slot must NOT
            if dnstx_id_rs(hi as u32) != settled { bad2 |= 1 << 11; }
            // and the reverse: the LOWER slot keeps its id against the higher one
            let idhi = dnstx_id_rs(hi as u32);
            if settle_id(lo as usize, idhi) != idhi { bad2 |= 1 << 12; }
            dnstx_free_rs(lo as u32);
            dnstx_free_rs(hi as u32);
        }
    }

    // 6. Owner lookup finds a Ring-3 slot and refuses owner 0 (kernel slots).
    let so = dnstx_alloc_rs(a.as_ptr(), a.len() as u32, 0x9ABC, 4242, 1000, 30_000);
    if so < 0 || dnstx_owner_slot_rs(4242) != so { bad |= 1 << 11; }
    if dnstx_owner_slot_rs(0) >= 0 { bad |= 1 << 12; }

    // 7. Rearm produces a DIFFERENT id and clears completion, so a straggler
    //    from an abandoned server cannot answer the re-asked question.
    if sb >= 0 {
        let old = dnstx_id_rs(sb as u32);
        let new = dnstx_rearm_rs(sb as u32, old.wrapping_add(1), 1000, 30_000);
        if new == old || new == 0 { bad |= 1 << 13; }
        if dnstx_done_rs(sb as u32) != 0 { bad |= 1 << 14; }
    }

    // 8. The reaper reclaims a slot whose deadline has passed, and only then.
    let sr = dnstx_alloc_rs(a.as_ptr(), a.len() as u32, 0xDEAD, 7777, 1000, 100);
    if sr < 0 { bad |= 1 << 15; }
    let before = REAPED.load(Ordering::SeqCst);
    reap(1050);                       // not yet due
    if REAPED.load(Ordering::SeqCst) != before { bad |= 1 << 16; }
    reap(2000);                       // due
    if REAPED.load(Ordering::SeqCst) == before { bad |= 1 << 17; }
    if sr >= 0 && STATE[sr as usize].load(Ordering::Acquire) != ST_FREE { bad |= 1 << 18; }

    // Put the table back exactly as it was found.
    for i in 0..NSLOT {
        DEADLINE[i].store(0, Ordering::Release);
        OWNER[i].store(0, Ordering::Release);
        TXID[i].store(0, Ordering::Release);
        HLEN[i].store(0, Ordering::Release);
        SRV0[i].store(0, Ordering::Release);
        SRV1[i].store(0, Ordering::Release);
        NSRV[i].store(0, Ordering::Release);
        ANSBY[i].store(0, Ordering::Release);
        STATE[i].store(ST_FREE, Ordering::Release);
    }
    if live_count() != 0 { bad |= 1 << 19; }
    PEAK.store(save_peak, Ordering::SeqCst);
    ALLOC_FAIL.store(save_af, Ordering::SeqCst);
    NOMATCH.store(save_nm, Ordering::SeqCst);
    REAPED.store(save_rp, Ordering::SeqCst);
    // SAFETY: kernel out-parameter, checked for null.
    unsafe { if !extra.is_null() { *extra = bad2; } }
    bad
}
