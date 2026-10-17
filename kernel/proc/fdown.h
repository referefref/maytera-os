// proc/fdown.h - #fdguard: the C view of rustkern/fdown.rs, the ownership guard
// for the SYSTEM-WIDE legacy fd tables in proc/fdlayer.c.
//
// The tables themselves (fd_table[]/e2fd[]/smbfd[]) stay in fdlayer.c, in C,
// where the filesystem code that fills them lives. What lives in Rust is the
// part that was missing and is pure decision: WHICH process may touch a slot.
// See rustkern/fdown.rs for the defect this closes and the owner-word layout.
//
// There is no C twin and therefore no -DRUST_* strangler flag: this is new
// logic, not a port, so the rollback is reverting the commit.
#ifndef MAYTERA_PROC_FDOWN_H
#define MAYTERA_PROC_FDOWN_H

#include "../types.h"

// fdown_check_rs() return codes, mirrored from rustkern/fdown.rs.
#define FDOWN_R_OK        0   // the caller owns this live slot: proceed
#define FDOWN_R_FREE     -1   // nobody owns it: let the existing used-check answer
#define FDOWN_R_NOTOWNER -2   // owned by someone else (or DEAD): refuse + audit

int  fdown_slots_rs(void);                       // == LEGACY_MAX_FDS (asserted)
int  fdown_claim_rs(uint32_t slot, uint32_t owner);
int  fdown_release_rs(uint32_t slot);
int  fdown_check_rs(uint32_t slot, uint32_t owner);
uint32_t fdown_owner_rs(uint32_t slot);
int  fdown_mark_dead_if_owner_rs(uint32_t slot, uint32_t owner);
// #fdreclaim: claim the teardown of a slot `owner` leaked. 1 = this call owns
// it and must free its buffers then fdown_release_rs() it; 0 = not ours.
int  fdown_take_for_reap_rs(uint32_t slot, uint32_t owner);
// 0 free, 1 live, 2 dead (leaked), 3 being reclaimed. Diagnostics/audit only.
int  fdown_state_rs(uint32_t slot);
uint32_t fdown_refusals_rs(void);
uint32_t fdown_exit_marks_rs(void);
uint32_t fdown_reaped_rs(void);
int  fdown_selftest_rs(void);

// Release every legacy fd slot the exiting process group owned, by poisoning
// each to DEAD so a future process handed the same pid cannot inherit it.
// Defined in proc/fdlayer.c (it owns the tables), called from proc_exit().
// Runs under cli() on the dying process's own stack.
void fdown_proc_exit(uint32_t owner);

// #fdreclaim: RECLAIM every legacy fd slot the now-gone thread group leaked,
// freeing the read/write windows they hold, and return them to the pool.
// Poisoning (above) only made them unreachable; this is what makes them
// reusable. Called from proc_exit() on EVERY exit, leader or worker thread: it
// only acts once the group has no live member left, which is the condition that
// makes freeing the buffers safe. Defined in proc/fdlayer.c (it owns the
// tables). Runs under cli(); frees but never blocks. See the block comment
// there for the full safety argument and for what it deliberately does not do.
void fdown_proc_reclaim(uint32_t owner);

// #fdreclaim: how many of the LEGACY_MAX_FDS-3 usable slots are free now. The
// instrument the gated oracle samples; a ceiling that falls and never recovers
// is the signature of a slot leak.
int  legacy_fd_free_slots(void);

// #fdreclaim: DEV-ONLY. Disable the reclaim for this boot (/CONFIG/FDRECLAIM.OFF),
// restoring the pre-fix leak so one build can be measured both ways.
void fdreclaim_set_disabled(int off);

// Boot-time: run the self-test and prove the Rust slot count agrees with
// LEGACY_MAX_FDS. Defined in proc/fdlayer.c, called from main.c.
void fdown_boot_check(void);

// #fdguard: DEV-ONLY runtime bypass accessors. fdguard_bypass() is read by
// the guards; fdguard_set_bypass() is called once at boot by the fdgtest
// worker when /CONFIG/FDGUARD.BYPASS is present. Never armed in production.
int  fdguard_bypass(void);
void fdguard_set_bypass(int on);

#endif // MAYTERA_PROC_FDOWN_H
