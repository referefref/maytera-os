// dospath.c - WINE-dosdevices-style drive-letter filesystem layer (#257)
// See dospath.h for the design + the dos_resolve_path contract.
#include "dospath.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../fs/perms.h"
#include "diskimg.h"

// Root FS access (the fat_* public wrappers route "/" paths to the ext2 root on
// VM <vmid>; on a FAT-root system they hit FAT directly). Same handle used by the
// DOS + Win16 file code.
// #742: fat_mkdir/fat_exists/fat_write_file are NOT re-declared here. ../fs/fat.h
// above owns them, and a private extern silently opts this whole file out of
// that header's MUST_CHECK attributes while binding to whatever signature was
// typed here. kernel/tools/persist-extern-gate now fails the build on it.
extern fat_fs_t g_fat_fs;

#define WINDIR_ROOT "/WINDIR"

static char g_cur_drive = 'C';   // current default DOS drive

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

// #739: the drive map is no longer three letters written down here. Which class
// a letter belongs to is rustkern/drvmap.rs's decision (via diskimg_letter_class)
// and whether a disc is in it is diskimg's. This file DERIVES its answers from
// those two, so DOS, Win16 and MSCDEX cannot disagree about what exists.
extern int diskimg_letter_class(int idx);
extern int diskimg_is_mounted(char letter);

static int letter_idx(char letter) {
    char d = up(letter);
    return (d >= 'A' && d <= 'Z') ? (d - 'A') : -1;
}

int dos_drive_known(char letter) {
    int i = letter_idx(letter);
    if (i < 0) return 0;
    switch (diskimg_letter_class(i)) {
        // A floppy DRIVE and the hard disk always exist, disk or no disk. That
        // is how real DOS behaves, and it is what lets a program distinguish
        // "drive not ready" from "no such drive".
        case DISKIMG_CLASS_FLOPPY:
        case DISKIMG_CLASS_FIXED:
            return 1;
        // A CD-ROM drive exists exactly while a disc is mounted on it, so an
        // eject and an absent drive look the same to a guest, which is what an
        // eject SHOULD look like.
        case DISKIMG_CLASS_CDROM:
            return diskimg_is_mounted(up(letter)) ? 1 : 0;
        default:
            return 0;
    }
}

int dos_drive_type(char letter) {
    if (!dos_drive_known(letter)) return DOS_DRIVE_NO_ROOT;
    switch (diskimg_letter_class(letter_idx(letter))) {
        case DISKIMG_CLASS_FLOPPY: return DOS_DRIVE_REMOVABLE;
        case DISKIMG_CLASS_FIXED:  return DOS_DRIVE_FIXED;
        // Win16's GetDriveType has no CDROM value, so a CD reports REMOTE. That
        // is the pre-existing choice, kept deliberately: an app that special-
        // cases REMOTE has always seen E: that way.
        case DISKIMG_CLASS_CDROM:  return DOS_DRIVE_REMOTE;
        default:                   return DOS_DRIVE_NO_ROOT;
    }
}

int dos_drive_writable(char letter) {
    if (!dos_drive_known(letter)) return 0;
    switch (diskimg_letter_class(letter_idx(letter))) {
        case DISKIMG_CLASS_FIXED:
            return 1;                                   // C: the hard disk
        case DISKIMG_CLASS_FLOPPY:
            // Folder-backed A:/B: are writable; a MOUNTED floppy image is not,
            // because there is no write-back to an image. Reporting that up
            // front beats a write that reaches fat_write() and fails there,
            // which a DOS program reads as a disk error rather than a
            // write-protected disk.
            return diskimg_is_mounted(up(letter)) ? 0 : 1;
        case DISKIMG_CLASS_CDROM:
        default:
            return 0;
    }
}

