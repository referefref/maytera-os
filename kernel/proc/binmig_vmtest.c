// proc/binmig_vmtest.c - [binmigrate] GATED boot prover for the ONE fact that
// matters: a file deleted during the first-boot setup wizard is STILL IN THE
// USER'S RECYCLE BIN after the wizard creates their account.
//
// TEST SCAFFOLDING, not a shipping feature. A no-op unless
// /CONFIG/BINMIG.TEST exists on the root fs, exactly like the five escrow
// verifiers it sits beside (proc/escrvt_vmtest.c and friends), and armed and
// JUDGED by tools/escrow-vmtest/run-escrow-vmtests.sh --suite BINMIG so it
// cannot become one more marker-gated test that nothing ever creates a marker
// for. That failure mode is real: four escrow verifiers were found on
// 2026-10-03 to have never run in any automated context.
//
// WHY THIS IS NOT A UNIT TEST, AND IT MATTERS. The defect is a DISAGREEMENT
// BETWEEN TWO MOMENTS of one live session: where the per-account state root
// resolves before the account exists, and where it resolves after. A test that
// called the resolver twice with made-up arguments would prove the resolver is
// consistent, which it is; the bug is that the FILES do not follow. So every
// step below goes through the real chokepoints on a real boot:
// recycle_capture() to delete, userconf_kpath() to ask where the bin is, and
// users_create_first_admin() to create the account. Nothing here spells a bin
// path. Three separate tests in this area have been written against a
// hardcoded path and measured the wrong directory.
//
// WHAT IT NEEDS, AND WHAT IT REFUSES TO PRETEND. It needs a genuine first-boot
// machine: zero accounts, and a session uid with no user-table entry (the
// #OOBEAUTH bootstrap session). On any other machine it CANNOT measure the
// defect, and it says NOT-RUN rather than printing a PASS that means nothing.
// The harness requires "OVERALL PASS", so a NOT-RUN is red.
//
// IT CHANGES THE MACHINE: it creates the first account. That is why it is
// gated, and it is also why the harness runs it against a COPY of a golden.
//
// C, not Rust, for the same reason proc/escrvt_vmtest.c is: it is entangled
// with the C filesystem, user-table and recycle APIs, which have no Rust
// surface, and it is throwaway verification code rather than a new kernel
// subsystem. The POLICY it exercises is in Rust (rustkern/binmigrate.rs,
// rustkern/userconf.rs, rustkern/recycle.rs).

#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/userconf.h"
#include "../fs/recycle_fs.h"
#include "../gui/desktop.h"
#include "process.h"
#include "users.h"

extern fat_fs_t g_fat_fs;
extern void kprintf(const char *fmt, ...);
extern void proc_sleep(uint32_t ms);
extern unsigned long rcy_row_name_rs(const unsigned char *row, unsigned long row_len,
                                     unsigned char *out, unsigned long cap);

#define BM_T_PATH   256
#define BM_T_VICTIM "/TMP/BMVICT.TXT"
#define BM_T_BYTES  "binmigrate victim: these bytes must survive account creation\n"
#define BM_T_USER   "bmtester"
// Two independent passwords, both of which must pass the FULL policy (>= 8
// bytes, >= PW_MIN_DISTINCT distinct bytes, no keyboard/counting run, not on
// the breached list, not containing the username) and must not be equal to
// each other. A refusal here would make the whole run red at step F with a
// policy code, which is unambiguous in the transcript.
#define BM_T_PW     "Grz7#vqLm2"
#define BM_T_ROOTPW "Kp4$wTbnX9"

static uint32_t bm_pass, bm_fail;

static void bm_line(const char *label, int ok) {
    if (ok) bm_pass++; else bm_fail++;
    kprintf("BINMIG: %s %s\n", label, ok ? "PASS" : "FAIL");
}

static void bm_notrun(const char *why) {
    kprintf("BINMIG: NOT-RUN %s\n", why);
    kprintf("BINMIG: OVERALL NOT-RUN (this machine cannot exhibit the defect; "
            "a PASS here would mean nothing)\n");
    kprintf("========== BINMIG END ==========\n");
}

// Does `path` hold exactly `want`? Returns 1 only if the file reads back and
// the bytes match, so "reachable" means literally readable, not merely listed.
static int bm_bytes_are(const char *path, const char *want) {
    uint32_t sz = 0;
    char *b = (char *)fat_read_file(&g_fat_fs, path, &sz);
    if (!b) return 0;
    uint32_t n = 0;
    while (want[n]) n++;
    int ok = (sz == n);
    for (uint32_t i = 0; ok && i < n; i++) if (b[i] != want[i]) ok = 0;
    kfree(b);
    return ok;
}

