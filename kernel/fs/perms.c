// perms.c - File permissions database for MayteraOS
// Stores file ownership and permission bits in a hash table, backed by /CONFIG/PERMS.DB

#include "perms.h"
#include "fat.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "bootlog.h"                        // #PERMSKIP: a security self-test
                                            // that only reaches a serial port
                                            // proves nothing on the laptop or
                                            // the iMac, which have none.
#include "../security/selftest_registry.h"  // #PERMSKIP: a group that declines
                                            // to run must say so LOUDLY.

// External filesystem
extern fat_fs_t g_fat_fs;

// Hash table buckets
static perm_entry_t *perm_table[PERM_TABLE_SIZE];

// Pre-allocated entry pool (avoid per-entry kmalloc in freestanding environment)
static perm_entry_t perm_pool[MAX_PERM_ENTRIES];
static int perm_pool_next = 0;

// Dirty flag for sync
static bool perms_dirty = false;
static bool perms_initialized = false;

// ============================================================================
// Internal helpers
// ============================================================================

// DJB2 hash
static uint32_t path_hash(const char *path) {
    uint32_t hash = 5381;
    while (*path) {
        hash = ((hash << 5) + hash) + (uint8_t)*path;
        path++;
    }
    return hash % PERM_TABLE_SIZE;
}

// Normalize path to uppercase for FAT consistency
static void normalize_path(const char *src, char *dst, size_t dst_size) {
    size_t i = 0;
    while (src[i] && i < dst_size - 1) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i] = c;
        i++;
    }
    dst[i] = '\0';
}

// Allocate a permission entry from the pool
static perm_entry_t *alloc_entry(void) {
    if (perm_pool_next >= MAX_PERM_ENTRIES) {
        return NULL;
    }
    perm_entry_t *e = &perm_pool[perm_pool_next++];
    memset(e, 0, sizeof(perm_entry_t));
    return e;
}

// Look up a permission entry by path
static perm_entry_t *perms_lookup(const char *path) {
    char norm[256];
    normalize_path(path, norm, sizeof(norm));

    uint32_t h = path_hash(norm);
    perm_entry_t *e = perm_table[h];
    while (e) {
        if (strcmp(e->path, norm) == 0) {
            return e;
        }
        e = e->next;
    }
    return NULL;
}

// Parse an octal string (e.g., "0755")
static uint16_t parse_octal(const char *s) {
    uint16_t val = 0;
    while (*s >= '0' && *s <= '7') {
        val = (val << 3) | (*s - '0');
        s++;
    }
    return val;
}

// Parse a decimal string
static uint32_t parse_uint(const char *s) {
    uint32_t val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val;
}

// Format octal for output (writes 4 chars like "0755")
static void format_octal(uint16_t mode, char *buf) {
    buf[0] = '0' + ((mode >> 9) & 7);
    buf[1] = '0' + ((mode >> 6) & 7);
    buf[2] = '0' + ((mode >> 3) & 7);
    buf[3] = '0' + (mode & 7);
    buf[4] = '\0';
}

// ============================================================================
// Load / Save
// ============================================================================

// Parse a single line from PERMS.DB
// Format: /PATH:UID:GID:MODE
static void parse_perms_line(const char *line) {
    if (!line || line[0] == '\0' || line[0] == '#') return;

    // Find first colon (end of path)
    const char *p = line;
    const char *path_start = p;
    while (*p && *p != ':') p++;
    if (*p != ':') return;

    char path[256];
    size_t path_len = (size_t)(p - path_start);
    if (path_len >= sizeof(path)) return;
    memcpy(path, path_start, path_len);
    path[path_len] = '\0';

    p++;  // skip ':'

    // Parse UID
    uint32_t uid = parse_uint(p);
    while (*p && *p != ':') p++;
    if (*p != ':') return;
    p++;

    // Parse GID
    uint32_t gid = parse_uint(p);
    while (*p && *p != ':') p++;
    if (*p != ':') return;
    p++;

    // Parse mode (octal)
    uint16_t mode = parse_octal(p);

    // Store in hash table
    perms_set(path, uid, gid, mode);
}