// The INVERSE of the "X:" -> /WINDIR/DRIVE_X mapping built in
// dos_resolve_path_ex(): given an already-resolved NATIVE path, is it the ROOT
// of a drive letter, and which one? Returns 'A'..'Z', or 0.
//
// It lives here, next to the forward mapping and sharing its WINDIR_ROOT, for
// the reason the drive map itself was centralised at #739: this is the FOURTH
// place that would otherwise spell "/WINDIR/DRIVE_" out by hand, and the three
// existing copies drifting apart is the exact fault that layer exists to stop.
//
// ROOT, not prefix. fs/fat.c's fat_img_path() asks a different question, "is
// this path INSIDE a mounted image", and answers with a prefix test. A volume
// label lives in the root directory and nowhere else, so a caller asking about
// /WINDIR/DRIVE_E/INSTALL must be told NO, which a prefix test would not do.
// Trailing slashes are tolerated because a DOS "E:\" resolves with one.
char dos_native_root_drive(const char *native) {
    if (!native) return 0;
    const char *pfx = WINDIR_ROOT "/DRIVE_";
    int i = 0;
    while (pfx[i]) { if (native[i] != pfx[i]) return 0; i++; }
    char letter = up(native[i]);
    if (letter < 'A' || letter > 'Z') return 0;
    const char *rest = native + i + 1;
    while (*rest == '/' || *rest == '\\') rest++;
    return *rest ? 0 : letter;
}

char dos_current_drive(void) { return g_cur_drive; }

// ---- per-drive current directory -----------------------------------------
// DOS tracks a CWD PER DRIVE, and "X:NAME" resolves against that drive's CWD,
// not against its root. We had neither, so a program that asked INT 21h 47h
// where it was and then built an absolute path from the answer could not open
// its own files. Stored native-style: uppercase, '/' separated, no leading or
// trailing slash. Empty string means the drive root.
#define DOS_CWD_MAX 96
static char g_drive_cwd[26][DOS_CWD_MAX];

static int drive_idx(char letter) {
    char d = up(letter);
    return (d >= 'A' && d <= 'Z') ? (d - 'A') : -1;
}

void dos_set_drive_cwd(char letter, const char *path) {
    int i = drive_idx(letter);
    if (i < 0) return;
    int n = 0;
    if (path) {
        while (*path == '/' || *path == '\\') path++;
        for (; path[n] && n < DOS_CWD_MAX - 1; n++)
            g_drive_cwd[i][n] = up(path[n] == '\\' ? '/' : path[n]);
    }
    while (n > 0 && g_drive_cwd[i][n - 1] == '/') n--;
    g_drive_cwd[i][n] = '\0';
}

const char *dos_get_drive_cwd(char letter) {
    int i = drive_idx(letter);
    return (i < 0) ? "" : g_drive_cwd[i];
}

void dos_set_current_drive(char letter) {
    if (dos_drive_known(letter)) g_cur_drive = up(letter);
}

// INT 21h 0Eh reports the number of logical drives. It used to be the constant
// 5 (an A..E span). It is now the real span: one past the highest KNOWN letter,
// with 5 as the floor so behaviour on a machine with nothing mounted is exactly
// what it was before this change.
int dos_drive_count(void) {
    int hi = 4;                                  // index of E:, the historic span
    for (int i = 25; i > hi; i--)
        if (dos_drive_known((char)('A' + i))) { hi = i; break; }
    return hi + 1;
}

int dos_path_writable_ex(const char *in, char cur_drive) {
    if (!in || !in[0]) return 1;
    if (in[0] == '/') return 1;                      // native absolute
    // (#740) A leading backslash is the CURRENT DRIVE's root, not the native
    // root, and must answer with THAT drive's writability. Same rule as
    // dos_resolve_path_ex; stating it in only one of the two would let a write
    // be accepted here and land somewhere else.
    if (in[0] == '\\') return dos_drive_writable(cur_drive);
    if (in[1] == ':') return dos_drive_writable(in[0]);
    return dos_drive_writable(cur_drive);             // bare relative -> current
}
int dos_path_writable(const char *in) {
    return dos_path_writable_ex(in, g_cur_drive);
}

