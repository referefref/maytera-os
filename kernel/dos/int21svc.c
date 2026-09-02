// dos/int21svc.c - THE ONE INT 21h SERVICE CORE (#736 Stage 1).
//
// Read int21svc.h first: it states why this file exists, the context/service
// split, and how a DOS/4GW DPMI host attaches as a THIRD CALLER.
//
// THE RULE THIS FILE IS WRITTEN UNDER: there is no "am I DOS or Win16" branch
// anywhere below. If a behaviour differs between guests it is read out of the
// context (psp_seg, has_ivt, dos_version, the console vtable, the CWD binding,
// the extend hook). A reviewer can enforce that mechanically: this file
// contains no reference to GUESTFS_SLOT_DOS or GUESTFS_SLOT_WIN16.
//
// LANGUAGE. C, and deliberately: this is a MOVE of existing C (int21() from
// dos/dosexec.c and win16_int()'s INT 21h block from exec/ne.c), not new logic.
// Rewriting it into Rust in the same change would make the behaviour-preserving
// claim unverifiable, which is the whole point of Stage 1. New logic that is
// genuinely new belongs in Rust per the 2026-07-16 rule.

#include "int21svc.h"
#include "dospath.h"
#include "diskimg.h"   // volume label + media size for 4Eh/36h
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/perms.h"     // R_OK / W_OK / X_OK
#include "../fs/guestfs.h"   // #708 the guest filesystem gate
#include "../cpu/wallclock.h" // #234a the ONE clock, in its DOS shape

// #679: a file a guest creates is owned by the guest's LAUNCHER, the same
// way a new inode gets its creator's uid/gid. Without this a non-root guest
// creates a file it then cannot write, because a path with no PERMS.DB entry
// falls to the root-owned 0755 default.
void perms_on_create(const char *path, uint32_t uid, uint32_t gid, int is_dir);
void perms_remove(const char *path);
#include "../exec/x86_16.h"

extern fat_fs_t g_fat_fs;

// (#740) rustkern/drvmap.rs. THE drive map's answers for INT 21h AH=44h
// AL=08h/09h, taken from the letter's class so this file never forms a second
// opinion about what a drive letter is (the fault #739 consolidated).
extern int32_t drvmap_ioctl_removable_rs(uint32_t class);
extern int32_t drvmap_ioctl_attrword_rs(uint32_t class);

// Set from a debugger / RC to log every INT 21h call. Off for ship.
int g_int21_trace = 0;

// ---- register helpers ----------------------------------------------------
#define SET_CF(c)   ((c)->flags |= 0x0001)
#define CLR_CF(c)   ((c)->flags &= ~0x0001)
#define AH_SET(c,v) ((c)->ax = (uint16_t)(((c)->ax & 0x00FF) | ((v) << 8)))
#define AL_SET(c,v) ((c)->ax = (uint16_t)(((c)->ax & 0xFF00) | ((v) & 0xFF)))
#define AH(c)       ((uint8_t)((c)->ax >> 8))
#define AL(c)       ((uint8_t)((c)->ax & 0xFF))
#define DL(c)       ((uint8_t)((c)->dx & 0xFF))

// ---- guest memory, always through the context ----------------------------
static inline uint8_t  g_rd8 (dos_svc_ctx_t *x, uint16_t s, uint16_t o) { return x->mem.rd8  ? x->mem.rd8 (x->mem_u, s, o) : 0; }
static inline void     g_wr8 (dos_svc_ctx_t *x, uint16_t s, uint16_t o, uint8_t v)  { if (x->mem.wr8)  x->mem.wr8 (x->mem_u, s, o, v); }
static inline uint16_t g_rd16(dos_svc_ctx_t *x, uint16_t s, uint16_t o) { return x->mem.rd16 ? x->mem.rd16(x->mem_u, s, o) : 0; }
static inline void     g_wr16(dos_svc_ctx_t *x, uint16_t s, uint16_t o, uint16_t v) { if (x->mem.wr16) x->mem.wr16(x->mem_u, s, o, v); }

static void con_putc(dos_svc_ctx_t *x, uint8_t ch) { if (x->con.putc) x->con.putc(x->con_u, ch); }
static int  con_get (dos_svc_ctx_t *x, uint16_t *k) { return x->con.getkey  ? x->con.getkey (x->con_u, k) : 0; }
static int  con_peek(dos_svc_ctx_t *x, uint16_t *k) { return x->con.peekkey ? x->con.peekkey(x->con_u, k) : 0; }

// Read an ASCIIZ string out of guest memory.
static void rd_asciiz(dos_svc_ctx_t *x, uint16_t seg, uint16_t off, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) {
        uint8_t ch = g_rd8(x, seg, (uint16_t)(off + i));
        if (ch == 0) break;
        out[i] = (char)ch;
    }
    out[i] = '\0';
}

// ---- the shared real-mode memory binding --------------------------------
static uint8_t  x16_rd8 (void *u, uint16_t s, uint16_t o)             { return x86_16_rd8 ((x86_16_cpu_t *)u, s, o); }
static void     x16_wr8 (void *u, uint16_t s, uint16_t o, uint8_t v)  { x86_16_wr8 ((x86_16_cpu_t *)u, s, o, v); }
static uint16_t x16_rd16(void *u, uint16_t s, uint16_t o)             { return x86_16_rd16((x86_16_cpu_t *)u, s, o); }
static void     x16_wr16(void *u, uint16_t s, uint16_t o, uint16_t v) { x86_16_wr16((x86_16_cpu_t *)u, s, o, v); }

void dos_svc_bind_x86_16(dos_svc_ctx_t *x, x86_16_cpu_t *cpu) {
    x->mem_u    = cpu;
    x->mem.rd8  = x16_rd8;
    x->mem.wr8  = x16_wr8;
    x->mem.rd16 = x16_rd16;
    x->mem.wr16 = x16_wr16;
}

// ---- the #708 gate -------------------------------------------------------
int dos_svc_allow(dos_svc_ctx_t *x, const char *native, int access, const char *what) {
    return guestfs_allow(x->guest_slot, native, access, what);
}

// Parent directory of a native path. Kept here rather than in each caller
// because the CREATE question ("may I write the parent?") is the one every
// guest layer has to ask and the one every layer got wrong independently.
static void parent_path(const char *path, char *out, int outsz) {
    int last = -1, n = 0;
    for (int i = 0; path[i]; i++) if (path[i] == '/') last = i;
    if (last <= 0) { out[0] = '/'; out[1] = '\0'; return; }
    for (int i = 0; i < last && n < outsz - 1; i++) out[n++] = path[i];
    out[n] = '\0';
}

int dos_svc_allow_create(dos_svc_ctx_t *x, const char *native, int leaf_exists,
                         const char *what) {
    if (leaf_exists) return dos_svc_allow(x, native, W_OK, what);
    char parent[DOS_SVC_PATH_MAX];
    parent_path(native, parent, sizeof(parent));
    return dos_svc_allow(x, parent, W_OK | X_OK, what);
}

// ---- path resolution -----------------------------------------------------
static const char *ctx_cwd_get(dos_svc_ctx_t *x, char drive) {
    return x->cwd_get ? x->cwd_get(x, drive) : "";
}
static const char *cwd_thunk(void *u, char drive) {
    return ctx_cwd_get((dos_svc_ctx_t *)u, drive);
}

// Does this native path name something that exists? fat_open() is the ROUTED
// accessor (it reaches ext2 on an ext2-root system, unlike fat_exists) and it
// opens directories too, which is what the 4Eh dirpath probe needs.
static int path_exists(dos_svc_ctx_t *x, const char *p) {
    if (!dos_svc_allow(x, p, R_OK, "path-probe")) return 0;
    fat_file_t f;
    if (fat_open(&g_fat_fs, p, &f) == 0) { fat_close(&f); return 1; }
    return 0;
}

// If a /WINDIR/DRIVE_X/... resolution does not exist, retry the same path
// drive-stripped at the native root, so the drive namespace and the native
// namespace 47h reports resolve to the same files. A path that exists under
// /WINDIR is never rewritten, and a create (which exists in neither) stays
// under /WINDIR, so this only ever rescues a miss.
static int native_fallback(dos_svc_ctx_t *x, char *p, int max) {
    if (strncmp(p, "/WINDIR/DRIVE_", 14) != 0) return 0;
    if (!p[14] || p[15] != '/') return 0;
    if (path_exists(x, p)) return 0;
    char alt[DOS_SVC_PATH_MAX];
    int n = 0;
    for (const char *q = p + 15; *q && n < (int)sizeof(alt) - 1; q++) alt[n++] = *q;
    alt[n] = '\0';
    if (!path_exists(x, alt)) {
        // #221: NEITHER LEAF EXISTS, WHICH IS WHAT A CREATE LOOKS LIKE.
        //
        // The comment above used to end "and a create (which exists in neither)
        // stays under /WINDIR, so this only ever rescues a miss". That was a
        // correct description of a real hole: a guest launched from the NATIVE
        // namespace (dos_run_file seeds the current drive's CWD from the app
        // directory, e.g. C: cwd = "DOS/NETHACK" for /DOS/NETHACK/NETHACK.EXE)
        // resolves every relative name to /WINDIR/DRIVE_C/DOS/NETHACK/<name>,
        // a directory that does not exist. Reading such a file was rescued by
        // the leaf test above; CREATING one was not, and fat_write_file()
        // failed with "FAILED on the medium" because the PARENT was missing.
        // Measured: NetHack could not create its `record` file, printed
        // "Warning: cannot write record" and stopped at getreturn() before a
        // game could start. Every file it needs (record, logfile, the lock and
        // level files, saves) is a create, so this was the whole wall.
        //
        // So ask the same question one level up: a create belongs wherever its
        // DIRECTORY is. If the /WINDIR directory is real we stay there (a
        // genuine C: tree keeps its writes); only when the drive directory does
        // not exist and the native one does is the path rewritten. That keeps
        // the two namespaces naming the same place for creates as well as for
        // opens, which is what this function is for.
        char pw[DOS_SVC_PATH_MAX], pn[DOS_SVC_PATH_MAX];
        parent_path(p,   pw, sizeof(pw));
        parent_path(alt, pn, sizeof(pn));
        if (path_exists(x, pw)) return 0;    // real C: directory: writes stay there
        if (!path_exists(x, pn)) return 0;   // neither directory exists: change nothing
    }
    int i = 0;
    for (; alt[i] && i < max - 1; i++) p[i] = alt[i];
    p[i] = '\0';
    return 1;
}

// ===========================================================================
// #rawrite: THE PER-USER WRITE OVERLAY
// ===========================================================================
// A DOS game keeps its mutable state beside its executable, because DOS had one
// user. /DOS/<GAME> is root-owned 0755 here, so a uid-1000 desktop session is
// refused every one of those writes. dospath.c has named the fix in a comment
// since #736 Stage 2 and stopped short of building it:
//
//     Fixing THAT needs guest writes into a program directory to be redirected
//     into a per-user overlay, which is a design [...] and not a permission
//     tweak. It is not built, and this comment is the place the next person
//     will look.
//
// This is that redirect. The POLICY (which of the two directories a path
// belongs to) is rustkern/dosovl.rs; what lives here is the part that has to
// touch the filesystem, because the choice is made by EXISTENCE:
//
//   exists in the overlay   -> the overlay      (the mutable copy wins)
//   else exists in the base -> the base         (read-through)
//   else                    -> the overlay      (a create lands here)
//
// THE ALTERNATIVE THAT WAS MEASURED AND REJECTED. The cheaper design is to
// launch the guest with its current directory set to a writable per-user
// directory and let reads fall through to the install. Measured under a
// DOSBox-X reference run on 2026-08-27: Red Alert launched as C:\RA\GAME.EXE
// with the shell current directory set to C:\SAVE wrote its DOS/4G swap file
// and rewrote REDALERT.INI into C:\RA, the EXECUTABLE's directory, and left
// C:\SAVE empty. The launch CWD is not a lever on that program, so the
// redirect has to happen where the path is resolved, which is here.
//
// WHY THIS IS NOT "chmod 0777 /DOS/RA". That directory holds GAME.DAT, and a
// user who can rewrite the executable can hand the next user a different
// program. fs/perms.c's #221b block rejects exactly that shortcut for NetHack
// and this change adds NO write grant outside the launching user's own home.

// Does the LAST component contain a DOS wildcard? A wildcard names no file, so
// the existence tests below cannot decide anything about it and would fall to
// the create branch, silently rewriting a 4Eh search into the overlay and
// hiding every base file from it. Wildcards are therefore left alone here and
// handled by the two-phase walk in find_step() instead.
static int leaf_has_wildcard(const char *p) {
    const char *leaf = p;
    for (const char *q = p; *q; q++) if (*q == '/') leaf = q + 1;
    for (const char *q = leaf; *q; q++) if (*q == '*' || *q == '?') return 1;
    return 0;
}

int dos_svc_overlay_dir(dos_svc_ctx_t *x, const char *native, char *out, int outsz) {
    if (!x || !x->ovl_base[0] || !x->ovl_dir[0] || !native || !out) return 0;
    return dosovl_map_rs((const unsigned char *)x->ovl_base,
                         (const unsigned char *)x->ovl_dir,
                         (const unsigned char *)native,
                         (unsigned char *)out, outsz);
}

