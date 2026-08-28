// rustkern/mprotect.rs - SYS_MPROTECT Ring-3 input validation (#404 / #526).
//
// NEW kernel code, so Rust per the 2026-07-16 rule. This is NOT a port: there
// is no C twin and no -DRUST_* strangler flag, because there was never any C
// here to strangle. do_mprotect() (mm/demand.c, #522) has existed with ZERO
// callers since it was written; what was missing was the syscall and, with it,
// every check that makes a Ring-3-reachable memory-permission call safe.
//
// WHY THE VALIDATION IS IN RUST AND NOT C. This is a privilege boundary: Ring 3
// names an address and a length, and the kernel then rewrites page tables for
// that range. The load-bearing part is therefore the ARITHMETIC that decides
// which pages are in range, and that is exactly the part C wraps silently.
// `addr + len` overflowing to a small number turns "the whole address space"
// into "one page and a pass". Every bound below is a checked_add that returns a
// distinct refusal on overflow rather than a wrapped value. This tree has
// already shipped that class twice (MAYTERA-SEC-2026-0014 total_length-ihl
// underflow; #489 p_filesz underflow), and argtab.rs was written for the same
// reason. The bounds living in the types is the point, not decoration.
//
// WHY EVERY REFUSAL HAS ITS OWN CODE. "A guard that never fires and a guard
// that is absent look identical." A single -1 for all refusals cannot tell a
// test which check ran, so a test cannot prove any individual check is alive.
// The codes below are distinct and are asserted one-by-one by /APPS/MMTEST
// subtest (8), so each guard is proven to fire for its OWN reason.

// Mirrors security/validate.h (USER_SPACE_*) and mm/vmm.h (page size). Rust
// cannot see a C #define, so these are duplicated constants, and a duplicated
// constant that drifts is a hole: a USER_SPACE_END that grew in C but not here
// would reject legal calls, and one that SHRANK in C but not here would accept
// a range the kernel no longer considers user memory. Locked against drift by
// _Static_assert in proc/syscall_mprotect_lock.c - change either side and the
// build fails in the file that says what to do about it.
const USER_SPACE_START: u64 = 0x0000_0000_0040_0000; // 4MB
const USER_SPACE_END: u64 = 0x0000_7FFF_FFFF_FFFF; // top of the user half
const PAGE_SIZE: u64 = 4096;
const PAGE_MASK: u64 = PAGE_SIZE - 1;

// Protection bits, as mm/demand.c's do_mprotect() decodes them into VMA_*.
const PROT_READ: u32 = 0x1;
const PROT_WRITE: u32 = 0x2;
const PROT_EXEC: u32 = 0x4;
const PROT_KNOWN: u32 = PROT_READ | PROT_WRITE | PROT_EXEC;

// Refusal codes. 0 accepts; every refusal is negative and DISTINCT.
// Kept in sync with the MP_* block in kernel/proc/syscall.h.
pub const MP_OK: i32 = 0;
pub const MP_E_PROT_BITS: i32 = -1; // prot carries bits that are not PROT_*
pub const MP_E_LEN: i32 = -2; // zero length
pub const MP_E_ALIGN: i32 = -3; // addr is not page-aligned
pub const MP_E_WX: i32 = -4; // W^X: PROT_WRITE|PROT_EXEC refused
pub const MP_E_OVERFLOW: i32 = -5; // addr+len wraps the address space
pub const MP_E_RANGE: i32 = -6; // range is not entirely user memory

/// Validate the Ring-3 arguments of mprotect(addr, len, prot).
///
/// Returns MP_OK (0) if the request is well-formed and confined to the user
/// half, or one of the distinct negative MP_E_* codes above. It answers ONLY
/// the question "is this request well-formed and is it aimed at user memory".
/// It deliberately does NOT decide whether the range is actually MAPPED: that
/// requires the mm lock and the VMA list, it is do_mprotect()'s job, and doing
/// it here would be a check-then-use race against a sibling thread's munmap.
///
/// No pointer is dereferenced here, so there is no `unsafe` block in this file
/// and nothing for SMAP to trap: `addr` is an address to be looked up in a page
/// table, never memory to be read.
#[no_mangle]
pub extern "C" fn mprotect_validate_rs(addr: u64, len: u64, prot: u32) -> i32 {
    // Unknown protection bits are REJECTED, not masked off. do_mprotect()
    // stores the raw `prot` into vma->prot, so silently accepting junk bits
    // would persist them in the VMA where a later reader would have to guess
    // what they meant.
    if (prot & !PROT_KNOWN) != 0 {
        return MP_E_PROT_BITS;
    }

    if len == 0 {
        return MP_E_LEN;
    }

    if (addr & PAGE_MASK) != 0 {
        return MP_E_ALIGN;
    }

    // W^X. A brand-new syscall has no existing callers to break, so refusing
    // write+execute here costs nothing today and closes the obvious route to
    // making a page writable, writing shellcode, and making it executable.
    // STATED PLAINLY because it is a deliberate divergence from POSIX, which
    // permits PROT_WRITE|PROT_EXEC: if this kernel ever grows a JIT, this is
    // the single line to revisit, and the JIT must then use a
    // write-then-reprotect sequence rather than a W+X mapping.
    if (prot & PROT_WRITE) != 0 && (prot & PROT_EXEC) != 0 {
        return MP_E_WX;
    }

    // The range must not wrap. checked_add is the whole reason this function is
    // in Rust: in C, `addr + len` here silently produces a small number for a
    // large len and every bound below it then passes.
    let end = match addr.checked_add(len) {
        Some(e) => e,
        None => return MP_E_OVERFLOW,
    };

    // Round the end up to a page boundary EXACTLY as do_mprotect() does, and
    // refuse if THAT addition wraps. Checking only addr+len would miss a len
    // chosen so that the rounding is what overflows.
    let end = match end.checked_add(PAGE_MASK) {
        Some(e) => e & !PAGE_MASK,
        None => return MP_E_OVERFLOW,
    };

    if end <= addr {
        return MP_E_OVERFLOW;
    }

    // Confine to the user half. This is what rejects a kernel address: the
    // kernel lives above USER_SPACE_END, and the identity-mapped low memory
    // below 4MB is not the caller's to reprotect either.
    if addr < USER_SPACE_START || end > USER_SPACE_END {
        return MP_E_RANGE;
    }

    MP_OK
}
