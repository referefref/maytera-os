// security.c - Unified Security Module implementation for MayteraOS
#include "security.h"
#include "seclog.h"
#include "uaccess_smap.h"
#include "../mm/vmm.h"
#include "nova.h"
#include "aiguard.h"   /* #745: the policy layer that gives nova.c a caller */
#include "../serial.h"
#include "../string.h"

// ============================================================================
// Module State
// ============================================================================

static uint32_t g_security_features = 0;
static int g_audit_level = 2;  // Default: warnings and errors

// Audit log
#define AUDIT_LOG_SIZE 64
static struct {
    audit_event_t event;
    uint32_t pid;
    uint64_t timestamp;
    char detail[64];
} g_audit_log[AUDIT_LOG_SIZE];
static uint32_t g_audit_index = 0;
static uint64_t g_audit_count = 0;

// ============================================================================
// CPU Feature Detection
// ============================================================================

// #624: the C CPUID decode is KEPT, renamed *_c, as the strangler-pattern
// reference arm. It is no longer the live path (see RUST_SECCORE below) but it
// is what the boot [RUST-DIFF] compares the Rust against. NOTE the C does NOT
// bounds-check the leaf against CPUID.0:EAX / CPUID.0x80000000:EAX; the Rust
// does. On a CPU that does not implement leaf 7, CPUID returns the highest
// supported leaf's data instead of zero, so the C arm can decode an unrelated
// register as "SMEP supported". That is a real bug and it is why the arms may
// legitimately DISAGREE on an old CPU; on any CPU with leaf 7 they agree.
bool cpu_has_smep_c(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, &eax, &ebx, &ecx, &edx);
    return (ebx & (1 << 7)) != 0;  // SMEP bit
}

bool cpu_has_smap_c(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, &eax, &ebx, &ecx, &edx);
    return (ebx & (1 << 20)) != 0;  // SMAP bit
}

bool cpu_has_nx_c(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 20)) != 0;  // NX bit
}

// ---------------------------------------------------------------------------
// Rust security core FFI (rustkern/seccore.rs)
// ---------------------------------------------------------------------------
typedef struct {
    uint64_t cr4;
    uint32_t features;
    uint32_t smep_status;
    uint32_t smap_status;
    uint32_t reserved;
} sec_init_report_t;

// Lock the FFI layout. A silent layout drift between the Rust #[repr(C)] struct
// and this one would make security_init() report a feature mask read from the
// wrong offset, i.e. claim features that are not on.
_Static_assert(sizeof(sec_init_report_t) == 24, "#624 SecInitReport size drift");
_Static_assert(__builtin_offsetof(sec_init_report_t, cr4) == 0, "#624 cr4 offset");
_Static_assert(__builtin_offsetof(sec_init_report_t, features) == 8, "#624 features offset");
_Static_assert(__builtin_offsetof(sec_init_report_t, smep_status) == 12, "#624 smep offset");
_Static_assert(__builtin_offsetof(sec_init_report_t, smap_status) == 16, "#624 smap offset");

// The FFI carries uint32_t 0/1, NOT bool. The x86-64 psABI leaves the upper
// bits of a byte-sized _Bool return unspecified, and the first boot of this
// code proved that is not theoretical: the [RUST-DIFF] line read
// "smep rs=1310721 c=1" from a dirty EAX. Passing a C _Bool INTO a Rust `bool`
// is worse still: any value other than 0 or 1 is undefined behaviour in Rust.
// Each side normalises at its own boundary.
extern uint32_t sec_cpu_has_smep_rs(void);
extern uint32_t sec_cpu_has_smap_rs(void);
extern uint32_t sec_cpu_has_nx_rs(void);
extern uint32_t sec_cpu_has_rdrand_rs(void);
extern uint32_t sec_cpu_has_rdseed_rs(void);
extern uint32_t sec_features_rs(void);
extern uint64_t sec_rdtsc_rs(void);
extern void sec_init_rs(uint32_t smep_requested, uint32_t smap_requested,
                        sec_init_report_t *out);

