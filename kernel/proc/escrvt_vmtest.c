// proc/escrvt_vmtest.c - GATED boot launcher for /APPS/ESCRVT, the
// UNCOOPERATIVE-ACTOR verifier for the recycling AI delete.
//
// TEST SCAFFOLDING, not a shipping feature. A no-op unless
// /CONFIG/ESCRVT.TEST exists on the root fs, modelled on proc/escrow2_vmtest.c
// which it sits beside. ESCRVT and its helper ESCRVTH are on
// build/unshipped-apps.list, so they are absent from a production golden.
//
// HOW THE MARKER FILE IS NO LONGER A WAY FOR THIS TO ROT. Four escrow
// verifiers were found on 2026-10-03 to have NEVER RUN in any automated
// context, because they are gated behind exactly this kind of marker and
// nothing in the tree created one: every boot they started, found no marker,
// and returned having checked nothing. The fix is not to remove the gate (a
// test that spawns Ring-3 processes and deletes files has no business running
// on a user's machine) but to make something CREATE the marker and ASSERT the
// output: tools/escrow-vmtest/run-escrow-vmtests.sh injects all five markers
// into a COPY of a golden, boots a throwaway VM, and fails if the expected
// PASS lines are absent. The gate stays; the inertness does not.
//
// C, not Rust, for the same reason escrow2_vmtest.c is: it is entangled with
// the C launch path (fat_read_file + fat_mkdir + escrow_spawn_marked), which
// has no Rust surface, and it is throwaway verification code rather than a new
// kernel subsystem. The POLICY this exercises is in Rust
// (rustkern/escrowmark.rs, rustkern/recycle.rs).

#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "process.h"

extern fat_fs_t g_fat_fs;
extern void kprintf(const char *fmt, ...);
extern void proc_sleep(uint32_t ms);
extern int escrow_spawn_marked(const char *path, const char *scope,
                               uint32_t ttl_ms, uint32_t uid);
extern int recycle_available(void);

static void escrvt_vmtest_worker(void *arg) {
    (void)arg;
    // Run AFTER the Stage 1 (7000ms) and Stage 2 (11000ms) workers so the
    // verdicts do not interleave on the serial line.
    proc_sleep(15000);
    uint32_t csz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/ESCRVT.TEST", &csz);
    if (!cfg) return;   // not flagged -> silent no-op (production path)
    kfree(cfg);

    kprintf("\n========== RECYCLING AI DELETE: UNCOOPERATIVE ACTOR ==========\n");
    if (!g_fat_fs.mounted) { kprintf("[ESCRVT-TEST] no fs\n"); return; }

    // The granted scope root must PRE-EXIST, for the reason escrow2_vmtest.c
    // documents at length: a MANDATORY actor is marked from its first
    // instruction and may only write INSIDE its scope, so it cannot create its
    // own scope root, and a create under a missing directory returns a bogus
    // success whose file is never visible. Creating it here makes the test
    // self-contained rather than dependent on who deployed the image.
    (void)fat_mkdir(&g_fat_fs, "/ESCRVTSC");

    // Say out loud whether the bin is usable BEFORE the actor runs. If it is
    // not, every delete in the run will be REFUSED (fail closed) and ESCRVT
    // will go red for a reason that is not an enforcement failure, so this line
    // is what tells the two apart.
    kprintf("[ESCRVT-TEST] recycle_available=%d (0 means every delete in this "
            "run will be REFUSED, which is fail-closed, not a bug)\n",
            recycle_available());

    // uid 0: the shipped image autologins root, so proving a uid-0 Ring-3 task
    // cannot destroy anything is the strong case.
    int pid = escrow_spawn_marked("/APPS/ESCRVT", "/ESCRVTSC", 3600000u, 0);
    kprintf("[ESCRVT-TEST] escrow_spawn_marked rc/pid=%d "
            "(>0 = spawned already-marked); watch for ESCRVT: and ESCRVTH: "
            "lines\n", pid);
    kprintf("========== UNCOOPERATIVE-ACTOR TEST ARMED ==========\n");
}

void escrvt_start_deferred_test(void) {
    proc_create_ex("escrvttest", escrvt_vmtest_worker, 0, PRIO_LOW, 256 * 1024);
}
