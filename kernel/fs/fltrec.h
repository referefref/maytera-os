// fltrec.h - RAW-BLOCK BOOT FLIGHT RECORDER. Breadcrumbs written to a reserved
// area of the boot medium by raw LBA write, needing NO FILESYSTEM.
//
// WHY THIS EXISTS
// ---------------
// The ASUS i7 laptop target has NO SERIAL PORT. Every persistent log this
// kernel writes (/BOOTLOG.TXT, /USBLOG.TXT, /DEVLOG.TXT, /boot/STAGE.TXT)
// reaches the medium only through a MOUNTED FILESYSTEM (bootlog_arm() takes a
// fat_fs_t and every flush goes through fat_write_file). A failure BEFORE or
// DURING the mount therefore writes nothing, anywhere, and the machine has no
// other channel. This recorder bypasses the filesystem: once the USB
// mass-storage block device answers reads and writes, breadcrumbs go to a raw
// LBA that no filesystem owns. The user powers off the hung machine, brings the
// stick back, and reads it with tools/fltrec/fltrec-read.sh.
//
// WHAT IT DOES AND DOES NOT COVER, stated honestly:
//   COVERS:  everything from "the USB MSC device answers a read and a write"
//            onward. That includes the whole FAT and ext2 mount path, TO-RAM,
//            the ELF loader, the compositor launch and userland, and it works
//            when the filesystem itself is the broken thing.
//   DOES NOT COVER: any failure BEFORE the block device exists (firmware
//            handoff, video mode set, PCI/xHCI bring-up up to the point a MSC
//            device is enumerated and READY). Nothing written through a block
//            device can. That is the screen's job.
//
// THE RESERVED AREA (measured on the golden, not derived)
// -------------------------------------------------------
// The golden image is GPT, 3,686,400 sectors of 512 bytes. LBA 0 is the
// protective MBR, LBA 1 the GPT header, LBA 2..33 the partition entry array,
// and partition 1 (the FAT ESP) is 2048-aligned. sgdisk reports "Total free
// space is 2014 sectors", which is exactly LBA 34..2047, verified all-zero in
// the current golden and in the asset base image. Nothing reads or writes it.
// It is INSIDE the image, so it travels with a whole-image dd and can be tested
// in a VM.
//
//   LBA 34             superblock
//   LBA 35             slot 0 header
//   LBA 36..537        slot 0 text   (502 sectors = 257,024 bytes)
//   LBA 538..1040      slot 1
//   LBA 1041..1543     slot 2
//   LBA 1544..2046     slot 3
//   LBA 2047           reserved, left zero
//
// FOUR slots means the last four boot attempts survive: the user can try three
// more times and still bring back the first failure.
//
// ARMING IS GATED ON A STRUCTURAL FACT, NOT A FILENAME. Before writing a single
// byte, fltrec_arm() requires a valid GPT header on the medium AND that every
// non-empty partition entry starts at or after LBA 2048. A disk with no GPT is
// refused outright: an MBR disk's post-MBR gap is exactly where GRUB embeds
// core.img, and this code will not gamble on someone else's disk.
//
// SEE ALSO: kernel/rustkern/fltrec.rs holds the record layout, the CRC32 and
// the append/dirty-sector arithmetic. That half is Rust because it is all
// byte-offset and sector-index arithmetic over disk-controlled bytes, which is
// where the off-by-one and the out-of-bounds read live, and because a flight
// recorder must not be the thing that faults on a machine already going wrong.

#ifndef FLTREC_H
#define FLTREC_H

#include "../types.h"

