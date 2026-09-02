// diskimg.h - removable disk-image mount/eject for the #257 drive layer (#196)
//
// Lets the user mount a disk IMAGE file onto a removable drive letter so that
// file access to that letter reads from the image's filesystem instead of the
// /WINDIR folder:
//   ISO9660 (.iso) and raw CD (.img)  -> E: (CD-ROM, read-only)
//   FAT12 floppy (.img / .ima)        -> A: (floppy)
//
// STREAMING, NOT RAM-RESIDENT (changed for #196 phase 2). The first version of
// this file loaded the whole image into RAM with fat_read_file() and capped it
// at 16 MiB. A CD does not fit that shape: the Red Alert discs are 653 MB and
// 677 MB, and MAIN.MIX inside the Allied disc is 454 MB on its own, larger than
// the entire 256 MB kernel heap. ISO images are now read through an imgfile_t
// (dos/imgfile.h), which serves byte ranges out of a 256 KiB cache, so mounting
// a 653 MB disc costs 256 KiB. FAT12 floppies are still read into RAM because
// a floppy image is at most 2.88 MB and the FAT12 reader indexes the image
// directly; the cap for that path is DISKIMG_FAT12_MAX.
//
// GUEST VISIBILITY IS ONE REDIRECT, IN fat_open(). An earlier version hooked
// only fat_read_file(), which meant a whole-file read of a file on a mounted
// disc worked while INT 21h 3Dh open + 3Fh read of the same file saw nothing:
// the exact defect #725 had already fixed once for the ext2 root, reintroduced
// in miniature. fat_open() is the ONE place a path becomes a handle, so the
// redirect lives there and fat_read/fat_seek/fat_readdir_n/fat_write and every
// caller built on them (DOS INT 21h, the Win16 KERNEL file APIs, the VFS
// adapter, Files) get the disc for free. When nothing is mounted the test is a
// string compare that fails and behavior is unchanged.
//
// EJECT AND RE-MOUNT ARE LIVE. diskimg_mount() on a drive that already has an
// image ejects the old one first, so swapping disc 1 for disc 2 needs no
// reboot. That is a requirement, not a nicety: Red Alert ships one ISO per
// faction and asks for the other disc mid-game.
//
// ===========================================================================
// #739 LAYER 4: MANY DRIVES, NOT TWO. WHAT CHANGED AND WHY.
// ---------------------------------------------------------------------------
// The first cut had a two-entry table, slot 0 = A: and slot 1 = E:, because the
// target was one CD and one floppy. The requirement is several CDs at once and
// two floppies, so the table is now 26 entries indexed by drive letter and the
// LETTER POLICY (which class a letter belongs to, which letter a new image
// gets, what MSCDEX should report) moved into rustkern/drvmap.rs as pure
// integer logic. Read that file first: it is where the model is written down,
// this file is only the machinery that carries it out.
//
// THREE THINGS THIS LAYER NOW GUARANTEES THAT IT DID NOT BEFORE:
//
// 1. A MOUNT GENERATION, so a swapped disc cannot be silently misread. A
//    fat_file_t records the drive letter and the path INSIDE the image, and
//    re-resolves that path on EVERY read. Both Red Alert discs contain a file
//    called \MAIN.MIX. Before this change, swapping disc 1 for disc 2 while a
//    guest held a handle open on \MAIN.MIX meant the next read silently
//    returned the OTHER disc's bytes at that offset, with no error anywhere.
//    Every mount now bumps a per-letter generation, fat_open() stamps it into
//    the handle, and a read whose stamp no longer matches FAILS instead of
//    answering with the wrong disc. That is the deliberate answer to "what
//    happens to a guest holding an open handle when its disc is ejected":
//    the handle is invalidated, never redirected, and never silently wrong.
//
// 2. A REFERENCE COUNT, so an eject cannot free an image out from under a
//    reader. Mounting used to be reachable only from the boot harness, before
//    any guest was running; it is now reachable from a Ring-3 syscall at any
//    moment, including while a DOS guest is mid-read. The image object is
//    refcounted: the drive slot holds one reference, every in-flight read holds
//    one, and the last one out frees it. Eject therefore never blocks and never
//    waits, which is what keeps it out of the #426 busy-wait family entirely.
//
// 3. READ SERIALISATION PER IMAGE. Each mounted image has ONE 256 KiB imgfile
//    cache. Two readers of the same disc (a DOS guest and the Files app, now
//    that both can reach it) would interleave inside that cache and hand each
//    other the wrong block. Reads take a per-image wait-queue turnstile, which
//    is the shared blocking primitive (sync/waitq.h), not a hand-rolled poll.
// ===========================================================================
#ifndef DOS_DISKIMG_H
#define DOS_DISKIMG_H

