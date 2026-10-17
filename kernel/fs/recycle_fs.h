// recycle_fs.h - the KERNEL side of "an AI can delete your files, but it can
// never destroy them".
//
// THE OWNER'S REQUIREMENT, verbatim (2026-10-03): "AI can delete files but it
// must be announced to the user AND they can be undeleted by AI or the user if
// they chose so, or if this change has a negative impact. A recycler service
// should auto delete these periodically with a controllable timing in the
// settings app, also with an option for never but defaulting to 30 days".
//
// So this is NOT a refusal. A delete by an AI actor SUCCEEDS from the caller's
// point of view. What the kernel changes is where the bytes go: into the user's
// existing Recycle Bin, with an index row saying when it happened and who did
// it, a record in the tamper-evident GraphFS journal, an edge in the graph
// joining the AI task to the file, and a notification on the desktop.
//
// WHY THIS IS IN THE KERNEL AND NOT IN THE AI CLIENT. Because the guarantee has
// to hold against code that is NOT cooperating. The userland AI tool layer
// (userland/libc/aicap.c) can be bypassed by any Ring-3 process that issues raw
// syscalls, including one running as uid 0, which is exactly how the shipped
// AI assistant runs. The redirect therefore lives at the FS-mutation syscall
// chokepoint (fs/escrow_guard.c escrow_fs_guard), which every delete reaches,
// so there is no path from Ring 3 to a hard unlink for a marked actor.
//
// WHAT IS BINDING, STATED PRECISELY. For a process carrying the escrow mark:
//   - unlink of a FILE          -> captured into the bin, announced, success
//   - unlink with no usable bin -> REFUSED (fail closed; never destroy)
//   - rmdir of a DIRECTORY      -> REFUSED (named exception, see below)
//   - clearing its own mark     -> refused for a LOCKED actor
//   - spawning a helper         -> the helper inherits the mark, LOCKED
//
// THE NAMED EXCEPTIONS, written down rather than discovered later:
//   1. DIRECTORIES ARE NOT RECYCLED. The bin is a flat store of files with one
//      index row each; it has no representation for a tree. A marked actor's
//      rmdir is refused instead. In practice this refuses only EMPTY
//      directories, because ext2's rmdir already refuses a non-empty one.
//   2. TRUNCATION AND OVERWRITE ARE NOT CAPTURED. A marked actor holding a
//      live in-scope WRITE grant may open an existing in-scope file for writing
//      and shorten or replace its contents, and no pre-image is kept. That is
//      destruction of data without a delete, and this slice does not close it.
//      docs/ESCROW_ROLLBACK_SEMANTICS.md already classes OVERWRITE as
//      not-reversible for the same reason (it needs a pre-image snapshot). The
//      capture machinery below is the mechanism that snapshot would reuse; the
//      hook is deliberately not wired, because snapshotting on every write-open
//      has a cost that has not been measured. Do not describe the guarantee as
//      covering overwrite.
//   4. THE CAPTURE REACHES THE ROOT VOLUME AND MOUNTED REMOVABLE VOLUMES, AND
//      NOT /MNT AUX MOUNTS. The bin itself always lives in the session user's
//      home on the ROOT volume, so capturing a file off a USB stick copies it
//      across volumes on purpose: the bin is the one place the user already
//      knows to look. A path under a /MNT/<name> aux mount (fs/blkmgr_mnt.c,
//      whole-file buffered, no fat_fs_t handle) is NOT reachable from here and
//      a marked actor's delete there is REFUSED with RCY_E_NOVOL, which is
//      fail-closed and correct but is a gap, not a design. It is named here so
//      the next person finds it written down instead of measuring it.
//   3. A process with NO escrow mark is untouched. escrow_fs_guard() returns
//      "allow" after reading one process_t byte, so the ordinary FS path for
//      the shell, the compositor, Files and the installer is byte-for-byte what
//      it was before this file existed. The mark is what makes a process an AI
//      actor, and only the kernel can set it.
#ifndef FS_RECYCLE_FS_H
#define FS_RECYCLE_FS_H

