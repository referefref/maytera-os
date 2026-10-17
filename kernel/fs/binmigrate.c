// binmigrate.c - [binmigrate] the filesystem half of the bootstrap-state
// migration. See fs/binmigrate.h for what the defect is and
// rustkern/binmigrate.rs for the policy.
//
// C, NOT RUST, and this is the justification the CHANGELOG entry carries:
// every line here is entanglement with the C filesystem and permission
// primitives (fat_open / fat_readdir_n / fat_is_dir / fat_exists / fat_move /
// fat_read_file / fat_write_file / fat_delete / fat_mkdir, perms_get /
// perms_set / perms_remove / perms_on_create / perms_sync), none of which has
// a Rust surface in this tree, plus the C user table and the C per-user path
// resolver. The parts with an actual failure shape, the bounded path join and
// the four-way per-entry disposition, ARE in Rust (rustkern/binmigrate.rs).
// That is the same split fs/recycle_fs.c has against rustkern/recycle.rs and
// fs/userconf.c has against rustkern/userconf.rs.
//
// NOT ONE PATH LITERAL IN THIS FILE. The old state root comes from
// userconf_state_root_rs(has_entry=0), the new one from the same function with
// the account's home, the "CONFIG" component from userconf_kpath_home() by
// truncation (so it can never disagree with where files are actually put), and
// the Recycle Bin index leaf from recycle_index_leaf(). Three separate tests
// in this area have already been written against a hardcoded bin path and
// measured the wrong directory; a hardcoded path here would instead MOVE the
// wrong directory.
//
// NO WAITING, NO POLLING, NO SPINNING. Straight-line blocking filesystem I/O
// in process context, which is already a may-block context, so
// wq_assert_may_block() is satisfied for free and concurrency-lint has nothing
// to see. Every loop is bounded by a table size or by readdir running out of
// entries, and makes real progress on each iteration.

#include "binmigrate.h"
#include "userconf.h"
#include "recycle_fs.h"
#include "fat.h"
#include "perms.h"
#include "bootlog.h"
#include "../proc/users.h"
#include "../gui/desktop.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"

extern fat_fs_t g_fat_fs;
extern void kprintf(const char *fmt, ...);

// The Rust policy.
extern int32_t binmig_plan_rs(int32_t is_dir, int32_t dest_exists,
                              int32_t mergeable, uint32_t depth);
extern int32_t binmig_join_rs(const char *dir, const char *name,
                              char *out, uint32_t cap);
extern int32_t binmig_arm_rs(int32_t has_entry, uint32_t uid);
extern int64_t binmig_armed_uid_rs(void);
extern int32_t binmig_claim_rs(uint32_t uid);
extern int32_t binmig_selftest_rs(uint32_t *out_checks);
// [noaccount]'s decision function, reused rather than re-derived: asking it
// with has_entry=0 is the ONLY way to learn where a no-account session's state
// lives without repeating the literal it owns.
extern int userconf_state_root_rs(int has_entry, const char *home,
                                  char *out, uint32_t cap);

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------
//
// A TWO-PHASE WALK, AND THAT IS NOT A STYLE CHOICE. ext2_readdir_ino() takes a
// BYTE CURSOR into the directory data, and ext2 unlink merges a freed dirent
// into the one before it. Moving files out of a directory while a cursor is
// open on it therefore re-flows the bytes under the cursor and can skip
// entries, which in this subsystem means silently leaving files behind in the
// directory nobody will look in again: the exact failure this code exists to
// prevent, reintroduced by the code preventing it. So the names are collected
// first, the directory is closed, and only then is anything moved.
//
// fs/escrow_undo.c's escu_rmtree() solves the same hazard the other way, by
// re-opening the directory and taking ONE child per pass until it is empty,
// and says so in its own comment. That shape does not work here: a SKIP
// deliberately LEAVES an entry in place, so "take one until empty" would spin
// on it forever. The snapshot is the version that terminates.
#define BM_MAX_ENTRIES  512   // ~400 is the most bin entries RCY_INDEX_MAX allows
#define BM_NAME_CAP      96   // refuse-and-report a longer name; never truncate
#define BM_PATH_CAP     256   // the same cap every other path buffer here uses

typedef struct {
    char    name[BM_NAME_CAP];
    uint8_t is_dir;
} bm_ent_t;

