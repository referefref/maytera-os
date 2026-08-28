// dos/int21svc.h - THE ONE INT 21h SERVICE CORE (#736 Stage 1).
//
// ===========================================================================
// WHY THIS FILE EXISTS
// ---------------------------------------------------------------------------
// MayteraOS carried THREE live INT 21h implementations, measured on build 1741:
//
//   1. int21() in dos/dosexec.c        28 functions, the DOS task
//   2. win16_int() in exec/ne.c        12 functions, every Win16 guest
//   3. k_dos3call() in exec/win16api.c 17 functions, the KERNEL.102 gateway
//
// plus a FOURTH that was deleted the day before (dos/int21.c, #713). Each
// carried its OWN file-handle table, its OWN find-first cursor, its OWN DTA and
// its OWN wildcard matcher, and they drifted: 62h answered a different PSP
// segment in each, 3Dh streamed in one and slurped the whole file in another,
// and when the #708 permission gate landed it had to be applied to all three
// SEPARATELY, because gating one left the others wide open. That cost was paid
// twice inside a week. A fourth caller (the DOS/4GW DPMI host, docs/
// DOS4GW_DESIGN.md section 6) is being designed right now and would have paid
// it a third time.
//
// ===========================================================================
// THE INSIGHT THE SPLIT IS BUILT ON
// ---------------------------------------------------------------------------
// THE IMPLEMENTATIONS DID NOT DIFFER IN THE SERVICES THEY PROVIDED. THEY
// DIFFERED IN STATE. "Open this path", "read N bytes", "enumerate this
// directory" is the same work whoever asks. What is genuinely per-guest is the
// handle table, the current directory, the PSP segment, the drive map, the DTA
// and the identity the access is checked against.
//
// So: ONE service core, and a CONTEXT that holds all of that state.
//
//   dos_svc_int21(ctx, cpu)
//
// THE CORE HAS NO KNOWLEDGE OF WHICH GUEST IS CALLING. There is no "am I DOS
// or Win16" branch in int21svc.c, and adding one is the signal that a piece of
// state belongs in the context instead. Where the two guests genuinely behave
// differently, the difference is expressed as state:
//
//   psp_seg          DOS answers 0100h to AH=62h, the Win16 layer answers 0080h
//   has_ivt          the DOS task has a real IVT at 0000:0000 for 25h/35h;
//                    a Win16 guest has none, so 35h answers 0000:0000
//   con              the console vtable: where AH=02h/09h output goes and where
//                    AH=01h/06h/07h/08h/0Bh input comes from. The DOS task has
//                    a text page and a key queue; the Win16 layer has serial
//                    only and no keyboard, so its getkey is NULL and the input
//                    calls answer "no key", which is what they must do.
//   mem              the guest-memory accessor pair. Real mode uses
//                    x86_16_rd8/wr8. A DPMI host points these at its own flat
//                    guest space without the core knowing.
//   cwd_get/cwd_set  the per-drive current directory BINDING. The DOS task owns
//                    a private store; the Win16 context binds to the shared
//                    dospath.c store because four other call sites in
//                    win16api.c still read that one and a private copy would
//                    re-create the divergence this file removes.
//   extend           the caller's own INT 21h functions, for services that are
//                    part of the MACHINE rather than the API: the DOS task's
//                    MCB memory model (48h/49h/4Ah) lives there, because a
//                    Win16 guest's memory comes from KERNEL's global heap and a
//                    DPMI guest's from DPMI 0501h. The core dispatches to it
//                    only for functions it does not implement itself.
//
// ===========================================================================
// HOW A DPMI HOST FOR DOS/4GW ATTACHES (docs/DOS4GW_DESIGN.md section 6)
// ---------------------------------------------------------------------------
// As a THIRD CALLER, not a third implementation. Concretely:
//
//   1. Hold one dos_svc_ctx_t per DOS/4GW guest. dos_svc_ctx_init(ctx,
//      GUESTFS_SLOT_DPMI, "dpmi") after adding that slot to fs/guestfs.h and
//      rustkern/guestfs.rs, and arm it at launch exactly as dos_launch does.
//   2. Bind ctx->mem to accessors over the extender's flat guest space. The
//      core only ever asks for (seg, off) pairs it was HANDED in the register
//      frame, so a bridge that puts a real-mode-style seg:off in the frame and
//      resolves it flat in the accessor works unchanged; nothing in the core
//      computes a linear address itself.
//   3. Bind ctx->con to the guest's stdout, and leave getkey NULL until INT 16h
//      is bridged.
//   4. On DPMI 0300h (simulate real-mode interrupt) with INTNUM=21h, fill an
//      x86_16_cpu_t register frame from the DPMI real-mode call structure and
//      call dos_svc_int21(ctx, &frame); copy AX, the flags CF bit, and any
//      registers the function is documented to return, back into the structure.
//      That is the whole bridge. It contains no case labels for DOS functions,
//      which is what makes the one-implementation rule structural rather than a
//      matter of discipline.
//   5. Put DPMI's own memory services in ctx->extend if the guest ever issues a
//      real-mode 48h/49h/4Ah through 0300h.
//
// Everything this core gains, the DPMI host gains for free, including the #736
// real AH=40h write and the #708 permission gate.
// ===========================================================================
#ifndef DOS_INT21SVC_H
#define DOS_INT21SVC_H