int dos_drive_image_mounted(char letter) {
    // #196: a removable drive (A:/E:) with a disk image mounted reads from the
    // image instead of its /WINDIR folder (served in fat_read_file via the
    // diskimg_try_read hook). Returns 0 when nothing is mounted -> folder-backed.
    return diskimg_is_mounted(letter);
}

// Append src to out[*pn] (bounded), converting backslashes to slashes and
// collapsing any run of slashes to a single one. Does NOT uppercase (done last).
static void append_norm(char *out, int *pn, int outsz, const char *src) {
    int n = *pn;
    for (const char *p = src; *p && n < outsz - 1; p++) {
        char c = (*p == '\\') ? '/' : *p;
        if (c == '/' && n > 0 && out[n - 1] == '/') continue;   // collapse //
        out[n++] = c;
    }
    *pn = n;
    out[n] = '\0';
}

// #736: the global-state binding. Kept so every existing caller (win16api.c,
// ne.c, the launchers) is byte-for-byte unaffected.
static const char *dospath_global_cwd(void *u, char drive) {
    (void)u; return dos_get_drive_cwd(drive);
}
void dos_resolve_path(const char *in, const char *reldir, char *out, int outsz) {
    dos_resolve_path_ex(in, reldir, g_cur_drive, dospath_global_cwd, 0, out, outsz);
}

