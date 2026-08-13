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

// Total size in bytes of the medium mounted on `letter`, or 0 for none.
uint64_t diskimg_media_size(char letter);

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

#endif // DOS_DISKIMG_H
