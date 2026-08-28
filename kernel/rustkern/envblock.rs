// rustkern/envblock.rs - #112: the initial-process-stack layout policy, and
// the rules an environment entry has to satisfy to be put on it.
//
// New kernel logic (there is no C twin to strangle), so Rust per the
// 2026-07-16 rule. The RAW STORES stay in C, in proc/process.c's
// setup_user_stack(), because they run inside a foreign-CR3 window with
// interrupts masked and SMAP AC set, and duplicating that machinery in Rust
// would fork a primitive rather than reuse one. The ARITHMETIC and the
// ACCEPT/REJECT DECISION live here, which is the fsperm.rs / spawnid.rs split.
//
// ===========================================================================
// WHY THE ARITHMETIC IS THE PART THAT MATTERS
// ---------------------------------------------------------------------------
// The old setup_user_argv() computed its layout like this:
//
//     uint64_t str_area = (stack_top - total_str) & ~0x7ULL;
//     uint64_t ptrs_needed = 1 + argc + 1 + 1;
//     uint64_t sp = (str_area - ptrs_needed * 8) & ~0xFULL;
//
// with argc clamped to 64 and each string's length taken from the KERNEL copy,
// so it could not run away. An environment block cannot be bounded that
// casually: its size is chosen by Ring 3, entry by entry, and `total_str`
// there is a plain sum. Every length computation below is checked_add /
// checked_mul returning a refusal on overflow rather than a wrapped value,
// because a wrapped total is exactly how a 16KB block becomes an 8-byte
// reservation and the strings then run off the end of the child's stack into
// whatever is mapped below it.
//
// This tree has shipped that class twice already (MAYTERA-SEC-2026-0014
// total_length-ihl underflow, #489 p_filesz underflow), which is why the
// bounds live in the types here and not in a comment.
//
// ===========================================================================
// WHAT AN ENVIRONMENT ENTRY MUST LOOK LIKE
// ---------------------------------------------------------------------------
// "NAME=VALUE": at least one byte of NAME, then '=', then anything (including
// nothing). An entry with no '=' is REFUSED rather than passed through,
// because getenv() in every libc scans for '=' and an entry without one is
// invisible to it: it would occupy a slot, count against the budget, and never
// be readable. Silently carrying it is the "plausible answer to a different
// question" failure this ticket's own /APPS/ENV rewrite was about.
//
// A leading '=' is refused for the same reason (empty name, unfindable).
// Duplicate names are NOT refused: POSIX leaves the winner unspecified and
// every libc takes the first, which is what libc/stdlib.c's env_find() does.
// Refusing them would break a caller that legitimately layers an override on
// top of an inherited block.
// ===========================================================================

// Caps. These are POLICY, not architecture: the child's stack is 2 MB
// (USER_STACK_SIZE), so the block could be far bigger. It is deliberately not,
// because a spawn is a Ring-3-triggered kernel allocation and the honest size
// for "the environment a shell exports" is kilobytes.
//
// ENV_MAX_ENTRIES is 64 to match the argc clamp spawn_impl() already applies.
// ENV_MAX_ENTRY is 512 so a PATH with a dozen elements fits; the argv cap next
// to it is 256, and an environment value is legitimately longer than an
// argument. ENV_MAX_TOTAL is the one that actually bounds the kernel copy.
pub const ENV_MAX_ENTRIES: u32 = 64;
pub const ENV_MAX_ENTRY: usize = 512;
pub const ENV_MAX_TOTAL: u64 = 16384;

// The initial stack must not eat more than this much of USER_STACK_SIZE. The
// child's own stack has to live below it; a block that consumed the stack
// would produce a process that faults on its first push, which is a far more
// confusing failure than a refused spawn.
pub const STACK_INIT_BUDGET: u64 = 128 * 1024;

// Refusal codes, returned negative to C.
pub const E_OVERFLOW: i32 = -1; // a length computation would wrap
pub const E_TOOBIG: i32 = -2; // over ENV_MAX_TOTAL or STACK_INIT_BUDGET
pub const E_MALFORMED: i32 = -3; // entry is not NAME=VALUE
pub const E_RANGE: i32 = -4; // entry count over ENV_MAX_ENTRIES

