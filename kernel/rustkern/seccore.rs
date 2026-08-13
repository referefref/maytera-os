// seccore.rs - #624 CPU security core: CPUID feature decode + enable POLICY, in Rust.
//
// WHY THIS FILE EXISTS
// --------------------
// kernel/security/security.c defined security_init() and it had ZERO CALLERS
// from the day it was written until build 995. Nothing in it ever ran: SMEP and
// SMAP were never enabled, and the code's mere existence implied otherwise.
// docs/SECURITY_AUDIT.md was correct to list these as open; the CODE was the
// thing that lied. This module is the real implementation, and main.c now calls
// it (see kernel/main.c, right after bootlog_arm()).
//
// WHAT IS RUST HERE AND WHAT IS DELIBERATELY NOT
// ----------------------------------------------
// Rust owns all the LOGIC and all the STATE:
//   * CPUID leaf/subleaf selection, max-leaf bounds checking, and bit decode
//   * the "should SMEP be turned on?" policy (supported? requested? verified?)
//   * the CR4 readback verification decision
//   * the live feature bitmask (the single source of truth for what is ON)
// C keeps ONLY the two privileged instruction accessors sec_cr4_read() and
// sec_cr4_write() (kernel/security/security.c). Per the Rust-first rule this is
// the stated exemption: a `mov %cr4, %rax` pair is an irreducible
// instruction-level access with no logic in it, and the kernel already has
// read_cr4/write_cr4 as static inlines in types.h, so re-deriving them in Rust
// asm! would duplicate a primitive rather than reuse one.
// CPUID, RDTSC, RDRAND and RDSEED are NOT shimmed through C: core::arch gives
// them to Rust directly, so they are implemented here.
//
// A CORRECTNESS FIX OVER THE C IT REPLACES
// ----------------------------------------
// The C cpu_has_smep()/cpu_has_smap()/cpu_has_rdseed() called CPUID leaf 7
// without first checking CPUID.0:EAX (the maximum supported basic leaf), and
// cpu_has_nx() called leaf 0x80000001 without checking CPUID.0x80000000:EAX.
// On a CPU that does not implement those leaves, CPUID returns the data of the
// HIGHEST supported leaf instead of zeroes, so the C could decode an unrelated
// register as "SMEP supported" and then write a CR4 bit that does not exist.
// Every accessor below bounds-checks its leaf first.

use core::arch::x86_64::{__cpuid_count, _rdtsc};

extern "C" {
    // kernel/security/security.c. Irreducible privileged instruction access.
    fn sec_cr4_read() -> u64;
    fn sec_cr4_write(v: u64);
}

// Mirrors SECURITY_FEATURE_* in kernel/security/security.h.
pub const FEAT_ASLR: u32 = 1 << 0;
pub const FEAT_STACK_GUARD: u32 = 1 << 1;
pub const FEAT_NX: u32 = 1 << 5;
pub const FEAT_SMEP: u32 = 1 << 6;
pub const FEAT_SMAP: u32 = 1 << 7;

const CR4_SMEP: u64 = 1 << 20;
const CR4_SMAP: u64 = 1 << 21;

// SMEP outcome codes, reported to C for logging. C only prints these; the
// decision that produced them was made here.
pub const SMEP_UNSUPPORTED: u32 = 0;
pub const SMEP_ENABLED: u32 = 1;
pub const SMEP_CONFIG_OFF: u32 = 2;
pub const SMEP_CR4_REJECTED: u32 = 3;

pub const SMAP_UNSUPPORTED: u32 = 0;
pub const SMAP_ENABLED: u32 = 1;
pub const SMAP_CONFIG_OFF: u32 = 2; // supported; /NOSMAP.TXT or -DCONFIG_NO_SMAP
pub const SMAP_CR4_REJECTED: u32 = 3;

/// Result of sec_init_rs(). Layout locked on the C side with _Static_assert.
#[repr(C)]
pub struct SecInitReport {
    pub cr4: u64,
    pub features: u32,
    pub smep_status: u32,
    pub smap_status: u32,
    pub reserved: u32,
}