void dos_resolve_path_ex(const char *in, const char *reldir, char cur_drive,
                         dos_cwd_lookup_fn cwd, void *u, char *out, int outsz) {
    if (!out || outsz <= 0) return;
    out[0] = '\0';
    if (!in) return;
    int n = 0;

    // (1) Already a native absolute path: pass through unchanged (the launcher
    //     and most kernel callers use these, e.g. /WIN16/MSEP/CHIPS.EXE).
    //
    // '/' ONLY. A leading BACKSLASH is handled below, because in DOS it does
    // not mean the native root, it means the root of the CURRENT DRIVE.
    if (in[0] == '/') {
        append_norm(out, &n, outsz, in);
        for (int i = 0; out[i]; i++) out[i] = up(out[i]);
        return;
    }

    // (2) Explicit drive prefix "X:" -> map to /WINDIR/DRIVE_X (the new feature).
    char drive = 0;
    const char *rest = in;
    if (in[0] && in[1] == ':') { drive = up(in[0]); rest = in + 2; }
    // (2b) A LEADING BACKSLASH WITH NO DRIVE LETTER IS THE CURRENT DRIVE'S ROOT.
    //
    // MEASURED on Discworld II (#740), and it is what stopped the game dead.
    // Having found E: with INT 21h AX=4409h, the game does AH=0Eh DL=4 (select
    // E:) and then AH=3Bh on "\DW2". That means E:\DW2. It resolved to the
    // NATIVE "/DW2", which does not exist, so the chdir failed, so the game
    // decided E: was not the disc, tried D: and C:, and printed "Discworld
    // cannot locate the Discworld CD."
    //
    // The old comment called the backslash form "the legacy root-relative path,
    // kept byte-identical to pre-#257 behavior". Pre-#257 there were no drive
    // letters, so native-root and current-drive-root were the same place and
    // the distinction could not be observed. Once E: existed they diverged, and
    // the legacy reading became simply wrong: no DOS program has ever meant
    // "the host's native root" by a backslash.
    //
    // NOTHING THAT WORKED BEFORE STOPS WORKING, and not by luck: a guest whose
    // current drive is C: now resolves "\FOO" to /WINDIR/DRIVE_C/FOO, and
    // int21svc.c's native_fallback() already rewrites a /WINDIR/DRIVE_X/... miss
    // back to the native "/FOO" when that is what exists. The rescue path this
    // change relies on predates it and exists for exactly this equivalence.
    else if (in[0] == '\\' && up(cur_drive) >= 'A' && up(cur_drive) <= 'Z') {
        drive = up(cur_drive);
        rest = in;                  // keep the leading '\', the branch strips it
    }
    // A backslash with no usable current drive keeps the old native reading:
    // refusing here would turn a resolvable path into nothing at all.
    else if (in[0] == '\\') {
        append_norm(out, &n, outsz, in);
        for (int i = 0; out[i]; i++) out[i] = up(out[i]);
        return;
    }

    if (drive) {
        // base = /WINDIR/DRIVE_X
        char base[24];
        int bn = 0;
        for (const char *p = WINDIR_ROOT "/DRIVE_"; *p; p++) base[bn++] = *p;
        base[bn++] = drive; base[bn] = '\0';
        append_norm(out, &n, outsz, base);
        if (rest[0] != '/' && rest[0] != '\\') {
            // drive-relative ("X:FOO"): resolve against THAT drive's CWD.
            // This used to fall back to the drive root, which is why a 4Eh
            // findfirst for "C:????????.???" searched C:\\ instead of the
            // directory the program actually thinks it is in.
            const char *cwdstr = cwd ? cwd(u, drive) : "";
            if (!cwdstr) cwdstr = "";
            if (n < outsz - 1) { out[n++] = '/'; out[n] = '\0'; }
            if (cwdstr[0]) {
                append_norm(out, &n, outsz, cwdstr);
                if (n < outsz - 1) { out[n++] = '/'; out[n] = '\0'; }
            }
        } else if (rest[0]) {
            if (n < outsz - 1) out[n++] = '/';  // ensure separator before rest
            out[n] = '\0';
            while (*rest == '/' || *rest == '\\') rest++;   // skip leading slashes
        }
        append_norm(out, &n, outsz, rest);
    } else {
        // (3) No drive letter and no leading separator: a BARE RELATIVE name.
        //
        // THE CURRENT DIRECTORY WINS OVER THE LAUNCHER'S HINT (#740). A guest
        // that has issued AH=3Bh has STATED where relative names resolve, and
        // no hint can be more authoritative than that. This arm used to go
        // straight to `reldir` (the .EXE's directory), so a chdir had no effect
        // on any subsequent bare-name open.
        //
        // MEASURED: Discworld II chdirs to \DW2 on the candidate CD and then
        // opens "VOLUME", which is the disc's identity file. Resolving that
        // against the .EXE directory instead of the directory it had just
        // selected turned the disc check into a miss, and the game concluded
        // there was no CD.
        //
        // THE LEGACY GAMES ARE UNAFFECTED, and not by luck. A guest that never
        // chdirs has an EMPTY cwd here and takes the reldir arm exactly as
        // before. A guest launched from the native namespace has its cwd seeded
        // with a native fragment ("DOS/KEEN5"), which composes to
        // /WINDIR/DRIVE_C/DOS/KEEN5/NAME; int21svc.c's native_fallback() maps a
        // /WINDIR/DRIVE_X miss back to the native path that exists, which is
        // the same rescue the drive namespace has always leaned on.
        const char *cwdstr = cwd ? cwd(u, cur_drive) : "";
        if (!cwdstr) cwdstr = "";
        char cd = up(cur_drive);
        if (cwdstr[0] && cd >= 'A' && cd <= 'Z') {
            char base[24]; int bn = 0;
            for (const char *p = WINDIR_ROOT "/DRIVE_"; *p; p++) base[bn++] = *p;
            base[bn++] = cd; base[bn] = '\0';
            append_norm(out, &n, outsz, base);
            if (n < outsz - 1) { out[n++] = '/'; out[n] = '\0'; }
            append_norm(out, &n, outsz, cwdstr);
            if (n < outsz - 1) { out[n++] = '/'; out[n] = '\0'; }
            append_norm(out, &n, outsz, rest);
        } else if (reldir && reldir[0]) {
            append_norm(out, &n, outsz, reldir);
            if (n > 0 && out[n - 1] != '/' && n < outsz - 1) { out[n++] = '/'; out[n] = '\0'; }
            append_norm(out, &n, outsz, rest);
        } else {
            // No reldir: resolve under the current drive root.
            char base[24]; int bn = 0;
            for (const char *p = WINDIR_ROOT "/DRIVE_"; *p; p++) base[bn++] = *p;
            base[bn++] = cur_drive; base[bn] = '\0';
            append_norm(out, &n, outsz, base);
            if (n < outsz - 1) { out[n++] = '/'; out[n] = '\0'; }
            append_norm(out, &n, outsz, rest);
        }
    }

    for (int i = 0; out[i]; i++) out[i] = up(out[i]);
}

