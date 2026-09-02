// blockdev.c - #307 block-device routing (ATA disk vs USB Mass Storage root)
//              + #375 USB "to-RAM" root so a slow USB stick is read once.
//              + #417 size that RAM copy off ACTUAL USED filesystem data
//                instead of raw device/partition capacity (see blk_try_toram).
//
// Single choke point for FS-layer sector I/O. Default = ATA (no behavior change
// for existing ATA-disk VMs). When a USB thumb drive carrying a valid MayteraOS
// layout is selected as root at boot, all FAT/ext2 sector reads/writes route to
// that USB MSC device instead, enabling a USB-only boot on real hardware.
//
// #375 TO-RAM ROOT (the slow-image-loading fix): the FAT/exFAT/ext2 drivers read
// the medium one 512-byte sector at a time with no cache (FAT-table walks, dir
// scans, file cluster reads), so on a SLOW USB stick (the real iMac) opening any
// image/app is a long stream of tiny USB round-trips and feels sluggish; VMs hide
// it because their virtual USB is instant. The clean fix is to copy the ENTIRE
// root block device into a RAM region ONCE at boot - with LARGE sequential
// multi-sector transfers - and then serve every subsequent sector read from RAM
// at memory speed, never touching the stick again for reads. The image is ~904 MB
// and the machine has 16 GB, so it fits trivially. Writes are WRITE-THROUGH (RAM
// + stick): the RAM copy stays coherent so reads remain instant, and on-disk data
// (settings, notes, and crucially the /HEARTBEAT.TXT + /BOOTLOG.TXT diagnostics)
// still persists to the physical stick so it survives a power-off. If the full
// RAM image does not fit (or the device is too large), we transparently fall back
// to a bounded demand block cache (still a big win for repeated reads). Both are
// active ONLY for a USB root; ATA-disk VMs are byte-for-byte unaffected.

#include "blockdev.h"
#include "../drivers/usb_msc.h"
#include "../mm/pmm.h"
#include "../mm/demand.h"   // #122 pass 3: page-fault witness inside the IF=0 window
#include "../string.h"
#include "../sync/spinlock.h"   // #421: canonical irqsave lock (see blk_lk below)
#include "../sync/waitq.h"      // #SB: the canonical park primitive. NOT a new
                                // wait mechanism: the staging claim below parks
                                // exactly the way msc_cmd_lock() already parks
                                // one call deeper on this same path (#617).
#include "../sync/noblock.h"    // #SB: wq_may_block(), the ONE definition of
                                // "may this context sleep" (#514).
#include "../cpu/scprof.h"      // #121: block-layer phase attribution
#include "../cpu/mono.h"        // #122: THE shared monotonic clock. TSC-backed
#include "bootlog.h"      // #DIAGLOG: the DURABLE sink. A line that is
                          // evidence about medium corruption must reach
                          // /BOOTLOG.TXT, not only a serial port the affected
                          // machine class does not have. See tools/diaglog-gate.
                                // and valid with interrupts off, which is the
                                // only reason an IF=0 window can be timed at
                                // all. NOT a new clock: see mono.h on why five
                                // subsystems each grew a private one first.

extern int kprintf(const char *fmt, ...);
extern void proc_yield(void);
extern void gfx_boot_progress(int percent);
extern void gfx_boot_log(const char *message);

// ATA DMA sector I/O (implemented in drivers/ata.c).
extern int ata_read_sectors_dma(uint8_t channel, uint8_t drive, uint32_t lba,
                                uint8_t count, void *buffer);
extern int ata_write_sectors_dma(uint8_t channel, uint8_t drive, uint32_t lba,
                                 uint8_t count, const void *buffer);

// #550: maximum number of CONSECUTIVE sectors coalesced into one SCSI READ(10)
// on the demand-cache miss path. This was 1, so every cache miss cost a separate
// USB Bulk-Only Transport command (CBW + data + CSW = three xHCI waits, and
// usb_msc serialises them all on one global g_msc_cmd_busy). A 17 KB WAV was 35
// commands; that is where the multi-hundred-ms stalls on the real iMac came from.
//
// NOTE this is a MAXIMUM, not a fixed count. It CANNOT simply be raised in place:
// the old loop passed it as the sector count while advancing the destination by
// exactly ONE sector per iteration, so any value above 1 would have overrun the
// caller's buffer. blk_read now coalesces an actual run of consecutive misses and
// reads it directly into the (contiguous) caller buffer. 64 sectors = 32 KB, the
// same transfer size the TO-RAM bulk fill has been using reliably (BLK_FILL_SECS).
#define BLK_USB_CHUNK 64

static int g_root_usb = 0;
static int g_root_usb_index = -1;

// =============================================================================
// #375: shared 2 MB RAM-chunk store (used by both TO-RAM and the demand cache).
// =============================================================================

#define BLK_SECTOR         512u
#define BLK_CHUNK_PAGES    512u                          // 2 MB per data chunk
#define BLK_CHUNK_BYTES    (BLK_CHUNK_PAGES * PMM_PAGE_SIZE)
#define BLK_CHUNK_SECS     (BLK_CHUNK_BYTES / BLK_SECTOR)  // 4096 sectors/chunk
#define BLK_MAX_CHUNKS     1280u                          // up to ~2.5 GB device
// Keep this many pages (256 MB) free after reserving RAM, so the OS is not
// starved. If the full TO-RAM image would breach it we fall back to the cache.
#define BLK_KEEP_FREE_PAGES 65536u
// TO-RAM bulk fill transfer size: 64 sectors = 32 KB per SCSI READ(10) (fast:
// 64x fewer USB commands than the 1-sector cold path). An xHCI Transfer TRB must
// NOT cross a 64 KB boundary (xhci_bulk_transfer emits ONE TRB), so each 2 MB
// chunk is 64 KB-ALIGNED (see blk_alloc_chunks) and 32 KB transfers step within
// each 64 KB window, so a fill transfer never crosses a 64 KB boundary. 64
// divides BLK_CHUNK_SECS (4096) evenly so a transfer never crosses a chunk either.
#define BLK_FILL_SECS      64u
#define BLK_ALIGN64K       0x10000ULL
// Over-allocate each chunk by 64 KB (16 pages) so it can be aligned up to 64 KB.
#define BLK_CHUNK_ALLOC_PAGES (BLK_CHUNK_PAGES + 16u)
// CRITICAL: the kernel HEAP is at VIRTUAL base HEAP_VIRT_BASE=0x10000000 (256 MB)
// and vmm-remaps that virtual window to arbitrary physical pages (see mm/heap.c).
// So the identity-mapped virtual address of any PHYSICAL page in
// [0x10000000, 0x10000000 + HEAP_MAX_SIZE) is HIJACKED by the heap: writing to
// such a page via an identity pointer actually clobbers the live heap. TO-RAM
// spans this range, so chunks whose usable region overlaps the heap window MUST
// be skipped, or the heap free-list is corrupted (observed as a #GP in
// remove_from_free_list right after a "100%" copy). HEAP_MAX_SIZE is 256 MB.
#define BLK_HEAP_WIN_LO    0x10000000ULL
#define BLK_HEAP_WIN_HI    0x20000000ULL   // 0x10000000 + HEAP_MAX_SIZE (256 MB)
#define BLK_MAX_PARKED     160u            // 256 MB window / 2 MB, + slack

static uint8_t  *g_chunk[BLK_MAX_CHUNKS];        // 64 KB-aligned usable base
static uint64_t  g_chunk_alloc[BLK_MAX_CHUNKS];  // raw pmm base (for freeing)
static uint32_t  g_nchunks = 0;
// Heap-window physical pages we permanently RESERVE (keep allocated) so that
// nothing - not TO-RAM, not a later AP stack or GUI buffer - ever uses a page
// whose identity virtual address is hijacked by the heap remap. Freed only at
// teardown. Leaving these floating (freeing them) let a post-copy allocation
// grab a window page and #GP jumping to garbage (SMP trampoline addr 0x8008).
static uint64_t  g_parked[BLK_MAX_PARKED];
static uint32_t  g_nparked = 0;

// Mode: 0 = off (pass-through to USB), 1 = TO-RAM, 2 = demand cache.
#define BLK_MODE_OFF   0
#define BLK_MODE_TORAM 1
#define BLK_MODE_CACHE 2
static int g_mode = BLK_MODE_OFF;

// #417: forced off by a boot-time config marker (/TORAMOFF.TXT). Demand cache
// still applies in this case, so a stick can still get *some* caching benefit
// with TO-RAM's bulk RAM copy disabled.
static int g_toram_disabled = 0;

// TO-RAM: g_chunk[] holds device sectors 0 .. g_ram_sectors-1 contiguously.
static uint64_t g_ram_sectors = 0;

// Demand cache (fallback): direct-mapped over g_chunk[], tag = resident LBA.
#define BLK_TAG_INVALID 0xFFFFFFFFFFFFFFFFULL
static uint64_t *g_tag = 0;
static uint32_t  g_slots = 0;
static uint32_t  g_mask = 0;

// #421 (AssaultCube shadow-cache freeze): this WAS a hand-rolled yielding lock,
//   static volatile int g_blk_busy;
//   blk_lk() { while (test_and_set(&g_blk_busy,1)) proc_yield(); }
// which is a #426 violation and it FROZE the whole kernel under AssaultCube's
// startup shadow-cache write storm (hundreds of tiny ext2 writes). Root cause,
// measured on build 916 by pausing the vCPU at the freeze and walking the proc
// table: the critical sections here are a single memcpy (a few instructions
// between the acquire xchg and the release mov). If the timer preempts a holder
// inside that window, the holder is left READY while still holding g_blk_busy,
// and because add_to_ready_queue() is PRIORITY-ordered a lower-priority holder
// can then be starved indefinitely by the higher-priority procs that are now
// spinning here. Every block-I/O proc piles into proc_yield()->sched_schedule(),
// producing a context-switch STORM on the BSP (CPU#0 pinned ~90%, IF=0 so the
// timer barely ticks, kernel heartbeat dead, NO panic). It never recovers.
// Fix: use the canonical irqsave spinlock, exactly like g_e2c_lock (fs/ext2.c)
// and g_ata_io_lock (drivers/ata.c). IRQs are masked across the tiny memcpy, so
// the holder can NEVER be preempted mid-hold: it always reaches the release, the
// lock can never be left stuck, and the yield-storm is structurally impossible.
// Every section below is a bounded memcpy + tag write with NO sleep/nested lock,
// so holding IRQs off across it is correct and short (bounded like ata_dma_wait).
static spinlock_t g_blk_lock = SPINLOCK_INIT;

