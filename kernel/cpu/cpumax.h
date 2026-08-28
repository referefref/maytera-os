// cpumax.h - THE definition of how many CPUs MayteraOS supports. (#143)
//
// There is exactly ONE number here and everything that is per-CPU derives from
// it: per-CPU data, run queues, per-core counters, CPU bitmasks, the MADT
// enumeration array and the parallel-for fan-out.
//
// WHY THIS FILE EXISTS. Before #143 there were FIVE independent answers to
// "how many CPUs", in three files and two languages, and no two of them agreed:
//
//   SCHED_RQ_CPUS      8    proc/process.c   run queues + every per-core
//                                            scheduler array. Also the real
//                                            cap on which cores may schedule
//                                            user work at all: sched_rq_cpu()
//                                            folds any id >= 8 onto queue 0
//                                            and sched_rq_set_consumer()
//                                            silently ignores it.
//   (bare literal)    16    cpu/smp.c        smp_parallel_for chunk fan-out,
//                                            with ch[16]/done[16] sized by an
//                                            unnamed literal.
//   SMP_MAX_CPUS     256    cpu/smp.h        per_cpu_data[], cpu_local[],
//                                            heartbeats, per-core percentages.
//   MAX_CPUS         256    drivers/acpi_madt.h  the parsed MADT CPU array.
//   CPUOBS_MAX_CPUS   32    rustkern/cpuobs.rs   the Rust core observer, whose
//                                            doc comment claimed it "Matches
//                                            SCHED_RQ_CPUS on the C side". It
//                                            did not: 32 against 8.
//
// The disagreement was not itself a crash. Every indexed write was range-checked
// against whichever of the five constants was in scope, so nothing went out of
// bounds. What it produced instead was SILENT TRUNCATION with no log line: a
// machine with 12 cores would enumerate all 12 into per_cpu_data[], report 12,
// and then schedule user work on 8 of them, because the ninth core folds onto
// queue 0 and can never register as a consumer of its own.
//
// WHY 32.
//
//   * The target machine is an iMac14,4: Core i5-4570R, 4 cores, 4 threads.
//     The test host is a 4-vCPU VM. 32 is eight times the largest thing this
//     OS is aimed at, so truncation is not reachable on any real target.
//   * A cap that is generous costs memory; a cap that silently truncates costs
//     a debugging session. Going 256 -> 32 SHRINKS the static per-CPU arrays
//     (per_cpu_data[] and cpu_local[] were sized for 256 CPUs that cannot
//     exist) by more than the scheduler arrays grow going 8 -> 32. The net
//     static cost of this change is negative; see the CHANGELOG for measured
//     .bss numbers.
//   * It keeps the on-stack per-core snapshots in the scheduler small. These
//     live on the interrupt path (sched_rq_pop() runs inside the timer ISR),
//     so they are the one place where a generous cap is genuinely expensive:
//     at 32 the largest snapshot frame is well under 1 KB of a 16 KB kernel
//     stack. At 256 it would have been several KB and this would be a
//     stack-overflow ticket instead.
//
// WHY THE MASK LIMIT IS A SEPARATE, ENFORCED NUMBER.
//
// CPU sets in this kernel are single-word bitmasks (g_rq_consumers in
// proc/process.c, SEEN_MASK in rustkern/cpuobs.rs). Shifting by a bit position
// >= the width of the shifted type is UNDEFINED BEHAVIOUR in C: it does not
// warn, it does not trap, and on x86 it will appear to work because the shift
// instruction masks the count to 6 bits, so bit 64 aliases bit 0. A CPU set
// that silently aliases core 64 onto core 0 is a scheduler that strands work
// for reasons no log line will ever explain.
//
// Those masks are now cpumask_t/AtomicU64, and the assertion below is what
// makes that safe FOREVER rather than safe TODAY. The pre-#143 code was also
// correct today: every `1u << cpu` was guarded by a check against
// SCHED_RQ_CPUS == 8. The bug was that nothing connected the guard to the mask
// width, so the UB was one edit away, and raising the cap is exactly the edit
// this ticket was asked to make. Raising MAYTERA_MAX_CPUS past 64 now FAILS
// THE BUILD with the message below instead of quietly aliasing cores.
//
// DO NOT add a second definition of this number anywhere. This project has
// shipped five version.h files, two Task Managers and two g_wallpapers[]
// arrays; a duplicated constant is not a hypothetical failure mode here.

#ifndef CPUMAX_H
#define CPUMAX_H

// Maximum number of logical CPUs MayteraOS supports. See above before changing.
#define MAYTERA_MAX_CPUS 32

// A CPU set: one bit per logical CPU. Wide enough for MAYTERA_MAX_CPUS by the
// assertion below, which is the only thing keeping the shifts defined.
typedef unsigned long long cpumask_t;

#define CPUMASK_BIT(cpu)  ((cpumask_t)1u << (cpu))

_Static_assert(MAYTERA_MAX_CPUS <= 64,
    "MAYTERA_MAX_CPUS exceeds the width of cpumask_t. CPU sets in this kernel "
    "are single-word bitmasks (g_rq_consumers, rustkern/cpuobs.rs SEEN_MASK). "
    "Shifting by a bit index >= the type width is undefined behaviour, and on "
    "x86 it aliases high cores onto low ones SILENTLY. Widen cpumask_t and "
    "both masks to a real bit-array before raising this number.");

_Static_assert(MAYTERA_MAX_CPUS >= 1, "MAYTERA_MAX_CPUS must be at least 1");

#endif // CPUMAX_H
