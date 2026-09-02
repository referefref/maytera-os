// kshim.c - the KERNEL-UNIVERSE half of the dosring3 shim (#DOSRING3).
//
// Compiled against the KERNEL's headers (the copies mkgen.sh assembles under
// gen/), so it sees exactly the declarations the DOS sources see and the
// compiler checks every definition here against them. It reaches Ring-3
// facilities only through kbridge.h, whose signatures use primitive types that
// are ABI-identical in both header worlds. See kbridge.h for why the wall
// exists.
//
// THE ONE RULE THIS FILE FOLLOWS. Nothing here returns a plausible value it
// cannot stand behind. Every symbol is in exactly one of three categories, and
// each is labelled at its definition:
//
//   REAL      a genuine Ring-3 implementation of the same semantics.
//   SCOPED    the facility does not apply to a Ring-3 DOS host, and the value
//             returned is the CORRECT answer for that scope, not a placeholder.
//             (Example: the Win16 protected-mode selector is 0 because this is
//             a DOS host and there is no Win16 context - the same answer the
//             kernel gives before any Win16 app has started.)
//   ABSENT    the facility genuinely does not exist in Ring 3. Returns the SAME
//             failure the kernel returns when the hardware or subsystem is
//             missing, and RECORDS ITSELF in a census printed at exit, so the
//             report can name precisely what was never exercised rather than
//             leaving it to be discovered later.
//
// An ABSENT symbol is never silently zero: dosring3_unimpl_report() prints the
// name and hit count of every one that was reached.
#include "types.h"
#include "serial.h"
#include "string.h"
#include "mm/heap.h"
#include "fs/fat.h"
#include "fs/perms.h"
#include "fs/guestfs.h"
#include "fs/bootlog.h"
#include "fs/ext2.h"
#include "fs/blockdev.h"
#include "sync/spinlock.h"
#include "sync/waitq.h"
#include "cpu/mono.h"
#include "cpu/wallclock.h"
#include "drivers/keyboard.h"
#include "drivers/audio.h"
#include "drivers/audio_pcm.h"
#include "drivers/usb_audio.h"
#include "drivers/usb_msc.h"
#include "proc/users.h"
#include "proc/process.h"   // PROC_AS_CALLER / PROC_AS_SESSION
#include "dos/dosfmq.h"   // (#fmbridge) the ring-neutral FM-queue seam
#include "kbridge.h"

// ===========================================================================
// The ABSENT census
// ===========================================================================
#define UNIMPL_MAX 64
static struct { const char *name; uint64_t hits; } g_unimpl[UNIMPL_MAX];
static int g_unimpl_n = 0;

static void unimpl_note(const char *name) {
    for (int i = 0; i < g_unimpl_n; i++) {
        if (g_unimpl[i].name == name) { g_unimpl[i].hits++; return; }
    }
    if (g_unimpl_n < UNIMPL_MAX) {
        g_unimpl[g_unimpl_n].name = name;
        g_unimpl[g_unimpl_n].hits = 1;
        g_unimpl_n++;
    }
}
#define ABSENT(n) unimpl_note(n)

void dosring3_unimpl_report(void) {
    if (g_unimpl_n == 0) {
        kprintf("[DOSRING3] ABSENT census: nothing unimplemented was reached.\n");
        return;
    }
    kprintf("[DOSRING3] ABSENT census: %d facility(ies) reached that do not "
            "exist in Ring 3 (each returned the same failure the kernel gives "
            "when the subsystem is missing):\n", g_unimpl_n);
    for (int i = 0; i < g_unimpl_n; i++) {
        kprintf("[DOSRING3]   %-32s %llu call(s)\n",
                g_unimpl[i].name, (unsigned long long)g_unimpl[i].hits);
    }
}

// ===========================================================================
// Diagnostics - REAL
// ===========================================================================
void kputc(char c) { kb_log(&c, 1); }
void kputs(const char *s) { if (s) kb_log(s, strlen(s)); }

static void kvprint(const char *fmt, __builtin_va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
    kb_log(buf, (unsigned long)n);
}

void kprintf(const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    kvprint(fmt, ap); __builtin_va_end(ap);
}
// In Ring 3 there is no console lock to bypass and no panic ordering to keep,
// so the "nolock" form is the same sink.
void kprintf_nolock(const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    kvprint(fmt, ap); __builtin_va_end(ap);
}
// The kernel mirrors guest console output (INT 21h AH=02h/06h/09h/40h, INT 10h
// AH=0Eh) to COM1 as a trace of what the guest printed. Same MEANING here, over
// this process's diagnostic channel; only the transport differs.
void serial_write(uint16_t port, char c) { (void)port; kb_log(&c, 1); }

// bootlog/audiolog are append-to-a-file diagnostics in the kernel. REAL: the
// same thing, through this process's own writes.
static void logfile_append(const char *path, const char *fmt, __builtin_va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    long fd = kb_open(path, KB_O_RDWR);
    if (fd < 0) return;
    (void)kb_seek(fd, 0, 2);           // append
    (void)kb_write(fd, buf, (unsigned long)n);
    kb_close(fd);
}
int bootlog_write(const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    logfile_append("/DOSR3LOG.TXT", fmt, ap); __builtin_va_end(ap);
    return 0;
}
void audiolog_write(const char *fmt, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    logfile_append("/DOSR3AUD.TXT", fmt, ap); __builtin_va_end(ap);
}

void dosring3_rust_panic(const char *msg) {
    kprintf("[DOSRING3] RUST PANIC: %s\n", msg ? msg : "(null)");
    dosring3_unimpl_report();
    kb_abort("dosring3: rust panic");
}

// ===========================================================================
// Memory - REAL. The DOS arenas are kmalloc'd in the kernel too, so this is a
// like-for-like substitution. Both the 1 MiB conventional arena and the 32 MiB
// DOS/4GW arena fit the 512 MB userland heap ceiling comfortably.
// ===========================================================================
void *kmalloc(size_t size) { return kb_malloc((unsigned long)size); }
void  kfree(void *ptr)     { kb_free(ptr); }

// memcpy_fast / memmove_fast / memset_fast ARE DELIBERATELY NOT DEFINED HERE.
//
// THIS BLOCK IS A GRAVESTONE. Defining them cost a full VM run and produced a
// hang that looked like a DOS-interpreter fault and was not.
//
// The reasoning that produced the bug was: "these are the KERNEL's SSE
// accelerated variants; Ring 3 has hardware SSE2 anyway, so forward them to
// plain memcpy/memset". That inverted the real relationship. In USERLAND these
// three are not a kernel nicety needing a stand-in, they are THE PRIMITIVES:
// userland/libc/memcpy_fast.asm defines all three in hand-written assembly, and
// libc's memset() is a FORWARDER to memset_fast().
//
// So the shim's memset_fast() { return memset(...); } closed a loop. Because a
// plain .o beats an archive member, the shim's definition won, and the linker
// produced exactly this:
//
//     memset_fast:  jmp    memset
//     memset:       movabs $memset_fast,%rax ; jmp *%rax
//
// Two instructions, infinite mutual tail-recursion, constant RSP. MEASURED by
// sampling the spinning process: five consecutive register dumps IDENTICAL
// (RDI=0x8002010fc0, RDX=0x49aa8, RSP unchanged) with RIP alternating between
// the two entry points. Every DOS memset - including the very first, clearing
// guest memory - never returned, which is why the host burned 98% of a core and
// never reached win16_host_create().
//
// THE RULE THIS BROKE is the one in CLAUDE.md: reuse the canonical primitive,
// never fork it. Before shimming a symbol, check whether the TARGET environment
// already defines it - the shim list is "what the DOS sources import", not
// "what this process must supply", and those two sets are not the same.
//
// libc provides all three. Nothing goes here.

