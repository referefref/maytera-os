// usbvol.h - #740: mount DATA VOLUMES that live in the unpartitioned tail of
//            the boot device, so a game's data does not have to fit in the OS
//            image.
//
// THE PROBLEM THIS SOLVES, IN NUMBERS
// -----------------------------------
// The golden image's ext2 root is 395003 blocks of 4096 = 1.62 GB total, with
// 1.13 GB free. Discworld 2's data is 1.54 GB COMPRESSED. It does not fit and
// it never will; this is not a tidy-up problem. The boot stick is 124 GB with
// about 1.8 GB used, so roughly 122 GB of it is unpartitioned space that the OS
// image never touches. That space is where large game data belongs.
//
// WHAT A DATA VOLUME IS
// ---------------------
// A raw ISO 9660 image written straight into the device at a 1 MiB-aligned byte
// offset at or above USBVOL_BASE (4 GiB). No new on-disk format is invented:
// ISO 9660 self-describes its own length, which is what lets several volumes be
// chained one after another with no table of contents. rustkern/usbvol.rs holds
// the contract and the arithmetic; read it first.
//
// WHAT IS REUSED RATHER THAN REBUILT
// ----------------------------------
// Almost everything. This file is a PROBE and a MOUNT DRIVER, not a filesystem:
//   - dos/imgfile.c gained one backing kind (IMGF_KIND_BLKDEV) that reads a raw
//     device range through the same 256 KiB cache every mounted image uses, so
//     a 100 GB volume costs 256 KiB of RAM.
//   - dos/diskimg.c is UNCHANGED. It receives a synthetic path
//     (IMGF_BLKDEV_PREFIX) and mounts a device-backed volume exactly as it
//     mounts a file-backed one, with the same ISO parser, placement policy,
//     mount generation, refcount and read turnstile.
//   - fs/fat.c is UNCHANGED. Guest visibility is still the single fat_open()
//     redirect, so INT 21h 3Dh/3Fh, the Win16 KERNEL file APIs, the VFS adapter
//     and Files all see the volume without knowing it exists.
// The one genuinely new thing is where the bytes come from.
//
// WHY THE READS DO NOT COME OUT OF THE RAM COPY
// ---------------------------------------------
// #375 copies the root device into RAM (TO-RAM) so a slow stick is read once.
// That is exactly wrong for a volume that exists BECAUSE it is too big to fit
// in the image, and it would be far too big to fit in RAM. USBVOL_BASE is set
// above the TO-RAM window's hard cap (fs/blockdev.c: BLK_MAX_CHUNKS *
// BLK_CHUNK_BYTES = 2560 MiB), so fs/blockdev.c:489 cannot take its RAM path
// for these LBAs and every read is a real device read. usbvol_probe_and_mount()
// MEASURES this with blk_cache_stats() rather than asserting it.
//
// USB ROOT ONLY, AND WHY THAT IS STATED RATHER THAN WORKED AROUND
// ---------------------------------------------------------------
// The probe runs only when the root block device is a USB MSC disk. Two honest
// reasons: it is the configuration the feature is for (and the one real
// hardware and VM <vmid> both use), and it is the only one where the device
// CAPACITY can be read (usb_msc_device_t.num_blocks). drivers/ata.h exposes no
// capacity accessor at all, so on an ATA root there is no bound to check a
// volume's declared length against and no reliable channel/drive identity to
// re-issue reads with. Guessing either would be the bug. Adding an ATA capacity
// accessor is the one-line change that would lift this.
#ifndef DOS_USBVOL_H
#define DOS_USBVOL_H

// Property test of rustkern/usbvol.rs. Logs ONE [USBVOL] line to serial and
// /BOOTLOG. Proves the geometry rules hold on THIS build; it is not a
// differential (there is no C twin to compare against).
void usbvol_selftest(void);

// Probe the boot device's tail for data volumes and mount each one on a free
// CD drive letter. No-op, with one explanatory line, when the root is not USB
// or when the tail holds nothing. Call once at boot AFTER the root filesystem
// is mounted and after blk_root_to_ram(), from main.c next to
// diskimg_boot_harness().
//
// Bounded by construction: at most USBVOL_MAX candidates are probed, and the
// chain walk is required to make strictly positive progress at every step
// (rustkern/usbvol.rs), so a corrupt or hostile length field terminates the
// scan instead of hanging it.
void usbvol_probe_and_mount(void);

// Drive-letter index the first volume was mounted on, or -1 if none. Lets a
// harness or the DOS launcher name the volume without re-deriving the policy.
int usbvol_first_letter_idx(void);

#endif // DOS_USBVOL_H