#include "../types.h"

// Who performed the delete. MIRRORED in rustkern/recycle.rs as RCY_ACTOR_*.
#define RCY_ACTOR_USER  0u
#define RCY_ACTOR_AI    1u

// recycle_capture() results.
#define RCY_OK           0   // captured: the caller must NOT unlink, and must
                             // report SUCCESS to Ring 3
#define RCY_E_NOFS      (-1) // no filesystem
#define RCY_E_NOBIN     (-2) // the bin could not be resolved or created
#define RCY_E_NOTFILE   (-3) // not a plain file (a directory: see exception 1)
#define RCY_E_READ      (-4) // could not read the victim's bytes
#define RCY_E_WRITE     (-5) // could not place the copy in the bin
#define RCY_E_INDEX     (-6) // the copy landed but its index row did not
#define RCY_E_UNLINK    (-7) // the copy and the row landed but the original
                             // would not go away (a duplicate, never a loss)
#define RCY_E_NOENT     (-8) // there is no such file: nothing to capture. SPLIT
                             // OUT OF RCY_E_READ, which reported it as "could
                             // not read the victim's bytes" and sent whoever
                             // read the serial line looking for an I/O fault
                             // when the file simply was not there.
#define RCY_E_NOVOL     (-9) // the path is on a volume this capture cannot
                             // reach: a mounted-but-unreadable removable
                             // volume (exFAT), or an aux /MNT mount. See
                             // exception 4 below. Also split out of
                             // RCY_E_READ, which is what made the #246 Stage 4
                             // red look like an I/O failure for an hour.

// Is the Recycle Bin usable RIGHT NOW for `uid`'s session: resolvable, and the
// directory exists or can be created? The delete-disposition policy in
// rustkern/escrowmark.rs takes this as its `recoverable` input, and a 0 here
// makes a marked actor's delete REFUSED rather than destructive.
int recycle_available(void);

// [noaccount] Call once the session uid is known (main.c, right after
// desktop_set_session). The Recycle Bin directory can be created by the escrow
// layer BEFORE login, when desktop_get_session_uid() is still 0, and it was
// then stamped root-owned 0700 and never looked at again, so the recycler
// daemon running as the session user could not read its own bin. MEASURED on
// golden 2566: /TMP/CONFIG/RECYCLE:0:0:0700 beside 1000:1000:0644 for every
// file the desktop wrote next to it.
//
// Corrects an EXISTING bin's ownership; never creates one. Cheap: an in-memory
// perms table lookup, with no write and no flush unless the owner is wrong.
void recycle_session_changed(void);

// Capture `path` into the session user's Recycle Bin instead of deleting it.
//
// ORDER OF OPERATIONS, and it is load-bearing: the copy is written and verified
// FIRST, then the index row, then the original is removed. Every crash window
// therefore leaves either the original or a restorable copy, and usually both.
// The reverse order (unlink first) is how a Recycle Bin loses files.
//
// `actor_kind` is RCY_ACTOR_* and `actor_id` is the AI task node id for an AI
// delete (0 otherwise); together they are what the index row and the GraphFS
// records attribute the deletion to.
//
// Blocking: filesystem I/O. Process (syscall) context only, exactly like
// gfs_journal_append(). Returns RCY_OK or a negative RCY_E_*.
int recycle_capture(const char *path, uint32_t actor_kind, uint32_t actor_id);