#include "../types.h"
#include "../fs/fat.h"

struct x86_16_cpu;
typedef struct dos_svc_ctx dos_svc_ctx_t;

// Handles 0..4 are the five DOS standard handles (stdin, stdout, stderr, aux,
// prn); files start at 5, which is what both of the merged implementations
// already handed out, so no guest sees a handle number change.
#define DOS_SVC_MAX_FH        32
// Open FILES, as opposed to handles. Fewer than the handle count on purpose:
// dup/dup2 make several handles share one open file, which is the whole reason
// the two are separate.
#define DOS_SVC_MAX_SFT       24
#define DOS_SVC_FIRST_FILE_FH 5
#define DOS_SVC_CWD_MAX       96
#define DOS_SVC_PATH_MAX      160

// ---------------------------------------------------------------------------
// Guest memory. Two accessor pairs, no linear-address arithmetic anywhere in
// the core, so a caller whose "segment" is a selector or a flat-space base can
// bind these without the core knowing.
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t  (*rd8) (void *u, uint16_t seg, uint16_t off);
    void     (*wr8) (void *u, uint16_t seg, uint16_t off, uint8_t v);
    uint16_t (*rd16)(void *u, uint16_t seg, uint16_t off);
    void     (*wr16)(void *u, uint16_t seg, uint16_t off, uint16_t v);
} dos_svc_mem_ops_t;

// ---------------------------------------------------------------------------
// Console. putc is where AH=02h/06h/09h/40h-to-stdout land. getkey/peekkey may
// be NULL, which means "this guest has no console keyboard": the input calls
// then answer "no key waiting", never block, and never invent a keystroke.
// Returns 1 if a key was produced, 0 if none. The 16-bit value is the BIOS
// (scan << 8) | ascii pair.
// ---------------------------------------------------------------------------
typedef struct {
    void (*putc)   (void *u, uint8_t ch);
    int  (*getkey) (void *u, uint16_t *out);
    int  (*peekkey)(void *u, uint16_t *out);
} dos_svc_con_ops_t;