// The two privileged-instruction accessors. This is the ENTIRE C surface the
// Rust security core needs, and the stated Rust-first exemption: a CR4
// read/write pair is an instruction-level access with no logic in it, and
// read_cr4/write_cr4 already exist as the tree's canonical inlines in types.h.
uint64_t sec_cr4_read(void) { return read_cr4(); }
void sec_cr4_write(uint64_t v) { write_cr4(v); }

#ifdef RUST_SECCORE
bool cpu_has_smep(void) { return sec_cpu_has_smep_rs() != 0; }
bool cpu_has_smap(void) { return sec_cpu_has_smap_rs() != 0; }
#else
bool cpu_has_smep(void) { return cpu_has_smep_c(); }
bool cpu_has_smap(void) { return cpu_has_smap_c(); }
#endif

// #624 SMEP boot gate. Default ON; -DCONFIG_NO_SMEP flips the compile-time
// default OFF; /NOSMEP.TXT on the FAT ESP turns it off with NO rebuild (same
// marker pattern as /TORAMOFF.TXT, #417). main.c reads the marker.
#ifdef CONFIG_NO_SMEP
static bool g_smep_requested = false;
#else
static bool g_smep_requested = true;
#endif

void security_smep_set_requested(bool on) { g_smep_requested = on; }

// #645 SMAP boot gate, deliberately the SAME SHAPE as the SMEP one above:
// default ON, -DCONFIG_NO_SMAP flips the compile-time default OFF, and
// /NOSMAP.TXT on the FAT ESP turns it off with NO rebuild. SMAP is the feature
// most likely to expose a latent "kernel touched a user page bare" bug, and
// such a bug presents as an unbootable machine, so the no-rebuild escape hatch
// is not a nicety: it is the difference between a recoverable box and a brick.
#ifdef CONFIG_NO_SMAP
static bool g_smap_requested = false;
#else
static bool g_smap_requested = true;
#endif

void security_smap_set_requested(bool on) { g_smap_requested = on; }

// #645: THE live SMAP switch, read from assembly (mm/uaccess.asm, cpu/idt.asm)
// as `[rel g_smap_active]` and from the uaccess_begin()/uaccess_end() inlines.
//
// It is set in exactly ONE place: security_init(), below, and ONLY when
// seccore.rs reports SMAP_ENABLED, which itself requires the CR4.SMAP write to
// have been READ BACK as taken. Until that moment every STAC/CLAC in the kernel
// is skipped, which is what makes it safe to boot this kernel on a CPU that
// does not implement the instructions at all (STAC/CLAC are #UD when
// CPUID.(EAX=7,ECX=0):EBX.SMAP is clear).
//
// `volatile` because assembly reads it and it is written from a different
// translation unit's perspective; not atomic, and it does not need to be: it is
// written once during single-threaded early boot, before proc_init() and before
// any AP is started, and is read-only for the rest of the kernel's life.
volatile uint8_t g_smap_active = 0;

// #19: the CR4 security bits the BSP actually ended up with, for cpu/smp.c's
// ap_entry() to copy onto every AP. Written once during single-threaded early
// boot (security_init), read-only afterwards. Mirrors the two CR4 bit numbers
// rustkern/seccore.rs owns; they are named here rather than re-derived at the
// AP so there is one definition of "the security bits".
#define CR4_SMEP_BIT  (1ULL << 20)
#define CR4_SMAP_BIT  (1ULL << 21)
uint64_t g_bsp_cr4_secbits = 0;

// #624 step 2: install the real per-boot stack canary. Called from kernel_main
// as early as serial output exists, so the window in which protected functions
// run against the compile-time placeholder is as small as possible.
//
// no_stack_protector is LOAD-BEARING here, not decoration: see the long comment
// on stack_guard_init() in stack_guard.c. kernel_main carries it too.
__attribute__((no_stack_protector))
void security_canary_init(void) {
    stack_guard_init();
}