void dos_svc_set_overlay(dos_svc_ctx_t *x, const char *base, const char *ovl) {
    if (!x) return;
    x->ovl_base[0] = '\0';
    x->ovl_dir[0]  = '\0';
    if (!base || !base[0] || !ovl || !ovl[0]) return;
    int n = 0;
    for (; base[n] && n < (int)sizeof(x->ovl_base) - 1; n++) x->ovl_base[n] = base[n];
    x->ovl_base[n] = '\0';
    n = 0;
    for (; ovl[n] && n < (int)sizeof(x->ovl_dir) - 1; n++) x->ovl_dir[n] = ovl[n];
    x->ovl_dir[n] = '\0';
}

// Is this native path an existing DIRECTORY? Needed as a separate question
// from path_exists() because a directory in the base must never be remapped;
// see overlay_apply().
static int path_is_dir(dos_svc_ctx_t *x, const char *p) {
    if (!dos_svc_allow(x, p, R_OK, "path-probe")) return 0;
    fat_file_t f;
    if (fat_open(&g_fat_fs, p, &f) != 0) return 0;
    int d = fat_is_dir(&f);
    fat_close(&f);
    return d;
}

static void overlay_apply(dos_svc_ctx_t *x, char *p, int max) {
    if (!x->ovl_base[0] || !x->ovl_dir[0]) return;
    if (leaf_has_wildcard(p)) return;
    char ovl[DOS_SVC_PATH_MAX];
    if (!dos_svc_overlay_dir(x, p, ovl, (int)sizeof(ovl))) return;
    // A DIRECTORY THAT EXISTS IN THE BASE IS NEVER REMAPPED, and this is
    // load-bearing rather than tidy. AH=3Bh chdir stores the RESOLVED path as
    // the guest's current directory, so remapping the install directory itself
    // would move the guest's whole notion of "where I am" into the overlay.
    // Every later bare name would then resolve to <home>/GAMES/RA/NAME, which
    // is NOT under ovl_base, so this function would not fire for it and the
    // read-through would be gone: measured consequence, REDALERT.MIX (25 MB,
    // deliberately not seeded) becomes unopenable and the game cannot start.
    //
    // The base directory is the string the overlay maps FROM, so it has to
    // stay the one the guest is standing in. Files are the things that move.
    //
    // KNOWN EDGE, stated rather than hidden: a guest that MKDIRs a new
    // subdirectory under its install and then chdirs into it lands in the
    // overlay namespace, because the base subdirectory does not exist and so
    // is not a directory here. Everything under such a directory is
    // guest-created and lives in the overlay anyway, so it is self-consistent;
    // it is only worth knowing that the two halves are not symmetric. No
    // shipped title does this.
    if (path_is_dir(x, p)) return;
    // Read-through: only when the overlay has nothing AND the base does. A path
    // absent from both is a CREATE and belongs in the overlay, which is the
    // whole point; a path present in both is the seeded mutable copy and the
    // overlay wins.
    if (!path_exists(x, ovl) && path_exists(x, p)) return;
    int i = 0;
    for (; ovl[i] && i < max - 1; i++) p[i] = ovl[i];
    p[i] = '\0';
}

// #221b: expand a leading "%HOME%" to the launching user's home directory.
//
// WHY A TOKEN AND NOT A DRIVE LETTER. The obvious DOS answer is "H: is your
// home". A drive letter is not free here: rustkern/drvmap.rs owns which CLASS
// a letter is, dospath.c maps every letter to a fixed folder under /WINDIR,
// and diskimg/MSCDEX derive their answers from the same table. All of them
// would have to learn that one letter is PER-GUEST state rather than a fixed
// mapping, and #739 exists precisely because that map used to be written down
// in three places that disagreed. A token is expanded once, here, in the one
// function that already holds per-guest path state, and the drive map does not
// change at all.
//
// Expanded BEFORE dos_resolve_path_ex() rather than rewritten after it. The
// home path is a NATIVE absolute path, so once expanded the string starts with
// '/' and dos_resolve_path_ex passes it through (uppercased) unchanged. Done
// afterwards, "%HOME%\X" would first be resolved as a RELATIVE name against
// the application directory and the result would have to be unpicked.
//
// The token must name a whole path component: "%HOME%" or "%HOME%\rest", never
// "%HOMEWORK%". Matching is case-insensitive because DOS names are, and a
// config file is edited by hand.
//
// Returns 1 if it expanded (and filled `out`), 0 to leave the path alone.
static int home_expand(dos_svc_ctx_t *x, const char *in, char *out, int outsz) {
    static const char TOK[] = "%HOME%";
    if (!x->homedir[0] || !in) return 0;
    int i = 0;
    for (; TOK[i]; i++) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c != TOK[i]) return 0;
    }
    if (in[i] && in[i] != '\\' && in[i] != '/') return 0;
    int n = 0;
    for (const char *p = x->homedir; *p && n < outsz - 1; p++) out[n++] = *p;
    for (const char *p = in + i; *p && n < outsz - 1; p++) out[n++] = *p;
    out[n] = '\0';
    return 1;
}

void dos_svc_resolve(dos_svc_ctx_t *x, const char *in, char *out, int outsz) {
    char home[DOS_SVC_PATH_MAX];
    if (home_expand(x, in, home, sizeof(home))) in = home;
    dos_resolve_path_ex(in, x->appdir, x->cur_drive, cwd_thunk, x, out, outsz);
    native_fallback(x, out, outsz);
    overlay_apply(x, out, outsz);      // #rawrite, and LAST: it decides between
                                       // two real directories, so it must see
                                       // the path native_fallback settled on.
}

// Fail a call the DOS way: AX = the error code, CF set, and the code retained
// for AH=59h. One helper, so a new failure path cannot forget the third part.
static void svc_err(dos_svc_ctx_t *x, x86_16_cpu_t *c, uint16_t code) {
    c->ax = code;
    x->last_err = code;
    SET_CF(c);
}

// ===========================================================================
// HANDLES AND OPEN FILES (#736 Stage 2)
// ===========================================================================
// jft[handle] indexes sft[], or is -1. See the comment on dos_svc_sft_t in the
// header for why the two levels exist: DUP shares the OPEN FILE, including its
// position, which a one-level table cannot express.

// The identity a file this guest creates should be owned by.
static void svc_creator(dos_svc_ctx_t *x, uint32_t *uid, uint32_t *gid) {
    *uid = 0; *gid = 0;
    if (guestfs_cred_rs(x->guest_slot, uid, gid) != 0) { *uid = 0; *gid = 0; }
}

static dos_svc_sft_t *sft_of(dos_svc_ctx_t *x, uint16_t h) {
    if (h >= DOS_SVC_MAX_FH) return 0;
    int i = x->jft[h];
    if (i < 0 || i >= DOS_SVC_MAX_SFT) return 0;
    if (x->sft[i].refs <= 0) return 0;
    return &x->sft[i];
}

// #745: is this DOS name the EMS driver's device? A DOS program detects
// expanded memory by OPENING THE DRIVER AS A FILE and then IOCTLing the handle,
// so this test is the FIRST half of EMS detection and the half most easily
// missed: a manager that answers INT 67h but cannot be opened by name is never
// asked anything at all. Measured, an EMS implementation that did only the
// interrupt recorded ZERO INT 67h calls.
//
// Matched on the BASENAME and case-insensitively, because DOS device names are
// reachable through any path or drive prefix and DOS-era code mixes case
// freely. EMMQXXX0 is the alternate name some managers answer to.
static int dos_name_is_emm(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    static const char *const names[2] = { "EMMXXXX0", "EMMQXXX0" };
    for (int n = 0; n < 2; n++) {
        int i = 0;
        for (; names[n][i]; i++) {
            char ch = b[i];
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            if (ch != names[n][i]) break;
        }
        if (!names[n][i] && b[i] == '\0') return 1;
    }
    return 0;
}

// #234h: is this DOS name a CHARACTER DEVICE, and which one? Returns a
// DOS_CHARDEV_* value or 0.
//
// Matched on the BASENAME and case-insensitively, for the reason spelled out
// above dos_name_is_emm(): a DOS device name is reachable through ANY path or
// drive prefix. "CON", "C:\CON", "\DIG\CON" and "con" are all the console,
// and a program that builds a path and appends a device name is doing the
// normal thing, not something exotic. The Dig arrived here as
// "/WINDIR/DRIVE_E/DIG/CON" after resolution, which is exactly why the check
// has to run on the RAW name BEFORE dos_svc_resolve().
//
// A trailing extension is stripped, because DOS accepts "CON.TXT" as CON. That
// is not a curiosity: it is the reason a DOS program can `COPY FILE.TXT CON`
// and the reason "NUL.EXT" is a valid bit bucket, and a matcher that misses it
// sends those to the filesystem.
//
// PRN and AUX are DELIBERATELY ABSENT and this is a decision, not an omission.
// There is no printer and no serial port to give a guest, and the honest answer
// to "open PRN" is the failure it gets today rather than a handle that silently
// eats a print job. NUL is different: a bit bucket that discards is not a
// fiction, it is the whole specification.
static int dos_name_chardev(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    // Take the stem: up to the first '.', at most 8 characters, upper-cased.
    char stem[9];
    int n = 0;
    for (; b[n] && b[n] != '.' && n < 8; n++) {
        char ch = b[n];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        stem[n] = ch;
    }
    stem[n] = '\0';
    // Anything left over that is not a plain extension is not a device name.
    if (b[n] && b[n] != '.') return 0;
    if (stem[0] == 'C' && stem[1] == 'O' && stem[2] == 'N' && stem[3] == 0)
        return DOS_CHARDEV_CON;
    if (stem[0] == 'N' && stem[1] == 'U' && stem[2] == 'L' && stem[3] == 0)
        return DOS_CHARDEV_NUL;
    return 0;
}

static int jft_alloc(dos_svc_ctx_t *x) {
    for (int i = DOS_SVC_FIRST_FILE_FH; i < DOS_SVC_MAX_FH; i++)
        if (x->jft[i] < 0) return i;
    return -1;
}
static int sft_alloc(dos_svc_ctx_t *x) {
    for (int i = 0; i < DOS_SVC_MAX_SFT; i++)
        if (x->sft[i].refs <= 0) return i;
    return -1;
}

// Pull the whole file into RAM so it can be modified. A read-only handle stays
// STREAMING (no heap, exactly as before); this runs only on the first write.
//
// It is read-whole/write-back rather than streaming-write for a measured
// reason: fat_write() REFUSES an ext2-backed handle (fs/fat.c: "an ext2-backed
// handle has no FAT cluster chain, so fat_write_inner would write into whatever
// cluster 0 maps to"), and the shipping golden is ext2-rooted. Whole-file
// write-back is also the model exec/win16api.c's _lopen/_lclose has used since
// #133, including for Word 6 document saves, so this reuses a mechanism that
// has been carrying real user data rather than inventing one.
static int sft_to_buffer(dos_svc_ctx_t *x, dos_svc_sft_t *f) {
    (void)x;
    if (f->buf) return 0;
    uint32_t sz = 0;
    void *d = f->path[0] ? fat_read_file(&g_fat_fs, f->path, &sz) : 0;
    if (f->streaming) { fat_close(&f->fat); f->streaming = 0; }
    if (!d) { sz = 0; d = kmalloc(64); if (!d) return -1; f->cap = 64; }
    else    { f->cap = sz ? sz : 1; }
    f->buf = (uint8_t *)d;
    f->size = sz;
    return 0;
}

// Grow the in-RAM image so `need` bytes fit. Doubling, so a program writing a
// savegame in 128-byte records does not reallocate once per record.
static int sft_reserve(dos_svc_sft_t *f, uint32_t need) {
    if (need <= f->cap) return 0;
    uint32_t ncap = f->cap ? f->cap : 64;
    while (ncap < need) {
        if (ncap > 0x4000000u) return -1;          // 64 MiB per file is plenty
        ncap *= 2;
    }
    uint8_t *nb = (uint8_t *)kmalloc(ncap);
    if (!nb) return -1;
    for (uint32_t i = 0; i < f->size && i < f->cap; i++) nb[i] = f->buf[i];
    for (uint32_t i = f->size; i < ncap; i++) nb[i] = 0;
    if (f->buf) kfree(f->buf);
    f->buf = nb; f->cap = ncap;
    return 0;
}

// THE MOMENT THE DATA ACTUALLY LANDS. Returns 0 on success or nothing-to-do.
static int sft_commit(dos_svc_ctx_t *x, dos_svc_sft_t *f) {
    if (!f->dirty || !f->path[0]) return 0;
    // Re-check at write-back. The open gate approved this path when the handle
    // was created; a chmod since then must take effect, and THIS is the moment
    // the bytes reach the medium.
    if (!dos_svc_allow(x, f->path, W_OK, "INT21/commit write-back")) {
        f->commit_failed = 1; f->wrote = 1;
        x->n_commit_fail++;
        kprintf("[int21:%s] COMMIT DENIED '%s' (%u bytes NOT written)\n",
                x->tag, f->path, f->size);
        return -1;
    }
    int rc = fat_write_file(&g_fat_fs, f->path, f->buf ? (const void *)f->buf : "", f->size);
    f->wrote = 1;
    if (rc != 0) {
        f->commit_failed = 1;
        x->n_commit_fail++;
        kprintf("[int21:%s] COMMIT FAILED '%s' (%u bytes NOT written)\n",
                x->tag, f->path, f->size);
        return -1;
    }
    uint32_t uid, gid; svc_creator(x, &uid, &gid);
    perms_on_create(f->path, uid, gid, 0);
    f->dirty = 0;
    x->n_commits++;
    kprintf("[int21:%s] commit '%s' %u bytes -> ok\n", x->tag, f->path, f->size);
    return 0;
}

