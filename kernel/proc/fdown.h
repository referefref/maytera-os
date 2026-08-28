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
uint32_t fdown_refusals_rs(void);
uint32_t fdown_exit_marks_rs(void);
int  fdown_selftest_rs(void);

// Release every legacy fd slot the exiting process group owned, by poisoning
// each to DEAD so a future process handed the same pid cannot inherit it.
// Defined in proc/fdlayer.c (it owns the tables), called from proc_exit().
// Runs under cli() on the dying process's own stack.
void fdown_proc_exit(uint32_t owner);

// Boot-time: run the self-test and prove the Rust slot count agrees with
// LEGACY_MAX_FDS. Defined in proc/fdlayer.c, called from main.c.
void fdown_boot_check(void);

// #fdguard: DEV-ONLY runtime bypass accessors. fdguard_bypass() is read by
// the guards; fdguard_set_bypass() is called once at boot by the fdgtest
// worker when /CONFIG/FDGUARD.BYPASS is present. Never armed in production.
int  fdguard_bypass(void);
void fdguard_set_bypass(int on);

#endif // MAYTERA_PROC_FDOWN_H