// ===========================================================================
// FILESYSTEM - REAL, and the seam that deserves the most care.
//
// MEASURED: 77 call sites, ALL through the FAT API against g_fat_fs. There is
// no second seam: ext2_*, vfs_* and blk_* have ZERO call sites in the DOS
// sources. That is because on the two-partition image the FAT API is ALREADY
// the routing layer - fat_path_on_ext2() dispatches by path - so one shim
// covers both filesystems.
//
// The Ring-3 target is the kernel's own open(), which performs the SAME routing
// AND additionally applies this process's real credentials to every access.
// See the guestfs note below for why that is a stronger boundary, not a weaker
// one.
//
// fat_file_t is OPAQUE to the DOS sources: verified by grep, not one of them
// reads a member. So its storage is reused here to carry a Ring-3 handle.
// ===========================================================================
fat_fs_t g_fat_fs;      // SCOPED: the DOS sources only ever pass its ADDRESS to
                        // the fat_* calls below, which ignore it because the
                        // routing they used it for now happens in the kernel.

// THE HANDLE, AND WHY IT IS NOT AN OVERLAY ANY MORE.
//
// The first working run exposed this: an earlier version cast fat_file_t to a
// private struct, on the strength of a grep that concluded no DOS source reads
// a member of it. THAT GREP WAS WRONG - it matched `f->size`-shaped patterns
// and the real accesses are `f->fat.<member>`, one indirection further out.
// A `_Static_assert` on sizeof gave false confidence: it proved the overlay
// FITS, which is not the same as proving nothing else reads the fields.
//
// MEASURED, by checking every member name individually against the DOS
// sources, the layer touches exactly THREE:
//
//   f->fat.is_dir     int21svc.c:1565  - refuse 3Dh open on a directory
//   f->fat.file_size  int21svc.c:1574  - the size 3Dh reports to the guest
//   fh->fat.position  int21svc.c:1658  - compared with the SFT's own cursor to
//                                        decide whether a seek is needed
//
// So those three are maintained in their REAL fields, with their real meanings.
// The Ring-3 fd cannot live in them, so it goes in a side table and only the
// SLOT INDEX is stored in the struct - in ext2_ino, which is measured to have
// no reader in the DOS sources and is meaningless in Ring 3 anyway. A magic in
// dirent_lba distinguishes a handle this shim opened from a zeroed or foreign
// one, so a stale fat_file_t reads as closed rather than as slot 0.
#define SHIM_FH_MAGIC 0x52335348u   // 'R3SH'
#define SHIM_FH_MAX   64

typedef struct {
    int           used;
    long          fd;        // >=0 for a file
    unsigned long dir;       // non-zero for a directory enumeration
    char          path[192]; // the directory's own path, to size its children
} shim_fh_t;

static shim_fh_t g_fh[SHIM_FH_MAX];

static shim_fh_t *fh_of(fat_file_t *f) {
    if (!f || f->dirent_lba != SHIM_FH_MAGIC) return 0;
    uint32_t slot = f->ext2_ino;
    if (slot == 0 || slot > SHIM_FH_MAX) return 0;
    shim_fh_t *h = &g_fh[slot - 1];
    return h->used ? h : 0;
}

static shim_fh_t *fh_alloc(fat_file_t *f) {
    for (int i = 0; i < SHIM_FH_MAX; i++) {
        if (g_fh[i].used) continue;
        memset(&g_fh[i], 0, sizeof g_fh[i]);
        g_fh[i].used = 1;
        g_fh[i].fd   = -1;
        memset(f, 0, sizeof *f);
        f->dirent_lba = SHIM_FH_MAGIC;
        f->ext2_ino   = (uint32_t)(i + 1);
        return &g_fh[i];
    }
    return 0;
}

static void fh_release(fat_file_t *f) {
    shim_fh_t *h = fh_of(f);
    if (!h) return;
    if (h->dir) kb_closedir(h->dir);
    if (h->fd >= 0) kb_close(h->fd);
    memset(h, 0, sizeof *h);
    memset(f, 0, sizeof *f);
}


// Convert a filesystem name to the 11-byte SPACE-PADDED FAT 8.3 form the DOS
// FindFirst/FindNext path expects. Uppercased, split at the LAST dot.
//
// HONEST LIMITATION, stated because it is a real behaviour difference and not
// a bug to be discovered later: real FAT generates a numeric tail (~1, ~2) to
// keep short names unique when several long names collapse to the same 8.3
// form. This does not. Two files in one directory whose names agree in the
// first 8 characters and the extension will therefore present the SAME short
// name to a guest that enumerates by short name. The long name is still
// delivered separately via name_out, which is what the enumeration itself
// matches on for non-8.3-only guests.
static void shim_to_8dot3(const char *in, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    if (!in || !in[0]) return;
    const char *dot = 0;
    for (const char *p = in; *p; p++) if (*p == '.') dot = p;
    int ni = 0;
    for (const char *p = in; *p && (dot ? p < dot : 1) && ni < 8; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[ni++] = (uint8_t)c;
    }
    if (dot) {
        int ei = 0;
        for (const char *p = dot + 1; *p && ei < 3; p++) {
            char c = *p;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            out[8 + ei++] = (uint8_t)c;
        }
    }
}

static int shim_join(const char *dir, const char *name, char *out, size_t cap) {
    if (!dir || !name || !out || cap < 2) return 0;
    size_t dn = strlen(dir), nn = strlen(name);
    int need_sep = (dn > 0 && dir[dn - 1] != '/');
    if (dn + (need_sep ? 1u : 0u) + nn + 1u > cap) return 0;
    memcpy(out, dir, dn);
    size_t o = dn;
    if (need_sep) out[o++] = '/';
    memcpy(out + o, name, nn);
    out[o + nn] = '\0';
    return 1;
}

int fat_open(fat_fs_t *fs, const char *path, fat_file_t *file) {
    (void)fs;
    if (!path || !file) return -1;
    shim_fh_t *h = fh_alloc(file);
    if (!h) return -1;

    if (kb_isdir(path)) {
        h->dir = kb_opendir(path);
        if (!h->dir) { fh_release(file); return -1; }
        { size_t n = strlen(path);
          if (n >= sizeof h->path) n = sizeof h->path - 1;
          memcpy(h->path, path, n); h->path[n] = '\0'; }
        file->is_dir    = 1;      // REAL field: int21svc.c:1565 refuses 3Dh on it
        file->file_size = 0;
        file->position  = 0;
        file->open      = 1;
        return 0;
    }

    h->fd = kb_open(path, KB_O_READ);
    if (h->fd < 0) { fh_release(file); return -1; }
    long sz = kb_seek(h->fd, 0, 2);
    (void)kb_seek(h->fd, 0, 0);
    file->is_dir    = 0;
    file->file_size = (uint32_t)(sz > 0 ? sz : 0);   // REAL field: int21svc.c:1574
    file->position  = 0;                             // REAL field: int21svc.c:1658
    file->open      = 1;
    return 0;
}