// ============================================================================
// #670 SYSTEM PERMS SEED
// ============================================================================
// perms_check() defaults a path with NO entry to root-owned 0755: world
// READABLE, root-only writable (see its `if (!e)` branch below). That default
// is how the kernel image itself (/KERNEL.ELF and its three sibling boot paths)
// and every on-disk diagnostic became readable by any Ring-3 process. An
// unstripped kernel.elf is a complete symbol map; a panic record is a live
// fault address.
//
// The seed below gives those files REAL entries instead of leaning on the
// default. Two properties matter:
//
//  1. It runs UNCONDITIONALLY, not only in perms_init()'s "no PERMS.DB found"
//     branch. That branch does not execute on a shipped image, because the
//     golden ships a /CONFIG/PERMS.DB. Adding entries there alone would have
//     changed nothing on a real system while LOOKING like a fix, which is the
//     exact prose-over-artifact trap blame.md keeps recording.
//  2. It only fills a MISSING entry. An existing entry (an admin chmod, or a
//     hand-authored PERMS.DB) always wins, so the seed can neither undo an
//     operator decision nor silently re-tighten something root loosened.
//
// Every path here is written by Ring 0 only (panic.c, bootlog.c, devlog.c,
// selfupdate.c, the installer) and read by nothing in userland: a tree-wide
// grep of userland/ finds no open of any of them. perms_check() is consulted
// only for p->privilege == PRIV_USER, so the in-kernel writers are unaffected
// by 0600. Off-line post-mortem is also unaffected: PERMS.DB is a
// MayteraOS-side sidecar and FAT/ext2 carry no such bits, so pulling the medium
// and reading it on another machine works exactly as before.
static const struct { const char *path; uint16_t mode; } perms_system_seed[] = {
    // The kernel image on every boot path the deploy writes. normalize_path()
    // uppercases, so "/boot/kernel.elf" and "/BOOT/KERNEL.ELF" are one key.
    { "/KERNEL.ELF",           0600 },
    { "/BOOT/KERNEL.ELF",      0600 },
    { "/EFI/BOOT/KERNEL.ELF",  0600 },
    { "/BOOT/KERNEL.ELF.BAK",  0600 },   // selfupdate.c's brick-safe backup
    // Persistent diagnostics. The panic record carries a fault address; the
    // device and boot logs describe the hardware and boot path in detail.
    // Both the pre-#670 root paths and the ESP paths panic.c now uses.
    { "/BOOT/PANIC.TXT",       0600 },
    { "/BOOT/STAGE.TXT",       0600 },
    { "/PANIC.TXT",            0600 },
    { "/STAGE.TXT",            0600 },
    { "/BOOTLOG.TXT",          0600 },
    { "/DEVLOG.TXT",           0600 },
    { "/USBLOG.TXT",           0600 },
    { "/AUDIOLOG.TXT",         0600 },
    { "/WIN16LOG.TXT",         0600 },
    { "/SELFUPD.LOG",          0600 },

    // ---------------------------------------------------------------------
    // #674: the /CONFIG secrets. Before #674 perms_check() never traversed, so
    // /CONFIG being 0700 protected NOTHING below it: each of these paths had no
    // entry of its own and fell into the `if (!e)` default above, which is
    // world-readable. Every one of them now carries its own mode, which is the
    // Linux shape: the directory is traversable and the protection lives on the
    // file (/etc is 0755, /etc/shadow is not).
    //
    // Reader ring was checked per path before choosing a mode (a grep of both
    // kernel/ and userland/ for every name). Ring-0 readers are unaffected by
    // 0600 because perms_check() is consulted only for p->privilege == PRIV_USER.
    // ---------------------------------------------------------------------
    // Kernel-only readers: 0600 costs nothing today.
    //   SSH.CFG      -> gui/sshterm.c st_load_cfg()
    //   SSHHOST.KEY  -> net/ssh/ssh2_server.c load_host_key()
    //   AUTHKEYS     -> net/ssh/ssh2_server.c authkey_allowed()
    //   KNOWN_HOSTS  -> net/ssh/ssh2.c ssh2_known_hosts()
    //   PERMS.DB     -> this file (perms_init); no userland reader exists,
    //                   chmod(1) goes through SYS_FS_PERM_INFO, not the file.
    // #745: SHADOW WAS THE ONE SECRET THE CODE SEED DID NOT COVER.
    //
    // Its 0600 was set ONLY in perms_init()'s "no PERMS.DB found" branch,
    // which by that branch's own admission does not execute on a shipped
    // image, because the golden ships a /CONFIG/PERMS.DB. The password
    // hashes were therefore protected purely by a LINE IN A DATA FILE
    // ("/CONFIG/SHADOW:0:0:0600", verified present on golden 1742), with
    // nothing in the kernel to restore it. Lose or regenerate that file and
    // SHADOW silently falls back to perms_check()'s no-entry default, which
    // is world-READABLE. That is exactly the failure mode property 1 of this
    // seed was written to prevent, and the most important file in the system
    // was the one file left outside it.
    //
    // The seed only fills a MISSING entry, so a PERMS.DB that already carries
    // the line keeps it and an operator chmod still wins. This costs nothing
    // on a healthy image and is the difference between "protected" and
    // "protected as long as one data file is intact".
    { "/CONFIG/SHADOW",        0600 },
    { "/CONFIG/SSH.CFG",       0600 },
    { "/CONFIG/SSHHOST.KEY",   0600 },
    // #697: SSHD.CFG decides whether this machine answers on the network and
    // which accounts may log in. Ring-3 write access to it is remote access,
    // so it gets the same 0600 as the host key it gates.
    { "/CONFIG/SSHD.CFG",      0600 },
    { "/CONFIG/AUTHKEYS",      0600 },
    { "/CONFIG/KNOWN_HOSTS",   0600 },
    { "/CONFIG/KNOWN_HO",      0600 },   // the FAT 8.3 twin that is also on disk
    { "/CONFIG/PERMS.DB",      0600 },
    // #692: CRON.CFG names programs and, since #692, the uid each runs as.
    // Write access to it is therefore write access to another account. It
    // had no entry and fell into the world-readable, root-writable default;
    // pinning it makes the "a line with no owner came from root" assumption
    // in cron_parse_line() an enforced property rather than an incidental
    // one. No userland reader exists (a tree-wide grep of userland/ finds
    // only a comment in apps/updated/main.c); cron is read via SYS_CRON_LIST.
    { "/CONFIG/CRON.CFG",      0600 },

    // Ring-3 readers exist: 0600 is a REAL behaviour change for a non-root
    // session, and is deliberate. Recorded here so nobody has to rediscover it:
    //   KIMI.KEY   -> userland/libc/aiclient.c load_key(), plus independent
    //                 copies in apps/paint/ai.c and apps/settings/main.c.
    //                 A non-root session loses AI in aichat/terminal/msh/rss/
    //                 paint and the Settings AI panel.
    //   AISVC.CFG  -> userland/libc/aiclient.c load_aisvc(); it can carry a key
    //                 that OVERRIDES KIMI.KEY, so locking one without the other
    //                 would be theatre. Not present on the shipped image; the
    //                 entry is harmless until Settings creates the file.
    //   EXTSVC.CFG -> apps/haservice/main.c load_config() and the Settings
    //                 external-services panel. It holds a Home Assistant token.
    //                 The HA desktop widget itself does NOT read it (it reads
    //                 the /HA*.TXT cache haservice writes), so the widget keeps
    //                 rendering; it is the daemon that would be denied.
    { "/CONFIG/KIMI.KEY",      0600 },
    { "/CONFIG/AISVC.CFG",     0600 },
    { "/CONFIG/EXTSVC.CFG",    0600 },

    // #745: files that had NO entry and so sat on the world-readable
    // no-entry default. Each was found by enumerating /CONFIG on golden 1742
    // and subtracting the seed, which is the only way to see a gap of this
    // shape: nothing points at a file that nobody remembered to protect.
    //
    //   MFA.DB      the TOTP shared secrets, written from Ring 3 by
    //               apps/mfa (DB_PATH at apps/mfa/main.c:28). Its own UI
    //               string says "stored obfuscated (not encrypted)", so the
    //               file IS the second factor: anyone who reads it can
    //               generate the codes. It is created at enrolment rather
    //               than shipped, which is why it is absent from the golden
    //               and absent from every previous audit of that directory.
    //               Seeding it here means the entry exists BEFORE the file
    //               does, so perms_on_create() (which never overwrites) finds
    //               a policy already in place instead of stamping its 0644
    //               default on a file full of secrets.
    //   SERVICES.CFG each line names an executable AND the uid to run it as
    //               (kernel/proc/services.c parses "name exec account uid
    //               perms autostart enabled"). Write access to it is the
    //               power to run code as any user at boot, which is the same
    //               authority CRON.CFG got 0600 for in #692. It was left on
    //               the default purely because nobody had enumerated it.
    //   LOGIN.CFG   names the account the machine autologins as. Write access
    //               is the power to choose who the desktop session runs as,
    //               which is the entire subject of this change.
    { "/CONFIG/MFA.DB",        0600 },
    { "/CONFIG/SERVICES.CFG",  0600 },
    { "/CONFIG/LOGIN.CFG",     0600 },

    // DELIBERATELY NOT 0600, with reasons, because "lock everything" is how a
    // permission model gets turned back off again:
    //   PASSWD/GROUP: these are Linux's /etc/passwd and /etc/group, and they are
    //     0644 there for a reason. They hold NO secrets (every hash lives in
    //     SHADOW, which is 0600). userland/libc/pwd.c and grp.c open them from
    //     Ring 3 for every uid->name lookup in the system: the Files properties
    //     owner column, the msh prompt, whoami, id, su, login, chown, adduser,
    //     passwd, and the COMPOSITOR'S OWN home-directory resolution
    //     (apps/compositor/profile.c). 0600 would break all of that to hide a
    //     username list. Explicit entries rather than the default so the intent
    //     is recorded and survives any future change to that default.
    //   CACERTS.PEM: public trust anchors, by definition not secret. Loaded by
    //     net/tls/cert_store.c in Ring 0, so it could in fact be 0600 with no
    //     functional cost today, but making public data root-only buys no
    //     security and would break the first userland TLS client anyone writes.
    { "/CONFIG/PASSWD",        0644 },
    { "/CONFIG/GROUP",         0644 },
    { "/CONFIG/CACERTS.PEM",   0644 },
};

// ============================================================================
// #221b SHARED GAME STATE SEED
// ============================================================================
// READ THIS BEFORE ADDING A LINE. Every entry in perms_system_seed[] above
// TIGHTENS a path: each one takes something off perms_check()'s world-readable
// no-entry default. Every entry BELOW does the opposite, it GRANTS write to a
// path that the same default would refuse, and a table that grants is a
// different kind of object from a table that restricts. They are separate so
// that "what did we open up" is one grep and not a mode-by-mode audit of a
// list whose other forty entries are locks.
//
// WHY ANYTHING NEEDS TO BE OPENED UP AT ALL. A DOS-era game keeps its state
// beside its executable, because DOS had one user. /DOS/<GAME> here is
// root-owned 0755, so a desktop session (uid 1000) is refused every such
// write. Measured on golden 2053: NetHack printed "Warning: cannot write
// record record", then "Some invalid directory locations were specified:
// leveldir, savedir, bonesdir, scoredir, lockdir, troubledir", and exited.
// The gate was working exactly as designed; there was simply nowhere writable
// for the game to put anything.
//
// The answer is NOT to make /DOS/NETHACK writable. That directory holds
// NETHACK.EXE and NHDAT, and a user who can rewrite the executable can hand
// the next user a different program. The state is split by WHAT IT IS instead:
//
//   /GAMES/NETHACK    0777  SHARED, and shared is the correct answer for a
//                           high-score table: `record` is one table for the
//                           machine, which is what it has meant since NetHack
//                           ran setgid on a UNIX timeshare. The lock file, the
//                           bones files (one player's death furnishing another
//                           player's dungeon) and the panic log are shared for
//                           the same reason. 0777 rather than a group mode
//                           because there is no "games" group to put a session
//                           user in, and the directory holds no code.
//   <home>/GAMES/NETHACK      PER-USER, created 0750 by
//                           users_make_home_skeleton(). Save files and the
//                           level files of a game in progress are private
//                           state: two people playing on one machine must not
//                           share them, and a save is exactly the file you do
//                           not want another account able to edit.
//
// The two `record` files below are NOT a duplicate, and the difference is
// measured, not assumed:
//
//   /GAMES/NETHACK/RECORD  is the REAL high-score table. topten() opens the
//                          record file for READING and gives up if the open
//                          fails ("Cannot open record file!"); it never
//                          creates it. Measured: with the directory writable
//                          but no `record` in it, a completed game wrote its
//                          logfile entry and NO score. So the file has to
//                          exist before the first game, and the build ships it
//                          empty.
//   /DOS/NETHACK/RECORD    exists so that NetHack's own startup probe passes.
//                          chdirx(hackdir, TRUE) calls check_recordfile()
//                          BEFORE initoptions() has read defaults.nh, so the
//                          probe runs with the DEFAULT (empty) score prefix
//                          and lands on hackdir/record whatever the config
//                          says. Measured: the warning names the file as
//                          "record", relative, which is what proves the config
//                          has not been read at that point. The probe opens
//                          O_RDWR and only warns if that fails, so an existing
//                          0666 file silences it. Nothing ever writes to it.
//
// These are files and directories the BUILD creates (build/build-golden.sh,
// section 3b-vii) and this table gives them their modes, so a fresh install
// has both halves. The seed only fills a MISSING entry, so an operator chmod
// still wins.
// #rawrite: AND THE THIRD INSTANCE ADDED NO ROW AT ALL, ON PURPOSE.
//
// Command & Conquer: Red Alert hits the identical wall and is the case that
// showed this table cannot be the answer for every game. Measured on a hand
// install at /DOS/RA:
//
//   [GUESTFS-DENY] guest=dos uid=1000 gid=1000 want=-w- op=INT21/3Ch create
//                  reason=PERMS path=/DOS/RA/./REDALERT.INI
//
// The two titles above were each rescued by pointing the GAME somewhere else:
// NetHack has DEFAULTS.NH, which names savedir and scoredir, and SimCity has a
// load/save requester that takes a typed path. Red Alert has NEITHER. Every
// mutable file it touches is a bare relative name in its own install
// directory, and it ignores the current directory it was launched with:
// measured under a DOSBox-X reference run on 2026-08-27, launched as
// C:\RA\GAME.EXE with the shell current directory set to C:\SAVE, it wrote its
// DOS/4G swap file and rewrote REDALERT.INI into C:\RA and left C:\SAVE empty.
//
// So there is no string this table could hold that makes Red Alert work
// EXCEPT a write grant on /DOS/RA itself, which is exactly the shortcut the
// block above rejects: that directory holds GAME.DAT, and a user who can
// rewrite the executable can hand the next user a different program. A 0777
// there would also be strictly worse than the NetHack case it is imitating,
// because NetHack's 0777 is on /GAMES/NETHACK, a directory holding NO CODE.
//
// The fix is therefore a MECHANISM and not a row: dos/int21svc.c now redirects
// a guest's writes into a per-user overlay under the launching user's own home
// (rustkern/dosovl.rs, and see the long note in dos/dospath.c, which had named
// this as the missing piece since #736 Stage 2). The overlay lives at
// <home>/GAMES/RA, created 0750 by users_make_home_skeleton(), so the ONLY
// permission change Red Alert needs is inside a directory the user already
// owns. Nothing under /DOS is loosened by one bit, and /DOS/RA stays root:root
// 0755 with no entry here at all.
//
// Read that as the shape for the next title: if a game can be POINTED at a
// writable directory, do that and put the mode here; if it cannot, redirect it
// and add nothing here.
static const struct { const char *path; uint16_t mode; } perms_shared_state_seed[] = {
    { "/GAMES/NETHACK",        0777 },
    { "/GAMES/NETHACK/RECORD", 0666 },
    { "/DOS/NETHACK/RECORD",   0666 },
    // #234f SimCity Classic. Same shape as NetHack and for the same reason:
    // /DOS/SIMCITY is root:root 0755 because it holds SIMCITY.EXE, so a
    // desktop session running as uid 1000 cannot write a city beside the
    // executable. This is the machine-wide half; the per-user half is
    // %HOME%/GAMES/SIMCITY, created by users_make_home_skeleton().
    //
    // SimCity Classic differs from NetHack in one way that matters: it has NO
    // config file that can name a save directory, so nothing redirects its
    // writes for it. What makes this usable is that its load/save requester
    // takes a TYPED path, and dospath.c resolves C:\GAMES\SIMCITY\NAME.CTY
    // to /WINDIR/DRIVE_C/GAMES/SIMCITY/NAME.CTY, which int21svc.c's
    // native_fallback() then maps to the native /GAMES/SIMCITY/NAME.CTY that
    // exists. So the player names the writable location once and the game
    // remembers it for the session.
    { "/GAMES/SIMCITY",        0777 },
};

