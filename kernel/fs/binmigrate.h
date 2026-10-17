// binmigrate.h - [binmigrate] move a first-boot bootstrap session's
// per-account state into the account the setup wizard just created.
//
// WHY THIS EXISTS, in one paragraph. [noaccount] gave the kernel one answer to
// "where does this session's per-account state live": the user-table home when
// the session uid has an entry, and "/TMP" when it has none. The first-boot
// bootstrap session (#OOBEAUTH) has none, so everything it writes, including
// the Recycle Bin and its index, lands under "/TMP/CONFIG". The setup wizard
// then creates the account AT THE SAME UID, with a home of "/HOME/<NAME8>",
// and from that instant the identical accessor answers "/HOME/<NAME8>/CONFIG"
// for the same live session. Anything the user deleted during setup was in a
// directory nothing would ever resolve again. Nothing moved it; there was no
// migration logic at any account-creation site in the tree.
//
// The policy (the per-entry disposition, the bounded join, the armed signal)
// is in rustkern/binmigrate.rs. This header is the C surface, and
// fs/binmigrate.c is the filesystem walk, which is C because every line of it
// is a call into the C fat_* / perms_* primitives.
#ifndef FS_BINMIGRATE_H
#define FS_BINMIGRATE_H

#include "../types.h"

// The per-entry disposition codes. A WIRE FORMAT shared with
// rustkern/binmigrate.rs, which defines the same four values as BM_ACT_*.
#define BM_ACT_MOVE     0   // the destination is free: rename it across
#define BM_ACT_APPEND   1   // both exist and the file is a row log: concatenate
#define BM_ACT_SKIP     2   // leave it where it is, and SAY SO
#define BM_ACT_RECURSE  3   // a directory: descend one level

// A renumbering on either side would silently turn every MOVE into an APPEND,
// which on the Recycle Bin index means concatenating a file onto itself. There
// is no runtime check that could catch that, so it is a compile-time one.
_Static_assert(BM_ACT_MOVE == 0 && BM_ACT_APPEND == 1 &&
               BM_ACT_SKIP == 2 && BM_ACT_RECURSE == 3,
               "BM_ACT_* is shared with rustkern/binmigrate.rs; renumber both or neither");

// Call once, the moment a session identity becomes known (main.c, beside
// recycle_session_changed()). `has_entry` must come from a real
// user_lookup_uid(), NEVER from comparing the home string against "/": root's
// own entry says its home IS "/", so the string cannot tell root from a
// bootstrap session.
//
// Arms the one-shot migration signal iff the session has NO entry. A session
// that has one DISARMS, so a lock-screen re-authentication cannot leave a
// stale arm behind.
void binmig_session_established(uint32_t uid, int has_entry);

// Call after an account has been COMMITTED for `uid` (both passwords set, the
// authentication proof passed, the home skeleton made), from every path that
// commits one: users_create_first_admin() and sys_user_create_pw().
//
// A no-op unless binmig_session_established() armed for exactly this uid, so
// it is free on every machine that already had an account, and it consumes the
// signal, so a second call does nothing.
//
// Deliberately NOT called from user_create(): both commit paths ROLL BACK with
// user_delete() if the password or the authentication proof fails, and a
// migration performed before the commit would have moved the user's files into
// the home of an account that then ceased to exist.
//
// Blocking: straight-line filesystem I/O (fat_open/readdir/move/read/write).
// Process context only, exactly like recycle_capture().
//
// Returns the number of items moved or merged (0 is the normal answer on a
// machine whose bootstrap session deleted nothing), or -1 if the migration was
// attempted and could not complete, which is reported on serial and in the
// boot log either way.
int binmig_on_account_committed(uint32_t uid);

// KILL-SWITCH: /CONFIG/BINMIG.OFF makes binmig_on_account_committed() the
// no-op it was before this work, so the SAME kernel can be measured with the
// fix on and off and the RED/GREEN arms of proc/binmig_vmtest.c differ by one
// file on the image rather than by two binaries. It is also an operator escape
// hatch; the data stays readable at its old path either way. The disabled path
// says so LOUDLY on serial and in the boot log.

// The boot self-test over the Rust policy. Prints one line, ALWAYS, including
// on a production golden: the project has found self-tests that existed and
// never executed, and a line costing microseconds is the cheapest defence
// against this one joining them. If the line is absent from a boot log, the
// code is not in that kernel.
void binmig_selftest(void);

#endif