// =============================================================================
// #122 BLOCK-LAYER TIME CENSUS: where does a blk_read actually go?
//
// THE TICKET SAYS "blk_read's RAM cache path costs 180 ms for a 128 KB refill
// with interrupts off, and it is not the device". Every clause of that is a
// measurement, so every clause needs an instrument that can return the OTHER
// answer. This one can.
//
// WHY NOT JUST USE cpu/scprof.c, WHICH ALREADY BRACKETS THIS FUNCTION. Because
// its unit is the SYSCALL and this ticket's unit is the CALL:
//
//   1. SCP_BLKREAD is a per-cpu accumulator summed over every blk_read a
//      syscall makes. A 128 KB window refill issues dozens of them, and the
//      sum cannot say whether that was one slow read or sixty ordinary ones.
//   2. scprof has no notion of an INTERRUPTS-OFF window, which is the half of
//      this ticket that matters beyond throughput. blk_lk() is
//      spinlock_acquire_irqsave(); "how long is IF actually 0" is not a
//      question about syscalls.
//   3. IT HAS A HOLE THAT MANUFACTURES THIS TICKET'S PREMISE. SCP_USBMSC
//      brackets usb_msc_read() and NOTHING bracketed ata_read_sectors_dma(),
//      so on an ATA-rooted volume every microsecond the DEVICE spends was
//      booked to "blk minus usb" - which is precisely the "it is not the
//      device" residual. Both device paths are timed here, and the ATA one is
//      now inside SCP_USBMSC too, so the older instrument stops lying as well.
//
// WHAT EACH FIELD IS, stated so nobody has to infer it later:
//
//   t_call    wall us from blk_read() entry to return. INCLUDES time this
//             thread was not running, if it was preempted inside blk_read.
//   t_dev     wall us inside usb_msc_read() / ata_read_sectors_dma(). The
//             device, both transports.
//   t_irqoff  wall us measured from BEFORE blk_lk() to AFTER blk_ul().
//             Deliberately outside the lock on both sides:
//             spinlock_acquire_irqsave() does its cli FIRST and then spins, so
//             a clock started after the acquire returns cannot see the wait.
//             #69 lost a whole pass to exactly that mistake. Nothing inside
//             this window can be preempted, so unlike t_call it is genuinely
//             time this core spent executing.
//   residual  t_call - t_dev - t_irqoff. Loop overhead, usb_msc_get_device(),
//             AND any preemption. It is NOT "our code" and must never be
//             quoted as if it were.
//
// AND THE ONE NUMBER THAT DECIDES THE TICKET: bytes / t_irqoff, the EFFECTIVE
// COPY BANDWIDTH of the RAM path. A term that is merely large tells you
// nothing until it is divided by the work it did. If the RAM cache path really
// costs 180 ms per 128 KB it is running at 0.7 MB/s, which no memcpy on any
// silicon this kernel runs on can do; if it reports memory speed then whatever
// is slow is somewhere else, however large t_call looks.
//
// #69's OTHER lesson is built in: the cache-lookup population is BIMODAL (a
// lookup that hits copies a sector, a lookup that misses copies nothing) and
// an average over both describes neither, so hits and misses are charged to
// separate counters from the start rather than split later.
// =============================================================================
// PASS 2. Pass 1 measured each window from BEFORE blk_lk() to AFTER blk_ul()
// and reported a worst case of 673679 us for a 4096-byte memcpy, i.e. 0.006
// MB/s. No memcpy does that, so the number was not measuring a memcpy, and the
// honest response to an impossible reading is to find out what it IS measuring
// rather than to publish it with a caveat.
//
// There are exactly two candidates and they are separable. `blk_lk_t()` reads
// the clock and THEN calls spinlock_acquire_irqsave(), which does its cli
// first. Between those two, INTERRUPTS ARE STILL ON: a timer tick landing
// there preempts this thread, and on a loaded host it may not run again for
// hundreds of milliseconds. The same is true after blk_ul() restores IF and
// before the closing read. So every window is measured as
//
//     outer = [ mouth ][ IF=0 body ][ mouth ]
//
// and only the body is a statement about this kernel's code. The body is timed
// with a second pair of reads taken INSIDE the critical section, where a
// preemption is structurally impossible, and `mouth = outer - inner` is kept
// as its own quantity rather than being silently folded into either.
//
// Note what this still CANNOT distinguish, because nothing inside the guest
// can: an inner window inflated by the HOST descheduling the vCPU. mono_us()
// is TSC-backed real time and keeps counting through a deschedule (#121). That
// is why the histogram below exists and why the run is paired with a host-side
// schedstat run_delay sample and a deliberate --cpulimit control.
// WHAT SHIPS AND WHY. The cheap half of this census (per-CALL wall time and
// per-CALL device time) is ALWAYS ON: it costs two mono_us() reads on a call
// that averages ~97 us, i.e. about 0.1%, and it is the number this ticket was
// opened about, so the next person to ask "is it the device" on a machine we
// do not have - the real iMac - can read the answer instead of rebuilding.
//
// The expensive half is OFF by default and built with -DBLK122_WINDOWS=1. It
// times every interrupts-off window (two more mono_us() reads per window) and,
// at level 2, every individual 512-byte memcpy inside a cache install. The
// probe measured itself at 0.05-0.095 us per window against a hit window whose
// whole body is 0.06 us, so leaving it on would roughly DOUBLE the cost of the
// cache lookup it is measuring. It answered its question - the answer was that
// there is nothing here to fix - and a probe that costs more than the thing it
// measures does not earn a permanent seat in a hot path.
#ifndef BLK122_WINDOWS
#define BLK122_WINDOWS 0
#endif

#define BLK_HIST_N 6   /* <1us, <10, <100, <1ms, <10ms, >=10ms */

// [no-ticket] rustkern/blkhist.rs owns the bucketing and every derived rate.
#define BLKHIST_N 6
// Distinct single-sector call sites tracked. Eight is enough to name the
// offenders; a ninth lands in c1_other, which is printed so the table can never
// silently under-report.
#define BLK_C1_SITES 8
extern uint32_t blkhist_bucket_rs(uint32_t sectors);
extern uint64_t blkhist_xfers_per_mb_x10_rs(uint64_t xfers, uint64_t sectors);
extern uint64_t blkhist_sectors_per_xfer_x10_rs(uint64_t xfers, uint64_t sectors);
extern uint64_t blkhist_projected_ms_rs(uint64_t xfers, uint64_t cmd_us);
typedef struct {
    uint64_t calls, sectors, hits, misses;
    uint64_t t_call, t_dev, n_dev;
    // Interrupts-off windows, split by whether the window did any copying.
    // #69: a did-work population averaged with a did-nothing one describes
    // neither, so they are separate counters from the start.
    uint64_t t_hit,  n_hit,  b_hit;    // lookups that found the sector: memcpy
    uint64_t t_dry,  n_dry;            // lookups that missed: tag compare only
    uint64_t t_ins,  n_ins,  b_ins;    // post-device install of a fetched run
    uint64_t i_hit,  i_dry,  i_ins;    // the same three, INNER (IF=0 body only)
    uint64_t t_mouth;                  // outer - inner, summed: preemption
    uint64_t max_call, max_call_sec, max_call_dev;
    uint64_t max_win,  max_win_b, max_win_inner; int max_win_kind;
    uint64_t max_inner, max_inner_b;   // worst window BY ITS IF=0 BODY
    uint64_t hist_n[BLK_HIST_N], hist_us[BLK_HIST_N];      // outer
    uint64_t ihist_n[BLK_HIST_N], ihist_us[BLK_HIST_N];    // inner
    // PASS 3. The IF=0 BODY, not its mouth, holds the time, and 46 windows of
    // 74381 hold 95% of it. A 32 KB memcpy that measures 182449 us is not a
    // memcpy measurement, so the next question is whether the CORE WAS
    // EXECUTING at all. Two witnesses, both taken inside the window:
    //   worst_iter   the longest SINGLE 512-byte memcpy iteration. No memory
    //                typing on any machine this kernel runs on makes one of
    //                those take milliseconds, so a large value means the core
    //                stopped between two instructions.
    //   pf_in_win    page faults taken during the window. An exception is NOT
    //                masked by cli, so a fault inside an irqsave section is
    //                perfectly possible and would be a real, fixable, in-guest
    //                cause. Zero here removes the whole hypothesis; non-zero
    //                names it.
    // If the core stopped and it was not a fault, what remains is the host
    // descheduling the vCPU, which mono_us() cannot distinguish from execution
    // (#121) and which is why this is paired with an external control.
    uint64_t worst_iter, worst_iter_bytes;
    uint64_t pf_in_win, n_win_with_pf;
    uint64_t probe_ns_x1000;           // measured cost of the probe itself
    // [no-ticket] ROUND TRIPS, the quantity a USB root actually pays for. A
    // command costs a fixed ~121-148 us whatever its size (measured on real
    // hardware), so "5 sectors per transfer" is the whole diagnosis and a mean
    // cannot tell "everything is 5" from "half are 1 and half are 64" (#69).
    uint64_t csz[BLKHIST_N];           // blk_read() call sizes, by sector count
    uint64_t xsz[BLKHIST_N];           // DEVICE transfer sizes, by sector count
    // [no-ticket] WHO issues the single-sector reads. The size histogram above
    // says 94% of blk_read calls and 78% of device commands are ONE SECTOR; it
    // cannot say which caller, and "the block layer does small reads" is not a
    // fixable statement. A return address is, via addr2line against
    // kernel.dbg.elf. Recorded only for count == 1, so it costs an 8-entry scan
    // on exactly the population being investigated and nothing on any other.
    uint64_t c1_ret[BLK_C1_SITES];     // caller return address
    uint64_t c1_n[BLK_C1_SITES];       // calls from it
    uint64_t c1_dev[BLK_C1_SITES];     // of those, ones that reached the device
    uint64_t c1_other;                 // calls from a 9th-or-later site
} blk_census_t;
// Updated WITHOUT synchronisation, deliberately and consistently with the
// counters already in this file (g_blk_write_calls, g_usb_reads). These are
// diagnostic totals, not control state: a lost increment under SMP costs a
// count, never a correctness property, and putting them under g_blk_lock would
// widen the very critical section the ticket is about. The report takes ONE
// struct-copy snapshot so at least the parts cannot disagree with the whole
// inside a single printed line.
static blk_census_t g_bc;

// #143: contention accounting ON THE SHARED PRIMITIVE, not a private counting
// lock forked into fs/. It adds no new spin loop (see spinlock.h).
static spin_acct_t g_blk_lock_acct = SPIN_ACCT_INIT;

static uint64_t blk_lk(void)        { return spinlock_acquire_irqsave_acct(&g_blk_lock, &g_blk_lock_acct); }
static void     blk_ul(uint64_t fl) { spinlock_release_irqrestore(&g_blk_lock, fl); }

// A TIMED interrupts-off window. t0 is taken BEFORE the cli, on purpose.
// mono_us() returns 0 until the TSC is calibrated (mono.h), and the block layer
// runs long before that on the boot path, so t0 == 0 means "no clock yet" and
// the sample is DROPPED rather than recorded as instantaneous. An uncounted
// sample is better than one that reads as zero.
static inline uint32_t blk_bucket(uint64_t us) {
    if (us < 1) return 0;
    if (us < 10) return 1;
    if (us < 100) return 2;
    if (us < 1000) return 3;
    if (us < 10000) return 4;
    return 5;
}

// t0 is read BEFORE the cli and t1 AFTER the acquire returns, so the pair
// brackets the mouth as well as the body. See the PASS 2 note above.
#if BLK122_WINDOWS
static inline uint64_t blk_lk_t(uint64_t *t0, uint64_t *t1) {
    *t0 = mono_us();
    uint64_t fl = blk_lk();
    *t1 = mono_us();
    return fl;
}
#else
// Gated off: the call sites are unchanged, so there is exactly ONE version of
// the read path in this file and no chance of the measured code and the
// shipped code drifting apart.
static inline uint64_t blk_lk_t(uint64_t *t0, uint64_t *t1) {
    *t0 = 0; *t1 = 0; return blk_lk();
}
#endif

#if BLK122_WINDOWS
static inline void blk_ul_t(uint64_t fl, uint64_t t0, uint64_t t1,
                            uint64_t bytes, int kind) {
    uint64_t i1 = mono_us();          // still inside: IF is 0 here
    blk_ul(fl);
    if (!t0) return;
    uint64_t outer = mono_us() - t0;
    uint64_t inner = (i1 > t1) ? i1 - t1 : 0;
    switch (kind) {
        case 0: g_bc.t_hit += outer; g_bc.i_hit += inner; g_bc.n_hit++; g_bc.b_hit += bytes; break;
        case 1: g_bc.t_dry += outer; g_bc.i_dry += inner; g_bc.n_dry++;                      break;
        default: g_bc.t_ins += outer; g_bc.i_ins += inner; g_bc.n_ins++; g_bc.b_ins += bytes; break;
    }
    g_bc.t_mouth += (outer > inner) ? outer - inner : 0;
    uint32_t bo = blk_bucket(outer), bi = blk_bucket(inner);
    g_bc.hist_n[bo]++;  g_bc.hist_us[bo]  += outer;
    g_bc.ihist_n[bi]++; g_bc.ihist_us[bi] += inner;
    if (outer > g_bc.max_win) {
        g_bc.max_win = outer; g_bc.max_win_b = bytes;
        g_bc.max_win_inner = inner; g_bc.max_win_kind = kind;
    }
    if (inner > g_bc.max_inner) { g_bc.max_inner = inner; g_bc.max_inner_b = bytes; }
}
#else
static inline void blk_ul_t(uint64_t fl, uint64_t t0, uint64_t t1,
                            uint64_t bytes, int kind) {
    (void)t0; (void)t1; (void)bytes; (void)kind;
    blk_ul(fl);
}
#endif

// Stats: RAM-served sectors vs USB-served sectors (verification).
static uint64_t g_ram_hits = 0;
static uint64_t g_usb_reads = 0;

