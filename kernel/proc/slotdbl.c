// slotdbl.c - #heapinvptr: the DETERMINISTIC reproducer for the DOUBLE RELEASE of
// a process slot's resources, and the regression gate for its fix.
//
// WHAT IT REPRODUCES, EXACTLY, AND WHY IT IS THE SECOND SPAWN.
//
// proc_create_user_as() has four failure exits that run AFTER some combination of
// the kernel stack, the user address space and the user stack pages already
// exists. Each one released those resources and then set
// state = PROC_STATE_UNUSED while LEAVING stack_base, cr3, user_stack_base and
// user_stack_size in the PCB still pointing at the memory it had just handed
// back. alloc_proc_slot() claims the LOWEST-INDEX UNUSED slot and then calls
// cleanup_proc_slot() on it, and the slot a failed spawn has just released IS
// that lowest-index slot: it was the lowest when it was claimed, and nothing
// below it became free in between. So the NEXT spawn released the same kernel
// stack, the same user stack pages and the same page tables a SECOND time.
//
// That is the whole of "it fires reliably on the SECOND spawn in a sequence". The
// second spawn is not the one that is broken; it is the one that steps on the
// first spawn's corpse.
//
// THE MEASUREMENT THAT NAMED IT (/root/co2600-fatapps-run.log, golden 2600, two
// independent boots, same three lines in the same order):
//     [PROC] WRITER: refused initial stack (argc=4 envc=3)
//     [HEAP] ERROR: kfree() called on invalid pointer 0x000000000fdb1000
//            (magic=0xbaadb4dd want 0x48454150)
//     [PMM] WARNING: Double free of page 0x10b1c000   (and three more)
// 0x0fdb1000 is 0x0fdb0000 + KSTACK_BAND with 0x0fdb0000 granule-aligned, which
// is exactly the shape kstack_alloc() hands out, and 0xbaadb4dd is KSTACK_POISON
// from mm/kstack.c. So the "invalid heap pointer" was never a heap pointer: it
// was a kernel stack being freed a second time, whose registry row was already
// gone, routed into kfree() by kstack_free()'s fallback, where the guard band was
// read as a block header. The [PMM] lines are vmm_destroy_user_space() walking
// page tables it had already freed.
//
// HOW IT FORCES THE FAILURE EXIT WITHOUT INJECTING ANYTHING. setup_user_stack()
// REFUSES an environment entry that is not NAME=VALUE, by design (#112: an entry
// with no '=' can never be found by getenv(), so carrying it would read back as
// absent and dropping it would run the child with an environment the caller did
// not ask for). One such entry takes the "refused initial stack" exit, which is
// the exact exit the measurement above took. There is no #ifdef-only code path
// and no fault injection: the reproducer drives the SHIPPING code with a legal
// argument the shipping code is documented to refuse, so what it exercises
// cannot drift away from what ships.
//
// WHAT IT MEASURES, AND WHY DELTAS. Four counters, sampled before and after:
//     kstack_free_foreign_count()   a kstack_free() that fell through to kfree()
//     kstack_free_orphan_count()    a guarded kernel stack freed twice (0 until
//                                   the detector in mm/kstack.c exists)
//     pmm_double_free_count()       a physical page handed back while already free
//     heap_invalid_free_count()     a kfree()/krealloc() refused
// A delta on ANY of them is a FAILURE. Deltas and not absolutes, deliberately:
// there are OTHER, unrelated defects that move these same counters elsewhere in
// the boot (blame.md names two this pass did not fix), and a reproducer that
// reported somebody else's bug as its own result is a verifier that cannot fail
// honestly.
//
// OFF BY DEFAULT, started only when /SLOTDBL.TXT is present on the ESP: the arm
// with the reproducer and the arm without are the same kernel bytes.
//
// LANGUAGE NOTE (project rule: new kernel code is Rust unless there is a stated
// reason). This file is C and the reason is that it is nothing but calls into the
// C process-creation path it exists to drive: proc_create_user_as(),
// fat_read_file() and four counter accessors. There is no decision in it worth a
// type system; the one piece of judgement in this change that IS pure arithmetic
// over integers, "is this unregistered pointer actually a kernel stack", lives in
// rustkern/kstackid.rs where it belongs.
#include "process.h"
#include "../types.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../mm/kstack.h"
#include "../security/selftest_registry.h"   // #PERMSKIP: skips are REGISTERED

extern int kprintf(const char *fmt, ...);
extern fat_fs_t g_fat_fs;

int g_slotdbl_gate = 0;          // set by main.c from /SLOTDBL.TXT

// Four rounds, so there are THREE second-spawns after the first poisoning. One
// round would allocate the slot and poison it and prove nothing, which is the
// shape of a self-test that asks nothing and therefore answers yes.
#define SLOTDBL_ROUNDS 4

