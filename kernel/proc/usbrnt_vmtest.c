// proc/usbrnt_vmtest.c - [renamevol] #246 GATED boot launcher for the ORDINARY
// (unmarked) removable-volume rename verifier /APPS/USBRNT.
//
// TEST SCAFFOLDING, not a shipping feature. It is a no-op unless
// /CONFIG/USBRNT.TEST exists on the root fs, exactly like proc/escrow_vmtest.c
// and proc/escrow4_vmtest.c which it is modelled on, so it never runs in a
// production golden (/APPS/USBRNT is on build/unshipped-apps.list and the
// marker ships on no image). It is C, not Rust, for the same reason those two
// are: it is entangled with the C launch path (fat_read_file + elf_validate +
// proc_create_user_as), the C hotplug table and the C permission table, none of
// which has a Rust surface, and it is throwaway verification code rather than a
// new kernel subsystem. The DECISION this work adds is in Rust
// (rustkern/renamevol.rs).
//
// WHY IT LAUNCHES THE PROVER **UNMARKED**, AND WHY THAT IS THE WHOLE POINT
// ==========================================================================
// ESCROW4T is spawned already marked as an AI escrow actor, so the escrow guard
// intercepts its destructive syscalls before they reach the filesystem. A
// marked prover therefore cannot distinguish "the escrow layer refused this"
// from "the filesystem could not find the path", because both are -1. The
// question being measured here is the ORDINARY user's: can a normal Ring-3
// process rename, mkdir, rmdir or unlink on a mounted USB stick at all. So this
// uses proc_create_user_as(), the same ordinary launch escrow_vmtest.c uses,
// and nothing marks the result.
//
// WHAT THIS LAUNCHER PREPARES, AND WHY RING 3 IS NOT TRUSTED TO DO IT
// ==========================================================================
// The prover's last arm is a SECURITY arm: as uid 1000, in a sticky directory
// owned by uid 1001, renaming over another user's file must be refused and
// renaming over its own file must be allowed. The facts that gate is judged on
// (who owns the directory, who owns each victim, and that the directory carries
// S_ISVTX) live in /CONFIG/PERMS.DB, which Ring 3 cannot write. They are
// written HERE, as kernel code, so the setup cannot itself be a Ring-3
// capability the arm is implicitly also testing.
//
// AND IT PRINTS THE BLINDNESS, ONCE, AS A MEASUREMENT
// ==========================================================================
// The defect under test is that the destination-existence question in
// sys_rename() was asked of `g_fat_fs`, which does not route a hot-plugged
// volume. That is not visible from Ring 3: a skipped permission check and a
// passed one both end in the same -1 while the rename itself is broken. So this
// asks the SAME live path BOTH WAYS and prints both answers:
//
//   fat_exists(&g_fat_fs, p)   - what the gate used to ask
//   recycle_victim_stat(p)     - the volume-aware oracle the capture path uses
//
// 0 against 1 on a file that demonstrably exists is the defect, stated as a
// number rather than as a reading of the source. This line is in BOTH arms of
// the kernel differential, so it is a constant and not the variable.

#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/perms.h"
#include "../fs/recycle_fs.h"
#include "../drivers/hotplug.h"
#include "process.h"

extern fat_fs_t g_fat_fs;
extern void kprintf(const char *fmt, ...);
extern void proc_sleep(uint32_t ms);
extern int  elf_validate(const void *data, uint32_t size);

// The identities the security arm is judged on. 1000 is the prover after it
// drops privilege; 1001 is "another user". They match the UID_ME / UID_OTHER
// constants in userland/apps/USBRNT/main.c; a mismatch would make the arm pass
// for the wrong reason, so both sides name them explicitly.
#define USBRNT_UID_ME     1000u
#define USBRNT_UID_OTHER  1001u
// S_ISVTX | rwxrwxrwx. 0o1000 is PERM_STICKY in rustkern/sticky.rs, which is
// what sticky_is_set_rs() reads; this is the same bit, written in C octal.
#define USBRNT_STICKY_DIR 01777