#include "../types.h"
#include "../sync/waitq.h"

#define DISKIMG_FMT_NONE     0
#define DISKIMG_FMT_ISO9660  1
#define DISKIMG_FMT_FAT12    2

// Drive classes. MUST match DRV_CLASS_* in rustkern/drvmap.rs, which is the
// authority; these exist so C callers can name them.
#define DISKIMG_CLASS_NONE   0
#define DISKIMG_CLASS_FLOPPY 1
#define DISKIMG_CLASS_FIXED  2
#define DISKIMG_CLASS_CDROM  3

// Maximum images mounted at once. MUST match DRVMAP_MAX_MOUNTS in
// rustkern/drvmap.rs (locked by _Static_assert in diskimg.c). Each mount pins a
// 256 KiB imgfile cache, so this is a RAM bound, not a taste.
#define DISKIMG_MAX_MOUNTS   8

// Auto-placement: pass this as the letter index to diskimg_mount_idx() to let
// rustkern/drvmap.rs choose the lowest free letter of the right class.
#define DISKIMG_LETTER_AUTO  (-1)

// A FAT12 floppy image is read whole into RAM. 4 MiB covers every floppy format
// (1.44 MB, 2.88 MB) with room to spare and bounds what a bad image can cost.
#define DISKIMG_FAT12_MAX    (4u * 1024u * 1024u)

// Whole-file reads (diskimg_read_file) allocate the entire file. That is fine for a config file or an executable and impossible
// for a 454 MB archive, so whole-file reads above this refuse and the caller
// must use diskimg_read_range() instead. Returning NULL is the honest failure:
// the alternative is a kmalloc that cannot succeed.
#define DISKIMG_WHOLE_MAX    (8u * 1024u * 1024u)

// ---------------------------------------------------------------------------
// MOUNT / EJECT
// ---------------------------------------------------------------------------
// THE placement entry point. `want` is a drive-letter INDEX (0 = A .. 25 = Z)
// or DISKIMG_LETTER_AUTO. The image is opened and PROBED first, and only then
// does drvmap_place_rs() decide where it goes, because the format is a property
// of the bytes and not of the filename: a .img can be a floppy or a raw CD, and
// a .iso can be neither.
//
// Returns the letter INDEX actually used (>= 0), or a negative error. The
// negative values are the DRVMAP_E_* codes from rustkern/drvmap.rs where the
// placement was refused, and the DISKIMG_E_* codes below where the image itself
// was the problem. Distinct codes on purpose: "mount failed" with no reason is
// what makes a user click the same button again.
int  diskimg_mount_idx(int want, const char *imgpath);

// Errors originating in this layer rather than in the placement policy. Kept
// below the drvmap range (which ends at -6) so a caller can print either.
#define DISKIMG_E_OPEN     -10   // cannot open the image file (missing, or unreadable)
#define DISKIMG_E_UNKNOWN  -11   // opened, but neither ISO 9660 nor a FAT12 floppy
#define DISKIMG_E_TOOBIG   -12   // not an ISO and too large to be a floppy
#define DISKIMG_E_NOMEM    -13   // out of heap for the image or its cache
#define DISKIMG_E_BUSY     -14   // slot state prevented the operation
// #184: the file IS an ISO 9660 image, and its primary descriptor points its
// root directory past the end of the file, so there is no directory to read.
// Distinct from DISKIMG_E_UNKNOWN on purpose: "unrecognised format" for a file
// that is plainly a recognised format sends the user looking in the wrong
// place. See iso_root_within_rs() in rustkern/iso9660.rs.
#define DISKIMG_E_TRUNC    -15   // ISO 9660, but truncated: root dir past EOF

