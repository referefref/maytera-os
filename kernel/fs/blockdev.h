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

// Sector I/O in 512-byte units. channel/drive give the ATA identity used on the
// ATA path; they are ignored on the USB path. Returns the number of sectors
// transferred (> 0) on success, <= 0 on error, matching the ata_*_dma calling
// convention the FS layer already checks against.
int blk_read(uint8_t channel, uint8_t drive, uint64_t lba, uint32_t count, void *buf);
int blk_write(uint8_t channel, uint8_t drive, uint64_t lba, uint32_t count, const void *buf);

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

#endif // BLOCKDEV_H