static void usbrnt_vmtest_worker(void *arg) {
    (void)arg;
    // AFTER the four escrow workers (7s / 9s / 11s / 16s) so the verdicts do
    // not interleave on the serial line, and comfortably after USB enumeration
    // and the hotplug mount.
    proc_sleep(26000);

    uint32_t csz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/USBRNT.TEST", &csz);
    if (!cfg) return;   // not flagged -> silent no-op (production path)
    kfree(cfg);

    kprintf("\n========== USB RENAME / PERMISSION TEST (#246 renamevol) ==========\n");
    if (!g_fat_fs.mounted) { kprintf("[USBRNT-TEST] no root fs\n"); return; }

    // Find a mounted, readable, removable volume (the synthetic stick).
    char mnt[HOTPLUG_MOUNT_PATH_LEN + 1];
    mnt[0] = '\0';
    for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
        hotplug_device_t *d = hotplug_get_device(i);
        if (!d || d->status != HOTPLUG_STATUS_MOUNTED) continue;
        if (!d->mount_point[0]) continue;
        if (!hotplug_fs_readable(d->fs_type)) continue;
        strncpy(mnt, d->mount_point, sizeof(mnt) - 1);
        mnt[sizeof(mnt) - 1] = '\0';
        kprintf("[USBRNT-TEST] using removable volume %s (VID=%04x PID=%04x)\n",
                mnt, d->vendor_id, d->product_id);
        break;
    }
    if (!mnt[0]) {
        kprintf("[USBRNT-TEST] NO removable volume mounted; the ordinary-user "
                "rename arms cannot run (attach a synthetic usb-storage)\n");
        return;
    }

    // ---- the sticky-directory facts the security arm is judged on.
    char dsh[160], dvic[192], down[192];
    snprintf(dsh,  sizeof(dsh),  "%s/SH", mnt);
    snprintf(dvic, sizeof(dvic), "%s/SH/VIC.TXT", mnt);
    snprintf(down, sizeof(down), "%s/SH/OWN.TXT", mnt);

    // The volume root itself: root-owned 0755, so an unprivileged uid can
    // TRAVERSE into /SH (#674 traversal) without being able to write the
    // volume root. Set explicitly rather than left to a default, because the
    // arm's meaning depends on it.
    perms_set(mnt,  0, 0, 0755);
    perms_set(dsh,  USBRNT_UID_OTHER, USBRNT_UID_OTHER, USBRNT_STICKY_DIR);
    perms_set(dvic, USBRNT_UID_OTHER, USBRNT_UID_OTHER, 0644);
    perms_set(down, USBRNT_UID_ME,    USBRNT_UID_ME,    0644);
    {
        int src = perms_sync();
        kprintf("[USBRNT-TEST] perms rows written: %s=0:0:0755  %s=%u:%u:%04o(sticky)"
                "  VIC.TXT=%u  OWN.TXT=%u  perms_sync=%d\n",
                mnt, dsh, USBRNT_UID_OTHER, USBRNT_UID_OTHER,
                (unsigned)USBRNT_STICKY_DIR, USBRNT_UID_OTHER, USBRNT_UID_ME, src);
    }

    // ---- THE BLINDNESS, MEASURED. Same live path, both oracles, one line.
    {
        int fe = fat_exists(&g_fat_fs, dvic);
        uint32_t len = 0; int isdir = 0;
        int vs = recycle_victim_stat(dvic, &len, &isdir);
        kprintf("[USBRNT-TEST] oracle-compare path=%s fat_exists(g_fat_fs)=%d "
                "recycle_victim_stat=%d len=%u isdir=%d "
                "(0 vs 1 means the gate asked a filesystem that cannot see this file)\n",
                dvic, fe, vs, (unsigned)len, isdir);
    }

    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, "/APPS/USBRNT", &sz);
    if (!data || sz == 0) {
        if (data) kfree(data);
        kprintf("[USBRNT-TEST] /APPS/USBRNT not found (build with unshipped apps)\n");
        return;
    }
    if (elf_validate(data, sz) != 0) {
        kfree(data);
        kprintf("[USBRNT-TEST] /APPS/USBRNT bad ELF\n");
        return;
    }
    // uid 0: the prover does its own one-way drop to uid 1000 for the security
    // arm. Starting as root is what lets it plant the unprivileged-side source
    // files and then become the unprivileged caller in one process.
    int pid = proc_create_user_as("/APPS/USBRNT", data, sz, 0, 0, proc_as_uid(0));
    kfree(data);
    kprintf("[USBRNT-TEST] launched /APPS/USBRNT pid=%d (UNMARKED: no escrow "
            "guard in the way); watch for USBRNT: lines\n", pid);
    kprintf("========== USB RENAME / PERMISSION TEST ARMED ==========\n");
}

void usbrnt_start_deferred_test(void) {
    proc_create_ex("usbrnttest", usbrnt_vmtest_worker, 0, PRIO_LOW, 256 * 1024);
}
