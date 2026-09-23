// proc/escu6_vmtest.c - #246/#305 Stage 6 GATED boot launcher for the escrow
// thin-client + generalised-chokepoint verifier /APPS/ESCU6.
//
// TEST SCAFFOLDING, not a shipping feature. It is a no-op unless
// /CONFIG/ESCU6.TEST exists on the root fs, exactly like the escrow
// (proc/escrow_vmtest.c) launcher it is modelled on, so it never runs in a
// production golden (the marker ships on no image; ESCU6 is on
// build/unshipped-apps.list). It is C, not Rust, for the same reason
// escrow_vmtest.c is: it is entangled with the C launch path (fat_read_file +
// elf_validate + proc_create_user_as), which has no Rust surface, and it is
// throwaway verification code, not a new kernel subsystem.
//
// WHY A LAUNCHER AND NOT A SHELL COMMAND. The golden boots straight to the GUI
// and QEMU pointer injection is unreliable (#334). Launching ESCU6 from a kernel
// worker with its stdio on /dev/console makes its verdict appear on the serial
// line a headless VM can capture, and ESCU6 also writes its verdict to
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

static void escu6_vmtest_worker(void *arg) {
    (void)arg;
    // Let the fs settle, the desktop come up, and (if present) a synthetic USB
    // stick enumerate and mount so the eject-chokepoint half has a real target.
    proc_sleep(9000);
    uint32_t csz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/ESCU6.TEST", &csz);
    if (!cfg) return;   // not flagged -> silent no-op (production path)
    kfree(cfg);

    kprintf("\n========== ESCROW STAGE 6 TEST (#246 thin client + generalised chokepoint) ==========\n");
    if (!g_fat_fs.mounted) { kprintf("[ESCU6-TEST] no fs\n"); return; }
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, "/APPS/ESCU6", &sz);
    if (!data || sz == 0) {
        if (data) kfree(data);
        kprintf("[ESCU6-TEST] /APPS/ESCU6 not found (build with unshipped apps)\n");
        return;
    }
    if (elf_validate(data, sz) != 0) {
        kfree(data);
        kprintf("[ESCU6-TEST] /APPS/ESCU6 bad ELF\n");
        return;
    }
    // uid 0: the shipped image autologins root. The point being proven is that a
    // uid-0 Ring-3 process, once it is a marked escrow actor, still cannot bypass
    // the kernel enforcement with raw syscalls, so launching as root is the
    // strong case.
    int pid = proc_create_user_as("/APPS/ESCU6", data, sz, 0, 0, proc_as_uid(0));
    kfree(data);
    kprintf("[ESCU6-TEST] launched /APPS/ESCU6 pid=%d; watch for ESCU6: lines\n", pid);
    kprintf("========== ESCROW STAGE 6 TEST ARMED ==========\n");
}

void escu6_start_deferred_test(void) {
    proc_create_ex("escu6test", escu6_vmtest_worker, 0, PRIO_LOW, 256 * 1024);
}