// Compatibility wrapper for the existing letter-based callers (the boot
// harness). Mounts on that exact letter; returns 0 on success, negative on
// error, NOT the letter index.
int  diskimg_mount(char letter, const char *imgpath);

// Eject: detach the image from the drive and revert it to its folder backing.
// ALWAYS SUCCEEDS if something was mounted. It does not refuse while a guest
// holds an open handle, and that is a decision, not an oversight: a user
// swapping discs mid-game is the case this whole feature exists for, and real
// hardware lets you press the button too. What eject guarantees is narrower and
// more useful than refusal: the outgoing image stays alive until the last
// in-flight read finishes (refcount), and every handle opened before the eject
// is invalidated by the generation bump rather than silently re-pointed at
// whatever is mounted next.
void diskimg_eject(char letter);
void diskimg_eject_idx(int idx);

// 1 if an image is currently mounted on the drive.
int  diskimg_is_mounted(char letter);

// Format of the mounted image (DISKIMG_FMT_*), or DISKIMG_FMT_NONE.
int  diskimg_format(char letter);

// Basename of the mounted image (e.g. "RA1.ISO"), or "" if none.
const char *diskimg_mounted_name(char letter);

// Size of the mounted image in bytes, 0 if none.
uint64_t diskimg_image_size(char letter);

// 1 if the mounted ISO also carries a Joliet supplementary descriptor.
int  diskimg_has_joliet(char letter);

// ---------------------------------------------------------------------------
// THE LIVE TABLE, as the rest of the kernel needs to see it
// ---------------------------------------------------------------------------
// Bitmask of letter indices that currently hold an image (bit 0 = A:). This is
// the ONE input the letter policy and the MSCDEX answer are both derived from,
// so the two cannot disagree the way three hardcoded constants in three files
// used to.
uint32_t diskimg_mounted_mask(void);

// How many images are mounted right now (popcount of the mask above).
int diskimg_mount_count(void);

// Class of a letter index, per rustkern/drvmap.rs (DISKIMG_CLASS_*).
int diskimg_letter_class(int idx);

// Current mount generation of a drive. Bumped on every mount AND every eject,
// starts at 0 and is never reused within a boot, so "the generation I was
// opened with" is a sound identity for the disc a handle belongs to. Returns 0
// for a letter that has never held anything.
uint32_t diskimg_generation(char letter);

// The MSCDEX view, derived from the live mask by drvmap_mscdex_rs(). Mirrors
// MscdexInfo in rustkern/drvmap.rs; sizeof-locked in diskimg.c.
typedef struct {
    uint32_t count;        // number of CD-ROM drives mounted
    uint32_t first;        // DOS drive number of the lowest CD letter (A=0)
    uint8_t  letters[32];  // the CD drive numbers, ascending, rest zero
} mscdex_info_t;
void diskimg_mscdex(mscdex_info_t *out);

// One drive's state, as handed to Ring 3 by SYS_DISKIMG. Mirrors
// diskimg_info_t in userland/libc/syscall.h; sizeof-locked in
// proc/syscall_argtab_lock.c because rustkern/argtab.rs hardcodes its size.
#define DISKIMG_F_MOUNTED   0x01u   // an image is on this letter
#define DISKIMG_F_JOLIET    0x02u   // that image carries a Joliet tree
#define DISKIMG_F_READONLY  0x04u   // writes to this drive are refused
#define DISKIMG_F_INUSE     0x08u   // at least one read is in flight right now
#define DISKIMG_F_MOUNTABLE 0x10u   // an image MAY be mounted on this letter