// ============================================================================
// Security Feature Initialization
// ============================================================================

// #646 deleted smap_disable()/smap_restore(): zero callers, and not save/restore
// at all, because they ASSUMED RFLAGS.AC was clear on entry instead of reading
// it, so a nested use re-armed SMAP while the outer copy was still running.
// #645 writes the correct pair, and deliberately does NOT put it here:
// security/uaccess_smap.h has uaccess_begin()/uaccess_end(), which READ AC and
// return a token saying whether THIS bracket owns it, so nesting is a no-op.
// They live in a header because the bracket must inline directly around the
// access; a call to a non-inlined helper would widen the AC window to include
// the call and return themselves.
//
// Note that almost nothing should need them. The kernel touches user memory
// through exactly four assembly primitives (mm/uaccess.asm), reached by the
// five copy_*_user wrappers in validate.c, and THOSE now carry the bracket at
// the instruction. uaccess_begin() is for a site that genuinely cannot be
// expressed as one of those copies.

// ---------------------------------------------------------------------------
// #645 SMAP PRE-FLIGHT: is the kernel's OWN memory user-accessible?
// ---------------------------------------------------------------------------
//
// THE QUESTION THIS ANSWERS, AND WHY IT IS NOT RHETORICAL.
// vmm_init() adopts the firmware's page tables verbatim (mm/vmm.c: it takes
// UEFI's CR3 and never rewrites the U/S bits of the adopted entries). SMAP
// faults a Ring-0 access to any page whose U/S bit is set at EVERY level. So if
// the UEFI identity map happens to mark conventional RAM user-accessible, then
// turning SMAP on does not merely break the user-copy paths, it breaks EVERY
// KERNEL MEMORY ACCESS, instantly, including the one in the fault handler that
// would try to report it. That is an unrecoverable boot loop on a machine whose
// only escape hatch is a file on the ESP it can no longer reach.
//
// This is exactly the kind of assumption that should not be an assumption. The
// tree already has vmm_get_effective_flags_in(), which ANDs the U/S bit down
// the walk the same way the CPU does, so the question is directly measurable.
// We measure it, print it, and REFUSE to arm SMAP if the answer is bad.
//
// HONEST SCOPE: this samples three representative kernel addresses (static
// data, the live kernel stack, and kernel text), not the whole address space.
// A firmware that marked only SOME kernel range user-accessible could still
// slip through. It converts an unbounded unknown into a narrow one, which is
// the most an early-boot probe can do; it does not make SMAP risk-free.
static bool smap_preflight_ok(void) {
    static const volatile uint64_t probe_static = 0x5645524946594D45ULL;
    volatile uint64_t probe_stack = 0;
    uint64_t cr3 = vmm_get_pml4();

    const struct { const char *what; uint64_t va; } probes[] = {
        { "kernel .data",  (uint64_t)(const void *)&probe_static },
        { "kernel stack",  (uint64_t)(void *)&probe_stack },
        { "kernel text",   (uint64_t)(void *)(uintptr_t)&security_init },
    };

    bool ok = true;
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        uint64_t fl = vmm_get_effective_flags_in(cr3, probes[i].va);
        bool user = (fl & VMM_FLAG_USER) != 0;
        kprintf("[SECURITY] SMAP pre-flight: %-13s va=0x%016lx eff=0x%lx U/S=%s\n",
                probes[i].what, probes[i].va, fl, user ? "USER" : "supervisor");
        if (user) {
            ok = false;
        }
    }
    if (!ok) {
        kprintf("[SECURITY] SMAP pre-flight FAILED: kernel memory is mapped "
                "USER-accessible in the live CR3. Enabling SMAP would fault on "
                "ordinary kernel accesses, not just user copies. NOT enabling.\n");
    }
    return ok;
}