void fat_close(fat_file_t *file) { fh_release(file); }

int fat_read(fat_file_t *file, void *buffer, uint32_t size) {
    shim_fh_t *h = fh_of(file);
    if (!h || h->fd < 0 || !buffer) return -1;
    long n = kb_read(h->fd, buffer, (unsigned long)size);
    if (n < 0) return -1;
    // Keep the REAL cursor in step. int21svc.c:1658 compares fat.position with
    // its own SFT cursor to decide whether a seek is needed; if this is not
    // maintained it seeks on every read, or worse, believes it need not seek
    // when it must.
    file->position += (uint32_t)n;
    return (int)n;
}

int fat_seek(fat_file_t *file, uint32_t position) {
    shim_fh_t *h = fh_of(file);
    if (!h || h->fd < 0) return -1;
    if (kb_seek(h->fd, (long)position, 0) < 0) return -1;
    file->position = position;
    return 0;
}

uint32_t fat_size(fat_file_t *file) {
    return file ? file->file_size : 0;
}

int fat_is_dir(fat_file_t *file) {
    return file ? file->is_dir : 0;
}

int fat_readdir_n(fat_file_t *dir, fat_dir_entry_t *entry, char *name_out, size_t name_cap) {
    shim_fh_t *h = fh_of(dir);
    if (!h || !h->dir) return -1;
    char nm[256];
    int isdir = 0;
    if (!kb_readdir(h->dir, nm, sizeof nm, &isdir)) return -1;
    if (name_out && name_cap) {
        size_t n = strlen(nm);
        if (n >= name_cap) n = name_cap - 1;
        memcpy(name_out, nm, n);
        name_out[n] = '\0';
    }
    if (entry) {
        memset(entry, 0, sizeof *entry);
        // The FindNext path (dos/int21svc.c:780-798) reads name[], attr and
        // file_size, and runs name[] through fat11_to_dotname(), so the 11-byte
        // SPACE-PADDED 8.3 form has to be real rather than left zero: a zeroed
        // name[0] reads as END OF DIRECTORY and would silently truncate every
        // guest's directory listing to nothing.
        shim_to_8dot3(nm, entry->name);
        entry->attr = (uint8_t)(isdir ? 0x10 : 0x20);   // ATTR_DIRECTORY / ARCHIVE
        entry->file_size = 0;
        if (!isdir) {
            char full[512];
            if (shim_join(h->path, nm, full, sizeof full)) {
                long sz = kb_size(full);
                if (sz > 0) entry->file_size = (uint32_t)sz;
            }
        }
    }
    return 0;
}

void *fat_read_file(fat_fs_t *fs, const char *path, uint32_t *size_out) {
    (void)fs;
    if (size_out) *size_out = 0;
    if (!path) return NULL;
    long sz = kb_size(path);
    if (sz < 0) return NULL;
    void *buf = kb_malloc((unsigned long)(sz > 0 ? sz : 1));
    if (!buf) return NULL;
    long fd = kb_open(path, KB_O_READ);
    if (fd < 0) { kb_free(buf); return NULL; }
    long got = 0;
    while (got < sz) {
        long n = kb_read(fd, (char *)buf + got, (unsigned long)(sz - got));
        if (n <= 0) break;
        got += n;
    }
    kb_close(fd);
    if (got != sz) { kb_free(buf); return NULL; }
    if (size_out) *size_out = (uint32_t)sz;
    return buf;
}

int fat_exists(fat_fs_t *fs, const char *path) { (void)fs; return path ? kb_exists(path) : 0; }
int fat_mkdir (fat_fs_t *fs, const char *path) { (void)fs; return path ? kb_mkdir(path) : -1; }
int fat_delete(fat_fs_t *fs, const char *path) { (void)fs; return path ? kb_unlink(path) : -1; }
int fat_rename(fat_fs_t *fs, const char *a, const char *b) {
    (void)fs; return (a && b) ? kb_rename(a, b) : -1;
}

MUST_CHECK int fat_write_file(fat_fs_t *fs, const char *path, const void *data, uint32_t size) {
    (void)fs;
    if (!path || (!data && size)) return -1;
    long fd = kb_open(path, KB_O_WRITE);
    if (fd < 0) return -1;
    unsigned long done = 0;
    while (done < size) {
        long n = kb_write(fd, (const char *)data + done, size - done);
        if (n <= 0) { kb_close(fd); return -1; }
        done += (unsigned long)n;
    }
    kb_close(fd);
    return 0;
}

// ext2_* / blk_* - ABSENT. MEASURED: zero call sites in the DOS sources; they
// arrive only through headers. A Ring-3 process has no raw block access, and
// nothing should give it any: filesystem access belongs at the open() boundary
// where credentials are enforced. Each returns the kernel's own "not available"
// value and records itself.
int      ext2_is_mounted(void)                      { ABSENT("ext2_is_mounted"); return 0; }
uint32_t ext2_resolve_path(const char *p)           { (void)p; ABSENT("ext2_resolve_path"); return 0; }
int      ext2_get_is_dir(uint32_t i, int *o)        { (void)i; if (o) *o = 0; ABSENT("ext2_get_is_dir"); return -1; }
int64_t  ext2_read_file_range(uint32_t i, uint64_t o, uint64_t l, void *d) {
    (void)i; (void)o; (void)l; (void)d; ABSENT("ext2_read_file_range"); return -1;
}
// #VOLAPI: blk_read IS NOT DEFINED HERE ANY MORE, AND THAT IS THE POINT.
//
// It used to be an ABSENT() stub returning -1. A stub is a RUNTIME refusal: the
// caller links, ships, runs, and gets a wrong answer in the field, which is
// exactly how the Ring-3 Red Alert failure presented (16,759,601,960
// instructions and then "PLEASE INSERT A RED ALERT CD"). With the stub deleted
// and the three block-reading sources dropped from the gen tree (mkgen.sh), the
// binary now has NO definition and NO caller, so a future kernel/dos change that
// reaches for raw block access fails AT THE LINK, by name, in this build.
//
// That converts the security rule "Ring 3 gets no raw block access" from a thing
// someone has to keep deciding into a thing the build cannot express. It is the
// same argument as leaving diskimg_mount()/diskimg_eject() unlinked rather than
// guarding them: a check can be forgotten, an absent symbol cannot.
//
// If you are here because a link failed on blk_read: do not add the stub back.
// Ask the kernel for what you need through the mediated gateway
// (shim/volshim.c, SYS_DISKIMG VOLINFO), or read the file through open().
int  blk_root_is_usb(void)     { ABSENT("blk_root_is_usb");     return 0; }
int  blk_root_usb_index(void)  { ABSENT("blk_root_usb_index");  return -1; }
void blk_cache_stats(uint64_t *h, uint64_t *m, int *e) {
    if (h) *h = 0; if (m) *m = 0; if (e) *e = 0; ABSENT("blk_cache_stats");
}