// #617 WRITE GENERATION - the demand cache's lost-update guard.
//
// THE BUG (reproduced deterministically by tools/blkdev-harness, 200/200 trials
// before this change, 0/200 after). blk_read()'s BLK_MODE_CACHE miss path drops
// g_blk_lock across the BLOCKING usb_msc_read() - it must, that is a slow USB
// round trip and holding an irqsave spinlock across it is the #618 freeze - and
// then installs the bytes it got back into the cache UNCONDITIONALLY. So:
//
//   reader:  ... miss on LBA X ... [lock dropped] ... device READ(X) -> "old"
//                                                    <-- PREEMPTED HERE
//   writer:                        WRITE(X,"new") to the medium, then takes
//                                  g_blk_lock and installs "new" in the cache
//   reader:  resumes, takes g_blk_lock, installs "old" over it, tags it VALID
//
// The medium now holds "new" and the cache serves "old", permanently, to every
// later reader. usb_msc_transport() serializing whole SCSI commands does NOT
// help: the losing window is between usb_msc_read() RETURNING and blk_lk(),
// which is a handful of instructions and one timer tick wide.
//
// THE FIX, and why it is this one. g_wgen is bumped (under g_blk_lock) by every
// write that touches the cache. A reader snapshots it before issuing its device
// read and, at install time, installs ONLY if it is unchanged. If some write
// landed while we were away we cannot tell whether it was OUR sector (an
// aliasing write to a different LBA can have overwritten the tag that would
// otherwise have told us), so we decline to install and leave whatever the
// writer put there - which is by construction at least as fresh as ours. The
// caller still gets the bytes the device returned; an overlapping read is
// entitled to either image, but the CACHE must never keep the stale one.
//
// This is deliberately the SAME shape as fs/ext2.c's g_e2c_wgen (#597), which
// fixed exactly this class one layer up. blockdev had no equivalent, and ext2
// readers never take ext2_lock, so the block layer could reintroduce staleness
// underneath a correctly-behaving ext2 cache.
//
// WHY IT CANNOT DEADLOCK OR COST ANYTHING: no new lock, no new lock ORDER, and
// nothing blocking added inside a critical section. The snapshot and the
// compare both happen inside g_blk_lock sections that already existed; the net
// addition to the hot path is one 64-bit load and one 64-bit compare per
// coalesced run. Critically it does NOT widen the lock across the I/O, which is
// what made #618 a 45-second install freeze.
//
// WHY NOT RE-READ INSTEAD OF DECLINING: a retry can lose the same race again,
// so it needs its own bound, and it spends an extra USB command to repair a
// cache entry the next miss would fetch anyway. Declining is unbounded-free.
//
// The skip count is exported (blk_stale_skips) rather than left invisible: if
// this ever fires often, that is a MEASUREMENT that read/write overlap is
// common on the root device, not a guess.
//
// #621: exporting it was NOT enough. blk_stale_skips() shipped in #617 with
// ZERO CALLERS anywhere in the kernel, which is the same trap this project
// keeps falling into (validate_user_ptr, sse_save, #433): the accessor exists,
// so everyone assumes somebody is watching it, and nobody is. It is now read
// once per heartbeat by the [HB] worker in main.c and printed as "blkstale=",
// unconditionally including when zero, plus a one-shot loud [BLKSTALE] line
// the first time it moves. Do not remove that reader without adding another.
static volatile uint64_t g_wgen = 0;
static uint64_t g_stale_skips = 0;

static inline uint8_t *chunk_sector(uint64_t sec) {
    return g_chunk[sec / BLK_CHUNK_SECS] + (sec % BLK_CHUNK_SECS) * BLK_SECTOR;
}