// ============================================================================
// Unified Initialization
// ============================================================================

// #624: security_init() had ZERO CALLERS from the day it was written until
// build 995. Everything it contained therefore never ran: SMEP and SMAP were
// never enabled. docs/SECURITY_AUDIT.md correctly listed these as open; it was
// this FILE that implied otherwise merely by existing.
//
// THE RULE FOR THIS FUNCTION FROM NOW ON: a SECURITY_FEATURE_* bit is set if
// and only if the feature is observably doing work in the shipped kernel. A bit
// that means "we have a .c file for this" is worse than no bit at all, because
// security_print_status() then reports it as "Enabled". The old body set
// ASLR / STACK_GUARD / PTR_VALIDATE / OVERFLOW_CHECK unconditionally while
// nothing in the kernel consumed any of them (and the kernel is built
// -fno-stack-protector, so the canary could not have been checked even in
// principle). Those four bits are NOT set here.
//
// The mask itself now lives in Rust (rustkern/seccore.rs G_FEATURES);
// g_security_features is a cache of it for the C-side query helpers.
void security_init(void) {
    /* #654: seed and report the ASLR entropy source at BOOT. Note this is no
     * longer load-bearing for randomness itself: aslr_get_random() now draws
     * from crypto/csprng.c, which self-instantiates, and golden 1008 produced
     * 11 distinct load bases in one boot with this call never having run. It
     * matters for the detection flags and for the boot report being true. */
    aslr_init();
    kprintf("[SECURITY] #624 CPU security core init v%d.%d.%d (logic: rustkern/seccore.rs)\n",
            SECURITY_VERSION_MAJOR, SECURITY_VERSION_MINOR, SECURITY_VERSION_PATCH);

    // --- [RUST-DIFF] strangler differential -------------------------------
    // Re-prove on THIS build, on THIS CPU, that the Rust CPUID decode agrees
    // with the C reference it replaced, before the result is used to write CR4.
    // Honest limit: this compares the DECODE only. It cannot exercise the
    // Rust's max-leaf bounds check, because a CPU that lacks leaf 7 is exactly
    // the case where the two arms are SUPPOSED to disagree; on any CPU that has
    // leaf 7 (all of ours) the check is a no-op and the arms must match.
    {
        bool rs_smep = sec_cpu_has_smep_rs() != 0, c_smep = cpu_has_smep_c();
        bool rs_smap = sec_cpu_has_smap_rs() != 0, c_smap = cpu_has_smap_c();
        bool rs_nx   = sec_cpu_has_nx_rs()   != 0, c_nx   = cpu_has_nx_c();
        int mism = (rs_smep != c_smep) + (rs_smap != c_smap) + (rs_nx != c_nx);
        kprintf("[RUST-DIFF] seccore cpuid: smep rs=%d c=%d, smap rs=%d c=%d, "
                "nx rs=%d c=%d -> %s\n",
                rs_smep, c_smep, rs_smap, c_smap, rs_nx, c_nx,
                mism ? "MISMATCH" : "MATCH");
    }

    kprintf("[SECURITY] cpuid: smep=%u smap=%u nx=%u rdrand=%u rdseed=%u\n",
            sec_cpu_has_smep_rs(), sec_cpu_has_smap_rs(), sec_cpu_has_nx_rs(),
            sec_cpu_has_rdrand_rs(), sec_cpu_has_rdseed_rs());

    // --- the policy runs in Rust ------------------------------------------
    // Pre-flight BEFORE the policy runs, and fold the result into the request:
    // a failed probe is exactly as good a reason not to enable SMAP as
    // /NOSMAP.TXT is, and it is one the machine can work out for itself.
    bool smap_ok = g_smap_requested;
    if (smap_ok && sec_cpu_has_smap_rs() != 0) {
        smap_ok = smap_preflight_ok();
    }

    sec_init_report_t rep;
    sec_init_rs(g_smep_requested ? 1u : 0u, smap_ok ? 1u : 0u, &rep);
    g_security_features = rep.features;

    // #645: arm the live SMAP switch ONLY from the feature bit, which seccore.rs
    // sets only after a CR4 readback proved the bit took. Deriving it from
    // g_smap_requested instead would arm every STAC/CLAC in the kernel on a
    // hypervisor that silently dropped the CR4 write, i.e. it would pay the cost
    // and claim the protection while having neither.
    g_smap_active = (rep.features & SECURITY_FEATURE_SMAP) ? 1u : 0u;

    // #19: PUBLISH THE BSP'S CR4 SECURITY BITS FOR THE APs. CR4 is per-CPU and
    // cpu/smp.c's ap_entry() never touched SMEP/SMAP, so on a 2-core box half
    // the machine ran unprotected while the banner below said ENABLED. The APs
    // copy exactly what the BSP ended up with, so the CPUID gate, the
    // /NOSMEP.TXT and /NOSMAP.TXT escape hatches and the CR4 readback that
    // gates them all keep deciding for the whole machine, in one place.
    g_bsp_cr4_secbits = sec_cr4_read() & (CR4_SMEP_BIT | CR4_SMAP_BIT);

    switch (rep.smep_status) {
        case 0: kprintf("[SECURITY] SMEP: NOT SUPPORTED by this CPU\n"); break;
        case 1: kprintf("[SECURITY] SMEP: ENABLED (cr4=0x%lx) - Ring-0 cannot "
                        "execute user pages\n", rep.cr4); break;
        case 2: kprintf("[SECURITY] SMEP: supported but DISABLED by config "
                        "(/NOSMEP.TXT or -DCONFIG_NO_SMEP)\n"); break;
        default: kprintf("[SECURITY] SMEP: CR4 write REJECTED by the platform "
                         "(cr4=0x%lx); NOT enabled\n", rep.cr4); break;
    }
    switch (rep.smap_status) {
        case 0: kprintf("[SECURITY] SMAP: NOT SUPPORTED by this CPU\n"); break;
        case 1: kprintf("[SECURITY] SMAP: ENABLED (cr4=0x%lx) - Ring-0 cannot "
                        "read/write user pages outside a stac/clac bracket\n",
                        rep.cr4); break;
        case 2: kprintf("[SECURITY] SMAP: supported but DISABLED by config "
                        "(/NOSMAP.TXT or -DCONFIG_NO_SMAP)\n"); break;
        default: kprintf("[SECURITY] SMAP: CR4 write REJECTED by the platform "
                         "(cr4=0x%lx); NOT enabled\n", rep.cr4); break;
    }
    kprintf("[SECURITY] SMAP: stac/clac brackets %s (g_smap_active=%u)\n",
            g_smap_active ? "ARMED" : "inert (no STAC/CLAC will execute)",
            (unsigned)g_smap_active);

    // The canary was installed far earlier (kernel_main -> security_canary_init).
    // Report it here so one boot log shows the whole security posture together.
    {
        extern uint64_t __stack_chk_guard;
        kprintf("[SECURITY] stack canary: LIVE, guard=0x%016lx "
                "(-fstack-protector-strong, guard=global)\n", __stack_chk_guard);
    }
#ifdef CANARY_SELFTEST
    {
        extern void security_canary_selftest(void);
        security_canary_selftest();
    }
#endif

    // #646 CORRECTION. The comment that stood here said "Nothing in this kernel
    // is address-randomised ... user-image ASLR is impossible until PIE lands
    // (#640)". PIE HAS landed: every userland app links -pie against
    // user-pie.ld, and exec/elf.c randomises the load base of each PIE image
    // over up to 512 2MB slots via aslr_get_random_range(). Golden 1008 was
    // measured producing 11 distinct load bases in a single boot.
    //
    // So the ASLR bit is now UNDER-reporting, not over-reporting. It stays
    // unset here on purpose: G_FEATURES is owned by rustkern/seccore.rs and
    // setting the bit only in this C cache would make the boot line's
    // "mask=0x%x (rust=0x%x)" pair disagree. The bit belongs in sec_init_rs().
    // Say the truth on the console meanwhile.
    kprintf("[SECURITY] ASLR: user PIE image base RANDOMISED by exec/elf.c "
            "(<=9 bits, 2MB grain); stack/heap/mmap/kernel NOT randomised\n");

    // #646: run the Nova prompt-injection ruleset self-test. It had zero
    // callers, i.e. it proved nothing on any shipped build. It is pure string
    // matching over static data with no allocation and no I/O, so it is safe at
    // this point in boot. Note what this does and does NOT prove: it proves the
    // KEYWORD matcher still fires on 7 known-malicious and stays silent on 4
    // known-benign prompts. It does NOT mean anything is being screened, see
    // the NOT-WIRED banner in nova.h.
    {
        /* NULL report buffer on purpose: the return value is the verdict, and a
         * local we write and never read is the small version of the same defect
         * this pass exists to remove. */
        int nfail = nova_selftest(0, 0);
        kprintf("[NOVA] ruleset self-test: %s (%d rules, %d failures)\n",
                nfail == 0 ? "PASS" : "FAIL", nova_rule_count(), nfail);
    }

    /* #745: the POLICY self-test, which is a different claim from the one
     * above. nova_selftest() proves the MATCHER still fires; this proves the
     * screen that CONSUMES it makes the right decision, in BOTH directions and
     * on the right scope. A guard that blocks everything passes any "did it
     * block" test, so the cases include benign LLM bodies that MUST pass and a
     * non-LLM body carrying injection text that MUST NOT be screened at all. */
    {
        static char rep[768];
        int gfail = aiguard_selftest_rs(rep, sizeof(rep));
        kprintf("[AIGUARD] policy self-test: %s (%d failure%s)\n",
                gfail == 0 ? "PASS" : "FAIL", gfail, gfail == 1 ? "" : "s");
        kprintf("%s", rep);
    }
    kprintf("[SECURITY] init complete, LIVE features mask=0x%x (rust=0x%x)\n",
            g_security_features, sec_features_rs());
}