typedef struct {
    uint32_t moved;
    uint32_t appended;
    uint32_t skipped;     // a collision we refused to resolve by clobbering
    uint32_t failed;      // an I/O refusal: the source is STILL THERE
    uint32_t unlisted;    // a name too long for the table, or the table filled
} bm_stats_t;

// ---------------------------------------------------------------------------
// "<root>/CONFIG", derived from the joiner rather than spelled again
// ---------------------------------------------------------------------------
//
// userconf_kpath_home(root, leaf) is the ONE function that decides where a
// per-account file goes. Building the directory by asking it for a file and
// dropping the leaf means the directory this code walks is, by construction,
// the directory those files are in. Re-deriving it as root + "/CONFIG" would
// be a second spelling that is correct only until the first one changes.
// fs/recycle_fs.c's rcy_ensure_bin() computes the bin's parent the same way
// and for the same reason.
static int bm_config_dir(const char *root, char *out, uint32_t cap) {
    char p[BM_PATH_CAP];
    if (userconf_kpath_home(root, "X", p, sizeof(p)) != 0) return -1;
    uint32_t n = 0;
    while (p[n]) n++;
    while (n > 1 && p[n - 1] != '/') n--;
    if (n <= 1) return -1;                 // no separator to cut at
    p[n - 1] = '\0';                       // drop the '/' and the leaf
    uint32_t i = 0;
    while (p[i]) { if (i + 1 >= cap) return -1; out[i] = p[i]; i++; }
    out[i] = '\0';
    return 0;
}

static int bm_is_dot(const char *n) {
    if (n[0] == '.' && n[1] == '\0') return 1;
    if (n[0] == '.' && n[1] == '.' && n[2] == '\0') return 1;
    return 0;
}

// Phase 1: snapshot a directory's entries. Returns the count, or -1 if the
// directory could not be opened. `*unlisted` is incremented for every entry
// this table could not hold, so a partial migration is REPORTED and never
// mistaken for a complete one.
static int bm_list(const char *dir, bm_ent_t *ents, uint32_t max, uint32_t *unlisted) {
    fat_file_t f;
    if (fat_open(&g_fat_fs, dir, &f) != 0) return -1;
    if (!fat_is_dir(&f)) { fat_close(&f); return -1; }

    uint32_t n = 0;
    fat_dir_entry_t de;
    char name[BM_PATH_CAP];
    while (fat_readdir_n(&f, &de, name, sizeof(name)) == 0) {
        if (bm_is_dot(name)) continue;
        uint32_t len = 0;
        while (name[len]) len++;
        if (len == 0 || len >= BM_NAME_CAP) { (*unlisted)++; continue; }
        if (n >= max) { (*unlisted)++; continue; }
        uint32_t i = 0;
        for (; i < len; i++) ents[n].name[i] = name[i];
        ents[n].name[i] = '\0';
        ents[n].is_dir = (de.attr & FAT_ATTR_DIRECTORY) ? 1 : 0;
        n++;
    }
    fat_close(&f);
    return (int)n;
}

// Stamp a migrated path for its new owner. The mode is carried from the source
// when the permission table had a row for it, so a 0700 Recycle Bin stays
// 0700; otherwise the ordinary create-time default applies.
//
// THE OWNER IS THE NEW ACCOUNT, NOT THE RECORDED ONE. [noaccount] measured
// /TMP/CONFIG/RECYCLE stamped 0:0:0700 because the escrow layer created it
// before the session had a uid, which is precisely the wrong owner to carry
// forward: it is the bug that made the recycler daemon unable to read its own
// bin.
static void bm_stamp(const char *src, const char *dst,
                     uint32_t uid, uint32_t gid, int is_dir) {
    uint32_t ouid = 0, ogid = 0;
    uint16_t omode = 0;
    if (perms_get(src, &ouid, &ogid, &omode) == 0 && (omode & 0777) != 0)
        perms_set(dst, uid, gid, (uint16_t)(omode & 0777));
    else
        perms_on_create(dst, uid, gid, is_dir);
    perms_remove(src);
}

