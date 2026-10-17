// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// userconf.h - #683: per-user preference paths. See userconf.c for the rule.
//
// A per-user preference NAME lives at <home>/CONFIG/<NAME>. READS fall back to
// the legacy /CONFIG (or /) location so an upgrade keeps the user's settings;
// WRITES always go to the user's home, which is what stops the desktop needing
// write authority over /etc. Root's home is "/", so for a root session every
// path below resolves to exactly the legacy one and nothing changes.
//
// [noaccount] AND WHEN THERE IS NO ACCOUNT AT ALL, <home> IS /TMP, NOT "/".
// A session with no user-table entry (every machine from power-on until the
// first-boot wizard finishes) used to resolve <home> to "/", so all 67 of the
// per-account files reachable through these accessors aimed at root-owned
// space and every write was refused with -13, in silence. uc_state_root() in
// userconf.c is the single answer; see it for the measurement, and note that
// the question is "does this uid have a passwd entry", never "is its home /",
// because root's entry legitimately SAYS "/".
//
// Names must be FAT 8.3 (uppercase, no leading dot), like the rest of the tree.
#ifndef _USERCONF_H
#define _USERCONF_H

// Build "<home>/<sub>/<name>", where <home> comes from the passwd table. `sub`
// may be NULL or "" for "<home>/<name>". Returns 0 on success, -1 if it will not
// fit. Fails rather than truncating: a truncated path is a different file.
//
// #745: this is the join userconf_path() always performed, lifted out so that
// anything needing a path THE SESSION USER CAN WRITE uses one implementation.
// Root's home is "/", so for a root session "<home>/<name>" is exactly "/<name>"
// and nothing about a root session changes.
int userhome_path(const char *sub, const char *name, char *out, unsigned long cap);

// The session user's home directory itself, with no trailing '/' (so "/" stays
// "/", and "/HOME/ADMIN/" becomes "/HOME/ADMIN"). Returns 0 on success, -1 if
// it will not fit.
//
// #745: this is the SAME lookup userhome_path() performs, factored out rather
// than copied, because the per-user install needs the home as a SANDBOX ROOT to
// confine package destinations against (pkgdest.c), not as a prefix to join a
// name onto. Two implementations of "where is home" is exactly how a sandbox
// root and the paths inside it come to disagree.
int userhome_root(char *out, unsigned long cap);

// ---------------------------------------------------------------------------
// #harendezvous: THE SAME JOIN, FOR A HOME THAT IS NOT THE CALLER'S.
//
// userhome_path() above answers for the CALLING process's own uid, which is the
// right question for an app the user launched and the WRONG one for a boot-time
// service: a service runs as uid 0, so its own answer is "/" forever, while the
// file it has to read belongs to the session user. These two make that case
// expressible without a second copy of the join (two implementations of "where
// is home" is exactly how a sandbox root and the paths inside it come to
// disagree, which is why userhome_root() exists at all).
//
// userhome_path_as() is the join userhome_path() performs, with the home passed
// in rather than looked up; userhome_path() is now a caller of it, so there is
// still exactly ONE joiner.
int userhome_path_as(const char *home, const char *sub, const char *name,
                     char *out, unsigned long cap);

// The ACTIVE SESSION's home, which for a non-root caller is just its own home.
// Root asks the kernel (SYS_SESSION_HOME), because the kernel holds the single
// definition of "the session user" (#745) and Ring 3 does not get to name it.
// Falls back to userhome_root() whenever the kernel declines, so the answer is
// never worse than today's. Returns 0 on success, -1 if it will not fit.
int userhome_root_session(char *out, unsigned long cap);

// "<session user's home>/[sub/]name". The READ half of a two-principal
// rendezvous: a privileged service resolves the requester's path with this, and
// the requester wrote it with userhome_path()/userhome_open_write(), so both
// ends agree by construction. Returns 0 on success, -1 if it will not fit.
int userhome_path_session(const char *sub, const char *name,
                          char *out, unsigned long cap);

