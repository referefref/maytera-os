// security.h - Unified Security Module for MayteraOS
//
// #646 HONESTY PASS (2026-08-05). Deleted from this header, all zero-caller:
//   security_init_process() / security_cleanup_process()
//       Bodies were `(void)proc;` plus a TODO list. The doc comment claimed
//       "Sets up ASLR, stack canaries, etc." for a function that did nothing at
//       all, and nothing called it either way. A no-op with a confident doc
//       comment is worse than an absent function, because it answers "do we
//       initialise per-process security state?" with a yes.
//   smap_disable() / smap_restore()
//       A stac/clac wrapper pair with zero callers, for a feature (SMAP) that
//       rustkern/seccore.rs deliberately does NOT enable until the stac/clac
//       audit lands (#624 step 4). They were also not save/restore at all: they
//       assumed AC was clear on entry rather than reading RFLAGS.AC, so a
//       nested use would have re-enabled SMAP early. Whoever does the audit
//       should write them properly then (pushfq/popfq or an explicit AC read),
//       against real call sites, rather than inherit a broken pair.
//   #include "overflow.h" and the whole overflow module (overflow.c/.h)
//       Zero includers outside security/, zero uses of any SAFE_*/safe_*/
//       validate_* helper it defined, and overflow_report() had no callers, so
//       overflow_get_count() was structurally always 0 while
//       overflow_print_info() announced "Integer Overflow Protection Status".
//       The tree already uses __builtin_{add,mul}_overflow directly (see
//       validate.c), which is the shared primitive; this was a private
//       reimplementation nobody adopted.
#ifndef SECURITY_H
#define SECURITY_H

#include "../types.h"
#include "aslr.h"
#include "validate.h"
#include "stack_guard.h"

// ============================================================================
// Security Module Version
// ============================================================================

#define SECURITY_VERSION_MAJOR  1
#define SECURITY_VERSION_MINOR  0
#define SECURITY_VERSION_PATCH  0

// ============================================================================
// Unified Security Initialization
// ============================================================================

/**
 * Initialize the CPU security core. Called from kernel_main (main.c), after the
 * FAT root is mounted so the /NOSMEP.TXT escape hatch is readable, and well
 * before proc_init()/any Ring-3 entry.
 */
void security_init(void);

/**
 * Print the full security posture.
 *
 * ZERO CALLERS as of #646, stated rather than hidden. It is kept, not deleted,
 * because it is the only place that gathers the ASLR / stack-guard / audit
 * posture into one report, and its three sub-printers are all honest now. It
 * needs a call site: a serial-shell `security` command is the natural one.
 * Until it has one, this whole report is unreachable at runtime.
 */
void security_print_status(void);

// ============================================================================
// Security Feature Flags
// ============================================================================
//
// THE RULE (from #624, restated): a bit here is set if and only if the feature
// is observably doing work in the shipped kernel. The mask is owned by
// rustkern/seccore.rs (G_FEATURES); these defines MIRROR the Rust constants, so
// do not renumber them on one side only.
//
// Which bits can currently be set at all, measured:
//   ASLR           - NOT set today. PIE user-image base randomisation IS live
//                    (exec/elf.c), so this bit is currently UNDER-reporting.
//                    Setting it belongs in seccore.rs, not here.
//   STACK_GUARD    - SET, by sec_mark_canary_live_rs() from stack_guard_init().
//   PTR_VALIDATE   - never set. Pointer validation is real and live (validate.c
//                    via copy_*_user and syscall_validate_args) but no code sets
//                    this bit.
//   OVERFLOW_CHECK - never set; the overflow module is deleted (#646).
//   GUARD_PAGES    - never set; stack guard pages are not implemented (#646).
//   NX             - set when CPUID reports NX.
//   SMEP           - set only after the CR4 write is READ BACK as taken.
//   SMAP           - #645: set only after the CR4.SMAP write is READ BACK as
//                    taken, exactly like SMEP. When set, g_smap_active is 1 and
//                    the STAC/CLAC brackets in mm/uaccess.asm and cpu/idt.asm
//                    execute; when clear they are skipped, so a CPU without the
//                    instructions (they are #UD without CPUID SMAP) still boots.
#define SECURITY_FEATURE_ASLR           (1 << 0)    // Address randomization
#define SECURITY_FEATURE_STACK_GUARD    (1 << 1)    // Stack canaries
#define SECURITY_FEATURE_PTR_VALIDATE   (1 << 2)    // Pointer validation
#define SECURITY_FEATURE_OVERFLOW_CHECK (1 << 3)    // Integer overflow checks
#define SECURITY_FEATURE_GUARD_PAGES    (1 << 4)    // Stack guard pages
#define SECURITY_FEATURE_NX             (1 << 5)    // No-execute pages
#define SECURITY_FEATURE_SMEP           (1 << 6)    // Supervisor Mode Exec Prevention
#define SECURITY_FEATURE_SMAP           (1 << 7)    // Supervisor Mode Access Prevention

