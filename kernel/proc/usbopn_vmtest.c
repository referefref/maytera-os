// proc/usbopn_vmtest.c - [openperm] #246 GATED boot launcher for /APPS/USBOPN,
// the verifier that asks whether the file-OPEN path and the DELETE path agree
// about permissions on a mounted removable volume.
//
// TEST SCAFFOLDING, not a shipping feature. A no-op unless
// /CONFIG/USBOPN.TEST exists on the root filesystem, exactly like
// proc/escrow_vmtest.c and proc/usbrnt_vmtest.c which it is modelled on.
// /APPS/USBOPN is on build/unshipped-apps.list and the marker ships on no
// image, so neither is present on a production golden.
//
// It is C and not Rust, for the same reason the four escrow launchers and
// usbrnt_vmtest.c are: it is entangled with the C launch path (fat_read_file +
// elf_validate + proc_create_user_as), the C hotplug table and the C
// permission table, none of which has a Rust surface, and it is throwaway
// verification code rather than a new kernel subsystem. The POLICY this work
// adds is in Rust (rustkern/volperm.rs).
//
// WHY IT LAUNCHES THE PROVER UNMARKED
// ==========================================================================
// The four escrow provers are spawned already marked as AI escrow actors, so
// the escrow guard intercepts their destructive syscalls before the filesystem
// sees them, and a -1 from the guard cannot be told apart from a -1 from a
// permission check. The question here is the ORDINARY user's. So this uses
// proc_create_user_as(), and nothing marks the result.
//
// WHAT THIS LAUNCHER BUILDS, AND WHY RING 3 IS NOT TRUSTED TO BUILD IT
// ==========================================================================
// Two fixture directories, identical in every respect except the medium:
//
//   /PMR            on the ROOT filesystem   - the CONTROL
//   <mount>/PM      on the removable volume  - the SUBJECT
//
// Each is mode 01777 (all users, plus S_ISVTX) owned by uid 1001, and each
// holds the same ten 64-byte files: five owned by uid 1001 ("another user")
// and five owned by uid 1000 (the prover after its privilege drop). Who owns
// what, and that the directory is sticky, are facts in /CONFIG/PERMS.DB, which
// Ring 3 cannot write. They are written HERE, as kernel code, so the setup is
// not itself a Ring-3 capability the arms would be implicitly testing.
//
// Both fixtures are built by the SAME two helpers below, which route through
// hotplug_fs_or_root() (drivers/hotplug.h), the ONE root-or-volume resolver.
// Using one code path for both media is the same discipline the prover applies
// in Ring 3: if the control is green and the subject is red, the only
// difference is the medium.
//
// THE ONE ROW THIS LAUNCHER DELIBERATELY DOES NOT WRITE, AND WHY IT MATTERS
// ==========================================================================
// It does NOT call perms_set() on the MOUNT POINT itself.
//
// proc/usbrnt_vmtest.c does (`perms_set(mnt, 0, 0, 0755)`), and for its
// purposes that is right. Here it would destroy the measurement. The prover's
// V8 arm asks whether an ordinary user can create a file in the VOLUME ROOT,
// which is the carve-out's stated reason for existing: perms_check()'s
// no-entry default is root-owned 0755, which grants o+x (so traversal into
// /PM works) and denies o+w (so a create in the volume root does not). If this
// launcher wrote that row by hand, V8 would be measuring the launcher instead
// of the mount path, and a fix that failed to give a mounted volume a usable
// ownership answer would look like it worked.
//
// CONSEQUENCE, STATED SO THE NEXT PERSON DOES NOT TRIP ON IT: USBOPN AND
// USBRNT MUST NOT BE RUN IN THE SAME BOOT. The mount-point row is one global
// fact and usbrnt_vmtest.c overwrites it with 0755. Both suites are optional
// in tools/escrow-vmtest/run-escrow-vmtests.sh and neither is on by default, so
// this is a matter of not passing both --suite flags at once.
//
// AND IT PRINTS THE TWO BLIND SPOTS IN THE GATE, AS NUMBERS
// ==========================================================================
// Neither defect is visible from Ring 3, because a skipped check and a passed
// check both end in a descriptor. So the launcher asks the live kernel
// directly and prints the answers:
//
//   1. perms_check(<victim on the volume>, 1000, 1000, W_OK) - what the
//      permission TABLE says about an unprivileged write to another user's
//      file on the stick. A -1 here, next to a successful open from Ring 3,
//      is the carve-out measured rather than read.
//
//   2. fat_exists(&g_fat_fs, <victim on the volume>) - the O_CREAT
//      existence probe in sys_open_k() is a g_fat_fs call, and g_fat_fs
//      cannot see a hot-plugged volume. It reads ABSENT for a file that is
//      plainly there. That is inert while the carve-out returns above it and
//      becomes live the moment the carve-out is removed, which is the
//      [renamevol] lesson repeated one gate over: do not wire half of this.
//      It should also make fs/volblind.c fire.
//
// Both lines are printed in BOTH arms of the kernel differential, so they are
// a constant and not the variable.