// ============================================================================
// Feature Queries
// ============================================================================

uint32_t security_get_features(void) {
    return g_security_features;
}

bool security_feature_enabled(uint32_t feature) {
    return (g_security_features & feature) != 0;
}

// ============================================================================
// Secure Memory Operations
// ============================================================================

// Volatile pointer prevents optimization
typedef void *(*volatile memset_ptr)(void *, int, size_t);
static memset_ptr secure_memset_ptr = memset;

void secure_zero(void *ptr, size_t size) {
    // Use volatile pointer to prevent compiler from optimizing away
    secure_memset_ptr(ptr, 0, size);

    // Memory barrier to ensure write completes
    __asm__ volatile("" ::: "memory");
}

int secure_compare(const void *a, const void *b, size_t size) {
    const volatile uint8_t *pa = (const volatile uint8_t *)a;
    const volatile uint8_t *pb = (const volatile uint8_t *)b;
    volatile uint8_t result = 0;

    // Compare all bytes, accumulating differences
    // This prevents early-exit timing attacks
    for (size_t i = 0; i < size; i++) {
        result |= pa[i] ^ pb[i];
    }

    return result;
}

// ============================================================================
// Process Security
// ============================================================================

// #646: security_init_process()/security_cleanup_process() are DELETED. Both
// bodies were `(void)proc;` plus a TODO, both had zero callers, and the header
// advertised "Sets up ASLR, stack canaries, etc.". Per-process security state is
// not initialised by this kernel; that is now said once, here, instead of being
// contradicted by two empty functions.

