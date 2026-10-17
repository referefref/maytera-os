// proc/fdreclaim_test.c - #fdreclaim: GATED boot oracle for the legacy fd slot
// leak and its reclaim (proc/fdlayer.c, rustkern/fdown.rs).
//
// TEST SCAFFOLDING, not a shipping feature. It is a no-op unless
// /CONFIG/FDRECLAIM.TEST exists on the root fs, exactly like the #fdguard
// (proc/fdguard_test.c) and #708 (proc/aieject_vmtest.c) launchers it is
// modelled on, so it never runs in a production golden.
//
// WHAT IT MEASURES, AND WHY THIS SHAPE.
//
// The claim being tested is "a process that dies without close()ing burns its
// legacy fd slots for the rest of the boot". The only direct evidence for that
// is the pool's FREE-SLOT CEILING falling across process deaths and never
// coming back, so that is what this samples: legacy_fd_free_slots() before and
// after each of several launches of /APPS/FDPROBE in its no-close mode, which
// is a program whose whole job is to open files and exit without closing them.
//
// PRE-FIX (or with /CONFIG/FDRECLAIM.OFF armed, which restores exactly the old
// behaviour on this same kernel) the ceiling drops to near zero on the FIRST
// run and stays there; every later run reports ok=0 because -EMFILE is the
// first thing it meets. POST-FIX the ceiling returns to its starting value
// after every run, because the dying process hands its slots back.
//
// WHY A KERNEL LAUNCHER AND NOT A SHELL COMMAND. The golden boots straight to
// the GUI; the kernel serial shell is reachable only when there is no
// framebuffer at all, it has no command that runs a userland ELF, and QEMU
// pointer injection into the GUI terminal is unreliable (#334). Launching
// FDPROBE from a kernel worker puts both its output and this oracle's verdict on
// the serial line a headless VM can capture, and the verdict also goes to
// /BOOTLOG.TXT for a durable record.
//
// It is C, not Rust, for the same stated reason fdguard_test.c and
// aieject_vmtest.c are: it is entangled with the C launch path (fat_read_file +
// elf_validate + proc_create_user_as + proc_wait), which has no Rust surface,
// and it is throwaway verification code, not a new kernel subsystem. The logic
// this change ADDS to the kernel proper lives in Rust (rustkern/fdown.rs).

#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "process.h"
#include "fdown.h"

extern fat_fs_t g_fat_fs;
extern void kprintf(const char *fmt, ...);
extern void proc_sleep(uint32_t ms);
extern int  elf_validate(const void *data, uint32_t size);
extern uint64_t heap_get_free_size(void);

#define FDR_ORACLE_REV 2   // bump on any change to this oracle
#define FDR_OPENS "200"     // more than the 125 usable slots, so it always hits the ceiling

// The files FDPROBE reopens, one per run, chosen to separate the two halves of
// the leak rather than to conflate them. Both are on the ext2 root of every
// image.
//
//   THEME.CFG is a few bytes, so each slot's #572 read window is a few bytes
//   too: if the free-slot ceiling still collapses here, it is the SLOTS that
//   leak, and no argument about heap pressure can explain it away.
//
//   CACERTS.PEM is ~220 KB, comfortably past EXT2_RWIN_BYTES, so every slot
//   pins a FULL 128 KiB window: the heap delta across the run is the other half
//   of the leak, the part measured in megabytes.
// BIG FILE FIRST, and the order is load-bearing. Measured 2026-09-28 with the
// small file first: run 1 burned all 125 slots, so runs 2-4 never obtained one
// and the large-window heap cost was never exercised at all. The big file has to
// go first or the heap half of the leak cannot be measured.
static const char *const FDR_TARGETS[] = {
    "/CONFIG/CACERTS.PEM",
    "/CONFIG/THEME.CFG",
    "/CONFIG/CACERTS.PEM",
    "/CONFIG/THEME.CFG",
};
#define FDR_RUNS ((int)(sizeof(FDR_TARGETS) / sizeof(FDR_TARGETS[0])))

// Launch /APPS/FDPROBE <target> 200 nc and wait for it to die. Returns its exit
// status, or -1 if it could not be launched at all.
static int fdr_run_probe(int run, const char *target)
{
    if (!g_fat_fs.mounted) {
        kprintf("[FDRECLAIM-TEST] no root fs\n");
        return -1;
    }
    // fat_read_file() routes to the ext2 root on an ext2-root image, which is
    // where /APPS lives; this is the same call gui/desktop.c's launcher makes.
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, "/APPS/FDPROBE", &sz);
    if (!data || sz == 0) {
        if (data) kfree(data);
        kprintf("[FDRECLAIM-TEST] /APPS/FDPROBE not found "
                "(build with --with-unshipped-apps)\n");
        return -1;
    }
    if (elf_validate(data, sz) != 0) {
        kfree(data);
        kprintf("[FDRECLAIM-TEST] /APPS/FDPROBE is not a valid ELF\n");
        return -1;
    }
    // A NULL-terminated kernel array of kernel strings, per setup_user_stack().
    char *argv[5];
    argv[0] = (char *)"/APPS/FDPROBE";
    argv[1] = (char *)target;
    argv[2] = (char *)FDR_OPENS;
    argv[3] = (char *)"nc";
    argv[4] = 0;
    int pid = proc_create_user_as("FDPROBE", data, sz, argv, 0, proc_as_uid(0));
    kfree(data);
    if (pid <= 0) {
        kprintf("[FDRECLAIM-TEST] run %d: spawn refused rc=%d\n", run, pid);
        return -1;
    }
    int status = 0;
    // proc_wait() is the shared primitive for "sleep until this child exits";
    // it is what makes this a measurement of the state AFTER proc_exit() has
    // run, which is where the reclaim lives. No hand-rolled poll (#426).
    proc_wait(pid, &status);
    return status;
}