/// The computed layout of a fresh process's initial stack.
///
/// Everything is an absolute user virtual address in the CHILD's address
/// space. C switches CR3 and stores through these; it does no arithmetic of
/// its own beyond `+ 8 * i` within the two pointer arrays whose extents are
/// fixed here.
///
/// Layout, high address to low, which is the SysV x86-64 initial process stack
/// and also exactly what the old comment in setup_user_argv() described as
/// "envp terminator (future)":
///
///     stack_top
///       <env strings>           envc NUL-terminated strings
///       <argv strings>          argc NUL-terminated strings
///       (8-byte alignment pad)
///       NULL                    envp terminator
///       envp[envc-1] .. envp[0]
///       NULL                    argv terminator
///       argv[argc-1] .. argv[0]
///       argc                    <- sp, 16-byte aligned
#[repr(C)]
pub struct StackPlan {
    /// Final user RSP. `argc` is stored here.
    pub sp: u64,
    /// Address of the argv[0] slot (sp + 8).
    pub argv_slots: u64,
    /// Address of the envp[0] slot (sp + 8 + (argc+1)*8).
    pub envp_slots: u64,
    /// Where argv string data starts (grows upward toward env_strings).
    pub argv_strings: u64,
    /// Where env string data starts (grows upward toward stack_top).
    pub env_strings: u64,
    /// Total bytes consumed below stack_top. Informational; C does not use it
    /// for addressing, but the selftest asserts on it.
    pub total: u64,
}

/// Validate one NUL-terminated environment entry and return its byte length
/// INCLUDING the NUL, or a negative E_* code.
///
/// `p` is a KERNEL pointer: the caller has already bounced the Ring-3 string
/// into kernel memory through the normal copy-in path. This function does not
/// dereference user memory and must never be handed a Ring-3 pointer.
///
/// # Safety
/// `p` must point to at least `cap` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn env_entry_len_rs(p: *const u8, cap: usize) -> i32 {
    if p.is_null() || cap == 0 {
        return E_MALFORMED;
    }
    let lim = if cap > ENV_MAX_ENTRY { ENV_MAX_ENTRY } else { cap };

    let mut i: usize = 0;
    let mut eq: usize = usize::MAX;
    while i < lim {
        let c = unsafe { *p.add(i) };
        if c == 0 {
            break;
        }
        if c == b'=' && eq == usize::MAX {
            eq = i;
        }
        i += 1;
    }
    // No NUL inside the limit: the entry is longer than policy allows, or the
    // caller's buffer is not NUL-terminated. Either way, refuse; do NOT
    // truncate, because a truncated PATH is a wrong answer that looks right.
    if i >= lim {
        return E_TOOBIG;
    }
    // eq == 0 is "=VALUE": an empty name, which getenv() can never match.
    if eq == usize::MAX || eq == 0 {
        return E_MALFORMED;
    }
    // i is the index of the NUL, so i + 1 is the length with it. i < lim <=
    // ENV_MAX_ENTRY <= isize::MAX, so the cast cannot lose bits.
    (i as i32) + 1
}

/// Compute the initial-stack layout, refusing rather than wrapping.
///
/// `argv_bytes` and `env_bytes` are the TOTALS of every string including its
/// NUL, as measured from the kernel copies. `argc`/`envc` are the entry
/// counts, not including the NULL terminators.
///
/// Returns 0 and fills `out` on success. On any refusal NOTHING is written to
/// `out` and the caller must abandon the spawn.
///
/// # Safety
/// `out` must be a valid, writable `StackPlan`.
#[no_mangle]
pub unsafe extern "C" fn user_stack_plan_rs(
    stack_top: u64,
    stack_size: u64,
    argc: u32,
    argv_bytes: u64,
    envc: u32,
    env_bytes: u64,
    out: *mut StackPlan,
) -> i32 {
    if out.is_null() {
        return E_MALFORMED;
    }
    if argc > ENV_MAX_ENTRIES || envc > ENV_MAX_ENTRIES {
        return E_RANGE;
    }
    if env_bytes > ENV_MAX_TOTAL || argv_bytes > ENV_MAX_TOTAL {
        return E_TOOBIG;
    }

    // ---- string area -------------------------------------------------------
    // env strings sit highest, argv strings below them. Both are byte-packed;
    // only the base of the whole string region needs alignment, because the
    // pointer arrays below it are what has to be 8-byte aligned.
    let str_bytes = match argv_bytes.checked_add(env_bytes) {
        Some(v) => v,
        None => return E_OVERFLOW,
    };
    let str_area = match stack_top.checked_sub(str_bytes) {
        Some(v) => v & !7u64,
        None => return E_OVERFLOW,
    };

    // ---- pointer array -----------------------------------------------------
    // argc + argv[argc] + NULL + envp[envc] + NULL
    let slots = match (argc as u64)
        .checked_add(envc as u64)
        .and_then(|v| v.checked_add(3))
    {
        Some(v) => v,
        None => return E_OVERFLOW,
    };
    let ptr_bytes = match slots.checked_mul(8) {
        Some(v) => v,
        None => return E_OVERFLOW,
    };
    let sp = match str_area.checked_sub(ptr_bytes) {
        Some(v) => v & !15u64,
        None => return E_OVERFLOW,
    };

    // ---- budget ------------------------------------------------------------
    let total = match stack_top.checked_sub(sp) {
        Some(v) => v,
        None => return E_OVERFLOW,
    };
    if total > STACK_INIT_BUDGET || total >= stack_size {
        return E_TOOBIG;
    }

    // argv_slots = sp + 8; envp_slots = sp + 8 + (argc + 1) * 8. Both are
    // inside [sp, str_area) by construction of ptr_bytes above, so these adds
    // cannot leave the reservation.
    let argv_slots = sp + 8;
    let envp_slots = argv_slots + ((argc as u64) + 1) * 8;

    unsafe {
        (*out).sp = sp;
        (*out).argv_slots = argv_slots;
        (*out).envp_slots = envp_slots;
        (*out).argv_strings = str_area;
        (*out).env_strings = str_area + argv_bytes;
        (*out).total = total;
    }
    0
}