#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/perms.h"
#include "../drivers/hotplug.h"
#include "process.h"

extern fat_fs_t g_fat_fs;
extern void kprintf(const char *fmt, ...);
extern void proc_sleep(uint32_t ms);
extern int  elf_validate(const void *data, uint32_t size);

// Must match UID_ME / UID_OTHER in userland/apps/USBOPN/main.c. A mismatch
// would make every deny arm pass for the wrong reason, so both sides name
// them explicitly rather than sharing a header neither owns.
#define USBOPN_UID_ME     1000u
#define USBOPN_UID_OTHER  1001u

// S_ISVTX | rwxrwxrwx, written in C octal. 0o1000 is PERM_STICKY in
// rustkern/sticky.rs, which is the bit sticky_is_set_rs() reads.
#define USBOPN_STICKY_DIR 01777

#define USBOPN_PRELEN 64

// The ten fixture files and who owns each. Five belong to "another user" and
// five to the prover, and they are interleaved by VERB rather than grouped by
// owner, because each verb needs its own victim: if a deny arm wrongly
// SUCCEEDS it destroys its target, and a later arm sharing that target would
// then be measuring an already-blanked file.
static const struct { const char *name; uint32_t uid; uint16_t mode; } usbopn_fix[] = {
    { "OVIC1.TXT",  USBOPN_UID_OTHER, 0644 },  // T1 deny : O_RDONLY|O_TRUNC
    { "OMINE1.TXT", USBOPN_UID_ME,    0644 },  // T2 allow: O_RDONLY|O_TRUNC
    { "OVIC2.TXT",  USBOPN_UID_OTHER, 0644 },  // T3 deny : O_WRONLY|O_TRUNC
    { "OMINE2.TXT", USBOPN_UID_ME,    0644 },  // T4 allow: O_WRONLY|O_TRUNC
    { "UVIC.TXT",   USBOPN_UID_OTHER, 0644 },  // U1 deny : unlink
    { "UMINE.TXT",  USBOPN_UID_ME,    0644 },  // U2 allow: unlink
    { "WVIC.TXT",   USBOPN_UID_OTHER, 0644 },  // W1 deny : O_WRONLY, no trunc
    { "WMINE.TXT",  USBOPN_UID_ME,    0644 },  // W2 allow: O_WRONLY, no trunc
    // The only two files that are NOT world-readable. Every other fixture file
    // is 0644, so no read arm above can detect a skipped R_OK; these two make
    // the confidentiality half of the same carve-out measurable.
    { "SVIC.TXT",   USBOPN_UID_OTHER, 0600 },  // S1 deny : read
    { "SMINE.TXT",  USBOPN_UID_ME,    0600 },  // S2 allow: read
};
#define USBOPN_NFIX ((int)(sizeof(usbopn_fix) / sizeof(usbopn_fix[0])))

// ---------------------------------------------------------------------------
// The two fixture builders. Both route through hotplug_fs_or_root(), so ONE
// code path serves the root filesystem and the removable volume, and the
// fat_fs_t handle is never chosen by hand. A hand-rolled "is this path on a
// volume" prefix match here is exactly the fault class fs/volblind.c was
// written to detect.
// ---------------------------------------------------------------------------
static int usbopn_mkdir_any(const char *abspath) {
    const char *rel = abspath;
    fat_fs_t *fs = hotplug_fs_or_root(abspath, &rel);
    if (!fs) return -1;
    return fat_mkdir(fs, rel);
}

static int usbopn_write_any(const char *abspath, const void *data, uint32_t n) {
    const char *rel = abspath;
    fat_fs_t *fs = hotplug_fs_or_root(abspath, &rel);
    if (!fs) return -1;
    // fat_write_file is MUST_CHECK and returns 0 on success.
    return fat_write_file(fs, rel, data, n);
}