// ===========================================================================
// PERMISSIONS AND THE GUEST FILESYSTEM GATE
//
// THE #708 GATE, AND WHY RING 3 IS A STRONGER BOUNDARY RATHER THAN A WEAKER
// ONE. In the kernel, the DOS interpreter runs as a Ring-0 thread carrying a
// SYNTHESISED uid, and guestfs_allow() is what stops it reaching files that uid
// should not have. That gate is self-policing: the same privileged thread that
// wants the file also decides whether it may have it, so it holds only as long
// as the interpreter is correct.
//
// In Ring 3 the enforcement moves. The host is an ordinary process running as
// the real user, and EVERY file access is a kernel open() subject to the
// kernel's own credential checks. The guest cannot reach a file its user could
// not reach from a shell, whatever the interpreter does or gets tricked into
// doing, because the decision is no longer the interpreter's to make.
//
// So guestfs_allow() returns "permitted" here NOT because the check is being
// skipped, but because it has been superseded by a check the guest cannot
// influence. This is an ARCHITECTURAL claim and it is not taken on trust: see
// the adversarial test in tools/dosring3-sandbox/, which drives the Ring-3 host
// at paths outside its drive roots, another user's files and /CONFIG
// credentials, on BOTH filesystems, and requires a refusal for each.
// ===========================================================================
int guestfs_allow(uint32_t slot, const char *native_path, int access, const char *what) {
    (void)slot; (void)native_path; (void)access; (void)what;
    return 1;
}
// ARMING IS STILL REQUIRED EVEN THOUGH guestfs_allow() IS SUPERSEDED.
//
// These were no-ops returning success, on the reasoning above that the Ring-3
// kernel open() is the real gate. That is true of the ENFORCEMENT half and
// false of the IDENTITY half: the armed slot is also where dos/dosexec.c reads
// the launching user back from (guestfs_cred_rs), and it is the ONLY place it
// reads it from, deliberately, because inside the guest thread the kernel's
// own uid is 0 by construction.
//
// MEASURED, NetHack, build 2282: with the slot unarmed, guestfs_cred_rs()
// returned E_NOT_ARMED, t->svc.homedir stayed empty, "%HOME%" was never
// expanded, and the guest's level/save directory resolved to the literal path
// .../NETHACK/%HOME%/GAMES/NETHACK. NetHack probes that directory for
// writability by creating a file in it; the create failed, and it printed
// "Some invalid directory locations were specified: leveldir, savedir" on its
// own text page and exited 1 after 661362 instructions. The in-kernel run of
// the same binary on the same image resolved /HOME/MAYTERA/GAMES/NETHACK and
// played.
//
// PROC_AS_CALLER is the correct kind here and not a convenience: in Ring 3
// there really IS a Ring-3 caller (this process), which is the one case
// spawn_ident_resolve_rs() accepts without a session lookup, and
// spawnid_caller_ident() below answers it with this process's own credentials.
int  guestfs_arm_caller (uint32_t slot) {
    return guestfs_arm_rs(slot, PROC_AS_CALLER, 0);
}
int  guestfs_arm_session(uint32_t slot) {
    return guestfs_arm_rs(slot, PROC_AS_SESSION, 0);
}

// THE LAUNCH-TIME ARM, exported across the wall for dosmain.c.
//
// In the kernel, dos_launch_common() arms the slot before proc_create() and
// REFUSES the launch if it cannot, because a guest with no resolvable identity
// would start, render, and then fail every file operation. The Ring-3 host
// calls dos_run_file() directly - it is already a process, so it has no
// proc_create() to sit in front of - and so it never reached that arm. The
// consequence was not a permission failure but a silently WRONG PATH: with the
// slot unarmed dosexec.c cannot read the launching user back, t->svc.homedir
// stays empty, and the "%HOME%" token in a guest path is left literal.
//
// Primitive-typed by construction (int, no arguments), so it crosses the
// kbridge.h wall without either universe learning the other's types.
int dosring3_arm_guest_identity(void) {
    return guestfs_arm_caller(GUESTFS_SLOT_DOS);
}
void guestfs_finish     (uint32_t slot) { (void)slot; }

// perms_* - the kernel's permission DATABASE. In Ring 3 the authority is the
// filesystem itself via open(), so these report "no override recorded", which
// is the same answer the kernel gives for a path with no perms entry.
int  perms_get  (const char *p, uint32_t *u, uint32_t *g, uint16_t *m) {
    (void)p; if (u) *u = kb_uid(); if (g) *g = kb_gid(); if (m) *m = 0644; return -1;
}
int  perms_check(const char *p, uint32_t uid, uint32_t gid, int access) {
    (void)p; (void)uid; (void)gid; (void)access; return 1;   // open() is the real check
}
void perms_set  (const char *p, uint32_t u, uint32_t g, uint16_t m) {
    (void)p; (void)u; (void)g; (void)m; ABSENT("perms_set");
}
void perms_remove(const char *p) { (void)p; ABSENT("perms_remove"); }
void perms_on_create(const char *p, uint32_t u, uint32_t g, int d) {
    (void)p; (void)u; (void)g; (void)d;   // the kernel applies the creating
                                          // process's own credentials already
}

// ===========================================================================
// TIME - REAL
// ===========================================================================
uint32_t g_timer_hz = 1000;    // SCOPED: Ring-3 clocks are millisecond-based,
                               // so the "tick" the DOS layer converts against
                               // is 1 ms. Nothing here reads a real timer IRQ.
uint64_t sched_now_ms(void) { return kb_mono_ms(); }
void rtc_read_time(int *h, int *m, int *s) { kb_localtime(h, m, s, 0, 0, 0, 0); }
void rtc_read_date(int *d, int *mo, int *y, int *wd) { kb_localtime(0, 0, 0, d, mo, y, wd); }

// ===========================================================================
// SYNCHRONISATION - REAL, and #426-compliant.
//
// The wait_event macros in sync/waitq.h expand to exactly the condvar protocol:
//
//     __wait_prepare(wq,&e,0);            ->  lock the queue's mutex
//     if (cond) { __wait_finish(...); }   ->  predicate tested UNDER the mutex
//     __wait_event_wait(&e);              ->  cond_wait: atomically unlock+sleep
//     __wait_finish(wq,&e);               ->  unlock
//
// and wake_up_all() locks the same mutex and broadcasts. No wake can be lost:
// a waker that sets the condition between the predicate test and the sleep
// blocks on the mutex until the sleeper has released it inside cond_wait, and
// its broadcast then wakes the sleeper, which re-tests the predicate because
// the macro loops. This is a real blocking primitive, not a poll.
//
// Spinlocks become mutexes. In Ring 3 there is no interrupt flag to save, and
// the IRQ the kernel's irqsave form protects against does not exist in a
// process: the only concurrency is between this app's own threads, which a
// mutex handles exactly. The returned "flags" value is therefore unused.
// ===========================================================================
#define SYNC_MAX 64
static struct { const void *key; void *m; void *c; } g_sync[SYNC_MAX];
static int   g_sync_n = 0;
static void *g_sync_guard = 0;

static void sync_guard_init(void) { if (!g_sync_guard) g_sync_guard = kb_mutex_new(); }