// ---------------------------------------------------------------------------
// GEOMETRY. These MUST match the constants at the top of rustkern/fltrec.rs and
// the ones re-derived in tools/fltrec/fltrec-read.sh. fltrec_selftest_rs()
// asserts the relationships between the Rust copies at boot, and fltrec.c
// _Static_asserts these C copies against the same values, so a hand-edit that
// would push the last slot past LBA 2046 and into the first partition fails to
// compile rather than corrupting a partition table.
// ---------------------------------------------------------------------------
#define FLTREC_SECTOR        512u
#define FLTREC_SB_LBA        34ull
#define FLTREC_SLOT_BASE_LBA 35ull
#define FLTREC_SLOTS         4u
#define FLTREC_SLOT_SECTORS  503u
#define FLTREC_TEXT_SECTORS  (FLTREC_SLOT_SECTORS - 1u)                  /* 502   */
#define FLTREC_TEXT_CAP      (FLTREC_TEXT_SECTORS * FLTREC_SECTOR)       /* 257024 */
#define FLTREC_REGION_LO     34ull
#define FLTREC_REGION_HI     2047ull

// Completion verdicts written into the slot header.
#define FLTREC_VERDICT_OPEN  0u   /* armed, never sealed: THIS BOOT DID NOT FINISH */
#define FLTREC_VERDICT_OK    1u
#define FLTREC_VERDICT_FAIL  2u

// ---------------------------------------------------------------------------
// FFI structs. Mirrored by #[repr(C)] FltSb / FltHdr in rustkern/fltrec.rs and
// sizeof-locked by _Static_assert in fltrec.c (the house pattern: a silently
// diverging layout would make the C read a different field than the Rust wrote,
// which is the failure this project keeps paying for).
// ---------------------------------------------------------------------------
typedef struct {
    uint64_t slot_base_lba;
    uint64_t boot_seq;
    uint32_t version;
    uint32_t slot_count;
    uint32_t slot_sectors;
    uint32_t text_sectors;
    uint32_t head_slot;
    uint32_t sb_lba;
} fltrec_sb_t;

typedef struct {
    uint64_t boot_seq;
    uint64_t seal_ms;
    uint32_t version;
    uint32_t slot_index;
    uint32_t build;
    uint32_t verdict;
    uint32_t text_len;
    uint32_t text_cap;
    uint32_t text_crc;   /* CRC32 of the first text_len bytes of the slot text */
    uint32_t _pad;       /* explicit, so ident sits at offset 48 on both sides  */
    uint8_t  ident[32];
} fltrec_hdr_t;

// ===========================================================================
// THE API
// ===========================================================================

// Arm the recorder. Call ONCE, as early as the root block device can answer a
// read and a write, and BEFORE any mount is attempted.
//
// Reads the superblock, decides the next slot, bumps the boot sequence number,
// writes the new superblock and slot header, zeroes the slot's text area, then
// flushes everything fltrec_write() accumulated before this point.
//
// Returns 1 if armed, 0 if not (the medium is not writable, the geometry is not
// 512-byte sectors, or the GPT safety gate declined). NEVER hangs, NEVER
// panics, and NEVER writes on any path where the gate declined.
//
// TWO ORDERING CONSTRAINTS, both load-bearing:
//
//  1. IT MUST BE CALLED BEFORE blk_root_to_ram(). blk_write() is write-through
//     AND keeps the RAM copy coherent, so writes reach the medium either way;
//     the problem is the READBACK. fltrec_arm() proves the medium is really
//     writable by writing the superblock and reading it back. Once TO-RAM or
//     the demand cache is enabled, blk_write() installs the new bytes into that
//     RAM copy on success, so the readback would be served from RAM and would
//     agree with itself even if the device silently dropped the write. Before
//     blk_root_to_ram() the block layer is BLK_MODE_OFF and the readback is a
//     real SCSI READ(10). Arming later does not break the recorder, it breaks
//     the ONE check that proves the recorder works.
//
//  2. IT MUST NOT BE CALLED FROM INSIDE A SCSI COMMAND. See fltrec_defer_begin()
//     below.
int fltrec_arm(void);

