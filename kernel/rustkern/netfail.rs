// netfail.rs - WHY a network fetch failed, in one word, on a machine with no
// serial port.  (#netfix2, 2026-09-03)
//
// THE PROBLEM THIS EXISTS FOR
//
// The owner reports, repeatedly and on two different machines: "I can ping
// 1.1.1.1 but the browser and the App Store do not work."  Every time, the only
// artifact we get is /BOOTLOG.TXT off a USB stick, and every time it has been
// unable to answer the question, because the fetch path's ~140 diagnostics are
// kprintf(), which is SERIAL ONLY, and neither of his machines has a serial
// port.  #imacnet (2026-09-02) fixed half of that by adding one durable
// [NETFETCH] line at the fetch chokepoint.  It carries rc=, status= and phase=.
//
// phase= was the right idea and it is not enough, for two reasons that this
// module closes:
//
//   1. phase= IS EMPTY ON THE SYNC PATHS.  It is read from the calling thread's
//      published http_progress_t record, and only the ASYNC workers publish
//      one.  Every fetch that boot services (and the App Store's sync path) make
//      reports `phase=n/a-sync`, i.e. exactly nothing.  MEASURED: of the seven
//      call sites that reach net_fetch_report(), five are sync.
//
//   2. EVEN A PHASE IS NOT A CAUSE.  "TLS-handshake" is where it stopped, not
//      what went wrong; a wrong RTC, an untrusted CA, a hostname mismatch and an
//      unsupported cipher all stop in exactly that phase and need four
//      completely different actions from the user.  Distinguishing them is the
//      whole difference between a log that closes a report and a log that starts
//      another round trip.
//
// So each layer that can fail records a REASON here as it returns its error, and
// the fetch chokepoint prints it.  One extra field, `why=`, on a line that is
// already being written; no extra log budget; and phase= is DERIVED from the
// reason when no progress record exists, which fills in the sync-path hole for
// free.
//
// DESIGN CONSTRAINTS, all forced by where this gets called from
//
//   - NEVER BLOCKS, NEVER ALLOCATES, NEVER TAKES A LOCK.  Failure sites include
//     dns_send(), which #549 established is reachable from a no-block TX context
//     (interrupts off, net_lock held).  Static storage and atomics only, which
//     is also why there is no String anywhere in here.
//   - NEVER PRINTS.  A failing fetch can retry many times per second; a print at
//     the failure site is how a log turns into a flood and buries the line you
//     needed.  Recording is separate from reporting, and only the chokepoint
//     reports, under the existing #imacnet suppression budget.
//   - LAST WRITER WINS, PER THREAD.  The layers are nested (dns inside connect
//     inside fetch), so the INNERMOST, most specific reason is set first and the
//     outer layers must not overwrite it with something vaguer.  Outer layers
//     therefore use netfail_note_weak_rs(), which only fills an EMPTY slot.
//
// WHY IT IS KEYED BY PID.  Fetches run concurrently: the browser's async worker,
// the App Store's, and a widget's sync fetch can all be in flight.  A single
// global "last failure" would cross-attribute one thread's TLS failure to
// another thread's DNS failure, which is worse than no field at all because it
// is confidently wrong.  Eight slots, keyed by proc_current_pid(), is enough for
// every concurrent fetch this OS can have in flight and costs 128 bytes.

use core::sync::atomic::{AtomicI32, AtomicU32, Ordering};

// ---------------------------------------------------------------------------
// Reason codes.  These are a stable wire format in the sense that they appear in
// a log file a human reads, so the NAME is the contract, not the number.
// ---------------------------------------------------------------------------
pub const NF_NONE: u32 = 0;

// Before anything was attempted.
pub const NF_BAD_URL: u32 = 1;
pub const NF_NO_CARRIER: u32 = 2;
pub const NF_NO_ADDRESS: u32 = 3;
pub const NF_IF_FAULTY: u32 = 4;
pub const NF_OOM: u32 = 5;

// Name resolution.
pub const NF_DNS_NO_SERVER: u32 = 10;
pub const NF_DNS_LINK_DOWN: u32 = 11;
pub const NF_DNS_SILENT: u32 = 12;
pub const NF_DNS_NXDOMAIN: u32 = 13;
pub const NF_DNS_SERVFAIL: u32 = 14;
pub const NF_DNS_REFUSED: u32 = 15;
pub const NF_DNS_NO_A: u32 = 16;
pub const NF_DNS_BAD_NAME: u32 = 17;
pub const NF_DNS_NEG_CACHED: u32 = 18;

// Getting a TCP connection.
pub const NF_ARP_UNRESOLVED: u32 = 20;
pub const NF_SOCKET: u32 = 21;
pub const NF_TCP_REFUSED: u32 = 22;
pub const NF_TCP_TIMEOUT: u32 = 23;
pub const NF_TCP_FAILED: u32 = 24;