// Open the session user's copy for READING, falling back to `legacy` (the
// pre-#683 absolute path) when it is not there. Same asymmetric rule as
// userhome_open_read(): per-user first, legacy second, so a stock image and an
// upgrade both keep working.
int userhome_open_read_session(const char *sub, const char *name,
                              const char *legacy);

// #appwrite: report a refused write to the one channel that does not itself
// depend on the permission that just failed (sys_bootlog -> /BOOTLOG.TXT).
// Called by userconf_open_write() and by any save site that wants the same
// breadcrumb. Bounded internally; safe to call on every failure.
void uc_report_write_refusal(const char *what, const char *path, int rc);

// Build "<home>/CONFIG/<name>". Returns 0 on success, -1 if it will not fit.
// Fails rather than truncating: a truncated path is a different file.
int userconf_path(const char *name, char *out, unsigned long cap);

// Open a per-user preference for reading, falling back to `legacy` (the
// pre-#683 absolute path, e.g. "/CONFIG/THEME.CFG") when the per-user copy does
// not exist. Returns an fd, or -1.
int userconf_open_read(const char *name, const char *legacy);

// Open a per-user preference for writing (O_WRONLY|O_CREAT|O_TRUNC), creating
// <home>/CONFIG if needed. Never writes the legacy location. Returns an fd, -1.
int userconf_open_write(const char *name);

// [noaccount] CORRECTED. This block used to describe these three as a
// SEPARATE family that targeted "/TMP/<name>" flat on a no-account session,
// while the ordinary accessors above kept aiming at an unreachable
// <home>/CONFIG/<name>. That split was itself the defect: it made the
// notification spool the ONE per-account file that worked before an account
// existed, and left the other sixty-six to fail exactly as before.
//
// userconf_path() now resolves the SESSION'S STATE ROOT (see uc_state_root()
// in userconf.c for the rule, the measurement behind it, and why the test is
// "does this uid have a passwd entry" and not "is its home /"). These three
// are therefore now one-line aliases of their ordinary counterparts, kept as
// NAMES for one release only: their call sites are a three-party rendezvous
// on a single file, and renaming them in the same change that moves that file
// would make it impossible to say which edit did what.
//
// userconf_open_read_scratch() keeps one real difference: NO legacy fallback.
// The spool is not a preference with a shipped system default, and falling
// back to one would let a user read notifications that are not theirs.
//
// The kernel mirror is userconf_kpath_scratch() in kernel/fs/userconf.c,
// likewise now an alias of userconf_kpath(). Both sides derive from the SAME
// decision (kernel/rustkern/userconf.rs, userconf_state_root_rs), so the
// single-spool invariant notify.c documents holds by construction rather than
// by three call sites remembering to use the same variant.
int userconf_path_scratch(const char *name, char *out, unsigned long cap);
int userconf_open_write_scratch(const char *name);
int userconf_open_read_scratch(const char *name);

// --- #peruser2: the same rule for files that do NOT live under <home>/CONFIG --
//
// userconf_open_read()/userconf_open_write() hardcode the "CONFIG" component,
// which is right for a preference and wrong for everything else this migration
// had to move: /ASSOC.CFG and /RAILPOS.TXT sit at the TOP of the user's space,
// /SQUADRON/FPSLOG.TXT sits in a per-game subdirectory. Fifteen call sites were
// about to hand-roll "userhome_path() then sys_open() then fall back", which is
// exactly the duplication userconf.c exists to prevent, so the "CONFIG" is a
// PARAMETER here instead. `sub` may be NULL or "" for "<home>/<name>".
//
// THE ROOT-IS-UNCHANGED PROPERTY IS WHY `sub` MATTERS AND IS NOT A STYLE
// CHOICE. Root's home is "/", so userhome_open_*(NULL, "ASSOC.CFG") resolves to
// exactly "/ASSOC.CFG" and userhome_open_*("CONFIG", "THEME.CFG") to exactly
// "/CONFIG/THEME.CFG". Pick `sub` to MATCH THE FILE'S LEGACY DIRECTORY and a
// root session cannot regress; pick it by taste and it silently can.
int userhome_open_read(const char *sub, const char *name, const char *legacy);
int userhome_open_write(const char *sub, const char *name);