#define SECURITY_FEATURE_ALL            0xFF

/**
 * Get enabled security features
 * @return bitmask of enabled features
 */
uint32_t security_get_features(void);

/**
 * Check if a security feature is enabled
 * @param feature   feature flag to check
 * @return          true if feature is enabled
 */
bool security_feature_enabled(uint32_t feature);

// ============================================================================
// CPU Security Features
// ============================================================================

/**
 * Check if SMEP is supported by CPU
 * @return true if supported
 */
bool cpu_has_smep(void);

/**
 * Check if SMAP is supported by CPU
 * @return true if supported
 */
bool cpu_has_smap(void);

// #624: security_enable_smep()/security_enable_smap() are GONE. Enabling a CR4
// security bit is now a policy decision owned by rustkern/seccore.rs
// (sec_init_rs), so there is exactly one place that can turn these on, and it
// verifies the CR4 readback before claiming success. Two entry points would
// mean two chances to claim a feature that is not live.

/**
 * #624 SMEP boot gate. main.c calls this BEFORE security_init() when the
 * /NOSMEP.TXT marker is present at the root of the FAT ESP, so SMEP can be
 * turned off on a machine that will not boot with it WITHOUT a rebuild.
 * Compile-time default is ON; build with -DCONFIG_NO_SMEP to default it OFF.
 * security.c deliberately carries no filesystem dependency, hence the setter.
 */
void security_smep_set_requested(bool on);

/**
 * #645 SMAP boot gate, the exact counterpart of the SMEP one above. main.c
 * calls this BEFORE security_init() when /NOSMAP.TXT is present at the root of
 * the FAT ESP, so SMAP can be turned off WITHOUT a rebuild on a machine it
 * breaks. Compile-time default is ON; -DCONFIG_NO_SMAP defaults it OFF.
 *
 * This escape hatch matters more than the SMEP one. A missed user-memory access
 * under SMAP is a #PF on a path the kernel takes during normal operation, so
 * the failure mode is "this machine no longer boots", and the fix must not
 * require the toolchain that produced the broken image.
 */
void security_smap_set_requested(bool on);

/**
 * Install the real per-boot stack canary. Called from kernel_main immediately
 * after serial_init(). See stack_guard.c for why the whole call chain must be
 * no_stack_protector.
 */
void security_canary_init(void);

/**
 * Irreducible privileged-instruction accessors, the ONLY part of the CPU
 * security core that is not Rust. Called from rustkern/seccore.rs.
 */
uint64_t sec_cr4_read(void);
void sec_cr4_write(uint64_t v);

// ============================================================================
// Secure Memory Operations
// ============================================================================

/**
 * Securely zero memory (not optimized away)
 *
 * ZERO CALLERS as of #646. Kept because a compiler-barrier-backed wipe is the
 * correct primitive for key material and crypto/ will want exactly this; call
 * it instead of memset when zeroing a secret.
 * @param ptr   memory to zero
 * @param size  bytes to zero
 */
void secure_zero(void *ptr, size_t size);

/**
 * Compare memory in constant time. Prevents timing side-channel attacks.
 *
 * ZERO CALLERS as of #646, and that is itself a finding: any password/HMAC/tag
 * comparison in crypto/ that uses memcmp is timing-variable. Kept, and it is
 * the primitive those sites should adopt.
 * @return 0 if equal, non-zero if different
 */
int secure_compare(const void *a, const void *b, size_t size);

// ============================================================================
// Security Audit Logging
// ============================================================================