// TLS.  The certificate cases are split because they need different actions.
pub const NF_TLS_CTX: u32 = 30;
pub const NF_TLS_HANDSHAKE: u32 = 31;
pub const NF_TLS_CERT_EXPIRED: u32 = 32;
pub const NF_TLS_CERT_NOT_YET_VALID: u32 = 33;
pub const NF_TLS_CLOCK_WRONG: u32 = 34;
pub const NF_TLS_CERT_UNTRUSTED: u32 = 35;
pub const NF_TLS_CERT_SIGNATURE: u32 = 36;
pub const NF_TLS_CERT_NAME: u32 = 37;
pub const NF_TLS_CERT_BAD: u32 = 38;

// HTTP, once bytes are moving.
pub const NF_HTTP_SEND: u32 = 40;
pub const NF_HTTP_NO_STATUS: u32 = 41;
pub const NF_HTTP_STATUS: u32 = 42;
pub const NF_HTTP_TRUNCATED: u32 = 43;
pub const NF_HTTP_CHUNKED: u32 = 44;
pub const NF_HTTP_REDIRECT_BLOCKED: u32 = 45;
pub const NF_HTTP_RECV_TIMEOUT: u32 = 46;
pub const NF_HTTP_TOO_BIG: u32 = 47;

const NF_MAX: u32 = 49;

// http_progress.h phase values, mirrored so a reason can imply a phase for the
// sync fetch paths that publish no progress record.  _Static_assert-locked on
// the C side (see net/netfail.h).
const PH_IDLE: u32 = 0;
const PH_RESOLVING: u32 = 1;
const PH_CONNECTING: u32 = 2;
const PH_TLS: u32 = 3;
const PH_SENDING: u32 = 4;
const PH_RECEIVING: u32 = 5;

/// The name that appears in the log.  NUL-terminated so C can `%s` it directly;
/// there is no allocation and no formatting anywhere in this module.
///
/// These read as a diagnosis, not as an error enum, because the reader is the
/// person trying to work out what to do next.  "TLS-CLOCK-WRONG" tells him to
/// fix the clock; "handshake_failure" would not.
fn name_bytes(reason: u32) -> &'static [u8] {
    match reason {
        NF_BAD_URL => b"BAD-URL\0",
        NF_NO_CARRIER => b"NO-CARRIER-cable-or-wifi-down\0",
        NF_NO_ADDRESS => b"NO-IP-ADDRESS-dhcp-never-bound\0",
        NF_IF_FAULTY => b"INTERFACE-TRIPPED-549-breaker\0",
        NF_OOM => b"OUT-OF-MEMORY\0",

        NF_DNS_NO_SERVER => b"DNS-NO-RESOLVER-CONFIGURED\0",
        NF_DNS_LINK_DOWN => b"DNS-SKIPPED-LINK-DOWN\0",
        NF_DNS_SILENT => b"DNS-RESOLVER-ANSWERED-NOTHING\0",
        NF_DNS_NXDOMAIN => b"DNS-NXDOMAIN-no-such-host\0",
        NF_DNS_SERVFAIL => b"DNS-SERVFAIL-resolver-error\0",
        NF_DNS_REFUSED => b"DNS-REFUSED-resolver-said-no\0",
        NF_DNS_NO_A => b"DNS-NO-A-RECORD\0",
        NF_DNS_BAD_NAME => b"DNS-HOSTNAME-UNPARSEABLE\0",
        NF_DNS_NEG_CACHED => b"DNS-NEGATIVE-CACHED-earlier-failure\0",

        NF_ARP_UNRESOLVED => b"ARP-UNRESOLVED-host-not-on-lan\0",
        NF_SOCKET => b"NO-SOCKET\0",
        NF_TCP_REFUSED => b"TCP-REFUSED-or-RESET\0",
        NF_TCP_TIMEOUT => b"TCP-CONNECT-TIMEOUT\0",
        NF_TCP_FAILED => b"TCP-CONNECT-FAILED\0",

        NF_TLS_CTX => b"TLS-NO-CONTEXT\0",
        NF_TLS_HANDSHAKE => b"TLS-HANDSHAKE-FAILED\0",
        NF_TLS_CERT_EXPIRED => b"TLS-CERT-EXPIRED\0",
        NF_TLS_CERT_NOT_YET_VALID => b"TLS-CERT-NOT-YET-VALID\0",
        NF_TLS_CLOCK_WRONG => b"TLS-FAILED-BECAUSE-SYSTEM-CLOCK-IS-WRONG\0",
        NF_TLS_CERT_UNTRUSTED => b"TLS-CERT-NO-TRUSTED-CA\0",
        NF_TLS_CERT_SIGNATURE => b"TLS-CERT-BAD-SIGNATURE\0",
        NF_TLS_CERT_NAME => b"TLS-CERT-WRONG-HOSTNAME\0",
        NF_TLS_CERT_BAD => b"TLS-CERT-REJECTED\0",

        NF_HTTP_SEND => b"HTTP-REQUEST-SEND-FAILED\0",
        NF_HTTP_NO_STATUS => b"HTTP-NO-STATUS-LINE-333\0",
        NF_HTTP_STATUS => b"HTTP-ERROR-STATUS\0",
        NF_HTTP_TRUNCATED => b"HTTP-BODY-TRUNCATED\0",
        NF_HTTP_CHUNKED => b"HTTP-BAD-CHUNKED-FRAMING\0",
        NF_HTTP_REDIRECT_BLOCKED => b"HTTP-REDIRECT-BLOCKED-ssrf\0",
        NF_HTTP_RECV_TIMEOUT => b"HTTP-RECEIVE-TIMEOUT\0",
        NF_HTTP_TOO_BIG => b"HTTP-RESPONSE-TOO-LARGE\0",

        _ => b"UNRECORDED\0",
    }
}

