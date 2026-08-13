// rustkern/pollsys.rs - POSIX poll(2) over the VFS fd layer (#745, local 82).
//
// NEW kernel code, so Rust per the 2026-07-16 rule. There is no C twin and no
// strangler flag: syscall 104 was declared in userland/libc/syscall.h and
// defined NOWHERE in the kernel, so every call fell into the dispatcher's
// default case, printed "[SYSCALL] Unknown syscall 104" and returned -1.
//
// ===========================================================================
// WHY THIS IS WORTH BUILDING AT ALL (the readiness primitive already existed)
// ---------------------------------------------------------------------------
// file_poll() (fs/vfs.c) has been the kernel's readiness primitive since the
// PTY work: sockets, PTY master, PTY slave, /dev/console and the Bluetooth HCI
// transport all implement ops->poll and return a POLL_IN/POLL_OUT/POLL_ERR/
// POLL_HUP bitmask. sys_sock_select() (net/socket.c) already drives it for
// select(). So poll() is not a new mechanism, it is the general entry point
// onto a mechanism that was already load-bearing but reachable only through a
// socket-shaped API.
//
// The kernel's POLL_* bits in fs/vfs.h are ALREADY the Linux poll(2) bit
// values (IN 0x01, OUT 0x04, ERR 0x08, HUP 0x10). That is not a coincidence
// this code relies on silently: poll_bits_match_rs() below asserts it at boot
// against the values C passes in, so a future edit to vfs.h that renumbers them
// goes loud instead of quietly returning the wrong revents.
//
// ===========================================================================
// HOW THE WAIT WORKS, AND EXACTLY WHAT IT DOES NOT DO
// ---------------------------------------------------------------------------
// #426 forbids a hand-rolled poll loop, so nothing here sleeps by itself. All
// blocking goes through ONE C shim, poll_wait_cond() in proc/pollwait.c, which
// is a four-line wrapper around the canonical wait_event_interruptible_deadline
// macro. The macro cannot be called from Rust (it is a statement-expression
// macro that must textually contain the condition), so the condition is passed
// in as a callback and the macro stays the single definition of what a wait IS.
// No waitq internals are duplicated here.
//
// THE HONEST LIMITATION, stated up front. A process can be parked on exactly
// ONE wait queue at a time: __wait_prepare() stores the entry in
// process_t::wait_entry, a single pointer, which signal delivery uses to find
// and kick the sleeper. Registering on N queues would leave N-1 of them
// invisible to that kick. So:
//
//   * If every fd being polled resolves to the SAME wait queue (the common
//     case: poll on one socket, or on a PTY pair, or on one pipe), we park on
//     that queue for the WHOLE timeout. The wake is exact, there is no
//     re-scan, and a byte arriving wakes us immediately.
//
//   * If the fds span DIFFERENT wait queues, we park on the first one for a
//     bounded 20ms slice and re-scan, exactly as sys_sock_select() has always
//     done. Readiness on the OTHER queues is then observed at up to 20ms
//     latency rather than immediately. This is a real cost and it is written
//     down rather than hidden: fixing it properly means moving the wait entry
//     list onto process_t so a waiter can sit on several queues at once, which
//     is a scheduler change and not this change.
//
// The 20ms slice is the same value sock_slice_ticks() uses, deliberately: two
// different slice constants for the same job is how this tree grows a second
// implementation of something, and that is its most repeated defect.
//
// ===========================================================================
// FILES WITH NO ops->poll
// ---------------------------------------------------------------------------
// file_poll() returns 0 ("nothing ready") when ops->poll is NULL. Taking that
// literally would make poll() block for the full timeout on a REGULAR FILE,
// which POSIX says is always ready for both reading and writing, and which is
// the single most common thing anyone will pass. So a file with no ops->poll is
// treated as a regular file: always ready. The one file kind for which that
// would have been a LIE, an anonymous pipe (empty pipe, not ready to read), had
// its ops->poll added in this same change rather than being left to fall into
// the default. usb_cdc_acm has no ops->poll and would report always-readable;
// it has no poll() callers today, and the correct fix is an ops->poll on that
// driver, not a special case here.
// ===========================================================================

use core::ffi::c_void;