// Create one directory if it does not already exist (best effort).
static void ensure_dir(const char *path) {
    if (!fat_exists(&g_fat_fs, path)) fat_mkdir(&g_fat_fs, path);
}

// (#133) Seed a file with default contents only if it does not already exist, so
// a user's later edits to WIN.INI / SYSTEM.INI survive reboots. fat_exists is not
// ext2-routed, so probe via the routed reader (fat_read_file -> ext2) instead.
extern void *fat_read_file(fat_fs_t *fs, const char *path, unsigned int *size_out);
extern void  kfree(void *p);
static void seed_file(const char *path, const char *contents) {
    unsigned int sz = 0;
    void *d = fat_read_file(&g_fat_fs, path, &sz);
    if (d) kfree(d);
    if (d && sz > 0) return;    // a non-empty file exists: keep the user's copy
    unsigned n = 0; while (contents[n]) n++;
    // #693: seeding a default DOS file is best effort, but a silent failure
    // shows up later as a mysteriously missing file, so it is logged.
    if (fat_write_file(&g_fat_fs, path, contents, n) != 0)
        kprintf("[DOS] failed to seed %s\n", path);
}

// #736 Stage 2: THE GUEST SCRATCH-LOCATION DECISION, made here rather than in
// a hand-edited PERMS.DB, because this is the code that CREATES the directory.
//
// THE PROBLEM. A path with no PERMS.DB entry falls to the root-owned 0755
// default, and creating a file is a write to its PARENT. /DOS, /GAMES and
// /WINDIR have no entry, so once the desktop stops autologging in as root, a
// guest can read its game but cannot write anything anywhere. Measured at
// uid 1000: 3Ch create, 39h mkdir and 56h rename all denied on /DOS/FSPROBE.
//
// WHAT WAS DECIDED, AND WHAT WAS DELIBERATELY NOT.
//
// NOT: loosening /DOS or /GAMES. They hold shipped program files shared by
// every user. Making them world-writable so a game can drop a savegame next to
// its .EXE would also let any user overwrite another user's saves and the game
// binaries themselves. A save file is not worth a writable program directory.
//
// YES: /WINDIR/DRIVE_C, and only it. That tree is not shipped content, it is
// created right here at boot, it holds no program, and it is precisely what
// "C:\" MEANS to a guest. A guest that wants somewhere to write has a name for
// it already. 0777 with no entry pre-existing, so an operator who chmods it
// keeps their choice across reboots (this only fills an ABSENT entry, the same
// rule perms_on_create follows).
//
// WHAT THIS DOES NOT SOLVE, stated because it is the interesting half: Keen 5
// writes SAVEGAM0.CK5 next to KEEN5E.EXE, in /DOS/KEEN5, and it always will,
// because that is what the game does. Under a non-root desktop that save is
// still denied. Fixing THAT needs guest writes into a program directory to be
// redirected into a per-user overlay, which is a design (where does the
// redirect live, how does a read find the overlaid file, what happens on
// uninstall) and not a permission tweak. It is not built, and this comment is
// the place the next person will look.
// ORDERING, and it was measured wrong first: dos_windir_init() runs at boot
// LONG BEFORE perms_init() loads /CONFIG/PERMS.DB (main.c line ~1029 against
// ~1614), so an entry written from there is discarded when the database loads.
// The kprintf fired, the entry looked set, and a uid-1000 create on C:\ was
// still denied. This is therefore called FROM main.c, immediately after
// perms_init(), not from dos_windir_init().
void dos_scratch_perms(void) {
    extern int  perms_get(const char *path, uint32_t *uid, uint32_t *gid, uint16_t *mode);
    extern void perms_set(const char *path, uint32_t uid, uint32_t gid, uint16_t mode);
    uint32_t u = 0, g = 0; uint16_t m = 0;
    if (perms_get(WINDIR_ROOT "/DRIVE_C", &u, &g, &m) == 0) return;   // operator's choice wins
    perms_set(WINDIR_ROOT "/DRIVE_C", 0, 0, 0777);
    kprintf("[dospath] guest scratch: %s/DRIVE_C set 0777 (see dos_scratch_perms)\n",
            WINDIR_ROOT);
}