// ---------------------------------------------------------------------------
// One open file. Exactly one backing is active:
//
//   console        one of the five standard handles
//   streaming      a fat_file_t, read-only, seeks through the file on disk.
//                  This is what an ordinary game data file gets, so opening
//                  EGAGRAPH.CK5 still costs no heap, exactly as before.
//   buffered       an in-RAM image of the file plus a dirty flag, written back
//                  whole on close/commit. A handle is PROMOTED from streaming
//                  to buffered on its first write, and a create starts here.
//
// The promotion exists because handle-based streaming writes to the ext2 root
// are not implemented (fat_write() refuses an ext2-backed handle, see fat.c),
// and the shipping golden IS ext2-rooted. Read-whole/write-back is the model
// the Win16 layer's _lopen/_lclose has used successfully since #133, including
// for Word 6 document saves, so this reuses a proven mechanism rather than
// inventing one.
// ---------------------------------------------------------------------------
// #736 Stage 2: THIS IS MS-DOS'S OWN TWO-LEVEL MODEL, and adopting it is what
// makes AH=45h/46h correct instead of approximated. Real DOS keeps a Job File
// Table in the PSP mapping a HANDLE to an entry in the System File Table, which
// holds the actual open file. DUP does not copy the file, it points a second
// handle at the same SFT entry and bumps its reference count, which is exactly
// why a seek through the duplicate moves the original's file position. A
// one-level handle array cannot express that, and every "dup" built on one is a
// silent divergence waiting for the first program that relies on the sharing.
// #234h: DOS CHARACTER DEVICES. A DOS program reaches the console, the printer
// and the bit bucket by OPENING A NAME, not through a special API: "CON",
// "NUL", "PRN", "AUX". Those names are not files and never were, and DOS
// resolves them ahead of the filesystem from any directory and any drive.
//
// This mattered, MEASURED: The Dig (LucasArts, DOS/4GW) opens "CON" during
// startup. With no device layer that became an ordinary path lookup,
// `3Dh open FAIL '/WINDIR/DRIVE_E/DIG/CON'`, and the game exited 1 after 7,700
// instructions. Nothing in the log said "unimplemented", because from the
// filesystem's point of view nothing was: it was asked for a file that is not
// there and correctly said so.
#define DOS_CHARDEV_OPAQUE 1
#define DOS_CHARDEV_CON    2
#define DOS_CHARDEV_NUL    3

typedef struct {
    int        refs;                      // handles pointing here; 0 = free
    int        streaming;                 // fat handle live (read path)
    fat_file_t fat;
    uint8_t   *buf;                       // buffered image (NULL when streaming)
    uint32_t   cap;
    uint32_t   size;
    uint32_t   pos;                       // SHARED by every handle, per DOS
    int        wr_ok;                     // opened with write access
    int        dirty;                     // buffered image differs from disk
    int        wrote;                     // a write-back has been attempted
    int        commit_failed;             // ... and it failed. 3Eh must report it.
    char       path[DOS_SVC_PATH_MAX];    // resolved NATIVE path (gate re-check)
    // #745: nonzero when this handle is a CHARACTER DEVICE rather than a
    // file. It has no fat_file_t and no buffer, so every path that would touch
    // the filesystem must test this first.
    //
    // #234h: WHICH device, not just whether. It stays an int and every value is
    // TRUTHY, so every pre-existing `if (f->chardev)` keeps its exact meaning
    // and no call site had to be audited for a behaviour change; only the
    // sites that now need to tell CON from NUL look at the value.
    //
    //   DOS_CHARDEV_OPAQUE  the EMS driver "EMMXXXX0". Opened by name as the
    //                       first half of expanded-memory detection; reads and
    //                       writes are consumed, and IOCTL is the point of it.
    //   DOS_CHARDEV_CON     the console. Reads take the keyboard, writes reach
    //                       the screen, i.e. the SAME con.getkey/con.putc the
    //                       predefined handles 0/1/2 use. This is not a new
    //                       console, it is the existing one reached by name.
    //   DOS_CHARDEV_NUL     the bit bucket. Writes are consumed, reads are EOF.
    int        chardev;
} dos_svc_sft_t;

// ---------------------------------------------------------------------------
// The per-guest context. Everything here is STATE. The core reads it; it never
// asks who owns it.
// ---------------------------------------------------------------------------
struct dos_svc_ctx {
    // ---- identity (#708) ----
    uint32_t  guest_slot;                 // GUESTFS_SLOT_*
    const char *tag;                      // log prefix only, never branched on

    // ---- machine bindings ----
    void               *mem_u;
    dos_svc_mem_ops_t   mem;
    void               *con_u;
    dos_svc_con_ops_t   con;
    int                 has_ivt;          // 25h/35h reach a real IVT at 0000:0
    // #745: nonzero when an EMS manager is installed for this guest, so
    // 3Dh may open "EMMXXXX0" as a character device. The DOS task sets it
    // once its arena exists; the Win16 layer leaves it 0.
    int                 has_ems;
    uint16_t            psp_seg;          // answered by AH=62h
    uint16_t            dos_version;      // AX answered by AH=30h. The DOS task
                                          // says 0005h (DOS 5.0); the Win16
                                          // layer has always said 0A03h (3.10),
                                          // which is what a Win 3.1 box
                                          // reports. That is state, not a
                                          // branch, which is why it lives here.

