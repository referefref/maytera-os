// proc/fdguard_test.c - #fdguard: GATED boot launcher for the two cross-process
// privilege-boundary exploit apps (/APPS/FDXTEST and /APPS/PTSXTEST).
//
// This is TEST SCAFFOLDING, not a shipping feature. It is a no-op unless
// /CONFIG/FDGUARD.TEST exists on the root fs, exactly like the #265 CRONTEST
// gated self-test it is modelled on, so it never runs in a production golden
// (the marker ships on no image). It is C, not Rust, for the same reason
// cron_launch_program() is: it is entangled with the C launch path
// (fat_read_file + elf_validate + proc_create_user_as), which has no Rust
// surface, and it is throwaway verification code, not a new kernel subsystem.
//
// WHY A LAUNCHER AND NOT A SHELL COMMAND. The golden boots straight to the GUI;
// the kernel serial shell is reachable only with no framebuffer, and QEMU
// pointer injection into the GUI terminal is unreliable (#334). Launching the
// apps from a kernel worker with their stdio on /dev/console makes their
// verdict appear on the serial line a headless VM can capture, and each app
// also writes its verdict to /BOOTLOG.TXT via sys_bootlog for a durable record.

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

static void launch_one(const char *path) {
    if (!g_fat_fs.mounted) { kprintf("[FDGUARD-TEST] no fs for %s\n", path); return; }
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, path, &sz);
    if (!data || sz == 0) {
        if (data) kfree(data);
        kprintf("[FDGUARD-TEST] %s not found (build with unshipped apps)\n", path);
        return;
    }
    if (elf_validate(data, sz) != 0) {
        kfree(data);
        kprintf("[FDGUARD-TEST] %s bad ELF\n", path);
        return;
    }
    // uid 0: the shipped image autologins root, and the exploit must be able to
    // open the marker file it creates; the point being proven is process
    // ISOLATION, which is orthogonal to uid (the old hole leaked between
    // processes at ANY privilege).
    int pid = proc_create_user_as(path, data, sz, 0, 0, proc_as_uid(0));
    kfree(data);
    kprintf("[FDGUARD-TEST] launched %s pid=%d\n", path, pid);
}

static void fdguard_test_worker(void *arg) {
    (void)arg;
    proc_sleep(6000);   // let the fs settle and the desktop come up
    uint32_t csz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/FDGUARD.TEST", &csz);
    if (!cfg) return;   // not flagged -> silent no-op (production path)
    kfree(cfg);

    // Dev-only: arm the enforcement bypass if its marker is present, so the
    // SAME image can show the exploit RED (bypass on) and GREEN (bypass off).
    { uint32_t bsz = 0;
      char *b = (char *)fat_read_file(&g_fat_fs, "/CONFIG/FDGUARD.BYPASS", &bsz);
      extern void fdguard_set_bypass(int on);
      if (b) { kfree(b); fdguard_set_bypass(1); } }

    kprintf("\n========== FDGUARD EXPLOIT TEST (#fdguard) ==========\n");
    kprintf("[FDGUARD-TEST] marker present; launching exploit apps\n");
    launch_one("/APPS/FDXTEST");
    proc_sleep(4000);   // stagger so the two verdicts do not interleave
    launch_one("/APPS/PTSXTEST");
    kprintf("[FDGUARD-TEST] launched; watch for FDXTEST:/PTSXTEST: verdict lines\n");
    kprintf("========== FDGUARD EXPLOIT TEST ARMED ==========\n");
}

void fdguard_start_deferred_test(void) {
    proc_create_ex("fdgtest", fdguard_test_worker, 0, PRIO_LOW, 256 * 1024);
}