// #674: /CONFIG shipped as mode 0700 (verified on golden build 1018, commit
// 1f2ee02: /CONFIG/PERMS.DB carries the line "/CONFIG:0:0:0700"). That is
// BACKWARDS relative to the model it is imitating. In Linux, /etc is 0755 and
// the protection lives on the individual file. A directory mode of 0700 is how
// you make a directory unusable to everyone but its owner, not how you protect
// its contents, and before #674 it protected nothing whatsoever, because
// perms_check() never looked at a parent directory.
//
// With #674's traversal live it stops being merely useless and becomes
// actively harmful: 0700 root:root denies SEARCH on /CONFIG to every non-root
// process, so nothing under /CONFIG resolves at all, including the entries
// that are supposed to stay readable (PASSWD, GROUP). The secrets are protected
// by their OWN 0600 entries seeded above; the directory must be traversable.
//
// #745: 0755 WAS ONE BIT TOO GENEROUS, AND THE MISSING BIT IS THE ENUMERATION.
//
// #674 argued, correctly, that the directory must be TRAVERSABLE and that
// protection belongs on the individual file. It then reached for 0755, which
// grants traversal (x) AND LISTING (r). Only the x is load-bearing: every
// reader #674 lists by name (PASSWD, GROUP, CACERTS.PEM) is opened by its full
// path, and a tree-wide grep of userland/ finds NO opendir("/CONFIG") at all,
// so nothing legitimate enumerates this directory. The r bit was paid for
// nothing.
//
// What it cost: with 0755 the protection model is an ENUMERATED ALLOWLIST, and
// the attacker gets to read the list. Anything not in perms_system_seed[] falls
// to perms_check()'s world-readable no-entry default, and an attacker can find
// those files by listing the directory rather than having to guess names.
// /CONFIG/MFA.DB is exactly such a file: created at enrolment (so it is absent
// from the golden and from every audit of that directory), holding TOTP seeds
// under a single fixed XOR byte, and world-readable the moment it exists.
//
// 0711 keeps every property #674 argues for and removes the enumeration.
// MEASURED, not assumed: rustkern/permpath.rs requires only X_OK on each
// intermediate component and applies the caller's requested access to the leaf
// alone, and sys_open_k() asks for R_OK on the object being opened, so a
// directory at 0711 can be traversed to a known child but cannot be opened for
// readdir. perms_selftest() asserts that split on the live database every boot.
//
// HONEST LIMIT, so nobody mistakes this for more than it is: 0711 hides the
// NAME LIST, not existence. SYS_STAT (proc/syscall.c sys_stat_path) performs no
// permission check whatsoever, so a caller who GUESSES a name still learns
// whether it exists and how big it is, and SYS_FS_PERM_INFO will still report
// its mode. This change removes the easy enumeration; it does not close the
// existence oracle. Those two syscalls are recorded in blame.md as the next
// piece of work, and are deliberately NOT changed here because adding a
// permission check to stat has a far wider blast radius than one directory mode
// and deserves its own change and its own verification.
//
// KNOWN DIVERGENCE, measured from the code: sys_chdir() (proc/syscall.c)
// validates by calling sys_open_k(path, 0), which asks for R_OK. POSIX chdir
// requires only search permission, so a non-root process cannot cd into a 0711
// directory on this system even though it can traverse it. That is a
// pre-existing bug in chdir rather than in this mode choice, it affects every
// 0711 directory and not just this one, and fixing it means changing a
// validation path used by every chdir in the system. Left alone here on
// purpose; recorded in blame.md.
//
// This is the one place the seed's "an operator chmod always wins" rule is
// relaxed, so it is deliberately narrow: it rewrites /CONFIG ONLY when it is
// EXACTLY the known-bad shipped value, root:root 0700. Any other owner, or any
// other mode, is somebody's decision and is left untouched.
static void perms_fix_config_dir(void) {
    uint32_t uid = 0, gid = 0;
    uint16_t mode = 0;
    if (perms_get("/CONFIG", &uid, &gid, &mode) != 0) return;  // no entry: seed/default handles it
    if (uid != 0 || gid != 0) return;                          // somebody else owns it: leave it
    uint16_t cur = mode & 0777;
    // 0700 is the known-bad SHIPPED value. 0755 is the value THIS FUNCTION
    // itself wrote between #674 and #745, so migrating it is not overriding an
    // operator decision, it is finishing our own. Any OTHER mode is somebody's
    // choice and is left untouched, which keeps the seed's "an operator chmod
    // always wins" rule intact.
    if (cur != 0700 && cur != 0755) return;

    perms_set("/CONFIG", 0, 0, 0711);
    kprintf("[PERMS] #745: /CONFIG %04o -> 0711 (searchable, NOT listable); "
            "secrets protected by their own 0600 entries\n", cur);
}

// ===========================================================================
// #674 BOOT SELF-TEST
// ===========================================================================
// blame.md's most-repeated lesson is that in-tree prose lies: a control is only
// real if you watched it fire. This runs on EVERY boot, against the LIVE
// permission database (not a synthetic one), and it exercises perms_check()
// itself rather than a re-implementation of it, so a bug in the walker cannot
// hide behind a test that shares it.
//
// It asserts BOTH directions. A test that only checks denials passes trivially
// if the checker denies everything, which would be a total outage dressed up as
// a security win.
//
// It does not modify the database, and it uses uid 1000/1002 (admin/ref, the
// accounts /CONFIG/PASSWD actually ships) rather than inventing users.
extern int perms_canon_rs(const char *src, char *out, uint32_t cap);
extern int perm_home_shape_rs(const char *path);   // rustkern/permhome.rs
extern int permhome_selftest_rs(void);             // rustkern/permhome.rs
extern int selftestreg_selftest_rs(void);          // rustkern/selftestreg.rs

