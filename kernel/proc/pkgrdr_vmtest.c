// proc/pkgrdr_vmtest.c - [provetwo] #246 GATED boot launcher for /APPS/PKGRDR,
// the prover for the TWO [volaudit] permission fixes that shipped UNPROVEN in
// golden 2575: SYS_PKG_WRITE's authorization gate (pkg_write_permit) and shell
// output redirection's authorization gate (open_redir_file).
//
// TEST SCAFFOLDING, not a shipping feature. A no-op unless
// /CONFIG/PKGRDR.TEST exists on the root filesystem, exactly like
// proc/usbopn_vmtest.c and proc/usbrnt_vmtest.c which it is modelled on.
// /APPS/PKGRDR and /APPS/PKGRDRH are on build/unshipped-apps.list and the
// marker ships on no image, so none of the three is on a production golden.
//
// It is C and not Rust for the same reason usbopn_vmtest.c is: it is entangled
// with the C launch path (fat_read_file + elf_validate + proc_create_user_as),
// the C hotplug table and the C permission table, none of which has a Rust
// surface, and it is throwaway verification code rather than a new kernel
// subsystem.
//
// ==========================================================================
// WHAT WAS UNPROVEN, AND WHY THE TWO PREVIOUS AGENTS COULD NOT PROVE IT
// ==========================================================================
// 84c9a92c fixed both gates the same way. Each decides "is this a CREATE or an
// OVERWRITE?" and each used to ask fat_exists(&g_fat_fs, path), which cannot
// see a hot-plugged volume. On a stick the answer was always ABSENT, so both
// took the CREATE arm and authorized against write+search on the PARENT
// DIRECTORY instead of against write on the FILE. The fix asks
// recycle_victim_stat(), the one volume-aware existence oracle.
//
// Its own commit message says: "NOT proven: the two security fixes by a
// multi-user prover on a volume". Two stated reasons, and this file answers
// both:
//
//   1. "SYS_PKG_WRITE sits behind a separate path authorization, so a test
//      showing REFUSED could be refused by THAT gate." It sits behind THREE
//      other gates, in this order (proc/syscall.c SYS_PKG_WRITE case):
//        escrow_fs_guard()      - the AI escrow scope decision
//        pkg_path_is_boot()     - inside pkg_write_permit, before the perms arm
//        pkg_write_permit()     - THE GATE UNDER TEST
//        escrow_replace_victim() - the pre-image capture
//      All four return -1 and the syscall returns -1, so a bare refusal is
//      worthless as evidence. THE ANSWER IS THE PAIRED ALLOW ARM: P2 performs
//      the SAME syscall, from the SAME uid, in the SAME directory, on the SAME
//      medium, differing only in who owns the target, and it must SUCCEED and
//      be byte-verified. A successful P2 proves escrow allowed, the boot-path
//      check allowed, the oracle said EXISTS and the writer wrote. After that
//      a P1 refusal has exactly one place left to come from.
//
//   2. "The redirection one is only reachable through the kernel shell, which
//      a Ring-3 test program cannot drive." THAT IS NOT TRUE OF THIS TREE, and
//      it is the reason this prover is an ordinary Ring-3 program and not a
//      serial-console script. open_redir_file() is called by spawn_impl(),
//      which Ring 3 reaches through SYS_SPAWN_REDIR (247, proc/syscall.h:298)
//      and SYS_SPAWN_ENV (394), both dispatched in syscall_dispatch_inner and
//      both with a libc wrapper (userland/libc/syscall.h sys_spawn_redir).
//      `cmd > file` in the kernel shell is ONE caller of that function; a
//      Ring-3 process asking for its child's stdout to be a file is another,
//      and it carries the uid the gate is about.
//
// ==========================================================================
// WHAT THE FIXTURE HAS TO LOOK LIKE FOR THE ARMS TO MEAN ANYTHING
// ==========================================================================
// The defect is "the CREATE branch was taken when the OVERWRITE branch was
// right". That is only OBSERVABLE where the two branches DISAGREE, so the
// fixture is built so they disagree as loudly as possible:
//
//   the directory is 01777 (sticky, world-writable)  -> the CREATE branch's
//        perms_check(parent, 1000, W_OK|X_OK) ALLOWS
//   the victim file is 0644 owned by uid 1001        -> the OVERWRITE branch's
//        perms_check(file, 1000, W_OK) DENIES
//
// So a kernel that takes the wrong branch PERMITS the write and a kernel that
// takes the right one REFUSES it. Both numbers are printed from Ring 0 below,
// so the disagreement is measured in the same boot as the arms rather than
// assumed from the mode bits.
//
// Two fixture directories, identical except for the medium:
//   /PKR            on the ROOT filesystem   - the CONTROL
//   <mount>/PK      on the removable volume  - the SUBJECT
// A red control means the prover or the fixture is broken; a green control
// next to a red subject means the kernel is.
//
// ==========================================================================
// WHY THE MOUNT-POINT ROW IS READ AND NOT WRITTEN
// ==========================================================================
// Same reason as usbopn_vmtest.c: [openperm] made the mount path stamp the
// mount point root:root 01777 (drivers/hotplug.c:641, rustkern/volperm.rs).
// Writing it here would hide a regression in that stamp. It is printed.
//
// CONSEQUENCE: PKGRDR MUST NOT SHARE A BOOT WITH USBRNT, which overwrites the
// mount-point row with 0755. USBOPN is harmless to share with but there is no
// reason to.

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