// The same capture, but the ORIGINAL IS LEFT IN PLACE. A copy of the current
// contents goes into the bin with its index row, and the file at `path` is
// untouched.
//
// WHY IT EXISTS. An `open(path, O_WRONLY|O_TRUNC)` with NO O_CREAT destroys
// every byte in an existing file and expects the file to still be there
// afterwards. recycle_capture() would move it away and the open would then fail
// with ENOENT, turning a destructive operation into a confusing one. This keeps
// the pre-image AND the file, which is what a pre-image snapshot means.
//
// Blocking: filesystem I/O. Process context only. RCY_OK or a negative RCY_E_*.
int recycle_snapshot(const char *path, uint32_t actor_kind, uint32_t actor_id);

// #246 partialwrite: a SNAPSHOT whose bytes the CALLER already holds, so the
// victim is not read at all. `pre`/`prelen` are the pre-image; everything else
// (the bin, the unique name, the index row, the perms, the user notification,
// the GraphFS edge) is identical to recycle_snapshot().
//
// WHY IT EXISTS. The ext2 preserving descriptor loads the WHOLE FILE into a
// kernel buffer at open and writes it back at close, so at the moment of the
// first write that lands on existing bytes, that buffer still holds the
// PRISTINE original. Keeping the pre-image from it costs no read; asking
// recycle_snapshot() to re-read the file would read bytes already in memory,
// and after the buffer has been modified it would be too late to read them at
// all. A zero-length `prelen` is a zero-byte file and is kept as one.
//
// The bytes belong to the CALLER and are not freed here.
//
// Blocking: filesystem I/O. Process context only. RCY_OK or a negative RCY_E_*.
int recycle_snapshot_bytes(const char *path, const void *pre, uint32_t prelen,
                           uint32_t actor_kind, uint32_t actor_id);

// A short human reason for an RCY_* result, for the one serial line a refusal
// prints. "rc=-4" sent a reader of that line hunting an I/O fault when the
// actual cause was a path on a volume the capture could not reach; a number
// that needs a header open beside it is not a diagnostic.
const char *recycle_strresult(int rc);

// [binmigrate] THE Recycle Bin index leaf name, as ONE definition.
//
// fs/binmigrate.c has to know which file in a migrated CONFIG directory is an
// append-only ROW LOG whose two halves may be concatenated, as opposed to a
// settings file where a collision must be left alone. That is exactly "is this
// the bin index", and a second spelling of the literal over there would be a
// second place to get it wrong. Asking is free; repeating is not.
const char *recycle_index_leaf(void);

// WHAT IS AT THIS PATH, ON WHICHEVER VOLUME ACTUALLY SERVES IT.
//
// Returns  1  a file or directory is there (*len_out and *isdir_out are filled)
//          0  nothing is there, on a volume that WAS readable
//         -1  the path matched a mounted removable volume that cannot serve
//             files (an exFAT stick), so NOTHING here can read it. A caller
//             about to destroy bytes must treat this as a REFUSAL, never as
//             "absent": that is the difference between failing closed and
//             failing open, and it is the bug this function was added for.
//
// WHY IT LIVES HERE AND NOT IN escrow_guard.c. This file already owns the one
// answer to "which filesystem serves this path" (rcy_victim_fs, added 2026-10-03
// for the delete route after the same g_fat_fs blindness refused every delete on
// a USB stick). The four non-delete routes needed the same answer and would
// otherwise have grown a second copy of it, and two copies of that question is
// how one of them comes to disagree with the other about where a user's file is.
// It is C, not Rust, because it is pure filesystem plumbing with no decision in
// it: the DECISION it feeds is escmark_victim_disposition_rs(), which is Rust.
//
// *len_out is 0 for a directory. Blocking (it opens the file to read its
// length); process context only, which every caller is.
int recycle_victim_stat(const char *path, uint32_t *len_out, int *isdir_out);

// Boot self-test: the pure Rust policies (retention, row format, trash naming,
// mark inheritance, delete disposition) plus the bin-availability probe.
// Prints one [RECYCLE] line. Always runs, including on a production golden: a
// self-test nobody has seen is indistinguishable from one that is not wired in.
void recycle_selftest(void);

#endif // FS_RECYCLE_FS_H