// ---------------------------------------------------------------------------
// CPUID, bounds-checked
// ---------------------------------------------------------------------------

fn max_basic_leaf() -> u32 {
    // SAFETY: CPUID leaf 0 is architecturally present on every x86-64 CPU.
    unsafe { __cpuid_count(0, 0).eax }
}

fn max_ext_leaf() -> u32 {
    // SAFETY: leaf 0x80000000 is present on every x86-64 CPU (long mode implies
    // the extended leaves exist); it returns the highest extended leaf in EAX.
    unsafe { __cpuid_count(0x8000_0000, 0).eax }
}

fn leaf7_ebx() -> u32 {
    if max_basic_leaf() < 7 {
        return 0;
    }
    // SAFETY: bounds-checked against CPUID.0:EAX immediately above.
    unsafe { __cpuid_count(7, 0).ebx }
}

fn leaf1_ecx() -> u32 {
    if max_basic_leaf() < 1 {
        return 0;
    }
    // SAFETY: bounds-checked against CPUID.0:EAX immediately above.
    unsafe { __cpuid_count(1, 0).ecx }
}

fn leaf_ext1_edx() -> u32 {
    if max_ext_leaf() < 0x8000_0001 {
        return 0;
    }
    // SAFETY: bounds-checked against CPUID.0x80000000:EAX immediately above.
    unsafe { __cpuid_count(0x8000_0001, 0).edx }
}

// NOTE ON THE FFI TYPE. These return u32 (0 or 1), never `bool`.
// The x86-64 psABI leaves the UPPER BITS of the return register unspecified for
// a byte-sized _Bool/bool, and the first boot of this code proved it is not
// theoretical: the [RUST-DIFF] line read `smep rs=1310721 c=1` because the C
// caller sampled a dirty EAX. In the argument direction the same sloppiness is
// undefined behaviour outright, since a Rust `bool` may only ever hold 0 or 1.
// So the FFI carries an explicitly normalised u32 in both directions and each
// side converts at its own boundary.

/// CPUID.(EAX=7,ECX=0):EBX[7] - Supervisor Mode Execution Prevention.
#[no_mangle]
pub extern "C" fn sec_cpu_has_smep_rs() -> u32 {
    (leaf7_ebx() & (1 << 7) != 0) as u32
}

/// CPUID.(EAX=7,ECX=0):EBX[20] - Supervisor Mode Access Prevention.
#[no_mangle]
pub extern "C" fn sec_cpu_has_smap_rs() -> u32 {
    (leaf7_ebx() & (1 << 20) != 0) as u32
}

/// CPUID.(EAX=7,ECX=0):EBX[18] - RDSEED.
#[no_mangle]
pub extern "C" fn sec_cpu_has_rdseed_rs() -> u32 {
    (leaf7_ebx() & (1 << 18) != 0) as u32
}

/// CPUID.(EAX=1):ECX[30] - RDRAND.
#[no_mangle]
pub extern "C" fn sec_cpu_has_rdrand_rs() -> u32 {
    (leaf1_ecx() & (1 << 30) != 0) as u32
}

/// CPUID.(EAX=0x80000001):EDX[20] - NX / execute-disable.
#[no_mangle]
pub extern "C" fn sec_cpu_has_nx_rs() -> u32 {
    (leaf_ext1_edx() & (1 << 20) != 0) as u32
}

// ---------------------------------------------------------------------------
// Live feature state
// ---------------------------------------------------------------------------

// Single source of truth for "which security features are actually ON". Touched
// only from security_init() during single-threaded early boot (before
// proc_init()) and read afterwards, so no locking is required; that constraint
// is the reason the setter is not exported.
static mut G_FEATURES: u32 = 0;

/// Live feature bitmask (SECURITY_FEATURE_* bits). Only bits whose feature is
/// observably doing work in this running kernel are ever set here.
#[no_mangle]
pub extern "C" fn sec_features_rs() -> u32 {
    unsafe { core::ptr::read_volatile(&raw const G_FEATURES) }
}

