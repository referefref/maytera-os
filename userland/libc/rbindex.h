// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// rbindex.h - THE ONE definition of where the Recycle Bin lives and what a row
// in its index means.
//
// WHY THIS FILE EXISTS. Before it there were TWO byte-for-byte copies of the
// index parser, in userland/apps/recyclebin/main.c and userland/apps/files/
// main.c, and a third was about to be written for the recycler daemon. The
// comment at the top of each copy already said the two must never disagree
// about where the bin is, because Files WRITES the store that the Recycle Bin
// READS: if they diverged, "Delete" would appear to work and the bin would
// appear empty. A shared resolver that cannot disagree is the only form of that
// guarantee that survives an edit to one copy. CLAUDE.md's rule is to improve
// the shared primitive, never to fork it.
//
// THE ROW FORMAT, and why it grew backward-compatibly in BOTH directions:
//
//   V1 (what shipped):  <trashname>|<original path>
//   V2 (this change):   <trashname>|<original path>|<deleted_unix>|<actor>
//
//   - A V1 row read by the V2 parser yields deleted_unix = 0 and actor "user".
//     A 0 timestamp means AGE UNKNOWN and is NEVER purged, so an existing bin
//     on an installed system is safe rather than silently eligible for
//     deletion. The kernel's rcy_is_expired_rs() resolves the same way.
//   - A V2 row read by a V1 parser is what forced the field ORDER. The shipped
//     V1 readers copied from the first '|' to END OF LINE as the original path,
//     so appending fields would have corrupted every restore. The parser here
//     stops the path at the next '|' or at end of line, which leaves a V1 row
//     parsing identically and makes a V2 row safe.
//
// `actor` is a short token, never free text:
//     "user"          a human acting through Files or the Recycle Bin
//     "ai:XXXXXXXX"   an AI actor, XXXXXXXX being its AI_TASK node id in hex,
//                     which is the join key to the GraphFS journal entry for
//                     the same delete.
//
// The retention half mirrors kernel/rustkern/recycle.rs byte for byte: the
// RECYCLE.CFG body this writes is exactly what rcy_parse_retention_rs() reads,
// and every ambiguity (absent file, typo, missing value) resolves to KEEPING
// the file, because the failure mode of keeping too long is disk space and the
// failure mode of purging too early is the thing the Recycle Bin exists to
// prevent.
#ifndef _RBINDEX_H
#define _RBINDEX_H

#define RB_NAME_MAX   64
#define RB_PATH_MAX   192
#define RB_ACTOR_MAX  16
#define RB_ROW_MAX    320
#define RB_MAX_ROWS   256

// One parsed index row.
typedef struct {
    char name[RB_NAME_MAX];           // the file's name INSIDE the bin
    char original_path[RB_PATH_MAX];  // where it came from; "" if the row is broken
    unsigned long deleted_unix;       // 0 = age unknown (a V1 row); never purge
    char actor[RB_ACTOR_MAX];         // "user" or "ai:xxxxxxxx"
} rb_row_t;

// ---------------------------------------------------------------------------
// Where the bin is
// ---------------------------------------------------------------------------
//
// By default these answer for the CALLING process's own uid, which is exactly
// what the previous per-app resolvers did and is the right question for an app
// the user launched. A service that runs as uid 0 on behalf of a session must
// call rbindex_use_session_home() FIRST (see below), because its own answer
// would be "/" forever while the bin it has to sweep belongs to the session
// user.
const char *rbindex_dir(void);        // "<home>/CONFIG/RECYCLE"
const char *rbindex_path(void);       // "<home>/CONFIG/RBINDEX.TXT"
const char *rbindex_cfg_path(void);   // "<home>/CONFIG/RECYCLE.CFG"

// Resolve against the ACTIVE SESSION's home instead of the caller's own.
// Must be called before the first rbindex_dir()/rbindex_path() call, because
// those cache. Deliberately opt-in: switching every caller to the session view
// would change where a root-uid app in an admin session looks, which would
// "lose" an existing root bin.
void rbindex_use_session_home(void);

// "<dir>/<name>" with exactly one separator. Shared because both app copies
// had their own rb_join() and a path join is not worth two implementations.
void rbindex_join(char *out, int outsz, const char *dir, const char *name);

// ---------------------------------------------------------------------------
// The row format (PURE: no I/O, no libc beyond what is defined in rbindex.c)
// ---------------------------------------------------------------------------

// Parse one line (WITHOUT its newline) into *out. Returns 1 on a usable row,
// 0 if there is no name or no '|' at all. A V1 row gives deleted_unix 0 and
// actor "user". A non-numeric or empty timestamp field also gives 0, because
// every parse failure must land on "age unknown", i.e. on keeping the file.
int rbindex_parse_row(const char *row, int len, rb_row_t *out);

// Render a row, newline-terminated, into out. Returns the byte count, or 0 if
// it would not fit. '|', CR and LF are stripped from the name and the path:
// they are the field and record separators and a path containing one would
// split a row. Neither is legal in a name on this filesystem, so this is belt
// and braces rather than a transformation anyone observes.
int rbindex_row_build(const rb_row_t *r, char *out, int cap);

// Is this actor token an AI actor ("ai:...")? Anything else, including an
// empty or unrecognised token, is treated as the user.
int rbindex_actor_is_ai(const char *actor);

