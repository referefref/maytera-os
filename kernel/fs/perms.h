// perms.h - File permissions database for MayteraOS
// Provides UNIX-style file permissions on FAT32 via a sidecar database
#ifndef PERMS_H
#define PERMS_H

#include "../types.h"

// Hash table size and limits
#define PERM_TABLE_SIZE     512
#define MAX_PERM_ENTRIES    2048

// Access check flags (matching POSIX)
#define R_OK    4   // Read permission
#define W_OK    2   // Write permission
#define X_OK    1   // Execute permission
#define F_OK    0   // File existence

// Permission entry for a single file/directory
typedef struct perm_entry {
    char path[256];             // Absolute path (uppercase FAT names)
    uint32_t uid;               // Owner UID
    uint32_t gid;               // Owner GID
    uint16_t mode;              // Permission bits (rwxrwxrwx + setuid/setgid/sticky)
    struct perm_entry *next;    // Hash chain
} perm_entry_t;

// ============================================================================
// API Functions
// ============================================================================

// Initialize permissions subsystem, load from /CONFIG/PERMS.DB
void perms_init(void);

// Check if a user has access to a file, with POSIX path resolution (#674):
// the path is canonicalized (made absolute, "." dropped, ".." popped, "//"
// collapsed) exactly the way the filesystem resolves it, and SEARCH (x) is
// required on every directory component before `access` is applied to the
// object itself. Root (uid 0) and pre-perms_init callers bypass, unchanged.
// Returns 0 on success, -1 (EACCES) on denied.
int perms_check(const char *path, uint32_t uid, uint32_t gid, int access);

// #674: the single-object decision, for ONE already-canonical path. No
// traversal, and no uid-0 / !perms_initialized bypass (perms_check owns those).
// Exposed for the Rust path walker in rustkern/permpath.rs; not a general API.
int perms_check_leaf(const char *path, uint32_t uid, uint32_t gid, int access);

// #674: boot self-test. Proves canonicalization and directory traversal against
// the LIVE database, in both directions (denied stays denied, allowed stays
// allowed). Called from perms_init(); logs [PERMS-SELFTEST].
void perms_selftest(void);

// #PERMSKIP: the SESSION-SCOPED run of the same vectors, against THE ACTUAL
// LOGGED-IN USER'S HOME, whatever the first-boot wizard let the owner call the
// account. perms_init() runs long before any session exists, so the boot run
// above can only enumerate what /CONFIG/PERMS.DB already holds; this one is
// called from kernel/main.c straight after the login-time home claim (#745) and
// is the definitive run. `home` is the session user's home ("/" for root, which
// is NOT a home directory: see the comment on the definition). Logs
// [PERMS-SELFTEST], durably.
void perms_selftest_session(const char *home, uint32_t uid, uint32_t gid);

// #appwrite: the OPERATIONAL question the rule self-test does not ask: given
// this session's identity, is there ANY directory on this filesystem it can
// create a file in? Decides nothing and changes no mode; it asks the same
// walker the real gate asks and writes the answer to the boot log. Called from
// perms_selftest_session(). Logs [PERMS-WRITEMAP], durably.
void perms_report_session_writability(const char *home, uint32_t uid, uint32_t gid);

// Set permissions for a file
void perms_set(const char *path, uint32_t uid, uint32_t gid, uint16_t mode);

// Remove permissions entry for a file
void perms_remove(const char *path);

// Flush permissions database to disk
MUST_CHECK int perms_sync(void);   // #693: 0 only if PERMS.DB is on the medium

// #679: coalesced write-back, called from the heartbeat worker. Without it,
// ownership recorded at create time never reaches /CONFIG/PERMS.DB and is lost
// at reboot, so a user could write a file they created only until they rebooted.
MUST_CHECK int perms_sync_if_dirty(void);   // #693

// Get permissions for a file
// Returns 0 on success, -1 if no entry exists
int perms_get(const char *path, uint32_t *uid, uint32_t *gid, uint16_t *mode);

// Change mode (chmod), respecting ownership
// Returns 0 on success, -1 on permission denied
int perms_chmod(const char *path, uint32_t caller_uid, uint16_t mode);

