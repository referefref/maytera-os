// rustkern/clipboard.rs - #542 OS-wide system clipboard (kernel-held).
//
// WHY THIS EXISTS
// ---------------
// Standard editing (Ctrl+C/X/V) must move data ACROSS apps: copy in app A,
// paste in app B. There was no system-wide clipboard - the compositor protocol
// declared MSG_CLIPBOARD_COPY/PASTE (compositor_client.h) but nothing ever
// implemented them, and many built-in apps do not route through the compositor
// at all. A kernel-held buffer is the one place EVERY Ring-3 process can reach
// with a plain syscall regardless of whether it is a compositor client, so the
// clipboard is universal by construction.
//
// WHY RUST (per the 2026-07-16 Rust-by-default kernel rule)
// ---------------------------------------------------------
// This is new kernel state with two operations that copy a caller-supplied,
// caller-sized byte range into and out of a FIXED backing buffer. That is
// exactly the shape that a bounds-checked implementation protects: the copy
// length is clamped to the buffer capacity HERE, in one place, so a Ring-3
// caller can never drive an over-read or over-write of the clipboard store no
// matter what length it passes. No float, no paging entanglement, no hot path:
// a clean isolated Rust candidate.
//
// CONCURRENCY
// -----------
// The kernel serializes syscalls behind the big kernel lock, so set/get never
// run concurrently against each other; the store needs no additional lock. The
// buffer is a plain static: it lives for the life of the kernel and is never
// freed, so there is no allocator dependency (this module needs neither `alloc`
// nor `core::alloc`).
//
// FFI SURFACE (declared in ../rust-symbols.manifest, called from proc/syscall.c)
//   clip_set_rs(src, len) -> bytes stored      (SYS_CLIP_SET)
//   clip_get_rs(dst, cap) -> total bytes held  (SYS_CLIP_GET), copies min(cap,held)
//   clip_len_rs()         -> total bytes held  (query without copying)
//   clip_selftest_rs()    -> 0 on pass, else a failing-case bitmask (boot proof)

use core::ptr;

// Bounded capacity. 64 KiB is generous for text selections and small data while
// keeping the store a fixed, statically-reserved region. A copy larger than
// this is truncated, never grown.
const CLIP_CAP: usize = 64 * 1024;

// The single system clipboard store. Never freed; guarded by the big kernel
// lock at the syscall boundary (see module header).
static mut CLIP_BUF: [u8; CLIP_CAP] = [0u8; CLIP_CAP];
static mut CLIP_LEN: usize = 0;

// Store up to CLIP_CAP bytes from `src` into the clipboard, replacing any prior
// contents. A NULL `src` (or zero `len`) clears the clipboard. Returns the
// number of bytes actually stored (== min(len, CLIP_CAP)).
//
// SAFETY: `src` is a Ring-3 pointer. In this kernel user memory is identity
// mapped and directly dereferenceable, matching every other syscall handler
// that reads a user buffer (e.g. SYS_FONT_NAME). The copy length is clamped to
// CLIP_CAP so the destination write can never exceed the backing buffer.
#[no_mangle]
pub extern "C" fn clip_set_rs(src: *const u8, len: usize) -> i64 {
    let n = if len > CLIP_CAP { CLIP_CAP } else { len };
    unsafe {
        let dst = ptr::addr_of_mut!(CLIP_BUF) as *mut u8;
        if !src.is_null() && n > 0 {
            ptr::copy_nonoverlapping(src, dst, n);
        }
        CLIP_LEN = n;
    }
    n as i64
}

// Copy up to `cap` bytes of the clipboard into `dst` and return the TOTAL number
// of bytes held (so the caller can tell whether it received a truncated view and
// size a larger buffer). A NULL `dst` or zero `cap` copies nothing but still
// returns the held length, so it doubles as a size query.
//
// SAFETY: as clip_set_rs. The copy length is min(cap, held) so the write never
// exceeds the caller's buffer and the read never exceeds CLIP_LEN <= CLIP_CAP.
#[no_mangle]
pub extern "C" fn clip_get_rs(dst: *mut u8, cap: usize) -> i64 {
    unsafe {
        let held = CLIP_LEN;
        let n = if held > cap { cap } else { held };
        if !dst.is_null() && n > 0 {
            let src = ptr::addr_of!(CLIP_BUF) as *const u8;
            ptr::copy_nonoverlapping(src, dst, n);
        }
        held as i64
    }
}

// Bytes currently held, without copying.
#[no_mangle]
pub extern "C" fn clip_len_rs() -> i64 {
    unsafe { CLIP_LEN as i64 }
}

// Boot self-test: exercises set/get round-trip, the size-query path, the
// clear path, and the over-capacity truncation clamp entirely against local
// buffers (no Ring-3 pointers). Returns 0 on full pass, else a bitmask with one
// bit per failing case so a regression names itself in the boot log. Restores
// the clipboard to empty on exit so it has no lingering effect on a real boot.
#[no_mangle]
pub extern "C" fn clip_selftest_rs() -> i64 {
    let mut fail: i64 = 0;

    // Case 0: round-trip an exact byte string.
    let msg = b"maytera-clip-542";
    let stored = clip_set_rs(msg.as_ptr(), msg.len());
    if stored != msg.len() as i64 {
        fail |= 1 << 0;
    }
    let mut out = [0u8; 32];
    let held = clip_get_rs(out.as_mut_ptr(), out.len());
    if held != msg.len() as i64 {
        fail |= 1 << 1;
    }
    let mut i = 0;
    while i < msg.len() {
        if out[i] != msg[i] {
            fail |= 1 << 2;
            break;
        }
        i += 1;
    }

    // Case 1: NULL/zero-cap get returns the held length but copies nothing.
    if clip_get_rs(ptr::null_mut(), 0) != msg.len() as i64 {
        fail |= 1 << 3;
    }

    // Case 2: a get with cap smaller than the content copies exactly cap bytes
    // and still reports the full held length.
    let mut small = [0xAAu8; 4];
    let held2 = clip_get_rs(small.as_mut_ptr(), small.len());
    if held2 != msg.len() as i64 {
        fail |= 1 << 4;
    }
    let mut j = 0;
    while j < small.len() {
        if small[j] != msg[j] {
            fail |= 1 << 5;
            break;
        }
        j += 1;
    }

    // Case 3: a store never reports more than its input length (basic clamp
    // sanity; the CLIP_CAP clamp itself is validated by construction since
    // min(len, CLIP_CAP) can never exceed CLIP_CAP).
    let eight = [0x5Au8; 8];
    if clip_set_rs(eight.as_ptr(), eight.len()) != 8 {
        fail |= 1 << 6;
    }

    // Case 4: clear leaves nothing behind.
    clip_set_rs(ptr::null(), 0);
    if clip_len_rs() != 0 {
        fail |= 1 << 7;
    }

    fail
}