static int sync_slot(const void *key) {
    sync_guard_init();
    kb_mutex_lock(g_sync_guard);
    int idx = -1;
    for (int i = 0; i < g_sync_n; i++) if (g_sync[i].key == key) { idx = i; break; }
    if (idx < 0 && g_sync_n < SYNC_MAX) {
        idx = g_sync_n++;
        g_sync[idx].key = key;
        g_sync[idx].m   = kb_mutex_new();
        g_sync[idx].c   = kb_cond_new();
    }
    kb_mutex_unlock(g_sync_guard);
    return idx;
}

void spinlock_init_named(spinlock_t *lock, const char *name) {
    (void)name; if (lock) (void)sync_slot(lock);
}
uint64_t spinlock_acquire_irqsave(spinlock_t *lock) {
    int i = sync_slot(lock);
    if (i >= 0) kb_mutex_lock(g_sync[i].m);
    return 0;
}
void spinlock_release_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    int i = sync_slot(lock);
    if (i >= 0) kb_mutex_unlock(g_sync[i].m);
}

void wait_queue_head_init(wait_queue_head_t *wq) { if (wq) (void)sync_slot(wq); }

void __wait_prepare(wait_queue_head_t *wq, wait_queue_entry_t *entry, int exclusive) {
    (void)exclusive;
    if (!wq || !entry) return;
    memset(entry, 0, sizeof *entry);
    entry->wq = wq;
    int i = sync_slot(wq);
    if (i >= 0) kb_mutex_lock(g_sync[i].m);
}

void __wait_finish(wait_queue_head_t *wq, wait_queue_entry_t *entry) {
    (void)entry;
    if (!wq) return;
    int i = sync_slot(wq);
    if (i >= 0) kb_mutex_unlock(g_sync[i].m);
}

int __wait_event_wait_deadline(wait_queue_entry_t *entry, uint64_t deadline) {
    if (!entry || !entry->wq) return WAIT_OK;
    int i = sync_slot(entry->wq);
    if (i < 0) return WAIT_OK;
    unsigned long long dl = (deadline == WAIT_DEADLINE_NEVER) ? 0ULL : (unsigned long long)deadline;
    int timed_out = kb_cond_wait(g_sync[i].c, g_sync[i].m, dl);
    entry->wake_reason = timed_out ? WAIT_TIMEOUT : WAIT_OK;
    return entry->wake_reason;
}
int __wait_event_wait(wait_queue_entry_t *entry) {
    return __wait_event_wait_deadline(entry, WAIT_DEADLINE_NEVER);
}
void wake_up_all(wait_queue_head_t *wq) {
    if (!wq) return;
    int i = sync_slot(wq);
    if (i < 0) return;
    kb_mutex_lock(g_sync[i].m);
    kb_cond_broadcast(g_sync[i].c);
    kb_mutex_unlock(g_sync[i].m);
}

// ===========================================================================
// PROCESSES / THREADS - REAL
// ===========================================================================
int proc_create(const char *name, void (*entry)(void *), void *arg,
                uint32_t stack_size) {
    (void)stack_size;
    return kb_thread_start(entry, arg, name) == 0 ? 1 : -1;
}
void proc_sleep(uint32_t ms) { kb_sleep_ms(ms); }
void proc_yield(void)        { kb_yield(); }

// ===========================================================================
// INPUT - REAL, over the Stage-1 focus-scoped syscall.
// ===========================================================================
volatile int g_dos_scancode_tap = 0;    // written by the DOS layer on focus edges
int  dos_scancode_get(void)   { return kb_scancode_get(); }
void dos_scancode_clear(void) { kb_scancode_clear(); }

int32_t mouse_x = 0, mouse_y = 0;
uint8_t mouse_buttons = 0;

uint32_t keyboard_get_modifiers(void) { return kb_key_modifiers(); }

// ===========================================================================
// HOST WINDOW - REAL. Same seam DOOM uses: SYS_WIN_BLIT then win_invalidate.
//
// The kernel's win16_host_create() hands the DOS task a RAW POINTER to
// user_windows[slot].content_buffer, which the six present functions rasterise
// into directly. In Ring 3 the host owns that buffer itself and blits it, which
// is the one genuinely new cost of this move (a full-frame copy_from_user per
// present: 256KB at mode 13h, ~1.2MB at 640x480).
// ===========================================================================
#define HOSTWIN_MAX 4
static struct {
    int       used, handle, w, h;
    uint32_t *buf;
} g_hw[HOSTWIN_MAX];

int win16_host_create(const char *title, int x, int y, int w, int h,
                      uint32_t **out_buf, int *out_w, int *out_h,
                      struct window **out_win) {
    int slot = -1;
    for (int i = 0; i < HOSTWIN_MAX; i++) if (!g_hw[i].used) { slot = i; break; }
    if (slot < 0) return -1;
    int handle = kb_win_create(title ? title : "DOS", x, y, w, h);
    if (handle < 0) return -1;
    int cw = w, ch = h;
    (void)kb_win_content_size(handle, &cw, &ch);
    if (cw <= 0) cw = w;
    if (ch <= 0) ch = h;
    uint32_t *buf = (uint32_t *)kb_malloc((unsigned long)cw * (unsigned long)ch * 4UL);
    if (!buf) { kb_win_destroy(handle); return -1; }
    for (long i = 0; i < (long)cw * ch; i++) buf[i] = 0xFF000000u;
    g_hw[slot].used = 1; g_hw[slot].handle = handle;
    g_hw[slot].w = cw;   g_hw[slot].h = ch;   g_hw[slot].buf = buf;
    if (out_buf) *out_buf = buf;
    if (out_w)   *out_w   = cw;
    if (out_h)   *out_h   = ch;
    if (out_win) *out_win = 0;      // SCOPED: there is no kernel window_t in
                                    // Ring 3. Every DOS caller uses the SLOT,
                                    // not this pointer; returning a fake one
                                    // would be worse than returning none.
    kb_pump_start(handle);
    return slot;
}

void win16_host_destroy(int slot) {
    if (slot < 0 || slot >= HOSTWIN_MAX || !g_hw[slot].used) return;
    kb_win_destroy(g_hw[slot].handle);
    if (g_hw[slot].buf) kb_free(g_hw[slot].buf);
    memset(&g_hw[slot], 0, sizeof g_hw[slot]);
}

void win16_host_invalidate(int slot) {
    if (slot < 0 || slot >= HOSTWIN_MAX || !g_hw[slot].used) return;
    // THE PRESENT. The DOS present functions have already rasterised into
    // g_hw[slot].buf exactly as they do in the kernel; this hands that buffer
    // to the compositor.
    kb_win_blit(g_hw[slot].handle, 0, 0, g_hw[slot].w, g_hw[slot].h, g_hw[slot].buf);
    kb_win_invalidate(g_hw[slot].handle);
}

int win16_host_is_focused(int slot) {
    if (slot < 0 || slot >= HOSTWIN_MAX || !g_hw[slot].used) return 0;
    return kb_win_focused(g_hw[slot].handle);
}
void win16_host_focus(int slot) { (void)slot; ABSENT("win16_host_focus"); }

