// rustkern/instdisk.rs - #306: enumerate the disks the installer may install TO.
//
// The installer used to scan four ATA slots and require ATA_TYPE_ATA, so AHCI
// and USB disks were invisible. On the iMac14,4 target - which has no legacy
// IDE at all - that means it found NO disk and the installer could not be used
// on the machine it exists for.
//
// New kernel code, so Rust per the standing directive. This is pure enumeration
// logic over three existing C driver APIs: no FPU, no paging, nothing that would
// justify C. The per-sector read/write dispatch stays in installer.c because it
// is three calls into those same driver APIs and lives with the clone loop.
//
// NVMe is deliberately ABSENT: there is no NVMe driver in this kernel (no
// kernel/drivers/nvme*), so claiming NVMe targets would be a lie.

use core::ffi::c_void;

/// Upper bound on tracked identities. Mirrors INST_MAX_TARGETS in installer.h;
/// the enumerator never writes more than `max` entries anyway, and this array
/// is only a dedup scratchpad.
const INST_MAX: usize = 16;


pub const INST_KIND_ATA: u8 = 0;
pub const INST_KIND_AHCI: u8 = 1;
pub const INST_KIND_USB: u8 = 2;

// Mirrors inst_target_t in installer.h; sizeof-locked there by _Static_assert.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct InstTarget {
    pub kind: u8,        // INST_KIND_*
    pub index: u8,       // ATA: (channel<<1)|drive.  AHCI: port.  USB: device idx
    pub is_boot: u8,     // 1 = this is the disk we booted from; never installable
    pub _pad: u8,
    pub sectors: u64,    // capacity in 512-byte sectors
}

extern "C" {
    // ATA
    fn ata_get_drive(channel: u8, drive: u8) -> *mut c_void;
    fn inst_ata_exists(channel: u8, drive: u8) -> i32;      // thin C accessor
    fn inst_ata_sectors(channel: u8, drive: u8) -> u64;
    fn inst_ata_serial(channel: u8, drive: u8, out: *mut u8);
    // AHCI
    fn ahci_get_port_count() -> i32;
    fn ahci_get_sector_count(port: i32) -> u64;
    fn inst_ahci_serial(port: i32, out: *mut u8);
    fn inst_ahci_port_mapped(port: i32) -> i32;
    // kprintf, so a dedup DECISION is visible on serial rather than inferred
    fn kprintf(fmt: *const u8, ...);
    // USB mass storage
    fn usb_msc_get_device_count() -> i32;
    fn inst_usb_sectors(index: i32) -> u64;                 // thin C accessor
    // Which device did we boot from?
    fn inst_boot_kind() -> i32;
    fn inst_boot_index() -> i32;
}

/// Fill `out` with every disk that could be installed to, marking the boot disk.
/// Returns the count written, or -1 on a bad argument.
///
/// A disk with zero sectors is skipped rather than reported as a 0-byte target:
/// an unreadable capacity means we cannot size the partitions, so offering it
/// would produce a "target disk is too small" failure at the worst moment.
#[no_mangle]
pub extern "C" fn inst_enumerate_targets(out: *mut InstTarget, max: i32) -> i32 {
    if out.is_null() || max <= 0 {
        return -1;
    }
    let cap = max as usize;
    let mut n: usize = 0;

    let bkind = unsafe { inst_boot_kind() } as u8;
    let bindex = unsafe { inst_boot_index() } as u8;

    // DEDUP, structurally rather than heuristically.
    //
    // ata.c maps every AHCI SATA disk into its own 4-slot disk table, so
    // ata_read_sectors/ata_write_sectors on that slot already go through the
    // AHCI backend. One physical disk was therefore enumerated TWICE: once via
    // that disk layer (kind=ATA) and once as a raw AHCI port (kind=AHCI).
    //
    // An earlier attempt deduped on IDENTIFY serial and preferred the RAW AHCI
    // view. That was wrong in a way only a real install revealed: raw
    // ahci_write() fails where the disk layer succeeds, so preferring it turned
    // a working install into "failed to write primary GPT header" (rc=-8). The
    // disk layer is the route every other disk I/O in this kernel takes, and it
    // is the route the verified install used, so it is the one to keep.
    //
    // Structural, not heuristic: a port is a duplicate iff the disk layer has
    // claimed it. No serial comparison, no capacity guessing, no way for two
    // genuinely distinct drives to be merged.

    // --- disk layer (ATA slots, which already cover AHCI SATA) -------------
    let mut ch: u8 = 0;
    while ch < 2 && n < cap {
        let mut u: u8 = 0;
        while u < 2 && n < cap {
            let exists = unsafe { inst_ata_exists(ch, u) };
            if exists != 0 {
                let secs = unsafe { inst_ata_sectors(ch, u) };
                if secs > 0 {
                    let idx = (ch << 1) | u;
                    unsafe {
                        *out.add(n) = InstTarget {
                            kind: INST_KIND_ATA,
                            index: idx,
                            is_boot: (bkind == INST_KIND_ATA && bindex == idx) as u8,
                            _pad: 0,
                            sectors: secs,
                        };
                    }
                    n += 1;
                }
            }
            u += 1;
        }
        ch += 1;
    }

    // --- raw AHCI ports the disk layer did NOT claim ----------------------
    let ports = unsafe { ahci_get_port_count() };
    let mut p: i32 = 0;
    while p < ports && n < cap {
        let secs = unsafe { ahci_get_sector_count(p) };
        if secs > 0 {
            let slot = unsafe { inst_ahci_port_mapped(p) };
            if slot >= 0 {
                unsafe {
                    kprintf(b"[INSTDISK] AHCI port %d is disk-layer slot %d; not listing it twice\n\0".as_ptr(),
                            p, slot);
                }
            } else {
                unsafe {
                    *out.add(n) = InstTarget {
                        kind: INST_KIND_AHCI,
                        index: p as u8,
                        is_boot: (bkind == INST_KIND_AHCI && bindex == p as u8) as u8,
                        _pad: 0,
                        sectors: secs,
                    };
                }
                n += 1;
            }
        }
        p += 1;
    }

    // --- USB mass storage --------------------------------------------------
    let devs = unsafe { usb_msc_get_device_count() };
    let mut d: i32 = 0;
    while d < devs && n < cap {
        let secs = unsafe { inst_usb_sectors(d) };
        if secs > 0 {
            unsafe {
                *out.add(n) = InstTarget {
                    kind: INST_KIND_USB,
                    index: d as u8,
                    is_boot: (bkind == INST_KIND_USB && bindex == d as u8) as u8,
                    _pad: 0,
                    sectors: secs,
                };
            }
            n += 1;
        }
        d += 1;
    }

    let _ = ata_get_drive; // referenced so the extern block stays honest
    n as i32
}

/// Is this target safe to install onto? Refuses the boot disk explicitly rather
/// than leaving that to the caller: installing onto the disk we are reading the
/// source from would corrupt the source mid-clone.
#[no_mangle]
pub extern "C" fn inst_target_installable(t: *const InstTarget, min_sectors: u64) -> i32 {
    if t.is_null() {
        return 0;
    }
    let t = unsafe { &*t };
    if t.is_boot != 0 {
        return 0;
    }
    if t.sectors < min_sectors {
        return 0;
    }
    1
}