// Open an ALREADY-RESOLVED path for writing: creates the parent directory,
// O_WRONLY|O_CREAT|O_TRUNC, and reports a refusal through
// uc_report_write_refusal(). For a destination computed at runtime (a note
// file, one blob in a cover-art cache) where there is no NAME to pass.
// userconf_open_write() is now this with the name->path step done first.
int userconf_open_write_path(const char *path);

// Tighten a just-written per-user file to owner-only (0600).
//
// For a file that IS a secret (TOTP seeds, a stored share password) the per-user
// home is necessary and NOT sufficient: perms_on_create() stamps the system
// default mode on a newly created file, so relocating a secret into a home
// without this leaves it group/world readable inside a directory that merely
// happens to be 0750 today. ONE place, so the mode is not re-decided per app.
// A refused chmod is reported, not fatal: the bytes are already written and
// failing the save afterwards would lose them for no gain.
void userconf_protect(const char *path);

// #aiaudit: create the parent directory of `path` if it is not already there.
// Idempotent, and the return is deliberately not reported: "already exists" and
// "created" are equally fine, and the open that follows is the real test.
//
// Extracted from userconf_open_write() rather than copied, because the AI audit
// sink (libc/aicap.c) does its own read-modify-write open and so cannot go
// through userconf_open_write(), but must agree with it about creating
// <home>/CONFIG.
//
// CORRECTED #prewrite: this said users_make_home_skeleton() creates "DESKTOP,
// DOCUMENT, DOWNLOAD, MUSIC, PICTURES and VIDEOS but NOT CONFIG". It HAS made
// CONFIG since #745 (kernel/proc/users.c, the subdirectory list also carries
// APPS, GAMES and the per-game directories). A doc that states the opposite of
// the code on the one question this function exists to answer is worse than no
// doc. What remains true is the second half: every home on an already-installed
// system predates the convention, so somebody still has to make it and there
// must be exactly one somebody.
//
// #prewrite: this is now MKDIR -P, not one level, because the owner's layout
// has a two-component tier (<home>/APPS/<app>/<file>).
void userconf_ensure_parent_dir(const char *path);

// ---------------------------------------------------------------------------
// #prewrite: THE NAMED PLACES, so an app ASKS for "this user's music directory"
// instead of assembling "<home>/MUSIC" itself. If the layout is only reachable
// by convention then every consumer hardcodes it, and the layout rots the first
// time it changes with no single place to change it. See userconf.c for which
// directories are real (users_make_home_skeleton() creates them at ACCOUNT
// CREATION, owned by the account at 0750) and for the case-folding note.
// ---------------------------------------------------------------------------

// "<home>/<sub>": the directory itself, no name joined on. `sub` NULL or "" is
// the home. Returns 0, or -1 if it will not fit (never truncates).
int userhome_dir(const char *sub, char *out, unsigned long cap);

// This user's music directory: the owner's `/home/<user>/music`.
int userhome_music_dir(char *out, unsigned long cap);

// This user's per-app data directory: the owner's `/home/<user>/apps/<app>/`.
// `app` is a BARE NAME. A separator, a leading "." or a control byte is
// REFUSED rather than normalised: an app name is not a path, and accepting one
// is how a per-app-data accessor becomes a way to write outside the home.
int userhome_app_dir(const char *app, char *out, unsigned long cap);

// One file in it: the owner's `/home/<user>/apps/<app>/database.db`.
int userhome_app_path(const char *app, const char *name, char *out, unsigned long cap);

// "/SHARED/<name>": the all-users space. Every account may create names there.
// Owner-only-delete is enforced in the KERNEL (fs/perms.c perms_may_remove),
// never here: a userland accessor is a convenience, not a boundary.
int shared_path(const char *name, char *out, unsigned long cap);

// Somewhere this session can ACTUALLY write a scratch file: the user's home
// when there is an account, and /SHARED's sibling /TMP (also 01777) when there
// is not. On a freshly built golden there is no /CONFIG/PASSWD by design, so
// the session has no home and every per-user path resolves to a root-owned
// location; this is the accessor that stops that being fatal.
int userscratch_path(const char *name, char *out, unsigned long cap);