// POSIX poll(2) event bits. Identical to fs/vfs.h POLL_* by construction; see
// poll_bits_match_rs().
const POLLIN: i16 = 0x001;
const POLLPRI: i16 = 0x002;
const POLLOUT: i16 = 0x004;
const POLLERR: i16 = 0x008;
const POLLHUP: i16 = 0x010;
const POLLNVAL: i16 = 0x020;

// Bits the kernel reports whether or not the caller asked for them (POSIX).
const POLL_ALWAYS: i16 = POLLERR | POLLHUP | POLLNVAL;

// A process holds at most MAX_FDS = 64 descriptors (proc/process.h), so a
// request for more than that cannot be satisfiable and is rejected rather than
// silently truncated. The array is a fixed kernel-stack buffer: 64 * 8 = 512
// bytes, which is safe on the kernel stack and needs no allocator.
const MAX_POLL_FDS: usize = 64;

const E_INVAL: i64 = -22;
const E_FAULT: i64 = -14;
const E_INTR: i64 = -4;

// waitq.h WAIT_* return codes.
const WAIT_EINTR: i32 = -4;

// Slice used when the polled fds span more than one wait queue. Milliseconds.
const POLL_SLICE_MS: u64 = 20;

/// The userland `struct pollfd`. Locked to 8 bytes against the C definition by
/// a _Static_assert in proc/pollwait.c.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct PollFd {
    pub fd: i32,
    pub events: i16,
    pub revents: i16,
}

extern "C" {
    // fs/vfs.c. Per-process fd table lookup; NULL when the fd is not a file_t
    // (a legacy FAT/ext2/SMB fd, or simply not open).
    fn fd_get(fd: i32) -> *mut c_void;
    // fs/vfs.c. Returns the ready POLL_* mask, or 0 when ops->poll is NULL.
    fn file_poll(f: *mut c_void, events: i32) -> i32;
    // fs/vfs.c (added with this change). 1 when this file kind implements
    // ops->poll at all, which is what distinguishes "asked, answered: nothing
    // ready" from "this file kind never answers".
    fn file_has_poll(f: *mut c_void) -> i32;
    // fs/vfs.c (added with this change). The wait queue that will be woken when
    // this file's readiness may have changed, or NULL when the file kind does
    // not publish one.
    fn file_poll_wq(f: *mut c_void, events: i32) -> *mut c_void;
    // proc/fdlayer.c (added with this change). 1 when `fd` names an open entry
    // in one of the legacy kernel-wide tables (FAT, ext2, SMB/NFS). Those are
    // all regular files.
    fn fd_legacy_is_open(fd: i32) -> i32;

    // security/validate.c
    fn copy_from_user(dest: *mut c_void, src: *const c_void, size: usize) -> i32;
    fn copy_to_user(dest: *mut c_void, src: *const c_void, size: usize) -> i32;

    // cpu/mono.c. The one shared real-elapsed-ms clock (#483/#499). NOT
    // timer_ticks, which counts ticks DELIVERED and arrives in bursts under
    // KVM, which is exactly how every tick-derived deadline in this tree
    // learned to expire instantly on a loaded host.
    fn sched_now_ms() -> u64;

    // proc/pollwait.c. wait_event_interruptible_deadline() with the condition
    // supplied as a callback. Returns WAIT_OK / WAIT_TIMEOUT / WAIT_EINTR.
    fn poll_wait_cond(
        wq: *mut c_void,
        deadline_ms: u64,
        ready: extern "C" fn(*mut c_void) -> i32,
        ctx: *mut c_void,
    ) -> i32;
    // proc/pollwait.c. The queue we park on when there is nothing better: a
    // module-local wait_queue_head_t that nothing wakes. Used only for
    // nfds == 0 (POSIX: poll() with no descriptors is a plain sleep) and for
    // a set of fds that publish no queue at all. In both cases the DEADLINE is
    // the wake, which is the correct semantics, not a workaround for a wake we
    // failed to arm.
    fn poll_idle_wq() -> *mut c_void;
}

/// Scan state, shared between the initial scan and the wait callback so both
/// use the same code. Lives on the caller's kernel stack.
struct Scan {
    fds: *mut PollFd,
    nfds: usize,
}