// ============================================================================
// Security Audit Logging
// ============================================================================

// External declaration for timer ticks
extern volatile uint64_t timer_ticks;

void security_audit(audit_event_t event, uint32_t pid, const char *detail) {
    if (g_audit_level == 0) return;

    // Log to buffer
    uint32_t idx = g_audit_index;
    g_audit_log[idx].event = event;
    g_audit_log[idx].pid = pid;
    g_audit_log[idx].timestamp = timer_ticks;

    if (detail) {
        strncpy(g_audit_log[idx].detail, detail, sizeof(g_audit_log[idx].detail) - 1);
        g_audit_log[idx].detail[sizeof(g_audit_log[idx].detail) - 1] = '\0';
    } else {
        g_audit_log[idx].detail[0] = '\0';
    }

    g_audit_index = (g_audit_index + 1) % AUDIT_LOG_SIZE;
    g_audit_count++;

    // Print based on audit level
    const char *event_name;
    int severity;

    switch (event) {
        case AUDIT_STACK_SMASH:
        case AUDIT_MEMORY_VIOLATION:
        case AUDIT_EXEC_VIOLATION:
            event_name = "CRITICAL";
            severity = 1;
            break;
        case AUDIT_SYSCALL_FAIL:
        case AUDIT_PTR_INVALID:
        case AUDIT_PERMISSION_DENIED:
        // #697: both outcomes of an authentication are WARNING. A DENIED
        // attempt is the obvious one; a SUCCEEDED remote login is equally
        // worth surfacing, because "somebody logged into this machine over
        // the network" is exactly the event the owner needs to see, and an
        // audit trail that records only failures cannot answer "who got in".
        case AUDIT_AUTH_SUCCESS:
        case AUDIT_AUTH_FAIL:
            event_name = "WARNING";
            severity = 2;
            break;
        case AUDIT_OVERFLOW:
        case AUDIT_AUTH_PROBE:   // #697: a refused credential OFFER, not an attempt
        default:
            event_name = "INFO";
            severity = 3;
            break;
    }

    if (severity <= g_audit_level) {
        kprintf("[AUDIT] %s: pid=%u", event_name, pid);
        if (detail) {
            kprintf(" - %s", detail);
        }
        kprintf("\n");
    }

    /* #653: wake the seclog worker so this event reaches /CONFIG/SECURITY.LOG
     * and, if CRITICAL/WARNING, the desktop notification spool.
     *
     * THIS MUST STAY A BARE WAKE. security_audit() is reached from syscall
     * validation, pointer checks and fault paths, some with interrupts off or
     * a spinlock held. Doing the file write here would block in a no-block
     * context: the exact pattern wq_assert_may_block() exists to catch. All
     * I/O belongs to the worker. */
    seclog_kick();
}