typedef struct {
    uint32_t letter;      // index, 0 = A .. 25 = Z
    uint32_t cls;         // DISKIMG_CLASS_*
    uint32_t fmt;         // DISKIMG_FMT_*
    uint32_t flags;       // DISKIMG_F_*
    uint32_t gen;         // mount generation
    uint32_t readers;     // in-flight reads on this image
    uint64_t size;        // image size in bytes, 0 if not mounted
    char     name[64];    // image basename, "" if not mounted
    char     path[192];   // full image path, "" if not mounted
} diskimg_info_t;

// Fill *out for letter index idx. Returns 0, or -1 for an out-of-range index.
// Succeeds for an UNMOUNTED letter too: the UI needs to show empty drives, and
// an empty CD letter is exactly what "no disc" looks like.
int diskimg_query(int idx, diskimg_info_t *out);
// ===========================================================================
// #VOLAPI: THE MEDIATED VOLUME GATEWAY (dimg_vol_t)
// ---------------------------------------------------------------------------
// THE REQUIREMENT, in the owner's words: "we need a proxy/gateway/api to allow
// virtual cd access to usermode apps". The alternative, exposing blk_read to
// Ring 3, was considered and REJECTED: it hands a userland process the whole
// device and destroys the boundary the Ring-3 DOS port exists to create. The
// standing rule is that privileged access is reachable "only with a negotiated
// contract". dimg_vol_t IS that contract, and this comment is its text.
//
// WHAT THE CONTRACT SAYS, and what it deliberately does not say:
//
//   THE APP ASKS   "which mounted volumes may I use, and what are they?"
//   THE KERNEL SAYS a per-letter identity: class, format, read-only, the disc's
//                   own LABEL, its geometry, and ONE VFS PATH (`root`) under
//                   which its files live.
//   THE APP THEN    uses ordinary open()/read()/lseek()/readdir() beneath that
//                   root, where EVERY existing credential check already applies.
//
// There is no LBA in this struct, no device identity, no channel/drive pair and
// no host-side image path. An app cannot express "read sector N of the disk"
// through it, because the only noun it is given is a directory.
//
// WHY A PATH AND NOT A FILE-HANDLE FAMILY. Because the files are ALREADY in the
// namespace: fs/fat.c's fat_open() has served a mounted image's subtree at
// /WINDIR/DRIVE_<L> since #196, and proc/syscall_path.h's path_root_ext2()
// routes every path syscall there via path_img_shadows(). So per-file
// open/read/seek/close, per-volume scoping, ".."-containment and the uid/gid
// check come from machinery that is already written, already exercised on every
// other path in the system, and already enforced at ~94 call sites rather than
// at however many a new bespoke syscall family remembered to check. A second
// handle family would have been a second place to forget. The ONLY thing that
// was genuinely missing from the namespace was the ANSWER TO "WHAT IS THIS
// DISC", which is what this struct is and nothing more.
//
// CAPABILITY GATING REUSES perms_check(), THE CANONICAL MECHANISM. A volume is
// enumerable to a caller exactly when that caller could traverse and read its
// root: perms_check(root, euid, egid, R_OK | X_OK). So "which apps may see
// which volumes" is a /CONFIG/PERMS.DB decision, expressed the same way every
// other access decision in this system is expressed, with no second policy
// scheme to keep in step. The no-entry default (fs/perms.c: root-owned 0755)
// makes a volume readable and NOT writable, which is the correct default for
// read-only media; an operator narrows it with a PERMS.DB entry on the root.
// Note the same honest limit sys_diskimg()'s MOUNT gate records: this means
// "the caller may read this", not "this is not a secret".
//
// READ-ONLY IS ENFORCED, NOT ASSUMED. ISO 9660 is read-only by nature, and this
// struct says so in DISKIMG_F_READONLY, but "by nature" is not enforcement: the
// kernel refuses writes/creates/unlinks/renames beneath an image-shadowed path
// outright (fs/fat.c img_ro_refuse()), so a bug in a caller cannot turn into a
// write attempt that some other layer happens to allow.
//
// SIZE IS LOCKED AT 288 BYTES ON PURPOSE. rustkern/argtab.rs validates syscall
// 361's out-buffer as a fixed 288-byte writable region (SZ_DISKIMG_INFO), so
// making this struct the SAME width lets the new command share the existing
// entry instead of needing a second one that could drift from it. The padding
// is not slack, it is the width lock; _Static_assert in
// proc/syscall_argtab_lock.c holds it.
// ===========================================================================
typedef struct {
    uint32_t letter;      // index, 0 = A .. 25 = Z
    uint32_t cls;         // DISKIMG_CLASS_* (drvmap.rs is the authority)
    uint32_t fmt;         // DISKIMG_FMT_*
    uint32_t flags;       // DISKIMG_F_* (MOUNTED / JOLIET / READONLY / INUSE)
    uint32_t gen;         // mount generation: identifies WHICH disc this is
    uint32_t bytes_per_sector;    // 2048 for ISO 9660
    uint32_t sectors_per_cluster; // 1 for ISO 9660
    uint32_t total_clusters;      // clamped to 0xFFFF, as DOS reports it
    uint64_t size;        // media size in bytes, 0 if not mounted
    char     label[40];   // the disc's OWN volume identifier, "" if none
    char     root[64];    // "/WINDIR/DRIVE_E" - where its files are, or ""
    uint8_t  reserved[144];  // width lock, see the note above. Always zeroed.
} dimg_vol_t;