// Must match UID_ME / UID_OTHER in userland/apps/PKGRDR/main.c. A mismatch
// would make every deny arm pass for the wrong reason, so both sides name them
// explicitly rather than sharing a header neither owns.
#define PKGRDR_UID_ME     1000u
#define PKGRDR_UID_OTHER  1001u

// S_ISVTX | rwxrwxrwx. 0o1000 is PERM_STICKY in rustkern/sticky.rs.
#define PKGRDR_STICKY_DIR 01777

#define PKGRDR_PRELEN 64

// Four fixture files, one per arm, each with its own target: a deny arm that
// wrongly SUCCEEDS destroys its target, and a later arm sharing it would then
// be measuring an already-blanked file.
static const struct { const char *name; uint32_t uid; uint16_t mode; } pkgrdr_fix[] = {
    { "PVIC.TXT",  PKGRDR_UID_OTHER, 0644 },  // P1 deny : SYS_PKG_WRITE
    { "PMINE.TXT", PKGRDR_UID_ME,    0644 },  // P2 allow: SYS_PKG_WRITE
    { "RVIC.TXT",  PKGRDR_UID_OTHER, 0644 },  // R1 deny : SYS_SPAWN_REDIR
    { "RMINE.TXT", PKGRDR_UID_ME,    0644 },  // R2 allow: SYS_SPAWN_REDIR
};
#define PKGRDR_NFIX ((int)(sizeof(pkgrdr_fix) / sizeof(pkgrdr_fix[0])))

// ---------------------------------------------------------------------------
// The two fixture builders. Both route through hotplug_fs_or_root(), so ONE
// code path serves the root filesystem and the removable volume and the
// fat_fs_t handle is never chosen by hand. A hand-rolled "is this path on a
// volume" prefix match here is exactly the fault class fs/volblind.c detects.
// ---------------------------------------------------------------------------
static int pkgrdr_mkdir_any(const char *abspath) {
    const char *rel = abspath;
    fat_fs_t *fs = hotplug_fs_or_root(abspath, &rel);
    if (!fs) return -1;
    return fat_mkdir(fs, rel);
}

static int pkgrdr_write_any(const char *abspath, const void *data, uint32_t n) {
    const char *rel = abspath;
    fat_fs_t *fs = hotplug_fs_or_root(abspath, &rel);
    if (!fs) return -1;
    return fat_write_file(fs, rel, data, n);   // MUST_CHECK, 0 on success
}

// Build one fixture directory and its four files, and write the ownership rows.
// Returns the number of files that did not read back byte-correct.
//
// The ROWS ARE KEYED BY THE ABSOLUTE PATH, never by the volume-relative one.
// /CONFIG/PERMS.DB is keyed by the path userland names, so a row filed under
// "/PK/PVIC.TXT" would describe a directory in the root of the boot volume.
static int pkgrdr_build_fixture(const char *dir, const char *pre) {
    int bad = 0;
    char p[192];

    (void)pkgrdr_mkdir_any(dir);   // an existing directory is not an error
    perms_set(dir, PKGRDR_UID_OTHER, PKGRDR_UID_OTHER, PKGRDR_STICKY_DIR);

    for (int i = 0; i < PKGRDR_NFIX; i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, pkgrdr_fix[i].name);
        int w = pkgrdr_write_any(p, pre, (uint32_t)PKGRDR_PRELEN);
        // READ IT BACK. A 0-byte victim would make a deny arm pass for the
        // wrong reason: there would be nothing left to destroy.
        uint32_t got = 0;
        void *rb = 0;
        {
            const char *rel = p;
            fat_fs_t *fs = hotplug_fs_or_root(p, &rel);
            if (fs) rb = fat_read_file(fs, rel, &got);
        }
        int ok = (w == 0) && rb && (got == (uint32_t)PKGRDR_PRELEN) &&
                 (memcmp(rb, pre, PKGRDR_PRELEN) == 0);
        if (rb) kfree(rb);
        if (!ok) {
            bad++;
            kprintf("[PKGRDR-TEST] FIXTURE BAD %s write-rc=%d read-back=%u bytes\n",
                    p, w, (unsigned)got);
        }
        perms_set(p, pkgrdr_fix[i].uid, pkgrdr_fix[i].uid, pkgrdr_fix[i].mode);
    }
    return bad;
}

