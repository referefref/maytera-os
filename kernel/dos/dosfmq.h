// dosfmq.h - THE SEAM between the DOS interpreter and the ONE FM event queue.
//
// (#fmbridge) WHY THIS FILE EXISTS.
//
// dos/dosexec.c is compiled TWICE from these exact bytes: into kernel.elf, and
// into /APPS/DOSUSER, the Ring-3 DOS host (userland/apps/dosring3). Until this
// file existed it reached into the FM queue as a VARIABLE - `g_dos_fmq`, a
// file-scope static in dosexec.c itself, taken under `g_dos_fmq_lock`.
//
// That is correct in Ring 0 and unfixable in Ring 3, for exactly the reason
// #flipfix names: a variable access cannot become a syscall. The Ring-3 host
// therefore had its OWN copy of the queue in its OWN address space, correctly
// filled by the guest and drained by nobody, while /APPS/FMSYNTH went on
// draining the KERNEL's queue through SYS_DOS_FM_EVENTS and found it empty.
// Every OPL2 register write a Ring-3 guest made was carried faithfully into a
// buffer that had no consumer. fm_launch_synth() returned -1 to keep the chip
// honestly ABSENT rather than advertise a synthesiser with nothing behind it,
// so the visible symptom was Ring-3 DOS guests have no music.
//
// So the access becomes a CALL. Ring 0 answers it in dos/dosfmq.c, which now
// owns the queue outright. Ring 3 answers it in
// userland/apps/dosring3/shim/kshim.c, which forwards to the SAME kernel queue
// through SYS_DOS_FM_HOST. There is still exactly ONE FM event queue in the
// system, one drop counter, one `active` flag and one teardown path, which is
// what stops the two DOS paths from drifting into different music.
//
// PUSH, NOT PULL, AND THE flipfix ARGUMENT DOES NOT INVERT IT.
// #flipfix chose pull for the framebuffer flip counter because a counter has a
// CURRENT VALUE that can be sampled at any moment, so a pushed copy of it goes
// silently stale the instant the pusher dies. An OPL2 register write has no
// current value: it is a discrete occurrence, and nothing can pull an event
// that has not happened yet. The failure mode pull was protecting against is
// therefore absent here, and its analogue is handled explicitly: a producer
// that stops is not a stale reading, it is the `active` flag being cleared,
// which reaches the consumer as DOS_FM_ENODEV and is how FMSYNTH learns to
// render its tail and exit.
//
// A pull design would also have had to INVENT a transport. FMSYNTH and DOSUSER
// are two Ring-3 address spaces; anything FMSYNTH could pull from would be a
// second kernel-mediated buffer beside the one that already exists. That is the
// fork CLAUDE.md forbids, and it would have given the system two places to
// learn that a guest had gone away.
//
// COST IS NOT WHAT DECIDED IT, and could not have been: Mutant Space Bats of
// Doom, an OPL2 title whose audio is entirely FM music, makes 179 OPL2 register
// writes in a whole session. At ~134 ns per syscall that is 24 microseconds of
// syscall for 145 seconds of music.
#ifndef MAYTERA_DOS_DOSFMQ_H
#define MAYTERA_DOS_DOSFMQ_H

#include "../types.h"

// Ring-0: dos/dosfmq.c.   Ring-3: apps/dosring3/shim/kshim.c.
// Every signature below is primitive-typed on purpose: the queue STRUCT stays
// private to its owner, so a Ring-3 build cannot accidentally instantiate one.

// Arm the queue for a guest that is starting. Idempotent.
void     dos_fmq_host_open(void);

// Is the queue accepting writes? The cheap pre-check on the guest's hot path.
int      dos_fmq_host_active(void);

// One OPL2 register write, timestamped by the caller with mono_us(). Never
// waits, never allocates; drops (and counts the drop) when the ring is full.
void     dos_fmq_host_push(uint8_t reg, uint8_t val, uint64_t t_us);

// The guest is gone. Closes the queue and reports the session totals. The
// consumer drains what is left FIRST and only then sees ENODEV, so the final
// note-off of a session is never lost.
void     dos_fmq_host_close(uint32_t *pushed, uint32_t *dropped);

// Live counters for the periodic profile line. Any pointer may be NULL.
void     dos_fmq_host_stats(uint32_t *pushed, uint32_t *dropped,
                            uint32_t *peak, uint32_t *used);

// The queue depth the peak above is out of. A function, not a macro, so the
// number has ONE definition and the Ring-3 host reports the capacity of the
// queue it is really feeding rather than of a constant it was compiled with.
uint32_t dos_fmq_host_capacity(void);

// Run rustkern/fmq.rs's self-test against the real queue and leave it OPEN
// (the self-test closes it as a postcondition; the knowledge that a guest is
// starting lives with the caller). Returns the number of failing checks.
int      dos_fmq_host_selftest(void);

// A process exited. Releases the drain latch if it held it; closes the queue if
// it was the Ring-3 producer that opened it. Returns 1 if either applied.
// Ring 3 has no view of process exits, so its implementation is a no-op and
// says so: the kernel observes them, on both paths.
int      dos_fmq_host_release_pid(uint32_t pid);

#endif // MAYTERA_DOS_DOSFMQ_H