/// Record that the stack canary is installed and being checked.
/// Called from stack_guard_init() once __stack_chk_guard holds a real random
/// value. The mask lives HERE and only here: a second, independently-maintained
/// copy on the C side would be free to drift, which is the exact defect class
/// #624 exists to remove.
///
/// # Safety
/// Ring 0, single-threaded early boot; G_FEATURES is not lock-protected.
#[no_mangle]
pub unsafe extern "C" fn sec_mark_canary_live_rs() {
    G_FEATURES |= FEAT_STACK_GUARD;
}

/// Read TSC. Exposed for the entropy work in #624 step 2 and for boot timing.
#[no_mangle]
pub extern "C" fn sec_rdtsc_rs() -> u64 {
    // SAFETY: RDTSC is unprivileged and has no memory effects.
    unsafe { _rdtsc() }
}

// ---------------------------------------------------------------------------
// The policy
// ---------------------------------------------------------------------------

/// Enable the CPU security features this kernel has decided it can run with,
/// and report exactly what happened. Called once, from security_init().
///
/// `smep_requested` is the gate: main.c clears it when the /NOSMEP.TXT marker
/// is present on the FAT ESP, giving a no-rebuild escape hatch on a machine
/// that will not boot with SMEP.
///
/// # Safety
/// Caller must be Ring 0, single-threaded early boot, and must pass a valid
/// writable `out` pointer.
#[no_mangle]
pub unsafe extern "C" fn sec_init_rs(
    smep_requested: u32,
    smap_requested: u32,
    out: *mut SecInitReport,
) {
    let mut features: u32 = 0;

    if sec_cpu_has_nx_rs() != 0 {
        features |= FEAT_NX;
    }

    // --- SMEP ---
    let smep_status = if sec_cpu_has_smep_rs() == 0 {
        SMEP_UNSUPPORTED
    } else if smep_requested == 0 {
        SMEP_CONFIG_OFF
    } else {
        let cr4 = sec_cr4_read();
        sec_cr4_write(cr4 | CR4_SMEP);
        // Read back. A hypervisor that does not implement SMEP can silently
        // drop the write. Claiming SMEP when CR4 did not take it is precisely
        // the false assurance #624 exists to remove, so the feature bit is set
        // ONLY when the bit is observably live in CR4.
        if sec_cr4_read() & CR4_SMEP != 0 {
            features |= FEAT_SMEP;
            SMEP_ENABLED
        } else {
            SMEP_CR4_REJECTED
        }
    };

    // --- ASLR: PIE user-image base randomisation is LIVE (#646) ---
    //
    // Set here rather than in the C cache, because g_security_features is a
    // COPY of G_FEATURES and setting it on one side only would make the boot
    // line's `mask=0x%x (rust=0x%x)` pair disagree, which is its own lie.
    //
    // Claim ONLY what runs: exec/elf.c randomises every ET_DYN load base over
    // up to 512 2MB slots (~9 bits), and since #660/#661 every shipped binary
    // is PIE, so it applies image-wide. Stack, heap, mmap and kernel text are
    // NOT randomised. The adjacent boot line states that qualifier.
    //
    // Under-reporting a live control is the same defect as over-reporting one:
    // both send the next reader to fix the wrong thing. This bit was unset
    // while the feature ran, which is why #624 was re-opened against code that
    // already worked.
    features |= FEAT_ASLR;

    // --- SMAP (#645) ---
    //
    // Identical shape to SMEP above, for the same reason: the CR4 write is READ
    // BACK, and FEAT_SMAP is set ONLY if the bit is observably live. A
    // hypervisor may silently drop the write, and claiming SMAP that is not on
    // is the false assurance #624 exists to remove.
    //
    // `smap_requested` folds together THREE gates decided on the C side
    // (security.c): the -DCONFIG_NO_SMAP compile default, the /NOSMAP.TXT ESP
    // marker, and the boot-time pre-flight probe that checks the kernel's OWN
    // memory is not mapped user-accessible in the live CR3. Any of the three
    // can veto, and the veto arrives here as a plain 0.
    //
    // STANDING WARNING FOR WHOEVER TURNS THIS ON. Enabling SMAP is NOT just
    // this CR4 write. 126 ledgered Ring-0 accesses to user memory in this tree
    // still bypass the four mm/uaccess.asm primitives (kernel/smap-uaccess.
    // manifest), and each is a #PF the instant this bit is set, on paths taken
    // during normal operation: the ELF loader writes PT_LOAD segments under the
    // child CR3 on EVERY process launch, and the sigframe is built by casting
    // the Ring-3 stack pointer on EVERY signal. That is why the kernel ships
    // with -DCONFIG_NO_SMAP and why tools/smap-uaccess-lint FAILS the build if
    // the default is flipped while the ledger is non-empty.
    let smap_status = if sec_cpu_has_smap_rs() == 0 {
        SMAP_UNSUPPORTED
    } else if smap_requested == 0 {
        SMAP_CONFIG_OFF
    } else {
        let cr4 = sec_cr4_read();
        sec_cr4_write(cr4 | CR4_SMAP);
        if sec_cr4_read() & CR4_SMAP != 0 {
            features |= FEAT_SMAP;
            SMAP_ENABLED
        } else {
            SMAP_CR4_REJECTED
        }
    };

    // OR, never assign. sec_mark_canary_live_rs() has ALREADY set the stack
    // guard bit by the time this runs: security_canary_init() is called from
    // kernel_main immediately after serial_init(), long before security_init().
    // An assignment here would silently WIPE it and under-report a feature that
    // is genuinely live, which is the same lying-mask defect in the other
    // direction. Caught by review, before it shipped.
    G_FEATURES |= features;

    if !out.is_null() {
        core::ptr::write(
            out,
            SecInitReport {
                cr4: sec_cr4_read(),
                features: G_FEATURES,
                smep_status,
                smap_status,
                reserved: 0,
            },
        );
    }
}