/// Compute revents for ONE descriptor. Pure given its inputs, which is what
/// makes it testable: poll_classify_rs() below is the same function reached
/// from the boot self-test with the C side stubbed out.
///
/// `state` says what the fd IS:
///   0 = not a valid descriptor        -> POLLNVAL
///   1 = a file_t that implements poll -> use `ready`
///   2 = a file_t or legacy fd with no poll implementation (regular file)
///       -> always ready for whatever the caller asked
fn classify(events: i16, state: u32, ready: i16) -> i16 {
    if state == 0 {
        return POLLNVAL;
    }
    if state == 2 {
        // Regular file: POSIX says always ready for read and for write. Report
        // only what was asked for; a caller that asked for neither gets 0 and
        // is not counted as ready, which is what POSIX means by an fd whose
        // events are all masked off.
        return events & (POLLIN | POLLPRI | POLLOUT);
    }
    // A real ops->poll answered. Requested bits are filtered by the request;
    // ERR/HUP/NVAL are reported whether asked for or not.
    (ready & (events | POLL_ALWAYS)) & !POLLNVAL
}

/// Run one full pass over the descriptor array, writing revents in place.
/// Returns the number of descriptors with a non-zero revents, which is exactly
/// poll(2)'s return value.
///
/// Idempotent: it derives revents purely from current kernel state and
/// overwrites, so the wait macro may evaluate it any number of times.
unsafe fn scan(s: &Scan) -> i32 {
    let mut count: i32 = 0;
    let mut i: usize = 0;
    while i < s.nfds {
        let p = unsafe { &mut *s.fds.add(i) };
        // POSIX: a negative fd is ignored and revents is cleared. This is the
        // documented way callers disable one slot of a persistent array, so it
        // must not be an error and must not be counted.
        if p.fd < 0 {
            p.revents = 0;
            i += 1;
            continue;
        }
        let f = unsafe { fd_get(p.fd) };
        let rev = if !f.is_null() {
            if unsafe { file_has_poll(f) } != 0 {
                let ready = unsafe { file_poll(f, (p.events | POLL_ALWAYS) as i32) } as i16;
                classify(p.events, 1, ready)
            } else {
                classify(p.events, 2, 0)
            }
        } else if unsafe { fd_legacy_is_open(p.fd) } != 0 {
            classify(p.events, 2, 0)
        } else {
            classify(p.events, 0, 0)
        };
        p.revents = rev;
        if rev != 0 {
            count += 1;
        }
        i += 1;
    }
    count
}

/// The wait condition handed to poll_wait_cond(). Returns non-zero once at
/// least one descriptor is ready, which is what makes the macro stop sleeping.
extern "C" fn scan_ready_cb(ctx: *mut c_void) -> i32 {
    if ctx.is_null() {
        return 1; // never sleep on a broken context
    }
    let s = unsafe { &*(ctx as *const Scan) };
    if unsafe { scan(s) } > 0 {
        1
    } else {
        0
    }
}

/// Choose the wait queue to park on, and say whether it covers EVERY polled fd.
///
/// Returns (queue, exclusive). `exclusive` is 1 when every descriptor that
/// publishes a queue publishes THIS one, so parking on it for the whole timeout
/// cannot miss a wake. 0 means the set spans several queues (or some fd
/// publishes none) and the caller must bound its sleep with a slice.
unsafe fn pick_wq(s: &Scan) -> (*mut c_void, i32) {
    let mut chosen: *mut c_void = core::ptr::null_mut();
    let mut exclusive: i32 = 1;
    let mut i: usize = 0;
    while i < s.nfds {
        let p = unsafe { &*s.fds.add(i) };
        if p.fd < 0 {
            i += 1;
            continue;
        }
        let f = unsafe { fd_get(p.fd) };
        let wq = if f.is_null() {
            core::ptr::null_mut()
        } else {
            unsafe { file_poll_wq(f, (p.events | POLL_ALWAYS) as i32) }
        };
        if wq.is_null() {
            // A descriptor with no queue can still become ready (a regular
            // file is ready NOW, so we would not be sleeping; a driver without
            // a published queue may change under us). Either way we cannot be
            // woken for it, so the sleep must be sliced.
            exclusive = 0;
        } else if chosen.is_null() {
            chosen = wq;
        } else if chosen != wq {
            exclusive = 0;
        }
        i += 1;
    }
    (chosen, exclusive)
}