// APPEND: both halves of a row log survive. Used for the Recycle Bin index,
// whose rows carry the original location, the deletion time and the actor
// ("user" or "ai:xxxxxxxx"), which is the attribution the owner's requirement
// is about. The bytes are copied verbatim, so none of it is re-derived and
// none of it can be lost in a reformat.
static int bm_append(const char *src, const char *dst, uint32_t uid, uint32_t gid) {
    uint32_t ssz = 0, dsz = 0;
    char *sbuf = (char *)fat_read_file(&g_fat_fs, src, &ssz);
    if (!sbuf) return -1;
    char *dbuf = (char *)fat_read_file(&g_fat_fs, dst, &dsz);

    char *joined = (char *)kmalloc(dsz + ssz + 2);
    if (!joined) { if (dbuf) kfree(dbuf); kfree(sbuf); return -1; }
    uint32_t w = 0;
    for (uint32_t i = 0; i < dsz; i++) joined[w++] = dbuf[i];
    // A destination that does not end in a newline would otherwise glue its
    // last row onto our first one and produce a single unparseable row.
    if (w > 0 && joined[w - 1] != '\n') joined[w++] = '\n';
    for (uint32_t i = 0; i < ssz; i++) joined[w++] = sbuf[i];
    if (dbuf) kfree(dbuf);
    kfree(sbuf);

    int rc = fat_write_file(&g_fat_fs, dst, joined, w);
    kfree(joined);
    if (rc != 0) return -1;                 // the source is untouched: nothing lost
    bm_stamp(src, dst, uid, gid, 0);
    (void)fat_delete(&g_fat_fs, src);
    return 0;
}

static void bm_walk(const char *srcdir, const char *dstdir, uint32_t depth,
                    uint32_t uid, uint32_t gid, bm_stats_t *st) {
    bm_ent_t *ents = (bm_ent_t *)kmalloc(sizeof(bm_ent_t) * BM_MAX_ENTRIES);
    if (!ents) { st->failed++; return; }

    int n = bm_list(srcdir, ents, BM_MAX_ENTRIES, &st->unlisted);
    if (n < 0) { kfree(ents); return; }     // not a directory, or gone: nothing to do

    const char *idx_leaf = recycle_index_leaf();

    for (int i = 0; i < n; i++) {
        char sp[BM_PATH_CAP], dp[BM_PATH_CAP];
        if (binmig_join_rs(srcdir, ents[i].name, sp, sizeof(sp)) < 0 ||
            binmig_join_rs(dstdir, ents[i].name, dp, sizeof(dp)) < 0) {
            // Refused rather than truncated. A truncated path is a different
            // file, and here that is either a clobber or a lost original.
            st->unlisted++;
            kprintf("[BINMIG] path too long for '%s' under %s: LEFT IN PLACE\n",
                    ents[i].name, srcdir);
            continue;
        }

        int dest_exists = (fat_exists(&g_fat_fs, dp) == 1);
        int mergeable = (idx_leaf && strcmp(ents[i].name, idx_leaf) == 0);
        int32_t act = binmig_plan_rs(ents[i].is_dir ? 1 : 0, dest_exists,
                                     mergeable, depth);

        switch (act) {
            case BM_ACT_RECURSE:
                if (!dest_exists) {
                    if (fat_mkdir(&g_fat_fs, dp) != 0 && fat_exists(&g_fat_fs, dp) != 1) {
                        st->failed++;
                        kprintf("[BINMIG] could not create %s: %s LEFT IN PLACE\n", dp, sp);
                        break;
                    }
                }
                // The destination directory is stamped BEFORE the walk, so the
                // files moved into it inherit from a parent that already has a
                // row; the source row is dropped AFTER, because the walk still
                // has to read through the source directory.
                {
                    uint32_t ouid = 0, ogid = 0; uint16_t omode = 0;
                    if (perms_get(sp, &ouid, &ogid, &omode) == 0 && (omode & 0777) != 0)
                        perms_set(dp, uid, gid, (uint16_t)(omode & 0777));
                    else
                        perms_on_create(dp, uid, gid, 1);
                }
                bm_walk(sp, dp, depth + 1, uid, gid, st);
                perms_remove(sp);
                // The source directory is now empty if everything moved. A
                // failure here is not an error: a SKIP or a failed move leaves
                // a file in it on purpose, and fat_delete only removes an
                // empty directory.
                (void)fat_delete(&g_fat_fs, sp);
                break;

            case BM_ACT_MOVE:
                if (fat_move(&g_fat_fs, sp, dp) == 0) {
                    bm_stamp(sp, dp, uid, gid, ents[i].is_dir ? 1 : 0);
                    st->moved++;
                } else {
                    st->failed++;
                    kprintf("[BINMIG] move FAILED %s -> %s; the original is still there\n", sp, dp);
                }
                break;

            case BM_ACT_APPEND:
                if (bm_append(sp, dp, uid, gid) == 0) {
                    st->appended++;
                } else {
                    st->failed++;
                    kprintf("[BINMIG] merge FAILED %s -> %s; the original is still there\n", sp, dp);
                }
                break;

            default:   // BM_ACT_SKIP
                st->skipped++;
                kprintf("[BINMIG] %s NOT migrated: %s already exists and is not a row log. "
                        "The source is still readable at its old path.\n", sp, dp);
                break;
        }
    }

    kfree(ents);
}