int win16_host_content_rect(int slot, int *ox, int *oy, int *ow, int *oh) {
    if (slot < 0 || slot >= HOSTWIN_MAX || !g_hw[slot].used) return -1;
    int w = g_hw[slot].w, h = g_hw[slot].h;
    (void)kb_win_content_size(g_hw[slot].handle, &w, &h);
    if (ox) *ox = 0; if (oy) *oy = 0;
    if (ow) *ow = w; if (oh) *oh = h;
    return 0;
}
int win16_host_work_area(int *ox, int *oy, int *ow, int *oh) {
    return kb_win_work_area(ox, oy, ow, oh);
}
void win16_host_route_close_to_dos(int slot) { (void)slot; }

// The kernel wakes DOS presenters from the framebuffer flip. In Ring 3 the
// blit and the invalidate complete synchronously inside win16_host_invalidate()
// above, so this queue exists to satisfy the wait_event call sites and is woken
// immediately.
//
// THE COMMENT ABOVE IS ABOUT THE WAIT QUEUE AND NOTHING ELSE. It used to sit on
// top of a second declaration as well:
//
//     volatile uint64_t g_fb_flip_count = 0;
//
// which is not a wait queue, is not inert, and was READ AS DATA on the hot path
// by rustkern/dosdisp.rs's frame gate. Nothing in userland/ ever wrote it, so
// the gate asked "has the screen moved on since my last frame?", got "no" on
// every single frame for the life of the process, and the only thing still
// delivering pictures was DOSDISP_STALE_MS - the 200 ms occluded-window
// backstop, whose own comment says it must never be the thing setting the
// visible rate. 200 ms is 5 Hz; the measured rate was 5.005 flips/s against
// 24.98 for the same guest on the in-kernel path.
//
// THE LESSON, because it is a shape and not a one-off: when stubbing a block of
// kernel symbols for a userland port, a symbol that is READ AS DATA needs a
// different justification from one that is merely CALLED, and the two must not
// share a comment. A reviewer who accepts the justification above carries it
// onto the next line, which is how a considered comment ends up vouching for a
// stub it was never about.
struct wait_queue_head g_fb_flip_wq;

// (#flipfix) The real thing, through the win16_host_* seam dos/dosexec.c
// already uses for everything else it needs from the window system.
//
// FAIL-OPEN, DELIBERATELY. kb_fb_flips() returns 0 when the running kernel has
// no SYS_FB_FLIP_COUNT. A constant 0 is exactly the stub this replaced, so it
// is never returned as-is: a local counter is returned instead, which differs
// on every call, so the gate presents every frame. dosdisp.rs compares for
// INEQUALITY rather than for growth, so that is the "always present" arm and
// not an accidental one, and it is also why a counter that jumps or resets
// cannot wedge the gate closed - only a counter that stops moving can, and that
// is the case the staleness backstop exists for.
uint64_t win16_host_flip_count(void) {
    static uint64_t no_syscall_tick = 0;
    uint64_t f = (uint64_t)kb_fb_flips();
    return f ? f : ++no_syscall_tick;
}

// ===========================================================================
// THE MONOTONIC CLOCK (#flipfix) - the KERNEL'S, not a second one.
//
// cpu/mono.h's mono_*() are thin wrappers over rustkern/mono.rs, which
// calibrates the TSC against PIT channel 0 with Ring-0 port I/O. Ring 3 cannot
// run that, and nothing here ever called mono_init(), so MONO_TSC_KHZ stayed 0
// and mono_ready() answered 0 forever. Every caller that branches on it went
// dark: dos_prof_report() returns immediately when !mono_ready(), which is why
// the [DOSFRAME] profile - the one instrument whose "frames shown N skipped M"
// field names the frame-gate defect above outright - emitted nothing at all
// from the Ring-3 host while the in-kernel arm emitted 24 lines over the same
// workload. The path you cannot see is the path the bug lives on.
//
// So mono.rs is NOT compiled into this host (see shim/dosrust.rs) and these
// three take its place, answering from SYS_MONO_US: the kernel's own
// TSC-backed clock, the same one mono.rs would have returned in Ring 0. That is
// one clock with one calibration, not a second calibration done here that could
// drift from it. Leaving mono.rs in and adding a Ring-3 entry point to it would
// also have left mono_busy_delay_us_rs() linked in, and its PIT fallback path
// does port I/O - a #GP waiting for a caller. An unresolved symbol at link time
// is a better failure than that.
//
// COST: one syscall per call, ~134 ns measured. Every call site in the DOS
// sources is inside the /CONFIG/DOSSPEED.CFG-gated profile (dosprof_t0/t1,
// dos_view_report, dos_prof_report), plus ktime.rs's RTC cache at most once a
// second, so the golden - which does not ship that file - pays nothing.
// ===========================================================================
int32_t mono_ready_rs(void) {
    // Latching, because 0 is BOTH "clock not ready" and "zero elapsed", and the
    // kernel's own contract says to branch on ready rather than on the value.
    // A DOS host starts many seconds into a boot, so one non-zero reading is
    // conclusive and there is no window where this is wrong in practice.
    static int32_t ready = 0;
    if (!ready && kb_kernel_mono_us() != 0ULL) ready = 1;
    return ready;
}
uint64_t mono_us_rs(void) { return (uint64_t)kb_kernel_mono_us(); }
uint64_t mono_ms_rs(void) { return (uint64_t)(kb_kernel_mono_us() / 1000ULL); }

// ===========================================================================
// WIN16 SCAFFOLDING - SCOPED.
//
// x86_16.c is shared by the DOS layer and the Win16 subsystem. This host is a
// DOS host: it never enters Win16 protected mode, so there is no DGROUP and no
// selector. Zero is the CORRECT answer, and the same one the kernel gives
// before any Win16 app has started - not a placeholder.
// ===========================================================================
uint16_t win16_dgroup_sel = 0;
uint16_t win16_dgroup_heap_base = 0;
volatile int g_x86_dbgring = 0;
void win16_trace(const char *fmt, ...) { (void)fmt; }

// ===========================================================================
// IDENTITY - REAL. The Ring-3 host runs AS the user, so its own credentials
// are the answer, rather than a session lookup the kernel had to synthesise.
// ===========================================================================
uint32_t desktop_get_session_uid(void)   { return kb_uid(); }
int      desktop_session_authenticated(void) { return 1; }
uint32_t spawnid_gid_for_uid(uint32_t uid) { (void)uid; return kb_gid(); }
int      spawnid_caller_ident(uint32_t *uid, uint32_t *gid) {
    if (uid) *uid = kb_uid();
    if (gid) *gid = kb_gid();
    return 0;
}

