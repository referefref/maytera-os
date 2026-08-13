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

// #679: a file a guest creates is owned by the guest's LAUNCHER, the same
// way a new inode gets its creator's uid/gid. Without this a non-root guest
// creates a file it then cannot write, because a path with no PERMS.DB entry
// falls to the root-owned 0755 default.
void perms_on_create(const char *path, uint32_t uid, uint32_t gid, int is_dir);
void perms_remove(const char *path);
#include "../exec/x86_16.h"

extern fat_fs_t g_fat_fs;

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
    if (!path_exists(x, alt)) return 0;
    int i = 0;
    for (; alt[i] && i < max - 1; i++) p[i] = alt[i];
    p[i] = '\0';
    return 1;
}

void dos_svc_resolve(dos_svc_ctx_t *x, const char *in, char *out, int outsz) {
    dos_resolve_path_ex(in, x->appdir, x->cur_drive, cwd_thunk, x, out, outsz);
    native_fallback(x, out, outsz);
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
    write_find_result(x, lbl, 0, 0x08);
    kprintf("[int21:%s] 4Eh volume label on %c: -> '%s'\n", x->tag, x->find_drive, lbl);
    return 1;
}

// Advance the active find iteration. 0 + DTA filled on a match, -1 at end.
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
    fat_dir_entry_t e;
    // #490: fat_readdir reconstructs VFAT long names of up to 255 chars into
    // this buffer, so it MUST be 256 bytes (the fat_readdir caller contract; a
    // smaller array is now a COMPILE error, see fat.h).
    char namebuf[256];
    if (x->find_dirpath[0] &&
        !dos_svc_allow(x, x->find_dirpath, R_OK | X_OK, "INT21/4Fh findnext")) return -1;
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
        if (dos_svc_wild_match(x->find_pat, dot)) {
            write_find_result(x, dot, e.file_size, e.attr);
            return 0;
        }
    }
    x->find_active = 0;
    fat_close(&x->find_dir);
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
    // Non-blocking by design. Blocking here would block the interpreter
    // thread, which is the banned #426 pattern AND would stop us pumping the
    // very input being waited for. A guest loops, exactly as it does around
    // the real BIOS. A context with no keyboard (con.getkey == NULL) answers
    // "no key" forever, which is correct and is what it did before.
    case 0x01:   // read char WITH echo      -> AL = char
    case 0x07:   // read char, no echo, no ^C
    case 0x08: { // read char, no echo, checks ^C
        uint16_t k;
        if (con_get(x, &k)) {
            uint8_t ascii = (uint8_t)(k & 0xFF);
            AL_SET(c, ascii ? ascii : (uint8_t)(k >> 8));
            if (ah == 0x01 && ascii) con_putc(x, ascii);
        } else {
            AL_SET(c, 0);
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

    case 0x2A:  // get system date -> CX=year DH=month DL=day AL=dow
        c->cx = 1992; c->dx = (11 << 8) | 19; AL_SET(c, 4);
        break;

    case 0x2C:  // get system time -> CH:CL hour:min DH:DL sec:hundredths
        c->cx = 0; c->dx = 0;
        break;

    case 0x30:  // get DOS version
        c->ax = x->dos_version;
        c->bx = 0xFF00;
        c->cx = 0;
        break;

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
        if (!dos_svc_allow(x, fp, R_OK | X_OK, "INT21/3Bh chdir")) { svc_err(x, c, 5); break; }
        fat_file_t probe;
        if (fat_open(&g_fat_fs, fp, &probe) != 0) { svc_err(x, c, 3); break; }
        int isdir = fat_is_dir(&probe);
        fat_close(&probe);
        if (!isdir) { svc_err(x, c, 3); break; }
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
        //  2. A MOUNTED IMAGE -> the medium's real total size, ZERO free. A
        //     mounted disc is read-only and there is no write-back, so "full"
        //     is the truth and not a placeholder. This is also what MSCDEX
        //     reports for a CD.
        //  3. ANYTHING ELSE (folder-backed A:/B:, the C: hard disk) -> the old
        //     generous fixed answer, unchanged. It is STILL A FICTION and is
        //     still listed as one: a real number needs a free-space query
        //     spanning FAT and ext2, which is Stage 2 work and not this.
        char drv = DL(c) ? (char)('A' + DL(c) - 1) : x->cur_drive;
        if (!dos_drive_known(drv)) { c->ax = 0xFFFF; CLR_CF(c); break; }
        uint64_t media = diskimg_media_size(drv);
        if (media) {
            // 2048-byte sectors, one sector per cluster: an ISO's own geometry,
            // and it keeps the cluster count inside 16 bits for a 128 GB medium.
            uint64_t clusters = (media + 2047u) / 2048u;
            if (clusters > 0xFFFFu) clusters = 0xFFFFu;
            c->ax = 1; c->cx = 2048; c->bx = 0; c->dx = (uint16_t)clusters;
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
        if (rc == 0)            { CLR_CF(c); c->ax = 0; x->last_err = 0; }
        else if (rc == -17)     { svc_err(x, c, 5); }   // already exists
        else                    { svc_err(x, c, 3); }   // path not found
        kprintf("[int21:%s] 39h mkdir '%s' -> rc=%d\n", x->tag, fp, rc);
        break;
    }

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
            kprintf("[int21:%s] 43h getattr MISS '%s'\n", x->tag, fp);
        }
        break;
    }

    case 0x44:  // IOCTL: report char device for stdin/stdout (AL=0 get info)
        if (AL(c) == 0) { c->dx = (c->bx <= 2) ? 0x0080 : 0x0000; }
        break;

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
        char dirpath[DOS_SVC_PATH_MAX]; int slash = -1;
        for (int i = 0; fp[i]; i++) if (fp[i] == '/') slash = i;
        if (slash < 0) {
            dirpath[0] = '/'; dirpath[1] = '\0';
            strncpy(x->find_pat, fp, sizeof(x->find_pat) - 1);
        } else {
            int n = slash > 0 ? slash : 1;
            for (int i = 0; i < n; i++) dirpath[i] = fp[i];
            dirpath[n] = '\0';
            strncpy(x->find_pat, fp + slash + 1, sizeof(x->find_pat) - 1);
        }
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
        // A pattern with no wildcard that misses is ENOENT (2); a wildcard that
        // matches nothing is "no more files" (0x12). The DOS3Call implementation
        // folded in here made that distinction and the DOS one did not, and it
        // is load-bearing: Word 6's MS-C runtime classifies the 59h code to
        // decide whether a candidate temp name is free, so collapsing both to
        // 0x12 meant no name was ever considered free.
        int has_wild = 0;
        for (int i = 0; x->find_pat[i]; i++)
            if (x->find_pat[i] == '*' || x->find_pat[i] == '?') has_wild = 1;
        if (fat_open(&g_fat_fs, dirpath, &x->find_dir) != 0) {
            svc_err(x, c, has_wild ? 18 : 2);
            kprintf("[int21:%s] 4Eh findfirst dir MISS '%s'\n", x->tag, dirpath);
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