// ---------------------------------------------------------------------------
// The two entry points
// ---------------------------------------------------------------------------

void binmig_session_established(uint32_t uid, int has_entry) {
    int armed = binmig_arm_rs(has_entry ? 1 : 0, uid);
    if (armed) {
        // Said out loud on the ONE boot where it matters, because the
        // alternative is a feature whose presence cannot be established from a
        // boot log. On every machine that has an account this prints nothing.
        char root[BM_PATH_CAP];
        if (userconf_state_root_rs(0, (const char *)0, root, sizeof(root)) < 0) root[0] = '\0';
        kprintf("[BINMIG] session uid=%u has NO user-table entry: its per-account "
                "state is under %s and will be migrated into the account the "
                "setup wizard creates\n", (unsigned)uid, root[0] ? root : "?");
        bootlog_write("[BINMIG] armed: bootstrap session uid=%u, state root '%s'",
                      (unsigned)uid, root[0] ? root : "?");
    }
}

// [binmigrate] THE KILL-SWITCH, AND IT IS WHAT MAKES THE FIX PROVABLE.
//
// /CONFIG/BINMIG.OFF restores the PRE-FIX behaviour exactly: the migration
// becomes the no-op it used to be, which is to say nothing in the tree moves
// the files. The same kernel.elf can therefore be measured both ways, so the
// RED and GREEN arms of proc/binmig_vmtest.c differ by ONE VARIABLE, a file on
// the image, rather than by two separately-compiled binaries that could differ
// in some other way nobody looked at. main.c's #fdreclaim and #305 seal
// switches exist for the same reason and are named the same way.
//
// It is also an operator escape hatch: a machine where this migration misfires
// can be booted without it and still reach its data at the old path, which is
// a strictly better failure mode than a kernel that must be replaced.
// fat_exists, not fat_read_file: the latter prints "[FAT] fat_read_file: open
// failed" for an absent file, and on the one boot per machine that reaches here
// that line looks like a fault and is not one. Existence is the whole question.
static int bm_disabled(void) {
    return fat_exists(&g_fat_fs, "/CONFIG/BINMIG.OFF") == 1;
}