/// The phase a reason implies.  Used ONLY when the calling thread published no
/// progress record, which is every sync fetch in the OS.
fn phase_of(reason: u32) -> u32 {
    match reason {
        NF_DNS_NO_SERVER | NF_DNS_LINK_DOWN | NF_DNS_SILENT | NF_DNS_NXDOMAIN
        | NF_DNS_SERVFAIL | NF_DNS_REFUSED | NF_DNS_NO_A | NF_DNS_BAD_NAME
        | NF_DNS_NEG_CACHED => PH_RESOLVING,

        NF_ARP_UNRESOLVED | NF_SOCKET | NF_TCP_REFUSED | NF_TCP_TIMEOUT | NF_TCP_FAILED => {
            PH_CONNECTING
        }

        NF_TLS_CTX
        | NF_TLS_HANDSHAKE
        | NF_TLS_CERT_EXPIRED
        | NF_TLS_CERT_NOT_YET_VALID
        | NF_TLS_CLOCK_WRONG
        | NF_TLS_CERT_UNTRUSTED
        | NF_TLS_CERT_SIGNATURE
        | NF_TLS_CERT_NAME
        | NF_TLS_CERT_BAD => PH_TLS,

        NF_HTTP_SEND => PH_SENDING,

        NF_HTTP_NO_STATUS
        | NF_HTTP_STATUS
        | NF_HTTP_TRUNCATED
        | NF_HTTP_CHUNKED
        | NF_HTTP_REDIRECT_BLOCKED
        | NF_HTTP_RECV_TIMEOUT
        | NF_HTTP_TOO_BIG => PH_RECEIVING,

        _ => PH_IDLE,
    }
}

// ---------------------------------------------------------------------------
// Per-thread slots.
// ---------------------------------------------------------------------------
const NSLOTS: usize = 8;

#[allow(clippy::declare_interior_mutable_const)]
const ZERO_U32: AtomicU32 = AtomicU32::new(0);
#[allow(clippy::declare_interior_mutable_const)]
const ZERO_I32: AtomicI32 = AtomicI32::new(0);

static SLOT_PID: [AtomicU32; NSLOTS] = [ZERO_U32; NSLOTS];
static SLOT_REASON: [AtomicU32; NSLOTS] = [ZERO_U32; NSLOTS];
static SLOT_DETAIL: [AtomicI32; NSLOTS] = [ZERO_I32; NSLOTS];
static SLOT_STAMP: [AtomicU32; NSLOTS] = [ZERO_U32; NSLOTS];

/// Monotonic, only ever compared for recency, so wrap is harmless.
static STAMP: AtomicU32 = AtomicU32::new(1);

/// Total reasons recorded, and how many were dropped for want of a slot.  Both
/// appear on [NETDIAG]; `dropped` being non-zero would mean this table is too
/// small, which is a thing to know rather than to guess about.
static N_SET: AtomicU32 = AtomicU32::new(0);
static N_DROP: AtomicU32 = AtomicU32::new(0);

fn find_slot(pid: u32) -> Option<usize> {
    (0..NSLOTS).find(|&i| SLOT_PID[i].load(Ordering::SeqCst) == pid
        && SLOT_REASON[i].load(Ordering::SeqCst) != NF_NONE)
}

fn claim_slot(pid: u32) -> Option<usize> {
    if let Some(i) = find_slot(pid) {
        return Some(i);
    }
    // A free slot: one that holds no live reason.
    for i in 0..NSLOTS {
        if SLOT_REASON[i].load(Ordering::SeqCst) == NF_NONE {
            SLOT_PID[i].store(pid, Ordering::SeqCst);
            return Some(i);
        }
    }
    // All eight hold an unread reason.  Evict the OLDEST: a reason nobody has
    // collected is stale by definition, and the newest failure is the one
    // somebody is about to ask about.
    let mut oldest = 0usize;
    let mut oldest_stamp = u32::MAX;
    for i in 0..NSLOTS {
        let s = SLOT_STAMP[i].load(Ordering::SeqCst);
        if s < oldest_stamp {
            oldest_stamp = s;
            oldest = i;
        }
    }
    N_DROP.fetch_add(1, Ordering::SeqCst);
    SLOT_PID[oldest].store(pid, Ordering::SeqCst);
    Some(oldest)
}