// Append one breadcrumb line. A newline is added if absent.
//
// Cheap by construction: appends into a RAM buffer and writes only the sectors
// that actually changed since the last flush, which is normally ONE sector.
//
// BEFORE ARMING IT STILL ACCUMULATES, exactly like bootlog_write() does, so the
// pre-arm breadcrumbs are carried down by fltrec_arm()'s first flush. This is
// how anything logged between "USB MSC device is ready" and "the recorder is
// armed" reaches the stick even though the recorder could not write yet.
//
// Does NOT mirror to kprintf. That is deliberate and is the one place this
// differs from bootlog_write(): kprintf takes g_console_lock, which is the
// deadlock 240dc9f fixed, and the machine this exists for has no serial port to
// mirror to anyway. Call sites that want serial output should keep their
// existing kprintf/bootlog_write call alongside.
void fltrec_write(const char *line);

// Force the dirty tail sector out now, and refresh the slot header so its
// text_len matches what is on the medium. fltrec_write() writes text sectors
// only; the header is refreshed here and at seal, so a caller that wants the
// header to reflect a checkpoint calls this.
void fltrec_flush(void);

// Write the final slot header with a completion verdict, so a reader can tell
// "this boot finished" from "this boot stopped here". `ok` non-zero records OK,
// zero records FAIL. Flushes any dirty text first. Idempotent.
void fltrec_seal(int ok);

// Non-zero once fltrec_arm() has succeeded.
int fltrec_armed(void);

// Override the 31-character identity string stamped into the slot header.
// fltrec.c fills it by default from version.h (version, build number, build
// date) because THIS KERNEL HAS NO COMMIT MACRO: there is no MAYTERA_GIT_COMMIT
// or equivalent anywhere in kernel/, so a slot header cannot carry a real
// commit hash today. If one is ever added, call this before fltrec_arm() (or
// before fltrec_seal(), which rewrites the header) and the field becomes the
// commit string the design asked for. Truncated at 31 characters plus NUL.
void fltrec_set_ident(const char *s);

// ---------------------------------------------------------------------------
// RE-ENTRANCY WINDOW. Same idea, and the same reason, as
// bootlog_defer_begin()/bootlog_defer_end().
//
// This recorder's flush path is blk_write() -> usb_msc_write() ->
// usb_msc_transport() -> msc_cmd_lock(), and msc_cmd_lock() is a plain
// test-and-set with NO OWNER TRACKING. A fltrec_write() issued from inside a
// SCSI command would therefore wait for a lock the same thread already holds,
// in a `for (;;)` that reparks every 10 ms and never returns: one burned thread
// per occurrence, progressively worse under load. That is #745 / task #62,
// already paid for once with /BOOTLOG.TXT.
//
// fltrec is defended THREE ways:
//   (a) STRUCTURALLY, against itself: an internal in-I/O flag means our own
//       flush can never re-enter our own flush, whatever a callee logs.
//   (b) By POLICY: fltrec_write() call sites are boot checkpoints, not driver
//       internals. Do not add one inside the storage stack.
//   (c) By this window, for when (b) is not enough. If you add a fltrec_write()
//       anywhere the storage stack can reach, bracket usb_msc_transport() with
//       these the way it is already bracketed with bootlog_defer_begin/end.
//       Inside the window fltrec_write() still accumulates into RAM; only the
//       device write is skipped, and the next call from a safe context carries
//       the accumulated delta down. Nesting is counted. ALWAYS pair them on
//       every return path.
// ---------------------------------------------------------------------------
void fltrec_defer_begin(void);
void fltrec_defer_end(void);

// Honesty counters. `sectors` = text+header sectors actually handed to
// blk_write, `failures` = blk_write calls that did not transfer what was asked,
// `dropped` = bytes fltrec_write() could not accumulate because the slot is
// full. All three are zero on a healthy boot, so a non-zero value distinguishes
// "measured, clean" from "never measured". Any pointer may be NULL.
void fltrec_stats(uint64_t *sectors, uint32_t *failures, uint64_t *dropped);

// Run the Rust self-test and report. Returns 0 on PASS, -1 on FAIL, and writes
// the number of assertions executed to *checks (a PASS with 0 checks is a
// vacuous harness, so the count is part of the verdict, not decoration).
// `make FLTTESTFAIL=1` makes it go RED on an otherwise healthy machine.
int fltrec_selftest(uint32_t *checks);

#endif // FLTREC_H