// Fill *out for letter index idx, WITHOUT any credential filtering (the
// syscall layer applies that; an in-kernel caller has already been trusted).
// Returns 0 on success, -1 for an out-of-range index. Succeeds for an unmounted
// letter, reporting flags without DISKIMG_F_MOUNTED and an empty label, because
// "no disc" is a state a caller needs to be able to observe.
int diskimg_volinfo(int idx, dimg_vol_t *out);

// The VFS path beneath which drive letter `idx`'s files are served, e.g.
// "/WINDIR/DRIVE_E". Written even for an unmounted letter (the folder exists);
// this is the ONE definition of the spelling, so the syscall layer and the
// permission check cannot disagree with fs/fat.c about it.
// Returns 0 on success, -1 on a bad index or too small a buffer.
int diskimg_vol_root(int idx, char *out, int cap);


// ---------------------------------------------------------------------------
// VOLUME LABEL AND MEDIA SIZE
// ---------------------------------------------------------------------------
// diskimg_volume_label(): the name written ON the disc, trimmed and truncated
// to the 11 characters DOS reports, NUL-terminated. Returns 1 and fills `out`,
// or 0 with out[0] = 0 when the letter holds no image or the image carries no
// usable label. `cap` should be at least 12.
//
// This is deliberately NOT a field of diskimg_info_t. That struct's size is
// locked in three places (proc/syscall_argtab_lock.c, rustkern/argtab.rs and
// the userland libc copy) because it crosses the syscall boundary, and a label
// is wanted by an in-kernel caller (the INT 21h volume-label search), not by
// Ring 3. A separate accessor costs one function and no ABI.
//
// Neither call reads a sector: both answers are in RAM from mount time, so
// neither can block and both are safe from any context that may hold a lock.
int diskimg_volume_label(char letter, char *out, int cap);