static int st_fail;
static int st_ran;      // vectors actually EXECUTED, so "PASS" carries a size
static int st_notrun;   // groups that declined, so "PASS" cannot hide one

// #PERMSKIP: every line below goes to bootlog_write(), not kprintf().
//
// This is a security self-test. Its verdict is worth exactly as much as the
// number of machines it can be read on, and kprintf() reaches a serial port
// only: serial is silent in GUI mode, and the two targets whose evidence
// actually matters (the owner's ASUS laptop, the iMac14,4) have no serial port
// at all. bootlog_write() mirrors to serial anyway and replays into
// /BOOTLOG.TXT once the root volume is writable, so choosing it costs nothing.
// perms_selftest() is called from perms_init(), not from main.c, which is the
// only reason kernel/tools/diaglog-gate was not already failing the build over
// this: that gate scopes to main.c's callees.
static void st_canon(const char *in, const char *want) {
    char got[256];
    int n = perms_canon_rs(in, got, sizeof(got));
    st_ran++;
    if (n < 0 || strcmp(got, want) != 0) {
        bootlog_write("[PERMS-SELFTEST] FAIL canon(\"%s\") = \"%s\" (want \"%s\")",
                      in, n < 0 ? "<error>" : got, want);
        st_fail++;
    }
}

static void st_check(const char *path, uint32_t uid, uint32_t gid, int access,
                     int want, const char *why) {
    int got = perms_check(path, uid, gid, access);
    st_ran++;
    // perms_check returns 0 (allow) or -1 (deny); compare on the sign only.
    if ((got == 0) != (want == 0)) {
        bootlog_write("[PERMS-SELFTEST] FAIL %s uid=%u access=%d -> %d (want %d) [%s]",
                      path, uid, access, got, want, why);
        st_fail++;
    }
}

// ===========================================================================
// #PERMSKIP: FINDING THE HOME TO TEST, INSTEAD OF NAMING ONE.
// ===========================================================================
// THE DEFECT THIS REPLACES. The traversal vectors were armed by the literal
// string "/HOME/ADMIN". The first-boot wizard lets the owner name the account
// and proc/users.c derives the home from that name, so an owner called "james"
// gets /HOME/JAMES and /HOME/ADMIN never exists. The kernel therefore printed
//
//     [PERMS-SELFTEST] SKIP traversal vectors (/HOME/ADMIN not 1000:0750)
//
// on every boot of a CORRECTLY PROVISIONED machine, forever, in a line that
// reads like routine noise about an unfinished image. MEASURED on golden build
// 2234: the shipped /CONFIG/PERMS.DB holds three entries and /CONFIG/PASSWD is
// empty, so the vectors could not run before provisioning either. The
// directory-traversal half of #674 - the half that stops one account reading
// another's home - has probably never been exercised on a real user's machine.
//
// DO NOT "FIX" THIS BY LOOKING AT THE INODE. perms_check() never consults the
// ext2 inode; perms_lookup() is a pure hash-table lookup and /CONFIG/PERMS.DB
// is the only source of truth. A previous attempt spent its time discovering
// that debugfs-set ext2 uid/mode changes nothing here.
//
// So the homes are DISCOVERED, from the live database, by SHAPE (rustkern/
// permhome.rs: "/HOME/" plus exactly one component). Whatever the owner called
// the account, it is found.
#define ST_MAX_HOMES 8

// The "some other account" identity used by the deny-direction vectors.
//
// It used to be a hardcoded 1002 ("ref"), an account this image does not ship
// and may never have. Worse, 1002 is a GUESS about a gid as well as a uid: if
// it ever collided with the home's own gid, the 0750 GROUP bits would ALLOW
// and the vector would quietly assert the opposite of what it reads as, which
// is a security test that passes by being wrong. 65534:65534 (the conventional
// "nobody") cannot be 0, and cannot collide with a real account here because
// uids start at 1000 and MAX_USERS is 32. The two decrements below make that
// total rather than merely overwhelmingly likely.
#define ST_OTHER_UID 65534u
#define ST_OTHER_GID 65534u

typedef struct {
    char     path[256];
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
} st_home_t;

// Walk the LIVE hash table for user home directories. Not a lookup of a name
// we chose: an enumeration of what this machine actually has.
//
// uid < 1000 is excluded on purpose. perms_check() returns 0 on its FIRST LINE
// for uid 0, so a root-owned home would make every owner-direction vector pass
// without consulting the walker at all: a group of assertions structurally
// incapable of failing. If a home IS root-owned, excluding it here is what
// turns the situation into a loud not-run instead.
static int st_collect_homes(st_home_t *out, int max) {
    int n = 0;
    for (int b = 0; b < PERM_TABLE_SIZE && n < max; b++) {
        for (perm_entry_t *e = perm_table[b]; e && n < max; e = e->next) {
            if (!perm_home_shape_rs(e->path)) continue;
            if (e->uid < 1000) continue;
            int i = 0;
            while (e->path[i] && i < (int)sizeof(out[n].path) - 1) {
                out[n].path[i] = e->path[i];
                i++;
            }
            out[n].path[i] = '\0';
            out[n].uid  = e->uid;
            out[n].gid  = e->gid;
            out[n].mode = e->mode;
            n++;
        }
    }
    return n;
}

// The traversal vectors, for ONE home, whatever it is called.
//
// The subject is a child path with NO ENTRY OF ITS OWN. That is the whole
// point of #674: the child is protected by its PARENT's mode, which is exactly
// what the pre-#674 exact-path lookup could not do. If every probe name we try
// already has an entry, there is no such child left and the group has to
// decline rather than test something else.
static void st_home_vectors(const st_home_t *h, const char *ctx) {
    static const char *cands[3] = { "/NOSUCH.STV", "/NOSUCH2.STV", "/NOSUCH3.STV" };
    char child[256];
    int have_child = 0;

    for (int c = 0; c < 3 && !have_child; c++) {
        int hl = 0; while (h->path[hl]) hl++;
        int cl = 0; while (cands[c][cl]) cl++;
        if (hl + cl + 1 > (int)sizeof(child)) break;
        for (int i = 0; i < hl; i++) child[i] = h->path[i];
        for (int i = 0; i <= cl; i++) child[hl + i] = cands[c][i];
        uint32_t cu = 0, cg = 0; uint16_t cm = 0;
        if (perms_get(child, &cu, &cg, &cm) != 0) have_child = 1;
    }
    if (!have_child) {
        selftest_notrun("perms/traversal",
                        "every probe name already has its own PERMS.DB entry, so no "
                        "path is left that is protected ONLY by its parent directory");
        st_notrun++;
        return;
    }

    uint32_t ou = ST_OTHER_UID, og = ST_OTHER_GID;
    if (ou == h->uid) ou--;
    if (og == h->gid) og--;

    // POLICY, ASSERTED RATHER THAN READ OFF THE ENTRY.
    //
    // proc/users.c creates every home 0750 and kernel/main.c re-asserts
    // 0750 at every login (#745), so a home that is not 0750 is drift from
    // the model. The vectors below therefore assert "other is refused"
    // UNCONDITIONALLY, and a world-searchable home FAILS them.
    //
    // Deriving the expectation from the mode instead - "deny if the o bits are
    // clear, allow if they are set" - would produce a test that agrees with
    // whatever it finds. That is the same shape as a counter incremented only
    // on the success path: it can never see the fault it is named after. If a
    // machine's home really is 0755, that is a finding about the machine, and
    // the right behaviour is to say so every boot until somebody fixes it.
    if ((h->mode & 0777) != 0750) {
        bootlog_write("[PERMS-SELFTEST] POLICY FAIL (%s): home '%s' is %04o, not 0750 "
                      "(owner %u:%u). Any bit granted to OTHER exposes this home to "
                      "every other account on the machine.",
                      ctx, h->path, (unsigned)(h->mode & 0777),
                      (unsigned)h->uid, (unsigned)h->gid);
        st_fail++;
    }

    st_check(child, ou, og, R_OK, -1,
             "no x on 0750 parent for other: child inherits protection");
    st_check(child, h->uid, h->gid, R_OK, 0,
             "owner traverses its own 0750 home");
    // #676: the exact predicate the O_CREAT gate consults. Creating a NAME is a
    // write to the PARENT DIRECTORY, so sys_open_k() asks
    // perms_check(parent, W_OK | X_OK).
    st_check(h->path, h->uid, h->gid, W_OK | X_OK, 0,
             "#676: create in OWN HOME permitted (this is what #679 unblocks)");
    st_check(h->path, ou, og, W_OK | X_OK, -1,
             "#676: create in ANOTHER user's home refused");
}

