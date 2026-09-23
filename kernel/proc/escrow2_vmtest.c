// proc/escrow2_vmtest.c - #246/#305 GATED boot launcher for the STAGE 2
// (mandatory / always-marked) kernel-escrow verifier /APPS/ESCROW2T
// (docs/CONTRACT_ENFORCEMENT_PLAN.md, Stage 2).
//
// TEST SCAFFOLDING, not a shipping feature. It is a no-op unless
// /CONFIG/ESCROW2.TEST exists on the root fs, exactly like proc/escrow_vmtest.c
// (Stage 1) which it is modelled on, so it never runs in a production golden
// (ESCROW2T ships on no image; it is on build/unshipped-apps.list). It is C, not
// Rust, for the same reason escrow_vmtest.c is: it is entangled with the C
// launch path (fat_read_file + escrow_spawn_marked), which has no Rust surface,
// and it is throwaway verification code, not a new kernel subsystem.
//
// WHAT IT PROVES. Unlike Stage 1's ESCROWT (which ENTERs escrow itself), this
// launcher uses the TRUSTED SPAWNER escrow_spawn_marked() to create the task
// ALREADY MARKED + LOCKED, so ESCROW2T never runs unmarked and cannot free
// itself. The kernel does the marking; the Ring-3 app only tries (and is
// refused) the escapes.

#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "process.h"

extern fat_fs_t g_fat_fs;
extern void kprintf(const char *fmt, ...);
extern void proc_sleep(uint32_t ms);
// The Stage 2 trusted spawner (fs/escrow_guard.c).
extern int escrow_spawn_marked(const char *path, const char *scope,
                               uint32_t ttl_ms, uint32_t uid);

static void escrow2_vmtest_worker(void *arg) {
    (void)arg;
    // Run AFTER the Stage 1 test worker (which sleeps 7000ms) so the two
    // verdicts do not interleave on the serial line.
    proc_sleep(11000);
    uint32_t csz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/ESCROW2.TEST", &csz);
    if (!cfg) return;   // not flagged -> silent no-op (production path)
    kfree(cfg);

    kprintf("\n========== ESCROW STAGE 2 (MANDATORY) TEST (#246) ==========\n");
    if (!g_fat_fs.mounted) { kprintf("[ESCROW2-TEST] no fs\n"); return; }

    // Create the granted scope root /ESCROW2SC FIRST, kernel-side. It MUST
    // pre-exist: a MANDATORY actor is marked from instruction 1 and can only
    // write INSIDE its granted scope, so it cannot create its own scope root;
    // and a create UNDER a missing directory returns a bogus success fd whose
    // file is never visible, which made ESCROW2T go RED for a reason UNRELATED
    // to enforcement whenever a deployment forgot to pre-create it (measured:
    // ESCROW2T-DIAG after_create=0, mark never visible; see blame.md
    // kescrow2fix). Creating it here makes the test SELF-CONTAINED and
    // DETERMINISTIC regardless of who deployed it. fat_mkdir routes to the ext2
    // root; a non-zero return just means it already exists (idempotent). This is
    // C for the same reason the rest of this launcher is: it is entangled with
    // the C fat/ext2 API and the C launch path, and it is throwaway test
    // scaffolding, not a new kernel subsystem.
    (void)fat_mkdir(&g_fat_fs, "/ESCROW2SC");
    // uid 0: the shipped image autologins root, so proving a uid-0 Ring-3 task
    // cannot escape is the strong case.
    int pid = escrow_spawn_marked("/APPS/ESCROW2T", "/ESCROW2SC", 3600000u, 0);
    kprintf("[ESCROW2-TEST] escrow_spawn_marked rc/pid=%d "
            "(>0 = spawned already-marked); watch for ESCROW2T: lines\n", pid);
    kprintf("========== ESCROW STAGE 2 TEST ARMED ==========\n");
}

void escrow2_start_deferred_test(void) {
    proc_create_ex("escrow2test", escrow2_vmtest_worker, 0, PRIO_LOW, 256 * 1024);
}
