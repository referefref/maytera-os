// rustkern/pipewr.rs - #111(a)(b): the write-side decision state machine for
// an anonymous pipe.
//
// NEW kernel logic, so Rust per the 2026-07-16 rule. There is no C twin to
// strangle and no -DRUST_* flag: before #111 the write side had no state
// machine at all. kernel/fs/pipe.c's pipe_write_fn() was five straight lines
// that wrote whatever happened to fit and returned that count, so the four
// situations this module distinguishes were all collapsed into one answer.
//
// ===========================================================================
// WHY THIS SEAM AND NOT THE WHOLE FUNCTION
// ---------------------------------------------------------------------------
// The rest of pipe_write_fn() is a ring-buffer copy under a uaccess_begin()/
// uaccess_end() SMAP bracket, plus a wait_event_interruptible() on a C
// wait_queue_head_t. Those are C macros with no Rust binding in this tree
// (rustkern/pollsys.rs reaches the wait path only through a hand-written C
// shim in proc/pollwait.c), and the read half of this very ring already uses
// the C macro directly. Moving the copy and the sleep would put one ring
// buffer's two halves in two languages with an FFI boundary running through
// the count/read_pos/write_pos invariant, for no safety gain.
//
// What IS worth having in Rust is the part with the bugs in it: deciding, from
// three facts, which of six things to do. The POSIX rules below are not
// intuitive, and getting one backwards is silent and expensive.
//
// ===========================================================================
// THE FOUR RULES THAT ARE EASY TO GET BACKWARDS
// ---------------------------------------------------------------------------
//  1. Reader gone, NOTHING written yet  -> SIGPIPE and -EPIPE. This is the
//     `yes | head -1` case: a producer that ignores its write result must be
//     stopped by the signal, or it loops forever. Pre-#111 this returned a
//     bare -1 with no signal, which is exactly why it looped forever.
//
//  2. Reader gone, SOME bytes ALREADY written -> return the SHORT COUNT, and
//     raise NOTHING. Those bytes really are in the ring and a reader that is
//     still draining will still see them. Reporting an error for a write that
//     partly succeeded would make the caller retry bytes it already sent.
//     This is the rule most likely to be written wrong, because the obvious
//     implementation checks `readers <= 0` once at the top and answers EPIPE
//     for both cases.
//
//  3. Interrupted, SOME bytes already written -> the short count, NOT -EINTR,
//     for the same reason as rule 2. Only an interrupt that got nowhere is
//     allowed to be reported as an interrupt.
//
//  4. Ring FULL and the reader is still alive -> BLOCK. Never a zero-byte
//     write. A write(2) that returns 0 for a non-zero count is not a legal
//     blocking-write result, and it is what made the libc flush loop in
//     userland/libc/stdio_file.c spin: MEASURED on golden 1993 at 4,616,023
//     zero-returns to move 256 KB, burning CPU at 97% of the rate of an
//     explicit busy loop.
//
// Rule 1 is checked BEFORE rule 3 deliberately: a dead reader is permanent and
// a pending signal is the more useful report, while an interrupt is transient.
//
// ===========================================================================
// KNOWN NON-GOAL, stated rather than half-done: PIPE_BUF ATOMICITY.
// ---------------------------------------------------------------------------
// POSIX requires a write of at most PIPE_BUF bytes to be all-or-nothing so
// that concurrent writers cannot interleave their records. This module does
// NOT implement that: it will hand back a partial copy whenever any space
// exists. Implementing it needs an exclusive-waiter discipline on the write
// queue (otherwise a small writer can be starved indefinitely by a large one),
// which is a second behaviour change #111 did not measure. A HALF-atomic pipe
// is worse than an honestly non-atomic one, because callers would start
// relying on it. Today the only multi-writer case in the tree is a shell
// pipeline stage, which has one writer per pipe.
// ===========================================================================

/// Copy `n` bytes from the caller's buffer into the ring, then ask again.
pub const PW_COPY: u32 = 0;
/// The ring is full and a reader is still alive: sleep on the write queue.
pub const PW_BLOCK: u32 = 1;
/// The whole request is in the ring. Return `n`.
pub const PW_DONE: u32 = 2;
/// Stopped early with `n` bytes already written. Return `n`, raise nothing.
pub const PW_SHORT: u32 = 3;
/// No reader and nothing written. Raise SIGPIPE, return -EPIPE.
pub const PW_EPIPE: u32 = 4;
/// Interrupted with nothing written. Return -EINTR.
pub const PW_EINTR: u32 = 5;

/// The answer. `n` is meaningful for PW_COPY (bytes to copy now), PW_DONE and
/// PW_SHORT (bytes to report to the caller); it is 0 for the rest.
#[repr(C)]
pub struct PipeWriteStep {
    pub action: u32,
    pub n: u32,
}