    // ---- DOS state ----
    char      appdir[128];                // relative opens resolve here

    // #221b: THE LAUNCHING USER'S HOME DIRECTORY, native form, no trailing
    // slash. Empty means "this guest has no resolvable home", and the token
    // below is then left alone rather than expanded to something wrong.
    //
    // WHY A GUEST NEEDS THIS AT ALL. A DOS program keeps its state (save game,
    // high score, preferences) next to its executable, because DOS had one
    // user. On this system /DOS/<GAME> is root-owned 0755, so a desktop
    // session running as uid 1000 is refused every one of those writes: the
    // measured symptom was NetHack printing "Warning: cannot write record" and
    // "Some invalid directory locations were specified: leveldir, savedir,
    // bonesdir, scoredir, lockdir, troubledir" and then exiting. The game's
    // own config file can point those elsewhere, but a DOS path is a FIXED
    // STRING and cannot say "the current user's home", so there was no
    // per-user destination any DOS-era config could name. This field is that
    // destination, and dos_svc_resolve() expands "%HOME%" to it.
    char      homedir[128];

    // #rawrite: THE PER-USER WRITE OVERLAY. Empty ovl_base means this guest has
    // no overlay and every path resolves exactly as it did before the overlay
    // existed. When both are set, a resolved native path that is ovl_base or
    // lies under it gets a second candidate spelling under ovl_dir, and
    // overlay_apply() (dos/int21svc.c) picks between them by EXISTENCE. The
    // mapping itself is rustkern/dosovl.rs; these two strings are the whole of
    // the per-guest state it needs.
    //
    // WHY A PAIR AND NOT A GLOBAL TABLE: the overlay destination depends on WHO
    // launched the guest, so it is per-run state that belongs next to homedir,
    // which is per-run for the same reason. A global would be the cross-guest
    // leak that #736 Stage 1b removed from the CWD store.
    char      ovl_base[128];        // read-only shared install, e.g. "/DOS/RA"
    char      ovl_dir[128];         // per-user writable twin
    char      cur_drive;                  // 'A'..'E'
    uint16_t  dta_seg, dta_off;
    // Last DOS error, answered by AH=59h (Get Extended Error). The MS-C runtime
    // Word 6 ships with calls 59h after a failed call and CLASSIFIES the result,
    // so losing the code (the old DOS3Call path returned 0) made its
    // GetTempFileName loop unable to recognise a free name. It is per-guest
    // state like everything else here.
    uint16_t  last_err;

    // #221: A BLOCKING CONSOLE READ THAT FOUND NOTHING.
    //
    // INT 21h AH=01/07/08 and INT 16h AH=00/10h are BLOCKING services on real
    // DOS: they do not return until a key exists. This core answered them
    // non-blockingly (AL=0) on the reasoning that "a guest loops, exactly as it
    // does around the real BIOS". That is true of a guest that POLLS (AH=0Bh
    // then AH=07h) and false of a guest that calls the blocking service once
    // and believes the answer. NetHack is the second kind, and it reads a
    // zero as EOF: tty_askname() counts ten EOFs in a few microseconds and
    // calls bail("Giving up after 10 tries."), and every yn_function() prompt
    // is auto-answered with its escape default.
    //
    // The fix is NOT to block the interpreter thread (the banned #426 pattern,
    // and it would stop us pumping the very input being waited for). The
    // service sets this flag and writes NOTHING to the register file; the
    // caller then re-issues the SAME interrupt on a later run-loop pass, after
    // the loop has pumped input, presented a frame and yielded. The guest
    // cannot tell that apart from an INT that took a long time, which is
    // exactly what a blocking BIOS call is.
    //
    // The caller MUST clear it. dos_svc_int21() sets it only, so a caller that
    // does not honour it gets the old non-blocking behaviour minus the AL=0
    // write, which is why every caller is wired up in the same change.
    uint8_t   input_blocked;
    // The Job File Table: handle -> index into sft[], or -1 for closed.
    // Handles 0..4 are the five DOS standard handles and are never in the SFT.
    signed char  jft[DOS_SVC_MAX_FH];
    dos_svc_sft_t sft[DOS_SVC_MAX_SFT];