/* ---------------------------------------------------------------------------
 * #653: audit ring read access (see security.h for why this is by sequence).
 * ------------------------------------------------------------------------- */
/* #653: see seclog.h for why syscall.c cannot include security.h. */
void seclog_report_bad_user_ptr(unsigned int pid, const char *detail) {
    security_audit(AUDIT_PTR_INVALID, (uint32_t)pid, detail);
}

/* #745: privilege elevation. Same narrow-producer shape, so proc/elevate.c
 * needs no security.h include and the audit ordinal stays in one place.
 *
 * INFO, not WARNING, and that is a decision rather than an oversight: this
 * fires on the raise, the cancel, every wrong password and the grant, and a
 * desktop toast for each would be four notifications for one deliberate
 * install the user is watching happen. The record belongs in
 * /CONFIG/SECURITY.LOG, which is where an owner looks to answer "who installed
 * this for everyone, and when", and that is what it gets. */
void seclog_report_elevation(unsigned int pid, const char *detail) {
    security_audit(AUDIT_ELEVATION, (uint32_t)pid, detail);
}

/* #745: the AI prompt-injection screen. WARNING severity, deliberately: unlike
 * the elevation records above (which narrate one install the user is watching),
 * a blocked LLM request is something the user asked for that did NOT happen,
 * and something the user did not ask for that DID. Both belong on screen, not
 * only in the log. */
