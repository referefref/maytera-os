// recycle_fs.c - the FS + graph glue behind recycle_fs.h.
//
// C, NOT RUST, and here is the justification the CHANGELOG entry carries: every
// line of this file is entanglement. It calls the C FAT/ext2 primitives
// (fat_read_file / fat_write_file / fat_exists / fat_delete / fat_mkdir), the C
// per-user path resolver (userconf_kpath), the C permissions table (perms_set /
// perms_sync), the C process table (proc_current) and the C GraphFS fold, none
// of which has a Rust surface in this tree. The parts with an actual failure
// shape, the retention rule, the index-row encoding and the collision-safe
// trash name, ARE in Rust (rustkern/recycle.rs), together with the delete
// disposition and mark-inheritance policy (rustkern/escrowmark.rs). That is the
// same split fs/escrow_undo.c has against rustkern/escrowundo.rs and
// fs/graphfs/fold.c has against rustkern/gfsfold.rs.
//
// The 2026-10-03 volume-routing fix (rcy_victim_fs below) is C for the same
// reason and one more: all it does is choose WHICH fat_fs_t * to hand to the
// next C call. The judgement inside it, "does this path lie under this mount
// point", is ALREADY in Rust (hotplug_path_split_rs, reached through
// hotplug_volume_for_path); a Rust wrapper here would add an FFI hop around an
// opaque C pointer and express no policy of its own.
//
// NO WAITING, NO POLLING, NO SPINNING. Everything here is straight-line
// blocking filesystem I/O on the syscall path, which is already a may-block
// context, so wq_assert_may_block() is satisfied for free and concurrency-lint
// has nothing to see. The one loop is the trash-name collision bump, which is
// bounded by RCY_NAME_TRIES and makes real progress on every iteration (a
// different candidate name), not a condition poll.

#include "recycle_fs.h"
#include "fat.h"
#include "../drivers/hotplug.h"   // the victim may live on a removable volume
#include "perms.h"
#include "userconf.h"
#include "graphfs/fold.h"
#include "../proc/process.h"
#include "../proc/users.h"
#include "../gui/desktop.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"

extern fat_fs_t g_fat_fs;
extern void kprintf(const char *fmt, ...);
extern process_t *proc_current(void);
extern long long realtime_sec_rs(void);          // UNIX seconds, 0 = clock unknown
extern int  fat_get_attr_info(const char *path, uint8_t *attr_out, int *is_dir_out);
extern void seclog_notify_user(int severity, const char *title, const char *body);

// The pure policy (rustkern/recycle.rs, rustkern/escrowmark.rs).
extern unsigned long rcy_trash_name_rs(const unsigned char *orig_name,
                                       uint64_t now_unix, uint32_t uniq,
                                       unsigned char *out, unsigned long cap);
extern unsigned long rcy_row_build_rs(const unsigned char *name,
                                      const unsigned char *orig,
                                      uint64_t deleted_unix,
                                      uint32_t actor_kind, uint32_t actor_id,
                                      unsigned char *out, unsigned long cap);
extern uint32_t rcy_object_local_rs(const unsigned char *path);
extern int32_t  rcy_policy_selftest_rs(uint32_t *out_checks);
extern int32_t  escmark_policy_selftest_rs(uint32_t *out_checks);

#define RCY_BIN_LEAF    "RECYCLE"
#define RCY_INDEX_LEAF  "RBINDEX.TXT"

// How many candidate names the collision bump will try before giving up. Each
// try is a different name, so this is a bounded search, not a retry of the same
// failing thing. 64 distinct names for one (second, basename) pair is far more
// than a human or an AI will produce in one second.
#define RCY_NAME_TRIES  64

// Cap on the index file we are willing to read-modify-write. Rows are about 150
// bytes, so this is roughly 400 entries. Past it a capture is REFUSED rather
// than silently dropping a row, because a bin entry with no index row is an
// unrestorable orphan: the file is there and nothing knows where it came from.
// The Recycle Bin app must be able to read at least this much; see the matching
// note in userland/libc/rbindex.h.
#define RCY_INDEX_MAX   65536

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// "<session user's home>/CONFIG/RECYCLE". Returns 0 on success.
static int rcy_bin_dir(char *out, uint32_t cap) {
    return userconf_kpath(RCY_BIN_LEAF, out, cap);
}