// Any ELF on the image will do: the reproducer never lets one run, it only needs
// the ELF load to SUCCEED so that the refusal happens at the user-stack step
// (the exit the measurement took) rather than earlier. Tried in order so the rig
// is not hostage to one app's name on a future image.
static const char *const g_slotdbl_bins[] = {
    "/APPS/HELLO", "/APPS/TRUE", "/APPS/ECHO", "/APPS/CAT",
    "/APPS/CLEAR", "/APPS/MSH", "/APPS/TERMINAL", 0
};

static void slotdbl_worker(void *arg) {
    (void)arg;

    uint32_t sz = 0;
    void *elf = 0;
    const char *path = 0;
    for (int i = 0; g_slotdbl_bins[i]; i++) {
        uint32_t got = 0;
        void *d = fat_read_file(&g_fat_fs, g_slotdbl_bins[i], &got);
        if (d && got >= 64) { elf = d; sz = got; path = g_slotdbl_bins[i]; break; }
        if (d) kfree(d);
    }
    if (!elf) {
        selftest_notrun("SLOTDBL", "no spawnable binary found on this image; the "
                                   "slot double-release reproducer did NOT run");
        return;
    }

    uint64_t f0 = kstack_free_foreign_count();
    uint64_t o0 = kstack_free_orphan_count();
    uint64_t p0 = pmm_double_free_count();
    uint64_t h0 = heap_invalid_free_count();

    kprintf("[SLOTDBL] ARMED on %s (%u bytes). %d rounds of a spawn that is "
            "REFUSED at the user-stack step; round 2 onward is the 'second spawn' "
            "that reuses the slot round 1 released. Baseline: kstack_foreign=%lu "
            "kstack_orphan=%lu pmm_double=%lu heap_invalid=%lu\n",
            path, (unsigned)sz, SLOTDBL_ROUNDS,
            (unsigned long)f0, (unsigned long)o0,
            (unsigned long)p0, (unsigned long)h0);

    // ONE entry, no '='. setup_user_stack() refuses it by design; see the header.
    char *bad_env[2];
    bad_env[0] = (char *)"SLOTDBL_NOT_AN_ASSIGNMENT";
    bad_env[1] = 0;

    for (int round = 0; round < SLOTDBL_ROUNDS; round++) {
        int r = proc_create_user_as("SLOTDBL", elf, sz, 0, bad_env, proc_as_uid(0));
        kprintf("[SLOTDBL] round %d: proc_create_user_as -> %d (a negative value "
                "is the EXPECTED refusal)\n", round + 1, r);
        if (r >= 0) {
            // The refusal did not happen, so the failure exit was never taken and
            // nothing was tested. Say so rather than reporting a pass.
            selftest_notrun("SLOTDBL", "a spawn that should have been REFUSED "
                                       "succeeded; the failure exit under test was "
                                       "never reached");
            kfree(elf);
            return;
        }
    }

    uint64_t f1 = kstack_free_foreign_count();
    uint64_t o1 = kstack_free_orphan_count();
    uint64_t p1 = pmm_double_free_count();
    uint64_t h1 = heap_invalid_free_count();

    int fail = (f1 != f0) || (o1 != o0) || (p1 != p0) || (h1 != h0);

    kprintf("[SLOTDBL] RESULT: %s | kstack_foreign +%lu | kstack_orphan +%lu | "
            "pmm_double +%lu | heap_invalid +%lu (all four MUST be +0)\n",
            fail ? "FAIL - the slot's resources were released twice" : "PASS",
            (unsigned long)(f1 - f0), (unsigned long)(o1 - o0),
            (unsigned long)(p1 - p0), (unsigned long)(h1 - h0));
    bootlog_write("[SLOTDBL] RESULT %s foreign+%lu orphan+%lu pmmdbl+%lu "
                  "heapinv+%lu",
                  fail ? "FAIL" : "PASS",
                  (unsigned long)(f1 - f0), (unsigned long)(o1 - o0),
                  (unsigned long)(p1 - p0), (unsigned long)(h1 - h0));
    if (fail)
        kprintf("[SLOTDBL] FAIL means a failed spawn left its released kernel "
                "stack / user pages / page tables still recorded in the PCB, and "
                "the next spawn's alloc_proc_slot() freed them again. See the "
                "header of proc/slotdbl.c.\n");

    kfree(elf);
}

void slotdbl_start(void) {
    if (!g_slotdbl_gate) return;
    int pid = proc_create("slotdbl", slotdbl_worker, 0, PRIO_LOW);
    kprintf("[SLOTDBL] reproducer worker -> pid %d\n", pid);
    bootlog_write("[SLOTDBL] reproducer worker started (pid %d)", pid);
}