// ===========================================================================
// The environment a KERNEL-launched process starts with.
//
// A process the kernel starts (the compositor from gui/desktop.c, a service
// from proc/services.c, a cron job from proc/cron.c) has no Ring-3 parent to
// inherit from, so there has to be a root of the tree. This is it, and it is
// deliberately tiny: only the variables that a program can do something
// sensible with in the absence of a login.
//
// HOME and USER are NOT here. They are per-session facts, they are not known
// at kernel-launch time, and inventing "/" and "user" for them is exactly the
// fabricated environment that the old /APPS/ENV was rewritten to stop
// printing. msh and the terminal set them from the real PASSWD entry once a
// session exists, and those values then propagate by inheritance.
// ===========================================================================
static ENV_DEFAULTS: [&[u8]; 3] = [
    b"PATH=/APPS\0",
    b"SHELL=/APPS/MSH\0",
    b"TERM=maytera\0",
];

/// The i-th kernel default entry as a NUL-terminated string, or null once the
/// list is exhausted. C iterates from 0 until it gets null.
#[no_mangle]
pub extern "C" fn env_default_rs(idx: u32) -> *const u8 {
    match ENV_DEFAULTS.get(idx as usize) {
        Some(s) => s.as_ptr(),
        None => core::ptr::null(),
    }
}

/// Boot self-test. Proves this policy is LIVE on this build rather than merely
/// compiled in. Returns a bitmask of FAILED checks; 0 means all passed.
///
/// # Safety
/// Only touches stack locals and the static default table.
#[no_mangle]
pub unsafe extern "C" fn envblock_selftest_rs() -> u32 {
    let mut fails: u32 = 0;
    let mut plan = StackPlan {
        sp: 0,
        argv_slots: 0,
        envp_slots: 0,
        argv_strings: 0,
        env_strings: 0,
        total: 0,
    };
    let top: u64 = 0xBFFF_0000;
    let size: u64 = 2 * 1024 * 1024;

    // 1. The plain case must succeed and land 16-byte aligned.
    if unsafe { user_stack_plan_rs(top, size, 2, 16, 3, 40, &mut plan) } != 0 {
        fails |= 1;
    } else if plan.sp & 15 != 0 {
        fails |= 2;
    } else if plan.envp_slots != plan.sp + 8 + 3 * 8 {
        // argc slot + argv[0..1] + argv NULL == 4 slots after sp
        fails |= 4;
    }

    // 2. A wrapping byte total must be REFUSED, not wrapped. This is the whole
    //    reason the arithmetic is here: u64::MAX + anything wraps in C.
    if unsafe { user_stack_plan_rs(top, size, 1, u64::MAX, 1, 8, &mut plan) } >= 0 {
        fails |= 8;
    }

    // 3. An over-budget block must be refused.
    if unsafe { user_stack_plan_rs(top, size, 0, 0, 64, ENV_MAX_TOTAL + 1, &mut plan) } >= 0 {
        fails |= 16;
    }

    // 4. Entry shape: NAME=VALUE accepted with the right length, no '=' and a
    //    leading '=' both refused.
    let ok = b"PATH=/APPS\0";
    if unsafe { env_entry_len_rs(ok.as_ptr(), ok.len()) } != 11 {
        fails |= 32;
    }
    let noeq = b"PATH\0";
    if unsafe { env_entry_len_rs(noeq.as_ptr(), noeq.len()) } != E_MALFORMED {
        fails |= 64;
    }
    let lead = b"=oops\0";
    if unsafe { env_entry_len_rs(lead.as_ptr(), lead.len()) } != E_MALFORMED {
        fails |= 128;
    }
    // 5. An unterminated buffer must be refused, never truncated.
    let raw = b"NOPE";
    if unsafe { env_entry_len_rs(raw.as_ptr(), raw.len()) } != E_TOOBIG {
        fails |= 256;
    }

    // 6. The default table must terminate.
    if env_default_rs(ENV_DEFAULTS.len() as u32) != core::ptr::null() {
        fails |= 512;
    }
    fails
}