/// Record WHY this thread's network operation failed.  `detail` is a
/// reason-specific number (an HTTP status, a TLS alert, a cert error code, an
/// rcode); 0 when there is nothing useful to add.
///
/// STRONG: overwrites whatever was there.  Use this at the site that KNOWS, i.e.
/// the innermost layer.
///
/// # Safety
/// FFI entry point.  Touches only this module's statics.
#[no_mangle]
pub extern "C" fn netfail_note_rs(pid: u32, reason: u32, detail: i32) {
    if reason == NF_NONE || reason >= NF_MAX {
        return;
    }
    if let Some(i) = claim_slot(pid) {
        SLOT_REASON[i].store(reason, Ordering::SeqCst);
        SLOT_DETAIL[i].store(detail, Ordering::SeqCst);
        SLOT_STAMP[i].store(STAMP.fetch_add(1, Ordering::SeqCst), Ordering::SeqCst);
        N_SET.fetch_add(1, Ordering::SeqCst);
    }
}

/// WEAK: record this reason ONLY if nothing more specific has already been
/// recorded for this thread.
///
/// This is the whole reason the module has two setters.  The layers nest:
/// https_get() -> https_connect() -> dns_resolve().  DNS knows the resolver went
/// silent; by the time the failure has propagated back to https_get() all it
/// knows is "connect returned -1".  If the outer layer overwrote the inner one,
/// every DNS fault in the OS would be logged as TCP-CONNECT-FAILED, which is not
/// merely less useful, it points the reader at the wrong subsystem.  So outer
/// layers fill in only when nobody underneath spoke up.
///
/// # Safety
/// FFI entry point.  Touches only this module's statics.
#[no_mangle]
pub extern "C" fn netfail_note_weak_rs(pid: u32, reason: u32, detail: i32) {
    if find_slot(pid).is_some() {
        return;
    }
    netfail_note_rs(pid, reason, detail);
}

/// Read and CLEAR this thread's reason.  Returns NF_NONE if none was recorded.
/// `detail_out` may be null.
///
/// Clearing on read is deliberate: a stale reason attached to the NEXT fetch
/// would be a confidently wrong log line, and this module's whole value is that
/// its answers can be trusted.
///
/// # Safety
/// FFI entry point.  `detail_out` must be null or a writable *mut i32.
#[no_mangle]
pub unsafe extern "C" fn netfail_take_rs(pid: u32, detail_out: *mut i32) -> u32 {
    match find_slot(pid) {
        None => {
            if !detail_out.is_null() {
                core::ptr::write(detail_out, 0);
            }
            NF_NONE
        }
        Some(i) => {
            let r = SLOT_REASON[i].load(Ordering::SeqCst);
            let d = SLOT_DETAIL[i].load(Ordering::SeqCst);
            SLOT_REASON[i].store(NF_NONE, Ordering::SeqCst);
            SLOT_DETAIL[i].store(0, Ordering::SeqCst);
            SLOT_PID[i].store(0, Ordering::SeqCst);
            SLOT_STAMP[i].store(0, Ordering::SeqCst);
            if !detail_out.is_null() {
                core::ptr::write(detail_out, d);
            }
            r
        }
    }
}

/// Drop this thread's reason without reading it.  Called when a fetch SUCCEEDS,
/// so a recovered-from failure (a retry that worked, a redirect that resolved)
/// cannot leak onto a later line.
///
/// # Safety
/// FFI entry point.
#[no_mangle]
pub extern "C" fn netfail_clear_rs(pid: u32) {
    if let Some(i) = find_slot(pid) {
        SLOT_REASON[i].store(NF_NONE, Ordering::SeqCst);
        SLOT_DETAIL[i].store(0, Ordering::SeqCst);
        SLOT_PID[i].store(0, Ordering::SeqCst);
        SLOT_STAMP[i].store(0, Ordering::SeqCst);
    }
}

/// The log name for a reason.  Always a valid NUL-terminated pointer.
///
/// # Safety
/// FFI entry point.  The returned pointer is to a 'static and is never freed.
#[no_mangle]
pub extern "C" fn netfail_name_rs(reason: u32) -> *const u8 {
    name_bytes(reason).as_ptr()
}

/// The http_progress phase a reason implies, for fetches that published none.
#[no_mangle]
pub extern "C" fn netfail_phase_rs(reason: u32) -> u32 {
    phase_of(reason)
}

/// Counters for [NETDIAG].  Either pointer may be null.
///
/// # Safety
/// FFI entry point.  Each pointer must be null or a writable *mut u32.
#[no_mangle]
pub unsafe extern "C" fn netfail_counters_rs(set: *mut u32, dropped: *mut u32) {
    if !set.is_null() {
        core::ptr::write(set, N_SET.load(Ordering::SeqCst));
    }
    if !dropped.is_null() {
        core::ptr::write(dropped, N_DROP.load(Ordering::SeqCst));
    }
}