// --- [filesui] THE REST OF THE NAMED PLACES ---------------------------------
//
// userhome_music_dir() arrived alone, so every other folder in the same
// skeleton was still reached by joining a literal onto the home. Files carried
// its own `qa_dir[]` of them and the office file chooser knew about none of
// them. These complete the set, so that a directory's on-disk name appears in
// exactly ONE place in the userland tree: here. See libc/places.c for the
// labels and libc/places.h for why the two are deliberately separate files.
//
// THE SINGULAR/PLURAL FAULT THESE FIX, recorded because the next person will
// meet it in a golden that predates the change. users_make_home_skeleton()
// created DOCUMENT and DOWNLOAD, singular, on the stated grounds that "FAT is
// 8.3-only so the on-disk names are <=8 chars". That grounds is STALE: homes
// live on the ext2 ROOT partition (two-partition layout, #365), which has no
// 8.3 limit, and the FAT ESP holds boot assets only. Meanwhile Files' sidebar
// label said "Documents". A label that does not match the directory it opens is
// a bug in whichever of the two is wrong, and the directory was wrong, so the
// directory is what changed. The accessors below are the only thing that knows.
int userhome_desktop_dir(char *out, unsigned long cap);
int userhome_documents_dir(char *out, unsigned long cap);
int userhome_downloads_dir(char *out, unsigned long cap);
int userhome_pictures_dir(char *out, unsigned long cap);
int userhome_videos_dir(char *out, unsigned long cap);

// The all-users directory ITSELF, where shared_path() gives one name inside it.
// A chooser needs somewhere to NAVIGATE to, which is a directory, not a file.
int shared_dir(char *out, unsigned long cap);

// The scratch directory itself, the sibling of shared_dir(). Same split, same
// reason, and the same session rule as userscratch_path(): the home when there
// is an account, /TMP when there is not.
int userscratch_dir(char *out, unsigned long cap);


// System media content: the shipped sample library at /MEDIA/MUSIC, root-owned
// 0755. NOT a per-user location and deliberately not derived from a home. It is
// here rather than in a UI because it is part of the layout, and because a
// second copy of the literal is how a moved directory starts rendering as an
// empty folder. A user's OWN music is userhome_music_dir(), a different place
// with a different owner; anything offering both must LABEL them differently.
int media_music_dir(char *out, unsigned long cap);

// The system media ROOT, /MEDIA. What a Places sidebar should offer: one row
// labelled "Media" whose contents are MUSIC today and can gain VIDEO without a
// UI change, rather than a row labelled "Music" sitting next to the user's own
// Music row and indistinguishable from it.
int media_dir(char *out, unsigned long cap);


// --- #743: completing a write, so a failed save cannot look like a good one ---
//
// UC_MUST_CHECK mirrors kernel/types.h's MUST_CHECK. Userland does not include
// kernel headers, so the attribute is spelled out here rather than shared.
// NOTE, because this bit an earlier ticket: a `(void)f()` cast does NOT silence
// warn_unused_result on gcc 12.2 and never did. To ignore one of these results
// deliberately, assign it and say in a comment why the failure does not matter.
#ifndef UC_MUST_CHECK
#define UC_MUST_CHECK __attribute__((warn_unused_result))
#endif

// Write every byte of `buf` to `fd`, fsync it, then close it. ALWAYS closes fd,
// on every path, so the caller never leaks one and must never close it again.
// Returns 0 ONLY if the write, the fsync and the close ALL succeeded; -1 if any
// of them failed. Any non-zero return means the file on disk is NOT known to
// hold `buf`, so the caller must not report success to the user.
UC_MUST_CHECK int userconf_finish_write(int fd, const void *buf, unsigned long len);

// Replace the entire contents of `path` with `buf`. Returns 0 on success, -1 on
// failure. Does NOT unlink first: a refused open leaves the previous file
// intact, which is the whole point. See userconf.c for the limits (this is not
// an atomic replace, and cannot be until the kernel grows one).
UC_MUST_CHECK int userconf_write_all(const char *path, const void *buf, unsigned long len);

#endif