int dos_svc_commit(dos_svc_ctx_t *x, int handle) {
    dos_svc_sft_t *f = sft_of(x, (uint16_t)handle);
    if (!f) return -1;
    return sft_commit(x, f);
}

// Drop one reference. The LAST one commits and frees; the others just go.
// Returns 0, or negative if the commit did not land (AH=3Eh reports it: telling
// a program its file is closed when the bytes never reached the disk is the
// exact defect this stage exists to remove).
static int handle_close(dos_svc_ctx_t *x, uint16_t h) {
    if (h >= DOS_SVC_MAX_FH) return -1;
    int i = x->jft[h];
    if (i < 0) return -1;
    x->jft[h] = -1;
    dos_svc_sft_t *f = &x->sft[i];
    if (--f->refs > 0) return 0;               // another handle still holds it
    // #745: a character device has no fat_file_t, no buffer and no path, so
    // there is nothing to commit and nothing to close. sft_commit() would in
    // fact return 0 harmlessly (it early-outs on an empty path), but relying on
    // that is relying on an unrelated function's internals staying that way.
    if (f->chardev) { memset(f, 0, sizeof(*f)); return 0; }
    int rc = sft_commit(x, f);
    if (f->streaming) fat_close(&f->fat);
    if (f->buf) kfree(f->buf);
    memset(f, 0, sizeof(*f));
    return rc;
}

// ---- find first / find next ---------------------------------------------
static char up1(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

int dos_svc_wild_match(const char *pat, const char *name) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;                    // trailing * matches rest
            while (*name) {
                if (dos_svc_wild_match(pat, name)) return 1;
                name++;
            }
            return dos_svc_wild_match(pat, name);   // allow * = empty at end
        } else if (*pat == '?') {
            if (!*name) return 0;
            pat++; name++;
        } else {
            if (up1(*pat) != up1(*name)) return 0;
            pat++; name++;
        }
    }
    return (*name == '\0');
}

// Render an 11-byte FAT entry name into "NAME.EXT".
static void fat11_to_dotname(const uint8_t *raw, char *out) {
    int o = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) out[o++] = (char)raw[i];
    if (raw[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) out[o++] = (char)raw[i];
    }
    out[o] = '\0';
}

// Write a DOS find result into the guest DTA (43-byte FindFirst structure).
static void write_find_result(dos_svc_ctx_t *x, const char *dotname,
                              uint32_t fsize, uint8_t attr) {
    uint16_t s = x->dta_seg, o = x->dta_off;
    g_wr8 (x, s, (uint16_t)(o + 0x15), attr);
    g_wr16(x, s, (uint16_t)(o + 0x16), 0);        // time
    g_wr16(x, s, (uint16_t)(o + 0x18), 0);        // date
    g_wr16(x, s, (uint16_t)(o + 0x1A), (uint16_t)(fsize & 0xFFFF));
    g_wr16(x, s, (uint16_t)(o + 0x1C), (uint16_t)(fsize >> 16));
    int i = 0;
    for (; dotname[i] && i < 12; i++) g_wr8(x, s, (uint16_t)(o + 0x1E + i), (uint8_t)dotname[i]);
    g_wr8(x, s, (uint16_t)(o + 0x1E + i), 0);
}

// DOS attribute-mask matching for 4Eh / 4Fh (the CX mask).
//
// THE MASK PERMITS, IT DOES NOT SELECT. A normal file (none of hidden, system,
// directory or volume-label set) matches EVERY mask, including zero; each of
// hidden (0x02), system (0x04) and directory (0x10) is returned only when its
// own bit is present in the mask. That is the DOS rule, and it is the reason
// CX had to start being read: an implementation that discards CX cannot express
// any of it, so every attribute-qualified search silently degenerated into the
// same unqualified one.
//
// Subdirectories were previously skipped UNCONDITIONALLY ("for simplicity"),
// which is the same defect wearing a different hat: a program that asks for
// directories was told there are none. They now appear exactly when asked for.
static int attr_match(uint16_t mask, uint8_t attr) {
    // Volume labels never come from the enumeration. See find_volume_step().
    if (attr & 0x08) return 0;
    if ((attr & 0x02) && !(mask & 0x02)) return 0;
    if ((attr & 0x04) && !(mask & 0x04)) return 0;
    if ((attr & 0x10) && !(mask & 0x10)) return 0;
    return 1;
}

// (doscd) THE 4Eh DIRECTORY/PATTERN SPLIT, in one externally testable place.
//
// Factored out of the AH=4Eh arm so dos_find_split_selftest() below exercises
// the SHIPPING function rather than a copy of it. A copy is what let the
// original defect hide: the rule "E:\\ and E:\\*.* are the same request" was
// written in a comment next to code that did not implement it.
//
// `fp` is an already-resolved NATIVE path. On return `dirpath` is the directory
// to enumerate and `pat` is the filename pattern ("" for a drive root).
void dos_find_split(const char *fp, char *dirpath, int dircap,
                    char *pat, int patcap) {
    int slash = -1;
    for (int i = 0; fp[i]; i++) if (fp[i] == '/') slash = i;
    // A RESOLVED PATH THAT IS ITSELF A DRIVE ROOT HAS NO SPLIT TO MAKE, and
    // this test must come FIRST because the generic split below is wrong for it.
    //
    // "E:\\" resolves to /WINDIR/DRIVE_E. Splitting that at the last slash
    // yields the directory /WINDIR and the pattern DRIVE_E, so find_drive
    // (computed from the directory half) is 0, find_volume_step() bails on
    // "not a drive root: no label here", and a VOLUME-LABEL search on the drive root
    // answers "no such disc" for every letter, forever.
    //
    // MEASURED (2026-08-29). Red Alert asks for its disc exactly this way.
    // On kernel 2241: "4Eh findfirst dir='/WINDIR/DRIVE_E' pat='' attr=0008
    // drive=E -> hit" then "4Eh volume label on E: -> 'CD1'". On dev HEAD:
    // "4Eh findfirst dir='/WINDIR' pat='DRIVE_E' attr=0008 drive=- -> none",
    // and the game puts up "PLEASE INSERT A RED ALERT CD INTO THE CD-ROM
    // DRIVE" with the disc mounted and listed on the desktop.
    //
    // WHAT CHANGED UNDERNEATH IT. This code never handled the drive root; it
    // only LOOKED as though it did, because dos_resolve_path_ex() used to emit
    // "/WINDIR/DRIVE_E/" WITH a trailing slash, which the split happened to
    // turn into (dir=/WINDIR/DRIVE_E, pat=""). Commit 6d14fece added
    // dospath_canon_rs(), whose header said "a trailing slash is not preserved.
    // Nothing in this tree depends on one." This did. The correctness of every
    // volume-label search rested on incidental punctuation in a string, so it
    // is a rule here instead: ask whether the path IS a drive root.
    if (dos_native_root_drive(fp)) {
        int n = 0;
        for (; fp[n] && n < dircap - 1; n++) dirpath[n] = fp[n];
        // Trailing separators are dropped: "E:\\" and "E:" must hand fat_open()
        // the SAME directory string, and a path ending in '/' is not one this
        // tree's filesystems open.
        while (n > 0 && (dirpath[n - 1] == '/' || dirpath[n - 1] == '\\')) n--;
        dirpath[n] = '\0';
        pat[0] = '\0';
        return;
    }
    if (slash < 0) {
        dirpath[0] = '/'; dirpath[1] = '\0';
        int k = 0;
        for (; fp[k] && k < patcap - 1; k++) pat[k] = fp[k];
        pat[k] = '\0';
        return;
    }
    int n = slash > 0 ? slash : 1;
    if (n > dircap - 1) n = dircap - 1;
    for (int i = 0; i < n; i++) dirpath[i] = fp[i];
    dirpath[n] = '\0';
    const char *tail = fp + slash + 1;
    int k = 0;
    for (; tail[k] && k < patcap - 1; k++) pat[k] = tail[k];
    pat[k] = '\0';
}

// The volume-label half of a find. Returns 1 when the DTA now holds the label.
//
// WHY THE LABEL IS SYNTHESISED AND NOT ENUMERATED. It is not a directory entry
// on any medium this layer serves. An ISO 9660 disc keeps its name in the
// primary volume descriptor, hundreds of megabytes away from any directory, and
// the FAT12 reader filters the 0x08 entry out of listings (correctly: it is not
// a file). Real MSCDEX synthesises one for a CD in exactly the same way and for
// exactly the same reason. diskimg_volume_label() is the single source.
//
// WHY THIS IS WORTH ANYTHING. _dos_findfirst(path, _A_VOLID, &find) is how a
// DOS program asks "which disc is in this drive", and it is how a multi-disc
// game decides whether the right one is inserted. With CX discarded and the
// 0x08 entries filtered, that call could not succeed on ANY drive, so disc
// identification returned "no such disc" for every letter, forever.
static int find_volume_step(dos_svc_ctx_t *x) {
    if (!(x->find_attr & 0x08)) return 0;   // this search did not ask for one
    if (x->find_vol_done) return 0;         // a volume has exactly one label
    x->find_vol_done = 1;
    if (!x->find_drive) return 0;           // not a drive root: no label here
    char lbl[16];
    if (!diskimg_volume_label(x->find_drive, lbl, sizeof lbl)) return 0;
    // The filename pattern is deliberately NOT applied. A volume-label search
    // names the directory it searches, and both spellings DOS accepts ("E:\\"
    // and "E:\\*.*") mean the same request; matching a label like "CD1" against
    // the pattern would answer one spelling and silently miss the other.
    //
    // (doscd) BOTH SPELLINGS ONLY REACH HERE BECAUSE OF THE DRIVE-ROOT TEST IN
    // THE 4Eh HANDLER. This comment claimed the two were equivalent while the
    // caller silently sent "E:\\" to the directory /WINDIR with the pattern
    // DRIVE_E, so find_drive was 0 and this function returned at the line
    // above. A claim about two callers being equivalent is worth exactly as
    // much as the test that makes them so.
    write_find_result(x, lbl, 0, 0x08);
    kprintf("[int21:%s] 4Eh volume label on %c: -> '%s'\n", x->tag, x->find_drive, lbl);
    return 1;
}

// #rawrite: in the overlay phase, has the BASE directory already returned this
// name? Without this test a file that exists in both (REDALERT.INI, seeded into
// the overlay at first run and still present in the install) is enumerated
// TWICE, and a program that builds a list from a 4Eh/4Fh walk shows it twice.
// A probe per overlay entry rather than a remembered name set: the overlay
// holds only what the guest itself wrote, so the walk is short, and a
// fixed-size "already seen" array would be a silent cap on how many saves can
// be listed.
static int overlay_shadows_base(dos_svc_ctx_t *x, const char *name) {
    if (!x->find_dirpath[0]) return 0;
    char probe[DOS_SVC_PATH_MAX];
    int n = 0;
    for (; x->find_dirpath[n] && n < (int)sizeof(probe) - 2; n++) probe[n] = x->find_dirpath[n];
    if (n > 0 && probe[n - 1] != '/') probe[n++] = '/';
    for (int i = 0; name[i] && n < (int)sizeof(probe) - 1; i++) probe[n++] = name[i];
    probe[n] = '\0';
    return path_exists(x, probe);
}

// Advance the active find iteration. 0 + DTA filled on a match, -1 at end.
//
// #rawrite: TWO PHASES, not one. An overlaid directory is two real directories,
// and a save game that exists only in the per-user overlay must still appear in
// a SAVEGAME.* scan or the game's own load menu is empty. Phase 0 walks the
// base install, phase 1 walks the overlay skipping anything the base already
// produced. find_ovl empty means there is only ever phase 0, which is every
// guest that has no overlay configured.
static int find_step(dos_svc_ctx_t *x) {
    if (!x->find_active) return -1;
    // The label goes FIRST, which is where a real FAT volume keeps it.
    if (find_volume_step(x)) return 0;
    // A mask of EXACTLY 0x08 is a volume-label-ONLY search: DOS returns the
    // label and nothing else. This is the case _A_VOLID compiles to, so getting
    // it wrong would hand a disc-identification caller the first file on the
    // disc and let it compare that against its table of disc names.
    if (x->find_attr == 0x08) {
        x->find_active = 0;
        fat_close(&x->find_dir);
        return -1;
    }
    for (;;) {
        fat_dir_entry_t e;
        // #490: fat_readdir reconstructs VFAT long names of up to 255 chars into
        // this buffer, so it MUST be 256 bytes (the fat_readdir caller contract; a
        // smaller array is now a COMPILE error, see fat.h).
        char namebuf[256];
        const char *dirnow = x->find_phase ? x->find_ovl : x->find_dirpath;
        if (dirnow[0] &&
            !dos_svc_allow(x, dirnow, R_OK | X_OK, "INT21/4Fh findnext")) return -1;
        while (fat_readdir(&x->find_dir, &e, namebuf) == 0) {
            if (e.name[0] == 0x00) break;             // end of directory
            if ((uint8_t)e.name[0] == 0xE5) continue; // deleted
            if (!attr_match(x->find_attr, e.attr)) continue;
            char dot[16];
            fat11_to_dotname(e.name, dot);
            if (dot[0] == 0) continue;
            // "." and ".." exist in a SUBDIRECTORY and never in a drive root, which
            // is what real DOS reports and what a program walking a tree is written
            // against. Both the ext2 and FAT readers below hand them back
            // everywhere. This only started mattering when directories began being
            // returned at all (they were skipped unconditionally before): a
            // recursive walker that meets "." in the root it started from has a
            // cycle straight back to itself.
            if (x->find_drive && dot[0] == '.') continue;
            if (x->find_phase == 1 && dot[0] == '.') continue;
            if (x->find_phase == 1 && overlay_shadows_base(x, dot)) continue;
            if (dos_svc_wild_match(x->find_pat, dot)) {
                write_find_result(x, dot, e.file_size, e.attr);
                return 0;
            }
        }
        fat_close(&x->find_dir);
        if (x->find_phase == 0 && x->find_ovl[0] &&
            fat_open(&g_fat_fs, x->find_ovl, &x->find_dir) == 0) {
            x->find_phase = 1;
            continue;                    // same pattern, second directory
        }
        break;
    }
    x->find_active = 0;
    return -1;
}