static void fdreclaim_test_worker(void *arg)
{
    (void)arg;
    // Let the fs settle, the desktop come up and the background daemons take
    // whatever fds they are going to take, so the starting ceiling is the real
    // steady-state one and not a boot transient.
    proc_sleep(12000);

    uint32_t csz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/FDRECLAIM.TEST", &csz);
    if (!cfg) return;          // not flagged -> silent no-op (production path)
    kfree(cfg);

    // Dev-only: put the leak BACK if its marker is present, so the SAME image
    // can show the measurement RED (leak) and GREEN (reclaimed).
    {
        uint32_t bsz = 0;
        char *b = (char *)fat_read_file(&g_fat_fs, "/CONFIG/FDRECLAIM.OFF", &bsz);
        if (b) { kfree(b); fdreclaim_set_disabled(1); }
    }

    int start = legacy_fd_free_slots();
    kprintf("\n========== LEGACY FD SLOT RECLAIM ORACLE (#fdreclaim) ==========\n");
    uint64_t heap0 = heap_get_free_size();
    // A build-identity line, because a measurement attributed to the wrong
    // kernel is worse than no measurement: one confused boot on 2026-09-28 was
    // enough to learn that. FDR_ORACLE_REV is bumped by hand whenever this
    // oracle changes shape, and the first target is printed so the run ORDER is
    // on the record beside its numbers.
    kprintf("[FDRECLAIM-TEST] oraclerev=%d opens=%s runs=%d first=%s\n",
            FDR_ORACLE_REV, FDR_OPENS, FDR_RUNS, FDR_TARGETS[0]);
    kprintf("[FDRECLAIM-TEST] free slots at start: %d  heapfreeKB=%lu\n",
            start, (unsigned long)(heap0 / 1024));
    bootlog_write("[FDRECLAIM-TEST] start free=%d heapfreeKB=%lu",
                  start, (unsigned long)(heap0 / 1024));

    int prev = start;
    int worst = start;
    int declines = 0;
    for (int run = 1; run <= FDR_RUNS; run++) {
        const char *target = FDR_TARGETS[run - 1];
        kprintf("[FDRECLAIM-TEST] run %d target=%s\n", run, target);
        int st = fdr_run_probe(run, target);
        if (st == -1) {
            kprintf("[FDRECLAIM-TEST] ABORTED at run %d (could not launch)\n", run);
            bootlog_write("[FDRECLAIM-TEST] ABORTED at run %d", run);
            kprintf("========== LEGACY FD SLOT RECLAIM ORACLE ABORTED ==========\n");
            return;
        }
        int now = legacy_fd_free_slots();
        uint64_t hnow = heap_get_free_size();
        kprintf("[FDRECLAIM-TEST] after run %d: free=%d (delta vs prev %d, "
                "vs start %d) heapfreeKB=%lu (vs start %ld) probe_exit=%d "
                "poisoned=%u reclaimed=%u\n",
                run, now, now - prev, now - start,
                (unsigned long)(hnow / 1024),
                (long)((long)(hnow / 1024) - (long)(heap0 / 1024)), st,
                fdown_exit_marks_rs(), fdown_reaped_rs());
        bootlog_write("[FDRECLAIM-TEST] run %d free=%d (start %d) heapKB=%lu "
                      "(start %lu) reclaimed=%u",
                      run, now, start, (unsigned long)(hnow / 1024),
                      (unsigned long)(heap0 / 1024), fdown_reaped_rs());
        if (now < prev) declines++;
        if (now < worst) worst = now;
        prev = now;
    }

    // THE VERDICT. "Recovered" is the load-bearing word: the ceiling after the
    // last death must be back where it started. A pool that merely declines
    // slowly is still leaking.
    int recovered = (prev >= start);
    kprintf("[FDRECLAIM-TEST] heapfreeKB start=%lu end=%lu (delta %ld)\n",
            (unsigned long)(heap0 / 1024),
            (unsigned long)(heap_get_free_size() / 1024),
            (long)((long)(heap_get_free_size() / 1024) - (long)(heap0 / 1024)));
    kprintf("[FDRECLAIM-TEST] VERDICT: %s  start=%d end=%d worst=%d "
            "declining_runs=%d/%d poisoned=%u reclaimed=%u\n",
            recovered ? "SLOTS RECLAIMED (pool stable)"
                      : "SLOTS LEAKED (pool declining)",
            start, prev, worst, declines, FDR_RUNS,
            fdown_exit_marks_rs(), fdown_reaped_rs());
    bootlog_write("[FDRECLAIM-TEST] VERDICT %s start=%d end=%d worst=%d "
                  "poisoned=%u reclaimed=%u",
                  recovered ? "RECLAIMED" : "LEAKED",
                  start, prev, worst,
                  fdown_exit_marks_rs(), fdown_reaped_rs());
    kprintf("========== LEGACY FD SLOT RECLAIM ORACLE DONE ==========\n");
}

void fdreclaim_start_deferred_test(void)
{
    proc_create_ex("fdrtest", fdreclaim_test_worker, 0, PRIO_LOW, 256 * 1024);
}
