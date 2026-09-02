// cpu/inputlat.h - #affinity: INPUT-TO-PRESENT LATENCY, the responsiveness
// instrument. Implementation is Rust (rustkern/inputlat.rs), per the
// all-new-kernel-code-in-Rust rule; read that file for what each stage means
// and, importantly, for what S_PRESENT does NOT measure.
//
// THE ONE-LINE VERSION. Every performance number this kernel had was
// throughput. This is the first one that measures what a user feels: how long
// from a key arriving to the screen changing. Three stages, because a single
// composite number would hide which half moved:
//   0 S_WAIT     key queued -> consumer dequeued it   (SCHEDULER latency)
//   1 S_INPUT    scancode arrived -> consumer dequeued it
//   2 S_PRESENT  scancode arrived -> first present after that dequeue
// Stages 0 and 1 are exact. Stage 2 is a LOWER BOUND on true input-to-photon.
#ifndef CPU_INPUTLAT_H
#define CPU_INPUTLAT_H

#include "../types.h"

#define INPUTLAT_S_WAIT     0u
#define INPUTLAT_S_INPUT    1u
#define INPUTLAT_S_PRESENT  2u
#define INPUTLAT_STAGES     3u
#define INPUTLAT_BUCKETS    24u

// The three hooks. All three are wait-free, allocation-free and lock-free, so
// they are safe in an ISR and inside sys_fb_flip()'s interrupts-off window
// (#426). Each is one relaxed atomic store plus a fetch_add on the common path.
void     inputlat_scancode_rs(uint64_t t0_us);   // cpu/isr.c keyboard_process_scancode()
void     inputlat_push_rs(void);                 // cpu/isr.c kb_push(), ACCEPT branch only
void     inputlat_deliver_rs(uint64_t t_us);     // cpu/isr.c keyboard_get_char()
void     inputlat_present_rs(uint64_t t_us, uint64_t damage_area);  // gui/fb_syscall.c sys_fb_flip()

// Readout.
int32_t  inputlat_stage_rs(uint32_t stage, uint64_t *n, uint64_t *p50,
                           uint64_t *p95, uint64_t *max, uint64_t *min,
                           uint64_t *mean);
void     inputlat_counts_rs(uint64_t *scancodes, uint64_t *pushed,
                            uint64_t *delivered, uint64_t *closed,
                            uint64_t *coalesced, uint64_t *ring_over,
                            uint64_t *last_area);
uint64_t inputlat_bucket_rs(uint32_t stage, uint32_t b);
uint64_t inputlat_bucket_lo_rs(uint32_t b);
void     inputlat_reset_rs(void);

// Self-test, and the negative control that proves the self-test can fail.
int32_t  inputlat_selftest_rs(void);
int32_t  inputlat_selftest_negative_rs(void);

// THE DELIBERATE NEGATIVE CONTROL for the LIVE path (cpu/isr.c).
//
// A self-test cannot prove the stamps are wired up, because it calls the
// accounting functions itself. Setting this to N busy-delays N REAL
// microseconds inside keyboard_get_char() BEFORE the delivery timestamp is
// taken, so S_WAIT and S_INPUT must both rise by ~N and S_PRESENT with them.
// If they do not move, the hooks are not on the live path and every number
// this module reports is fiction.
//
// DEFAULT 0, which makes the whole thing a single predictable-branch load per
// key drain. Non-zero is a measurement mode, never a shipping state, and
// nothing sets it except the operator.
extern volatile uint64_t g_inputlat_inject_us;

// Rust u64/u32/i32 must match this kernel's types exactly across the FFI. Lock
// it in rather than trusting it, as every Rust seam in this tree does.
_Static_assert(sizeof(uint64_t) == 8, "inputlat: u64 FFI width");
_Static_assert(sizeof(uint32_t) == 4, "inputlat: u32 FFI width");
_Static_assert(sizeof(int32_t)  == 4, "inputlat: i32 FFI width");

#endif