// ---- lifecycle -----------------------------------------------------------
static const char *priv_cwd_get(dos_svc_ctx_t *x, char drive) {
    char d = up1(drive);
    if (d < 'A' || d > 'Z') return "";
    return x->cwd_priv[d - 'A'];
}
static void priv_cwd_set(dos_svc_ctx_t *x, char drive, const char *path) {
    char d = up1(drive);
    if (d < 'A' || d > 'Z') return;
    char *dst = x->cwd_priv[d - 'A'];
    int n = 0;
    if (path) {
        while (*path == '/' || *path == '\\') path++;
        for (; path[n] && n < DOS_SVC_CWD_MAX - 1; n++)
            dst[n] = up1(path[n] == '\\' ? '/' : path[n]);
    }
    while (n > 0 && dst[n - 1] == '/') n--;
    dst[n] = '\0';
}

void dos_svc_ctx_init(dos_svc_ctx_t *x, uint32_t guest_slot, const char *tag) {
    memset(x, 0, sizeof(*x));
    x->guest_slot = guest_slot;
    x->tag        = tag ? tag : "guest";
    x->cur_drive  = 'C';
    x->psp_seg    = 0x0000;
    x->dos_version = 0x0005;          // DOS 5.0 unless the caller says otherwise
    x->cwd_get    = priv_cwd_get;
    x->cwd_set    = priv_cwd_set;
    x->dta_off    = 0x0080;           // the PSP default DTA
    for (int i = 0; i < DOS_SVC_MAX_FH; i++) x->jft[i] = -1;
}

void dos_svc_ctx_close_all(dos_svc_ctx_t *x) {
    // A guest that exits with a file open (which is most of them: DOS does the
    // closing for you at 4Ch) must still get its data written. This is the last
    // point at which the identity slot is still armed, which is why the caller
    // must run it BEFORE guestfs_finish().
    for (int i = 0; i < DOS_SVC_MAX_FH; i++)
        if (x->jft[i] >= 0) (void)handle_close(x, (uint16_t)i);
    if (x->find_active) { fat_close(&x->find_dir); x->find_active = 0; }
}

void dos_svc_report(dos_svc_ctx_t *x) {
    kprintf("[int21:%s] %u calls, %u unimplemented, %u file writes (%u bytes), "
            "%u committed, %u commit FAILURES",
            x->tag, x->n_calls, x->n_miss, x->n_writes, x->n_bytes_written,
            x->n_commits, x->n_commit_fail);
    if (x->last_miss_n) {
        kprintf(", last unimplemented AH:");
        for (int i = 0; i < x->last_miss_n; i++) kprintf(" %02x", x->last_miss[i]);
    }
    kprintf("\n");
}

static void note_miss(dos_svc_ctx_t *x, uint8_t ah) {
    x->n_miss++;
    for (int i = 0; i < x->last_miss_n; i++) if (x->last_miss[i] == ah) return;
    if (x->last_miss_n < (int)sizeof(x->last_miss)) x->last_miss[x->last_miss_n++] = ah;
}

// ===========================================================================
// #736 Stage 2: THE WIN16 LAYER HAS A SECOND HANDLE SPACE, AND A GUEST CAN
// CARRY A HANDLE ACROSS.
//
// MEASURED, and it cost a Word 6 regression before it was understood. Word
// opens a file through the Win16 KERNEL API (_lopen -> win16api.c's g_files[],
// handles 1..63) and then issues INT 21h AH=45h on the handle it got back:
//
//   [int21:win16] AH=45 ... ax=4597 bx=0007
//
// On REAL Windows that is correct, because _lopen is a thin wrapper over INT
// 21h 3Dh and there is ONE handle space. Here there are two, and handle 7 means
// nothing to this core. Answering the DOS-correct "invalid handle" made Word 6
// terminate during startup with 4Ch: window opened, no menu bar, no document.
//
// So for a handle this core does not own, 45h/46h behave as they did before
// Stage 2: CF clear, registers untouched. That IS still a fiction and it is
// listed as one. The REAL fix is to give win16api.c's file objects handles out
// of THIS table, which is the same consolidation this ticket did for INT 21h
// and for the DTA, and it is the next piece of work rather than this one.
//
// Returns 1 if the handle belongs to this core (carry on), 0 if the caller
// should stop because the compatibility answer has already been given.
// ===========================================================================
static int svc_handle_is_ours(dos_svc_ctx_t *x, x86_16_cpu_t *c, uint16_t h,
                              const char *what) {
    if (h < DOS_SVC_MAX_FH && x->jft[h] >= 0) return 1;
    if (!x->warned_foreign_handle) {
        x->warned_foreign_handle = 1;
        kprintf("[int21:%s] %s on handle %u, which this core did not open: it is "
                "a Win16 KERNEL handle. Answering as DOS did before #736 Stage 2 "
                "(CF clear, registers untouched). The two handle spaces are not "
                "unified yet.\n", x->tag, what, h);
    }
    CLR_CF(c);
    return 0;
}