// THE MEDIUM'S OWN GEOMETRY, for INT 21h AH=36h (#234e).
//
// THIS REPLACED diskimg_media_size(), WHICH IS DELETED RATHER THAN LEFT BESIDE
// IT. That function answered only "how many bytes", so its one caller had to
// INVENT a sector size to turn the answer into the four registers DOS wants.
// It invented 2048-byte sectors, one sector per cluster, for EVERY mounted
// image, because the only image class anyone had in mind was a CD. A FAT12
// floppy on A: was therefore described to the guest as a CD-geometry medium: a
// 720 KB disk came back as 360 clusters of one 2048-byte sector, when the disk
// itself says 512-byte sectors, two per cluster. The BYTE TOTAL happened to be
// right, so nothing looked wrong, and a guest that sizes a buffer or a record
// from CX (bytes per sector) got a number four times too large.
//
// Deleting the old accessor is the point, not tidiness. Leaving it would leave
// a zero-caller function whose whole shape invites the next caller to make the
// same invention, and this tree has an expensive history of exactly that
// (e1000_exit_crash_context(), term_mouse_report(), sse_save/sse_restore).
// One accessor that returns the whole triple cannot be misused that way.
//
// The geometry is not a guess for either class: an ISO states 2048 in its
// primary descriptor and rustkern/iso9660.rs refuses any other value, and a
// FAT12 image states bytes-per-sector and sectors-per-cluster in the BPB that
// fat12_parse() already reads at mount time. This hands back what the medium
// said, so the fiction has nowhere left to live.
//
// total_clusters counts the DATA area only, which is what DOS means by a
// total cluster count. Returns 1 and fills all three on success, 0 if the
// letter holds no image (outputs untouched).
int diskimg_geometry(char letter, uint32_t *bytes_per_sector,
                     uint32_t *sectors_per_cluster, uint32_t *total_clusters);

// ---------------------------------------------------------------------------
// GENERATION-CHECKED ACCESS (what fat_read / fat_readdir_n use)
// ---------------------------------------------------------------------------
// Identical to diskimg_read_range() / diskimg_readdir_n() except that the
// caller states WHICH mount it believes it is reading. If the drive has been
// ejected or swapped since, the call fails with DISKIMG_E_STALE instead of
// answering out of a different disc.
#define DISKIMG_E_STALE  -20
int64_t diskimg_read_range_gen(char letter, uint32_t gen, const char *relpath,
                               uint64_t off, uint64_t len, void *dst);
int diskimg_readdir_n_gen(char letter, uint32_t gen, const char *relpath, unsigned index,
                          char *name_out, int name_cap,
                          int *isdir_out, unsigned *size_out);

// Boot-time property test of rustkern/drvmap.rs. Logs ONE [DRVMAP] line to
// serial + /BOOTLOG. Called from main.c. Not a differential (there is no C twin
// to compare against): it proves the allocator obeys its own stated invariants
// on this exact build.
void diskimg_drvmap_selftest(void);

// Look up relpath in the mounted image. Fills *size_out (file size in bytes)
// and *isdir_out. Returns 1 if found, 0 if not. Either out pointer may be NULL.
int  diskimg_stat(char letter, const char *relpath,
                  uint64_t *size_out, int *isdir_out);

// Read a byte RANGE of a file in the mounted image. This is the streaming entry
// point and the only one that works on files larger than DISKIMG_WHOLE_MAX.
// Returns bytes read (0 at/after EOF) or negative on error.
int64_t diskimg_read_range(char letter, const char *relpath,
                           uint64_t off, uint64_t len, void *dst);

// Read a whole file from the mounted image. relpath is relative to the image
// root, with either '/' or '\\' separators (a leading separator is ignored).
// Returns a kmalloc'd buffer (caller kfree) + *size_out, or NULL on miss or if
// the file exceeds DISKIMG_WHOLE_MAX.
void *diskimg_read_file(char letter, const char *relpath, unsigned int *size_out);

// Directory listing callback. is_dir: 1 directory, 0 file. size: file size bytes.
typedef void (*diskimg_dir_cb)(const char *name, int is_dir, unsigned int size,
                               void *ud);
// List the directory relpath ("" / "/" = root) of the mounted image. Returns the
// entry count, or negative on error.
int  diskimg_listdir(char letter, const char *relpath, diskimg_dir_cb cb, void *ud);

