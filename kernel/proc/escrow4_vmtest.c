// proc/escrow4_vmtest.c - #246/#305 GATED boot launcher for the STAGE 4
// (CAP_SCOPE_DEVICE) end-to-end verifier /APPS/ESCROW4T
// (docs/CONTRACT_ENFORCEMENT_PLAN.md, Stage 4).
//
// TEST SCAFFOLDING, not a shipping feature. It is a no-op unless
// /CONFIG/ESCROW4.TEST exists on the root fs, exactly like proc/escrow2_vmtest.c
// which it is modelled on, so it never runs in a production golden (ESCROW4T
// ships on no image; it is on build/unshipped-apps.list). It is C, not Rust,
// for the same reason escrow2_vmtest.c is: it is entangled with the C launch
// path (hotplug scan + fat I/O + escrow_spawn_marked), which has no Rust
// surface, and it is throwaway verification code.
//
// WHAT IT PROVES (the real, end-to-end half of Stage 4). It finds a REAL mounted
// removable volume (a synthetic QEMU usb-storage stick), device-scopes an escrow
// grant to it via escrow_spawn_marked (which binds the grant's OBJECT to that
// device's validated identity), and launches /APPS/ESCROW4T ALREADY MARKED +
// device-bound. ESCROW4T then proves an in-scope write on the CORRECT device is
// ALLOWED end to end. The deterministic device-swap / BadUSB / fail-closed
// verdicts are proven separately by the in-kernel escdev_selftest().

#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../drivers/hotplug.h"
#include "process.h"

extern fat_fs_t g_fat_fs;
extern void kprintf(const char *fmt, ...);
extern void proc_sleep(uint32_t ms);
extern int escrow_spawn_marked(const char *path, const char *scope,
                               uint32_t ttl_ms, uint32_t uid);

static void escrow4_vmtest_worker(void *arg) {
    (void)arg;
    // Run AFTER the Stage 1 (7s) and Stage 2 (11s) workers so the verdicts do
    // not interleave on the serial line, and after USB enumeration + mount.
    proc_sleep(16000);
    uint32_t csz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/ESCROW4.TEST", &csz);
    if (!cfg) return;   // not flagged -> silent no-op (production path)
    kfree(cfg);

    kprintf("\n========== ESCROW STAGE 4 (CAP_SCOPE_DEVICE) E2E TEST (#246) ==========\n");
    if (!g_fat_fs.mounted) { kprintf("[ESCROW4-TEST] no root fs\n"); return; }

    // Find a mounted, readable, removable volume (the synthetic stick), that is
    // NOT the root. Its mount_point becomes the device-scoped grant's scope.
    char scope[HOTPLUG_MOUNT_PATH_LEN + 1];
    scope[0] = '\0';
    for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
        hotplug_device_t *d = hotplug_get_device(i);
        if (!d || d->status != HOTPLUG_STATUS_MOUNTED) continue;
        if (!d->mount_point[0]) continue;
        if (!hotplug_fs_readable(d->fs_type)) continue;
        strncpy(scope, d->mount_point, sizeof(scope) - 1);
        scope[sizeof(scope) - 1] = '\0';
        kprintf("[ESCROW4-TEST] using removable volume %s (VID=%04x PID=%04x)\n",
                scope, d->vendor_id, d->product_id);
        break;
    }
    if (!scope[0]) {
        kprintf("[ESCROW4-TEST] NO removable volume mounted; cannot run the "
                "end-to-end device-scoped write (attach a synthetic usb-storage). "
                "The device-scope DECISION is still proven by escdev_selftest.\n");
        return;
    }

    // Publish the scope so ESCROW4T knows where its in-scope writes go (reads are
    // not gated, so a marked actor may read this).
    int wrc = fat_write_file(&g_fat_fs, "/CONFIG/ESCROW4.SCOPE", scope, (uint32_t)strlen(scope));
    if (wrc != 0)
        kprintf("[ESCROW4-TEST] warn: could not publish scope to /CONFIG/ESCROW4.SCOPE (rc=%d)\n", wrc);

    int pid = escrow_spawn_marked("/APPS/ESCROW4T", scope, 3600000u, 0);
    kprintf("[ESCROW4-TEST] escrow_spawn_marked rc/pid=%d scope=%s "
            "(>0 = spawned already-marked + device-bound); watch for ESCROW4T: lines\n",
            pid, scope);
    kprintf("========== ESCROW STAGE 4 E2E TEST ARMED ==========\n");
}

void escrow4_start_deferred_test(void) {
    proc_create_ex("escrow4test", escrow4_vmtest_worker, 0, PRIO_LOW, 256 * 1024);
}