// Audit event types
typedef enum {
    AUDIT_SYSCALL_FAIL,         // Syscall validation failure
    AUDIT_STACK_SMASH,          // Stack smashing detected
    AUDIT_PTR_INVALID,          // Invalid pointer access
    AUDIT_OVERFLOW,             // Integer overflow detected
    AUDIT_PERMISSION_DENIED,    // Permission denied
    AUDIT_EXEC_VIOLATION,       // Execute permission violation
    AUDIT_MEMORY_VIOLATION,     // Memory access violation
    // #697. APPEND ONLY below this line: the ordinal is what gets persisted
    // into /CONFIG/SECURITY.LOG, so inserting in the middle would silently
    // relabel every historical record.
    AUDIT_AUTH_SUCCESS,         // An interactive/remote login SUCCEEDED
    AUDIT_AUTH_FAIL,            // An interactive/remote login was DENIED
    // A credential was OFFERED and refused before any attempt was made with it
    // (an ssh publickey query with no signature). This is a probe, not an
    // attempt, and it happens once per key an ordinary client offers, so it is
    // INFO: it belongs in the log, it must not raise a desktop toast.
    AUDIT_AUTH_PROBE,
    // #785: a NETWORK SERVICE was started, stopped, enabled or disabled. This
    // exists because the first cut of that change logged a listener start as
    // AUDIT_AUTH_SUCCESS and a stop as AUDIT_PERMISSION_DENIED, which are both
    // false: nobody authenticated and nobody was denied. Mislabelling is worse
    // than not auditing, because an incident review counts AUTH_SUCCESS
    // records and would have counted boots as logins. Appended, per the
    // append-only rule above, so no historical ordinal is relabelled.
    AUDIT_SERVICE_STATE,
    // #745: a privilege ELEVATION was requested, refused, cancelled or granted.
    // Appended, per the append-only rule above, so no historical ordinal in
    // /CONFIG/SECURITY.LOG is relabelled.
    AUDIT_ELEVATION,
    // #745: an outbound LLM request was REFUSED, or allowed and flagged, by the
    // prompt-injection screen. Appended, per the append-only rule above, so no
    // historical ordinal in /CONFIG/SECURITY.LOG is relabelled. Ordinal 12; the
    // name and severity tables in rustkern.rs carry the matching arm, because
    // an enum in this tree has TWO switches and the second one is the file
    // record (#697: a new event logged itself as "INFO UNKNOWN" for exactly
    // this reason).
    AUDIT_AI_INJECTION,
    // #fdguard: a cross-process file-descriptor access, or an attach to a
    // /dev/pts terminal the caller does not own, was REFUSED. Appended per
    // the append-only rule above so no historical ordinal is relabelled.
    // Ordinal 13; the name and severity tables in rustkern.rs carry the
    // matching arm (a new event without one logs as INFO UNKNOWN, #697).
    AUDIT_IO_BOUNDARY,
} audit_event_t;

/**
 * Log a security audit event.
 *
 * MUST stay non-blocking: it is reached from syscall validation and fault
 * paths, some with interrupts off or a spinlock held. Ring write + kprintf +
 * seclog_kick(); all I/O belongs to the seclog worker (seclog.c).
 * @param event     event type
 * @param pid       process ID (0 for kernel)
 * @param detail    additional detail string
 */
void security_audit(audit_event_t event, uint32_t pid, const char *detail);

/* ------------------------------------------------------------------------
 * #653: read access to the audit ring, so the seclog worker can drain it to
 * /CONFIG/SECURITY.LOG and to the desktop notification spool.
 *
 * Read access is by SEQUENCE NUMBER, not by index, on purpose. The ring holds
 * only the last AUDIT_LOG_SIZE events; a consumer that fell behind must be
 * able to tell "I missed some" from "there is nothing new". Handing out an
 * index cannot express that, so security_audit_fetch() fails for a sequence
 * that has already been overwritten and the caller records the loss.
 * ------------------------------------------------------------------------ */
typedef struct {
    uint32_t event;        /* audit_event_t ordinal */
    uint32_t pid;
    uint64_t timestamp;    /* timer_ticks at the time of the event */
    char     detail[64];
} security_audit_rec_t;

/* Total events ever recorded. This is the sequence number of the NEXT event. */
uint64_t security_audit_seq(void);

/* Copy the record with sequence `seq`. Returns 0 on success, -1 if `seq` is in
 * the future or has already been overwritten in the ring. */
int security_audit_fetch(uint64_t seq, security_audit_rec_t *out);

/**
 * Set audit logging level
 *
 * ZERO CALLERS as of #646: the level is fixed at its compiled-in default of 2
 * (warnings and errors). Kept as the one knob a future settings/shell path
 * should drive; note that level 0 silences the audit trail, so whoever wires it
 * up owes it a privilege check.
 * @param level     0=off, 1=errors only, 2=warnings, 3=all
 */
void security_set_audit_level(int level);

#endif // SECURITY_H