// Build one fixture directory and its ten files, and write the ownership rows.
// Returns the number of files that did not read back with the right length.
//
// The ROWS ARE KEYED BY THE ABSOLUTE PATH, never by the volume-relative one.
// /CONFIG/PERMS.DB is keyed by the path userland names, so a row filed under
// "/PM/OVIC1.TXT" would describe a directory in the root of the boot volume
// instead. [renamevol] records the same trap in sys_mkdir.
static int usbopn_build_fixture(const char *dir, const char *pre) {
    int bad = 0;
    char p[192];

    // An existing directory is not an error: a re-run on the same stick image
    // should rebuild rather than refuse. fat_mkdir's return is therefore not
    // checked here; the per-file read-back below is what decides whether the
    // fixture is usable, and that is the claim the prover's FIXTURE arm
    // re-checks from Ring 3.
    (void)usbopn_mkdir_any(dir);
    perms_set(dir, USBOPN_UID_OTHER, USBOPN_UID_OTHER, USBOPN_STICKY_DIR);

    for (int i = 0; i < USBOPN_NFIX; i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, usbopn_fix[i].name);
        int w = usbopn_write_any(p, pre, (uint32_t)USBOPN_PRELEN);
        // READ IT BACK. A 0-byte victim would make a deny arm pass for the
        // wrong reason: there would be nothing left to destroy.
        uint32_t got = 0;
        void *rb = 0;
        {
            const char *rel = p;
            fat_fs_t *fs = hotplug_fs_or_root(p, &rel);
            if (fs) rb = fat_read_file(fs, rel, &got);
        }
        int ok = (w == 0) && rb && (got == (uint32_t)USBOPN_PRELEN) &&
                 (memcmp(rb, pre, USBOPN_PRELEN) == 0);
        if (rb) kfree(rb);
        if (!ok) {
            bad++;
            kprintf("[USBOPN-TEST] FIXTURE BAD %s write-rc=%d read-back=%u bytes\n",
                    p, w, (unsigned)got);
        }
        perms_set(p, usbopn_fix[i].uid, usbopn_fix[i].uid, usbopn_fix[i].mode);
    }
    return bad;
}

