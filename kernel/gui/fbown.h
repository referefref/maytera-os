// gui/fbown.h - #745 (task #59): the C view of rustkern/fbown.rs, the
// ownership + lifetime state machine for the single framebuffer claim.
//
// The framebuffer MAPPING (page tables, damage rects, flips) stays in
// gui/fb_syscall.c, in C, where the VMM calls already live. What moved to Rust
// is the part that was wrong and is pure decision: WHO may hold the screen, and
// WHEN the hold ends. See rustkern/fbown.rs for the rule and the defect.
//
// There is no C twin and therefore no -DRUST_* strangler flag: this is new
// logic, not a port, so the rollback is reverting the commit rather than
// dropping a define.
//
// THE ONE-LINE VERSION: the claim is ARMED by the code that launches
// /APPS/COMPOSIT, CLAIMED by that process's first sys_fb_map(), and RELEASED at
// the proc_exit() chokepoint. Between a compositor dying and the next one being
// launched, nothing can claim the screen at all.

#ifndef MAYTERA_GUI_FBOWN_H
#define MAYTERA_GUI_FBOWN_H

#include "../types.h"

// Mirrors FbownStats in rustkern/fbown.rs. sizeof-locked on both sides: a
// field added on one side and not the other fails the build here and the
// `const _: () = assert!(...)` there.
typedef struct {
    uint32_t owner;                 // owning pid, 0 = unclaimed
    uint32_t expect;                // pid the open window is narrowed to, 0 = any
    uint32_t armed;                 // 1 = a claim window is open
    uint32_t claims;                // successful claims this boot
    uint32_t releases;              // releases via the proc_exit() hook
    uint32_t stale_cleared;         // backstop releases (owner found dead)
    uint32_t refused_unarmed;       // claims refused: no window open
    uint32_t refused_not_expected;  // claims refused: window is another pid's
    uint32_t refused_not_owner;     // claims refused: somebody else owns it
} fbown_stats_t;
_Static_assert(sizeof(fbown_stats_t) == 36,
               "fbown_stats_t must stay layout-identical to FbownStats in "
               "rustkern/fbown.rs");

// --- the Rust state machine (rustkern/fbown.rs) ----------------------------
int      fbown_arm_rs(uint32_t pid);
int      fbown_disarm_rs(void);
int      fbown_claim_rs(uint32_t pid);
int      fbown_is_owner_rs(uint32_t pid);
uint32_t fbown_owner_rs(void);
int      fbown_release_rs(uint32_t pid);
void     fbown_note_stale_rs(void);
void     fbown_reset_rs(void);
int      fbown_stats_rs(fbown_stats_t *out);
int      fbown_selftest_rs(void);

// --- the C glue (gui/fb_syscall.c), for callers outside this file ----------

// Open the claim window for `pid`, or for "the next claimant" when pid == 0.
// gui/desktop.c calls it with 0 immediately BEFORE launching /APPS/COMPOSIT
// and with the launched pid immediately after; arming first is what makes the
// pair race-free against a compositor scheduled on another core.
void fb_owner_arm(uint32_t pid);

// Shut a window that will never be used (the compositor failed to launch).
void fb_owner_disarm(void);

// Does this pid hold the framebuffer? proc/elevate.c's compositor gate.
int fb_owner_is(uint32_t pid);

// The owning pid, or 0. proc/process.c's idle loop uses "0 == no userland
// compositor" to decide whether the kernel desktop still needs ticking.
uint32_t fb_owner_pid(void);

// PROCESS EXIT HOOK. Called from proc_exit() (proc/process.c) for EVERY dying
// process; a no-op unless this one held the claim. Runs under cli() on the
// dying process's own stack, so it must not block: it does not.
void fb_owner_proc_exit(uint32_t pid);

// Boot-time: run the self-test and print the verdict. Called from main.c.
void fbown_boot_check(void);

#endif // MAYTERA_GUI_FBOWN_H