// Set default permissions for a newly created file
void perms_set_default(const char *path, uint32_t uid, uint32_t gid, int is_dir);

// #679 prereq: record create-time ownership, the way Linux gives a new inode
// its creator's uid/gid. Call at EVERY create point with the creating process's
// euid/egid. Does NOT overwrite an existing entry, so a seeded 0600 or an
// operator chmod survives a delete-and-rewrite by its Ring-0 owner.
// Without this, perms_check()'s root-owned no-entry default denies W_OK to
// every non-root process for every path it creates, including in its own home.
void perms_on_create(const char *path, uint32_t uid, uint32_t gid, int is_dir);

// (perms-orphan, no-ticket, wallpaperpersist-followup): does a PERMS.DB entry exist for `path` at all (after the same
// canonicalization perms_check()/perms_on_create() use)? Read-only. See
// perms.c for why this differs from perms_get()'s -1-on-no-entry: this is
// for a caller that must tell "no policy exists yet" apart from "a real
// entry denies this", which a plain perms_check()/perms_get() failure cannot.
int perms_has_entry(const char *path);

// ---------------------------------------------------------------------------
// #prewrite: S_ISVTX. THE ONE DECISION for "may this principal remove or
// replace this name", used by unlink, rmdir, both ends of rename and the DOS
// INT 21h delete/rename/rmdir gate.
//
// WHY IT EXISTS. The owner asked for an all-users pre-writable directory.
// Implemented naively that is a 0777 directory, and in a system with more than
// one account a 0777 directory lets EVERY user delete or overwrite EVERY other
// user's files. Before this there was no concept in this file that could stop
// that: perms_check_leaf() reads only bits 0..8, so bit 9 was inert.
//
// WHAT IT CHANGES, precisely: NOTHING, on any directory whose PERMS.DB mode
// does not carry 01000. The sticky bit only ever SUBTRACTS, and only where it
// was deliberately set, which is what makes it safe to wire into five call
// sites in one change.
//
// On a sticky directory a name may be removed by root, by the OWNER OF THE
// VICTIM, or by the OWNER OF THE DIRECTORY. The third principal is the
// automation answer and is not decoration: see rustkern/sticky.rs.
//
// `parent` and `victim` are absolute paths; both are canonicalized here with
// the same canonicalizer perms_check() uses. Returns 0 to allow, -1 to deny.
// It applies the ordinary write-on-the-parent precondition itself, so a caller
// replaces its perms_check(parent, W_OK) with this rather than adding to it.
#define PERM_STICKY 01000
int perms_may_remove(const char *parent, const char *victim,
                     uint32_t uid, uint32_t gid);

// #prewrite: boot self-test for the rule above. Logs [STICKY-SELFTEST].
void perms_sticky_selftest(void);

// ===========================================================================
// [openperm] #246: IS THIS PATH SUBJECT TO THE LOCAL PERMISSION TABLE AT ALL?
// ===========================================================================
// The two answers rustkern/volperm.rs's openperm_gate_rs() returns. They live
// in this header, and not at the one call site, because there are TWO callers
// and they must name the same integers: the gate itself in sys_open_k()
// (proc/fdlayer.c), and the boot-time cross-language constant lock in
// hotplug_init() (drivers/hotplug.c) that proves these C names still match the
// Rust constants. If they ever drifted, every removable path would silently
// stop being judged, which is precisely the defect #246 [openperm] removed, so
// it is checked rather than assumed.
//
// EXEMPT is for a medium with a real authority of its own: an SMB or NFS share
// decides access server-side (NTLM/RPC authentication plus the share ACLs) and
// knows a remote identity this table has never heard of.
//
// A REMOVABLE VOLUME IS **NOT** EXEMPT. It has no server and no ACLs, so
// either this table decides or nothing does, and "nothing does" was measured
// as an unprivileged process blanking, overwriting and reading another user's
// files on a mounted stick. See rustkern/volperm.rs for the measurement and
// for why the mount point gets a 01777 row so that enforcing this does not
// make a stick read-only to the person who plugged it in.
#define OPENPERM_EXEMPT   0   /* skip the local permission table entirely */
#define OPENPERM_ENFORCE  1   /* consult the local permission table */

#endif // PERMS_H