// ===========================================================================
// SYSTEM-CLOCK PLAUSIBILITY, and the sticky per-owner reason userland reads.
// ===========================================================================
//
// WHY A CLOCK CHECK LIVES IN THE NETWORK FAILURE MODULE.  A wrong real-time
// clock is, from the user's chair, a NETWORK fault: every HTTPS page fails,
// every app that fetches fails, and ping is perfect, because ICMP has no
// certificates and no notion of "now".  It is the same observable symptom as an
// unreachable resolver and it needs a completely different fix, so it has to be
// distinguishable in the one artifact we get from the owner's machines.
//
// THE TEST IS SOUND, NOT A HEURISTIC.  This kernel cannot be running before the
// day it was compiled.  So an RTC reading EARLIER than the build date is not
// "suspicious", it is definitively wrong, with no false positives available.
// The upper bound is a heuristic and is set deliberately loose (10 years) so
// that a machine legitimately left running for a long time is never accused.
//
// IT DOES NOT CLAMP OR BYPASS ANYTHING.  Certificate validation still uses the
// real RTC.  Silently substituting a different clock would mean an OS that
// quietly decides for itself when "now" is, which is a security property nobody
// asked us to change.  This only supplies the WORD for the log.
static BUILD_Y: AtomicU32 = AtomicU32::new(0);
static BUILD_M: AtomicU32 = AtomicU32::new(0);
static BUILD_D: AtomicU32 = AtomicU32::new(0);

const CLOCK_OK: u32 = 0;
const CLOCK_BEFORE_BUILD: u32 = 1;
const CLOCK_ABSURDLY_AHEAD: u32 = 2;
const CLOCK_UNKNOWN: u32 = 3;

/// Record the date this kernel was compiled.  Called once at boot from C,
/// because __DATE__ is a C preprocessor construct that Rust cannot see.
#[no_mangle]
pub extern "C" fn netfail_clock_set_build_rs(y: u32, m: u32, d: u32) {
    BUILD_Y.store(y, Ordering::SeqCst);
    BUILD_M.store(m, Ordering::SeqCst);
    BUILD_D.store(d, Ordering::SeqCst);
}

/// CLOCK_OK / CLOCK_BEFORE_BUILD / CLOCK_ABSURDLY_AHEAD / CLOCK_UNKNOWN.
#[no_mangle]
pub extern "C" fn netfail_clock_check_rs(y: u32, m: u32, d: u32) -> u32 {
    let by = BUILD_Y.load(Ordering::SeqCst);
    if by == 0 || y == 0 {
        return CLOCK_UNKNOWN;
    }
    let bm = BUILD_M.load(Ordering::SeqCst);
    let bd = BUILD_D.load(Ordering::SeqCst);
    let now = (y as u64) * 10000 + (m as u64) * 100 + (d as u64);
    let built = (by as u64) * 10000 + (bm as u64) * 100 + (bd as u64);
    if now < built {
        return CLOCK_BEFORE_BUILD;
    }
    if y > by + 10 {
        return CLOCK_ABSURDLY_AHEAD;
    }
    CLOCK_OK
}

// ---------------------------------------------------------------------------
// The PUBLISHED reason: what the last fetch belonging to a given owner failed
// with, kept until the next one replaces it.
//
// This is separate from the per-thread slots above and it has to be, for a
// reason that is easy to get wrong: the per-thread slot is CLEARED when the
// fetch chokepoint reads it, and the process that wants to SHOW the message
// (the browser, the App Store) asks later, from a different thread, after the
// worker has exited.  Publishing under the OWNER's thread-group id is what lets
// "Fetch failed" finally become a sentence.
// ---------------------------------------------------------------------------
static PUB_OWNER: [AtomicU32; NSLOTS] = [ZERO_U32; NSLOTS];
static PUB_REASON: [AtomicU32; NSLOTS] = [ZERO_U32; NSLOTS];
static PUB_DETAIL: [AtomicI32; NSLOTS] = [ZERO_I32; NSLOTS];
static PUB_STAMP: [AtomicU32; NSLOTS] = [ZERO_U32; NSLOTS];