static void st_summary(const char *ctx) {
    if (st_fail == 0 && st_notrun == 0) {
        bootlog_write("[PERMS-SELFTEST] #674 PASS (%s): %d vector(s), "
                      "canonicalization + directory traversal enforced", ctx, st_ran);
    } else if (st_fail == 0) {
        // NOT a pass. A group that declined is the failure mode this whole
        // change exists to make visible, and it must not be reported in the
        // same word as a clean run.
        bootlog_write("[PERMS-SELFTEST] #674 *** NOT FULLY VERIFIED *** (%s): "
                      "%d vector(s) passed but %d group(s) DID NOT RUN",
                      ctx, st_ran, st_notrun);
    } else {
        bootlog_write("[PERMS-SELFTEST] #674 *** %d FAILURE(S) *** (%s, %d vector(s) "
                      "run, %d group(s) did not run) permission model is NOT correct",
                      st_fail, ctx, st_ran, st_notrun);
    }
}

// ===========================================================================
// #674 BOOT SELF-TEST
// ===========================================================================
// blame.md's most-repeated lesson is that in-tree prose lies: a control is only
// real if you watched it fire. This runs on EVERY boot, against the LIVE
// permission database (not a synthetic one), and it exercises perms_check()
// itself rather than a re-implementation of it, so a bug in the walker cannot
// hide behind a test that shares it.
//
// It asserts BOTH directions. A test that only checks denials passes trivially
// if the checker denies everything, which would be a total outage dressed up as
// a security win.
//
// It does not modify the database.
void perms_selftest(void) {
    st_fail = 0;
    st_ran = 0;
    st_notrun = 0;

    // The two pure Rust helpers this test now leans on, proven BEFORE they are
    // used to select anything. A shape predicate that matched nothing would
    // recreate the exact bug being fixed, and would do it silently: the homes
    // would simply stop being found and the group would decline forever.
    {
        int bad = permhome_selftest_rs();
        if (bad) {
            bootlog_write("[PERMS-SELFTEST] FAIL home-shape predicate: %d bad vector(s); "
                          "home discovery cannot be trusted this boot", bad);
            st_fail += bad;
        } else {
            selftest_ran("perms/homeshape");
        }
        bad = selftestreg_selftest_rs();
        if (bad) {
            bootlog_write("[PERMS-SELFTEST] FAIL not-run register: %d bad vector(s)", bad);
            st_fail += bad;
        } else {
            selftest_ran("selftest/register");
        }
    }

    // --- canonicalization: the walker must see the object the FS will open ---
    st_canon("/CONFIG/SHADOW",            "/CONFIG/SHADOW");
    st_canon("/CONFIG/../CONFIG/SHADOW",  "/CONFIG/SHADOW");  // ".." popped
    st_canon("/CONFIG/./SHADOW",          "/CONFIG/SHADOW");  // "." dropped
    st_canon("//CONFIG//SHADOW",          "/CONFIG/SHADOW");  // "//" collapsed
    st_canon("CONFIG/SHADOW",             "/CONFIG/SHADOW");  // made absolute
    st_canon("/A/B/../../C",              "/C");
    st_canon("/..",                       "/");               // never above root
    st_canon("/../../..",                 "/");
    st_canon("/",                         "/");
    st_canon("/CONFIG/",                  "/CONFIG");         // trailing slash
    selftest_ran("perms/canon");

    // --- enforcement, on the live database --------------------------------
    // Guard on the entries this test reasons about, so a hand-edited PERMS.DB
    // produces a clear not-run rather than a misleading FAIL.
    uint32_t u = 0, g = 0; uint16_t m = 0;
    if (perms_get("/CONFIG/KIMI.KEY", &u, &g, &m) == 0 && u == 0 && (m & 0777) == 0600) {
        // The whole point of #674: three spellings of ONE object, all denied.
        st_check("/CONFIG/KIMI.KEY",           1000, 1000, R_OK, -1, "0600 root, other has no r");
        st_check("/CONFIG/../CONFIG/KIMI.KEY", 1000, 1000, R_OK, -1, "dotdot bypass closed");
        st_check("/CONFIG/./KIMI.KEY",         1000, 1000, R_OK, -1, "dot bypass closed");
        st_check("//CONFIG//KIMI.KEY",         1000, 1000, R_OK, -1, "double-slash bypass closed");
        st_check("CONFIG/KIMI.KEY",            1000, 1000, R_OK, -1, "relative bypass closed");
        // The uid-0 early-out must be EXACTLY as it was before #674.
        st_check("/CONFIG/KIMI.KEY",              0,    0, R_OK,  0, "root bypass preserved");
        selftest_ran("perms/secret");
    } else {
        selftest_notrun("perms/secret",
                        "/CONFIG/KIMI.KEY is not root-owned 0600 in PERMS.DB (it is absent "
                        "from images that ship no API key), so the dot/dotdot/double-slash "
                        "bypass vectors against a real 0600 secret did not run");
        st_notrun++;
    }

    // The other direction: /CONFIG must stay TRAVERSABLE and its public files
    // readable, or #674 is an outage rather than a fix.
    if (perms_get("/CONFIG", &u, &g, &m) == 0 &&
        ((m & 0777) == 0755 || (m & 0777) == 0711)) {
        st_check("/CONFIG",        1000, 1000, X_OK, 0, "/CONFIG searchable (like /etc)");
        st_check("/CONFIG/PASSWD", 1000, 1000, R_OK, 0, "uid->name lookup must keep working");
        selftest_ran("perms/configopen");
    } else {
        selftest_notrun("perms/configopen",
                        "/CONFIG is neither 0755 nor 0711 in PERMS.DB, so the "
                        "must-stay-usable direction of #674 did not run and this kernel "
                        "has not checked that it is not simply denying everything");
        st_notrun++;
    }

    // Traversal proper: a directory that denies search to others must protect a
    // child that has NO entry of its own. This is precisely what the pre-#674
    // exact-path lookup could not do.
    //
    // #PERMSKIP: over EVERY home this machine actually has, discovered by
    // shape. See st_collect_homes() above for why this is no longer a literal
    // path, and why zero homes is a loud not-run rather than a quiet SKIP.
    {
        st_home_t homes[ST_MAX_HOMES];
        int nh = st_collect_homes(homes, ST_MAX_HOMES);
        if (nh == 0) {
            selftest_notrun("perms/traversal(boot)",
                            "no user home directory exists in /CONFIG/PERMS.DB, so the "
                            "directory-traversal half of the permission model is UNVERIFIED. "
                            "Expected on a virgin image before the first-boot wizard; a "
                            "DEFECT on any machine that has an account");
            st_notrun++;
        } else {
            for (int i = 0; i < nh; i++) {
                bootlog_write("[PERMS-SELFTEST] traversal vectors on home %d/%d: "
                              "'%s' %u:%u %04o", i + 1, nh, homes[i].path,
                              (unsigned)homes[i].uid, (unsigned)homes[i].gid,
                              (unsigned)(homes[i].mode & 0777));
                st_home_vectors(&homes[i], "boot");
            }
            selftest_ran("perms/traversal(boot)");
        }
    }

    // #676: the exact predicate the O_CREAT gate consults. Creating a NAME is a
    // write to the PARENT DIRECTORY, so sys_open_k() (and open_redir_file())
    // ask perms_check(parent, W_OK | X_OK). Asserted here in BOTH directions on
    // the live database, because the allow direction alone is what a boot
    // happens to exercise.
    //
    // ===================================================================
    // #229 CORRECTION. THE JUSTIFICATION THIS ASSERTION CARRIED WAS FALSE,
    // AND IT WAS FALSE ABOUT THE ONE THING IT WAS ASSERTING.
    // ===================================================================
    // It read: "the desktop creates files in its own home and NEVER TRIES TO
    // CREATE ONE IN /CONFIG, so nothing in a normal boot log shows the gate
    // REFUSING."
    //
    // /APPS/SETUP, the first-boot wizard, tried FOUR TIMES on every virgin
    // boot: /CONFIG/SETUPDONE, /CONFIG/SETUPSKIP, /CONFIG/SETUPNEW and
    // /CONFIG/NETIP.CFG. The boot log did not merely SHOW the refusal, it
    // showed it as the reason a virgin machine could not be set up and could
    // not reach a desktop (#226, measured on golden 2011):
    //
    //     [PERMS-DENY] proc=SETUP uid=1000 gid=1000 want=-wx path=/CONFIG
    //
    // A PASSING SELF-TEST WHOSE JUSTIFICATION IS FALSE IS A BUG REPORT THAT
    // PRINTS PASS EVERY BOOT. The refusal being asserted here was the live
    // symptom of the top-severity defect on the tracker, and the sentence
    // beside it said the situation could not arise. Whoever read this block
    // while chasing that bug was told, by the kernel's own self-test, to look
    // elsewhere.
    //
    // THE ASSERTION ITSELF IS CORRECT AND IS KEPT UNCHANGED. /CONFIG holds
    // SHADOW, AUTHKEYS, SSHD.CFG and the owner's API keys; a uid-1000 process
    // creating names in it is exactly what must not happen, and #745 spent a
    // whole change tightening this directory (0755 -> 0711) rather than
    // loosening it. What was wrong was the CLAIM ABOUT REALITY, not the
    // POLICY. #229 fixed the caller, not the gate: the wizard now asks the
    // kernel for first-run state through SYS_FIRSTRUN
    // (kernel/rustkern/firstrun.rs), which owns the set of legal keys and
    // writes them from Ring 0, so there is no longer anything on the machine
    // that wants to create a name here.
    //
    // HONEST SCOPE, unchanged: this proves the decision function, not the
    // syscall wiring. The wiring is evidenced separately by a uid-1000 session
    // creating <home>/UIPROFIL.YML while still being refused /CONFIG - and,
    // since #229, by the wizard completing every step without one
    // [PERMS-DENY] line.
    st_check("/CONFIG", 1000, 1000, W_OK | X_OK, -1,
             "#676/#229: create in /CONFIG refused (root-owned, no w for other)");
    st_check("/",       1000, 1000, W_OK | X_OK, -1,
             "#676: create in / refused (root-owned 0755)");
    selftest_ran("perms/create");

    st_summary("boot");
}

