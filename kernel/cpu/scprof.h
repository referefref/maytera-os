// scprof.h - #121: per-syscall time census and phase attribution.
//
// See rustkern/scprof.rs for why this exists and what each recorded field
// means. In short: #118 proved proc/syscall.asm:89 holds the Big Kernel Lock
// for the whole syscall, but its instrument keys on the acquire's return
// address, which EVERY syscall shares, so it names the doorway and never the
// room. This keys on the syscall number and on named phases.
//
// COST. Two mono_us() reads (a TSC read and a multiply-shift) and two 80-byte
// array copies per syscall, plus two mono_us() reads per phase probe. No shared
// cacheline, no lock, no I/O.
#ifndef SCPROF_H
#define SCPROF_H

#include "../types.h"

// Phase ids. MUST stay in step with SCP_PHASE_N in rustkern/scprof.rs and with
// g_scp_phase_name[] in cpu/scprof.c. A drifted id does not fail loudly, it
// silently attributes time to the wrong name, so the count is asserted at
// compile time in scprof.c and the FFI struct size is locked.
#define SCP_PREFAULT     0   // mm_prefault_range() ahead of the copy (sys_read)
#define SCP_FILEREAD     1   // file_read(): the VFS/pty/pipe/socket fd path
#define SCP_EXT2FILL     2   // ext2 read-window refill loop in sys_read
#define SCP_EXT2COPY     3   // the memcpy out of that window into user memory
#define SCP_FATREAD      4   // fat_read() legacy-fd fallback
#define SCP_BLKREAD      5   // blk_read(): the whole block layer, cache included
#define SCP_USBMSC       6   // usb_msc_read(): device I/O only, cache excluded
#define SCP_SPAWNLOAD    7   // spawn_impl: fat_read_file() of the whole binary
#define SCP_SPAWNCREATE  8   // spawn_impl: proc_create_user_as() (ELF + address space)
#define SCP_ELFLOAD      9   // elf_load_user_named(): segment map + copy + bss
#define SCP_VMMSTACK    10   // vmm_alloc_user_pages() for the new user stack
#define SCP_PMMALLOC    11   // pmm_alloc_page(), cumulative: the physical allocator
#define SCP_PHASE_N      12

// Saved on the caller's C stack across a syscall or a phase. Nesting therefore
// costs nothing and needs no depth counter: an inner call restores the outer's
// snapshot on the way out. That matters because a syscall that never returns
// (SYS_EXIT) or that resumes at proc/syscall.asm's fork-child label would leave
// a per-cpu depth counter permanently wrong, and a permanently wrong depth
// counter silently mis-attributes every later syscall on that core.
typedef struct { uint64_t t0, i0, b0, ph[SCP_PHASE_N]; } scp_frame_t;
typedef struct { uint64_t t0; } scp_span_t;

// Syscall bracket. scp_enter() snapshots; scp_exit() records the delta.
scp_frame_t scp_enter(void);
void        scp_exit(uint64_t num, scp_frame_t f);

// Phase bracket. Cheap enough to put inside a loop body.
scp_span_t  scp_begin(void);
void        scp_end(int phase, scp_span_t s);

// One store, from the ISR wrapper, before it decides anything about the BKL, so
// the witness works whether whole-kernel locking is on or off.
void        scp_irq_tick(void);

// Boot-time proof that the probe reads a KNOWN duration. Prints [SCPROF-PROBE].
void        scp_selftest(void);

// Periodic serial report; called from sched_smp_report() with the BKL dropped.
void        scp_report(void);

#endif