/// Publish (or clear, with reason NF_NONE) the outcome of a fetch for `owner`.
#[no_mangle]
pub extern "C" fn netfail_publish_rs(owner: u32, reason: u32, detail: i32) {
    if owner == 0 {
        return;
    }
    for i in 0..NSLOTS {
        if PUB_OWNER[i].load(Ordering::SeqCst) == owner {
            PUB_REASON[i].store(reason, Ordering::SeqCst);
            PUB_DETAIL[i].store(detail, Ordering::SeqCst);
            PUB_STAMP[i].store(STAMP.fetch_add(1, Ordering::SeqCst), Ordering::SeqCst);
            return;
        }
    }
    if reason == NF_NONE {
        return; // nothing to say and no slot yet: do not consume one
    }
    let mut victim = 0usize;
    let mut oldest = u32::MAX;
    for i in 0..NSLOTS {
        if PUB_OWNER[i].load(Ordering::SeqCst) == 0 {
            victim = i;
            oldest = 0;
            break;
        }
        let s = PUB_STAMP[i].load(Ordering::SeqCst);
        if s < oldest {
            oldest = s;
            victim = i;
        }
    }
    PUB_OWNER[victim].store(owner, Ordering::SeqCst);
    PUB_REASON[victim].store(reason, Ordering::SeqCst);
    PUB_DETAIL[victim].store(detail, Ordering::SeqCst);
    PUB_STAMP[victim].store(STAMP.fetch_add(1, Ordering::SeqCst), Ordering::SeqCst);
}

/// What `owner`'s last fetch failed with.  Does NOT clear: the same process may
/// ask twice (once to set the status bar, once to draw an error page).
///
/// # Safety
/// FFI entry point.  `detail_out` must be null or writable.
#[no_mangle]
pub unsafe extern "C" fn netfail_published_rs(owner: u32, detail_out: *mut i32) -> u32 {
    for i in 0..NSLOTS {
        if PUB_OWNER[i].load(Ordering::SeqCst) == owner {
            if !detail_out.is_null() {
                core::ptr::write(detail_out, PUB_DETAIL[i].load(Ordering::SeqCst));
            }
            return PUB_REASON[i].load(Ordering::SeqCst);
        }
    }
    if !detail_out.is_null() {
        core::ptr::write(detail_out, 0);
    }
    NF_NONE
}

// ---------------------------------------------------------------------------
// Self-test.  Every check here is one that a plausible WRONG implementation
// fails; a test that only the correct implementation can fail is not worth
// running at boot.  Returns a bitmask of failed checks, 0 = PASS.
// ---------------------------------------------------------------------------