// ===========================================================================
// Entropy and the stack canary (#624 step 2)
// ===========================================================================
//
// THE BUG THIS MUST NOT REPEAT: MAYTERA-SEC-2026-0015, where every box started
// its DHCP xid at 0xDEADBEF0. A predictable canary is worse than no canary,
// because it buys the appearance of protection while an attacker who knows the
// value simply rewrites it. So: hardware RNG where the CPU has one, a jitter
// source that is genuinely non-deterministic where it does not, and a boot-time
// print so the value can be compared across cold boots instead of assumed.
//
// MEASURED, and it drove the design: the kvm64 CPU profile this project tests
// on reports rdrand=0 AND rdseed=0 (see the [SECURITY] cpuid boot line). So the
// fallback is not a theoretical branch, it is the branch that actually runs on
// every test VM, and it had to be built to be good rather than to be present.

static mut POOL: u64 = 0;
static mut POOL_COUNT: u64 = 0;

/// SplitMix64. A strong 64-bit finaliser: avalanches every input bit across the
/// whole output, which is what makes the low, noisy TSC-delta bits usable.
fn splitmix64(mut x: u64) -> u64 {
    x = x.wrapping_add(0x9E37_79B9_7F4A_7C15);
    let mut z = x;
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^ (z >> 31)
}

fn rdrand64() -> Option<u64> {
    for _ in 0..10 {
        let v: u64;
        let ok: u8;
        // SAFETY: RDRAND is unprivileged, has no memory operands and no memory
        // effects. Guarded by the CPUID check at every call site.
        unsafe {
            core::arch::asm!(
                "rdrand {v}", "setc {ok}",
                v = out(reg) v, ok = out(reg_byte) ok,
                options(nomem, nostack)
            );
        }
        if ok != 0 {
            return Some(v);
        }
    }
    None
}

fn rdseed64() -> Option<u64> {
    for _ in 0..32 {
        let v: u64;
        let ok: u8;
        // SAFETY: as rdrand64 above.
        unsafe {
            core::arch::asm!(
                "rdseed {v}", "setc {ok}",
                v = out(reg) v, ok = out(reg_byte) ok,
                options(nomem, nostack)
            );
        }
        if ok != 0 {
            return Some(v);
        }
    }
    None
}