// ===========================================================================
// #rawrite: MATERIALISING THE PER-USER WRITE OVERLAY
// ===========================================================================
// The comment above (dos_scratch_perms) ends by naming this as the thing that
// was NOT built:
//
//     Fixing THAT needs guest writes into a program directory to be redirected
//     into a per-user overlay, which is a design (where does the redirect live,
//     how does a read find the overlaid file, what happens on uninstall) and
//     not a permission tweak. It is not built, and this comment is the place
//     the next person will look.
//
// Those three questions, answered:
//
//   WHERE DOES THE REDIRECT LIVE?  In dos_svc_resolve(), the ONE function that
//     turns a guest path into a native path (dos/int21svc.c overlay_apply),
//     with the pure policy in rustkern/dosovl.rs. Not in a permission row: no
//     mode on /DOS/<GAME> can make a save writable without also making the
//     EXECUTABLE writable, and a user who can rewrite the executable hands the
//     next user a different program. That is the shortcut fs/perms.c's #221b
//     block rejects for NetHack, and it is rejected here for the same reason.
//
//   HOW DOES A READ FIND THE OVERLAID FILE?  By existence, checked in that one
//     place: the overlay wins when it has the file, the base is read through
//     when it does not, and a create lands in the overlay. Directory
//     enumeration is walked in two phases (int21svc.c find_step) so a save
//     that exists only in the overlay still appears in a SAVEGAME.* scan.
//
//   WHAT HAPPENS ON UNINSTALL?  Nothing here, deliberately. The overlay is a
//     normal directory in the user's own home, owned by them at 0750; it
//     outlives the install exactly the way a save game should, and removing it
//     is a file-manager operation, not a special case in the kernel.
//
// THE SEED IS WHAT MAKES THE RULE ACCESS-INDEPENDENT. Red Alert opens
// REDALERT.INI with 3Ch (create/truncate) and rewrites it whole. If the overlay
// were empty, that path would exist only in the base and the read-through would
// send the write to the read-only install. Copying every SMALL file across when
// the overlay is first created means the mutable copy is already there, so no
// access bit has to be plumbed through eleven resolve call sites.
//
// SMALL, and the cap is the point: 256 KiB copies the .INI/.CFG/.PAL class and
// refuses the data archives (REDALERT.MIX is 25 MB). A game that wants to
// rewrite a 25 MB archive in place is denied, which is the correct answer for
// shipped read-only data.
#define DOSOVL_SEED_MAX 262144u

// Does this native path exist? fat_exists is NOT ext2-routed (see seed_file
// above), so probe through the routed opener, which opens directories too.
static int ovl_exists(const char *path) {
    fat_file_t f;
    if (fat_open(&g_fat_fs, path, &f) != 0) return 0;
    fat_close(&f);
    return 1;
}

