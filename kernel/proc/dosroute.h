// dosroute.h - THE one decision about where a DOS guest runs (#67/#168).
//
// WHY THIS EXISTS. With a DOS guest the Big Kernel Lock is held ~93% of wall
// clock with SMP off and ~97% with SMP on, because the interpreter simply IS a
// Ring-0 kernel thread. Turning SMP on therefore makes the machine SLOWER, not
// faster: measured on the ASUS i7-4720HQ config, compositor flips fell 22.9/s
// -> 3.1/s and the idle cores burned 9.2e7 pause-spins/s waiting for the lock.
// Running the SAME interpreter in Ring 3 (/APPS/DOSUSER) drops BKL to 3.7% and
// lifts the Amdahl ceiling from 1.02x to 3.59x; under a parallel load that was
// 29.8x the guest instruction rate of the SMP-off baseline.
//
// So the payoff is one switch away, and this file is the switch. The routing
// decision is made in ONE place, behind SYS_DOS_RUN, which is where all five
// Ring-3 launch call sites across four subsystems (Start menu x2, terminal,
// AI client) already converge. No userland change is needed for any of them.
//
// WHAT IS DELIBERATELY *NOT* ROUTED, and it is not an oversight:
//   dos_launch_kernel()  (/CONFIG/DOSRUN.CFG)   - always in-kernel
//   dosring3_start_...() (/CONFIG/DOSRING3.CFG) - always Ring 3
// Those two have exactly one caller each and are the two arms of the
// differential test harness that proves the Ring-3 port preserves behaviour.
// Routing them would destroy the only oracle the port has, while looking like
// thoroughness. They stay pinned.
//
// THE SHIPPING DEFAULT IS IN-KERNEL. An absent /CONFIG/DOSROUTE.CFG, an
// unreadable one, and a zeroed policy all mean "everything in-kernel", so the
// no-config state needs no special case anywhere.
#ifndef DOSROUTE_H
#define DOSROUTE_H

#include "../types.h"
#include "process.h"

// Mirrors rustkern/dospolicy.rs. The Rust side owns the definition; these
// _Static_asserts below are what stops the two drifting.
#define DOSROUTE_KERNEL     0
#define DOSROUTE_RING3      1
#define DOSPOL_MAX_RULES    32
#define DOSPOL_PAT_CAP      64
#define DOSPOL_PROG_CAP     256

typedef struct {
    int32_t  valid;
    int32_t  default_ring3;
    int32_t  n;
    int32_t  n_bad;
    int32_t  mode[DOSPOL_MAX_RULES];
    uint8_t  pat[DOSPOL_MAX_RULES][DOSPOL_PAT_CAP];
} dos_policy_t;

// #[repr(C)] on the Rust side gives 4-byte alignment and no padding here:
// 4 i32 = 16, [i32;32] = 128, [[u8;64];32] = 2048. Total 2192.
_Static_assert(sizeof(dos_policy_t) == 2192,
               "dos_policy_t must match DosPolicy in rustkern/dospolicy.rs");
_Static_assert(sizeof(((dos_policy_t *)0)->mode) == 32 * 4,
               "DOSPOL_MAX_RULES must match dospolicy.rs");
_Static_assert(sizeof(((dos_policy_t *)0)->pat) == 32 * 64,
               "DOSPOL_PAT_CAP/MAX_RULES must match dospolicy.rs");
_Static_assert(__builtin_offsetof(dos_policy_t, mode) == 16,
               "DosPolicy field order changed");

// Rust FFI (rustkern/dospolicy.rs).
int      dospolicy_parse_rs(const uint8_t *buf, uint32_t len, dos_policy_t *out);
int      dospolicy_route_rs(const dos_policy_t *pol, const uint8_t *prog, int *out_rule);
int      dospolicy_rule_text_rs(const dos_policy_t *pol, int idx, uint8_t *out, uint32_t outlen);
uint32_t dospolicy_selftest_rs(void);
uint32_t dospolicy_selftest_red_rs(void);

// THE routed launch. This is what SYS_DOS_RUN calls. `line` is a whole launch
// line, "<path>[ <command tail>]"; the routing decision is made on the program
// half only. Returns 0 on launch, <0 on refusal - the same contract
// dos_launch() has, so the syscall\x27s return value is unchanged.
int dosroute_launch(const char *line);

// Spawn the Ring-3 DOS host for `line` under `ident`. ONE definition of
// "spawn DOSUSER", shared by the routed launch (proc_as_caller) and the
// /CONFIG/DOSRING3.CFG differential harness (proc_as_session). Returns the new
// pid, or <0.
int dosroute_spawn_ring3(const char *line, proc_ident_t ident);

// Boot-time self-test of the policy matcher, printed by main.c.
void dosroute_selftest(void);

#endif // DOSROUTE_H