/// TSC-jitter entropy, for CPUs with no RDRAND/RDSEED.
///
/// It deliberately does NOT use the absolute TSC value. A VM resets the TSC to
/// approximately zero on every cold boot, so "read the TSC early in boot" would
/// yield a nearly IDENTICAL canary every time: that is the 0xDEADBEF0 bug in a
/// different costume. What is sampled instead is the VARIANCE in how long a
/// fixed amount of work takes. That variance comes from cache state, interrupt
/// arrival and host scheduling, none of which repeat exactly, and it survives
/// a TSC that starts from zero.
///
/// HONEST LIMIT: this is a jitter source, not a certified entropy source. It
/// has not been run through a statistical battery (that needs far more samples
/// than a boot can collect). It is materially better than a constant and
/// materially worse than RDSEED, and the boot log prints which one was used so
/// the distinction is never invisible.
fn tsc_jitter(rounds: u32) -> u64 {
    let mut acc: u64 = 0;
    for i in 0..rounds {
        // SAFETY: RDTSC is unprivileged with no memory effects.
        let t0 = unsafe { _rdtsc() };
        // Data-dependent spin length, so neither the compiler nor the branch
        // predictor can make the duration constant.
        let spin = 48 + (acc & 0x3F);
        let mut k = acc ^ (i as u64);
        for _ in 0..spin {
            k = splitmix64(k);
            // SAFETY: empty asm block, purely an optimisation barrier so the
            // loop above is not folded away.
            unsafe { core::arch::asm!("", options(nomem, nostack, preserves_flags)) };
        }
        let t1 = unsafe { _rdtsc() };
        acc = splitmix64(acc ^ t1.wrapping_sub(t0) ^ k);
    }
    acc
}

pub const ENTROPY_JITTER: u32 = 0;
pub const ENTROPY_RDRAND: u32 = 1;
pub const ENTROPY_RDSEED: u32 = 2;

/// Which entropy source this CPU will actually use. Reported at boot so that
/// "the canary is random" is a checkable claim rather than an assertion.
#[no_mangle]
pub extern "C" fn sec_entropy_source_rs() -> u32 {
    if sec_cpu_has_rdseed_rs() != 0 {
        ENTROPY_RDSEED
    } else if sec_cpu_has_rdrand_rs() != 0 {
        ENTROPY_RDRAND
    } else {
        ENTROPY_JITTER
    }
}

/// One 64-bit random value. Every call folds fresh material into a persistent
/// pool, so successive calls cannot repeat even if the hardware source stalls.
///
/// # Safety
/// Ring 0, and (like the rest of this module) intended for single-threaded
/// early boot; POOL is not lock-protected.
#[no_mangle]
pub unsafe extern "C" fn sec_random_u64_rs() -> u64 {
    let fresh = match sec_entropy_source_rs() {
        ENTROPY_RDSEED => rdseed64().or_else(rdrand64).unwrap_or_else(|| tsc_jitter(64)),
        ENTROPY_RDRAND => rdrand64().unwrap_or_else(|| tsc_jitter(64)),
        _ => tsc_jitter(96),
    };
    POOL_COUNT = POOL_COUNT.wrapping_add(1);
    POOL = splitmix64(POOL ^ fresh ^ POOL_COUNT);
    // Return a finalised view of the pool rather than the pool itself, so a
    // caller who learns one output learns nothing about the pool state.
    splitmix64(POOL ^ 0xA5A5_5A5A_C3C3_3C3C)
}

/// Generate the value for __stack_chk_guard.
///
/// Byte 0 is forced to 0x00. A str*/strcpy-class overflow copies up to the
/// first NUL, so it cannot rewrite a canary whose lowest byte is NUL with the
/// correct value. This is the standard terminator-canary trick (glibc and Linux
/// both do it) and costs 8 of the 64 bits, leaving 56 bits of entropy.
///
/// The older C in stack_guard.c also forced byte 1 to 0x0A and byte 7 to 0x0D,
/// which would have cost 24 bits for very little extra coverage. Not carried
/// over.
///
/// # Safety
/// See sec_random_u64_rs.
#[no_mangle]
pub unsafe extern "C" fn sec_canary_generate_rs() -> u64 {
    sec_random_u64_rs() & !0xFFu64
}