/// # Safety
/// FFI entry point.  Saves and restores every static it touches, so it is safe
/// to run before net_init() and leaves no residue.
#[no_mangle]
pub unsafe extern "C" fn netfail_selftest_rs(checks_out: *mut u32) -> u32 {
    // Save.
    let mut sp = [0u32; NSLOTS];
    let mut sr = [0u32; NSLOTS];
    let mut sd = [0i32; NSLOTS];
    let mut ss = [0u32; NSLOTS];
    for i in 0..NSLOTS {
        sp[i] = SLOT_PID[i].load(Ordering::SeqCst);
        sr[i] = SLOT_REASON[i].load(Ordering::SeqCst);
        sd[i] = SLOT_DETAIL[i].load(Ordering::SeqCst);
        ss[i] = SLOT_STAMP[i].load(Ordering::SeqCst);
        SLOT_PID[i].store(0, Ordering::SeqCst);
        SLOT_REASON[i].store(NF_NONE, Ordering::SeqCst);
        SLOT_DETAIL[i].store(0, Ordering::SeqCst);
        SLOT_STAMP[i].store(0, Ordering::SeqCst);
    }
    let s_set = N_SET.load(Ordering::SeqCst);
    let s_drop = N_DROP.load(Ordering::SeqCst);

    let mut fail: u32 = 0;
    let mut n: u32 = 0;
    let mut d: i32 = 0;
    macro_rules! ck {
        ($cond:expr) => {{
            if !($cond) {
                fail |= 1 << n;
            }
            n += 1;
        }};
    }

    // 1. Nothing recorded reads back as NF_NONE with detail 0.
    d = 99;
    ck!(netfail_take_rs(7, &mut d) == NF_NONE && d == 0);

    // 2. A strong note round-trips reason AND detail.
    netfail_note_rs(7, NF_TLS_CERT_EXPIRED, -3);
    ck!(netfail_take_rs(7, &mut d) == NF_TLS_CERT_EXPIRED && d == -3);

    // 3. TAKE CLEARS.  A stale reason on the next fetch is the failure mode that
    //    makes this whole module untrustworthy, so it is asserted, not assumed.
    ck!(netfail_take_rs(7, &mut d) == NF_NONE);

    // 4. WEAK does not overwrite a specific inner reason.  This is the one that
    //    the obvious implementation (one setter) gets wrong, and getting it
    //    wrong logs every DNS fault in the OS as a TCP failure.
    netfail_note_rs(7, NF_DNS_SILENT, 0);
    netfail_note_weak_rs(7, NF_TCP_FAILED, 0);
    ck!(netfail_take_rs(7, &mut d) == NF_DNS_SILENT);

    // 5. WEAK DOES fill an empty slot, otherwise outer layers say nothing at all.
    netfail_note_weak_rs(7, NF_TCP_FAILED, -5);
    ck!(netfail_take_rs(7, &mut d) == NF_TCP_FAILED && d == -5);

    // 6. STRONG does overwrite.  (The inner layer is allowed to refine itself.)
    netfail_note_rs(7, NF_TLS_HANDSHAKE, 0);
    netfail_note_rs(7, NF_TLS_CLOCK_WRONG, 2000);
    ck!(netfail_take_rs(7, &mut d) == NF_TLS_CLOCK_WRONG && d == 2000);

    // 7. THREADS DO NOT CROSS-ATTRIBUTE.  Two pids, two reasons, each reads back
    //    its own.  A single global slot passes checks 1-6 and fails this one,
    //    which is exactly why it is here.
    netfail_note_rs(11, NF_DNS_NXDOMAIN, 3);
    netfail_note_rs(12, NF_TCP_REFUSED, 0);
    let a = netfail_take_rs(11, &mut d);
    let b = netfail_take_rs(12, core::ptr::null_mut());
    ck!(a == NF_DNS_NXDOMAIN && d == 3 && b == NF_TCP_REFUSED);

    // 8. clear() removes without reading.
    netfail_note_rs(7, NF_HTTP_STATUS, 404);
    netfail_clear_rs(7);
    ck!(netfail_take_rs(7, &mut d) == NF_NONE);

    // 9. Reason 0 and an out-of-range reason are both refused, so a caller
    //    passing an uninitialised variable cannot manufacture a diagnosis.
    netfail_note_rs(7, NF_NONE, 1);
    netfail_note_rs(7, NF_MAX + 100, 1);
    ck!(netfail_take_rs(7, &mut d) == NF_NONE);

    // 10. Overflowing the table evicts the OLDEST and keeps the NEWEST.  Fill all
    //     eight, then add a ninth; slot for pid 100 (the first) must be gone and
    //     pid 108 (the newest) must be present.
    for p in 100..108u32 {
        netfail_note_rs(p, NF_TCP_TIMEOUT, p as i32);
    }
    netfail_note_rs(108, NF_DNS_SILENT, 0);
    let evicted = netfail_take_rs(100, core::ptr::null_mut());
    let newest = netfail_take_rs(108, core::ptr::null_mut());
    ck!(evicted == NF_NONE && newest == NF_DNS_SILENT);
    for p in 101..108u32 {
        netfail_clear_rs(p);
    }

    // 11. EVERY REASON THAT IS ACTUALLY USED HAS A REAL NAME.
    //
    //     The first version of this check walked 1..NF_MAX and `continue`d on
    //     UNRECORDED, so it could not fail: the numbering has intentional gaps
    //     between the groups, and skipping them skipped the only thing worth
    //     asserting.  A self-test that cannot go red is worse than no self-test,
    //     because its green is read as evidence.
    //
    //     So the used reasons are listed explicitly.  Adding a constant to
    //     net/netfail.h and forgetting a name here makes this check RED rather
    //     than making every log line for that fault read "UNRECORDED", which
    //     looks like "we never instrumented this" and is a lie.
    {
        const USED: [u32; 34] = [
            NF_BAD_URL, NF_NO_CARRIER, NF_NO_ADDRESS, NF_IF_FAULTY, NF_OOM,
            NF_DNS_NO_SERVER, NF_DNS_LINK_DOWN, NF_DNS_SILENT, NF_DNS_NXDOMAIN,
            NF_DNS_SERVFAIL, NF_DNS_REFUSED, NF_DNS_NO_A, NF_DNS_BAD_NAME,
            NF_DNS_NEG_CACHED,
            NF_ARP_UNRESOLVED, NF_SOCKET, NF_TCP_REFUSED, NF_TCP_TIMEOUT,
            NF_TCP_FAILED,
            NF_TLS_CTX, NF_TLS_HANDSHAKE, NF_TLS_CERT_EXPIRED,
            NF_TLS_CERT_NOT_YET_VALID, NF_TLS_CLOCK_WRONG, NF_TLS_CERT_UNTRUSTED,
            NF_TLS_CERT_SIGNATURE, NF_TLS_CERT_NAME, NF_TLS_CERT_BAD,
            NF_HTTP_SEND, NF_HTTP_NO_STATUS, NF_HTTP_STATUS, NF_HTTP_TRUNCATED,
            NF_HTTP_CHUNKED, NF_HTTP_REDIRECT_BLOCKED,
        ];
        let mut named = true;
        for &r in USED.iter() {
            let nb = name_bytes(r);
            if nb == b"UNRECORDED\0" || nb.is_empty() || nb[nb.len() - 1] != 0 {
                named = false;
            }
            // And a name must imply a phase other than IDLE, or the sync-path
            // fill-in this module exists for silently produces nothing.
            if r >= NF_DNS_NO_SERVER && phase_of(r) == PH_IDLE {
                named = false;
            }
        }
        ck!(named);
        // The reverse direction: an UNUSED number still answers safely rather
        // than returning a dangling or empty pointer.
        ck!(name_bytes(NF_MAX + 500) == b"UNRECORDED\0");
    }

    // 12. Names are NUL-terminated, which C's %s depends on absolutely.
    ck!(name_bytes(NF_TLS_CLOCK_WRONG).last() == Some(&0u8));
    ck!(name_bytes(9999).last() == Some(&0u8));

    // 13. The reason -> phase map is not the identity and not a constant.  A
    //     phase_of() that returned IDLE for everything would satisfy "it
    //     compiles" and silently restore the n/a-sync hole this module exists to
    //     close, so assert three different reasons give three different phases.
    ck!(phase_of(NF_DNS_SILENT) == PH_RESOLVING
        && phase_of(NF_TCP_TIMEOUT) == PH_CONNECTING
        && phase_of(NF_TLS_CLOCK_WRONG) == PH_TLS
        && phase_of(NF_HTTP_NO_STATUS) == PH_RECEIVING
        && phase_of(NF_NONE) == PH_IDLE);

    // 14. The clock verdict is SOUND in the one direction that matters: a date
    //     before the build date is always wrong, a date after it is not.  A
    //     check that accused every clock, or no clock, would compile fine and
    //     be worthless, so both directions are asserted.
    {
        let sby = BUILD_Y.load(Ordering::SeqCst);
        let sbm = BUILD_M.load(Ordering::SeqCst);
        let sbd = BUILD_D.load(Ordering::SeqCst);
        netfail_clock_set_build_rs(2026, 9, 3);
        ck!(netfail_clock_check_rs(2000, 1, 1) == CLOCK_BEFORE_BUILD);
        ck!(netfail_clock_check_rs(2026, 9, 2) == CLOCK_BEFORE_BUILD);
        ck!(netfail_clock_check_rs(2026, 9, 3) == CLOCK_OK);
        ck!(netfail_clock_check_rs(2027, 1, 1) == CLOCK_OK);
        ck!(netfail_clock_check_rs(2099, 1, 1) == CLOCK_ABSURDLY_AHEAD);
        // With no build date recorded the answer is UNKNOWN, never a false
        // accusation.  An early-boot caller must not be told the clock is wrong
        // merely because we have not been told when we were built.
        netfail_clock_set_build_rs(0, 0, 0);
        ck!(netfail_clock_check_rs(2000, 1, 1) == CLOCK_UNKNOWN);
        netfail_clock_set_build_rs(sby, sbm, sbd);
    }

    // 15. A published reason SURVIVES being read (the browser asks twice) and is
    //     per-owner.  The per-thread slots clear on read; if publishing shared
    //     that behaviour the second reader would get "no error" and the error
    //     page would be blank, which is the exact bug this exists to fix.
    {
        let mut spo = [0u32; NSLOTS];
        let mut spr = [0u32; NSLOTS];
        for i in 0..NSLOTS {
            spo[i] = PUB_OWNER[i].load(Ordering::SeqCst);
            spr[i] = PUB_REASON[i].load(Ordering::SeqCst);
            PUB_OWNER[i].store(0, Ordering::SeqCst);
            PUB_REASON[i].store(NF_NONE, Ordering::SeqCst);
        }
        netfail_publish_rs(42, NF_TLS_CLOCK_WRONG, 2000);
        netfail_publish_rs(43, NF_DNS_SILENT, 0);
        let r1 = netfail_published_rs(42, &mut d);
        let r2 = netfail_published_rs(42, core::ptr::null_mut());
        let r3 = netfail_published_rs(43, core::ptr::null_mut());
        ck!(r1 == NF_TLS_CLOCK_WRONG && d == 2000 && r2 == NF_TLS_CLOCK_WRONG
            && r3 == NF_DNS_SILENT);
        // A success clears it, so a working page does not keep showing the last
        // error.
        netfail_publish_rs(42, NF_NONE, 0);
        ck!(netfail_published_rs(42, core::ptr::null_mut()) == NF_NONE);
        // owner 0 ("no process context") is refused rather than treated as a
        // wildcard that every caller would then match.
        netfail_publish_rs(0, NF_DNS_SILENT, 0);
        ck!(netfail_published_rs(0, core::ptr::null_mut()) == NF_NONE);
        for i in 0..NSLOTS {
            PUB_OWNER[i].store(spo[i], Ordering::SeqCst);
            PUB_REASON[i].store(spr[i], Ordering::SeqCst);
        }
    }

    // Restore.
    for i in 0..NSLOTS {
        SLOT_PID[i].store(sp[i], Ordering::SeqCst);
        SLOT_REASON[i].store(sr[i], Ordering::SeqCst);
        SLOT_DETAIL[i].store(sd[i], Ordering::SeqCst);
        SLOT_STAMP[i].store(ss[i], Ordering::SeqCst);
    }
    N_SET.store(s_set, Ordering::SeqCst);
    N_DROP.store(s_drop, Ordering::SeqCst);

    if !checks_out.is_null() {
        core::ptr::write(checks_out, n);
    }
    fail
}