static void usbopn_vmtest_worker(void *arg) {
    (void)arg;
    // AFTER the four escrow workers (7s/9s/11s/16s) and after usbrnt's 26s, so
    // the verdicts do not interleave on the serial line, and comfortably after
    // USB enumeration and the hotplug mount.
    proc_sleep(31000);

    uint32_t csz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/USBOPN.TEST", &csz);
    if (!cfg) return;   // not flagged -> silent no-op (the production path)
    kfree(cfg);

    kprintf("\n========== USB OPEN-vs-DELETE PERMISSION TEST (#246 openperm) ==========\n");
    if (!g_fat_fs.mounted) { kprintf("[USBOPN-TEST] no root fs\n"); return; }

    // ---- find a mounted, readable, removable volume (the synthetic stick).
    char mnt[HOTPLUG_MOUNT_PATH_LEN + 1];
    mnt[0] = '\0';
    for (int i = 0; i < HOTPLUG_MAX_DEVICES; i++) {
        hotplug_device_t *d = hotplug_get_device(i);
        if (!d || d->status != HOTPLUG_STATUS_MOUNTED) continue;
        if (!d->mount_point[0]) continue;
        if (!hotplug_fs_readable(d->fs_type)) continue;
        strncpy(mnt, d->mount_point, sizeof(mnt) - 1);
        mnt[sizeof(mnt) - 1] = '\0';
        kprintf("[USBOPN-TEST] using removable volume %s (VID=%04x PID=%04x)\n",
                mnt, d->vendor_id, d->product_id);
        break;
    }
    if (!mnt[0]) {
        kprintf("[USBOPN-TEST] NO removable volume mounted; the open-vs-delete "
                "arms cannot run (attach a synthetic usb-storage)\n");
        return;
    }

    // ---- the 64-byte known pre-image, built here so the kernel side and the
    // Ring-3 side cannot disagree about what "intact" means.
    char pre[USBOPN_PRELEN];
    for (int i = 0; i < USBOPN_PRELEN; i++) pre[i] = (char)('A' + (i % 26));

    char vdir[HOTPLUG_MOUNT_PATH_LEN + 8];
    snprintf(vdir, sizeof(vdir), "%s/PM", mnt);

    int badr = usbopn_build_fixture("/PMR", pre);
    int badv = usbopn_build_fixture(vdir,   pre);
    {
        int src = perms_sync();
        kprintf("[USBOPN-TEST] fixtures built: /PMR bad=%d  %s bad=%d  "
                "(dirs %04o owned %u, five victims owned %u, five own files owned %u) "
                "perms_sync=%d\n",
                badr, vdir, badv, (unsigned)USBOPN_STICKY_DIR,
                USBOPN_UID_OTHER, USBOPN_UID_OTHER, USBOPN_UID_ME, src);
    }

    // ---- THE MOUNT POINT'S OWN ROW, READ AND NOT WRITTEN. This is what the
    // prover's V8 usability arm is really measuring, so print what is actually
    // there. No row at all means perms_check() falls to its no-entry default
    // (root-owned 0755): o+x so traversal into /PM works, no o+w so an
    // unprivileged create in the volume root does not.
    {
        uint32_t muid = 0, mgid = 0; uint16_t mmode = 0;
        int has = (perms_get(mnt, &muid, &mgid, &mmode) == 0);
        kprintf("[USBOPN-TEST] mount-point row %s: %s uid=%u gid=%u mode=%04o "
                "(NOT written by this launcher on purpose; V8 measures whatever "
                "the mount path leaves here)\n",
                mnt, has ? "PRESENT" : "ABSENT (no-entry default root:root 0755)",
                (unsigned)muid, (unsigned)mgid, (unsigned)mmode);
    }

    // ---- BLIND SPOT 1: what the permission TABLE says about the write the
    // carve-out is about to permit anyway.
    {
        char vic[192];
        snprintf(vic, sizeof(vic), "%s/OVIC1.TXT", vdir);
        int w = perms_check(vic, USBOPN_UID_ME, USBOPN_UID_ME, W_OK);
        int r = perms_check(vic, USBOPN_UID_ME, USBOPN_UID_ME, R_OK);
        kprintf("[USBOPN-TEST] table-says path=%s uid=%u perms_check(W_OK)=%d "
                "perms_check(R_OK)=%d (W_OK=-1 next to a SUCCESSFUL Ring-3 "
                "O_TRUNC open is the #250 carve-out, measured)\n",
                vic, USBOPN_UID_ME, w, r);
    }

    // ---- BLIND SPOT 2: the O_CREAT existence probe inside the same gate is a
    // g_fat_fs call, and g_fat_fs cannot see a hot-plugged volume.
    {
        char vic[192];
        snprintf(vic, sizeof(vic), "%s/OVIC1.TXT", vdir);
        int fe = fat_exists(&g_fat_fs, vic);
        const char *rel = vic;
        fat_fs_t *vfs = hotplug_fs_or_root(vic, &rel);
        int ve = vfs ? fat_exists(vfs, rel) : -1;
        kprintf("[USBOPN-TEST] oracle-compare path=%s fat_exists(g_fat_fs)=%d "
                "fat_exists(hotplug_fs_or_root)=%d (0 vs 1 means sys_open_k's "
                "O_CREAT probe asks a filesystem that cannot see this file)\n",
                vic, fe, ve);
    }

    // ---- launch the prover as uid 0; it does its own one-way drop to uid
    // 1000 after it has verified the fixture, which root can do and uid 1000
    // deliberately cannot (the two 0600 files).
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, "/APPS/USBOPN", &sz);
    if (!data || sz == 0) {
        if (data) kfree(data);
        kprintf("[USBOPN-TEST] /APPS/USBOPN not found (build with unshipped apps)\n");
        return;
    }
    if (elf_validate(data, sz) != 0) {
        kfree(data);
        kprintf("[USBOPN-TEST] /APPS/USBOPN bad ELF\n");
        return;
    }
    int pid = proc_create_user_as("/APPS/USBOPN", data, sz, 0, 0, proc_as_uid(0));
    kfree(data);
    kprintf("[USBOPN-TEST] launched /APPS/USBOPN pid=%d (UNMARKED: no escrow "
            "guard in the way); watch for USBOPN: lines\n", pid);
    kprintf("========== USB OPEN-vs-DELETE PERMISSION TEST ARMED ==========\n");
}

void usbopn_start_deferred_test(void) {
    proc_create_ex("usbopntest", usbopn_vmtest_worker, 0, PRIO_LOW, 256 * 1024);
}