// Positional directory step, the cursor form fat_readdir_n() needs. `index` is
// the 0-based entry number within relpath's directory. Returns 1 and fills the
// out params, or 0 at end of directory / -1 on error. Cursor-by-index rather
// than by byte offset deliberately: the on-disc record layout is the parser's
// business, not the caller's.
int  diskimg_readdir_n(char letter, const char *relpath, unsigned index,
                       char *name_out, int name_cap,
                       int *isdir_out, unsigned *size_out);

// ---------------------------------------------------------------------------
// ISO 9660 parse seam (#196 / #404). The live symbols route to the Rust ports
// under -DRUST_ISO9660 and to the verbatim C references otherwise; the C is
// kept so rollback is deleting one CFLAGS line, and so the boot differential
// has something to compare against. See rustkern/iso9660.rs.
// ---------------------------------------------------------------------------

// Mirrors IsoVol in rustkern/iso9660.rs. sizeof-locked in diskimg.c.
typedef struct {
    uint32_t root_lba;
    uint32_t root_len;
    uint32_t block_size;
    uint32_t kind;        // 1 = primary, 2 = Joliet supplementary
    uint32_t joliet_ucs;  // Joliet level 1/2/3, 0 when kind != 2
    uint32_t _pad;
    // The RAW 32 bytes at descriptor offset 40: space-padded, not terminated,
    // not trimmed. ASCII on a primary descriptor, UCS-2 BIG-ENDIAN on a Joliet
    // one, so it is decoded by whoever knows which `kind` it asked for. See the
    // doc comment on IsoVol::volid in rustkern/iso9660.rs, which is the
    // authority; this is the mirror.
    uint8_t  volid[32];
} iso_vol_t;

// Mirrors IsoDirRec in rustkern/iso9660.rs. sizeof-locked in diskimg.c.
typedef struct {
    uint32_t next;
    uint32_t lba;
    uint32_t len;
    uint32_t is_dir;
    uint32_t multi;
    uint32_t name_off;
    uint32_t name_len;
    uint32_t _pad;
} iso_dirrec_t;

int iso_vd_parse(const uint8_t *sec, uint32_t len, iso_vol_t *out);
int iso_vd_parse_c(const uint8_t *sec, uint32_t len, iso_vol_t *out);
int iso_vd_parse_rs(const uint8_t *sec, uint32_t len, iso_vol_t *out);

int iso_dirrec_at(const uint8_t *buf, uint32_t buflen, uint32_t pos, iso_dirrec_t *out);
int iso_dirrec_at_c(const uint8_t *buf, uint32_t buflen, uint32_t pos, iso_dirrec_t *out);
int iso_dirrec_at_rs(const uint8_t *buf, uint32_t buflen, uint32_t pos, iso_dirrec_t *out);

int iso_name_decode(const uint8_t *src, uint32_t srclen, int joliet,
                    uint8_t *out, uint32_t outcap);
int iso_name_decode_c(const uint8_t *src, uint32_t srclen, int joliet,
                      uint8_t *out, uint32_t outcap);
int iso_name_decode_rs(const uint8_t *src, uint32_t srclen, int joliet,
                       uint8_t *out, uint32_t outcap);

// Boot-time differential: proves the Rust ports agree with the C references on
// this exact build, over hand-built and fuzzed descriptor/record/name vectors.
// Logs ONE [RUST-DIFF] line to serial + /BOOTLOG. Called from main.c.
void diskimg_iso_rust_selftest(void);

// #196 live proof harness. Reads /CDTEST.TXT from the root filesystem; if it
// exists, each line is an image path, and the harness mounts each in turn on
// E:, lists the root directory, checksums a named file, then ejects and mounts
// the next WITHOUT rebooting. Absent file = no-op. Called from main.c.
void diskimg_boot_harness(void);

// [no-ticket] Force the ISO resolved-extent memo off (every read re-walks the
// directory tree, the pre-change behaviour). Set from /CONFIG/CDRAOFF.CFG and
// by dos/cdbench.c's control arm.
void isomemo_set_disabled(int off);
int  isomemo_disabled(void);

#endif // DOS_DISKIMG_H