// ---------------------------------------------------------------------------
// Retention (PURE). Mirrors kernel/rustkern/recycle.rs.
// ---------------------------------------------------------------------------

#define RB_RETENTION_NEVER         0u
#define RB_RETENTION_DEFAULT_DAYS  30u
#define RB_RETENTION_MAX_DAYS      3650u

// Parse a RECYCLE.CFG body. Returns days, or RB_RETENTION_NEVER for "never"
// and for "0". An absent, empty, commented-out or UNPARSEABLE setting returns
// RB_RETENTION_DEFAULT_DAYS: a typo must not silently turn the recycler off,
// and must not silently turn it into an immediate shredder either.
unsigned rbindex_parse_retention(const char *buf, int len);

// Render the exact RECYCLE.CFG body for `days`. Returns bytes written, or 0.
int rbindex_format_retention(unsigned days, char *out, int cap);

// 1 = purge, 0 = keep. Keeps on EVERY ambiguity: retention never, an unknown
// age (deleted_unix 0), or a clock that has moved backwards.
int rbindex_is_expired(unsigned long deleted_unix, unsigned long now_unix,
                       unsigned retention_days);

#ifndef RBINDEX_PURE_ONLY
// ---------------------------------------------------------------------------
// The index on disk
// ---------------------------------------------------------------------------

// Read the whole index into memory. Returns the number of rows parsed, or 0
// (an absent index is an empty bin, not an error).
int rbindex_load(void);
int rbindex_count(void);
const rb_row_t *rbindex_at(int i);

// Find the row for a bin-side file name. Returns 1 and fills *out, or 0.
// Operates on the last rbindex_load().
//
// NOTE FOR ANY CALLER THAT THINKS IT KNOWS THE BIN-SIDE NAME: it is NOT the
// original basename. A kernel-captured entry is named
// D<4 hex of the clock><4 hex uniq>.<EXT> by rcy_trash_name_rs(), so two files
// of the same name deleted from two directories cannot collide and the name
// survives a FAT 8.3 volume. The original name exists ONLY in the row.
int rbindex_lookup(const char *name, rb_row_t *out);

// Find the NEWEST row whose original_path is `path`. 1 and fills *out, or 0.
// This is the lookup an undelete-by-path tool needs: a caller knows where the
// file used to be, not what the bin called it. Newest is by recorded deletion
// time; an unknown-age row is taken only when nothing dated matches.
int rbindex_find_by_original(const char *path, rb_row_t *out);

// The AI task node id out of an "ai:XXXXXXXX" actor token. 0 for a user row and
// for a malformed token, because a partial id is worse than no id.
unsigned long rbindex_actor_id(const char *actor);

// Restore one entry to its recorded original path and drop its row.
//
// 0 on success. -1 the move failed or the row has no recorded origin, -2 the
// destination already exists and RB_RESTORE_NOCLOBBER was asked for, -3 the file
// is back but the row could not be dropped (so the entry is still listed and
// the next sweep reports it as an orphan row).
//
// rename() first, then a copy that is FSYNC'd before the bin copy is unlinked,
// because (#239) ext2 rename is unreliable and (#742) a copy that was never
// flushed is not a copy.
#define RB_RESTORE_NOCLOBBER 1
int rbindex_restore(const rb_row_t *e, int flags);

// APPEND one row, durably. Returns 0 only if the record is on the medium.
// Without a row a trashed file shows as "(unknown)" and can never be restored,
// so a silent failure here is a quieter version of the same data loss.
int rbindex_append(const char *name, const char *orig,
                   unsigned long deleted_unix, const char *actor);

// Remove the row for `name` and rewrite the index, durably. Returns 0 only if
// the rewritten index reached the medium, and only then is the in-RAM copy
// updated, so RAM and disk cannot disagree.
//
// #742 DISCIPLINE, PRESERVED EXACTLY: this must never unlink the index first.
// The old code did, and then hoped the create worked; when it did not, the
// restore information for EVERY item in the bin was gone, not just this one,
// and nothing said so. O_TRUNC does the same job in one operation with no
// window in which the index does not exist.
int rbindex_remove(const char *name);

// Truncate the index to nothing, durably. ONLY legitimate when every file in
// the bin is provably gone: those rows are the only record of where the files
// came from, so clearing the index while files remain turns a recoverable
// failure into a permanent one.
int rbindex_clear(void);

// ---------------------------------------------------------------------------
// The retention setting on disk
// ---------------------------------------------------------------------------

// Read <home>/CONFIG/RECYCLE.CFG. Absent or unreadable gives the default (30).
unsigned rbindex_read_retention(void);

// Write it, durably. Returns 0 only if the setting is on the medium.
int rbindex_write_retention(unsigned days);

// The seven values the Settings control offers, in order. Index 0 is Never.
// Exposed so Settings does not hand-write a second copy of the list, and so a
// test can round-trip every one of them.
extern const unsigned rb_retention_choices[7];
int rbindex_retention_choice_index(unsigned days);   // nearest match, never -1
const char *rbindex_retention_label(unsigned days);  // "Never", "30 days", ...
#endif // RBINDEX_PURE_ONLY

#endif // _RBINDEX_H