/// poll(2).
///
/// `ufds`       user pointer to `nfds` struct pollfd. Validated by the #503
///              argtab descriptor before this is reached, and copied in and out
///              rather than dereferenced in place.
/// `nfds`       descriptor count. > MAX_POLL_FDS is EINVAL, not a truncation.
/// `timeout_ms` < 0 waits forever, 0 returns immediately, > 0 is a bound in
///              real milliseconds.
///
/// Returns the number of ready descriptors, 0 on timeout, or a negative errno.
#[no_mangle]
pub unsafe extern "C" fn sys_poll_rs(ufds: *mut c_void, nfds: u64, timeout_ms: i64) -> i64 {
    if nfds > MAX_POLL_FDS as u64 {
        return E_INVAL;
    }
    let n = nfds as usize;

    // Absolute deadline computed ONCE, before anything can sleep, so a
    // spurious wake cannot re-arm a fresh full timeout.
    let deadline: u64 = if timeout_ms < 0 {
        u64::MAX // waitq.h WAIT_DEADLINE_NEVER
    } else {
        unsafe { sched_now_ms() }.wrapping_add(timeout_ms as u64)
    };

    // nfds == 0 is a legal POSIX sleep. Honour the timeout through the wait
    // primitive rather than returning 0 immediately: a caller using poll(NULL,
    // 0, ms) as a sleep is entitled to sleep.
    if n == 0 {
        if timeout_ms != 0 {
            let rc = unsafe {
                poll_wait_cond(poll_idle_wq(), deadline, never_ready_cb, core::ptr::null_mut())
            };
            if rc == WAIT_EINTR {
                return E_INTR;
            }
        }
        return 0;
    }

    if ufds.is_null() {
        return E_FAULT;
    }

    let mut buf: [PollFd; MAX_POLL_FDS] = [PollFd { fd: -1, events: 0, revents: 0 }; MAX_POLL_FDS];
    let bytes = n * core::mem::size_of::<PollFd>();
    if unsafe { copy_from_user(buf.as_mut_ptr() as *mut c_void, ufds as *const c_void, bytes) } != 0
    {
        return E_FAULT;
    }

    let s = Scan { fds: buf.as_mut_ptr(), nfds: n };

    let mut count = unsafe { scan(&s) };
    if count == 0 && timeout_ms != 0 {
        let (wq, exclusive) = unsafe { pick_wq(&s) };
        let ctx = &s as *const Scan as *mut c_void;
        loop {
            let (park_on, slice_deadline) = if exclusive != 0 && !wq.is_null() {
                // Every fd is behind this one queue: sleep the whole timeout.
                (wq, deadline)
            } else {
                // Mixed set: bound the sleep so the fds behind the OTHER queues
                // are still observed, at slice granularity.
                let park = if wq.is_null() { unsafe { poll_idle_wq() } } else { wq };
                let slice = unsafe { sched_now_ms() }.wrapping_add(POLL_SLICE_MS);
                let d = if deadline != u64::MAX && (slice as i64).wrapping_sub(deadline as i64) > 0
                {
                    deadline
                } else {
                    slice
                };
                (park, d)
            };
            let rc = unsafe { poll_wait_cond(park_on, slice_deadline, scan_ready_cb, ctx) };
            count = unsafe { scan(&s) };
            if count > 0 {
                break;
            }
            if rc == WAIT_EINTR {
                return E_INTR;
            }
            // Overall deadline reached? Signed compare, so a deadline already
            // behind us cannot be read as "far in the future" via unsigned
            // underflow.
            if deadline != u64::MAX
                && (unsafe { sched_now_ms() } as i64).wrapping_sub(deadline as i64) >= 0
            {
                break;
            }
        }
    }

    if unsafe { copy_to_user(ufds, buf.as_ptr() as *const c_void, bytes) } != 0 {
        return E_FAULT;
    }
    count as i64
}

/// Condition for the nfds == 0 sleep: never ready, so the deadline is the only
/// way out. Deliberately a named function rather than a closure so it is
/// obvious at the call site that nothing can wake it early except a signal.
extern "C" fn never_ready_cb(_ctx: *mut c_void) -> i32 {
    0
}