    // find-first / find-next cursor
    fat_file_t find_dir;
    int        find_active;
    char       find_pat[16];
    char       find_dirpath[DOS_SVC_PATH_MAX];
    // #rawrite: the OVERLAY half of a directory enumeration. A find that lands
    // on an overlaid directory has TWO directories to walk, and a save game
    // that exists only in the overlay must still appear in a SAVEGAME.* scan
    // or the game's load menu is empty. find_ovl is the overlay directory (or
    // empty, meaning there is only one phase); find_phase is 0 while the base
    // directory is being walked and 1 while the overlay is.
    char       find_ovl[DOS_SVC_PATH_MAX];
    int        find_phase;
    // The CX attribute mask AH=4Eh was called with. It used to be dropped on
    // the floor, which made every attribute-qualified search impossible: most
    // visibly _dos_findfirst(path, _A_VOLID, &f), the standard way a DOS
    // program identifies which disc is in a drive, which could never succeed.
    uint16_t   find_attr;
    // The drive letter this search is rooted at ('A'..'Z'), or 0 when it is not
    // a drive root. A volume label exists in the root directory and nowhere
    // else, so a search of a subdirectory must not produce one.
    char       find_drive;
    // A volume has exactly ONE label, so the synthetic entry is emitted at most
    // once per search however many times 4Fh is called.
    uint8_t    find_vol_done;

    // Per-drive current directory BINDING. cwd_get must never return NULL.
    // dos_svc_ctx_init() points both at the ctx-private store below; a caller
    // that must share the dospath.c globals rebinds them after init.
    const char *(*cwd_get)(dos_svc_ctx_t *ctx, char drive);
    void        (*cwd_set)(dos_svc_ctx_t *ctx, char drive, const char *path);
    char        cwd_priv[26][DOS_SVC_CWD_MAX];

    // Caller-owned INT 21h functions the core does not implement. Return 1 if
    // handled, 0 to fall through to the core's "unimplemented" report. May be
    // NULL.
    int (*extend)(dos_svc_ctx_t *ctx, struct x86_16_cpu *c, uint8_t ah);
    // Called after AH=4Ch has latched halted/exit_code, for a caller that wants
    // to do something at guest exit (the DOS task dumps its instruction ring).
    void (*on_terminate)(dos_svc_ctx_t *ctx, int code);
    void *owner;                          // the caller's own object (dos_task_t)

    // Bounce buffer for streaming reads. PER CONTEXT, not a file-scope static:
    // two guests can be live at once (the DOS and Win16 layers have separate
    // busy flags), and a shared read buffer between two filesystem readers is
    // exactly the shape of the #103 FAT corruption.
    uint8_t io_buf[4096];