int dos_overlay_prepare(const char *base, const char *ovl,
                        unsigned int uid, unsigned int gid) {
    if (!base || !base[0] || !ovl || ovl[0] != '/') return -1;

    // mkdir -p. Same shape as users_make_home_skeleton(): every parent first,
    // because the home may exist while <home>/GAMES does not.
    char path[160];
    int n = 0;
    for (int i = 0; ovl[i] && n < (int)sizeof(path) - 2; i++) {
        path[n++] = ovl[i];
        if (ovl[i] == '/' && n > 1) { path[n - 1] = '\0'; ensure_dir(path); path[n - 1] = '/'; }
    }
    path[n] = '\0';
    ensure_dir(path);
    if (!ovl_exists(path)) {
        kprintf("[dos] #rawrite: could not create the write overlay %s; the guest "
                "will keep writing to its install directory and being refused\n", path);
        return -1;
    }
    // The directory is the user's, not root's: 0750 so no OTHER non-root
    // account can even list another player's saves. Set on EVERY run, so an
    // overlay made before this line existed is corrected.
    perms_set(path, uid, gid, 0750);

    // SEED PER FILE, EVERY LAUNCH, AND THE ALTERNATIVE COST A WHOLE BUILD.
    //
    // This block used to run only when it had just CREATED the overlay
    // directory ("if (!fresh) return 0"). MEASURED on golden 2229: it never
    // ran at all, because users_make_home_skeleton() creates <home>/GAMES/RA
    // at LOGIN, which is long before any DOS guest launches. The directory was
    // therefore never fresh, REDALERT.INI was never seeded, and overlay_apply's
    // read-through correctly sent the write to the read-only install, so the
    // deny this whole change exists to remove was BYTE-IDENTICAL to the one
    // before it:
    //
    //   [GUESTFS-DENY] guest=dos uid=1000 gid=1000 want=-w- op=INT21/3Ch
    //                  create reason=PERMS path=/DOS/RA/./REDALERT.INI
    //
    // A directory-level "have I done this" flag is the wrong shape for a
    // per-file job: it can be true for a directory that is missing every file
    // the job was supposed to put in it, and the two facts are set by
    // different subsystems at different times. The per-file test cannot get
    // out of step with itself, and it also HEALS an overlay made by an older
    // build.
    //
    // A file the user has deleted comes back. That is deliberate for the class
    // being copied (config and palette files): the same rule seed_file() above
    // applies to WIN.INI, where a missing default is worse than a re-seeded
    // one. It never overwrites, so an edited copy is always kept.
    fat_file_t d;
    if (fat_open(&g_fat_fs, base, &d) != 0) return 0;
    fat_dir_entry_t e;
    char nm[256];
    unsigned seeded = 0;
    while (fat_readdir(&d, &e, nm) == 0) {
        if (e.name[0] == 0x00) break;
        if ((unsigned char)e.name[0] == 0xE5) continue;
        if (e.attr & 0x10) continue;                 // directories are not seeded
        if (nm[0] == '\0' || nm[0] == '.') continue;
        if (e.file_size == 0) continue;
        char src[160], dst[160];
        int a = 0, b = 0;
        for (; base[a] && a < (int)sizeof(src) - 2; a++) src[a] = base[a];
        if (a > 0 && src[a - 1] != '/') src[a++] = '/';
        for (int i = 0; nm[i] && a < (int)sizeof(src) - 1; i++) src[a++] = nm[i];
        src[a] = '\0';
        for (; ovl[b] && b < (int)sizeof(dst) - 2; b++) dst[b] = ovl[b];
        if (b > 0 && dst[b - 1] != '/') dst[b++] = '/';
        for (int i = 0; nm[i] && b < (int)sizeof(dst) - 1; i++) dst[b++] = nm[i];
        dst[b] = '\0';
        // Already in the overlay: leave the user's copy alone. This is the
        // test that makes the seed idempotent and order-independent.
        if (ovl_exists(dst)) continue;
        // Over the cap: NOT copied, and SAID SO by name. A silent skip here is
        // how a game ends up denied on one particular file with nothing in the
        // log connecting it to this decision.
        if (e.file_size > DOSOVL_SEED_MAX) {
            kprintf("[dos] #rawrite: %s is %u bytes (> %u), NOT seeded; it stays "
                    "read-only in %s and a write to it will be refused\n",
                    nm, (unsigned)e.file_size, DOSOVL_SEED_MAX, base);
            continue;
        }
        unsigned int sz = 0;
        void *data = fat_read_file(&g_fat_fs, src, &sz);
        if (!data) continue;
        if (sz > 0 && fat_write_file(&g_fat_fs, dst, data, sz) == 0) {
            // 0600: this is one player's settings file, and the directory is
            // already 0750, so a wider mode would buy nothing and say the
            // wrong thing about who the file belongs to.
            perms_set(dst, uid, gid, 0600);
            seeded++;
        } else {
            kprintf("[dos] #rawrite: failed to seed %s into the overlay\n", nm);
        }
        kfree(data);
    }
    fat_close(&d);
    kprintf("[dos] #rawrite: overlay %s ready for uid=%u; seeded %u missing "
            "file(s) (<=%u bytes) from %s\n", ovl, uid, seeded, DOSOVL_SEED_MAX, base);
    return 0;
}