// ===========================================================================
// SELF-TEST
// ---------------------------------------------------------------------------
// Proves the CLASSIFICATION, which is the part that decides what userland sees,
// at boot on the real build rather than merely compiling. The blocking path
// cannot be self-tested from here (it needs a live scheduler and a real file),
// and pretending otherwise would be exactly the "a guard that never fires and a
// guard that is absent look identical" trap. It is verified on a VM instead.
//
// Returns a bit mask of failures; 0 is a pass.
// ===========================================================================

/// Assert that the kernel's fs/vfs.h POLL_* bits still equal the POSIX poll(2)
/// bits this module reports to userland. C passes its own POLL_* in, so this
/// compares two independent definitions rather than one against itself.
#[no_mangle]
pub extern "C" fn poll_bits_match_rs(c_in: i32, c_out: i32, c_err: i32, c_hup: i32) -> u32 {
    let mut fails: u32 = 0;
    if c_in != POLLIN as i32 {
        fails |= 1 << 0;
    }
    if c_out != POLLOUT as i32 {
        fails |= 1 << 1;
    }
    if c_err != POLLERR as i32 {
        fails |= 1 << 2;
    }
    if c_hup != POLLHUP as i32 {
        fails |= 1 << 3;
    }
    fails
}

#[no_mangle]
pub extern "C" fn pollsys_selftest_rs() -> u32 {
    let mut fails: u32 = 0;

    // 1. An invalid fd is POLLNVAL, and POLLNVAL alone: it is not an error
    //    return from poll(), it is a per-descriptor report.
    if classify(POLLIN, 0, 0) != POLLNVAL {
        fails |= 1 << 0;
    }

    // 2. A regular file is ready for exactly what was asked for, and for
    //    nothing else. Asking for read only must NOT report writable.
    if classify(POLLIN, 2, 0) != POLLIN {
        fails |= 1 << 1;
    }
    if classify(POLLOUT, 2, 0) != POLLOUT {
        fails |= 1 << 2;
    }
    if classify(POLLIN | POLLOUT, 2, 0) != (POLLIN | POLLOUT) {
        fails |= 1 << 3;
    }
    // 3. A regular file with no requested events reports nothing, so it is not
    //    counted as ready. (POSIX: events==0 still reports ERR/HUP/NVAL, and a
    //    regular file has none of those.)
    if classify(0, 2, 0) != 0 {
        fails |= 1 << 4;
    }

    // 4. A driver's readiness is FILTERED by the request. A socket that is
    //    writable must not be reported to a caller that only asked to read;
    //    reporting it would make poll() return a descriptor the caller has no
    //    reason to act on, and every select()-shaped loop would spin.
    if classify(POLLIN, 1, POLLOUT) != 0 {
        fails |= 1 << 5;
    }
    if classify(POLLIN, 1, POLLIN | POLLOUT) != POLLIN {
        fails |= 1 << 6;
    }

    // 5. ERR and HUP are reported whether or not they were requested. This is
    //    the half of poll(2) that callers rely on to notice a closed peer.
    if classify(POLLIN, 1, POLLHUP) != POLLHUP {
        fails |= 1 << 7;
    }
    if classify(POLLOUT, 1, POLLERR) != POLLERR {
        fails |= 1 << 8;
    }
    if classify(0, 1, POLLHUP) != POLLHUP {
        fails |= 1 << 9;
    }

    // 6. A driver can never manufacture POLLNVAL: that bit means "the fd is
    //    not open", which only this module is in a position to decide.
    if classify(POLLIN, 1, POLLNVAL | POLLIN) != POLLIN {
        fails |= 1 << 10;
    }

    // 7. POLLPRI is a requestable bit on a regular file (always ready), which
    //    keeps the "ready for what you asked" rule uniform.
    if classify(POLLPRI, 2, 0) != POLLPRI {
        fails |= 1 << 11;
    }

    // 8. The struct the syscall copies in and out is 8 bytes. If this ever
    //    changes, the argtab length arithmetic (nfds * 8) is wrong and the
    //    validator would prove things about the wrong number of bytes.
    if core::mem::size_of::<PollFd>() != 8 {
        fails |= 1 << 12;
    }

    fails
}
