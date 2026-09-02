#ifndef BLOCKDEV_H
#define BLOCKDEV_H

// #307: Block-device routing layer.
//
// The FAT and ext2 filesystem drivers historically read/wrote 512-byte sectors
// straight through the ATA/IDE DMA driver. To boot MayteraOS from a USB thumb
// drive we need those same sector reads to hit a USB Mass Storage (BBB/SCSI)
// device instead. This layer is the single choke point: by default it routes to
// ATA (so every existing ATA-disk VM boots unchanged, no regression); when the
// kernel selects a USB MSC disk as its root at boot, it routes to that device.
//
// Sectors are always 512 bytes here. USB MSC block I/O only participates when
// the selected device reports a 512-byte logical block size (checked at
// selection time in main.c); otherwise USB root is not enabled.

#include "../types.h"

// Select a USB MSC device (index into the usb_msc device table) as the root
// block device. All subsequent blk_read/blk_write calls route to it.
void blk_set_root_usb(int usb_msc_index);

// Revert to the ATA path (default state).
void blk_clear_root_usb(void);

// Non-zero if the root block device is currently a USB MSC disk.
int blk_root_is_usb(void);
int blk_root_usb_index(void);

// Sector I/O in 512-byte units. channel/drive give the ATA identity used on the
// ATA path; they are ignored on the USB path. Returns the number of sectors
// transferred (> 0) on success, <= 0 on error, matching the ata_*_dma calling
// convention the FS layer already checks against.
int blk_read(uint8_t channel, uint8_t drive, uint64_t lba, uint32_t count, void *buf);
MUST_CHECK int blk_write(uint8_t channel, uint8_t drive, uint64_t lba, uint32_t count, const void *buf);

// #375/#417: after the USB root is verified, copy the root device into RAM
// (TO-RAM) so all later reads are RAM-speed and the slow stick is never
// touched for reads. `used_bytes_hint` is the caller's best estimate of how
// much of the device actually holds real filesystem data (e.g. derived from
// the mounted FAT filesystem's used-cluster count), NOT the raw partition or
// device capacity. Sizing off raw device/volume capacity broke on a live-USB
// stick whose GPT+FAT32 had been expanded to fill a 116 GiB drive while only
// ~340 MB was real data: TO-RAM saw a 116 GB "device" and bailed out to a tiny
// demand cache, and the resulting flood of single-sector cold
// reads/write-throughs was enough to trip an unrelated xHCI ring-wrap bug and
// crash before the desktop ever came up. Pass 0 if no filesystem-level hint is
// available (falls back to legacy whole-device sizing). Falls back to a
// bounded demand cache if the sized copy does not fit RAM. Returns 1 if the
// (used-data-sized) root is now in RAM, 0 otherwise.
int blk_root_to_ram(uint64_t used_bytes_hint);

// #417: force TO-RAM off (still allows the bounded demand cache), e.g. when a
// boot-time config marker (/TORAMOFF.TXT) asks for it. Call before
// blk_root_to_ram().
void blk_toram_set_disabled(int disabled);

// #375: RAM stats for verification/diagnostics. hits = sectors served from RAM,
// misses = sectors read from USB. *enabled: 0 off, 1 TO-RAM, 2 demand cache.
void blk_cache_stats(uint64_t *hits, uint64_t *misses, int *enabled);

// ===========================================================================
// #250 AUXILIARY (NON-ROOT) VOLUME I/O.
//
// blk_read/blk_write above are the ROOT device's path: they consult the
// TO-RAM copy and the demand cache, both of which are indexed by LBA ALONE
// and hold sectors of exactly one device. A hot-plugged second USB stick has
// its own LBA 0, so routing it through those functions would serve it the
// ROOT device's sectors and, worse, a write would land on the boot medium.
// That is why a hot-plugged volume gets its own entry point rather than a
// "which device" argument on the existing one: there is no correct way to
// share a device-blind cache between two devices.
//
// `usb_index` is an index into the USB MSC device table (usb_msc_get_device).
// No caching of any kind: every call is a real SCSI transfer. Same return
// convention as blk_read/blk_write (sectors transferred, <= 0 on error).
int blk_read_aux(int usb_index, uint64_t lba, uint32_t count, void *buf);
MUST_CHECK int blk_write_aux(int usb_index, uint64_t lba, uint32_t count, const void *buf);

// #617: demand-cache installs DECLINED because a write completed while the
// read's device I/O was in flight (the lost-read-modify-write guard). See the
// g_wgen comment in blockdev.c. Non-zero means real read/write overlap on the
// root device; it is a measurement, not an error.
uint64_t blk_stale_skips(void);

// [no-ticket] I/O totals for charging one operation with what it cost the
// device: calls, sectors, device transfers, device microseconds. Snapshot
// before and after and subtract. Same counters [BLK122] prints.
void blk_census_io(uint64_t *calls, uint64_t *sectors,
                   uint64_t *n_dev, uint64_t *t_dev);


// ===========================================================================
// #SB: WRITE-STAGING OWNERSHIP. See rustkern/blkstage.rs for the full defect
// write-up (the ext2 superblock destroyed on golden build 2215) and for why
// the staging buffer's address lives in Rust and nowhere else.
// ===========================================================================

// Mirrors BlkStageStats in rustkern/blkstage.rs. sizeof-locked on both sides.
typedef struct {
    uint64_t base;          // installed staging buffer, 0 = never installed
    uint32_t bytes;         // its size
    uint32_t owned;         // bitmask, see BLKSTAGE_OWNED_* below
    uint64_t claims;
    uint64_t releases;
    uint64_t contended;     // claims that found the buffer already held
    uint64_t bad_releases;  // releases by a non-owner: always a caller bug
    uint64_t seal_broken;   // MUST STAY ZERO. Non-zero = active corruption.
    uint64_t verifies;
} blkstage_stats_t;
_Static_assert(sizeof(blkstage_stats_t) == 64,
               "blkstage_stats_t must match BlkStageStats in rustkern/blkstage.rs");

// blkstage_stats_t.owned bits. Kept as one field rather than two so the
// struct size stays locked at 64 across the FFI.
#define BLKSTAGE_OWNED_HELD          0x1u  // a writer holds the claim right now
#define BLKSTAGE_OWNED_UNSAFE_BUILD  0x2u  // built `make BLKSTAGE_UNSAFE=1`:
                                           // the claim is REMOVED. Never ship.

void     blkstage_stats_rs(blkstage_stats_t *out);
uint64_t blkstage_seal_broken_rs(void);

// One serial line summarising the staging counters. Called once late in boot
// so a reader can tell "measured, zero" from "never measured" (blame.md).
void blk_stage_report(void);

// Total sectors whose staged payload was found altered between staging and the
// device write. This is the counter that would have been non-zero on the boot
// that destroyed the owner's root filesystem. Carried on the heartbeat line.
extern uint64_t g_blk_seal_broken;

// #SB: `make BLKSTAGETEST=1` only. Compiles to nothing otherwise.
#ifdef BLKSTAGE_TEST
void blk_stage_selftest(void);
#endif

#endif // BLOCKDEV_H