// ===========================================================================
// #PERMSKIP: THE SESSION-SCOPED RUN.
// ===========================================================================
// perms_init() runs at main.c:2934 and users_init() at :2940, so at boot time
// there is no session and no loaded user database: the boot run above can only
// enumerate what PERMS.DB already holds. This one runs from the login path,
// where kernel/main.c has just claimed the home for the session identity, so it
// tests THE ACTUAL USER'S HOME, under THE ACTUAL USER'S uid/gid, whatever the
// owner chose to call the account. That is the definitive run.
//
// ROOT SESSIONS. Root's home is "/", and the filesystem root is not a home
// directory: it is root:root 0755 by design and every account must be able to
// traverse it, so asserting "another account cannot search it" would assert the
// opposite of the model. Stating that here rather than leaving a third silent
// skip: for a root session the vectors run against every non-root home the
// database holds, and if there are none, that is a LOUD not-run.
void perms_selftest_session(const char *home, uint32_t uid, uint32_t gid) {
    st_fail = 0;
    st_ran = 0;
    st_notrun = 0;

    int root_home = (home == NULL || home[0] == '\0' ||
                     (home[0] == '/' && home[1] == '\0'));

    if (root_home) {
        st_home_t homes[ST_MAX_HOMES];
        int nh = st_collect_homes(homes, ST_MAX_HOMES);
        if (nh == 0) {
            selftest_notrun("perms/traversal(session)",
                            "session user is root, whose home is the filesystem root and is "
                            "not a home directory, and PERMS.DB holds no non-root home to "
                            "test instead: no account on this machine has a protected home");
            st_notrun++;
        } else {
            bootlog_write("[PERMS-SELFTEST] session user is uid %u with home '/' (root); "
                          "the filesystem root is not a home directory, so the traversal "
                          "vectors run against the %d non-root home(s) in PERMS.DB",
                          (unsigned)uid, nh);
            for (int i = 0; i < nh; i++) st_home_vectors(&homes[i], "session/root");
            selftest_ran("perms/traversal(session)");
        }
    } else {
        st_home_t h;
        uint32_t u = 0, g = 0; uint16_t m = 0;
        if (perms_get(home, &u, &g, &m) != 0) {
            selftest_notrun("perms/traversal(session)",
                            "the session user's home has NO entry in /CONFIG/PERMS.DB, so "
                            "nothing is protecting it and there is nothing to verify; "
                            "main.c claims it at every login (#745) and that did not happen");
            st_notrun++;
        } else {
            int i = 0;
            while (home[i] && i < (int)sizeof(h.path) - 1) { h.path[i] = home[i]; i++; }
            h.path[i] = '\0';
            h.uid = u; h.gid = g; h.mode = m;

            // The database entry must BE the session identity. If it is not,
            // the login-time claim did not take, and every vector below would
            // be testing somebody else's ownership while reading as though it
            // had tested this session's.
            if (u != uid || g != gid) {
                bootlog_write("[PERMS-SELFTEST] FAIL session home '%s' is owned %u:%u but the "
                              "session is %u:%u; the login-time claim (#745) did not take",
                              home, (unsigned)u, (unsigned)g,
                              (unsigned)uid, (unsigned)gid);
                st_fail++;
            }
            // The account name is the tail of the home path, but ONLY if the
            // path really has the "/HOME/<name>" shape. Printing h.path + 6
            // unconditionally would read past the NUL of a shorter path and
            // put uninitialized kernel stack bytes into a log that is written
            // to disk. Every home users.c produces has that shape today; "it
            // has the right shape in practice" is precisely the reasoning this
            // whole change exists to stop trusting.
            const char *who = perm_home_shape_rs(h.path) ? (h.path + 6) : "?";
            bootlog_write("[PERMS-SELFTEST] session home '%s' %u:%u %04o (user '%s' as "
                          "provisioned, not a hardcoded name)",
                          h.path, (unsigned)h.uid, (unsigned)h.gid,
                          (unsigned)(h.mode & 0777), who);
            st_home_vectors(&h, "session");
            selftest_ran("perms/traversal(session)");
        }
    }

    st_summary("session");
}

static void perms_seed_system(void) {
    unsigned ns = sizeof(perms_shared_state_seed) / sizeof(perms_shared_state_seed[0]);
    for (unsigned i = 0; i < ns; i++) {
        if (perms_lookup(perms_shared_state_seed[i].path)) continue;   // operator wins
        perms_set(perms_shared_state_seed[i].path, 0, 0,
                  perms_shared_state_seed[i].mode);
    }
    unsigned n = sizeof(perms_system_seed) / sizeof(perms_system_seed[0]);
    unsigned added = 0;
    for (unsigned i = 0; i < n; i++) {
        if (perms_lookup(perms_system_seed[i].path)) continue;  // operator wins
        perms_set(perms_system_seed[i].path, 0, 0, perms_system_seed[i].mode);
        added++;
    }
    if (added) {
        kprintf("[PERMS] #670: seeded %u system entries (kernel image + diagnostics)\n",
                added);
    }
}

void perms_init(void) {
    kprintf("[PERMS] Initializing permissions database...\n");

    // Clear hash table
    memset(perm_table, 0, sizeof(perm_table));
    perm_pool_next = 0;
    perms_dirty = false;

    // Try to load /CONFIG/PERMS.DB
    if (!g_fat_fs.mounted) {
        kprintf("[PERMS] No filesystem mounted, using defaults\n");
        perms_initialized = true;
        return;
    }

    // Ensure /CONFIG directory exists
    fat_mkdir(&g_fat_fs, "/CONFIG");

    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, "/CONFIG/PERMS.DB", &size);
    if (data && size > 0) {
        kprintf("[PERMS] Loading permissions from /CONFIG/PERMS.DB (%u bytes)\n", size);

        // Parse line by line
        char line[512];
        int line_pos = 0;
        const char *src = (const char *)data;
        int entry_count = 0;

        for (uint32_t i = 0; i <= size; i++) {
            if (i == size || src[i] == '\n' || src[i] == '\r') {
                line[line_pos] = '\0';
                if (line_pos > 0) {
                    parse_perms_line(line);
                    entry_count++;
                }
                line_pos = 0;
            } else if (line_pos < (int)sizeof(line) - 1) {
                line[line_pos++] = src[i];
            }
        }

        kfree(data);
        kprintf("[PERMS] Loaded %d permission entries\n", entry_count);
        perms_dirty = false;  // Just loaded, not dirty
    } else {
        kprintf("[PERMS] No PERMS.DB found, creating defaults\n");

        // Set up default permissions
        perms_set("/", 0, 0, 0755);
        perms_set("/APPS", 0, 0, 0755);
        perms_set("/BOOT", 0, 0, 0755);
        perms_set("/CONFIG", 0, 0, 0700);
        perms_set("/CONFIG/SHADOW", 0, 0, 0600);
        perms_set("/HOME", 0, 0, 0755);

        if (perms_sync() != 0)
            kprintf("[PERMS] sync failed (entries stay dirty and will be retried)\n");
    }

    // #670: applies to BOTH branches above, so a PERMS.DB that predates this
    // change gets the missing entries too. perms_sync() no-ops when nothing was
    // added (it early-returns on !perms_dirty).
    perms_seed_system();
    perms_fix_config_dir();   // #674: must run AFTER the seed (it reads /CONFIG)
    if (perms_sync() != 0)
        kprintf("[PERMS] sync failed (entries stay dirty and will be retried)\n");

    perms_initialized = true;
    perms_selftest();         // #674: proves the walker on the LIVE database
    {   // #58: and the CWD RESOLVER, which shares this file's canonicalizer.
        // "A rule that has never been watched being right is a comment"
        // (dos/dosexec.c). The cases include the exact golden-1811 failure
        // recorded in blame.md - mkdir("texpacks") under a chdir'd cwd landing
        // at the filesystem root and reporting success - plus the
        // ".. cannot escape the root" case that is what makes cwd resolution
        // safe to expose to Ring 3 at all, and the cwd="/" case that proves a
        // process which never calls chdir sees no change whatsoever.
        extern int path_resolve_selftest_rs(void);
        int rbad = path_resolve_selftest_rs();
        kprintf("[PERMS] #58 cwd path-resolution selftest: %s (%d failing)\n",
                rbad == 0 ? "PASS" : "FAIL", rbad);
        // The END-TO-END half does NOT run here. It needs a current process to
        // carry a cwd, and perms_init() runs long before the first one exists:
        // wired here it printed "[#58] e2e SKIPPED: no current process" on
        // every boot. It is latched to the first Ring-3 open instead
        // (proc/fdlayer.c). Keeping the loud-skip line was what surfaced this;
        // a test that skipped quietly would have read exactly like a pass.
    }
    kprintf("[PERMS] Permissions database ready\n");
}