int binmig_on_account_committed(uint32_t uid) {
    if (!binmig_claim_rs(uid)) return 0;      // not our session, or already done

    if (bm_disabled()) {
        // LOUD. A disabled data-rescue path that said nothing would be
        // indistinguishable from one that is not in the kernel at all.
        kprintf("[BINMIG] DISABLED by /CONFIG/BINMIG.OFF: uid=%u committed an "
                "account and its bootstrap per-account state was NOT migrated. "
                "This is the pre-fix behaviour and it STRANDS the first-boot "
                "Recycle Bin.\n", (unsigned)uid);
        bootlog_write("[BINMIG] DISABLED by /CONFIG/BINMIG.OFF; uid=%u state NOT migrated",
                      (unsigned)uid);
        return 0;
    }

    if (!g_fat_fs.mounted) {
        kprintf("[BINMIG] uid=%u account committed but no filesystem is mounted; "
                "NOTHING MIGRATED\n", (unsigned)uid);
        return -1;
    }

    user_entry_t *u = user_lookup_uid(uid);
    if (!u || !u->home[0]) {
        kprintf("[BINMIG] uid=%u account committed with no usable home; "
                "NOTHING MIGRATED\n", (unsigned)uid);
        bootlog_write("[BINMIG] uid=%u committed with no home; nothing migrated", (unsigned)uid);
        return -1;
    }

    char oldroot[BM_PATH_CAP], newroot[BM_PATH_CAP];
    if (userconf_state_root_rs(0, (const char *)0, oldroot, sizeof(oldroot)) < 0 ||
        userconf_state_root_rs(1, u->home, newroot, sizeof(newroot)) < 0) {
        kprintf("[BINMIG] could not resolve the state roots for uid=%u; NOTHING MIGRATED\n",
                (unsigned)uid);
        return -1;
    }
    if (strcmp(oldroot, newroot) == 0) return 0;   // nothing moved: same place

    char src[BM_PATH_CAP], dst[BM_PATH_CAP];
    if (bm_config_dir(oldroot, src, sizeof(src)) != 0 ||
        bm_config_dir(newroot, dst, sizeof(dst)) != 0) {
        kprintf("[BINMIG] could not resolve the CONFIG directories for uid=%u; "
                "NOTHING MIGRATED\n", (unsigned)uid);
        return -1;
    }
    if (fat_exists(&g_fat_fs, src) != 1) return 0;  // the bootstrap session wrote nothing

    // STAMP ONLY WHAT WE CREATE. users_make_home_skeleton() owns the mode of
    // every home subdirectory and stamps CONFIG 0750; it runs just before this
    // on the account-creation path, so an unconditional perms_set here would
    // silently override the one place that decides, for one user only. There
    // is no functional difference on a single-user account (gid == uid), which
    // is precisely why such a divergence survives: it has no symptom until
    // somebody wonders why this user's CONFIG is stricter than everyone's.
    if (fat_exists(&g_fat_fs, dst) != 1) {
        if (fat_mkdir(&g_fat_fs, dst) != 0 && fat_exists(&g_fat_fs, dst) != 1) {
            kprintf("[BINMIG] could not create %s; NOTHING MIGRATED from %s\n", dst, src);
            bootlog_write("[BINMIG] FAILED: could not create '%s'", dst);
            return -1;
        }
        perms_on_create(dst, uid, u->gid, 1);
    }

    bm_stats_t st;
    st.moved = st.appended = st.skipped = st.failed = st.unlisted = 0;
    bm_walk(src, dst, 0, uid, u->gid, &st);

    // The bootstrap CONFIG directory itself, once empty. A failure is expected
    // and silent when something was deliberately left in it.
    (void)fat_delete(&g_fat_fs, src);

    // The Recycle Bin's ownership stamp is re-checked by the one function that
    // owns that decision, now that the bin has a new path and the session has
    // an account. Reused rather than repeated: it is idempotent, it is cheap,
    // and it is where the "who owns the bin" rule lives.
    recycle_session_changed();
    if (perms_sync() != 0)
        kprintf("[PERMS] sync failed after the bootstrap-state migration; "
                "the permissions are NOT on disk\n");

    int total = (int)(st.moved + st.appended);
    kprintf("[BINMIG] uid=%u: migrated %u moved + %u merged from %s to %s "
            "(skipped %u, failed %u, unlisted %u)\n",
            (unsigned)uid, st.moved, st.appended, src, dst,
            st.skipped, st.failed, st.unlisted);
    bootlog_write("[BINMIG] uid=%u moved=%u merged=%u skipped=%u failed=%u unlisted=%u '%s'->'%s'",
                  (unsigned)uid, st.moved, st.appended, st.skipped, st.failed,
                  st.unlisted, src, dst);

    if (st.failed || st.unlisted || st.skipped) return -1;
    return total;
}

void binmig_selftest(void) {
    uint32_t checks = 0;
    int32_t bad = binmig_selftest_rs(&checks);
    int64_t armed = binmig_armed_uid_rs();
    kprintf("[BINMIG] bootstrap-state migration policy selftest: %s "
            "(%u checks ran, %d failing); armed uid=%d (-1 = nothing armed: either "
            "this session has an account, or this ran before login)\n",
            bad == 0 ? "PASS" : "FAIL", checks, (int)bad, (int)armed);
    bootlog_write("[BINMIG] policy selftest %s ran=%u bad=%d armed=%d",
                  bad == 0 ? "PASS" : "FAIL", checks, (int)bad, (int)armed);
}