/// Decide the next step of a pipe write.
///
/// `requested`  total bytes the caller asked to write.
/// `done`       bytes already copied into the ring by this call.
/// `ring_size`  capacity of the ring in bytes (never 0 for a live pipe).
/// `ring_count` bytes currently occupying the ring.
/// `readers`    open read-end descriptions. <= 0 means the pipe is broken.
/// `intr`       non-zero once a wait returned WAIT_EINTR.
///
/// Pure: no allocation, no I/O, no kernel state. Every path returns.
#[no_mangle]
pub extern "C" fn pipe_write_step_rs(
    requested: u64,
    done: u64,
    ring_size: u32,
    ring_count: u32,
    readers: i32,
    intr: i32,
) -> PipeWriteStep {
    // Satisfied. Checked first so a completed write is never reclassified as
    // an error by a reader that closed after the last byte landed, which is a
    // perfectly ordinary race in a shell pipeline.
    if done >= requested {
        return PipeWriteStep { action: PW_DONE, n: clamp_u32(done) };
    }

    // Rules 1 and 2.
    if readers <= 0 {
        return if done > 0 {
            PipeWriteStep { action: PW_SHORT, n: clamp_u32(done) }
        } else {
            PipeWriteStep { action: PW_EPIPE, n: 0 }
        };
    }

    // Rule 3.
    if intr != 0 {
        return if done > 0 {
            PipeWriteStep { action: PW_SHORT, n: clamp_u32(done) }
        } else {
            PipeWriteStep { action: PW_EINTR, n: 0 }
        };
    }

    // Rule 4. saturating_sub, not `-`: a corrupted count greater than the ring
    // size would wrap in release mode and produce an enormous "free space",
    // turning a bookkeeping bug into a heap overwrite. Zero free space is the
    // safe reading of an impossible state, and it routes to BLOCK, not COPY.
    let free = ring_size.saturating_sub(ring_count);
    if free == 0 {
        return PipeWriteStep { action: PW_BLOCK, n: 0 };
    }

    let remaining = requested - done;
    let n = if remaining < free as u64 { remaining as u32 } else { free };
    PipeWriteStep { action: PW_COPY, n }
}

/// A pipe request is bounded by the ring size in practice, but `requested`
/// comes from a Ring-3 size_t, so saturate rather than truncate: a silently
/// wrapped byte count reported back to userland is a wrong answer that looks
/// like a right one.
fn clamp_u32(v: u64) -> u32 {
    if v > u32::MAX as u64 { u32::MAX } else { v as u32 }
}

// ===========================================================================
// SELF-TEST. Runs at boot next to the other rustkern checks, because a pure
// decision function is exactly the thing that can be wrong in a way no boot
// notices. Each case is one of the rules above, INCLUDING the two that the
// obvious implementation gets backwards.
// ===========================================================================

/// Returns 0 if every case holds, otherwise the 1-based number of the first
/// case that failed.
#[no_mangle]
pub extern "C" fn pipe_write_step_selftest_rs() -> u32 {
    // 1: ordinary partial write into an empty 64 KB ring.
    let s = pipe_write_step_rs(200_000, 0, 65536, 0, 1, 0);
    if s.action != PW_COPY || s.n != 65536 { return 1; }

    // 2: ring full, reader alive -> BLOCK, and NEVER a zero-byte COPY.
    let s = pipe_write_step_rs(200_000, 65536, 65536, 65536, 1, 0);
    if s.action != PW_BLOCK { return 2; }

    // 3: request satisfied.
    let s = pipe_write_step_rs(4096, 4096, 65536, 4096, 1, 0);
    if s.action != PW_DONE || s.n != 4096 { return 3; }

    // 4: RULE 1. No reader, nothing written -> EPIPE.
    let s = pipe_write_step_rs(4096, 0, 65536, 0, 0, 0);
    if s.action != PW_EPIPE { return 4; }

    // 5: RULE 2. No reader, but 100 bytes already landed -> SHORT(100), and
    // emphatically NOT EPIPE. This is the case the naive top-of-function
    // `if (readers <= 0) return -EPIPE;` gets wrong.
    let s = pipe_write_step_rs(4096, 100, 65536, 100, 0, 0);
    if s.action != PW_SHORT || s.n != 100 { return 5; }

    // 6: RULE 3. Interrupted with nothing written -> EINTR.
    let s = pipe_write_step_rs(4096, 0, 65536, 0, 1, 1);
    if s.action != PW_EINTR { return 6; }

    // 7: RULE 3, other half. Interrupted after 7 bytes -> SHORT(7).
    let s = pipe_write_step_rs(4096, 7, 65536, 7, 1, 1);
    if s.action != PW_SHORT || s.n != 7 { return 7; }

    // 8: a dead reader outranks an interrupt when nothing was written, so the
    // caller gets the signal rather than a retryable-looking EINTR.
    let s = pipe_write_step_rs(4096, 0, 65536, 0, 0, 1);
    if s.action != PW_EPIPE { return 8; }

    // 9: a completed write is DONE even with the reader gone (rule ordering).
    let s = pipe_write_step_rs(64, 64, 65536, 64, 0, 0);
    if s.action != PW_DONE || s.n != 64 { return 9; }

    // 10: the impossible-state guard. count > size must read as "no space" and
    // route to BLOCK, never to a COPY with a wrapped enormous length.
    let s = pipe_write_step_rs(4096, 0, 65536, 70000, 1, 0);
    if s.action != PW_BLOCK { return 10; }

    // 11: a zero-length request is satisfied immediately, so the C loop cannot
    // spin on it.
    let s = pipe_write_step_rs(0, 0, 65536, 0, 1, 0);
    if s.action != PW_DONE || s.n != 0 { return 11; }

    0
}