// #679: WRITE-BACK OF RUNTIME OWNERSHIP.
//
// perms_sync() had exactly three callers, all of them one-shot: perms_init(),
// chmod, and user administration. Nothing flushed the table during ordinary
// operation. So create-time ownership recorded by perms_on_create() lived in
// RAM ONLY and was lost at the next boot, at which point the file would fall
// back to perms_check()'s root-owned no-entry default and its creator would no
// longer be able to write it. MEASURED: after a uid-1000 boot the compositor
// successfully created /HOME/ADMIN/UIPROFIL.YML, and /CONFIG/PERMS.DB still
// held the same 34 entries it had at boot, with no entry for the new file.
// The fix would have looked correct for exactly one boot and then regressed,
// which is the failure shape blame.md keeps recording.
//
// COALESCED, not per-create. perms_sync() rewrites the WHOLE database as one
// file, so calling it from the create path would turn a thousand-file App Store
// install into a thousand full-database writes. Instead the existing heartbeat
// worker (main.c heartbeat_worker, a real thread on a timer that already
// performs file I/O) calls this, and perms_sync() early-returns when nothing is
// dirty. No new thread, no new timer, and no poll loop for the concurrency lint
// to object to.
//
// LOCKING, honestly: the perms table has no lock, and this adds a periodic
// READER concurrent with syscall-context writers. That is safe for the reader
// because perms_set() fully initializes an entry BEFORE linking it at the head
// of its chain, and x86-64 store ordering (TSO) does not reorder those two
// stores, so a walker sees either the old head or a fully-formed new entry. It
// can miss an entry that appears mid-walk; that entry is still dirty and is
// written by the next flush. What this does NOT fix is the pre-existing
// writer/writer race in alloc_entry()'s non-atomic perm_pool_next++, which two
// concurrent creates could tear. That is untouched by this change and is
// recorded here rather than silently inherited.
int perms_sync_if_dirty(void) {
    return perms_sync();   // early-returns unless perms_dirty
}

// #693: returns 0 only if /CONFIG/PERMS.DB is on the medium. The important part
// is NOT the return value, it is that perms_dirty is now cleared ONLY on
// success. Clearing it after a failed write is the exact bug #695 documented:
// the dirty flag is the only record that these entries still need writing, so
// dropping it converts a retryable failure into permanent, silent loss.
int perms_sync(void) {
    if (!perms_dirty || !g_fat_fs.mounted) return 0;

    kprintf("[PERMS] Syncing permissions to disk...\n");

    // Build the file content
    char *buf = kmalloc(64 * 1024);  // 64KB buffer
    if (!buf) {
        kprintf("[PERMS] Failed to allocate sync buffer\n");
        return -1;
    }

    int pos = 0;
    int count = 0;

    for (int i = 0; i < PERM_TABLE_SIZE; i++) {
        perm_entry_t *e = perm_table[i];
        while (e) {
            // Format: /PATH:UID:GID:MODE\n
            char mode_str[8];
            format_octal(e->mode, mode_str);

            // Build line manually
            int line_len = strlen(e->path);
            if (pos + line_len + 32 >= 64 * 1024) break;  // Buffer full

            memcpy(buf + pos, e->path, line_len);
            pos += line_len;
            buf[pos++] = ':';

            // UID
            char num[16];
            int n = 0;
            uint32_t v = e->uid;
            if (v == 0) { num[n++] = '0'; }
            else {
                char tmp[16]; int t = 0;
                while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
                while (t > 0) num[n++] = tmp[--t];
            }
            memcpy(buf + pos, num, n);
            pos += n;
            buf[pos++] = ':';

            // GID
            n = 0;
            v = e->gid;
            if (v == 0) { num[n++] = '0'; }
            else {
                char tmp[16]; int t = 0;
                while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
                while (t > 0) num[n++] = tmp[--t];
            }
            memcpy(buf + pos, num, n);
            pos += n;
            buf[pos++] = ':';

            // Mode (octal)
            memcpy(buf + pos, mode_str, 4);
            pos += 4;
            buf[pos++] = '\n';

            count++;
            e = e->next;
        }
    }

    // Write to disk
    int rc = fat_write_file(&g_fat_fs, "/CONFIG/PERMS.DB", buf, pos);
    kfree(buf);
    if (rc != 0) {
        kprintf("[PERMS] FAILED to write /CONFIG/PERMS.DB (rc=%d); staying DIRTY "
                "so the next sync retries\n", rc);
        return rc;
    }
    perms_dirty = false;
    kprintf("[PERMS] Synced %d entries to /CONFIG/PERMS.DB\n", count);
    return 0;
}

// ============================================================================
// Permission checking
// ============================================================================

// #674: the SINGLE-OBJECT decision. This is the verbatim pre-#674 body of
// perms_check(), MINUS the early-outs. It is non-static so the Rust path walker
// (rustkern/permpath.rs) can apply it to each component of a resolved path.
//
// It deliberately does NOT carry the uid-0 or !perms_initialized bypasses:
// perms_check() applies those before it ever calls the walker, so this entry
// point can never become a way around them.
//
// `path` must already be CANONICAL (absolute, no "." / ".." / "//"). The only
// normalization applied is perms_lookup()'s uppercasing, which is what makes
// "/boot/kernel.elf" and "/BOOT/KERNEL.ELF" a single key.
int perms_check_leaf(const char *path, uint32_t proc_uid, uint32_t proc_gid, int access) {
    perm_entry_t *e = perms_lookup(path);
    if (!e) {
        // No entry means default permissions: owned by root, mode 0755
        // Everyone can read/execute, only root can write
        if (access & W_OK) return -1;  // EACCES
        return 0;
    }

    uint16_t mode = e->mode;
    uint16_t bits;

    if (proc_uid == e->uid)       bits = (mode >> 6) & 7;  // Owner bits
    else if (proc_gid == e->gid)  bits = (mode >> 3) & 7;  // Group bits
    else                          bits = mode & 7;          // Other bits

    if ((access & R_OK) && !(bits & 4)) return -1;  // EACCES
    if ((access & W_OK) && !(bits & 2)) return -1;  // EACCES
    if ((access & X_OK) && !(bits & 1)) return -1;  // EACCES
    return 0;
}

// rustkern/permpath.rs. Canonicalizes `path` the way the filesystem resolves it
// (absolute, "." dropped, ".." popped, "//" collapsed), then requires search
// (x) on the root and on every intermediate directory component before applying
// `access` to the object itself. Returns 0 allow / -1 deny / -2 too-long.
extern int perms_path_check_rs(const char *path, uint32_t uid, uint32_t gid, int access);
#define PERMS_ETOOLONG (-2)

// Cap on [PERMS-DENY] console lines for the lifetime of the boot. See the
// comment at the emit site for why this is a hard cap and not a time-based
// rate limit.
#define PERMS_DENY_LOG_MAX 200u