    // ---- diagnostics ----
    // A service core that is not reached is indistinguishable from one that is,
    // unless it counts. dos_svc_report() prints these at teardown.
    uint32_t n_calls;
    uint32_t n_miss;                      // functions nothing implemented
    uint32_t n_writes;                    // AH=40h calls that hit a real file
    uint32_t n_bytes_written;
    uint32_t n_commits;                   // files written back to the medium
    uint32_t n_commit_fail;               // ... and the ones that did NOT land
    int      warned_lfn;                  // #221 one-shot: AH=71h declined
    int      warned_foreign_handle;       // one-shot: a handle from the Win16
                                          // KERNEL's separate table reached an
                                          // INT 21h handle call. See the note
                                          // above svc_handle_is_ours().
    uint8_t  last_miss[8];                // ring of the last unimplemented AHs
    int      last_miss_n;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
// Zero the context and install the safe defaults: no console, no IVT, PSP
// 0000h, drive C, private CWD store, no handles open. The caller then binds
// what it has.
void dos_svc_ctx_init(dos_svc_ctx_t *ctx, uint32_t guest_slot, const char *tag);

// Close every open handle, COMMITTING any dirty buffered file, and drop the
// find cursor. Call at guest teardown, BEFORE the identity slot is disarmed:
// a write-back is a filesystem access and is gated like any other, so
// disarming first would silently lose the data the guest thought it saved.
void dos_svc_ctx_close_all(dos_svc_ctx_t *ctx);

// One line of enforcement/usage evidence for the run.
void dos_svc_report(dos_svc_ctx_t *ctx);

// Commit one open file to the medium if it is dirty. Returns 0 on success or if
// there was nothing to do, negative if the write did NOT land. Exposed because
// AH=68h (commit) and a future EXEC both need it without closing the handle.
int dos_svc_commit(dos_svc_ctx_t *ctx, int handle);

// ---------------------------------------------------------------------------
// THE ENTRY POINT. Services one INT 21h call against `ctx`, with the register
// frame in `c`. Clears CF on entry, exactly as the merged implementations did.
// ---------------------------------------------------------------------------
void dos_svc_int21(dos_svc_ctx_t *ctx, struct x86_16_cpu *c);

// ---------------------------------------------------------------------------
// Shared helpers, exported so no caller has to fork a copy. These are the
// pieces that were duplicated three ways before.
// ---------------------------------------------------------------------------

// Resolve a DOS path against this context's appdir, current drive and per-drive
// CWD, into a native MayteraOS path. Includes the /WINDIR -> native-root
// fallback. This is the ONLY path-to-native translation the core performs.
// #rawrite: rustkern/dosovl.rs. Returns 1 and fills `out` with the overlay
// spelling of `path` when `path` is `base` or lies under it, 0 otherwise (and
// on every failure, so a fault is "not overlaid", never a wrong path).
int dosovl_map_rs(const unsigned char *base, const unsigned char *ovl,
                  const unsigned char *path, unsigned char *out, int outsz);
unsigned int dosovl_selftest_rs(void);

// #rawrite: the C wrapper over dosovl_map_rs that knows about a context. 1 when
// `native` is overlaid for this guest and `out` now holds the overlay spelling.
int dos_svc_overlay_dir(dos_svc_ctx_t *ctx, const char *native, char *out, int outsz);

// #rawrite: configure this guest's per-user write overlay. `base` is the shared
// read-only install directory, `ovl` the per-user writable twin. Either NULL or
// empty disables the overlay entirely (the pre-#rawrite behaviour).
void dos_svc_set_overlay(dos_svc_ctx_t *ctx, const char *base, const char *ovl);

void dos_svc_resolve(dos_svc_ctx_t *ctx, const char *in, char *out, int outsz);

// The #708 gate, for this context's identity. 1 = allow, 0 = deny.
int dos_svc_allow(dos_svc_ctx_t *ctx, const char *native, int access,
                  const char *what);

// The gate for a CREATE. Creating a file that does not exist is a WRITE TO ITS
// PARENT, which is how sys_open() already treats it; checking W_OK on a path
// with no perms entry would instead hit the root-owned 0755 default and deny
// every guest create. `leaf_exists` says which of the two questions to ask.
int dos_svc_allow_create(dos_svc_ctx_t *ctx, const char *native, int leaf_exists,
                         const char *what);

// Bind `ctx` to the x86_16 interpreter's REAL-MODE memory accessors. Every
// caller that runs its guest on exec/x86_16.c uses this rather than writing
// its own four thunks (both of them had, identically). A DPMI host that runs
// its guest somewhere else fills ctx->mem itself instead; that is the ONLY
// difference, and it is why ctx->mem is a vtable and not a hard call.
void dos_svc_bind_x86_16(dos_svc_ctx_t *ctx, struct x86_16_cpu *cpu);

// DOS 8.3 wildcard match on rendered "NAME.EXT" forms, case-insensitive.
// There were two private copies of this; this is the one.
int dos_svc_wild_match(const char *pat, const char *name);

#endif // DOS_INT21SVC_H