static void blk_free_chunks(void) {
    for (uint32_t i = 0; i < g_nchunks; i++) {
        if (g_chunk_alloc[i]) { pmm_free_pages(g_chunk_alloc[i], BLK_CHUNK_ALLOC_PAGES); }
        g_chunk_alloc[i] = 0; g_chunk[i] = 0;
    }
    g_nchunks = 0;
    for (uint32_t j = 0; j < g_nparked; j++)
        if (g_parked[j]) pmm_free_pages(g_parked[j], BLK_CHUNK_ALLOC_PAGES);
    g_nparked = 0;
    if (g_tag) {
        pmm_free_pages((uint64_t)g_tag, (g_slots * sizeof(uint64_t) + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
        g_tag = 0;
    }
    g_slots = 0; g_mask = 0; g_ram_sectors = 0;
    g_mode = BLK_MODE_OFF;
}

// Allocate `n` 2 MB chunks. Returns number actually allocated (may be < n).
static uint32_t blk_alloc_chunks(uint32_t n) {
    if (n > BLK_MAX_CHUNKS) n = BLK_MAX_CHUNKS;
    // Chunks whose usable region overlaps the heap virtual window are unusable
    // via identity pointers (they alias the heap); PARK them (keep them allocated
    // so pmm's first-fit walks past them) and KEEP them reserved for the whole
    // session so no later allocation reuses a window page either.
    uint32_t got = 0;
    for (uint32_t i = 0; i < n; ) {
        uint64_t raw = pmm_alloc_pages(BLK_CHUNK_ALLOC_PAGES);
        if (!raw) break;
        uint64_t aligned = (raw + BLK_ALIGN64K - 1) & ~(BLK_ALIGN64K - 1);
        uint64_t use_lo = aligned;
        uint64_t use_hi = aligned + (uint64_t)BLK_CHUNK_PAGES * PMM_PAGE_SIZE; // +2 MB usable
        // Overlaps the heap window? [use_lo,use_hi) vs [WIN_LO,WIN_HI)
        if (use_lo < BLK_HEAP_WIN_HI && use_hi > BLK_HEAP_WIN_LO) {
            if (g_nparked < BLK_MAX_PARKED) { g_parked[g_nparked++] = raw; continue; }
            pmm_free_pages(raw, BLK_CHUNK_ALLOC_PAGES);   // out of park slots: stop
            break;
        }
        g_chunk_alloc[i] = raw;
        g_chunk[i] = (uint8_t *)aligned;   // 64 KB-aligned, TRB-safe, heap-safe
        got++;
        i++;
    }
    g_nchunks = got;
    return got;   // parked chunks stay reserved until blk_free_chunks()
}

// -----------------------------------------------------------------------------
// TO-RAM: copy the USB root into RAM with large sequential reads. The window
// size is derived from ACTUAL USED filesystem data (used_bytes_hint), not the
// raw device/partition capacity: a live-USB stick's GPT+FAT32 can be expanded
// to fill a much larger drive than the real payload (e.g. 116 GB volume, ~340
// MB of real files), and sizing off the raw capacity made TO-RAM bail out to a
// tiny demand cache on every such stick even though the real data would have
// fit trivially. Returns 1 on success (mode = TO-RAM), 0 if it did not fit /
// failed.
// -----------------------------------------------------------------------------
static int blk_try_toram(usb_msc_device_t *dev, uint64_t used_bytes_hint) {
    uint64_t dev_total = dev->num_blocks;
    if (dev_total == 0) return 0;

    uint64_t total;
    if (used_bytes_hint > 0) {
        // Sectors needed to hold the real data, plus a generous +50% safety
        // margin (fragmentation headroom + room for the writes a live session
        // makes: settings, notes, /HEARTBEAT.TXT, /BOOTLOG.TXT, ...), rounded
        // up to a whole 2 MB chunk. Anything beyond this window still works
        // correctly (blk_read/blk_write fall through to a live USB access for
        // any LBA outside it); this just decides how much rides in RAM.
        uint64_t need_sectors = (used_bytes_hint + BLK_SECTOR - 1) / BLK_SECTOR;
        uint64_t margin = need_sectors / 2 + (16ull * 1024 * 1024) / BLK_SECTOR;  // +50% + 16MB floor
        need_sectors += margin;
        need_sectors = ((need_sectors + BLK_CHUNK_SECS - 1) / BLK_CHUNK_SECS) * BLK_CHUNK_SECS;
        total = (need_sectors < dev_total) ? need_sectors : dev_total;
        kprintf("[TORAM] used-data hint %llu MB -> RAM window %llu MB (device is %llu MB)\n",
                (unsigned long long)(used_bytes_hint / (1024 * 1024)),
                (unsigned long long)((total * BLK_SECTOR) / (1024 * 1024)),
                (unsigned long long)((dev_total * BLK_SECTOR) / (1024 * 1024)));
    } else {
        // No filesystem-level hint available (caller doesn't know / legacy
        // call site): fall back to the original whole-device sizing so
        // behavior is unchanged for callers that can't compute a hint.
        total = dev_total;
        kprintf("[TORAM] no used-data hint; sizing off whole device (%llu MB)\n",
                (unsigned long long)((dev_total * BLK_SECTOR) / (1024 * 1024)));
    }

    // #539: the +50% write-margin can push `total` past the chunk cap when the
    // hint already approximates the whole image (e.g. the two-partition golden
    // written to a stick MUCH larger than the ~1.8 GB payload). We ALWAYS fill
    // from LBA 0 upward and the payload lives at the FRONT of the device, so if
    // the REAL data (the un-margined hint) fits inside the cap window, clamp the
    // window to the cap instead of falling back to the demand cache: a cap-sized
    // (2.5 GB) window still holds the whole <=2.5 GB image in RAM, and only the
    // never-read empty tail past the cap is excluded. Gated on a real hint so the
    // no-hint whole-device path (data location unknown) keeps the old behavior.
    uint64_t cap_sectors = (uint64_t)BLK_MAX_CHUNKS * BLK_CHUNK_SECS;
    if (used_bytes_hint > 0 && total > cap_sectors) {
        uint64_t data_sectors = (used_bytes_hint + BLK_SECTOR - 1) / BLK_SECTOR;
        if (data_sectors <= cap_sectors) {
            total = cap_sectors;
            kprintf("[TORAM] window clamped to cap %llu MB (payload %llu MB is at front of device)\n",
                    (unsigned long long)((total * BLK_SECTOR) / (1024 * 1024)),
                    (unsigned long long)(used_bytes_hint / (1024 * 1024)));
        }
    }

    uint32_t need_chunks = (uint32_t)((total + BLK_CHUNK_SECS - 1) / BLK_CHUNK_SECS);
    if (need_chunks > BLK_MAX_CHUNKS) {
        kprintf("[TORAM] RAM window too large (%llu sectors, ~%llu MB), using demand cache\n",
                (unsigned long long)total, (unsigned long long)((total * BLK_SECTOR) / (1024 * 1024)));
        return 0;
    }
    uint64_t need_pages = (uint64_t)need_chunks * BLK_CHUNK_ALLOC_PAGES;
    uint64_t free_pages = pmm_get_free_pages();
    if (free_pages < need_pages + BLK_KEEP_FREE_PAGES) {
        kprintf("[TORAM] not enough RAM (%llu free, need %llu pages), using demand cache\n",
                (unsigned long long)free_pages, (unsigned long long)need_pages);
        return 0;
    }

    uint32_t mb = (uint32_t)((total * BLK_SECTOR) / (1024 * 1024));
    kprintf("[TORAM] copying %u MB root to RAM (%u chunks, %llu free pages)...\n",
            mb, need_chunks, (unsigned long long)free_pages);
    gfx_boot_log("[BOOT] Copying root to RAM (fast reads)...");

    if (blk_alloc_chunks(need_chunks) < need_chunks) {
        kprintf("[TORAM] chunk allocation short (%u/%u), reverting to cache\n",
                g_nchunks, need_chunks);
        blk_free_chunks();
        return 0;
    }
    g_ram_sectors = (uint64_t)need_chunks * BLK_CHUNK_SECS;

    // Bulk sequential fill. Large 32 KB reads; on any read error retry that span
    // one sector at a time, and if even that fails, abort TO-RAM (fall back).
    uint64_t base = 0;
    int last_pct = -1;
    while (base < total) {
        uint32_t n = (uint32_t)((total - base) < BLK_FILL_SECS ? (total - base) : BLK_FILL_SECS);
        uint8_t *dst = chunk_sector(base);
        if (usb_msc_read(dev, 0, base, dst, n) != 0) {
            int ok = 1;
            for (uint32_t k = 0; k < n; k++) {
                if (usb_msc_read(dev, 0, base + k, chunk_sector(base + k), 1) != 0) { ok = 0; break; }
            }
            if (!ok) {
                kprintf("[TORAM] read failed at LBA %llu, reverting to demand cache\n",
                        (unsigned long long)(base));
                blk_free_chunks();
                return 0;
            }
        }
        base += n;
        int pct = (int)((base * 100) / total);
        if (pct != last_pct && (pct % 2) == 0) {
            last_pct = pct;
            gfx_boot_progress(60 + pct * 38 / 100);   // map fill onto 60..98%
            kprintf("[TORAM] fill %d%% (%llu/%llu sectors)\n",
                    pct, (unsigned long long)base, (unsigned long long)total);
        }
    }

    g_mode = BLK_MODE_TORAM;
    g_ram_hits = 0;
    g_usb_reads = 0;
    kprintf("[TORAM] root fully in RAM (%u MB); stick idle for reads now\n", mb);
    gfx_boot_log("[BOOT] Root loaded to RAM");
    gfx_boot_progress(98);
    return 1;
}

// -----------------------------------------------------------------------------
// Demand cache fallback: bounded direct-mapped RAM cache of 512-byte sectors.
// -----------------------------------------------------------------------------
// #539: raised from 64 (128 MB) to 256 (512 MB). When TO-RAM cannot fit the
// whole two-partition image, the ext2 ROOT should still get a meaningful cache
// rather than only 128 MB. This is only a CEILING: blk_enable_cache() below
// trims `want` down to the free-RAM budget (leaving BLK_KEEP_FREE_PAGES), so a
// low-RAM box never OOMs; it just gets a smaller cache.
#define BLK_CACHE_CHUNKS 256u   // up to 512 MB cache when TO-RAM does not fit
static void blk_enable_cache(void) {
    uint32_t want = BLK_CACHE_CHUNKS;
    uint64_t free_pages = pmm_get_free_pages();
    // Trim to leave the keep-free margin.
    uint64_t budget = (free_pages > BLK_KEEP_FREE_PAGES) ? (free_pages - BLK_KEEP_FREE_PAGES) : 0;
    uint32_t fit = (uint32_t)(budget / BLK_CHUNK_ALLOC_PAGES);
    if (fit < want) want = fit;
    if (want == 0) { kprintf("[BLKC] no RAM for cache; passthrough\n"); return; }

    if (blk_alloc_chunks(want) == 0) { blk_free_chunks(); return; }
    g_slots = g_nchunks * BLK_CHUNK_SECS;        // power of two (chunks*4096)
    g_mask  = g_slots - 1;
    uint32_t tag_pages = (uint32_t)((g_slots * sizeof(uint64_t) + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    g_tag = (uint64_t *)pmm_alloc_pages(tag_pages);
    if (!g_tag) { blk_free_chunks(); kprintf("[BLKC] tag alloc failed; passthrough\n"); return; }
    memset(g_tag, 0xFF, g_slots * sizeof(uint64_t));
    g_mode = BLK_MODE_CACHE;
    g_ram_hits = 0; g_usb_reads = 0;
    kprintf("[BLKC] demand cache: %u MB (%u chunks)\n",
            (g_slots * BLK_SECTOR) / (1024 * 1024), g_nchunks);
}

// =============================================================================

void blk_set_root_usb(int usb_msc_index) {
    g_root_usb = 1;
    g_root_usb_index = usb_msc_index;
    kprintf("[BLK] root block device -> USB MSC index %d\n", usb_msc_index);
}

// #417: force TO-RAM off (demand cache still applies). Called from main.c
// when /TORAMOFF.TXT is present at the FAT root.
void blk_toram_set_disabled(int disabled) {
    g_toram_disabled = disabled;
    if (disabled) kprintf("[TORAM] disabled by /TORAMOFF.TXT config marker\n");
}

// #375/#417: called by main.c AFTER the USB root is verified as a MayteraOS
// root. `used_bytes_hint` (bytes of real filesystem data, 0 if unknown) sizes
// the RAM window; see blk_try_toram(). Attempts TO-RAM; on failure (or when
// disabled by config) enables the demand cache. Returns 1 if the sized root
// is now in RAM, 0 if using the (still helpful) demand cache / passthrough.
int blk_root_to_ram(uint64_t used_bytes_hint) {
    if (!g_root_usb || g_mode != BLK_MODE_OFF) return (g_mode == BLK_MODE_TORAM);
    usb_msc_device_t *dev = usb_msc_get_device(g_root_usb_index);
    if (!dev || !dev->ready) return 0;
    if (!g_toram_disabled && blk_try_toram(dev, used_bytes_hint)) return 1;
    blk_enable_cache();
    return 0;
}

void blk_clear_root_usb(void) {
    g_root_usb = 0;
    g_root_usb_index = -1;
    blk_free_chunks();
}

int blk_root_is_usb(void) { return g_root_usb; }
// #306: which USB device is the root? The installer needs it to refuse the
// disk it booted from - cloning a disk onto itself corrupts the source.
int blk_root_usb_index(void) { return g_root_usb_index; }

void blk_cache_stats(uint64_t *hits, uint64_t *misses, int *enabled) {
    if (hits) *hits = g_ram_hits;
    if (misses) *misses = g_usb_reads;
    if (enabled) *enabled = g_mode;   // 0 off, 1 toram, 2 cache
}

// [no-ticket] The four I/O totals a caller needs to charge ONE operation for
// what it cost the device. Snapshot before, snapshot after, subtract. They are
// the SAME counters [BLK122] prints, so a benchmark and the boot census can
// never disagree about what happened.
void blk_census_io(uint64_t *calls, uint64_t *sectors,
                   uint64_t *n_dev, uint64_t *t_dev) {
    blk_census_t c = g_bc;    // one snapshot, so the four cannot come from
                              // four different instants
    if (calls)   *calls   = c.calls;
    if (sectors) *sectors = c.sectors;
    if (n_dev)   *n_dev   = c.n_dev;
    if (t_dev)   *t_dev   = c.t_dev;
}

// #617: how many demand-cache installs were declined because a write landed
// during the read. Zero on a read-only workload (boot, fsck, app launch) by
// construction, so a non-zero value is a real measurement of read/write overlap
// on the root device, not noise.
uint64_t blk_stale_skips(void) { return g_stale_skips; }

// #122: PROVE THE PROBE BEFORE ANY READING FROM IT IS BELIEVED.
//
// This tree has had sixteen instruments caught lying in three days and four in
// this ticket's own family become the thing they measured. So the census
// answers two questions about ITSELF, at boot, on every build:
//
//   1. Does a timed interrupts-off window read a KNOWN duration? A 5 ms busy
//      wait is executed inside a real blk_lk_t()/blk_ul_t() pair, against the
//      same clock, and the recorded window is printed. If that is not 5000 us
//      nothing else printed here may be quoted.
//   2. What does the probe COST? 20000 empty windows are timed, which gives
//      the per-window overhead the readings carry. Publishing the overhead
//      next to the reading is what makes "the RAM path costs X" a claim
//      somebody else can check rather than one they have to trust.
//
// The census is snapshotted and restored around this, so the self-test never
// contributes a sample to the numbers it validates.
void blk_census_selftest(void) {
#if !BLK122_WINDOWS
    // Nothing to validate: the window probe is compiled out. Say so rather
    // than print a silent zero, which is how an instrument stops being read.
    kprintf("[BLK122-PROBE] window probe NOT BUILT (rebuild with -DBLK122_WINDOWS=1)\n");
#else
    if (!mono_ready()) { kprintf("[BLK122-PROBE] SKIPPED (mono clock not calibrated)\n"); return; }
    blk_census_t save = g_bc;
    const uint64_t want = 5000;

    g_bc.max_win = 0; g_bc.t_hit = 0; g_bc.n_hit = 0; g_bc.b_hit = 0;
    { uint64_t wt0, wt1; uint64_t fl = blk_lk_t(&wt0, &wt1);
      // mono_busy_delay_us() is THE shared short busy-delay (mono.h) and works
      // with interrupts off, which is exactly this window. A hand-rolled
      // `while (mono_us() - t0 < want)` here was correctly REFUSED by
      // kernel/tools/concurrency-lint: burning a core for a fixed real
      // duration already has one implementation in this kernel and does not
      // need a second inside an instrument.
      mono_busy_delay_us(want);
      blk_ul_t(fl, wt0, wt1, 4096, 0); }
    uint64_t got = g_bc.max_win;

    // Empty windows: the probe's own cost, plus one cli/sti and one
    // uncontended acquire/release. That is exactly the overhead a real
    // one-sector cache lookup carries, so it is the right thing to subtract.
    g_bc.t_dry = 0; g_bc.n_dry = 0;
    const uint64_t N = 20000;
    uint64_t e0 = mono_us();
    for (uint64_t i = 0; i < N; i++) { uint64_t wt0, wt1; uint64_t fl = blk_lk_t(&wt0, &wt1); blk_ul_t(fl, wt0, wt1, 0, 1); }
    uint64_t e1 = mono_us();
    uint64_t per_x1000 = ((e1 - e0) * 1000ull) / N;      // us per window, x1000
    uint64_t self_x1000 = (g_bc.t_dry * 1000ull) / N;    // what it reports of itself

    g_bc = save;
    g_bc.probe_ns_x1000 = per_x1000;

    uint64_t lo = want - want / 10, hi = want + want / 10;
    kprintf("[BLK122-PROBE] %s: asked %llu us, window recorded %llu us | probe cost "
            "%llu.%03llu us/window measured outside, %llu.%03llu us/window as the probe "
            "reports itself (n=%llu)\n",
            (got >= lo && got <= hi) ? "OK" : "BAD",
            (unsigned long long)want, (unsigned long long)got,
            (unsigned long long)(per_x1000 / 1000), (unsigned long long)(per_x1000 % 1000),
            (unsigned long long)(self_x1000 / 1000), (unsigned long long)(self_x1000 % 1000),
            (unsigned long long)N);
#endif
}

// #122: the census, printed next to [SCPROF-*] so the per-call and per-syscall
// views of the same microseconds can be read against each other.
//
// EVERY RATE IS PRINTED AS THE CONTRACT'S OWN QUANTITY (MB/s of copy), not just
// as the term that was changed. #176's first fix hit its target and still broke
// the subsystem's stated time contract, and was caught only because the
// diagnostic reported the contract's quantity rather than the new term alone.
void blk_census_report(void) {
    blk_census_t c = g_bc;   // one snapshot; a torn read across fields would
                             // otherwise let the parts disagree with the whole
    if (!c.calls) { kprintf("[BLK122] no blk_read samples yet\n"); return; }
    uint64_t t_win = c.t_hit + c.t_dry + c.t_ins;          // outer
    uint64_t i_win = c.i_hit + c.i_dry + c.i_ins;          // IF=0 body only
    uint64_t resid = (c.t_call > c.t_dev + t_win) ? c.t_call - c.t_dev - t_win : 0;
    uint64_t bytes = c.b_hit + c.b_ins;
    // 1 byte per microsecond == 1 MB/s, so bytes/us IS MB/s with no conversion.
    uint64_t hit_mbps = c.i_hit ? c.b_hit / c.i_hit : 0;
    uint64_t ins_mbps = c.i_ins ? c.b_ins / c.i_ins : 0;
    uint64_t all_mbps = i_win ? bytes / i_win : 0;

    // ALWAYS ON. The one question the ticket asked, answered on any machine
    // without a rebuild: of the block layer's wall time, how much is the
    // device? avg_us per call is printed too, because a total is not a rate.
    kprintf("[BLK122] mode=%d calls=%llu sectors=%llu | t_call=%lluus "
            "(avg %lluus/call) device=%lluus in %llu xfers -> DEVICE IS %llu%% "
            "OF THE BLOCK LAYER\n",
            g_mode, (unsigned long long)c.calls, (unsigned long long)c.sectors,
            (unsigned long long)c.t_call,
            (unsigned long long)(c.calls ? c.t_call / c.calls : 0),
            (unsigned long long)c.t_dev, (unsigned long long)c.n_dev,
            (unsigned long long)(c.t_call ? c.t_dev * 100 / c.t_call : 0));
    // [no-ticket] THE quantity this ticket turns on, printed as the contract's
    // own unit rather than as a raw count, and next to the projection it
    // implies on a device whose command cost is known.
    {
        uint64_t spx = blkhist_sectors_per_xfer_x10_rs(c.n_dev, c.sectors);
        uint64_t xpm = blkhist_xfers_per_mb_x10_rs(c.n_dev, c.sectors);
        kprintf("[BLK122] ROUND TRIPS: %llu.%llu sectors/xfer, %llu.%llu xfers/MB"
                " | at 121us/cmd that is %llums of pure round trip\n",
                (unsigned long long)(spx / 10), (unsigned long long)(spx % 10),
                (unsigned long long)(xpm / 10), (unsigned long long)(xpm % 10),
                (unsigned long long)blkhist_projected_ms_rs(c.n_dev, 121));
        // Same shape as the HIST lines below: one line built with the kernel's
        // own snprintf rather than a second string helper.
        static const char *lbl[BLKHIST_N] = { "1", "2-4", "5-16", "17-32", "33-64", ">64" };
        char cb[200]; char xb[200]; int cn = 0, xn = 0;
        cb[0] = 0; xb[0] = 0;
        for (int i = 0; i < BLKHIST_N; i++) {
            cn += snprintf(cb + cn, sizeof(cb) - (size_t)cn, " %s=%llu", lbl[i],
                           (unsigned long long)c.csz[i]);
            xn += snprintf(xb + xn, sizeof(xb) - (size_t)xn, " %s=%llu", lbl[i],
                           (unsigned long long)c.xsz[i]);
            if (cn >= (int)sizeof(cb) - 24 || xn >= (int)sizeof(xb) - 24) break;
        }
        kprintf("[BLK122] blk_read call sectors:%s\n", cb);
        kprintf("[BLK122] DEVICE xfer sectors:%s\n", xb);
        // The single-sector callers by return address. addr2line -e
        // kernel.dbg.elf <addr> names the line. "dev" is how many of that
        // site's calls actually reached the device rather than the RAM cache,
        // which is the column that costs round trips.
        for (int i = 0; i < BLK_C1_SITES; i++) {
            if (!g_bc.c1_ret[i]) break;
            kprintf("[BLK122] 1-sector caller ret=%llx calls=%llu dev=%llu\n",
                    (unsigned long long)c.c1_ret[i],
                    (unsigned long long)c.c1_n[i],
                    (unsigned long long)c.c1_dev[i]);
        }
        if (c.c1_other)
            kprintf("[BLK122] 1-sector callers beyond the %d tracked: %llu call(s)\n",
                    BLK_C1_SITES, (unsigned long long)c.c1_other);
    }
    kprintf("[BLK122] worst blk_read %lluus for %llu sectors, of which device "
            "%lluus | cache hits=%llu misses=%llu | lock acquires=%llu "
            "contended=%llu\n",
            (unsigned long long)c.max_call, (unsigned long long)c.max_call_sec,
            (unsigned long long)c.max_call_dev,
            (unsigned long long)c.hits, (unsigned long long)c.misses,
            (unsigned long long)g_blk_lock_acct.acquires,
            (unsigned long long)g_blk_lock_acct.contended);
#if !BLK122_WINDOWS
    (void)t_win; (void)i_win; (void)resid; (void)bytes;
    (void)hit_mbps; (void)ins_mbps; (void)all_mbps;
}
#else
    static const char *const B[BLK_HIST_N] = { "<1us", "<10us", "<100us", "<1ms", "<10ms", ">=10ms" };
    kprintf("[BLK122] WINDOWS outer=%lluus inner(IF=0 body)=%lluus "
            "mouth(preempt/starve)=%lluus residual=%lluus | of t_call: "
            "IF0body=%llu%% mouth=%llu%% residual=%llu%%\n",
            (unsigned long long)t_win, (unsigned long long)i_win,
            (unsigned long long)c.t_mouth, (unsigned long long)resid,
            (unsigned long long)(c.t_call ? i_win * 100 / c.t_call : 0),
            (unsigned long long)(c.t_call ? c.t_mouth * 100 / c.t_call : 0),
            (unsigned long long)(c.t_call ? resid * 100 / c.t_call : 0));
    // THE CONTRACT'S OWN QUANTITY. "180 ms for a 128 KB refill" is a bandwidth
    // claim of 0.7 MB/s. Printing MB/s makes the claim checkable instead of
    // merely large; #176 was caught only because the diagnostic reported the
    // contract's quantity and not just the term that had been changed.
    kprintf("[BLK122] RAMPATH hit n=%llu body=%lluus %lluKB -> %llu MB/s | "
            "dry n=%llu body=%lluus | install n=%llu body=%lluus %lluKB -> %llu MB/s "
            "| ALL -> %llu MB/s\n",
            (unsigned long long)c.n_hit, (unsigned long long)c.i_hit,
            (unsigned long long)(c.b_hit / 1024), (unsigned long long)hit_mbps,
            (unsigned long long)c.n_dry, (unsigned long long)c.i_dry,
            (unsigned long long)c.n_ins, (unsigned long long)c.i_ins,
            (unsigned long long)(c.b_ins / 1024), (unsigned long long)ins_mbps,
            (unsigned long long)all_mbps);
    kprintf("[BLK122] WORST outer window %lluus (body %lluus, %llu bytes, kind %d) "
            "| WORST IF=0 BODY %lluus for %llu bytes | probe %llu.%03lluus/window\n",
            (unsigned long long)c.max_win, (unsigned long long)c.max_win_inner,
            (unsigned long long)c.max_win_b, c.max_win_kind,
            (unsigned long long)c.max_inner, (unsigned long long)c.max_inner_b,
            (unsigned long long)(c.probe_ns_x1000 / 1000),
            (unsigned long long)(c.probe_ns_x1000 % 1000));
    // The distribution, not the mean. #69: an average over a bimodal
    // population pointed at the OPPOSITE fix, and only splitting it showed so.
    // Same shape as scp_phases() in cpu/scprof.c: build one line with the
    // kernel's own snprintf rather than inventing a second string helper.
    { char o[240]; char n2[240]; int po = 0, pq = 0;
      o[0] = 0; n2[0] = 0;
      for (int i = 0; i < BLK_HIST_N; i++) {
        po += snprintf(o + po, sizeof(o) - (size_t)po, " %s=%llu/%lluus", B[i],
                       (unsigned long long)c.hist_n[i], (unsigned long long)c.hist_us[i]);
        pq += snprintf(n2 + pq, sizeof(n2) - (size_t)pq, " %s=%llu/%lluus", B[i],
                       (unsigned long long)c.ihist_n[i], (unsigned long long)c.ihist_us[i]);
        if (po >= (int)sizeof(o) - 32 || pq >= (int)sizeof(n2) - 32) break; }
      kprintf("[BLK122] HIST outer:%s\n", o);
      kprintf("[BLK122] HIST IF0body:%s\n", n2); }
#if BLK122_WINDOWS >= 2
    kprintf("[BLK122] STALL WITNESS worst single 512B memcpy inside an IF=0 "
            "window = %lluus | page faults taken inside install windows = %llu "
            "in %llu window(s)\n",
            (unsigned long long)c.worst_iter, (unsigned long long)c.pf_in_win,
            (unsigned long long)c.n_win_with_pf);
#endif
}
#endif

// #121: ONE bracket over the whole block layer, RAM-cache hits included, so
// the syscall census can separate "the device was slow" from "everything
// above the device was slow". SCP_USBMSC below splits the actual device I/O
// back out of that, because a TO-RAM hit and a SCSI READ(10) are both blk_read
// and are four orders of magnitude apart.
static int blk_read_inner(uint8_t channel, uint8_t drive, uint64_t lba,
                          uint32_t count, void *buf, uint64_t *dev_us);
int blk_read(uint8_t channel, uint8_t drive, uint64_t lba, uint32_t count, void *buf) {
    scp_span_t __sb = scp_begin();
    // #122: per-CALL census, alongside #121's per-SYSCALL one. Two mono_us()
    // reads per blk_read; the smallest call this kernel issues is one sector
    // and the probe is ~0.02 us, so it cannot be a meaningful fraction of what
    // it measures. That is asserted, not assumed: blk_census_selftest()
    // measures the probe's own cost at boot and prints it next to the readings.
    uint64_t __t0 = mono_us();
    // #121's OWN LESSON, applied to this instrument: pass 1 derived this call's
    // device time as a delta of the GLOBAL accumulator, and the global is
    // written by every thread. The miss path deliberately drops the lock across
    // the blocking device read, so another thread's I/O lands in the delta. It
    // reported a worst call of 673893 us "of which device 843138 us" - a part
    // larger than the whole, which is the only reason it was caught, and is
    // exactly how #121 caught its own version of this. A per-call quantity has
    // to be carried on the CALLER'S STACK.
    uint64_t __dev = 0;
    int __r = blk_read_inner(channel, drive, lba, count, buf, &__dev);
    scp_end(SCP_BLKREAD, __sb);
    if (__t0) {
        uint64_t d = mono_us() - __t0;
        g_bc.calls++; g_bc.sectors += count; g_bc.t_call += d;
        g_bc.csz[blkhist_bucket_rs(count)]++;
        if (count == 1) {
            uint64_t ra = (uint64_t)(uintptr_t)__builtin_return_address(0);
            int slot = -1;
            for (int i = 0; i < BLK_C1_SITES; i++) {
                if (g_bc.c1_ret[i] == ra) { slot = i; break; }
                if (g_bc.c1_ret[i] == 0)  { g_bc.c1_ret[i] = ra; slot = i; break; }
            }
            if (slot < 0) g_bc.c1_other++;
            else { g_bc.c1_n[slot]++; if (__dev) g_bc.c1_dev[slot]++; }
        }
        if (d > g_bc.max_call) {
            g_bc.max_call = d; g_bc.max_call_sec = count; g_bc.max_call_dev = __dev;
        }
    }
    return __r;
}
static int blk_read_inner(uint8_t channel, uint8_t drive, uint64_t lba,
                          uint32_t count, void *buf, uint64_t *dev_us) {
    if (g_root_usb) {
        // TO-RAM: serve entirely from RAM (the win). Stick untouched.
        if (g_mode == BLK_MODE_TORAM && lba + count <= g_ram_sectors) {
            uint8_t *p = (uint8_t *)buf;
            // #122: ONE interrupts-off window covering the whole count. This is
            // the widest IF=0 window the block layer has, so it is the one the
            // ticket's "with interrupts off" clause is about.
            uint64_t __wt0, __wt1; uint64_t __bf = blk_lk_t(&__wt0, &__wt1);
            for (uint32_t i = 0; i < count; i++)
                memcpy(p + (uint64_t)i * BLK_SECTOR, chunk_sector(lba + i), BLK_SECTOR);
            g_ram_hits += count;
            blk_ul_t(__bf, __wt0, __wt1, (uint64_t)count * BLK_SECTOR, 0);
            g_bc.hits += count;
            return (int)count;
        }
        usb_msc_device_t *dev = usb_msc_get_device(g_root_usb_index);
        if (!dev || !dev->ready) return -1;
        uint8_t *p = (uint8_t *)buf;
        for (uint32_t i = 0; i < count; ) {
            uint64_t lba_i = lba + i;
            uint8_t *dst = p + (uint64_t)i * BLK_SECTOR;
            // Demand cache hit? Serve this one sector from RAM and move on.
            if (g_mode == BLK_MODE_CACHE) {
                uint32_t idx = (uint32_t)lba_i & g_mask;
                uint64_t __wt0, __wt1; uint64_t __bf = blk_lk_t(&__wt0, &__wt1);
                if (g_tag[idx] == lba_i) {
                    memcpy(dst, chunk_sector(idx), BLK_SECTOR);
                    g_ram_hits++;
                    // #122 kind 0 = this window copied a sector.
                    blk_ul_t(__bf, __wt0, __wt1, BLK_SECTOR, 0);
                    g_bc.hits++;
                    i++;
                    continue;
                }
                // #122 kind 1 = this window compared a tag and copied nothing.
                // Charged apart from the hits because #69 proved that averaging
                // a did-work population with a did-nothing one describes
                // neither, and pointed at the opposite fix.
                blk_ul_t(__bf, __wt0, __wt1, 0, 1);
                g_bc.misses++;
            }
            // #550 MISS: coalesce the run of consecutive sectors that ALSO miss
            // and fetch the whole run in ONE SCSI READ(10), straight into the
            // caller's buffer (which is contiguous, so no bounce buffer needed).
            // Stop the run at the first sector already cached: re-reading it from
            // USB would be slower than the memcpy we would otherwise do.
            uint32_t run = 1;
            while (run < BLK_USB_CHUNK && i + run < count) {
                if (g_mode == BLK_MODE_CACHE) {
                    uint64_t l2 = lba + i + run;
                    uint32_t idx2 = (uint32_t)l2 & g_mask;
                    int hit;
                    uint64_t __wt0, __wt1; uint64_t __bf = blk_lk_t(&__wt0, &__wt1);
                    hit = (g_tag[idx2] == l2);
                    blk_ul_t(__bf, __wt0, __wt1, 0, 1);   // #122: tag peek, no copy
                    if (hit) break;
                }
                run++;
            }
            // An xHCI Transfer TRB must NOT cross a 64 KB boundary (xhci_bulk_
            // transfer emits ONE TRB). The TO-RAM fill path gets this for free
            // because its chunks are 64 KB-aligned, but the CALLER's buffer here
            // has arbitrary alignment, so clip the run at the next 64 KB edge.
            uint64_t off  = (uint64_t)dst & (BLK_ALIGN64K - 1);
            uint32_t room = (uint32_t)((BLK_ALIGN64K - off) / BLK_SECTOR);
            if (room == 0) room = 1;
            if (run > room) run = room;

            // #617: snapshot the write generation BEFORE the blocking device
            // read, so any write that completes while the lock is dropped is
            // visible to us when we come back to install.
            //
            // Deliberately NOT under g_blk_lock, unlike the install check. An
            // aligned 64-bit load is atomic on x86-64 and g_wgen is volatile
            // and monotonically increasing, so the only thing the snapshot has
            // to guarantee is gen0 <= the generation the medium was in when our
            // read observed it - and reading it immediately before issuing the
            // read gives exactly that. Every ordering that could go "wrong"
            // goes wrong SAFELY: seeing a stale (lower) gen0 can only make the
            // install check fail and skip, never install stale bytes. Taking
            // the lock here would add a cli/sti round trip to every coalesced
            // miss run on the hottest read path in the product for no
            // correctness gain (#619 throughput).
            uint64_t gen0 = g_wgen;
            scp_span_t __su = scp_begin();   // #121: device I/O only
            uint64_t __dt0 = mono_us();      // #122: same span, per-call census
            int __ur = usb_msc_read(dev, 0, lba_i, dst, run);
            if (__dt0) { uint64_t __dd = mono_us() - __dt0;
                         g_bc.t_dev += __dd; g_bc.n_dev++; *dev_us += __dd; }
            g_bc.xsz[blkhist_bucket_rs(run)]++;
            scp_end(SCP_USBMSC, __su);
            if (__ur != 0) return -1;
            g_usb_reads += run;
            if (g_mode == BLK_MODE_CACHE) {
                uint64_t __wt0, __wt1; uint64_t __bf = blk_lk_t(&__wt0, &__wt1);
                uint64_t __ib = 0;
                if (g_wgen == gen0) {
                    // #122 pass 3: per-ITERATION timing. Two mono_us() reads per
                    // 512-byte copy is heavy relative to the copy, and that is
                    // accepted deliberately: this arm exists to answer "did the
                    // core stop", where a 30x probe overhead on a 0.06 us
                    // operation is still six orders of magnitude below the
                    // 182 ms being explained. The aggregate MB/s figures are
                    // taken from the hit path, which is NOT instrumented this
                    // way, so the two readings do not contaminate each other.
#if BLK122_WINDOWS >= 2
                    uint64_t __pk0 = 0, __pk1 = 0, __junk;
                    demand_get_stats(&__pk0, &__junk, &__junk, &__junk);
#endif
                    for (uint32_t k = 0; k < run; k++) {
                        uint32_t idx = (uint32_t)(lba_i + k) & g_mask;
#if BLK122_WINDOWS >= 2
                        uint64_t __k0 = mono_us();
#endif
                        memcpy(chunk_sector(idx), dst + (uint64_t)k * BLK_SECTOR,
                               BLK_SECTOR);
                        g_tag[idx] = lba_i + k;
#if BLK122_WINDOWS >= 2
                        if (__k0) { uint64_t __kd = mono_us() - __k0;
                                    if (__kd > g_bc.worst_iter) {
                                        g_bc.worst_iter = __kd;
                                        g_bc.worst_iter_bytes = BLK_SECTOR; } }
#endif
                    }
#if BLK122_WINDOWS >= 2
                    demand_get_stats(&__pk1, &__junk, &__junk, &__junk);
                    if (__pk1 > __pk0) { g_bc.pf_in_win += __pk1 - __pk0;
                                         g_bc.n_win_with_pf++; }
#endif
                    __ib = (uint64_t)run * BLK_SECTOR;
                } else {
                    // #617: a write landed while our I/O was in flight. Our
                    // image may already be stale and we cannot prove otherwise,
                    // so do not publish it as valid. The cache keeps the
                    // writer's copy, which is at least as fresh.
                    g_stale_skips++;
                }
                blk_ul_t(__bf, __wt0, __wt1, __ib, 2);   // #122: install window
            }
            i += run;
        }
        return (int)count;
    }
    // #122: the ATA transfer is DEVICE time and was bracketed by NOTHING, so on
    // an ATA-rooted volume 100% of it landed in scprof's "blk minus usb"
    // residual - the exact quantity this ticket calls "not the device". Both
    // transports are inside SCP_USBMSC now; the phase means DEVICE I/O, which
    // is what its own comment always claimed it meant.
    {   scp_span_t __sa = scp_begin();
        uint64_t __dt0 = mono_us();
        int __ar = ata_read_sectors_dma(channel, drive, (uint32_t)lba, (uint8_t)count, buf);
        if (__dt0) { uint64_t __dd = mono_us() - __dt0;
                     g_bc.t_dev += __dd; g_bc.n_dev++; *dev_us += __dd; }
        g_bc.xsz[blkhist_bucket_rs(count)]++;
        scp_end(SCP_USBMSC, __sa);
        return __ar; }
}

// #614: WRITE-side run coalescing - the App Store "20 minute freeze".
//
// blk_write issued ONE SCSI WRITE(10) per 512-byte sector, ALWAYS. A 103 MB
// package download (/STOREDL.TMP) plus its 104 MB unpack is ~207 MB = ~414,000
// Bulk-Only Transport commands, each costing THREE xHCI waits (CBW + data +
// CSW) all serialized on the single g_msc_cmd_busy command lock. The READ path
// was given run coalescing by #550; the write path never was, and that
// asymmetry is where essentially the whole install wall clock went.
//
// It could not just copy blk_read's shape, for a reason worth stating: the old
// loop advanced `src` by exactly one sector per iteration (its own comment said
// so), and, more fundamentally, xhci_bulk_transfer() puts the CALLER's pointer
// straight into a TRB as a PHYSICAL address while the kernel heap at
// 0x10000000 is vmm-remapped page by page to ARBITRARY physical pages
// (mm/heap.c) - so a >4 KB transfer directly out of a kmalloc'd buffer is not
// guaranteed physically contiguous. Writes therefore stage through a dedicated
// 64 KB-aligned, physically CONTIGUOUS bounce buffer: one memcpy per run (a few
// microseconds) buys a 64x reduction in USB commands and makes no contiguity
// assumption about the caller's buffer at all. If the bounce buffer cannot be
// allocated we fall back to the exact previous one-sector-per-command loop, so
// this can never turn a working write into a failing one.
#define BLK_WBOUNCE_SECS   BLK_USB_CHUNK                                  /* 64 sectors = 32 KB */
#define BLK_WBOUNCE_PAGES  ((BLK_WBOUNCE_SECS * BLK_SECTOR) / PMM_PAGE_SIZE)
// #SB: THE POINTER IS NOT HELD IN THIS FILE ANY MORE.
//
// It used to be `static uint8_t *g_wbounce`, reachable from every line of this
// translation unit, staged into with a bare memcpy and then handed to
// usb_msc_write() which PARKS on msc_cmd_lock(). A second writer running in
// that window overwrote the staged payload and the first writer then DMAd the
// second writer's bytes to the first writer's LBA. That destroyed the ext2
// primary superblock on the owner's boot stick (golden build 2215); the full
// byte-level evidence is in rustkern/blkstage.rs.
//
// The address now lives in rustkern/blkstage.rs and is only obtainable by
// taking an exclusive claim. This file keeps a bool, not a pointer, so there
// is no variable here that a future memcpy could accidentally target.
static int g_wbounce_tried = 0;
static int g_stage_ready   = 0;   // a staging buffer was installed successfully

// #SB owner-module FFI (rustkern/blkstage.rs).
extern int      blkstage_install_rs(uint64_t base, uint32_t bytes);
extern uint64_t blkstage_token_rs(void);
extern int      blkstage_free_rs(void);
extern uint64_t blkstage_try_claim_rs(uint64_t token);
extern int      blkstage_release_rs(uint64_t token);
extern int      blkstage_seal_rs(uint64_t token, uint32_t len);
extern int      blkstage_verify_rs(uint64_t token);

uint64_t g_blk_seal_broken = 0;          // see blockdev.h
static uint64_t g_stage_noblock  = 0;    // acquires that fell back: cannot park
static uint64_t g_stage_timeout  = 0;    // acquires that fell back: waited too long
static int      g_stage_shouted  = 0;    // victim LBAs reported so far

static void blk_wbounce_install_once(void) {
    if (g_wbounce_tried) return;
    g_wbounce_tried = 1;
    // Over-allocate by 64 KB so the usable base can be aligned UP to a 64 KB
    // boundary: an xHCI Transfer TRB must not cross 64 KB, and a 32 KB transfer
    // that starts on a 64 KB boundary never can.
    for (int attempt = 0; attempt < 8; attempt++) {
        uint64_t raw = pmm_alloc_pages(BLK_WBOUNCE_PAGES + 16u);
        if (!raw) break;
        uint64_t aligned = (raw + BLK_ALIGN64K - 1) & ~(BLK_ALIGN64K - 1);
        uint64_t hi = aligned + (uint64_t)BLK_WBOUNCE_PAGES * PMM_PAGE_SIZE;
        // Same heap-window hazard blk_alloc_chunks documents: a PHYSICAL page in
        // [0x10000000,0x20000000) is aliased by the heap's virtual remap, so its
        // identity pointer must never be used. Park it and retry.
        if (aligned < BLK_HEAP_WIN_HI && hi > BLK_HEAP_WIN_LO) {
            if (g_nparked < BLK_MAX_PARKED) { g_parked[g_nparked++] = raw; continue; }
            pmm_free_pages(raw, BLK_WBOUNCE_PAGES + 16u);
            break;
        }
        // Hand it to the owner module and FORGET THE ADDRESS. From this line
        // on, the only way to reach these bytes is blkstage_try_claim_rs().
        int ir = blkstage_install_rs(aligned,
                                     (uint32_t)((uint64_t)BLK_WBOUNCE_SECS * BLK_SECTOR));
        if (ir < 0) {
            // DURABLE, and from the fault-safe sink: this runs inside the block
            // write path, so bootlog_write()'s flush would re-enter blk_write().
            bootlog_fault_write("[BLKSTAGE] refused a SECOND staging buffer; keeping "
                    "the first. Two staging buffers is the shape this guard exists "
                    "to prevent.");
        } else {
            g_stage_ready = 1;
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// #SB: taking and giving back the staging claim.
// ---------------------------------------------------------------------------
// PARKING IS SAFE HERE AND IS NOT A NEW HAZARD. usb_msc_write(), one call
// deeper on this exact path, has parked on msc_cmd_lock() since #617, so any
// caller that can reach blk_write() on a USB root can already sleep here. The
// no-block branch below is the same wq_may_block() gate #617 uses, for the
// same reason (enumeration and IRQ context must not park).
//
// THE FALLBACK IS NOT A WORKAROUND FOR A MISSING WAKE (#426). Failing to get
// the claim degrades to the per-sector, no-bounce loop that shipped before
// #614 and is always correct: the caller's own buffer goes straight to the
// device, so there is nothing to steal. Losing the claim therefore costs
// throughput and never correctness, which is why a bounded wait is the right
// answer rather than an unbounded one. The wake source is our own release and
// is always armed; the bound exists so that a claim leaked by some future
// failure path can never wedge every write in the system.
static wait_queue_head_t g_stage_wq = { .head = NULL, .lock = SPINLOCK_INIT };
#define BLK_STAGE_PARK_MS  5      // one park quantum, then re-test the claim
#define BLK_STAGE_MAX_MS   500    // total, then take the always-correct slow path
//
// WHY 500 AND NOT SOMETHING LARGER. MEASURED, on the phase-B stress (4 writer
// threads, 60802 claims, 1298 of them contended): at a 2000 ms cap, 2 acquires
// waited the FULL two seconds before giving up. Two in sixty thousand is rare,
// and a two-second stall on a filesystem write is still a bad thing to leave in
// on purpose. The claim has no FIFO fairness (whoever wins the compare-exchange
// wins), so a writer CAN be starved; the cap is what bounds that, and the path
// it falls back to is correct, just slower. Both outcomes are right and one is
// more responsive, so the cap is the smaller number. Raising it again would
// need a measurement showing the fallback is costing more than the stall.

static uint8_t *blk_stage_acquire(uint64_t tok) {
    blk_wbounce_install_once();
    if (!g_stage_ready) return 0;             // no buffer: pre-#614 direct path
    // NOTE: this function is IDENTICAL on a `make BLKSTAGE_UNSAFE=1` build.
    // The knob is a RUSTFLAG and the whole behavioural difference lives inside
    // blkstage_try_claim_rs(), which on that build hands the buffer to every
    // caller without exclusion. Keeping the C the same in both builds is what
    // makes the RED/GREEN pair evidence about the CLAIM specifically, rather
    // than about two kernels that differ in more than one way.
    uint64_t p = blkstage_try_claim_rs(tok);
    if (p) return (uint8_t *)(uintptr_t)p;
    if (!wq_may_block()) { g_stage_noblock++; return 0; }
    uint64_t deadline = sched_now_ms() + BLK_STAGE_MAX_MS;
    for (;;) {
        wait_queue_entry_t wqe;
        __wait_prepare(&g_stage_wq, &wqe, 0);
        // Re-test AFTER queueing: this is the lost-wake close, the same
        // ordering msc_cmd_lock() documents.
        if (!blkstage_free_rs())
            (void)__wait_event_wait_deadline(&wqe, sched_now_ms() + BLK_STAGE_PARK_MS);
        __wait_finish(&g_stage_wq, &wqe);
        p = blkstage_try_claim_rs(tok);
        if (p) return (uint8_t *)(uintptr_t)p;
        if ((int64_t)(sched_now_ms() - deadline) >= 0) { g_stage_timeout++; return 0; }
    }
}

static void blk_stage_release(uint64_t tok) {
    if (!blkstage_release_rs(tok))
        // DURABLE. Same re-entrancy constraint as above: this is on the write
        // path, so the fault ring (no lock, no allocation, no filesystem) is the
        // only sink usable here. bootlog_heartbeat() drains it to /BOOTLOG.TXT
        // within about two seconds.
        bootlog_fault_write("[BLKSTAGE] release by a NON-OWNER (token %llu). Some path "
                "took the staging buffer without claiming it.", (unsigned long long)tok);
    wake_up_all(&g_stage_wq);
}

// One line, late in boot. A guard nobody has watched is indistinguishable from
// one that is switched off, so the numbers are printed even when they are all
// zero, which is the healthy case (blame.md).
void blk_stage_report(void) {
    blkstage_stats_t s;
    blkstage_stats_rs(&s);
    if (s.owned & BLKSTAGE_OWNED_UNSAFE_BUILD)
        bootlog_write("[BLKSTAGE] *** THIS KERNEL WAS BUILT BLKSTAGE_UNSAFE=1. The "
                "single-owner claim over the block write staging buffer is "
                "REMOVED ON PURPOSE and this kernel WILL corrupt the medium "
                "under concurrent writes. It exists only to prove the guard "
                "goes red. NEVER SHIP IT. ***");
    bootlog_write("[BLKSTAGE] buffer=%s claims=%llu rel=%llu contended=%llu "
            "fallback(noblock=%llu timeout=%llu) verified=%llu badrel=%llu "
            "STOLEN=%llu",
            s.base ? "installed" : "none",
            (unsigned long long)s.claims, (unsigned long long)s.releases,
            (unsigned long long)s.contended,
            (unsigned long long)g_stage_noblock, (unsigned long long)g_stage_timeout,
            (unsigned long long)s.verifies, (unsigned long long)s.bad_releases,
            (unsigned long long)s.seal_broken);
}

// #745 (task #62): DEVICE WRITE ACCOUNTING. The progressive-degradation report
// on the real iMac (responsive at boot, unusable inside a minute) is the #373
// shape, and #373 was a WRITE STORM over the slow USB-MSC stack. Nothing in the
// heartbeat record could see it: blkc=h/m counts block-cache READ hits/misses,
// and there was no write counter at all. These two are reported per-heartbeat
// so a stick brought back off the machine shows write traffic climbing (or
// flat, which rules the whole class out) alongside the widening beat gaps.
uint64_t g_blk_write_calls   = 0;
uint64_t g_blk_write_sectors = 0;

// ===========================================================================
// #250: auxiliary (non-root) volume I/O. See the comment in blockdev.h for why
// this is a separate entry point and not an argument on blk_read/blk_write.
// ===========================================================================
int blk_read_aux(int usb_index, uint64_t lba, uint32_t count, void *buf) {
    if (usb_index < 0 || !buf || count == 0) return -1;
    // A hot-plugged volume must NEVER be able to name the root device by
    // index. It cannot today (the root is selected at boot and hotplug slots
    // are separate), but the check is one line and the failure mode if it ever
    // does is a write to the boot medium.
    if (g_root_usb && usb_index == g_root_usb_index) return -1;
    usb_msc_device_t *dev = usb_msc_get_device(usb_index);
    if (!dev || !dev->ready) return -1;
    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < count; ) {
        uint32_t run = count - i;
        if (run > BLK_USB_CHUNK) run = BLK_USB_CHUNK;
        if (usb_msc_read(dev, 0, lba + i, p + (uint64_t)i * BLK_SECTOR, run) < 0) {
            return (i > 0) ? (int)i : -1;
        }
        i += run;
    }
    return (int)count;
}

int blk_write_aux(int usb_index, uint64_t lba, uint32_t count, const void *buf) {
    if (usb_index < 0 || !buf || count == 0) return -1;
    if (g_root_usb && usb_index == g_root_usb_index) return -1;
    usb_msc_device_t *dev = usb_msc_get_device(usb_index);
    if (!dev || !dev->ready) return -1;
    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < count; ) {
        uint32_t run = count - i;
        if (run > BLK_USB_CHUNK) run = BLK_USB_CHUNK;
        if (usb_msc_write(dev, 0, lba + i, p + (uint64_t)i * BLK_SECTOR, run) != 0) {
            return (i > 0) ? (int)i : -1;
        }
        i += run;
    }
    return (int)count;
}

int blk_write(uint8_t channel, uint8_t drive, uint64_t lba, uint32_t count, const void *buf) {
    g_blk_write_calls++;
    g_blk_write_sectors += count;
    if (g_root_usb) {
        usb_msc_device_t *dev = usb_msc_get_device(g_root_usb_index);
        if (!dev || !dev->ready) return -1;
        const uint8_t *p = (const uint8_t *)buf;
        // #SB: one token for this whole call. The claim is held across every
        // run of the loop, not re-taken per run: re-taking would reopen the
        // window between one run's memcpy and the next run's device call.
        uint64_t stok = blkstage_token_rs();
        uint8_t *bnc = blk_stage_acquire(stok);
        int stage_rc = (int)count;
        for (uint32_t i = 0; i < count; ) {
            uint64_t lba_i = lba + i;
            const uint8_t *src = p + (uint64_t)i * BLK_SECTOR;
            uint32_t run = 1;
            if (bnc) {
                run = count - i;
                if (run > BLK_WBOUNCE_SECS) run = BLK_WBOUNCE_SECS;
                memcpy(bnc, src, (uint64_t)run * BLK_SECTOR);
                (void)blkstage_seal_rs(stok, (uint32_t)((uint64_t)run * BLK_SECTOR));
            }
            // WRITE-THROUGH: persist to the physical medium (settings, notes, and
            // the /HEARTBEAT.TXT + /BOOTLOG.TXT diagnostics must survive power-off),
            // then keep the RAM copy coherent so reads stay instant.
            int __wr = usb_msc_write(dev, 0, lba_i,
                                     bnc ? (const void *)bnc : (const void *)src, run);
            // #SB: were the bytes the device just read still OUR bytes? On a
            // kernel built with the claim this can never fail. It is checked
            // anyway, always, because the alternative is a guard nobody has
            // ever seen a value from.
            if (bnc && blkstage_verify_rs(stok) < 0) {
                g_blk_seal_broken += run;
                // Every victim LBA is named, up to a cap. The first version of
                // this shouted once, which was enough to prove the fault and
                // not enough to say WHICH sectors of the filesystem now hold
                // the wrong bytes. That second question is the one a person
                // holding a broken disk actually asks.
                if (g_stage_shouted < 16) {
                    g_stage_shouted++;
                    bootlog_fault_write("[BLKSTAGE] CORRUPTION IN PROGRESS: the payload staged for "
                            "LBA %llu (%u sector(s)) was replaced between staging and "
                            "the device write. Some path is using the block write "
                            "staging buffer without holding the claim. This is the "
                            "fault that destroyed an ext2 primary superblock on golden "
                            "build 2215. The bytes now on the medium at that LBA are "
                            "NOT the bytes the filesystem asked to write.",
                            (unsigned long long)lba_i, (unsigned)run);
                }
#ifdef BLKSTAGE_TEST_DEVICE
                // See the file comment: freeze the medium so a later, correct
                // rewrite cannot heal the damage before it can be looked at.
                if (run > 1) {
                    extern void kpanic_halt(void);
                    kprintf("[BLKSTAGE] BLKSTAGE_FREEZE: halting immediately so the "
                            "corrupt bytes at LBA %llu stay on the medium. A running "
                            "kernel would rewrite this block correctly within seconds "
                            "and the evidence would be gone, which is exactly why a "
                            "clean fsck afterwards proves nothing.\n",
                            (unsigned long long)lba_i);
                    kpanic_halt();
                }
#endif
            }
            if (__wr != 0) {
                // Failed: RAM still holds the pre-write (on-disk) data, which is
                // correct, so leave TO-RAM untouched. For the demand cache, drop
                // any stale copy of the whole run so we don't serve unwritten data.
                if (g_mode == BLK_MODE_CACHE) {
                    uint64_t __bf = blk_lk();
                    g_wgen++;   // #617: an invalidate is a cache-visible change too
                    for (uint32_t k = 0; k < run; k++) {
                        uint32_t idx = (uint32_t)(lba_i + k) & g_mask;
                        if (g_tag[idx] == lba_i + k) g_tag[idx] = BLK_TAG_INVALID;
                    }
                    blk_ul(__bf);
                }
                stage_rc = -1;
                break;
            }
            if (g_mode == BLK_MODE_TORAM) {
                uint64_t __bf = blk_lk();
                for (uint32_t k = 0; k < run; k++) {
                    if (lba_i + k >= g_ram_sectors) break;
                    memcpy(chunk_sector(lba_i + k), src + (uint64_t)k * BLK_SECTOR,
                           BLK_SECTOR);
                }
                blk_ul(__bf);
            } else if (g_mode == BLK_MODE_CACHE) {
                uint64_t __bf = blk_lk();
                g_wgen++;   // #617: see the g_wgen comment. Must be inside the
                            // SAME critical section as the install, so a reader
                            // can never observe the new bytes with the old gen.
                for (uint32_t k = 0; k < run; k++) {
                    uint32_t idx = (uint32_t)(lba_i + k) & g_mask;
                    memcpy(chunk_sector(idx), src + (uint64_t)k * BLK_SECTOR, BLK_SECTOR);
                    g_tag[idx] = lba_i + k;
                }
                blk_ul(__bf);
            }
            i += run;
        }
        if (bnc) blk_stage_release(stok);
        return stage_rc;
    }
    return ata_write_sectors_dma(channel, drive, (uint32_t)lba, (uint8_t)count, buf);
}


#ifdef BLKSTAGE_TEST
// ===========================================================================
// #SB DESTRUCTIVE SELF-TEST.  `make BLKSTAGETEST=1`
// ===========================================================================
// WHY THIS EXISTS IN THIS SHAPE.
//
// The fix is an exclusive claim, and an exclusive claim makes the fault it
// removes INVISIBLE: with the claim in place the seal can never break, so a
// green line proves nothing on its own. blame.md's repeated finding is that
// this project ships self-tests that pass while testing the wrong thing, and
// that a guard nobody has watched go RED is indistinguishable from one that is
// switched off (#514/#665, and the four *TESTFAIL knobs that exist for exactly
// this reason). So this test is designed to be run TWICE:
//
//   make BLKSTAGETEST=1 BLKSTAGE_UNSAFE=1   -> claim removed, must report FAIL
//   make BLKSTAGETEST=1                     -> claim present, must report PASS
//
// Both kernels are otherwise identical and both contain this test, so a PASS
// is evidence about the CLAIM and not about the test being wired up.
//
// WHAT IT DOES AND DOES NOT COVER, stated plainly.
//
// COVERED: the staging layer itself, which is where the defect is. Three
// threads stage a payload that is unique to (thread, iteration), seal it, wait
// (standing in for the msc_cmd_lock() park that opens the real window), and
// then verify. On the unsafe build they trample each other; on the fixed build
// the claim serialises them.
//
// NOT COVERED: usb_msc_write() and the xHCI transfer beneath it. This test
// performs NO device I/O, deliberately. A self-test that writes to real
// sectors to prove a corruption bug is a self-test that can corrupt a real
// disk, and the whole reason this ticket exists is that this kernel already
// destroyed one filesystem. The end-to-end evidence is taken separately, on a
// throwaway image, by booting the unsafe build under real write load and
// reading the ext2 superblock back from the host.
// ===========================================================================
#include "../proc/process.h"
#include "fat.h"        // phase B writes REAL files through the routing writer
#include "panic.h"      // stage_set(): the UNLOCKED raw-sector breadcrumb path
extern fat_fs_t g_fat_fs;   // main.c; the single mounted FAT root (same
                            // private-extern idiom fs/devlog.c and fs/netfs.c
                            // already use; there is no header for it)

#define SBT_THREADS 3u
#define SBT_ITERS   200u
#define SBT_BYTES   (16u * 1024u)

// Never ships (the whole block is behind BLKSTAGE_TEST), so a static pattern
// buffer per thread is preferred over kmalloc: one fewer failure mode inside
// the thing being measured.
static uint8_t  g_sbt_pat[SBT_THREADS][SBT_BYTES];
static wait_queue_head_t g_sbt_delay_wq = { .head = NULL, .lock = SPINLOCK_INIT };
static wait_queue_head_t g_sbt_join_wq  = { .head = NULL, .lock = SPINLOCK_INIT };
static volatile int      g_sbt_live     = 0;
static volatile uint64_t g_sbt_ops      = 0;
static volatile uint64_t g_sbt_stolen   = 0;
static volatile uint64_t g_sbt_fallback = 0;

static void sbt_worker(void *arg) {
    // Identity comes from the argument proc_create passes through. It only has
    // to make each thread's payload distinct from the others.
    uint32_t id = (uint32_t)(uintptr_t)arg % SBT_THREADS;
    uint8_t *mine = g_sbt_pat[id];

    for (uint32_t it = 0; it < SBT_ITERS; it++) {
        // Unique to (thread, iteration) so a stolen payload can never fold to
        // the same value as ours by coincidence.
        for (uint32_t k = 0; k + 3 < SBT_BYTES; k += 4) {
            uint32_t v = ((id + 1u) << 24) ^ (it << 8) ^ k;
            mine[k]     = (uint8_t)v;
            mine[k + 1] = (uint8_t)(v >> 8);
            mine[k + 2] = (uint8_t)(v >> 16);
            mine[k + 3] = (uint8_t)(v >> 24);
        }

        uint64_t tok = blkstage_token_rs();
        uint8_t *b = blk_stage_acquire(tok);
        if (!b) { g_sbt_fallback++; continue; }
        memcpy(b, mine, SBT_BYTES);
        (void)blkstage_seal_rs(tok, SBT_BYTES);

        // Stand in for the msc_cmd_lock() park that opens the real window. A
        // bounded wait on a condition that is never true is a DELAY, not a
        // poll: the thread is off the run queue for the whole interval and
        // there is no wake it could miss, because there is no wake.
        (void)wait_event_timeout(&g_sbt_delay_wq, 0, wq_ms_to_ticks(1));

        if (blkstage_verify_rs(tok) < 0) g_sbt_stolen++;
        g_sbt_ops++;
        blk_stage_release(tok);
    }

    __sync_fetch_and_sub(&g_sbt_live, 1);
    wake_up_all(&g_sbt_join_wq);
}

#ifdef BLKSTAGE_TEST_DEVICE
// ---------------------------------------------------------------------------
// PHASE B: REAL DEVICE WRITES.  `make BLKSTAGETEST=1 BLKSTAGE_TEST_DEVICE=1`
// ---------------------------------------------------------------------------
// THIS PHASE WRITES TO THE ROOT FILESYSTEM AND, ON A BLKSTAGE_UNSAFE BUILD, IS
// EXPECTED TO DESTROY IT. Run it ONLY on a throwaway image.
//
// Phase A proves the staging layer races. It deliberately performs no device
// I/O, so on its own it cannot show the thing that actually happened to the
// owner's stick: an ext2 metadata write landing on the medium carrying another
// writer's bytes. Phase B closes that gap the only way it can be closed, by
// generating the concurrent write load a quiet boot never produces.
//
// THE PAIRING MATTERS AND THE OBVIOUS ONE DOES NOT WORK. MEASURED.
//
// The first version of this phase ran three concurrent fat_write_file()
// threads. It produced 12174 staging claims and contended=0: not once were two
// writers inside blk_write() at the same time, and REAL-SECTORS-CORRUPTED was
// 0 even on the unsafe build. fat_write_file() serialises on the filesystem
// lock, so file writes cannot race EACH OTHER.
//
// The pairing that can race is the one the stick actually shows.
// fs/panic.c's fixed_slot_write() is, by design, a "Raw, unlocked,
// single-sector overwrite" with "no fat_lock()", reached from stage_set() ->
// stage_flush(), and it targets /boot/STAGE.TXT on the FAT ESP. ext2 metadata
// updates take the ext2 path. The two share NO lock. That is precisely why the
// two damaged locations recovered from the stick were the /boot/STAGE.TXT data
// sector and the ext2 primary superblock, and nothing else.
//
// So phase B runs both: EVEN thread ids rewrite a file on the ext2 root
// (allocating and freeing blocks, which makes ext2 update the block bitmap,
// the group descriptor and THE PRIMARY SUPERBLOCK free counts), and ODD thread
// ids drive stage_set(), the unlocked breadcrumb writer.
#define SBTD_THREADS 4u
#define SBTD_ITERS   240u
#define SBTD_BYTES   (48u * 1024u)

static uint8_t g_sbtd_buf[SBTD_THREADS][SBTD_BYTES];
static wait_queue_head_t g_sbtd_join_wq = { .head = NULL, .lock = SPINLOCK_INIT };
static volatile int      g_sbtd_live = 0;
static volatile uint64_t g_sbtd_writes = 0;
static volatile uint64_t g_sbtd_errs = 0;

static void sbtd_worker(void *arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg % SBTD_THREADS;
    if (id & 1u) {
        // THE UNLOCKED BREADCRUMB WRITER. stage_set() -> stage_flush() ->
        // fixed_slot_write() -> fat_write_sector() -> blk_write(), taking no
        // filesystem lock on the way, which is what lets it overlap an ext2
        // metadata write.
        char detail[24];   // fs/panic.c truncates to STAGE_DETAIL_MAX itself
        for (uint32_t it = 0; it < SBTD_ITERS * 60u; it++) {
            snprintf(detail, sizeof(detail), "sbstress %u/%u", (unsigned)id,
                     (unsigned)it);
            stage_set(STAGE_FS_MOUNTED, detail);
            g_sbtd_writes++;
        }
    } else {
        uint8_t *buf = g_sbtd_buf[id];
        char path[24];
        snprintf(path, sizeof(path), "/SBSTRS%u.TMP", (unsigned)id);
        for (uint32_t it = 0; it < SBTD_ITERS; it++) {
            for (uint32_t k = 0; k + 3 < SBTD_BYTES; k += 4) {
                uint32_t v = ((id + 1u) << 24) ^ (it << 8) ^ k;
                buf[k]     = (uint8_t)v;
                buf[k + 1] = (uint8_t)(v >> 8);
                buf[k + 2] = (uint8_t)(v >> 16);
                buf[k + 3] = (uint8_t)(v >> 24);
            }
            if (fat_write_file(&g_fat_fs, path, buf, SBTD_BYTES) != 0) g_sbtd_errs++;
            else g_sbtd_writes++;
        }
    }
    __sync_fetch_and_sub(&g_sbtd_live, 1);
    wake_up_all(&g_sbtd_join_wq);
}

static void blk_stage_device_stress(void) {
    kprintf("[BLKSTAGE-DEV] *** THIS BUILD WRITES TO THE ROOT FILESYSTEM ON "
            "PURPOSE AND MAY DESTROY IT. THROWAWAY IMAGES ONLY. ***\n");
    if (!g_fat_fs.mounted) {
        kprintf("[BLKSTAGE-DEV] SKIPPED: no filesystem mounted.\n");
        return;
    }
    uint64_t stolen_before = blkstage_seal_broken_rs();
    g_sbtd_live = (int)SBTD_THREADS;
    for (uint32_t t = 0; t < SBTD_THREADS; t++) {
        if (proc_create("sbdev", sbtd_worker, (void *)(uintptr_t)t, PRIO_NORMAL) < 0)
            __sync_fetch_and_sub(&g_sbtd_live, 1);
    }
    int wr = wait_event_timeout(&g_sbtd_join_wq, g_sbtd_live == 0,
                                wq_ms_to_ticks(180000));
    uint64_t stolen_after = blkstage_seal_broken_rs();
    kprintf("[BLKSTAGE-DEV] threads=%u iters=%u payload=%uB writes=%llu errs=%llu "
            "finished=%s REAL-SECTORS-CORRUPTED=%llu\n",
            (unsigned)SBTD_THREADS, (unsigned)SBTD_ITERS, (unsigned)SBTD_BYTES,
            (unsigned long long)g_sbtd_writes, (unsigned long long)g_sbtd_errs,
            (wr == WAIT_OK) ? "yes" : "NO",
            (unsigned long long)(stolen_after - stolen_before));
}
#endif // BLKSTAGE_TEST_DEVICE

void blk_stage_selftest(void) {
    if (!g_root_usb) {
        bootlog_write("[BLKSTAGE-TEST] SKIPPED: this machine has an ATA root, so no "
                "staging buffer is installed and there is nothing to race. The "
                "defect is USB-root-specific, which is why no VM ever showed it.");
        return;
    }
    blk_wbounce_install_once();
    if (!g_stage_ready) {
        bootlog_write("[BLKSTAGE-TEST] SKIPPED: no staging buffer could be installed.");
        return;
    }

    g_sbt_ops = 0; g_sbt_stolen = 0; g_sbt_fallback = 0;
    g_sbt_live = (int)SBT_THREADS;
    for (uint32_t t = 0; t < SBT_THREADS; t++) {
        if (proc_create("sbtest", sbt_worker, (void *)(uintptr_t)t, PRIO_NORMAL) < 0) {
            __sync_fetch_and_sub(&g_sbt_live, 1);
            bootlog_write("[BLKSTAGE-TEST] could not create worker %u", (unsigned)t);
        }
    }

    int wr = wait_event_timeout(&g_sbt_join_wq, g_sbt_live == 0,
                                wq_ms_to_ticks(60000));
    int finished = (wr == WAIT_OK);

    // The verdict states its own preconditions. "0 stolen" out of 0 operations
    // is the classic pass-while-testing-nothing, so ops > 0 is part of PASS.
    const char *verdict;
    if (!finished)               verdict = "INCONCLUSIVE (workers did not finish)";
    else if (g_sbt_ops == 0)     verdict = "INCONCLUSIVE (no payload was ever staged)";
    else if (g_sbt_stolen == 0)  verdict = "PASS";
    else                         verdict = "FAIL";

    bootlog_write("[BLKSTAGE-TEST] threads=%u iters=%u payload=%uB staged=%llu "
            "STOLEN=%llu fallback=%llu -> %s",
            (unsigned)SBT_THREADS, (unsigned)SBT_ITERS, (unsigned)SBT_BYTES,
            (unsigned long long)g_sbt_ops, (unsigned long long)g_sbt_stolen,
            (unsigned long long)g_sbt_fallback, verdict);
    {
        blkstage_stats_t bs;
        blkstage_stats_rs(&bs);
        if (bs.owned & BLKSTAGE_OWNED_UNSAFE_BUILD)
            bootlog_write("[BLKSTAGE-TEST] this kernel was built BLKSTAGE_UNSAFE=1: the "
                    "claim is removed on purpose, so FAIL is the EXPECTED result "
                    "and a PASS here would mean the test is not testing what it "
                    "says it tests.");
    }
#ifdef BLKSTAGE_TEST_DEVICE
    blk_stage_device_stress();
#endif
    // #DIAGLOG: the counters AGAIN, now that the race has been run. main.c
    // calls blk_stage_report() BEFORE this test, where seal_broken is
    // necessarily 0 on any build, so that first line can only ever show the
    // healthy value. This second one is what makes the RED arm legible: on a
    // BLKSTAGE_UNSAFE=1 kernel it carries a NON-ZERO STOLEN=, durably, in
    // /BOOTLOG.TXT. A counter never observed non-zero is worth nothing.
    blk_stage_report();
}
#endif // BLKSTAGE_TEST