// ===========================================================================
// AUDIO - REAL (#181 Ring-3 audio). Stage 4 of the port.
//
// WHAT MOVED AND WHAT DID NOT. The Sound Blaster, OPL2 and 8237 emulation are
// pure software state machines over dos_task_t and were already running here
// unchanged; they contain no real port I/O. Only the SINK was missing. So this
// section wires the sink and nothing else: it adds no pacing, no buffering and
// no state of its own, because all three already exist in
// kernel/drivers/audio_pcm.c and are SHARED with the in-kernel DOS path.
//
// THE BLOCKING QUESTION, ANSWERED BY MEASUREMENT RATHER THAN WORKED AROUND.
// SYS_AUDIO_PCM_WRITE blocks on the sink's wait queue while the ring is full,
// which is identical to audio_pcm_write_kernel(): both doors call the same
// pcm_write_common(). The concern that a blocking write would stall the guest
// does not apply, because the caller is not the guest: dos_sb_pump() is started
// by proc_create("dossbpump", ...) (dosexec.c), which in Ring 3 is a real
// pthread. The interpreter thread keeps bursting its DOS_SLICE_MS = 4 ms and
// yielding while the pump thread sleeps in the kernel on wq_space. That is the
// same two-thread arrangement the in-kernel path has, so the pacing is not
// merely similar between the two paths, it is the same code on the same queue.
//
// #426: there is no sleep, no proc_yield and no poll below. The two waits are
// the kernel's own wait_event_timeout, reached through SYS_AUDIO_PCM_CTL, woken
// by the mixer's existing wake_up_all(&s->wq_space) on EVERY consume and on
// every teardown path. The ms bound is a backstop for a dead sink, not the
// mechanism.
// ===========================================================================
bool audio_is_available(void) { return kb_pcm_avail() ? true : false; }

// Left 0 deliberately, and the consequence is stated rather than hidden.
// sb_installed_policy() takes uac_is_ready() first, then audio_is_available(), and
// AUDIO_PCM_CTL_AVAIL already folds the USB DAC into its answer, so the card is
// advertised correctly. The ONE thing that differs from Ring 0 is the wording
// of a single status log line in dosexec.c, which will say "HDA/AC97" on a
// machine whose output is actually a USB DAC. That is a log string, not a
// guest-visible behaviour, and inventing a second syscall op to fix a log line
// was not worth the surface.
int uac_is_ready(void) { return 0; }

// ===========================================================================
// THE OPL2 / FM BRIDGE - REAL (#fmbridge). The last functional gap in Ring-3
// DOS audio, and it was never an audio problem.
//
// WHAT WAS WRONG. dos/dosexec.c reached the FM event queue as a VARIABLE:
// `static dos_fm_queue_t g_dos_fmq`, a file-scope static in that very file.
// This host compiles dosexec.c UNMODIFIED, so it got its own private copy of
// that queue in its own address space. A Ring-3 guest's OPL2 register writes
// were timestamped and queued perfectly - into a buffer with no consumer,
// because /APPS/FMSYNTH drains the KERNEL's queue through SYS_DOS_FM_EVENTS.
// fm_launch_synth() here returned -1 so that opl2_installed_policy() would at
// least report the chip ABSENT rather than advertise a synthesiser with nothing
// behind it, which is the fabrication #175 refused to ship. The honest ABSENT
// was correct; the missing transport was the bug.
//
// A variable access cannot become a syscall (#flipfix). So the queue moved to
// its own owner, kernel/dos/dosfmq.c, dosexec.c reaches it through the
// dos/dosfmq.h seam in BOTH rings, and this file answers that seam by
// forwarding to the KERNEL's queue over SYS_DOS_FM_HOST. There is still exactly
// ONE FM event queue in the system - and now it is checkable: `nm` on this
// binary finds no g_dos_fmq, because rustkern/fmq.rs is not compiled in and
// dosfmq.c is not in the gen tree (see mkgen.sh).
//
// PUSH RATHER THAN PULL, and #flipfix does not contradict it: see
// kernel/dos/dosfmq.h for the argument. In one line - a counter has a current
// value that can be sampled, an event does not, so there is nothing for a
// consumer to pull until the producer has produced it.
// ===========================================================================

// The `active` memo. dos_fm_note_write() asks this on EVERY guest write to port
// 0x389, before deciding to carry the event; in Ring 0 it is an unlocked read of
// a byte. Answering it with a syscall would put one on the guest's port-write
// path even for a guest with no queue open, which is the only case the
// pre-check exists to make cheap.
//
// It is a MEMO, not a second source of truth: this process is the only thing
// that opens or closes ITS queue, and the value is set from what the kernel
// actually answered, so a refused open leaves it 0 and no push is ever
// attempted. The kernel re-tests `active` under the queue lock inside
// dos_fmq_push_rs() regardless, exactly as it does for the in-kernel path.
static int g_fm_open = 0;

void dos_fmq_host_open(void) {
    int rc = kb_fm_open();
    g_fm_open = (rc == 0);
    if (rc != 0) {
        // NAME THE ARM. A silent failure here looks identical to a guest that
        // simply never wrote a register, and that ambiguity is what made this
        // defect survive as long as it did.
        kprintf("[DOSRING3] (#fmbridge) the kernel REFUSED the FM queue (rc=%d): "
                "this guest gets NO music. rc=-16 means an in-kernel DOS guest "
                "still holds it; rc<0 otherwise means this kernel has no "
                "SYS_DOS_FM_HOST.\n", rc);
        return;
    }
    kprintf("[DOSRING3] (#fmbridge) FM queue opened in the KERNEL; this guest's "
            "OPL2 writes now reach the same queue /APPS/FMSYNTH drains.\n");
}

int dos_fmq_host_active(void) { return g_fm_open; }

void dos_fmq_host_push(uint8_t reg, uint8_t val, uint64_t t_us) {
    kb_fm_push((unsigned)reg, (unsigned)val, (unsigned long long)t_us);
}

void dos_fmq_host_close(uint32_t *pushed, uint32_t *dropped) {
    // Read the counters BEFORE the close, so the numbers reported are this
    // session's. They would survive the close (dos_fmq_close_rs clears only
    // `active`), but reading first means the totals cannot be lost to a race
    // with the kernel's own opener-died teardown.
    long p = kb_fm_pushed(), d = kb_fm_dropped();
    if (pushed)  *pushed  = (p < 0) ? 0u : (uint32_t)p;
    if (dropped) *dropped = (d < 0) ? 0u : (uint32_t)d;
    if (g_fm_open) kb_fm_close();
    g_fm_open = 0;
}

void dos_fmq_host_stats(uint32_t *pushed, uint32_t *dropped,
                        uint32_t *peak, uint32_t *used) {
    if (pushed)  { long v = kb_fm_pushed();  *pushed  = (v < 0) ? 0u : (uint32_t)v; }
    if (dropped) { long v = kb_fm_dropped(); *dropped = (v < 0) ? 0u : (uint32_t)v; }
    if (peak)    { long v = kb_fm_peak();    *peak    = (v < 0) ? 0u : (uint32_t)v; }
    if (used)    { long v = kb_fm_used();    *used    = (v < 0) ? 0u : (uint32_t)v; }
}

// Asked of the KERNEL rather than compiled in as a constant, so the [FMQ] line
// reports the capacity of the queue this host is really feeding rather than of
// one it was built against. They agree today; a constant is how they would stop
// agreeing without anyone noticing.
uint32_t dos_fmq_host_capacity(void) {
    long v = kb_fm_capacity();
    return (v < 0) ? 0u : (uint32_t)v;
}