// ---------------------------------------------------------------------------
// THE BRANCH-DISAGREEMENT MEASUREMENT. Printed from Ring 0 for BOTH media, so
// the reader can see that the two branches really do give different answers
// here, and which oracle each gate would consult.
//
// Four numbers per medium, and each one is load-bearing:
//   create-branch   perms_check(dir,  1000, W_OK|X_OK)  must be 0  (ALLOW)
//   overwrite-branch perms_check(vic, 1000, W_OK)       must be -1 (DENY)
//   fixed oracle    recycle_victim_stat(vic)            must be 1  (EXISTS)
//   old oracle      fat_exists(&g_fat_fs, vic)          0 on a volume, 1 on root
//
// The last pair IS the defect, as two numbers. On the root filesystem both
// oracles say 1 and the gates were always right. On the volume the old oracle
// says 0 and the new one says 1, which is why the root arms passed for years
// while the volume arms were never asked.
// ---------------------------------------------------------------------------
static void pkgrdr_print_branch_disagreement(const char *tag, const char *dir) {
    char vic[192], mine[192];
    snprintf(vic,  sizeof(vic),  "%s/PVIC.TXT",  dir);
    snprintf(mine, sizeof(mine), "%s/PMINE.TXT", dir);

    int cb  = perms_check(dir,  PKGRDR_UID_ME, PKGRDR_UID_ME, W_OK | X_OK);
    int obv = perms_check(vic,  PKGRDR_UID_ME, PKGRDR_UID_ME, W_OK);
    int obm = perms_check(mine, PKGRDR_UID_ME, PKGRDR_UID_ME, W_OK);
    int newo = recycle_victim_stat(vic, 0, 0);
    int oldo = fat_exists(&g_fat_fs, vic);

    kprintf("[PKGRDR-TEST] branch-disagreement %s dir=%s "
            "create-branch(perms_check dir W_OK|X_OK uid=%u)=%d "
            "overwrite-branch(perms_check PVIC W_OK)=%d "
            "overwrite-branch(perms_check PMINE W_OK)=%d "
            "fixed-oracle(recycle_victim_stat PVIC)=%d "
            "old-oracle(fat_exists g_fat_fs PVIC)=%d\n",
            tag, dir, PKGRDR_UID_ME, cb, obv, obm, newo, oldo);

    if (cb == 0 && obv != 0)
        kprintf("[PKGRDR-TEST] branch-disagreement %s USABLE: the two branches "
                "disagree (create ALLOWS, overwrite DENIES), so taking the "
                "wrong one is observable as a permitted write\n", tag);
    else
        kprintf("[PKGRDR-TEST] branch-disagreement %s UNUSABLE: the branches "
                "AGREE, so no arm on this medium can tell them apart\n", tag);
}