void seclog_report_ai_injection(unsigned int pid, const char *detail) {
    security_audit(AUDIT_AI_INJECTION, (uint32_t)pid, detail);
}

uint64_t security_audit_seq(void) {
    return g_audit_count;
}

int security_audit_fetch(uint64_t seq, security_audit_rec_t *out) {
    if (!out) return -1;
    if (seq >= g_audit_count) return -1;                    /* not written yet */
    if (g_audit_count - seq > AUDIT_LOG_SIZE) return -1;    /* already overwritten */

    /* g_audit_index advances in lockstep with g_audit_count, so the slot for a
     * given sequence is simply seq % AUDIT_LOG_SIZE. */
    uint32_t idx = (uint32_t)(seq % AUDIT_LOG_SIZE);
    out->event     = (uint32_t)g_audit_log[idx].event;
    out->pid       = g_audit_log[idx].pid;
    out->timestamp = g_audit_log[idx].timestamp;
    for (unsigned i = 0; i < sizeof(out->detail); i++)
        out->detail[i] = g_audit_log[idx].detail[i];
    out->detail[sizeof(out->detail) - 1] = '\0';
    return 0;
}

void security_set_audit_level(int level) {
    g_audit_level = level;
}

// ============================================================================
// Status Reporting
// ============================================================================

void security_print_status(void) {
    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  MayteraOS Security Status\n");
    kprintf("============================================================\n");
    kprintf("\n");

    kprintf("Security Features:\n");
    kprintf("  ASLR (bit):       %s   [PIE image base IS randomised by exec/elf.c;\n"
            "                          the bit is owned by seccore.rs and unset]\n",
            (g_security_features & SECURITY_FEATURE_ASLR) ? "Enabled" : "Disabled");
    kprintf("  Stack Guard:      %s\n", (g_security_features & SECURITY_FEATURE_STACK_GUARD) ? "Enabled" : "Disabled");
    kprintf("  Ptr Validation:   LIVE (validate.c; the bit is never set by anything)\n");
    kprintf("  Overflow Check:   not implemented (module deleted, #646)\n");
    kprintf("  Guard Pages:      not implemented\n");
    kprintf("  NX (No-Execute):  %s\n", (g_security_features & SECURITY_FEATURE_NX) ? "Supported" : "Not Available");
    kprintf("  SMEP:             %s\n", (g_security_features & SECURITY_FEATURE_SMEP) ? "Enabled" : "Not Available");
    kprintf("  SMAP:             %s\n", (g_security_features & SECURITY_FEATURE_SMAP) ? "Enabled" : "Not Available");
    kprintf("\n");

    // Print subsystem details
    // #624: aslr_init() had ZERO callers, so g_entropy_source stayed at its
    // static default ASLR_ENTROPY_NONE and aslr_get_random() skipped BOTH
    // hardware-RNG branches - the kernel never used RDRAND/RDSEED even where
    // the CPU has them, always falling through to the TSC-mixed pool. Seed
    // first, THEN print, so the printed source is the real one.
    /* #654: aslr_init() used to be called HERE, inside a dump-on-demand
     * printer that nothing calls at boot - so the #624 fix for "aslr_init has
     * zero callers" put the call in a function with no callers of its own. It
     * now runs from security_init(); this is only the printer. */
    aslr_print_info();
    kprintf("\n");
    stack_guard_print_info();

    kprintf("\nAudit Statistics:\n");
    kprintf("  Total events:     %lu\n", g_audit_count);
    kprintf("  Audit level:      %d\n", g_audit_level);
    kprintf("  Ptr rejections:   %lu\n", validate_failure_count());

    kprintf("\n============================================================\n");
}
