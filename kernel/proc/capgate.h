// proc/capgate.h - Stage 0 of the system capability API.
// C view of rustkern/capgate.rs. See that file for the five measured defects
// this closes and for why the compositor check here is deliberately NOT
// gui/fb_syscall.c's is_compositor().
//
// docs/SYSTEM_CAPABILITY_API.md section 12, Stage 0.

#ifndef PROC_CAPGATE_H
#define PROC_CAPGATE_H

#include "../types.h"

// Refusal kinds. MUST stay in step with the CAPGATE_K_* consts in
// rustkern/capgate.rs; they are indices into a fixed array on both sides.
#define CAPGATE_K_INPUT_OBSERVE  0u   // SYS_GET_KEYBOARD from a non-compositor
#define CAPGATE_K_INPUT_INJECT   1u   // SYS_INJECT_KEY from a non-compositor
#define CAPGATE_K_TCP_OWNER      2u   // raw socket index that is not the caller's
#define CAPGATE_K_SHM_OWNER      3u   // shm region that is not the caller's
#define CAPGATE_K_DEV_PERM       4u   // /dev node refused by perms_check()
#define CAPGATE_K_MAX            5u

typedef struct {
    uint32_t refused[CAPGATE_K_MAX];
    uint32_t allowed[CAPGATE_K_MAX];
} capgate_stats_t;
_Static_assert(sizeof(capgate_stats_t) == 40,
               "capgate_stats_t must stay layout-identical to CapgateStats in "
               "rustkern/capgate.rs");

// --- the Rust rules (rustkern/capgate.rs) ---------------------------------

// Is `caller_pid` the latched compositor? 1/0.
//
// NON-CLAIMING. This reads the framebuffer-ownership latch; it can never set
// it. Do NOT "simplify" this to gui/fb_syscall.c's is_compositor(): that
// function CLAIMS the latch on a miss, which is correct where it lives
// (sys_fb_map IS the act of becoming the compositor) and would be a privilege
// escalation here, because whoever holds the latch may also read a pending
// elevation request and submit the password for it (proc/elevate.c).
int      capgate_is_compositor_rs(uint32_t caller_pid);

// Does a Ring-3 caller own a kernel handle addressed by a raw index?  1/0.
// Thread-group aware: a sibling pthread of the opener is admitted, an
// unrelated process is not, and an unowned (kernel) handle is refused.
int      capgate_owner_ok_rs(uint32_t owner_pid, uint32_t owner_tgid,
                             uint32_t cur_pid,   uint32_t cur_tgid);

// `tgid ? tgid : pid`. Used by BOTH the stamp sites and the check sites so the
// two cannot disagree about what a thread group is.
uint32_t capgate_tgid_of_rs(uint32_t pid, uint32_t tgid);

void     capgate_note_refusal_rs(uint32_t kind);
void     capgate_note_allowed_rs(uint32_t kind);
uint32_t capgate_refusals_rs(uint32_t kind);
int      capgate_stats_rs(capgate_stats_t *out);
uint32_t capgate_selftest_rs(void);

// --- the C glue (proc/capgate.c) ------------------------------------------

// Boot self-test driver: runs capgate_selftest_rs() and logs the result to
// serial AND the durable boot log. Every case in it is a REFUSAL.
void capgate_selftest(void);

// "Is the CURRENT process the compositor?" Wraps proc_current() so the four
// call sites do not each repeat the null check. 1/0.
int  capgate_caller_is_compositor(void);

// Ownership check for the current process against a stamped handle. Handles
// proc_current() being NULL (refuses) and normalises the caller's thread
// group. Returns 1 if allowed, 0 if refused; the CALLER does the counting so
// the refusal kind stays at the call site where it is obvious.
int  capgate_caller_owns(uint32_t owner_pid, uint32_t owner_tgid);

// Print the refusal ledger. Called from the shell `capgate` command and once
// at the end of boot, so "the gate is wired and has refused N" is observable
// on a machine with no serial port.
void capgate_report(void);

#endif // PROC_CAPGATE_H