int perms_check(const char *path, uint32_t proc_uid, uint32_t proc_gid, int access) {
    // Root bypasses all checks
    if (proc_uid == 0) return 0;

    // Kernel processes (called before perms_init) always pass
    if (!perms_initialized) return 0;

    // #745 ELEVATION GRANT. The compositor drew a trusted prompt, the person at
    // the keyboard proved they are the owner of this session, and the kernel
    // issued this process a grant. It is as narrow as a privilege can be made
    // here: ONE process (a field on process_t that Ring 3 has no syscall to
    // write), ONE path prefix (a kernel constant, never anything the app said),
    // and a bounded deadline. elev_path_covered_rs() fails CLOSED on any path
    // that is not a plain absolute path under that prefix, so "/APPS/../CONFIG/
    // SHADOW" is refused rather than normalised. See proc/elevate.h.
    //
    // Placed here and not in pkg_write_permit() because the App Store's package
    // members are written with ordinary sys_open/sys_mkdir/chmod, not with
    // SYS_PKG_WRITE; perms_check() is the one place all of them meet.
    {
        extern int elev_grant_permits(const char *path, uint32_t proc_uid);
        if (elev_grant_permits(path, proc_uid)) return 0;
    }

    // #674: POSIX path resolution. Before this, perms_check() was an EXACT-PATH
    // lookup of the one string it was handed: /CONFIG being 0700 did not protect
    // /CONFIG/KIMI.KEY (no entry of its own -> the permissive default above),
    // and neither "/CONFIG/../CONFIG/SHADOW" nor the bare "CONFIG/SHADOW" (which
    // sys_open_k() makes absolute AFTER this check) matched the 0600 entry that
    // was supposed to guard it.
    int r = perms_path_check_rs(path, proc_uid, proc_gid, access);
    if (r == PERMS_ETOOLONG) {
        kprintf("[PERMS] #674: path exceeds %d bytes canonicalized, denying\n",
                (int)sizeof(((perm_entry_t *)0)->path));
        r = -1;
    }
    if (r != 0) {
        // #674: say WHICH process was denied WHICH path. Without this a
        // permission problem reaches userland as a bare EACCES from open() and
        // surfaces as "the AI panel is blank", with nothing on the console
        // connecting it to a mode. Naming the process and the path is the
        // difference between a measurable policy and a mystery.
        //
        // Bounded, not rate-limited by time: a tight retry loop in one app must
        // not be able to flood the serial console (which is the only debug
        // channel on real hardware) or to push earlier, more informative lines
        // out of a captured log. The cap is deliberately generous enough to
        // cover a whole desktop session start.
        static unsigned deny_logged = 0;
        if (deny_logged < PERMS_DENY_LOG_MAX) {
            deny_logged++;
            extern const char *proc_current_name(void);
            kprintf("[PERMS-DENY] proc=%s uid=%u gid=%u want=%c%c%c path=%s\n",
                    proc_current_name(), proc_uid, proc_gid,
                    (access & R_OK) ? 'r' : '-',
                    (access & W_OK) ? 'w' : '-',
                    (access & X_OK) ? 'x' : '-',
                    path);
            if (deny_logged == PERMS_DENY_LOG_MAX) {
                kprintf("[PERMS-DENY] log cap (%u) reached; further denials silent\n",
                        (unsigned)PERMS_DENY_LOG_MAX);
            }
        }
    }
    return r;
}

// ============================================================================
// Permission management
// ============================================================================

void perms_set(const char *path, uint32_t uid, uint32_t gid, uint16_t mode) {
    char norm[256];
    normalize_path(path, norm, sizeof(norm));

    // Check if entry already exists
    perm_entry_t *e = perms_lookup(norm);
    if (e) {
        e->uid = uid;
        e->gid = gid;
        e->mode = mode;
        perms_dirty = true;
        return;
    }

    // Allocate new entry
    e = alloc_entry();
    if (!e) {
        kprintf("[PERMS] Permission table full\n");
        return;
    }

    strncpy(e->path, norm, sizeof(e->path) - 1);
    e->uid = uid;
    e->gid = gid;
    e->mode = mode;

    // Insert at head of hash chain
    uint32_t h = path_hash(norm);
    e->next = perm_table[h];
    perm_table[h] = e;

    perms_dirty = true;
}

void perms_remove(const char *path) {
    char norm[256];
    normalize_path(path, norm, sizeof(norm));

    uint32_t h = path_hash(norm);
    perm_entry_t *prev = NULL;
    perm_entry_t *e = perm_table[h];

    while (e) {
        if (strcmp(e->path, norm) == 0) {
            if (prev) prev->next = e->next;
            else perm_table[h] = e->next;
            // Note: we don't free pool entries (they're a simple bump allocator)
            // In a full implementation, we'd use a free list
            perms_dirty = true;
            return;
        }
        prev = e;
        e = e->next;
    }
}

int perms_get(const char *path, uint32_t *uid, uint32_t *gid, uint16_t *mode) {
    perm_entry_t *e = perms_lookup(path);
    if (!e) return -1;

    if (uid) *uid = e->uid;
    if (gid) *gid = e->gid;
    if (mode) *mode = e->mode;
    return 0;
}

int perms_chmod(const char *path, uint32_t caller_uid, uint16_t mode) {
    // Root can chmod anything
    if (caller_uid == 0) {
        perm_entry_t *e = perms_lookup(path);
        if (e) {
            e->mode = mode;
            perms_dirty = true;
        } else {
            perms_set(path, 0, 0, mode);
        }
        return 0;
    }

    // #745: an elevation grant covers chmod inside its prefix. It has to,
    // because change 2 above makes the installed file ROOT-owned, so the
    // ordinary "must own the file" test below would refuse the installer's own
    // 0555 stamp on the binary it just wrote. Same narrow scope as every other
    // use of the grant: one process, one prefix, one deadline.
    {
        extern int elev_grant_permits(const char *path, uint32_t proc_uid);
        if (elev_grant_permits(path, caller_uid)) {
            perm_entry_t *ge = perms_lookup(path);
            if (ge) { ge->mode = mode; perms_dirty = true; }
            else    { perms_set(path, 0, 0, mode); }
            return 0;
        }
    }

    // Non-root: must own the file
    perm_entry_t *e = perms_lookup(path);
    if (!e) return -1;  // No entry, default root-owned
    if (e->uid != caller_uid) return -1;  // EPERM

    e->mode = mode;
    perms_dirty = true;
    return 0;
}

// ===========================================================================
// #679 prerequisite: CREATE-TIME OWNERSHIP
// ===========================================================================
// THE BUG THIS FIXES, measured under #674 on a uid-1000 session: the compositor
// could not write /HOME/ADMIN/UIPROFIL.YML even though /HOME/ADMIN is
// 1000:1000 0750, i.e. even though it owns that directory outright.
//
// The cause is perms_check()'s no-entry default. A path with no entry is
// treated as ROOT-OWNED, and that branch denies W_OK to everyone who is not
// root. Since nothing but mkdir ever created an entry, EVERY file created by
// EVERY non-root process fell into it. The practical effect was that no
// non-root user could write any file anywhere, including in their own home
// directory, so no non-root session could persist any state at all. That is a
// far bigger obstacle to running the desktop as a normal user than the missing
// path traversal #674 fixed, and it is why enabling non-root would have broken
// the desktop rather than secured it.
//
// Linux does not have this problem because a created inode carries its
// CREATOR's uid/gid from the moment it exists. This does the same thing for the
// perms.c sidecar: at every create point, record the creating process as the
// owner, with the conventional default modes (0644 for a file, 0755 for a
// directory).
//
// WHY IT REFUSES TO OVERWRITE AN EXISTING ENTRY. Several files in this system
// are DELETED AND REWRITTEN in place by Ring 0 on every boot: /BOOTLOG.TXT,
// /PANIC.TXT, /CONFIG/PERMS.DB itself. Those carry deliberate 0600 entries from
// the #670 and #674 seeds, and an unconditional perms_set() here would silently
// downgrade them to 0644 the first time their writer ran, quietly undoing the
// hardening from one directory over. The same rule protects an operator chmod.
// Ownership is therefore established ONCE, when the path first appears; a path
// that already has a policy keeps it.
//
// This lives in C, not Rust, for the same reason perms_check_leaf() does: it is
// three lines against the existing C hash table and its C-internal pool/chain
// layout, and its callers are the existing C syscall handlers. Reimplementing
// the lookup in Rust to add a setter would fork a shared primitive, which is
// the thing CLAUDE.md forbids, to gain nothing.
// IT CANONICALIZES, and that is not incidental. #674's whole finding was that a
// permission key is only meaningful in the form the checker will look it up in:
// perms_check() canonicalizes (absolute, "." dropped, ".." popped, "//"
// collapsed) before it looks anything up, so a create that recorded the raw
// caller-supplied spelling would file the entry under a key that is never
// consulted, and the owner would silently not be the owner. The create points
// see paths in several spellings (a bare name that sys_open_k() only makes
// absolute LATER, an ext2-relative path, a "/ext2/..." prefixed path), which is
// exactly the situation that produces such a mismatch.
void perms_on_create(const char *path, uint32_t uid, uint32_t gid, int is_dir) {
    if (!path || !path[0]) return;

    extern int perms_canon_rs(const char *src, char *out, uint32_t cap);
    char canon[256];
    if (perms_canon_rs(path, canon, sizeof(canon)) < 0) return;  // unusable key

    if (perms_lookup(canon)) return;  // seed, or operator chmod, or re-create: keep it

    // #745: a file created under an ELEVATION GRANT belongs to ROOT, not to the
    // person who authenticated. Without this the modal's own fine print would
    // be false in both directions: "the installer runs as root" would be a
    // sentence with nothing behind it, and "only an administrator can remove
    // it" would be wrong, because the installing user would own every binary in
    // /APPS and could rewrite it in place later with no prompt at all.
    {
        extern int elev_grant_permits(const char *path, uint32_t proc_uid);
        if (elev_grant_permits(canon, uid)) { uid = 0; gid = 0; }
    }
    perms_set(canon, uid, gid, is_dir ? 0755 : 0644);
}

void perms_set_default(const char *path, uint32_t uid, uint32_t gid, int is_dir) {
    uint16_t mode = is_dir ? 0755 : 0644;
    perms_set(path, uid, gid, mode);
}
