// cpu/tickack.h - #tickdead: acknowledge the tick BEFORE the lock, and measure
// how long the acknowledgement actually took.
//
// See rustkern/tickack.rs for the defect, the scope decision and the blame
// rule. This header is the C surface: the pre-dispatch hook cpu/idt.c calls,
// the EOI witness the tick handlers call, and the counters that carry the
// evidence to /BOOTLOG.TXT.
//
// WHY THE HOT PATH IS C AND NOT RUST. The all-new-kernel-code-in-Rust rule
// allows C where the code is genuinely entangled with asm and port-level
// hardware access. tick_ack_pre_dispatch() is `outb` to the 8259, a memory-
// mapped Local APIC write and one `rdtsc`, executed with interrupts masked from
// inside an IDT gate, on every timer interrupt. Every DECISION it makes - which
// vectors qualify, what counts as a late acknowledgement, and whether a silent
// clock is BLOCKED or ABSENT - is in rustkern/tickack.rs with self-tests, and
// this file only executes them. That is the same split, for the same reason, as
// cpu/tickwatch.c against rustkern/tickwatch.rs.
#ifndef MAYTERA_CPU_TICKACK_H
#define MAYTERA_CPU_TICKACK_H

#include "../types.h"

// ---------------------------------------------------------------------------
// THE ARM SWITCH. One binary, two arms.
//
// 1 (default, and the only shipping value): the tick vectors are acknowledged
//   in tick_ack_pre_dispatch(), BEFORE isr_handler() contends for the BKL.
// 0: the pre-fix ordering, in which each tick handler sends its own EOI after
//   the lock wait. Selected by /CONFIG/TICKEOI.CFG holding "0", so the fixed and
//   broken arms can be compared on ONE kernel.elf rather than on two builds that
//   differ in more than the thing under test.
//
// Read on the hot path; written once, from the boot path, before any user
// process exists.
extern volatile int g_tick_early_eoi;

// ---------------------------------------------------------------------------
// Hot path
// ---------------------------------------------------------------------------

// Called from isr_handler() (cpu/idt.c) on EVERY interrupt, after the stack
// guard and BEFORE bkl_acquire(). A no-op for every vector that is not one of
// the three timer vectors: two compares and a return.
void tick_ack_pre_dispatch(uint64_t vec);

// Called immediately after the EOI for a tick vector, from whichever side
// actually sent it. Closes this frame's entry-to-acknowledge measurement and
// clears its in-flight mark.
//
// A frame that never reaches this call leaves its in-flight mark SET, and that
// is deliberate rather than a leak: an interrupt that was delivered and never
// acknowledged is exactly the fault this file exists for, and a stuck count is
// its fingerprint. tick_blame_rs() reads it to say BLOCKED rather than ABSENT.
void tick_ack_note_eoi(uint64_t vec);

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

// Frames for the 8259 IRQ0 vector currently between IDT entry and EOI, summed
// over all cores. Healthy: 0 or 1. Persistently non-zero: the native tick is
// not dead, it is being held by this kernel.
uint32_t tick_ack_inflight_pic(void);

// Longest entry-to-acknowledge delay, in microseconds, since the last call.
// READ-AND-RESET, so each heartbeat record describes its own interval: a
// lifetime maximum set once during boot hides everything after it.
uint64_t tick_ack_max_us_take(void);

// Lifetime figures, for the boot report and the panic path.
extern volatile uint64_t g_tick_ack_max_us;      // worst ever seen
extern volatile uint64_t g_tick_ack_worst_vec;   // ...and on which vector
extern volatile uint64_t g_tick_ack_late_n;      // acks that cost at least a tick
extern volatile uint64_t g_tick_ack_n;           // tick-vector frames seen

// Compact field for the [HB] heartbeat line. Returns bytes written.
int tick_ack_hb_field(char *buf, uint32_t cap);

// Runs the Rust self-test and prints the arm. Called once from the boot path.
void tick_ack_boot_report(void);

// Reads /CONFIG/TICKEOI.CFG and selects the arm. Called once, after the root
// filesystem is mounted. Absent means the shipping (fixed) arm.
void tick_ack_read_gate(void);

// ---------------------------------------------------------------------------
// #tickdead FAULT INJECTION. `make TICKACKFAULT=1`. Compiled out of the golden.
//
// WHY IT EXISTS. The campaign that measured this defect reproduced the
// MECHANISM (entry-to-EOI delays of 456 ms and 625 ms, 50,606 acknowledgements
// costing at least a tick each) but never the terminal event: across 1,083
// judged windows in eight boots, `tickwatch`'s own verdict was `ok` every
// single time, in BOTH arms. That is not luck, it is the instrument: tick
// reinjection CONSERVES the count, so a window that lost ticks in the middle
// and got them back in a burst still averages 1000 permille. `[TICKSRC] NATIVE
// TICK DEAD` needs one stall to cover a WHOLE window, which is a tail event.
//
// So the new BLOCKED-versus-ABSENT attribution in cpu/tickwatch.c was linked
// and never executed, which this project has a documented name for. This
// manufactures the tail deterministically.
//
// WHAT IT INJECTS, AND WHY THIS SHAPE. It stalls ONE IRQ0 frame, exactly where
// bkl_acquire() sits, for TICKACK_FAULT_MS. It is faithful in the two respects
// that decide the outcome:
//   * position: after tick_ack_pre_dispatch(), before dispatch. That is the
//     window whose length the fix removes.
//   * interrupts ON during the stall, because bkl_take_locked()'s wait loop
//     executes `sti`. A stall with IF=0 would starve the clock in BOTH arms and
//     prove nothing, which is the "control arm identical to the test arm"
//     mistake this tree records.
// ONE-SHOT, and that is load-bearing rather than tidy: with the fix in place
// IRQ0 keeps arriving during the stall, so a re-entrant version would nest a
// frame every 4 ms for the whole stall and reproduce #rahang's stack overflow
// instead of this ticket's defect.
#ifndef TICKACK_FAULT_MS
#define TICKACK_FAULT_MS    3000u
#endif
#ifndef TICKACK_FAULT_AT_MS
#define TICKACK_FAULT_AT_MS 60000u
#endif
#ifdef TICKACK_FAULT_TEST
void tick_ack_fault_stall(uint64_t vec);
extern volatile uint64_t g_tick_ack_fault_fired;
#endif

#endif // MAYTERA_CPU_TICKACK_H