// Find the index row whose SECOND field (the original location) is `orig`.
// Copies the whole row (without its newline) into `row_out` and the row's
// first field (the trash-side name) into `name_out`, using the kernel's own
// row-name parser rather than a second copy of it.
//
// THE ROW IS KEPT VERBATIM ON PURPOSE. The strongest available oracle for "the
// attribution survived" is that the exact same bytes are present afterwards:
// it covers the actor column, the original location and the deletion
// timestamp in one comparison, and it cannot be satisfied by a row that was
// re-derived with today's clock or today's uid.
static int bm_find_row(const char *index_path, const char *orig,
                       char *row_out, uint32_t row_cap,
                       char *name_out, uint32_t name_cap) {
    uint32_t sz = 0;
    char *b = (char *)fat_read_file(&g_fat_fs, index_path, &sz);
    if (!b) return 0;

    uint32_t olen = 0; while (orig[olen]) olen++;
    int found = 0;
    uint32_t i = 0;
    while (i < sz && !found) {
        uint32_t s = i;
        while (i < sz && b[i] != '\n') i++;
        uint32_t len = i - s;
        if (i < sz) i++;                       // step over the newline
        if (len == 0 || len + 1 > row_cap) continue;

        // field 1 ends at the first '|', field 2 at the second.
        uint32_t f1 = 0; while (f1 < len && b[s + f1] != '|') f1++;
        if (f1 >= len) continue;
        uint32_t f2s = f1 + 1, f2 = f2s;
        while (f2 < len && b[s + f2] != '|') f2++;
        if (f2 - f2s != olen) continue;
        int same = 1;
        for (uint32_t k = 0; k < olen; k++)
            if (b[s + f2s + k] != orig[k]) { same = 0; break; }
        if (!same) continue;

        for (uint32_t k = 0; k < len; k++) row_out[k] = b[s + k];
        row_out[len] = '\0';
        unsigned long nl = rcy_row_name_rs((const unsigned char *)row_out, len,
                                           (unsigned char *)name_out, name_cap);
        found = (nl > 0);
    }
    kfree(b);
    return found;
}

// Is `needle` (a whole index row) present in the file at `path`?
static int bm_file_has_row(const char *path, const char *needle) {
    uint32_t sz = 0;
    char *b = (char *)fat_read_file(&g_fat_fs, path, &sz);
    if (!b) return 0;
    uint32_t nl = 0; while (needle[nl]) nl++;
    int found = 0;
    if (nl > 0 && sz >= nl) {
        for (uint32_t i = 0; i + nl <= sz && !found; i++) {
            uint32_t k = 0;
            while (k < nl && b[i + k] == needle[k]) k++;
            if (k == nl) found = 1;
        }
    }
    kfree(b);
    return found;
}

static void bm_join(char *out, uint32_t cap, const char *dir, const char *name) {
    uint32_t n = 0;
    for (const char *p = dir; *p && n + 1 < cap; p++) out[n++] = *p;
    if (n == 0 || out[n - 1] != '/') { if (n + 1 < cap) out[n++] = '/'; }
    for (const char *p = name; *p && n + 1 < cap; p++) out[n++] = *p;
    out[n] = '\0';
}