// ===========================================================================
// THE DISPATCHER
// ===========================================================================
void dos_svc_int21(dos_svc_ctx_t *x, x86_16_cpu_t *c) {
    uint8_t ah = AH(c);
    x->n_calls++;
    CLR_CF(c);
    // #221: cleared HERE, at the one entry, rather than trusted to every
    // caller. A flag that stays set because one caller forgot would make every
    // later interrupt look blocked and livelock the guest; clearing on entry
    // means the flag can only ever describe the call that just ran.
    x->input_blocked = 0;
    if (g_int21_trace)
        kprintf("[int21:%s] AH=%02x al=%02x bx=%04x cx=%04x dx=%04x ds=%04x es=%04x cs:ip=%04x:%04x\n",
                x->tag, ah, AL(c), c->bx, c->cx, c->dx, c->ds, c->es, c->cs, c->ip);

    switch (ah) {

    // ---- console output --------------------------------------------------
    case 0x02:  // display character (DL)
        con_putc(x, (uint8_t)(c->dx & 0xFF));
        AL_SET(c, c->dx & 0xFF);
        break;

    case 0x09: { // print $-terminated string at DS:DX
        for (int i = 0; i < 4096; i++) {
            uint8_t ch = g_rd8(x, c->ds, (uint16_t)(c->dx + i));
            if (ch == '$') break;
            con_putc(x, ch);
        }
        break;
    }

    // ---- console input ---------------------------------------------------
    // BLOCKING, which is what DOS documents these three as (#221). The
    // interpreter thread is still never blocked: with no key available the
    // service writes NOTHING and raises x->input_blocked, and the caller
    // re-issues the SAME interrupt on a later run-loop pass, after that loop
    // has pumped input, presented a frame and yielded. From the guest's side
    // that is an INT that took a while to return, which is precisely what a
    // blocking BIOS/DOS read is.
    //
    // What it replaced, and why that was wrong: this returned AL=0 with the
    // note "a guest loops, exactly as it does around the real BIOS". A POLLING
    // guest does (AH=0Bh, then AH=07h only once a key exists) and is entirely
    // unaffected by this change, because it never reaches the empty case. A
    // guest that calls the blocking service ONCE and believes the answer is
    // not looping and never will: NetHack's tty_nhgetch() maps a zero to ESC,
    // tty_askname() counts ten ESCs in microseconds and calls
    // bail("Giving up after 10 tries."), and every yn_function() prompt takes
    // its escape default without a key ever being pressed.
    //
    // A context with no keyboard at all (con.getkey == NULL) must NOT block, or
    // it would wait forever for a key that has no source. con_get() answering
    // "no key" is the same shape as an empty queue, so the two are separated
    // here rather than inside con_get().
    case 0x01:   // read char WITH echo      -> AL = char
    case 0x07:   // read char, no echo, no ^C
    case 0x08: { // read char, no echo, checks ^C
        uint16_t k;
        if (con_get(x, &k)) {
            uint8_t ascii = (uint8_t)(k & 0xFF);
            AL_SET(c, ascii ? ascii : (uint8_t)(k >> 8));
            if (ah == 0x01 && ascii) con_putc(x, ascii);
        } else if (x->con.getkey) {
            x->input_blocked = 1;      // caller re-issues this INT; touch nothing
        } else {
            AL_SET(c, 0);              // no keyboard exists: answering is correct
        }
        break;
    }

    case 0x0C: {  // flush input buffer, then run the input call in AL
        // BOUNDED drain, not a wait: the BIOS ring holds 15 entries, so this
        // always terminates. Written as a counted loop with a real body both
        // because that is what it means and because concurrency-lint reads an
        // empty-bodied loop as a busy-wait, which is the correct default.
        for (int i = 0; i < 16; i++) {
            uint16_t k;
            if (!con_get(x, &k)) break;
        }
        uint8_t sub_fn = AL(c);
        if (sub_fn == 0x01 || sub_fn == 0x06 || sub_fn == 0x07 ||
            sub_fn == 0x08 || sub_fn == 0x0A) {
            AH_SET(c, sub_fn);
            dos_svc_int21(x, c);   // one definition of each input call, not a copy
            // #221: if the sub-function blocked, the caller is about to re-issue
            // THIS interrupt, so the register file has to look exactly as the
            // guest left it - including the AH=0Ch we just overwrote. (Re-doing
            // the flush on the retry is harmless: the buffer is already empty.)
            if (x->input_blocked) AH_SET(c, 0x0C);
        }
        break;
    }

    case 0x06:  // direct console I/O
        if ((c->dx & 0xFF) == 0xFF) {
            uint16_t k;
            if (con_get(x, &k)) {
                // AL = the ASCII byte; for a key that has none (arrows, function
                // keys) DOS hands back the SCAN code instead.
                uint8_t ascii = (uint8_t)(k & 0xFF);
                AL_SET(c, ascii ? ascii : (uint8_t)(k >> 8));
                c->flags &= ~0x0040;
            } else { AL_SET(c, 0); c->flags |= 0x0040; }   // ZF=1 -> no char
        } else {
            con_putc(x, (uint8_t)(c->dx & 0xFF));
        }
        break;

    case 0x0B: {  // check stdin status (non-destructive)
        uint16_t k;
        AL_SET(c, con_peek(x, &k) ? 0xFF : 0x00);
        break;
    }

    // ---- drives, DTA, version, PSP, IVT ----------------------------------
    case 0x0E:  // select default drive: DL=drive (0=A,2=C,4=E) -> AL=#drives
        {
            char d = (char)('A' + (uint8_t)(c->dx & 0xFF));
            if (dos_drive_known(d)) x->cur_drive = d;
            AL_SET(c, (uint8_t)dos_drive_count());
        }
        break;

    case 0x19:  // get current default drive -> AL (0=A,2=C,4=E)
        AL_SET(c, (uint8_t)(x->cur_drive - 'A'));
        break;

    case 0x1A:  // set DTA (DS:DX)
        x->dta_seg = c->ds;
        x->dta_off = c->dx;
        break;

    case 0x2F:  // get DTA -> ES:BX
        c->es = x->dta_seg;
        c->bx = x->dta_off;
        break;

    case 0x25:  // set interrupt vector (AL=int, DS:DX=handler)
        if (x->has_ivt) {
            uint8_t vec = AL(c);
            g_wr16(x, 0x0000, (uint16_t)(vec * 4),     c->dx);   // offset
            g_wr16(x, 0x0000, (uint16_t)(vec * 4 + 2), c->ds);   // segment
            // No flag is latched here on purpose: writing the IVT IS the
            // installation, and the DOS run loop re-derives its hook flags from
            // the IVT, so this path and a direct IVT write share ONE mechanism.
        }
        break;

    case 0x35:  // get interrupt vector AL -> ES:BX
        if (x->has_ivt) {
            uint8_t vec = AL(c);
            c->bx = g_rd16(x, 0x0000, (uint16_t)(vec * 4));
            c->es = g_rd16(x, 0x0000, (uint16_t)(vec * 4 + 2));
        } else {
            c->es = 0; c->bx = 0;
        }
        break;

    // (#234a) BOTH of these used to answer a compile-time constant: 19 Nov 1992
    // for ever, and midnight for ever. That is not cosmetic. MEASURED on Epyx
    // Rogue (/DOS/ROGUE/ROGUE.EXE, golden 2053): its seed is CX+DX from AH=2Ch,
    // so the seed was 0; its generator is seed = seed*125 mod 2796203, for which
    // ZERO IS A FIXED POINT; every rnd() returned 0; and dungeon generation,
    // being a rejection loop over rnd(), never terminated. The game showed its
    // title, took one keypress, cleared to a blank text screen and burned CPU in
    // the C runtime's 32-bit divide helper for ever. It reads as a dead
    // keyboard and the keyboard is not involved.
    //
    // A guest that seeds from the clock is the RULE, not the exception, so this
    // was a whole-class defect: every DOS program on the image replayed the
    // identical "random" sequence on every run.
    case 0x2A: {  // get system date -> CX=year DH=month DL=day AL=dow
        kdos_clock_t k; kdos_clock_now(&k);
        c->cx = k.year;
        c->dx = (uint16_t)(((uint16_t)k.month << 8) | k.day);
        AL_SET(c, k.weekday);
        break;
    }

    case 0x2C: {  // get system time -> CH:CL hour:min DH:DL sec:hundredths
        kdos_clock_t k; kdos_clock_now(&k);
        c->cx = (uint16_t)(((uint16_t)k.hour << 8) | k.minute);
        c->dx = (uint16_t)(((uint16_t)k.second << 8) | k.hundredth);
        break;
    }

    case 0x30:  // get DOS version
        c->ax = x->dos_version;
        c->bx = 0xFF00;
        c->cx = 0;
        break;

    // (#digrun) AH=38h - GET COUNTRY-DEPENDENT INFORMATION.
    //
    // MEASURED on The Dig's IMUSE.EXE, whose Rational DOS/16M loader stub asks
    // for it during startup. It is not a formatting nicety to the programs that
    // call it: a C runtime asks once, near entry, and a refusal there is a
    // refusal from something that has no fallback path.
    //
    // AL=0 is 'the current country' and is the only form anything in the
    // corpus issues; DS:DX is a 34-byte buffer (DOS 3.0+; the DOS 2.x form was
    // 32 and the two extra bytes are the data-list separator, which is why the
    // buffer is filled to 34 and not to 32). DX=FFFFh with AL!=0 is SET
    // country, which this core does not do and declines the documented way.
    //
    // The values are the US/DOS defaults, and they are the honest answer rather
    // than a placeholder: this layer has one locale, decides nothing from it,
    // and a guest that reads mm/dd/yy and '.' as a decimal point is reading
    // exactly what the rest of the emulated environment behaves like.
    case 0x38: {
        if ((c->ax & 0xFF) != 0x00 || c->dx == 0xFFFF) {   // SET country
            svc_err(x, c, 0x0001);
            kprintf("[int21:%s] AH=38h AL=%02x (set country) declined; only "
                    "AL=00h get-current-country is implemented\n",
                    x->tag, c->ax & 0xFF);
            break;
        }
        static const uint8_t ci[34] = {
            0x00, 0x00,                   //  0 date format: 0 = USA mm/dd/yy
            '$', 0, 0, 0, 0,               //  2 currency symbol, ASCIIZ[5]
            ',', 0,                       //  7 thousands separator
            '.', 0,                       //  9 decimal separator
            '-', 0,                       // 11 date separator
            ':', 0,                       // 13 time separator
            0x00,                         // 15 currency format: symbol leads, no space
            0x02,                         // 16 digits after the decimal point
            0x00,                         // 17 time format: 12-hour
            0x00, 0x00, 0x00, 0x00,       // 18 case-map FAR call: none
            ',', 0,                       // 22 data-list separator
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0  // 24 reserved
        };
        for (int i = 0; i < 34; i++)
            g_wr8(x, c->ds, (uint16_t)(c->dx + i), ci[i]);
        c->bx = 0x0001;                   // country code 1 = USA
        CLR_CF(c); x->last_err = 0;
        kprintf("[int21:%s] 38h country info -> 34 bytes at %04x:%04x, country 1 (USA)\n",
                x->tag, c->ds, c->dx);
        break;
    }

    case 0x41: { // delete file DS:DX
        char dp[DOS_SVC_PATH_MAX], fp[DOS_SVC_PATH_MAX];
        rd_asciiz(x, c->ds, c->dx, dp, sizeof(dp));
        if (!dos_path_writable_ex(dp, x->cur_drive)) { svc_err(x, c, 5); break; }
        dos_svc_resolve(x, dp, fp, sizeof(fp));
        if (fat_exists(&g_fat_fs, fp) != 1) { svc_err(x, c, 2); break; }
        // POSIX, and DOS agrees: unlinking needs write+search on the DIRECTORY,
        // not on the file. dos_svc_allow_create() with leaf_exists=0 asks
        // exactly that question of the parent.
        if (!dos_svc_allow_create(x, fp, 0, "INT21/41h delete")) { svc_err(x, c, 5); break; }
        if (fat_delete(&g_fat_fs, fp) != 0) { svc_err(x, c, 5); break; }
        perms_remove(fp);
        CLR_CF(c); c->ax = 0; x->last_err = 0;
        kprintf("[int21:%s] 41h delete '%s' -> ok\n", x->tag, fp);
        break;
    }

    case 0x56: { // rename DS:DX -> ES:DI
        char op[DOS_SVC_PATH_MAX], np[DOS_SVC_PATH_MAX];
        char ofp[DOS_SVC_PATH_MAX], nfp[DOS_SVC_PATH_MAX];
        rd_asciiz(x, c->ds, c->dx, op, sizeof(op));
        rd_asciiz(x, c->es, c->di, np, sizeof(np));
        if (!dos_path_writable_ex(op, x->cur_drive) ||
            !dos_path_writable_ex(np, x->cur_drive)) { svc_err(x, c, 5); break; }
        dos_svc_resolve(x, op, ofp, sizeof(ofp));
        dos_svc_resolve(x, np, nfp, sizeof(nfp));
        if (fat_exists(&g_fat_fs, ofp) != 1) { svc_err(x, c, 2); break; }
        if (fat_exists(&g_fat_fs, nfp) == 1)  { svc_err(x, c, 5); break; }  // exists
        // BOTH directories are modified, so both are checked. Checking only the
        // source would let a guest move a file it may delete into a directory
        // it may not write.
        if (!dos_svc_allow_create(x, ofp, 0, "INT21/56h rename (source dir)") ||
            !dos_svc_allow_create(x, nfp, 0, "INT21/56h rename (target dir)")) {
            svc_err(x, c, 5); break;
        }
        if (fat_rename(&g_fat_fs, ofp, nfp) != 0) { svc_err(x, c, 5); break; }
        perms_remove(ofp);
        { uint32_t uid, gid; svc_creator(x, &uid, &gid); perms_on_create(nfp, uid, gid, 0); }
        CLR_CF(c); c->ax = 0; x->last_err = 0;
        kprintf("[int21:%s] 56h rename '%s' -> '%s' ok\n", x->tag, ofp, nfp);
        break;
    }

    case 0x3A: { // rmdir DS:DX
        char dp[DOS_SVC_PATH_MAX], fp[DOS_SVC_PATH_MAX];
        rd_asciiz(x, c->ds, c->dx, dp, sizeof(dp));
        if (!dos_path_writable_ex(dp, x->cur_drive)) { svc_err(x, c, 5); break; }
        dos_svc_resolve(x, dp, fp, sizeof(fp));
        if (fat_exists(&g_fat_fs, fp) != 1) { svc_err(x, c, 3); break; }
        if (!dos_svc_allow_create(x, fp, 0, "INT21/3Ah rmdir")) { svc_err(x, c, 5); break; }
        // fat_delete() is documented to remove "a file or empty directory".
        if (fat_delete(&g_fat_fs, fp) != 0) { svc_err(x, c, 5); break; }
        perms_remove(fp);
        CLR_CF(c); c->ax = 0; x->last_err = 0;
        kprintf("[int21:%s] 3Ah rmdir '%s' -> ok\n", x->tag, fp);
        break;
    }

    case 0x3B: { // chdir DS:DX
        char dp[DOS_SVC_PATH_MAX], fp[DOS_SVC_PATH_MAX];
        rd_asciiz(x, c->ds, c->dx, dp, sizeof(dp));
        dos_svc_resolve(x, dp, fp, sizeof(fp));
        // (#740) A FAILING chdir logged NOTHING, and a failing chdir is a
        // decision point for a real guest: Discworld II switches to each
        // candidate drive, chdirs, and moves on to the next drive if that
        // fails, so a silent failure here is a silent "this is not the disc".
        // The register effect was already correct; only the diagnostic was
        // missing, which is exactly the case that costs a whole measurement
        // round to rediscover.
        if (!dos_svc_allow(x, fp, R_OK | X_OK, "INT21/3Bh chdir")) {
            kprintf("[int21:%s] 3Bh chdir DENIED '%s' -> '%s'\n", x->tag, dp, fp);
            svc_err(x, c, 5); break;
        }
        fat_file_t probe;
        if (fat_open(&g_fat_fs, fp, &probe) != 0) {
            kprintf("[int21:%s] 3Bh chdir FAIL '%s' -> '%s' (no such path)\n",
                    x->tag, dp, fp);
            svc_err(x, c, 3); break;
        }
        int isdir = fat_is_dir(&probe);
        fat_close(&probe);
        if (!isdir) {
            kprintf("[int21:%s] 3Bh chdir FAIL '%s' -> '%s' (not a directory)\n",
                    x->tag, dp, fp);
            svc_err(x, c, 3); break;
        }
        // Which drive's CWD this sets: an explicit "X:" names it, otherwise the
        // current drive. The stored form is the drive-relative native path,
        // which is what AH=47h answers and what dos_resolve_path_ex() resolves
        // "X:NAME" against, so the two cannot disagree.
        char drv = (dp[0] && dp[1] == ':') ? up1(dp[0]) : x->cur_drive;
        const char *rel = fp;
        {
            char pfx[24]; int n = 0;
            const char *w = "/WINDIR/DRIVE_";
            for (int i = 0; w[i]; i++) pfx[n++] = w[i];
            pfx[n++] = drv; pfx[n] = '\0';
            int pl = n;
            if (strncmp(fp, pfx, (unsigned)pl) == 0) rel = fp + pl;
        }
        while (*rel == '/') rel++;
        x->cwd_set(x, drv, rel);
        CLR_CF(c); c->ax = 0; x->last_err = 0;
        kprintf("[int21:%s] 3Bh chdir %c: -> '%s'\n", x->tag, drv, rel);
        break;
    }

    case 0x45: { // dup handle BX -> AX = new handle
        // A REAL dup: the new handle shares the OPEN FILE, so a seek or a write
        // through either moves the other's position. That is what DOS does, and
        // it is only expressible because the handle table is two-level.
        uint16_t h = c->bx;
        if (!svc_handle_is_ours(x, c, h, "45h dup")) break;
        if (h >= DOS_SVC_MAX_FH || x->jft[h] < 0) { svc_err(x, c, 6); break; }
        int nh = jft_alloc(x);
        if (nh < 0) { svc_err(x, c, 4); break; }
        x->jft[nh] = x->jft[h];
        x->sft[(int)x->jft[h]].refs++;
        c->ax = (uint16_t)nh;
        CLR_CF(c);
        break;
    }

    case 0x46: { // dup2: force handle CX to be a duplicate of BX
        uint16_t h = c->bx, nh = c->cx;
        if (!svc_handle_is_ours(x, c, h, "46h dup2")) break;
        if (h >= DOS_SVC_MAX_FH || x->jft[h] < 0) { svc_err(x, c, 6); break; }
        if (nh >= DOS_SVC_MAX_FH || nh < DOS_SVC_FIRST_FILE_FH) { svc_err(x, c, 6); break; }
        if (nh == h) { c->ax = nh; CLR_CF(c); break; }
        if (x->jft[nh] >= 0) (void)handle_close(x, nh);   // dup2 closes the target
        x->jft[nh] = x->jft[h];
        x->sft[(int)x->jft[h]].refs++;
        c->ax = nh;
        CLR_CF(c);
        break;
    }

    case 0x5C: { // lock (AL=0) / unlock (AL=1) a file region
        // #736 Stage 2: a REAL no-op, and the distinction matters. Before this
        // it fell to the unimplemented default and returned CF clear with the
        // function number still in AX, i.e. an accidental success. Turning that
        // into "invalid function" is what DOS with no SHARE loaded answers, and
        // it is what this core did for one build: measured, it made Word 6
        // terminate during startup, because Windows 3.1 always loaded SHARE and
        // Word takes a lock on its own temp file.
        //
        // So it succeeds, and the postcondition the caller relies on is
        // genuinely TRUE rather than pretended: at most one 16-bit guest runs at
        // a time (g_dos_busy / g_win16_busy), and this core holds the only
        // handle table, so there is no second opener a lock could exclude. A
        // lock that cannot be contended is a lock that is always held.
        //
        // If concurrent guests ever share a file, THIS is a fiction again and
        // has to become a real region lock on the SFT entry.
        uint16_t h = c->bx;
        if (!svc_handle_is_ours(x, c, h, "5Ch lock/unlock")) break;
        c->ax = 0;
        CLR_CF(c);
        break;
    }

    case 0x68:   // commit file (flush without closing)
    case 0x6A: { // ... and its DOS 4 alias
        dos_svc_sft_t *f = sft_of(x, c->bx);
        if (!f) { svc_err(x, c, 6); break; }
        if (sft_commit(x, f) != 0) { svc_err(x, c, 5); break; }
        CLR_CF(c);
        break;
    }

    case 0x36: {  // get disk free space (DL = 0 default, 1 = A:, 2 = B:, ...)
        // AX sectors/cluster, BX free clusters, CX bytes/sector, DX total
        // clusters; AX = 0xFFFF means INVALID DRIVE and is the only error this
        // call has.
        //
        // It used to return the SAME generous fiction (about 256 MB free)
        // for every letter A..Z including letters with no drive behind them, so
        // it could not say "no such drive" and a program enumerating drives
        // through it was told all 26 exist. Three answers now, in order of how
        // much is actually known:
        //
        //  1. NO SUCH DRIVE -> 0xFFFF. dos_drive_known() is the same authority
        //     dos_drive_type() and the MSCDEX letter list use, so a guest cannot
        //     get one answer here and a different one there.
        //  2. A MOUNTED IMAGE -> the medium's OWN geometry and total size,
        //     ZERO free. A mounted disc is read-only and there is no
        //     write-back, so "full" is the truth and not a placeholder. This
        //     is also what MSCDEX reports for a CD.
        //
        //     #234e CORRECTED THE GEOMETRY HALF OF THIS. It used to report
        //     2048-byte sectors for every mounted image because the class it
        //     was written for was CD-ROM; a FAT12 floppy on A: was described
        //     with a CD's sector size. The numbers now come from
        //     diskimg_geometry(), i.e. from the ISO descriptor or the FAT12
        //     BPB, so neither class is described as the other.
        //  3. ANYTHING ELSE (folder-backed A:/B:, the C: hard disk) -> the old
        //     generous fixed answer, unchanged. It is STILL A FICTION and is
        //     still listed as one: a real number needs a free-space query
        //     spanning FAT and ext2, which is Stage 2 work and not this.
        char drv = DL(c) ? (char)('A' + DL(c) - 1) : x->cur_drive;
        if (!dos_drive_known(drv)) { c->ax = 0xFFFF; CLR_CF(c); break; }
        uint32_t g_bps = 0, g_spc = 0, g_clusters = 0;
        if (diskimg_geometry(drv, &g_bps, &g_spc, &g_clusters)) {
            // THE MEDIUM'S OWN GEOMETRY, NOT A CD'S (#234e). This arm used to
            // hardcode 2048-byte sectors and one sector per cluster for every
            // mounted image, on the reasoning that a mounted image is a CD.
            // Then a FAT12 floppy mounted on A:, and a 720 KB disk that says
            // 512-byte sectors and two sectors per cluster in its own BPB was
            // reported to the guest as 360 single-sector 2048-byte clusters.
            //
            // The TOTAL was right both times, which is why it went unnoticed:
            // 360 * 2048 and 713 * 1024 are both about 720 KB. What was wrong
            // was CX, and CX is the register a program reads when it wants to
            // know how big a sector is before sizing a buffer or a record.
            //
            // diskimg_geometry() answers from the ISO descriptor or the FAT12
            // BPB, so this call now reports what the disk says about itself.
            // Free space stays ZERO for every class, which is not a placeholder:
            // a mounted image is read-only with no write-back (DISKIMG_F_READONLY),
            // so full is the truth.
            c->ax = (uint16_t)g_spc;
            c->cx = (uint16_t)g_bps;
            c->bx = 0;
            c->dx = (uint16_t)g_clusters;
        } else {
            c->ax = 8; c->cx = 512; c->bx = 0xFFFF; c->dx = 0xFFFF;
        }
        CLR_CF(c);
        break;
    }

    case 0x59:  // get extended error (BX=version)
        // MOVED from k_dos3call(). AX = the last error, BH = class, BL =
        // suggested action, CH = locus, CF clear.
        c->ax = x->last_err;
        if (x->last_err == 2) { c->bx = 0x0801; c->cx = (uint16_t)((c->cx & 0x00FF) | 0x0200); }
        else                  { c->bx = 0; }
        CLR_CF(c);
        break;

    case 0x39: { // mkdir (DS:DX)
        // MOVED from k_dos3call(), which is where the tree's only real INT 21h
        // mkdir lived. Two things are fixed by the move rather than added: it
        // now reports the actual result instead of "success regardless (best
        // effort)", and it is reachable from a DOS guest, which it never was.
        char dp[DOS_SVC_PATH_MAX], fp[DOS_SVC_PATH_MAX];
        rd_asciiz(x, c->ds, c->dx, dp, sizeof(dp));
        if (!dos_path_writable_ex(dp, x->cur_drive)) { svc_err(x, c, 5); break; }
        dos_svc_resolve(x, dp, fp, sizeof(fp));
        if (!dos_svc_allow_create(x, fp, 0, "INT21/39h mkdir")) { svc_err(x, c, 5); break; }
        int rc = fat_mkdir(&g_fat_fs, fp);
        // (#740 digplay) RECORD THE OWNER OF WHAT WE JUST MADE, exactly as 3Ch
        // does one case below. Without this the mkdir SUCCEEDS and the guest
        // still cannot use the directory: perms_check_leaf() treats a path with
        // no PERMS.DB row as root:root 0755, so a guest running as uid 1000
        // creates a directory it is then refused write and search on.
        //
        // MEASURED on The Dig. Its launcher answers "Play the Dig" by writing
        // the iMuse sound configuration to C:\DIG\, so it does
        //     4Eh findfirst C:\ "DIG"        -> none
        //     39h mkdir     C:\DIG           -> rc=0, and no perms row
        //     4Eh findfirst C:\DIG "*.*"     -> none
        //     39h mkdir     C:\DIG           -> [GUESTFS-DENY] want=-wx PERMS
        // and on that refusal it abandons the configuration and returns to its
        // main menu. The scratch drive itself is 0777 (dos_scratch_perms), so
        // CREATING names in C:\ was allowed all along; what was missing is the
        // row for the created name, which is the difference between a writable
        // scratch drive and one that can only ever hold files in its root.
        if (rc == 0) {
            uint32_t uid, gid; svc_creator(x, &uid, &gid);
            perms_on_create(fp, uid, gid, 1);
        }
        if (rc == 0)            { CLR_CF(c); c->ax = 0; x->last_err = 0; }
        else if (rc == -17)     { svc_err(x, c, 5); }   // already exists
        else                    { svc_err(x, c, 3); }   // path not found
        kprintf("[int21:%s] 39h mkdir '%s' -> rc=%d\n", x->tag, fp, rc);
        break;
    }

    // (#172) 51h and 62h ARE THE SAME FUNCTION. 51h is the original,
    // undocumented-until-DOS-5 form; 62h is the DOS 3.0+ documented alias, and
    // MS-DOS dispatches both to one routine. Only 62h was implemented, so a
    // program using the older spelling got the generic in-range default
    // (CF=1, AX=1) with BX untouched.
    //
    // MEASURED, /DOS/STUNTS/LOAD.EXE relocated to high memory, one run: it
    // issues 51h, reads BX as its PSP, gets 0, copies 256 bytes from
    // 0000:0000 - THE INTERRUPT VECTOR TABLE - as its PSP template, and then
    // issues `AH=49 ... es=0000`, handing segment 0 back to the allocator.
    // It derailed 20 instructions later on a decoded 0x0F10 at 8d72:82c5.
    // A missing function whose failure mode is "segment 0 looks like a valid
    // answer" is worth two case labels.
    //
    // C, not Rust: this adds a second case label to an existing implemented
    // function in an existing C switch. A Rust version would be an FFI hop to
    // do `c->bx = x->psp_seg`.
    case 0x51:  // get current PSP segment -> BX (undocumented spelling)
    case 0x62:  // get PSP segment -> BX
        c->bx = x->psp_seg;
        break;

    case 0x4C:  // terminate with return code AL
        c->exit_code = AL(c);
        c->halted = 1;
        kprintf("[int21:%s] 4Ch exit code=%d\n", x->tag, c->exit_code);
        if (x->on_terminate) x->on_terminate(x, c->exit_code);
        break;

    // ---- files -----------------------------------------------------------
    case 0x3C: { // create/truncate file DS:DX, attr CX -> AX=handle
        // #736 Stage 2: REAL. This used to allocate a handle and write nothing,
        // so a program that created a file, wrote it and closed it was told
        // three times that everything had worked and lost the lot.
        char dp[DOS_SVC_PATH_MAX], fp[DOS_SVC_PATH_MAX];
        rd_asciiz(x, c->ds, c->dx, dp, sizeof(dp));
        if (!dos_path_writable_ex(dp, x->cur_drive)) { svc_err(x, c, 5); break; }
        dos_svc_resolve(x, dp, fp, sizeof(fp));
        int existed = (fat_exists(&g_fat_fs, fp) == 1);
        // Creating a file that does not exist is a WRITE TO ITS PARENT, which
        // is how sys_open() already treats it. Asking for W_OK on the leaf
        // instead would hit the root-owned 0755 no-entry default and deny every
        // guest create, including a legitimate one.
        if (!dos_svc_allow_create(x, fp, existed, "INT21/3Ch create")) { svc_err(x, c, 5); break; }
        int si = sft_alloc(x);
        int h  = jft_alloc(x);
        if (si < 0 || h < 0) { svc_err(x, c, 4); break; }   // too many open files
        // Materialise an EMPTY file on the medium immediately, before returning
        // a handle. 3Ch is documented to truncate, and a later 43h/4Eh probe by
        // the same program must see the file even if it never writes a byte.
        if (fat_write_file(&g_fat_fs, fp, "", 0) != 0) {
            kprintf("[int21:%s] 3Ch create '%s' FAILED on the medium\n", x->tag, fp);
            svc_err(x, c, 3);                              // path not found
            break;
        }
        {
            uint32_t uid, gid; svc_creator(x, &uid, &gid);
            perms_on_create(fp, uid, gid, 0);
        }
        dos_svc_sft_t *f = &x->sft[si];
        memset(f, 0, sizeof(*f));
        f->refs = 1; f->wr_ok = 1;
        f->buf = (uint8_t *)kmalloc(64);
        if (!f->buf) { memset(f, 0, sizeof(*f)); svc_err(x, c, 8); break; }
        f->cap = 64; f->size = 0; f->pos = 0;
        strncpy(f->path, fp, sizeof(f->path) - 1);
        x->jft[h] = (signed char)si;
        c->ax = (uint16_t)h;
        CLR_CF(c);
        kprintf("[int21:%s] 3Ch create '%s' -> h%d\n", x->tag, fp, h);
        break;
    }

    case 0x3D: { // open file DS:DX, AL=mode -> AX=handle / err
        char dp[DOS_SVC_PATH_MAX], fp[DOS_SVC_PATH_MAX];
        rd_asciiz(x, c->ds, c->dx, dp, sizeof(dp));
        // #745: a CHARACTER DEVICE is not a path and must be answered before
        // any of the filesystem machinery below, because none of it applies:
        // there is nothing to resolve, nothing to gate (the device is ours, not
        // the guest's filesystem) and nothing on disk to open.
        // #234h: the same is true of CON and NUL, and for the same reason.
        // They are answered here, on the RAW name, because resolution would
        // turn "CON" into a path under the current directory and the answer
        // must not depend on where the guest happens to be standing.
        {
            int cd = (x->has_ems && dos_name_is_emm(dp)) ? DOS_CHARDEV_OPAQUE
                                                         : dos_name_chardev(dp);
            if (cd) {
                int dsi = sft_alloc(x);
                int dh  = jft_alloc(x);
                if (dsi < 0 || dh < 0) { svc_err(x, c, 4); break; }
                dos_svc_sft_t *df = &x->sft[dsi];
                memset(df, 0, sizeof(*df));
                df->refs = 1; df->wr_ok = 1; df->chardev = cd;
                x->jft[dh] = (signed char)dsi;
                c->ax = (uint16_t)dh;
                CLR_CF(c);
                kprintf("[int21:%s] 3Dh open '%s' -> h%d (%s character device)\n",
                        x->tag, dp, dh,
                        cd == DOS_CHARDEV_CON ? "CON" :
                        cd == DOS_CHARDEV_NUL ? "NUL" : "EMS");
                break;
            }
        }
        dos_svc_resolve(x, dp, fp, sizeof(fp));
        // #708: THE chokepoint. This is the one place a DOS path becomes a
        // handle, so gating here covers 3Eh/3Fh/42h as well; 3Fh re-checks
        // anyway, because a handle can outlive a chmod.
        if (!dos_svc_allow(x, fp, R_OK, "INT21/3Dh open")) { svc_err(x, c, 5); break; }
        int si = sft_alloc(x);
        int h  = jft_alloc(x);
        if (si < 0 || h < 0) { svc_err(x, c, 4); break; }
        dos_svc_sft_t *f = &x->sft[si];
        memset(f, 0, sizeof(*f));
        if (fat_open(&g_fat_fs, fp, &f->fat) != 0) {
            memset(f, 0, sizeof(*f));
            svc_err(x, c, 2);   // file not found
            kprintf("[int21:%s] 3Dh open FAIL '%s'\n", x->tag, fp);
            break;
        }
        // A DIRECTORY IS NOT A FILE. fat_open() opens both, so 3Dh used
        // to hand back a perfectly good handle onto a directory whose size is
        // 0. Every read on it then returned 0 bytes, which a caller cannot tell
        // apart from an empty file, so "the path I built is wrong" arrived
        // looking like "the file I wanted is empty" and the program gave up
        // without saying anything. Real DOS answers 05h (access denied), which
        // is a fact the caller can act on.
        //
        // MEASURED, not theoretical: a 32-bit extended program that reopens its
        // own image to find the appended payload built a path ending in a
        // separator, opened the DIRECTORY, read 28 bytes, got 0, and exited
        // silently with code 0. Nothing in the log said anything was wrong.
        if (f->fat.is_dir) {
            fat_close(&f->fat);
            memset(f, 0, sizeof(*f));
            svc_err(x, c, 5);   // access denied
            kprintf("[int21:%s] 3Dh open REFUSED (is a directory) '%s'\n", x->tag, fp);
            break;
        }
        f->refs      = 1;
        f->streaming = 1;                        // stays streaming until a write
        f->size      = f->fat.file_size;
        f->pos       = 0;
        f->wr_ok     = ((AL(c) & 0x03) != 0);    // 1 = write, 2 = read/write
        strncpy(f->path, fp, sizeof(f->path) - 1);
        x->jft[h] = (signed char)si;
        c->ax = (uint16_t)h;
        kprintf("[int21:%s] 3Dh open '%s' -> h%d size=%u%s\n", x->tag, fp, h,
                f->size, f->wr_ok ? " rw" : "");
        break;
    }

    case 0x3E: { // close handle BX
        uint16_t h = c->bx;
        if (h < DOS_SVC_FIRST_FILE_FH) break;    // a standard handle: no-op
        if (h >= DOS_SVC_MAX_FH || x->jft[h] < 0) { svc_err(x, c, 6); break; }
        // #736 Stage 2: close is where a buffered file reaches the medium, so
        // it is also where a FAILED write must be reported. Returning success
        // from close after the bytes did not land is the same lie as the write
        // stub, one call later.
        if (handle_close(x, h) != 0) { svc_err(x, c, 5); break; }
        CLR_CF(c);
        break;
    }

    case 0x3F: { // read CX bytes from handle BX to DS:DX -> AX=bytes read
        uint16_t h = c->bx, len = c->cx;
        if (h >= DOS_SVC_MAX_FH || (h >= DOS_SVC_FIRST_FILE_FH && x->jft[h] < 0)) {
            svc_err(x, c, 6); break;
        }
        if (h == 0) {   // stdin from the console keyboard
            uint16_t got = 0, k;
            while (got < len && con_get(x, &k)) {
                uint8_t ch = (uint8_t)(k & 0xFF);
                if (!ch) continue;            // extended key: no ASCII to hand back
                g_wr8(x, c->ds, (uint16_t)(c->dx + got), ch);
                got++;
                if (ch == '\r') break;
            }
            c->ax = got; break;
        }
        if (h < DOS_SVC_FIRST_FILE_FH) { c->ax = 0; break; }   // aux/prn: nothing to read
        dos_svc_sft_t *fh = sft_of(x, h);
        if (!fh) { svc_err(x, c, 6); break; }
        // #234h: a character device has no fat_file_t and no buffer, so it must
        // be answered before anything below touches either. CON reads the
        // keyboard through the SAME con_get() the predefined stdin handle uses,
        // so a program that opens "CON" by name and one that reads handle 0 get
        // the identical stream rather than two different consoles. NUL and the
        // EMS driver are at end-of-file by definition.
        //
        // This arm also closes a latent fault rather than only adding a
        // feature: before it, a read on the EMS device fell through to the file
        // path with a zeroed fat_file_t and an empty path, and what that did
        // was never anybody's intention.
        if (fh->chardev) {
            uint16_t got = 0, k;
            if (fh->chardev == DOS_CHARDEV_CON) {
                while (got < len && con_get(x, &k)) {
                    uint8_t ch = (uint8_t)(k & 0xFF);
                    if (!ch) continue;        // extended key: no ASCII to hand back
                    g_wr8(x, c->ds, (uint16_t)(c->dx + got), ch);
                    got++;
                    if (ch == '\r') break;
                }
            }
            c->ax = got;
            CLR_CF(c);
            break;
        }
        // #708: re-check on every read. Gating only at open is the POSIX rule,
        // but it lets a guest hold a handle across a chmod that revoked it.
        if (fh->path[0] && !dos_svc_allow(x, fh->path, R_OK, "INT21/3Fh read")) {
            c->ax = 5; SET_CF(c); break;
        }
        uint16_t total = 0;
        if (fh->buf) {                      // buffered image
            uint32_t n = len;
            if (fh->pos > fh->size) fh->pos = fh->size;
            if (n > fh->size - fh->pos) n = fh->size - fh->pos;
            for (uint32_t i = 0; i < n; i++)
                g_wr8(x, c->ds, (uint16_t)(c->dx + i), fh->buf[fh->pos + i]);
            fh->pos += n;
            total = (uint16_t)n;
        } else if (fh->streaming) {
            if (fh->fat.position != fh->pos) fat_seek(&fh->fat, fh->pos);
            while (total < len) {
                uint32_t remain = (uint32_t)(len - total);
                uint16_t want = (uint16_t)(remain > sizeof(x->io_buf) ? sizeof(x->io_buf) : remain);
                int r = fat_read(&fh->fat, x->io_buf, want);
                if (r <= 0) break;
                for (int i = 0; i < r; i++)
                    g_wr8(x, c->ds, (uint16_t)(c->dx + total + i), x->io_buf[i]);
                total += (uint16_t)r;
                fh->pos += (uint32_t)r;
                if (r < (int)want) break;
            }
        }
        c->ax = total;
        kprintf("[int21:%s] 3Fh read h%u want=%u got=%u pos=%u\n", x->tag, h, len, total, fh->pos);
        break;
    }

    case 0x40: { // write CX bytes from DS:DX to handle BX -> AX=written
        uint16_t h = c->bx, len = c->cx;
        if (h == 1 || h == 2) {  // stdout / stderr
            for (uint16_t i = 0; i < len; i++)
                con_putc(x, g_rd8(x, c->ds, (uint16_t)(c->dx + i)));
            c->ax = len; break;
        }
        // #736 Stage 2: REAL. This used to discard the data and report the full
        // count written, which is data loss wearing a success code: the same
        // shape as the paint_save() that did no I/O and returned true.
        dos_svc_sft_t *f = sft_of(x, h);
        if (!f) { svc_err(x, c, 6); break; }
        // #745: a write to a character device consumes the bytes and reports
        // success. It must never reach sft_to_buffer(), which would allocate a
        // file image and hand it to the commit path with an empty path.
        if (f->chardev) {
            // #234h: CON is the screen, so its bytes must actually be drawn.
            // Consuming them silently is the shape #736 removed from the file
            // write path ("data loss wearing a success code"), and it is no
            // more acceptable when the destination is a console. NUL and the
            // EMS device really do discard, which is their specification.
            if (f->chardev == DOS_CHARDEV_CON)
                for (uint16_t i = 0; i < len; i++)
                    con_putc(x, g_rd8(x, c->ds, (uint16_t)(c->dx + i)));
            c->ax = len; CLR_CF(c); break;
        }
        if (!f->wr_ok) { svc_err(x, c, 5); break; }        // opened read-only
        // Every write is gated, not just the open: a handle can outlive a chmod
        // that revoked it, and the commit re-checks again for the same reason.
        if (f->path[0] && !dos_svc_allow(x, f->path, W_OK, "INT21/40h write")) {
            svc_err(x, c, 5); break;
        }
        if (sft_to_buffer(x, f) != 0) { svc_err(x, c, 8); break; }
        if (len == 0) {
            // CX=0 TRUNCATES at the current position. Documented DOS behaviour
            // and the only way a program has to shorten a file in place.
            if (f->pos < f->size) f->size = f->pos;
            f->dirty = 1;
            c->ax = 0; CLR_CF(c);
            break;
        }
        if (sft_reserve(f, f->pos + len) != 0) { svc_err(x, c, 8); break; }
        for (uint16_t i = 0; i < len; i++)
            f->buf[f->pos + i] = g_rd8(x, c->ds, (uint16_t)(c->dx + i));
        f->pos += len;
        if (f->pos > f->size) f->size = f->pos;
        f->dirty = 1;
        x->n_writes++;
        x->n_bytes_written += len;
        c->ax = len;
        CLR_CF(c);
        break;
    }

    case 0x42: { // lseek handle BX, CX:DX offset, AL=whence -> DX:AX position
        uint16_t h = c->bx;
        dos_svc_sft_t *fh = sft_of(x, h);
        if (!fh) { svc_err(x, c, 6); break; }
        int32_t off = (int32_t)(((uint32_t)c->cx << 16) | c->dx);
        uint32_t np;
        if (AL(c) == 0)      np = (uint32_t)off;                  // SEEK_SET
        else if (AL(c) == 1) np = fh->pos + (uint32_t)off;        // SEEK_CUR
        else                 np = fh->size + (uint32_t)off;       // SEEK_END
        fh->pos = np;
        if (fh->streaming) fat_seek(&fh->fat, np);
        c->ax = (uint16_t)(np & 0xFFFF);
        c->dx = (uint16_t)(np >> 16);
        break;
    }

    case 0x43: { // get/set file attributes (DS:DX path). AL=0 get, AL=1 set.
        char dp[DOS_SVC_PATH_MAX], fp[DOS_SVC_PATH_MAX];
        rd_asciiz(x, c->ds, c->dx, dp, sizeof(dp));
        dos_svc_resolve(x, dp, fp, sizeof(fp));
        // #708: get-attributes is an EXISTENCE ORACLE over the whole
        // filesystem, so it is gated exactly like a read. Without this a guest
        // denied every open could still map /CONFIG one probe at a time.
        if (!dos_svc_allow(x, fp, R_OK, "INT21/43h getattr")) { svc_err(x, c, 5); break; }
        if (AL(c) == 1) { CLR_CF(c); break; }   // set: accept silently
        fat_file_t probe;
        if (fat_open(&g_fat_fs, fp, &probe) == 0) {
            fat_close(&probe);
            c->cx = 0x20;   // archive (normal file). Stage 1 preserves this
            CLR_CF(c);      // even for a directory, exactly as before.
        } else {
            svc_err(x, c, 2);   // not found
            // NOT "MISS". AH=43h is implemented and this IS its answer: the
            // file is not there, CF=1 AX=2, which is what DOS returns and what
            // the caller is asking. The word MISS is what the harness's
            // histogram counts, and a game that probes for an optional cache
            // file once per scene would otherwise rank above every function
            // that is genuinely absent. See the commit for the measurement.
            kprintf("[int21:%s] 43h getattr '%s' -> absent (CF=1 AX=2)\n",
                    x->tag, fp);
        }
        break;
    }

    case 0x44: {  // IOCTL
        // #745: the SECOND half of EMS detection. Having opened "EMMXXXX0", the
        // program asks IOCTL whether the handle is a CHARACTER DEVICE, and only
        // a yes here makes it go on to issue INT 67h. Bit 7 of DX is the ISDEV
        // flag that answer lives in.
        dos_svc_sft_t *df = (c->bx >= DOS_SVC_FIRST_FILE_FH) ? sft_of(x, c->bx) : NULL;
        if (df && df->chardev) {
            switch (AL(c)) {
            case 0x00:                       // get device information
                // #234h: bit 7 ISDEV is what every device shares; the low bits
                // say WHICH standard device this is, and a program that has
                // just opened CON checks them to confirm it really has the
                // console. Bit 0 = is-stdin, bit 1 = is-stdout, bit 2 = is-NUL.
                // Reporting a bare 0x0080 for CON says "some device", which is
                // true and useless.
                c->dx = 0x0080;              // ISDEV
                if (df->chardev == DOS_CHARDEV_CON) c->dx |= 0x0003;
                if (df->chardev == DOS_CHARDEV_NUL) c->dx |= 0x0004;
                CLR_CF(c);
                break;
            case 0x06:                       // get input status
            case 0x07:                       // get output status
                AL_SET(c, 0xFF);             // ready
                CLR_CF(c);
                break;
            default:
                CLR_CF(c);                   // accept and ignore
                break;
            }
            break;
        }
        if (AL(c) == 0) { c->dx = (c->bx <= 2) ? 0x0080 : 0x0000; break; }

        // ---- the DRIVE subfunctions (#740) -------------------------------
        // AL=08h "does this drive use removable media" and AL=09h "is this
        // drive remote". BL is the drive (0 = default, 1 = A:). BOTH were
        // falling through this switch with CF already cleared and the guest's
        // own registers untouched, i.e. reported as a success whose result was
        // the caller's stale DX. Discworld II's entire CD search is three
        // AX=4409h calls and that is what it was reading. See
        // rustkern/drvmap.rs for the answers and why bit 12 is the truth here.
        //
        // The class comes from THE drive map (dos_drive_known + the letter
        // class), never from a second opinion in this file.
        if (AL(c) == 0x08 || AL(c) == 0x09) {
            char drv = (c->bx & 0xFF) ? (char)('A' + (c->bx & 0xFF) - 1) : x->cur_drive;
            if (!dos_drive_known(drv)) { svc_err(x, c, 0x0F); break; }  // invalid drive
            int cls = diskimg_letter_class((int)(up1(drv) - 'A'));
            int v = (AL(c) == 0x08) ? drvmap_ioctl_removable_rs((uint32_t)cls)
                                    : drvmap_ioctl_attrword_rs((uint32_t)cls);
            if (v < 0) { svc_err(x, c, 0x0F); break; }
            if (AL(c) == 0x08) c->ax = (uint16_t)v;   // 0 = removable, 1 = fixed
            else               c->dx = (uint16_t)v;   // device attribute word
            CLR_CF(c);
            kprintf("[int21:%s] 44%02xh drive %c: -> %s=%04x\n",
                    x->tag, AL(c), drv, AL(c) == 0x08 ? "AX" : "DX", (unsigned)v);
            break;
        }

        // ---- every other IOCTL subfunction --------------------------------
        // A logged MISS WITH THE CORRECT REGISTER EFFECT, not a silent success.
        // AH=44h is inside MS-DOS's function table, so a subfunction the system
        // cannot service is CF=1, AX=0001 "invalid function"; that is what DOS
        // itself answers, and it is what the default arm of this dispatcher
        // already does for a whole function in the same range.
        note_miss(x, ah);
        kprintf("[int21:%s] AH=44 AL=%02x (unimplemented IOCTL subfunction) "
                "bx=%04x cx=%04x dx=%04x\n", x->tag, AL(c), c->bx, c->cx, c->dx);
        svc_err(x, c, 0x01);
        break;
    }

    case 0x47: { // get current directory (DL=drive, 0=default) -> DS:SI ASCIIZ
        char drv = (c->dx & 0xFF) ? (char)('A' + (c->dx & 0xFF) - 1) : x->cur_drive;
        uint16_t off = c->si;
        const char *d = ctx_cwd_get(x, drv);
        int i = 0;
        for (; d[i]; i++)
            g_wr8(x, c->ds, (uint16_t)(off + i), (uint8_t)(d[i] == '/' ? '\\' : d[i]));
        g_wr8(x, c->ds, (uint16_t)(off + i), 0);
        c->ax = 0x0100;
        CLR_CF(c);
        break;
    }

    case 0x4E: { // find first matching file (DS:DX spec, CX attr mask) -> DTA
        char dp[DOS_SVC_PATH_MAX], fp[DOS_SVC_PATH_MAX];
        rd_asciiz(x, c->ds, c->dx, dp, sizeof(dp));
        dos_svc_resolve(x, dp, fp, sizeof(fp));
        char dirpath[DOS_SVC_PATH_MAX];
        dos_find_split(fp, dirpath, (int)sizeof(dirpath),
                       x->find_pat, (int)sizeof(x->find_pat));
        x->find_pat[sizeof(x->find_pat) - 1] = '\0';
        // The attribute mask. Previously never read, so no search could
        // ask for anything but plain files.
        x->find_attr     = c->cx;
        x->find_vol_done = 0;
        // Which drive's ROOT is this, if any? Computed from the resolved
        // directory rather than from the raw "E:" prefix, so a search that got
        // here through a per-drive CWD, a relative path or the default drive
        // lands on the same answer. native_fallback() below cannot disturb it:
        // it only rewrites paths strictly INSIDE a drive directory.
        x->find_drive = dos_native_root_drive(dirpath);
        // dos_svc_resolve could not rescue this one: it was handed a WILDCARD,
        // which exists nowhere, so the fallback runs on the directory part.
        native_fallback(x, dirpath, (int)sizeof(dirpath));
        // #708: enumerating a directory needs read AND search on it, the same
        // pair sys_open()'s directory path demands.
        if (!dos_svc_allow(x, dirpath, R_OK | X_OK, "INT21/4Eh findfirst")) {
            svc_err(x, c, 5); break;
        }
        if (x->find_active) { fat_close(&x->find_dir); x->find_active = 0; }
        // #rawrite: is this an overlaid directory? Computed from the RESOLVED
        // directory (never from the raw spec), and armed only when the overlay
        // directory really exists, so a guest with an overlay configured but
        // nothing written yet takes exactly the single-phase path it always did.
        x->find_phase  = 0;
        x->find_ovl[0] = '\0';
        {
            char ovl[DOS_SVC_PATH_MAX];
            if (dos_svc_overlay_dir(x, dirpath, ovl, (int)sizeof(ovl)) &&
                path_exists(x, ovl)) {
                strncpy(x->find_ovl, ovl, sizeof(x->find_ovl) - 1);
                x->find_ovl[sizeof(x->find_ovl) - 1] = '\0';
            }
        }
        // A pattern with no wildcard that misses is ENOENT (2); a wildcard that
        // matches nothing is "no more files" (0x12). The DOS3Call implementation
        // folded in here made that distinction and the DOS one did not, and it
        // is load-bearing: Word 6's MS-C runtime classifies the 59h code to
        // decide whether a candidate temp name is free, so collapsing both to
        // 0x12 meant no name was ever considered free.
        int has_wild = 0;
        for (int i = 0; x->find_pat[i]; i++)
            if (x->find_pat[i] == '*' || x->find_pat[i] == '?') has_wild = 1;
        if (fat_open(&g_fat_fs, dirpath, &x->find_dir) != 0 &&
            !(x->find_ovl[0] &&
              fat_open(&g_fat_fs, x->find_ovl, &x->find_dir) == 0 &&
              (x->find_phase = 1) == 1)) {
            // #rawrite: the base directory can be absent while the per-user
            // overlay is not (a directory the guest created itself). Walking
            // the overlay alone is the right answer; reporting "no such
            // directory" would hide the guest's own files from it.
            svc_err(x, c, has_wild ? 18 : 2);
            // Same reasoning as AH=43h above: the directory does not exist and
            // saying so with the documented code is the FUNCTION WORKING.
            kprintf("[int21:%s] 4Eh findfirst dir '%s' -> absent (CF=1 AX=%d)\n",
                    x->tag, dirpath, has_wild ? 18 : 2);
            break;
        }
        x->find_active = 1;
        strncpy(x->find_dirpath, dirpath, sizeof(x->find_dirpath) - 1);
        x->find_dirpath[sizeof(x->find_dirpath) - 1] = '\0';
        if (find_step(x) == 0) { CLR_CF(c); c->ax = 0; x->last_err = 0; }
        else { svc_err(x, c, has_wild ? 18 : 2); }
        kprintf("[int21:%s] 4Eh findfirst dir='%s' pat='%s' attr=%04x drive=%c -> %s\n",
                x->tag, dirpath, x->find_pat, (unsigned)x->find_attr,
                x->find_drive ? x->find_drive : '-',
                (c->flags & 0x0001) ? "none" : "hit");
        break;
    }

    case 0x4F:  // find next -> DTA or CF
        if (find_step(x) == 0) { CLR_CF(c); c->ax = 0; x->last_err = 0; }
        else { svc_err(x, c, 18); }
        break;

    case 0x6C: { // extended open/create (DOS 4.0+): AX=6C00h, DS:SI = path
        // WHY THIS EXISTS AND 3Dh/3Ch ARE NOT ENOUGH (#221). 6C00h is the only
        // INT 21h call that can express "create this file, but FAIL if it is
        // already there" - O_CREAT|O_EXCL - in ONE atomic-looking operation.
        // 3Dh followed by 3Ch is not the same function: it is a test and a
        // create with a window between them. Every runtime that offers O_EXCL
        // on DOS 4.0+ uses this, and djgpp does, which is why NetHack lands
        // here: getlock() takes its NHPERM lock with O_CREAT|O_EXCL, and with
        // 6Ch missing it printed "Waiting for access to c:/dos/nethack\\NHPERM
        // ... I give up. Sorry." and exited before a game could start.
        //
        //   BX = open mode (access in bits 0-2, exactly 3Dh's AL)
        //   CX = attributes to give a file it CREATES
        //   DX = action: low nibble  if the file EXISTS (0 fail, 1 open, 2 truncate)
        //                high nibble if it does NOT    (0 fail, 1 create)
        //   -> CF=0, AX = handle, CX = 1 opened / 2 created / 3 truncated
        //
        // The work is handed to the EXISTING 3Dh and 3Ch, the same way AH=0Ch
        // hands its sub-function back to this dispatcher, so the handle table,
        // the SFT, the permission gate and the native-path fallback have ONE
        // implementation rather than a third copy that drifts.
        if (AL(c) != 0x00) { svc_err(x, c, 0x01); break; }  // only 6C00h is defined
        uint16_t mode   = c->bx;
        uint16_t attr   = c->cx;
        uint16_t action = c->dx;
        uint8_t  if_ex  = (uint8_t)(action & 0x0F);
        uint8_t  if_new = (uint8_t)((action >> 4) & 0x0F);

        char dp[DOS_SVC_PATH_MAX], fp[DOS_SVC_PATH_MAX];
        rd_asciiz(x, c->ds, c->si, dp, sizeof(dp));
        dos_svc_resolve(x, dp, fp, sizeof(fp));
        // path_exists(), not fat_exists(): it is the ROUTED probe that reaches
        // ext2 on an ext2-root system (see its own comment). Getting this wrong
        // would report every existing file as absent and turn an O_EXCL create
        // that must fail into one that succeeds.
        int exists = path_exists(x, fp);

        uint8_t  sub;          // the classic function that does the work
        uint16_t taken;        // what to report in CX on success
        if (exists) {
            if (if_ex == 0) { svc_err(x, c, 0x50); break; }   // 50h: file exists
            sub   = (if_ex == 2) ? 0x3C : 0x3D;               // truncate vs open
            taken = (if_ex == 2) ? 3 : 1;
        } else {
            if (if_new == 0) { svc_err(x, c, 0x02); break; }  // 02h: file not found
            sub   = 0x3C;
            taken = 2;
        }

        uint16_t save_dx = c->dx;
        c->dx = c->si;                                   // 3Dh/3Ch take DS:DX
        if (sub == 0x3C) c->cx = attr;                   // 3Ch reads CX as attributes
        AH_SET(c, sub);
        AL_SET(c, (uint8_t)(mode & 0x07));               // 3Dh reads AL as access mode
        dos_svc_int21(x, c);
        c->dx = save_dx;
        if (!(c->flags & 0x0001)) c->cx = taken;         // success: CX = action taken
        kprintf("[int21:%s] 6C00h ext-open '%s' mode=%04x action=%04x exists=%d -> %s\n",
                x->tag, fp, mode, action, exists,
                (c->flags & 0x0001) ? "FAIL" : (taken == 2 ? "created" :
                                                taken == 3 ? "truncated" : "opened"));
        break;
    }

    // ---- the DOS 7.0 long-filename API -----------------------------------
    // WE DO NOT IMPLEMENT IT, AND THIS IS HOW YOU SAY SO (#221).
    //
    // The whole AH=71h family has its OWN documented refusal: CF=1 with
    // AX=7100h, "long filename functions not supported". It is a special case
    // precisely BECAUSE a caller has to distinguish "this API does not exist
    // here" from "this particular call failed", and the only difference is the
    // value in AH.
    //
    // Falling through to the generic default arm returned CF=1 AX=0001
    // (invalid function), which is the right answer for an unimplemented
    // function INSIDE the DOS table and the wrong answer for this one, because
    // AH is 00h and not 71h. MEASURED, djgpp (NetHack, and therefore every
    // DJGPP binary): libc's _get_volume_info() reads the refusal as
    //     CF set and AH == 0x71  ->  return 0        (no LFN, definitive)
    //     CF set and AH != 0x71  ->  return 0x80000000 (could not determine)
    // and _use_lfn() turns that second answer into LFN = YES unless the LFN
    // environment variable says "n". So answering AX=0001 did not decline the
    // API; it told djgpp to GO ON USING IT. NetHack then issued 716Ch (extended
    // open), 714Eh/71A1h (find first/close), 7143h (get/set attributes),
    // 7141h (delete) and 7160h (truename) for every file operation, all of
    // which missed, and it could not create its NHPERM lock file: "Waiting for
    // access to c:/dos/nethack\NHPERM ... I give up. Sorry." One run, 133 MISS
    // lines, all of them this.
    //
    // With the documented refusal djgpp uses the SHORT-name API instead - 3Ch,
    // 3Dh, 43h, 41h, 4Eh/4Fh - which this core implements. That is why the fix
    // is one return value and not the LFN family: the family is optional and
    // saying so correctly is the whole requirement.
    //
    // This is not a stub standing in for future work. If the LFN API is ever
    // implemented, this case is what it replaces, and until then a guest that
    // is TOLD no behaves better than one that is left to guess.
    case 0x71: {
        uint16_t sub = c->ax;             // read BEFORE svc_err overwrites AX
        svc_err(x, c, 0x7100);
        if (!x->warned_lfn) {
            x->warned_lfn = 1;
            kprintf("[int21:%s] AH=71h (DOS 7 long filenames) DECLINED with the "
                    "documented CF=1 AX=7100h; the guest will use the short-name "
                    "API. First call was AX=%04x.\n", x->tag, sub);
        }
        break;
    }

    default:
        // Not a service this core implements. Offer it to the caller's own
        // functions (the DOS task's MCB memory model lives there) before
        // reporting a miss.
        if (x->extend && x->extend(x, c, ah)) break;
        note_miss(x, ah);
        kprintf("[int21:%s] AH=%02x (unimplemented) ax=%04x bx=%04x cx=%04x dx=%04x\n",
                x->tag, ah, c->ax, c->bx, c->cx, c->dx);
        // #736 Stage 2: AND SAY SO, BUT ONLY WHERE DOS WOULD.
        //
        // Every implementation this core replaced cleared CF on entry and left
        // it clear here, so an unimplemented function was reported to the guest
        // as HAVING SUCCEEDED with whatever was in AX read as its result. That
        // is the write stub's defect wearing a different hat.
        //
        // BUT "always answer invalid function" IS ALSO WRONG, AND IT WAS
        // MEASURED WRONG. MS-DOS's INT 21h dispatcher has a function table that
        // ends at AH=6Ch. A call inside that range that the system cannot
        // service returns CF=1 AX=1, invalid function; that is what DOS with no
        // SHARE loaded answers for AH=5Ch. A call ABOVE the table is a network
        // or OEM extension, and a DOS with no redirector installed simply
        // RETURNS, leaving the caller's registers alone.
        //
        // Failing that second class broke Word 6: AH=DCh (a network station
        // probe) started returning CF=1, Word fell into a retry loop issuing
        // AH=1Ch nineteen times, and then EXITED during startup with 4Ch. The
        // window opened with no menu bar and no document, which is what a
        // faithful-looking blanket rule costs when it is faithful to the wrong
        // half of the ABI.
        if (ah <= 0x6C) {
            svc_err(x, c, 1);            // in-range and unsupported: invalid function
        } else {
            CLR_CF(c);                   // out-of-range extension: DOS just returns
        }
        break;
    }
}
