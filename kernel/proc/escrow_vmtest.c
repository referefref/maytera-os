// proc/escrow_vmtest.c - #246/#305 GATED boot launcher for the kernel-escrow
// verifier /APPS/ESCROWT (docs/CONTRACT_ENFORCEMENT_PLAN.md, Stage 1).
//
// TEST SCAFFOLDING, not a shipping feature. It is a no-op unless
// /CONFIG/ESCROW.TEST exists on the root fs, exactly like the #fdguard
// (proc/fdguard_test.c) launcher it is modelled on, so it never runs in a
// production golden (the marker ships on no image; ESCROWT is on
// build/unshipped-apps.list). It is C, not Rust, for the same reason
// fdguard_test.c is: it is entangled with the C launch path (fat_read_file +
// elf_validate + proc_create_user_as), which has no Rust surface, and it is
// throwaway verification code, not a new kernel subsystem.
//
// WHY A LAUNCHER AND NOT A SHELL COMMAND. The golden boots straight to the GUI
// and QEMU pointer injection is unreliable (#334). Launching ESCROWT from a
// kernel worker with its stdio on /dev/console makes its verdict appear on the
// serial line a headless VM can capture, and ESCROWT also writes its verdict to
// /BOOTLOG.TXT via sys_bootlog for a durable record.

#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "process.h"

extern fat_fs_t g_fat_fs;
extern void kprintf(const char *fmt, ...);
extern void proc_sleep(uint32_t ms);
extern int  elf_validate(const void *data, uint32_t size);

static void escrow_vmtest_worker(void *arg) {
    (void)arg;
    proc_sleep(7000);   // let the fs settle and the desktop come up
    uint32_t csz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/ESCROW.TEST", &csz);
    if (!cfg) return;   // not flagged -> silent no-op (production path)
    kfree(cfg);

    kprintf("\n========== ESCROW KERNEL-ENFORCEMENT TEST (#246) ==========\n");
    if (!g_fat_fs.mounted) { kprintf("[ESCROW-TEST] no fs\n"); return; }
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, "/APPS/ESCROWT", &sz);
    if (!data || sz == 0) {
        if (data) kfree(data);
        kprintf("[ESCROW-TEST] /APPS/ESCROWT not found (build with unshipped apps)\n");
        return;
    }
    if (elf_validate(data, sz) != 0) {
        kfree(data);
        kprintf("[ESCROW-TEST] /APPS/ESCROWT bad ELF\n");
        return;
    }
    // uid 0: the shipped image autologins root. The point being proven is that
    // a uid-0 Ring-3 process under an escrow contract still cannot bypass the
    // grant with raw syscalls, so launching as root is the strong case.
    int pid = proc_create_user_as("/APPS/ESCROWT", data, sz, 0, 0, proc_as_uid(0));
    kfree(data);
    kprintf("[ESCROW-TEST] launched /APPS/ESCROWT pid=%d; watch for ESCROWT: lines\n", pid);
    kprintf("========== ESCROW KERNEL-ENFORCEMENT TEST ARMED ==========\n");
}

void escrow_start_deferred_test(void) {
    proc_create_ex("escrowtest", escrow_vmtest_worker, 0, PRIO_LOW, 256 * 1024);
}