int dos_fmq_host_selftest(void) {
    int v = kb_fm_selftest();
    if (v < 0) {
        // A REFUSAL IS NOT A PASS, and it is not an ordinary failure either.
        // Scoring it as "1 failing check" and saying nothing else printed
        // "[dos] #182 FM bridge selftest: FAIL (1 failing)" on a run where the
        // queue was fine and this process simply did not own it - a line that
        // sends a reader to look at the queue. So the score still counts (an
        // untested self-test must never read as green) and the REASON is said
        // out loud beside it.
        kprintf("[DOSRING3] (#fmbridge) the FM self-test did not RUN (rc=%d): "
                "this process does not own the kernel FM queue. That is a "
                "refusal, not a broken queue - but it is scored as a failure "
                "because a self-test that never ran must never read as a "
                "pass.\n", v);
        return 1;
    }
    return v;
}

// SCOPED, and the scope is the whole point. This host has no view of process
// exits: the KERNEL observes them, for both DOS paths, and kernel/dos/dosfmq.c
// releases the queue's latches from proc_exit() - including closing the queue
// when THIS process dies without reaching dos_on_terminate(). Doing anything
// here would be a second, worse implementation of a job the kernel already does
// correctly for us.
int dos_fmq_host_release_pid(uint32_t pid) { (void)pid; return 0; }

// REAL now. The kernel runs its OWN fm_launch_synth() (gui/desktop.c) on our
// behalf: one launcher, one set of preconditions - including the refusal to
// launch on a machine with no audio sink, which is what keeps the OPL2 from
// reporting PRESENT with nothing behind it. Owner-only: only the process
// holding the FM queue may ask, which is why dos_fmq_host_open() runs first
// (dosexec.c opens the queue BEFORE launching, so the synthesiser cannot miss
// a write that arrives while it is still loading).
int fm_launch_synth(void) {
    if (!g_fm_open) {
        kprintf("[DOSRING3] (#fmbridge) not asking for a synthesiser: this "
                "process does not hold the FM queue, so nothing would drain "
                "it. The OPL2 will report ABSENT.\n");
        return -1;
    }
    return kb_fm_launch();
}

int64_t audio_pcm_open_kernel(uint32_t r, uint32_t c, uint32_t f) {
    return (int64_t)kb_pcm_open(r, c, f);
}

int64_t audio_pcm_write_kernel(int h, const int16_t *b, uint32_t fr) {
    if (!b || fr == 0) return 0;
    // The capture tap runs BEFORE the write and on the same buffer, so what it
    // records is exactly what the sink is handed - not a re-derivation of it.
    (void)kb_pcm_tap(b, fr);
    return (int64_t)kb_pcm_write(h, b, fr);
}

int64_t audio_pcm_close_kernel(int h) { return (int64_t)kb_pcm_close(h); }

uint32_t audio_pcm_consumed_kernel(int h) {
    long v = kb_pcm_consumed(h);
    return (v < 0) ? 0u : (uint32_t)v;
}

int audio_pcm_wait_below_kernel(int h, uint32_t m, uint32_t ms) {
    return (int)kb_pcm_wait_below(h, m, ms);
}

int audio_pcm_wait_consumed_kernel(int h, uint32_t t, uint32_t ms) {
    return (int)kb_pcm_wait_consumed(h, t, ms);
}

// audio_resample_stream{,_init} are NOT shimmed. They are compiled from
// kernel/drivers/audio_resample.c, which mkgen.sh copies verbatim into the gen
// tree: the same source file the kernel links. It was split out of
// drivers/audio.c for exactly this (it is pure fixed-point arithmetic over
// caller-owned buffers, with no driver state), so the two DOS paths cannot
// resample a guest's samples differently. Re-declaring them here as a shim
// would have been the third instance in this port of substituting something
// plausible for something that already existed.

// USB mass storage - ABSENT. A Ring-3 process has no USB transport, and should
// not: removable media reaches the guest through the filesystem, where the
// kernel's credential checks apply.
usb_msc_device_t *usb_msc_get_device(int index) {
    (void)index; ABSENT("usb_msc_get_device"); return 0;
}

// blk_census_io - ABSENT (block-layer instrumentation; no block layer here).
void blk_census_io(uint64_t *calls, uint64_t *sectors, uint64_t *us, uint64_t *maxus) {
    if (calls) *calls = 0; if (sectors) *sectors = 0;
    if (us) *us = 0; if (maxus) *maxus = 0;
    ABSENT("blk_census_io");
}

// user_lookup_uid - REAL for this process's own uid, ABSENT for any other.
//
// The Ring-3 host runs AS the user, so its own identity is the only one it can
// answer for, and it can answer it properly: the home comes from libc's
// userhome_root() (kb_home), i.e. from /CONFIG/PASSWD through getpwuid(),
// which is the SAME table the kernel's users.c parses. For any other uid this
// stays ABSENT, because this process cannot read the shadow database and must
// not invent an entry.
//
// Only `home` has a consumer in the DOS sources (dosexec.c, the "%HOME%"
// token). username is filled anyway so a future reader gets something truthful
// instead of an empty string that could pass for a valid name.
//
// A home of "/" is users.c's "this account has no home" fallback and
// dosexec.c already rejects it, so a ROOT session leaves "%HOME%" unexpanded
// here exactly as it does in the kernel. Nothing about a root session changes.
user_entry_t *user_lookup_uid(uint32_t uid) {
    static user_entry_t s_self;
    static int s_ready = 0;
    if (uid != kb_uid()) { ABSENT("user_lookup_uid(foreign uid)"); return 0; }
    if (!s_ready) {
        s_ready = 1;
        s_self.uid    = kb_uid();
        s_self.gid    = kb_gid();
        s_self.active = 1;
        s_self.home[0] = 0;
        if (kb_home(s_self.home, sizeof(s_self.home)) != 0) s_self.home[0] = 0;
        // Cosmetic: derive the name from the home's last component rather than
        // reading a second table, and leave it empty if there is no home.
        const char *base = s_self.home;
        for (const char *q = s_self.home; *q; q++) if (*q == '/') base = q + 1;
        int n = 0;
        for (; base[n] && n < (int)sizeof(s_self.username) - 1; n++)
            s_self.username[n] = base[n];
        s_self.username[n] = 0;
    }
    return &s_self;
}

int ext2_read_inode(uint32_t ino, ext2_inode_t *out) {
    (void)ino; (void)out; ABSENT("ext2_read_inode"); return -1;
}

// fb_put_pixel - ABSENT, AND ITS CENSUS ENTRY IS A LOAD-BEARING ASSERTION.
//
// A central claim of this port is that the DOS layer does NOT write to the
// framebuffer: it rasterises into a window content buffer and is composited
// like any other app (MEASURED: zero fb_blit/fb_put_pixel/fb_put_row/
// fb_get_backbuffer references in kernel/dos/*.c). This symbol is pulled in
// only by video/font.c's own glyph-DRAWING helper, which the DOS layer does not
// call - it calls font_get_glyph_cp437() and rasterises the bits itself.
//
// So this is not merely unimplemented: if "fb_put_pixel" ever appears in the
// ABSENT census at exit, the claim is FALSE for that guest and the port has
// found a real framebuffer write. That makes the census a standing test of the
// premise rather than a list of excuses.
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t colour) {
    (void)x; (void)y; (void)colour;
    ABSENT("fb_put_pixel [PREMISE VIOLATION: DOS must not touch the framebuffer]");
}
