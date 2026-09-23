// proc/aieject_vmtest.c - #708 GATED boot launcher for the AI safe-eject
// verifier /APPS/AIEJTEST.
//
// TEST SCAFFOLDING, not a shipping feature. It is a no-op unless
// /CONFIG/AIEJECT.TEST exists on the root fs, exactly like the escrow
// (proc/escrow_vmtest.c) and #fdguard (proc/fdguard_test.c) launchers it is
// modelled on, so it never runs in a production golden (the marker ships on no
// image; AIEJTEST is on build/unshipped-apps.list). It is C, not Rust, for the
// same reason escrow_vmtest.c is: it is entangled with the C launch path
// (fat_read_file + elf_validate + proc_create_user_as), which has no Rust
// surface, and it is throwaway verification code, not a new kernel subsystem.
//
// WHY A LAUNCHER AND NOT A SHELL COMMAND. The golden boots straight to the GUI
// and QEMU pointer injection is unreliable (#334). Launching AIEJTEST from a
// kernel worker with its stdio on /dev/console makes its verdict appear on the
// serial line a headless VM can capture, and AIEJTEST also writes its verdict to
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

static void aieject_vmtest_worker(void *arg) {
    (void)arg;
    // Let the fs settle, the desktop come up, and the synthetic USB stick
    // enumerate through USB-MSC and get mounted by hotplug before we probe it.
    proc_sleep(9000);
    uint32_t csz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/AIEJECT.TEST", &csz);
    if (!cfg) return;   // not flagged -> silent no-op (production path)
    kfree(cfg);

    kprintf("\n========== AI SAFE-EJECT DEVICE-EXECUTOR TEST (#708) ==========\n");
    if (!g_fat_fs.mounted) { kprintf("[AIEJ-TEST] no fs\n"); return; }
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, "/APPS/AIEJTEST", &sz);
    if (!data || sz == 0) {
        if (data) kfree(data);
        kprintf("[AIEJ-TEST] /APPS/AIEJTEST not found (build with unshipped apps)\n");
        return;
    }
    if (elf_validate(data, sz) != 0) {
        kfree(data);
        kprintf("[AIEJ-TEST] /APPS/AIEJTEST bad ELF\n");
        return;
    }
    // uid 0: the shipped image autologins root, and the point being proven is
    // that even a uid-0 Ring-3 caller is held to the manifest + busy + removable
    // guards, so launching as root is the strong case.
    int pid = proc_create_user_as("/APPS/AIEJTEST", data, sz, 0, 0, proc_as_uid(0));
    kfree(data);
    kprintf("[AIEJ-TEST] launched /APPS/AIEJTEST pid=%d; watch for AIEJT: lines\n", pid);
    kprintf("========== AI SAFE-EJECT DEVICE-EXECUTOR TEST ARMED ==========\n");
}

void aieject_start_deferred_test(void) {
    proc_create_ex("aiejtest", aieject_vmtest_worker, 0, PRIO_LOW, 256 * 1024);
}