static int rcy_index_path(char *out, uint32_t cap) {
    return userconf_kpath(RCY_INDEX_LEAF, out, cap);
}

// Join dir + "/" + name, refusing (rather than truncating) if it will not fit:
// a truncated path is a different file, and in this file a different file is
// either a clobber or a lost original.
static int rcy_join(char *out, uint32_t cap, const char *dir, const char *name) {
    uint32_t n = 0;
    for (const char *p = dir; *p; p++) { if (n + 1 >= cap) return -1; out[n++] = *p; }
    if (n == 0 || out[n - 1] != '/') { if (n + 1 >= cap) return -1; out[n++] = '/'; }
    for (const char *p = name; *p; p++) { if (n + 1 >= cap) return -1; out[n++] = *p; }
    out[n] = '\0';
    return 0;
}

static const char *rcy_basename(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

// Create the bin directory, and the "<home>/CONFIG" above it, if they are not
// there. Returns 0 if the directory exists afterwards.
// [noaccount] THE BIN BELONGS TO THE SESSION USER, AND THE STAMP HAS TO BE
// RE-CHECKED, NOT JUST APPLIED AT CREATION.
//
// This ran inside the create-only branch, so whatever uid the FIRST call
// happened to see was the owner forever. MEASURED on golden 2565, one boot of
// a machine with no account: PERMS.DB came back
//     /TMP/CONFIG/RECYCLE:0:0:0700
// while every file the desktop wrote beside it was 1000:1000:0644. The bin is
// created when escrow first asks "is there a usable bin", which happens before
// desktop_get_session_uid() has an answer, so it was stamped root-owned 0700
// and the early return above meant it was never looked at again. The result:
// the recycler daemon running as the session user was refused a plain READ of
// its own bin, which is the serial line
//     [FSDENY] REFUSED op=read proc=RECYCLED target=/TMP/CONFIG/RECYCLE
//              (object exists; its own mode/owner refuse r--)
//
// SAFE BECAUSE THE PATH ALREADY SEPARATES USERS. The bin is derived from THIS
// session's own state root, so two users have two different directories and
// this can never hand one user the other's bin; the only way the recorded
// owner differs from the session uid is that it was stamped before the session
// had one. Re-stamping is also not a privilege grant on the no-account path:
// /TMP is 01777, so that uid could have created the directory itself.
//
// IDEMPOTENT AND CHEAP. recycle_available() calls this on every escrow
// decision, so the common case must cost a table lookup and nothing else:
// perms_get() answers from the in-memory table, and neither perms_set() nor
// the flush runs unless the owner is actually wrong.
static void rcy_stamp_bin_owner(const char *dir) {
    uint32_t uid = desktop_get_session_uid();
    user_entry_t *u = user_lookup_uid(uid);
    // gid: the user's own group, or the uid itself when there is no user-table
    // entry yet. That matches what perms_on_create() stamps on everything else
    // the same session writes (measured: 1000:1000), rather than leaving a
    // uid-1000 directory in group 0.
    uint32_t gid = u ? u->gid : uid;

    uint32_t ouid = 0, ogid = 0; uint16_t omode = 0;
    if (perms_get(dir, &ouid, &ogid, &omode) == 0 &&
        ouid == uid && ogid == gid && (omode & 0777) == 0700)
        return;                      // already right: no write, no flush

    perms_set(dir, uid, gid, 0700);
    IGNORE_RESULT("a perms flush failure is reported by perms_sync itself; it must not "
                  "turn a recoverable delete into a refused one", perms_sync());
}

static int rcy_ensure_bin(char *dir_out, uint32_t cap) {
    if (!g_fat_fs.mounted) return -1;
    if (rcy_bin_dir(dir_out, cap) != 0) return -1;

    if (!fat_exists(&g_fat_fs, dir_out)) {
        // The parent is "<home>/CONFIG": the same join minus the leaf. Build it
        // by truncating at the last '/' rather than re-deriving it, so the two
        // can never disagree about where the bin lives.
        char parent[256];
        uint32_t n = 0;
        while (dir_out[n] && n + 1 < sizeof(parent)) { parent[n] = dir_out[n]; n++; }
        parent[n] = '\0';
        while (n > 1 && parent[n - 1] != '/') n--;
        if (n > 1) { parent[n - 1] = '\0'; if (!fat_exists(&g_fat_fs, parent)) (void)fat_mkdir(&g_fat_fs, parent); }

        if (fat_mkdir(&g_fat_fs, dir_out) != 0 && !fat_exists(&g_fat_fs, dir_out)) return -1;
    }

    // [noaccount] OUTSIDE the create branch: see rcy_stamp_bin_owner().
    rcy_stamp_bin_owner(dir_out);
    return 0;
}

// [noaccount] THE STAMP NEEDED A TRIGGER, NOT JUST A SITE.
//
// Moving rcy_stamp_bin_owner() out of the create-only branch was necessary and
// NOT sufficient, and the boot that proved it is worth recording: golden 2566
// still came back with
//     /TMP/CONFIG/RECYCLE:0:0:0700
// because rcy_ensure_bin() is reached ONLY from recycle_available(), and the
// only things that call recycle_available() are the escrow decision paths and
// the gated boot self-test. On a machine nobody has touched, the boot
// self-test is the single caller, it runs BEFORE login_run(), and
// desktop_get_session_uid() is still 0 at that point. Nothing asked again, so
// nothing corrected it, and the recycler daemon was still refused a read of
// its own bin.
//
// So the moment the session identity BECOMES KNOWN is the moment to re-check,
// and main.c already does exactly this for the user's home directory three
// lines earlier (perms_get, then perms_set only if it differs). This is the
// same idiom for the same reason.
//
// IT DOES NOT CREATE THE BIN. A login should not manufacture a Recycle Bin for
// a session that may never delete anything; it should only correct one that is
// already there and wrongly owned. A session that does delete something
// creates the bin through recycle_available() with its uid already set.
void recycle_session_changed(void) {
    char dir[256];
    if (!g_fat_fs.mounted) return;
    if (rcy_bin_dir(dir, sizeof(dir)) != 0) return;
    if (!fat_exists(&g_fat_fs, dir)) return;   // nothing to correct
    rcy_stamp_bin_owner(dir);
}

int recycle_available(void) {
    char dir[256];
    return rcy_ensure_bin(dir, sizeof(dir)) == 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// WHICH VOLUME IS THE VICTIM ON
// ---------------------------------------------------------------------------
//
// THE BUG THIS EXISTS FOR, measured on 2026-10-03 (#246 Stage 4, ESCROW4T).
// Everything below used to ask g_fat_fs about the FULL path. g_fat_fs routes
// "/" to the ext2 root, /SMB, /NFS and a mounted disk image's drive letter, but
// it does NOT route a hot-plugged removable volume's mount point: /USB0/... is
// resolved by the fd layer against that volume's OWN fat_fs_t, which is why an
// open() of /USB0/e4.txt works. So fat_exists(&g_fat_fs, "/USB0/e4.txt") was
// false, the capture returned "could not read the victim's bytes", and the
// guard did the right thing with the wrong information: it FAILED CLOSED and
// refused the delete. Nothing was ever destroyed, so this was never a data
// loss; it meant a marked AI actor could not delete anything at all on a USB
// stick, which is exactly the owner's thumbdrive scenario.
//
// Returns the fat_fs_t that serves `path` and sets *rel to the path to use with
// it, or NULL if nothing here can reach it (RCY_E_NOVOL territory).
// [renamevol] 2026-10-04: THIS BODY MOVED TO drivers/hotplug.c AS
// hotplug_fs_or_root(). It was the only correct root-or-volume resolver in the
// tree, and it was file-static, so proc/syscall.c could not use it and
// performed every mkdir, rmdir, unlink and rename against g_fat_fs with the
// full path - which is why none of the four worked on a mounted USB stick. The
// rule now has one definition, next to the hotplug resolver it is built on, and
// this call keeps the name the rest of this file reads by.
//
// The behaviour is unchanged, deliberately: refuse on a mounted-but-unreadable
// volume (falling through to the root would ask the WRONG filesystem about this
// path and STEP 3 would then unlink whatever the root happens to have at that
// name), refuse with no mounted root, otherwise the root with the path
// untouched.
static fat_fs_t *rcy_victim_fs(const char *path, const char **rel) {
    return hotplug_fs_or_root(path, rel);
}

// ---------------------------------------------------------------------------
// WHAT IS AT THIS PATH (see the contract on the declaration in recycle_fs.h)
//
// Built on rcy_victim_fs() above, so it answers for the ext2 root, the FAT ESP,
// a mounted disk image, /SMB, /NFS and a hot-plugged removable volume by the
// SAME rule the capture itself uses. If the two could disagree, one of them
// would be filing a user's file in the wrong place or missing it entirely.
// ---------------------------------------------------------------------------
int recycle_victim_stat(const char *path, uint32_t *len_out, int *isdir_out) {
    if (len_out) *len_out = 0;
    if (isdir_out) *isdir_out = 0;
    if (!path || path[0] != '/') return 0;

    int matched = 0;
    const char *vrel = 0;
    fat_fs_t *vfs = hotplug_volume_for_path(path, &vrel, &matched);
    if (matched && !vfs) return -1;   // mounted, cannot serve files: UNREACHABLE

    if (vfs) {
        // A REMOVABLE VOLUME, asked through ITS OWN fat_fs_t and ITS OWN
        // relative path. fat_open() is used rather than fat_exists() because it
        // answers all three questions (present, directory, length) in one
        // directory read, and because fat_get_attr_info() is hard-wired to
        // g_fat_fs and so cannot see this volume at all.
        fat_file_t f;
        if (fat_open(vfs, vrel, &f) != 0) return 0;
        int isdir = (f.is_dir || (f.attr & FAT_ATTR_DIRECTORY)) ? 1 : 0;
        if (isdir_out) *isdir_out = isdir;
        if (len_out) *len_out = isdir ? 0u : f.file_size;
        fat_close(&f);
        return 1;
    }

    // THE ROOT FAMILY, asked with exactly the calls the four routes already
    // made, in the same order, so this change cannot alter a single one of the
    // root answers those routes were already proven on. fat_exists() and
    // fat_read_file() both dispatch to the ext2 root, the FAT ESP and a mounted
    // disk image; fat_get_attr_info() answers the directory question.
    if (!g_fat_fs.mounted) return 0;
    if (fat_exists(&g_fat_fs, path) != 1) return 0;
    {
        uint8_t attr = 0; int d = 0;
        extern int fat_get_attr_info(const char *pp, uint8_t *attr_out, int *is_dir_out);
        if (fat_get_attr_info(path, &attr, &d) == 0 && d) {
            if (isdir_out) *isdir_out = 1;
            return 1;                      // *len_out stays 0 for a directory
        }
    }
    if (len_out) {
        uint32_t sz = 0;
        void *dat = fat_read_file(&g_fat_fs, path, &sz);
        if (dat) kfree(dat);
        *len_out = sz;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// The graph record: who touched what
// ---------------------------------------------------------------------------
//
// The journal record for the DECISION itself is written by escrow_guard.c, next
// to every other enforcement decision, so there is one place that records what
// the guard did. What is added HERE is the RELATIONSHIP, which is the part the
// owner asked for: an OBJECT identity for the file and a DERIVED_FROM edge from
// the AI task to it. With those, gfs_edges_get() answers "what did this agent
// touch in this session" and "which agents touched this file", which is what
// "undo this agent's work because it broke something" actually needs. A single
// per-file journal line cannot answer either question without a scan.
//
// Best effort by design: a graph that is full, degraded or not yet up must not
// turn a recoverable delete into a refused one. The bytes are already safe in
// the bin and the index row is the restore authority; the graph is the richer
// record on top of it, not the thing standing between the user and their file.
static void rcy_record_relationship(const char *path, uint32_t actor_kind,
                                    uint32_t actor_id) {
    if (actor_kind != RCY_ACTOR_AI || actor_id == 0) return;
    if (!gfs_fold_ready()) return;

    uint32_t obj = GFS_NODE_ID(GFS_KIND_OBJECT, rcy_object_local_rs((const unsigned char *)path));
    // A duplicate is the normal case (the same file touched twice), and the
    // fold reports it as a refusal rather than an error. Either way the node
    // exists afterwards, which is all the edge needs.
    (void)gfs_node_create(obj, GFS_KIND_OBJECT, 0);
    (void)gfs_relate(actor_id, obj, GFS_REL_DERIVED_FROM);
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

// The shared body. `remove_original` distinguishes a CAPTURE (the file is going
// away, which is what a delete means) from a SNAPSHOT (the file stays and only
// a copy of its current contents is kept). One function, because two copies of
// a read-write-index-remove sequence is how one of them comes to skip a step.
// #246 partialwrite: `pre`/`prelen`, when `pre` is non-NULL, ARE the bytes to
// keep, and the victim is NOT read. The caller that uses this is the ext2
// preserving descriptor, whose open already filled a whole-file buffer from the
// file and still holds the PRISTINE contents at the moment of the first
// overwriting write. Re-reading the file there would be a second read of bytes
// already in kernel memory, so the in-memory form is strictly cheaper and is
// also the only form that is correct once the buffer has been modified. Every
// other step (the bin, the unique name, the index row, the perms, the
// notification, the graph edge) is byte-for-byte the same, which is the whole
// reason this is a parameter and not a second function body.
static int recycle_do(const char *path, uint32_t actor_kind, uint32_t actor_id,
                      int remove_original, const void *pre, uint32_t prelen) {
    if (!path || !path[0]) return RCY_E_NOFS;
    // The BIN is always on the root volume, whatever volume the victim is on,
    // so the root must be mounted even for a capture off a USB stick.
    if (!g_fat_fs.mounted) return RCY_E_NOFS;

    char dir[256];
    if (rcy_ensure_bin(dir, sizeof(dir)) != 0) return RCY_E_NOBIN;

    // The VICTIM, however, may be on a removable volume. Every operation on it
    // below uses (vfs, vrel); everything about the bin keeps using g_fat_fs.
    const char *vrel = path;
    fat_fs_t *vfs = rcy_victim_fs(path, &vrel);
    if (!vfs) return RCY_E_NOVOL;

    if (!fat_exists(vfs, vrel)) return RCY_E_NOENT;

    // Exception 1: directories are not recycled. The caller's policy already
    // refuses them, but asking the filesystem DIRECTLY here means the invariant
    // holds even if some future caller forgets, and it gives a distinct error
    // instead of the "could not read" a directory would otherwise produce.
    //
    // fat_get_attr_info() takes no fat_fs_t and only ever answers for the root,
    // so on a removable volume the question has to be asked of that volume's
    // own handle. fat_open() reports is_dir for both backends and fat_close()
    // on a directory handle is the same no-op it is anywhere else.
    {
        if (vfs == &g_fat_fs) {
            uint8_t attr = 0; int isdir = 0;
            if (fat_get_attr_info(path, &attr, &isdir) == 0 && isdir) return RCY_E_NOTFILE;
        } else {
            fat_file_t probe;
            if (fat_open(vfs, vrel, &probe) == 0) {
                int isdir = probe.is_dir || (probe.attr & FAT_ATTR_DIRECTORY);
                fat_close(&probe);
                if (isdir) return RCY_E_NOTFILE;
            }
        }
    }

    // THE READ, unless the caller already handed us the bytes. `owned` says
    // whether `data` came from kmalloc and must be freed; a caller-supplied
    // pre-image belongs to the caller and must not be.
    uint32_t sz = 0;
    void *data = 0;
    int owned = 0;
    if (pre) {
        data = (void *)pre;
        sz = prelen;
    } else {
        data = fat_read_file(vfs, vrel, &sz);
        owned = 1;
    }

    // AN EMPTY FILE IS STILL A FILE, and this is the bug that would otherwise
    // have been here. fat_read_file() returns NULL for a zero-byte file, which
    // is indistinguishable from a read failure by its return value alone, so
    // the naive version refused to recycle an empty file and therefore REFUSED
    // THE DELETE: an AI could delete every file on the system except an empty
    // one, for no reason a user could possibly guess. We already know the path
    // exists and is not a directory, so a NULL read with a reported size of 0
    // is the empty-file case, and it is captured as a zero-byte bin copy.
    int empty = 0;
    if (!data || sz == 0) {
        if (sz == 0) {
            empty = 1;
        } else {
            // A genuine read failure on a non-empty file. Cannot capture, so
            // the caller must refuse the delete.
            return RCY_E_READ;
        }
    }

    uint64_t now = 0;
    { long long t = realtime_sec_rs(); if (t > 0) now = (uint64_t)t; }

    // Pick a free name inside the bin. Bounded, and every iteration tries a
    // genuinely different candidate.
    unsigned char tname[32];
    char tpath[320];
    int placed = 0;
    for (uint32_t uniq = 1; uniq <= RCY_NAME_TRIES; uniq++) {
        if (rcy_trash_name_rs((const unsigned char *)rcy_basename(path), now, uniq,
                              tname, sizeof(tname)) == 0) break;
        if (rcy_join(tpath, sizeof(tpath), dir, (const char *)tname) != 0) break;
        if (!fat_exists(&g_fat_fs, tpath)) { placed = 1; break; }
    }
    if (!placed) { if (owned && data) kfree(data); return RCY_E_WRITE; }

    // STEP 1: the copy. Written and VERIFIED before anything is removed.
    {
        // An EMPTY file is written as a zero-length file, which is what it is.
        // There is no truncate-by-path primitive in this FS layer
        // (fat_truncate_to takes an open fat_file_t), and faking it by writing
        // one byte would hand the user a "restored" file that differs from
        // their original, so a zero-length write is the only honest option. If
        // the backend refuses it, the capture fails and the DELETE IS REFUSED,
        // which is the right direction: better that an AI cannot delete an
        // empty file than that it deletes one unrecoverably. /APPS/ESCRVT has
        // an arm for exactly this case, so the answer is measured on a real
        // boot rather than assumed here.
        int wrc = fat_write_file(&g_fat_fs, tpath, empty ? "" : data, empty ? 0u : sz);
        if (owned && data) kfree(data);
        if (wrc != 0) {
            if (empty)
                kprintf("[RECYCLE] could not place a ZERO-LENGTH copy of %s in "
                        "the bin; the delete is REFUSED rather than made "
                        "unrecoverable\n", path);
            return RCY_E_WRITE;
        }
    }
    if (!fat_exists(&g_fat_fs, tpath)) return RCY_E_WRITE;

    {
        uint32_t uid = desktop_get_session_uid();
        user_entry_t *u = user_lookup_uid(uid);
        perms_set(tpath, uid, u ? u->gid : 0, 0600);
        IGNORE_RESULT("see above: the bytes are already safe; a perms flush failure is "
                      "logged by perms_sync and does not change the outcome", perms_sync());
    }

    // STEP 2: the index row. Without it the copy is an unrestorable orphan, so
    // a failure here UNDOES the copy and refuses the delete: better to refuse
    // than to leave the user a file in the bin that cannot go home.
    {
        char ipath[256];
        if (rcy_index_path(ipath, sizeof(ipath)) != 0) { (void)fat_delete(&g_fat_fs, tpath); return RCY_E_INDEX; }

        uint32_t isz = 0;
        void *cur = fat_read_file(&g_fat_fs, ipath, &isz);
        if (cur && isz > RCY_INDEX_MAX) { kfree(cur); (void)fat_delete(&g_fat_fs, tpath); return RCY_E_INDEX; }

        unsigned char row[320];
        unsigned long rlen = rcy_row_build_rs(tname, (const unsigned char *)path,
                                              now, actor_kind, actor_id,
                                              row, sizeof(row));
        if (rlen == 0) { if (cur) kfree(cur); (void)fat_delete(&g_fat_fs, tpath); return RCY_E_INDEX; }

        uint32_t total = isz + (uint32_t)rlen;
        char *buf = (char *)kmalloc(total ? total : 1);
        if (!buf) { if (cur) kfree(cur); (void)fat_delete(&g_fat_fs, tpath); return RCY_E_INDEX; }
        if (cur && isz) memcpy(buf, cur, isz);
        memcpy(buf + isz, row, rlen);
        if (cur) kfree(cur);

        int wrc = fat_write_file(&g_fat_fs, ipath, buf, total);
        kfree(buf);
        if (wrc != 0) { (void)fat_delete(&g_fat_fs, tpath); return RCY_E_INDEX; }

        uint32_t uid = desktop_get_session_uid();
        user_entry_t *u = user_lookup_uid(uid);
        perms_set(ipath, uid, u ? u->gid : 0, 0600);
        IGNORE_RESULT("see above", perms_sync());
    }

    // STEP 3: and only now does the original go away, IF it is going away at
    // all. If this fails the user has the file twice, which is untidy and is
    // not a loss. For a SNAPSHOT the original is deliberately kept.
    if (remove_original && fat_delete(vfs, vrel) != 0) return RCY_E_UNLINK;

    // The relationship, and the announcement. Neither can fail the capture: the
    // file is already safe and already listed.
    rcy_record_relationship(path, actor_kind, actor_id);

    if (actor_kind == RCY_ACTOR_AI) {
        // ANNOUNCED, as the owner required, and not buried in a log: this is the
        // same per-session notification spool the compositor drains and toasts,
        // the one libc/notify.c and security/seclog.c already write to.
        char body[224];
        int b = 0;
        const char *pre = remove_original ? "AI moved to Recycle Bin: "
                                         : "AI overwrote, old copy in Recycle Bin: ";
        for (const char *c = pre; *c && b < (int)sizeof(body) - 1; c++) body[b++] = *c;
        for (const char *c = path; *c && b < (int)sizeof(body) - 1; c++) body[b++] = *c;
        body[b] = '\0';
        seclog_notify_user(2 /* NOTIFY_WARNING */,
                           remove_original ? "File deleted by AI"
                                           : "File overwritten by AI", body);
    }

    kprintf("[RECYCLE] %s %s -> %s (actor=%s %08x, t=%llu) bytes kept\n",
            remove_original ? "captured (DELETE REDIRECTED)" : "snapshotted (PRE-IMAGE)",
            path, tpath, actor_kind == RCY_ACTOR_AI ? "ai" : "user",
            (unsigned)actor_id, (unsigned long long)now);
    return RCY_OK;
}

int recycle_capture(const char *path, uint32_t actor_kind, uint32_t actor_id) {
    return recycle_do(path, actor_kind, actor_id, 1 /* the file is going away */,
                      0, 0);
}

int recycle_snapshot(const char *path, uint32_t actor_kind, uint32_t actor_id) {
    return recycle_do(path, actor_kind, actor_id, 0 /* keep the original */, 0, 0);
}

int recycle_snapshot_bytes(const char *path, const void *pre, uint32_t prelen,
                           uint32_t actor_kind, uint32_t actor_id) {
    if (!pre) return RCY_E_READ;   // the caller said it had the bytes and did not
    return recycle_do(path, actor_kind, actor_id, 0 /* keep the original */,
                      pre, prelen);
}

// [binmigrate] See recycle_fs.h. The leaf, not a path: the DIRECTORY it lives
// in is whatever userconf_kpath() says, and that is the whole point of this
// being a leaf.
const char *recycle_index_leaf(void) { return RCY_INDEX_LEAF; }

const char *recycle_strresult(int rc) {
    switch (rc) {
        case RCY_OK:         return "captured";
        case RCY_E_NOFS:     return "no filesystem";
        case RCY_E_NOBIN:    return "no usable Recycle Bin";
        case RCY_E_NOTFILE:  return "not a plain file";
        case RCY_E_READ:     return "the file's bytes could not be read";
        case RCY_E_WRITE:    return "the bin copy could not be written";
        case RCY_E_INDEX:    return "the bin index row could not be written";
        case RCY_E_UNLINK:   return "the original would not go away";
        case RCY_E_NOENT:    return "no such file";
        case RCY_E_NOVOL:    return "on a volume the Recycle Bin cannot reach";
        default:             return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
//
// ALWAYS RUNS, including on a production golden. The project has found, on one
// day, seven checks that existed and never executed, and four escrow verifiers
// gated behind a marker file that nothing creates. A policy self-test that
// costs microseconds and prints one line is the cheapest possible defence
// against this one joining them: if the line is absent from a boot log, the
// code is not there.
void recycle_selftest(void) {
    uint32_t c1 = 0, c2 = 0;
    int32_t r1 = rcy_policy_selftest_rs(&c1);
    int32_t r2 = escmark_policy_selftest_rs(&c2);
    int avail = recycle_available();
    kprintf("[RECYCLE] selftest retention/rows/names %s (%u checks, rc=%d); "
            "mark inherit/exit/disposition %s (%u checks, rc=%d); bin %s\n",
            r1 == 0 ? "PASS" : "FAIL", (unsigned)c1, (int)r1,
            r2 == 0 ? "PASS" : "FAIL", (unsigned)c2, (int)r2,
            avail ? "available" : "UNAVAILABLE (a marked actor's delete will be REFUSED)");
}