static void binmig_vmtest_worker(void *arg) {
    (void)arg;
    // AFTER the escrow workers (7s/9s/11s/15s/16s) so the verdicts do not
    // interleave on the serial line, and late enough that login_run() has
    // long since established the session identity this test depends on.
    proc_sleep(22000);

    uint32_t csz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/BINMIG.TEST", &csz);
    if (!cfg) return;                 // not flagged -> silent no-op (production path)
    kfree(cfg);

    bm_pass = bm_fail = 0;
    kprintf("\n========== BINMIG (first-boot Recycle Bin migration) ==========\n");

    if (!g_fat_fs.mounted) { bm_notrun("no filesystem mounted"); return; }

    // ---- A. PRECONDITIONS. Say what machine this is, in numbers. ----------
    uint32_t uid = desktop_get_session_uid();
    int naccounts = users_count_active();
    user_entry_t *before = user_lookup_uid(uid);
    kprintf("BINMIG: machine state: session uid=%u, active accounts=%d, "
            "this uid has a user-table entry=%s\n",
            (unsigned)uid, naccounts, before ? "YES" : "no");
    if (naccounts != 0 || before != 0) {
        bm_notrun("this is not a first-boot bootstrap session (it already has "
                  "an account), so the stranding window does not exist here");
        return;
    }
    bm_line("A.preconditions(first-boot bootstrap session, no accounts)", 1);

    // ---- B. DELETE A FILE THE WAY AN AI ACTOR WOULD -----------------------
    if (fat_write_file(&g_fat_fs, BM_T_VICTIM, BM_T_BYTES,
                       (uint32_t)strlen(BM_T_BYTES)) != 0) {
        bm_notrun("could not create the victim file");
        return;
    }
    int cap_rc = recycle_capture(BM_T_VICTIM, RCY_ACTOR_AI, 0x0A17E57u);
    kprintf("BINMIG: recycle_capture(%s) rc=%d (%s)\n",
            BM_T_VICTIM, cap_rc, recycle_strresult(cap_rc));
    bm_line("B.delete-during-setup captured into the bin", cap_rc == RCY_OK);
    if (cap_rc != RCY_OK) {
        kprintf("BINMIG: OVERALL FAIL (%u passed, %u failed)\n", bm_pass, bm_fail);
        kprintf("========== BINMIG END ==========\n");
        return;
    }

    // ---- C/D/E. WHERE IS IT NOW, AND IS IT REALLY RECOVERABLE? ------------
    // Asked through userconf_kpath(), the ONE accessor, exactly as the bin's
    // own code and the Recycle Bin app ask it.
    char oldbin[BM_T_PATH], oldidx[BM_T_PATH];
    if (userconf_kpath("RECYCLE", oldbin, sizeof(oldbin)) != 0 ||
        userconf_kpath(recycle_index_leaf(), oldidx, sizeof(oldidx)) != 0) {
        bm_notrun("could not resolve the bin before account creation");
        return;
    }
    kprintf("BINMIG: bin BEFORE account creation: %s (index %s)\n", oldbin, oldidx);

    char row[512], trash[128];
    int have_row = bm_find_row(oldidx, BM_T_VICTIM, row, sizeof(row),
                               trash, sizeof(trash));
    bm_line("D.index row recorded with the original location", have_row);
    if (!have_row) {
        kprintf("BINMIG: OVERALL FAIL (%u passed, %u failed)\n", bm_pass, bm_fail);
        kprintf("========== BINMIG END ==========\n");
        return;
    }
    kprintf("BINMIG: index row = '%s' (trash name '%s')\n", row, trash);

    char oldfile[BM_T_PATH];
    bm_join(oldfile, sizeof(oldfile), oldbin, trash);
    bm_line("E.the deleted bytes are readable from the bin BEFORE account creation",
            bm_bytes_are(oldfile, BM_T_BYTES));

    // ---- F. THE USER FINISHES SETUP ---------------------------------------
    int cr = users_create_first_admin(BM_T_USER, BM_T_PW, BM_T_ROOTPW);
    kprintf("BINMIG: users_create_first_admin('%s') rc=%d\n", BM_T_USER, cr);
    bm_line("F.the setup wizard's account was created", cr == 0);
    if (cr != 0) {
        kprintf("BINMIG: OVERALL FAIL (%u passed, %u failed)\n", bm_pass, bm_fail);
        kprintf("========== BINMIG END ==========\n");
        return;
    }

    // ---- G. THE STRANDING WINDOW MUST ACTUALLY EXIST ON THIS BUILD --------
    char newbin[BM_T_PATH], newidx[BM_T_PATH];
    if (userconf_kpath("RECYCLE", newbin, sizeof(newbin)) != 0 ||
        userconf_kpath(recycle_index_leaf(), newidx, sizeof(newidx)) != 0) {
        bm_notrun("could not resolve the bin after account creation");
        return;
    }
    kprintf("BINMIG: bin AFTER account creation:  %s (index %s)\n", newbin, newidx);
    int moved_root = (strcmp(oldbin, newbin) != 0);
    bm_line("G.the bin's resolved path CHANGED at account creation "
            "(no change = nothing to strand, and nothing measured)", moved_root);
    if (!moved_root) {
        kprintf("BINMIG: OVERALL FAIL (%u passed, %u failed)\n", bm_pass, bm_fail);
        kprintf("========== BINMIG END ==========\n");
        return;
    }

    // ---- H/I. THE TWO ARMS THAT ARE THE BUG -------------------------------
    char newfile[BM_T_PATH];
    bm_join(newfile, sizeof(newfile), newbin, trash);
    int reachable = bm_bytes_are(newfile, BM_T_BYTES);
    kprintf("BINMIG: looking for the deleted file at %s\n", newfile);
    bm_line("H.the file deleted during setup is READABLE from the account's own bin",
            reachable);
    bm_line("I.its index row survived VERBATIM (actor, original location and "
            "deletion time all intact)", bm_file_has_row(newidx, row));

    // ---- J. Evidence for a RED run: where did it actually end up? ---------
    kprintf("BINMIG: old location %s still holds the file: %s\n",
            oldfile, fat_exists(&g_fat_fs, oldfile) == 1 ? "YES (STRANDED)" : "no");
    kprintf("BINMIG: old index %s still exists: %s\n",
            oldidx, fat_exists(&g_fat_fs, oldidx) == 1 ? "YES" : "no");

    kprintf("BINMIG: OVERALL %s (%u passed, %u failed)\n",
            bm_fail == 0 ? "PASS" : "FAIL", bm_pass, bm_fail);
    kprintf("========== BINMIG END ==========\n");
}

void binmig_start_deferred_test(void) {
    proc_create_ex("binmigtest", binmig_vmtest_worker, 0, PRIO_LOW, 256 * 1024);
}