void dos_windir_init(void) {
    ensure_dir(WINDIR_ROOT);
    ensure_dir(WINDIR_ROOT "/DRIVE_A");
    ensure_dir(WINDIR_ROOT "/DRIVE_B");   // #739: B: is a real floppy slot now
    ensure_dir(WINDIR_ROOT "/DRIVE_C");
    // #739: CD letters are allocated on demand (E: upward) and are ONLY visible
    // while a disc is mounted, at which point fat_open()'s image branch wins
    // before anything looks at the folder. Pre-creating 22 empty directories
    // would buy nothing and would make an ejected drive look browsable.
    ensure_dir(WINDIR_ROOT "/DRIVE_E");
    // Minimal C:\WINDOWS so GetWindowsDirectory/GetSystemDirectory probes succeed.
    ensure_dir(WINDIR_ROOT "/DRIVE_C/WINDOWS");
    ensure_dir(WINDIR_ROOT "/DRIVE_C/WINDOWS/SYSTEM");
    ensure_dir(WINDIR_ROOT "/DRIVE_C/WINDOWS/TEMP");
    // (#133) Seed minimal Win3.1 .INI files so GetProfile*/GetPrivateProfile* and
    // apps that probe [windows]/[intl]/[boot] find a real, editable file.
    seed_file(WINDIR_ROOT "/DRIVE_C/WINDOWS/WIN.INI",
        "[windows]\r\nspooler=yes\r\ndevice=\r\nrun=\r\nload=\r\n"
        "[Desktop]\r\nWallpaper=(None)\r\nPattern=(None)\r\n"
        "[intl]\r\nsCountry=United States\r\niCountry=1\r\nsLanguage=enu\r\n"
        "[fonts]\r\n[extensions]\r\n[mci extensions]\r\n[sounds]\r\n");
    seed_file(WINDIR_ROOT "/DRIVE_C/WINDOWS/SYSTEM.INI",
        "[boot]\r\nshell=progman.exe\r\nsystem.drv=system.drv\r\n"
        "[keyboard]\r\n[boot.description]\r\n[386Enh]\r\n[drivers]\r\n[mci]\r\n");
    kprintf("[dospath] /WINDIR drive layer ready (A/B floppy, C fixed, E..Z CD on demand; "
            "C:\\WINDOWS + WIN.INI/SYSTEM.INI seeded)\n");
}
