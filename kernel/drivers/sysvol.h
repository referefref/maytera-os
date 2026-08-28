// drivers/sysvol.h - #162: the C surface of THE system master-volume state.
//
// The state itself lives in rustkern/sysvol.rs (new kernel logic, so Rust per
// the 2026-07-16 rule). Everything that reads or writes the system volume goes
// through here: SYS_SET_VOLUME / SYS_GET_VOLUME / SYS_SET_MUTE / SYS_VOL_STATE
// in proc/syscall.c, and the media-key hotkey in cpu/isr.c. There is
// deliberately no second path; see the header of rustkern/sysvol.rs for why
// that is stated so firmly.
//
// CONTEXT RULES, because two of these are callable from a hard IRQ:
//
//   sysvol_key_rs()      SAFE IN IRQ CONTEXT. Atomics only. Returns 1 when the
//                        apply worker should be woken; the caller does the
//                        wake_up() (wake_up is irqsave-safe, sync/waitq.h).
//   sysvol_dirty_rs()    Pure read, no lock, drains nothing: legal as a
//                        wait_event() condition, which is evaluated with
//                        interrupts off.
//   sysvol_apply_rs()    TOUCHES THE CODEC. Thread context only.
//   everything else      Pure reads/atomic writes; safe anywhere.

#ifndef DRIVERS_SYSVOL_H
#define DRIVERS_SYSVOL_H

#include "../types.h"

// Media-key actions for sysvol_key_rs(). Must match ACT_* in rustkern/sysvol.rs.
#define SYSVOL_ACT_UP     0
#define SYSVOL_ACT_DOWN   1
#define SYSVOL_ACT_MUTE   2

// One media-key press. IRQ-SAFE. Returns 1 if the hardware now needs applying.
int      sysvol_key_rs(int action);

// SYS_SET_VOLUME / SYS_SET_MUTE. Return 1 if the hardware now needs applying.
int      sysvol_set_rs(int level);
int      sysvol_mute_rs(int mute);

// SYS_GET_VOLUME, and the mute flag.
int      sysvol_get_rs(void);
int      sysvol_muted_rs(void);

// SYS_VOL_STATE: level | muted<<8 | seq<<16 | keyseq<<32. See rustkern/sysvol.rs.
uint64_t sysvol_state_rs(void);

// Apply-worker plumbing.
int      sysvol_dirty_rs(void);
int      sysvol_apply_rs(void);        // thread context ONLY
uint32_t sysvol_applied_count_rs(void);

// Vector self-test over the pure state machine (0 = pass). Runs at boot.
uint32_t sysvol_selftest_rs(uint32_t *out_checks);

// -------------------------------------------------------------------------
// Kernel-side plumbing implemented in drivers/audio.c.
// -------------------------------------------------------------------------

// Called by the media-key path after sysvol_key_rs() reports a change. Wakes
// the apply worker. IRQ-SAFE (wake_up takes an irqsave spinlock).
void sysvol_wake_apply(void);

// Apply-now, for callers already in thread context (the syscall path). This is
// the SAME sysvol_apply_rs(); it exists so the tray slider never depends on
// the worker having started.
void sysvol_apply_now(void);

// Start the deferred-apply worker. Call once from main.c AFTER proc_init().
// No-op if audio never initialised, idempotent if called twice.
void sysvol_start_worker(void);

// Vector self-tests for BOTH #162 pure-logic pieces (the volume state machine
// and the HID report-descriptor parser), printed as one PASS/FAIL line each.
// Call once at boot. Touches no hardware.
void sysvol_selftest(void);

#endif // DRIVERS_SYSVOL_H