static void pkgrdr_vmtest_worker(void *arg) {
    (void)arg;
    // AFTER the four escrow workers (7s/9s/11s/16s), usbrnt's 26s and usbopn's
    // 31s, so the verdicts do not interleave on the serial line, and
    // comfortably after USB enumeration and the hotplug mount.
    proc_sleep(36000);

    uint32_t csz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/PKGRDR.TEST", &csz);
    if (!cfg) return;   // not flagged -> silent no-op (the production path)
    kfree(cfg);

    kprintf("\n========== SYS_PKG_WRITE + SHELL-REDIRECT PERMISSION TEST (#246 provetwo) ==========\n");
    if (!g_fat_fs.mounted) { kprintf("[PKGRDR-TEST] no root fs\n"); return; }

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
        kprintf("[PKGRDR-TEST] using removable volume %s (VID=%04x PID=%04x)\n",
                mnt, d->vendor_id, d->product_id);
        break;
    }
    if (!mnt[0]) {
        kprintf("[PKGRDR-TEST] NO removable volume mounted; the pkg-write and "
                "redirect arms cannot run (attach a synthetic usb-storage)\n");
        return;
    }

    // ---- the 64-byte known pre-image, built here so the kernel side and the
    // Ring-3 side cannot disagree about what "intact" means.
    char pre[PKGRDR_PRELEN];
    for (int i = 0; i < PKGRDR_PRELEN; i++) pre[i] = (char)('A' + (i % 26));

    char vdir[HOTPLUG_MOUNT_PATH_LEN + 8];
    snprintf(vdir, sizeof(vdir), "%s/PK", mnt);

    int badr = pkgrdr_build_fixture("/PKR", pre);
    int badv = pkgrdr_build_fixture(vdir,   pre);
    {
        int src = perms_sync();
        kprintf("[PKGRDR-TEST] fixtures built: /PKR bad=%d  %s bad=%d  "
                "(dirs %04o owned %u, two victims owned %u, two own files owned %u) "
                "perms_sync=%d\n",
                badr, vdir, badv, (unsigned)PKGRDR_STICKY_DIR,
                PKGRDR_UID_OTHER, PKGRDR_UID_OTHER, PKGRDR_UID_ME, src);
        // THE SAME VERDICT AS A SHORT LINE, and the harness keys on THIS one.
        //
        // MEASURED 2026-10-04: the long line above came back SPLICED in one
        // boot out of four. fs/volblind.c's report reaches the serial log
        // through the FAULTLOG writer, which flushes asynchronously and is not
        // serialised against kprintf, so it landed mid-token:
        //   "[PKGRDR-TEST] fixtures builEMOVABLE VOLUME, ... ot: /PKR bad=0"
        // Every arm in that boot passed and the harness still reported PKGRDR
        // RED, because its REQUIRE regex spanned 24 characters of a 180-column
        // line. A long required pattern is a coin toss on a shared serial
        // line, and this launcher PROVOKES the interleaving itself with its
        // deliberate old-oracle probe.
        //
        // The failure direction was safe (a false RED, never a false GREEN),
        // which is the right way round, but it costs a 5-minute re-run. A
        // short line that a splice is unlikely to break, plus a FORBID on a
        // non-zero count, keeps both properties and stays fail-closed: if even
        // this line is spliced the run goes red rather than quiet.
        kprintf("[PKGRDR-TEST] fixture-bad root=%d vol=%d\n", badr, badv);
    }

    // ---- THE MOUNT POINT'S OWN ROW, READ AND NOT WRITTEN.
    {
        uint32_t muid = 0, mgid = 0; uint16_t mmode = 0;
        int has = (perms_get(mnt, &muid, &mgid, &mmode) == 0);
        kprintf("[PKGRDR-TEST] mount-point row %s: %s uid=%u gid=%u mode=%04o "
                "(read, never written by this launcher: [openperm] made the "
                "MOUNT PATH stamp it, and a regression there must show up here)\n",
                mnt, has ? "PRESENT" : "ABSENT (no-entry default root:root 0755)",
                (unsigned)muid, (unsigned)mgid, (unsigned)mmode);
    }

    // ---- the disagreement that makes the arms meaningful, for both media.
    pkgrdr_print_branch_disagreement("ROOT-CONTROL", "/PKR");
    pkgrdr_print_branch_disagreement("VOLUME",       vdir);

    // ---- launch the prover as uid 0; it does its own one-way drop to uid
    // 1000 after it has verified the fixture, which root can do and uid 1000
    // deliberately cannot.
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, "/APPS/PKGRDR", &sz);
    if (!data || sz == 0) {
        if (data) kfree(data);
        kprintf("[PKGRDR-TEST] /APPS/PKGRDR not found (build with unshipped apps)\n");
        return;
    }
    if (elf_validate(data, sz) != 0) {
        kfree(data);
        kprintf("[PKGRDR-TEST] /APPS/PKGRDR bad ELF\n");
        return;
    }
    // The helper the redirect arms spawn must be on the image too, or BOTH
    // redirect arms fail for an environment reason and the pair proves nothing.
    // Checked HERE rather than discovered from a confusing Ring-3 red.
    {
        uint32_t hsz = 0;
        void *h = fat_read_file(&g_fat_fs, "/APPS/PKGRDRH", &hsz);
        kprintf("[PKGRDR-TEST] redirect-helper /APPS/PKGRDRH: %s (%u bytes)\n",
                (h && hsz) ? "PRESENT" : "MISSING", (unsigned)hsz);
        if (h) kfree(h);
    }
    int pid = proc_create_user_as("/APPS/PKGRDR", data, sz, 0, 0, proc_as_uid(0));
    kfree(data);
    kprintf("[PKGRDR-TEST] launched /APPS/PKGRDR pid=%d (UNMARKED: no escrow "
            "guard in the way); watch for PKGRDR: lines\n", pid);
    kprintf("========== SYS_PKG_WRITE + SHELL-REDIRECT PERMISSION TEST ARMED ==========\n");
}

void pkgrdr_start_deferred_test(void) {
    proc_create_ex("pkgrdrtest", pkgrdr_vmtest_worker, 0, PRIO_LOW, 256 * 1024);
}
