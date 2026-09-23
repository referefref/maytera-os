// rustkern/blkmgr.rs - Disk Manager kernel floor: Stages 0-2 (#404 disk-mgr).
//
// NEW kernel code, so Rust per the 2026-07-16 rule. There is no C twin and no
// strangler flag: none of these syscalls existed before this change, so there
// is nothing to strangle (same footing as getrandom.rs). The only C touched is
// (a) de-static'ing installer.c's proven per-kind sector dispatch so this Rust
// can reuse it (the DMA stays in the ata/ahci/usb hot paths, exactly the
// entanglement instdisk.rs already cites for keeping that dispatch in C), and
// (b) a one-line euid accessor + a marker-gated boot-harness call in main.c
// (the getrandom_selftest_rs / diskimg_boot_harness idiom).
//
// WHY A DISK MANAGER FLOOR NEEDS ITS OWN MODULE, and what each stage is:
//   Stage 0  - a SYNTHETIC RAM-backed scratch block device (kind BLK_KIND_SCRATCH)
//              so every destructive test (raw write, partition-table write, and
//              later mkfs) lands on scratch, NEVER on a real disk. Plus a
//              marker-gated boot oracle (blkmgr_selftest_rs) that proves the
//              round-trip AND that the refusals FIRE (not merely compile, #514).
//   Stage 1  - SYS_BLK_ENUM (one unified device list) + gated SYS_BLK_READ /
//              SYS_BLK_WRITE raw-LBA sector I/O to Ring 3.
//   Stage 2  - SYS_PART_PREPARE / SYS_PART_WRITE: two-phase GPT partition-table
//              write. PREPARE returns a nonce bound to (device + proposed-table
//              hash + current-on-disk-table hash); APPLY must echo that nonce and
//              re-present the identical layout or it is refused, so a UI mis-click
//              or a stale/forged request cannot silently repartition.
//
// THE SAFETY MODEL, AS BUILT (all enforced HERE, in the kernel, never trusting
// the Ring-3 descriptor - the installer precedent, sys_inst_install):
//   1. ROOT ONLY. Every mutating syscall checks euid == 0 via syscall_caller_euid().
//   2. BOOT-DISK LOCKOUT, STRUCTURAL. The caller passes only (kind,index); the
//      kernel re-resolves the device and re-derives is_boot from inst_boot_kind()/
//      inst_boot_index() ITSELF. A write to the boot disk returns BLKMGR_E_BOOTDISK
//      BEFORE any device call and BEFORE the user payload is even read. The
//      mounted root + ESP ARE the boot disk, so this covers them.
//   3. BUSY REFUSAL. A USB target with open handles (hotplug_vol_busy) is refused
//      (BLKMGR_E_BUSY). HONEST LIMIT: "mounted but idle removable" is only coarsely
//      detectable without a general mount table (which does not exist yet, Stage 4),
//      so this refuses open-handle busy, not every auto-mount. The load-bearing
//      property - the boot/system disk can never be written - is structural above.
//   4. BOUNDS. lba + count is checked against the resolved device size, and lba
//      must fit u32 (the installer's per-kind dispatch takes a u32 LBA).
//   5. TWO-PHASE NONCE for partition writes (Stage 2), described at the syscalls.
//
// core + alloc: alloc is NOT available in this crate, so buffers come from the C
// kmalloc/kfree (the ONE shared heap) exactly like the rest of the kernel, and
// the scratch device's backing store is a kmalloc region whose address lives in
// an atomic here (the blkstage.rs "the address lives in Rust" pattern).

use core::ffi::c_void;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

extern "C" {
    fn kmalloc(size: usize) -> *mut c_void;
    fn kprintf(fmt: *const u8, ...);
    // crypto/csprng.c (also feeds SYS_GETRANDOM). Never blocks.
    fn csprng_bytes(buf: *mut c_void, len: usize);
    // security/validate.c: atomic entry-check-AND-copy with an exception-table
    // fixup; non-zero return means a fault was contained.
    fn copy_to_user(dest: *mut c_void, src: *const c_void, size: usize) -> i32;
    fn copy_from_user(dest: *mut c_void, src: *const c_void, size: usize) -> i32;
    // proc/syscall.c (appended accessor): current caller euid, or 0xFFFFFFFF if
    // there is no current process.
    fn syscall_caller_euid() -> u32;

    // Boot-disk identity (gui/installer.c). kind: 2=USB else 0=ATA; index the
    // 0-3 ATA slot or the USB device index.
    fn inst_boot_kind() -> i32;
    fn inst_boot_index() -> i32;

    // Per-kind sector dispatch, de-static'd from installer.c. Route into the
    // exact ata_dma/ahci/usb_msc calls the verified installer clone uses. LBA is
    // u32, count is u8 (<=255); return is sectors transferred (>0) or <=0.
    fn inst_kind_read(kind: u8, index: u8, lba: u32, cnt: u8, buf: *mut c_void) -> i32;
    fn inst_kind_write(kind: u8, index: u8, lba: u32, cnt: u8, buf: *const c_void) -> i32;

    // Capacity + existence accessors (gui/installer.c; also used by instdisk.rs).
    fn inst_ata_exists(channel: u8, drive: u8) -> i32;
    fn inst_ata_sectors(channel: u8, drive: u8) -> u64;
    fn inst_ata_serial(channel: u8, drive: u8, out: *mut u8);
    fn ahci_get_port_count() -> i32;
    fn ahci_get_sector_count(port: i32) -> u64;
    fn inst_ahci_port_mapped(port: i32) -> i32;
    fn inst_ahci_serial(port: i32, out: *mut u8);
    fn usb_msc_get_device_count() -> i32;
    fn inst_usb_sectors(index: i32) -> u64;

    // Removable open-handle busy count (drivers/hotplug.c). 0 = not busy.
    fn hotplug_vol_busy(index: i32) -> i32;
}

// Device kinds. 0/1/2 mirror INST_KIND_* (installer.h); 3 is the scratch device
// this module adds. Kept in sync with BLK_KIND_* in fs/blkmgr.h.
const KIND_ATA: u8 = 0;
const KIND_AHCI: u8 = 1;
const KIND_USB: u8 = 2;
const KIND_SCRATCH: u8 = 3;

const SECTOR: usize = 512;

// Per-call sector caps. Small on purpose: partition tables are tens of sectors,
// and a bounded per-call transfer keeps the kmalloc bounce and the work an
// attacker-chosen count can demand both fixed. Ring 3 loops for more.
const BLK_IO_MAX_SECTORS: u32 = 256; // 128 KiB per SYS_BLK_READ/WRITE call
const PART_MAX: usize = 128; // GPT NumberOfPartitionEntries
const ENUM_CAP: usize = 32; // upper bound on devices returned by SYS_BLK_ENUM

// Error codes. Negative. The "clean" errnos match the rest of the tree; the
// -70x sentinels are DISTINCT so an oracle can assert exactly WHICH refusal
// fired rather than lumping every failure into -1 (the #514/#665 "watch it go
// red" discipline: a refusal that cannot be told apart from a generic error is
// a refusal nobody has actually seen fire).
const E_PERM: i64 = -1;
const E_FAULT: i64 = -14;
const E_INVAL: i64 = -22;
const BLKMGR_E_BOOTDISK: i64 = -700;
const BLKMGR_E_BUSY: i64 = -701;
const BLKMGR_E_NONCE: i64 = -702;
const BLKMGR_E_LAYOUT: i64 = -703;
const BLKMGR_E_DEVCHANGED: i64 = -704;
const BLKMGR_E_RANGE: i64 = -705;
const BLKMGR_E_NODEV: i64 = -706;

// ---------------------------------------------------------------------------
// Struct byte sizes locked against fs/blkmgr.h by _Static_assert AND against
// the argtab SZ_* constants. If any of these drifts the build fails loudly.
// ---------------------------------------------------------------------------
// blk_dev_t (56 bytes)
#[repr(C)]
#[derive(Clone, Copy)]
struct BlkDev {
    kind: u8,
    index: u8,
    is_boot: u8,
    is_removable: u8,
    sector_size: u32,
    sectors: u64,
    model: [u8; 40],
}

// part_spec_t (72 bytes) - one proposed partition.
#[repr(C)]
#[derive(Clone, Copy)]
struct PartSpec {
    type_guid: [u8; 16],
    start_lba: u64,
    size_lba: u64,
    name: [u8; 40],
}

// part_token_t (40 bytes) - the PREPARE result echoed back to APPLY.
#[repr(C)]
#[derive(Clone, Copy)]
struct PartToken {
    nonce: [u8; 16],
    layout_hash: u64,
    cur_hash: u64,
    dev_sectors: u64,
}

// inst_target_t (16 bytes) mirror, so inst_enumerate_targets can be reused by
// extern without importing instdisk's Rust type.
#[repr(C)]
#[derive(Clone, Copy)]
struct InstTgt {
    kind: u8,
    index: u8,
    is_boot: u8,
    _pad: u8,
    sectors: u64,
}
extern "C" {
    fn inst_enumerate_targets(out: *mut InstTgt, max: i32) -> i32;
}

// ---------------------------------------------------------------------------
// Stage 0: the synthetic scratch block device.
//
// A kmalloc region whose base address lives ONLY in these atomics (the
// blkstage.rs "the address lives in Rust" shape). Present iff SCRATCH_BASE != 0.
// It is initialised ONLY behind the /DISKMGR.TST marker (see the boot harness),
// so a normal golden never allocates it and never exposes a scratch device.
// ---------------------------------------------------------------------------
static SCRATCH_BASE: AtomicU64 = AtomicU64::new(0);
static SCRATCH_SECTORS: AtomicU32 = AtomicU32::new(0);

fn scratch_base() -> u64 {
    SCRATCH_BASE.load(Ordering::Acquire)
}
fn scratch_sectors() -> u64 {
    SCRATCH_SECTORS.load(Ordering::Acquire) as u64
}

/// Allocate the scratch device with `sectors` 512-byte sectors and zero it.
/// Idempotent: a second call with the device already present returns 0 without
/// reallocating. Returns 0 on success, negative on failure. Called ONCE from the
/// marker-gated boot harness; never from a syscall.
#[no_mangle]
pub extern "C" fn blkmgr_scratch_init_rs(sectors: u32) -> i32 {
    if scratch_base() != 0 {
        return 0;
    }
    if sectors == 0 || sectors as usize > (64 * 1024 * 1024) / SECTOR {
        return -1; // cap at 64 MiB so a bad marker cannot exhaust the heap
    }
    let bytes = sectors as usize * SECTOR;
    let p = unsafe { kmalloc(bytes) };
    if p.is_null() {
        return -1;
    }
    // SAFETY: kmalloc returned `bytes` writable bytes; we own them exclusively.
    unsafe { core::ptr::write_bytes(p as *mut u8, 0, bytes) };
    SCRATCH_SECTORS.store(sectors, Ordering::Release);
    SCRATCH_BASE.store(p as u64, Ordering::Release);
    0
}

/// 1 if the scratch device is present, else 0.
#[no_mangle]
pub extern "C" fn blkmgr_scratch_present_rs() -> i32 {
    (scratch_base() != 0) as i32
}

// ---------------------------------------------------------------------------
// Device resolution + write policy. The single place that decides what a
// (kind,index) really is and whether a write may touch it.
// ---------------------------------------------------------------------------
struct Resolved {
    sectors: u64,
    is_boot: bool,
    is_scratch: bool,
}

fn resolve(kind: u8, index: u8) -> Option<Resolved> {
    if kind == KIND_SCRATCH {
        let s = scratch_sectors();
        if scratch_base() == 0 || s == 0 {
            return None;
        }
        return Some(Resolved { sectors: s, is_boot: false, is_scratch: true });
    }
    let bkind = unsafe { inst_boot_kind() } as i64;
    let bindex = unsafe { inst_boot_index() } as i64;
    let is_boot = (bkind == kind as i64) && (bindex == index as i64);
    let sectors: u64 = match kind {
        KIND_ATA => {
            let ch = index >> 1;
            let u = index & 1;
            if index > 3 || unsafe { inst_ata_exists(ch, u) } == 0 {
                return None;
            }
            unsafe { inst_ata_sectors(ch, u) }
        }
        KIND_AHCI => {
            let ports = unsafe { ahci_get_port_count() };
            if (index as i32) >= ports {
                return None;
            }
            // A port the disk layer already claimed is addressed as ATA, not
            // AHCI (matches inst_enumerate_targets dedup). Refuse the raw view.
            if unsafe { inst_ahci_port_mapped(index as i32) } >= 0 {
                return None;
            }
            unsafe { ahci_get_sector_count(index as i32) }
        }
        KIND_USB => {
            let devs = unsafe { usb_msc_get_device_count() };
            if (index as i32) >= devs {
                return None;
            }
            unsafe { inst_usb_sectors(index as i32) }
        }
        _ => return None,
    };
    if sectors == 0 {
        return None;
    }
    Some(Resolved { sectors, is_boot, is_scratch: false })
}

/// 0 = a write may proceed; a negative sentinel = refused (and why). This is the
/// load-bearing safety gate and is called FIRST on every write path, before any
/// device I/O and before the user payload is read.
fn write_permitted(r: &Resolved, kind: u8, index: u8) -> i64 {
    if r.is_scratch {
        return 0;
    }
    if r.is_boot {
        return BLKMGR_E_BOOTDISK;
    }
    if kind == KIND_USB && unsafe { hotplug_vol_busy(index as i32) } > 0 {
        return BLKMGR_E_BUSY;
    }
    0
}

// ---------------------------------------------------------------------------
// Core sector I/O on a KERNEL buffer. `write == false` reads into kbuf, `true`
// writes from kbuf. Returns sectors transferred (>0) or a negative sentinel.
// Used by both the syscall wrappers (with a kmalloc bounce) and the oracle
// (with kernel buffers), so both exercise the SAME safety gate.
// ---------------------------------------------------------------------------
fn blk_rw_core(kind: u8, index: u8, lba: u64, count: u32, kbuf: *mut u8, write: bool) -> i64 {
    if count == 0 {
        return 0;
    }
    if count > BLK_IO_MAX_SECTORS {
        return E_INVAL;
    }
    let r = match resolve(kind, index) {
        Some(r) => r,
        None => return BLKMGR_E_NODEV,
    };
    // Bounds: lba must fit u32 (installer dispatch takes u32) and lba+count must
    // lie inside the device. checked_add so the bound itself cannot wrap.
    let end = match lba.checked_add(count as u64) {
        Some(v) => v,
        None => return BLKMGR_E_RANGE,
    };
    if lba > u32::MAX as u64 || end > r.sectors {
        return BLKMGR_E_RANGE;
    }
    if write {
        let perm = write_permitted(&r, kind, index);
        if perm != 0 {
            return perm; // refused before ANY byte is written
        }
    }
    if r.is_scratch {
        let base = scratch_base() as *mut u8;
        if base.is_null() {
            return BLKMGR_E_NODEV;
        }
        let off = lba as usize * SECTOR;
        let n = count as usize * SECTOR;
        // SAFETY: bounds above proved [lba, lba+count) inside the device, so
        // [off, off+n) is inside the kmalloc'd scratch region. kbuf is the
        // caller's buffer of at least n bytes. The two never alias (scratch vs a
        // separate bounce/oracle buffer), so copy_nonoverlapping is sound.
        unsafe {
            let dev = base.add(off);
            if write {
                core::ptr::copy_nonoverlapping(kbuf, dev, n);
            } else {
                core::ptr::copy_nonoverlapping(dev, kbuf, n);
            }
        }
        return count as i64;
    }
    // Real device: dispatch in <=255-sector chunks (installer dispatch is u8 cnt).
    let mut done: u32 = 0;
    while done < count {
        let chunk = core::cmp::min(count - done, 255) as u8;
        let this_lba = (lba as u32).wrapping_add(done);
        let ptr = unsafe { kbuf.add(done as usize * SECTOR) } as *mut c_void;
        let rc = if write {
            unsafe { inst_kind_write(kind, index, this_lba, chunk, ptr as *const c_void) }
        } else {
            unsafe { inst_kind_read(kind, index, this_lba, chunk, ptr) }
        };
        if rc != chunk as i32 {
            return if done == 0 { -1 } else { done as i64 };
        }
        done += chunk as u32;
    }
    count as i64
}

// ---------------------------------------------------------------------------
// FNV-1a 64 fold. Used for the layout hash and the current-on-disk-table hash
// that bind a PREPARE nonce. This is anti-mis-click / anti-stale binding for an
// already-authenticated root caller, NOT a cryptographic adversary defence, so a
// non-cryptographic fold is the right tool (and needs no float, no table).
// ---------------------------------------------------------------------------
fn fnv1a(seed: u64, bytes: &[u8]) -> u64 {
    let mut h = seed;
    for &b in bytes {
        h ^= b as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01B3);
    }
    h
}

fn layout_hash(specs: &[PartSpec]) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    h = fnv1a(h, &(specs.len() as u64).to_le_bytes());
    for s in specs {
        h = fnv1a(h, &s.type_guid);
        h = fnv1a(h, &s.start_lba.to_le_bytes());
        h = fnv1a(h, &s.size_lba.to_le_bytes());
        h = fnv1a(h, &s.name);
    }
    h
}

/// Fingerprint the current on-disk partition table (LBA 0,1,2). Reads only; a
/// blank disk hashes to the fingerprint of whatever is there (often zeros),
/// which is still a stable value to detect "the disk changed under the UI".
fn current_table_hash(kind: u8, index: u8) -> u64 {
    let mut buf = [0u8; SECTOR * 3];
    let r = blk_rw_core(kind, index, 0, 3, buf.as_mut_ptr(), false);
    if r != 3 {
        // Cannot read: hash the failure so a later PREPARE/APPLY still compares
        // like-with-like (both will fail to read identically), rather than
        // pretending success.
        return 0xdead_0000_0000_0000 ^ (r as u64);
    }
    fnv1a(0xcbf2_9ce4_8422_2325, &buf)
}

// ---------------------------------------------------------------------------
// Stage 2: GPT serialisation. Generalises gui/installer.c's inst_build_gpt_header
// (which could only emit the fixed 2-partition clone) to an arbitrary layout.
// It lives HERE, not in parttbl.rs, because parttbl.rs is explicitly PARSE ONLY
// (its own header) and treats the table as UNTRUSTED input; a writer belongs
// with the rest of the Disk Manager's mutating logic. The bytes it emits are
// verified by re-parsing them with parttbl's parser in the oracle.
// ---------------------------------------------------------------------------
fn crc32(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &b in data {
        crc ^= b as u32;
        let mut i = 0;
        while i < 8 {
            let m = (crc & 1).wrapping_neg();
            crc = (crc >> 1) ^ (0xEDB8_8720 & m);
            i += 1;
        }
    }
    !crc
}

fn put_le32(b: &mut [u8], off: usize, v: u32) {
    b[off..off + 4].copy_from_slice(&v.to_le_bytes());
}
fn put_le64(b: &mut [u8], off: usize, v: u64) {
    b[off..off + 8].copy_from_slice(&v.to_le_bytes());
}

fn build_protective_mbr(disk_sectors: u64, sec: &mut [u8; SECTOR]) {
    for x in sec.iter_mut() {
        *x = 0;
    }
    // One 0xEE partition covering the disk, per UEFI protective-MBR rules.
    let p = 446;
    sec[p] = 0x00; // not bootable
    sec[p + 1] = 0x00;
    sec[p + 2] = 0x02;
    sec[p + 3] = 0x00; // CHS start
    sec[p + 4] = 0xEE; // type: GPT protective
    sec[p + 5] = 0xFF;
    sec[p + 6] = 0xFF;
    sec[p + 7] = 0xFF; // CHS end
    put_le32(sec, p + 8, 1); // StartingLBA
    let span = if disk_sectors - 1 > 0xFFFF_FFFF { 0xFFFF_FFFF } else { (disk_sectors - 1) as u32 };
    put_le32(sec, p + 12, span);
    sec[510] = 0x55;
    sec[511] = 0xAA;
}

fn build_gpt_header(
    sec: &mut [u8; SECTOR],
    disk_sectors: u64,
    my_lba: u64,
    alt_lba: u64,
    entry_lba: u64,
    parr_crc: u32,
    disk_guid: &[u8; 16],
) {
    for x in sec.iter_mut() {
        *x = 0;
    }
    sec[0..8].copy_from_slice(b"EFI PART");
    put_le32(sec, 8, 0x0001_0000); // revision 1.0
    put_le32(sec, 12, 92); // header size
    put_le32(sec, 16, 0); // header CRC (computed last)
    put_le32(sec, 20, 0); // reserved
    put_le64(sec, 24, my_lba);
    put_le64(sec, 32, alt_lba);
    put_le64(sec, 40, 34); // FirstUsableLBA
    put_le64(sec, 48, disk_sectors - 34); // LastUsableLBA
    sec[56..72].copy_from_slice(disk_guid);
    put_le64(sec, 72, entry_lba);
    put_le32(sec, 80, PART_MAX as u32); // NumberOfPartitionEntries
    put_le32(sec, 84, 128); // SizeOfPartitionEntry
    put_le32(sec, 88, parr_crc);
    let hc = crc32(&sec[0..92]);
    put_le32(sec, 16, hc);
}

/// Fill a >=16384-byte entry array from `specs` and return its CRC32. Each entry
/// gets a fresh UniquePartitionGUID from the CSPRNG.
fn build_entry_array(specs: &[PartSpec], arr: &mut [u8]) -> u32 {
    for x in arr[..PART_MAX * 128].iter_mut() {
        *x = 0;
    }
    for (i, s) in specs.iter().enumerate() {
        if i >= PART_MAX {
            break;
        }
        let e = i * 128;
        arr[e..e + 16].copy_from_slice(&s.type_guid);
        // UniquePartitionGUID from CSPRNG.
        let mut ug = [0u8; 16];
        unsafe { csprng_bytes(ug.as_mut_ptr() as *mut c_void, 16) };
        arr[e + 16..e + 32].copy_from_slice(&ug);
        let last = s.start_lba + s.size_lba - 1;
        put_le64(arr, e + 32, s.start_lba);
        put_le64(arr, e + 40, last);
        put_le64(arr, e + 48, 0); // attributes
        // PartitionName: ASCII name[] -> UTF-16LE, up to 36 units.
        let mut k = 0usize;
        while k < 36 && k < s.name.len() && s.name[k] != 0 {
            arr[e + 56 + k * 2] = s.name[k];
            arr[e + 56 + k * 2 + 1] = 0;
            k += 1;
        }
    }
    crc32(&arr[..PART_MAX * 128])
}

/// Validate a proposed layout against a disk of `disk_sectors`. Returns 0 if the
/// layout is well-formed (in bounds, non-empty, non-overlapping, ascending), or
/// a negative errno. A bad layout must never reach the writer.
fn validate_layout(specs: &[PartSpec], disk_sectors: u64) -> i64 {
    if specs.is_empty() || specs.len() > PART_MAX {
        return E_INVAL;
    }
    let first_usable: u64 = 34;
    let last_usable: u64 = disk_sectors.saturating_sub(34);
    let mut prev_end: u64 = first_usable;
    for s in specs {
        if s.size_lba == 0 {
            return E_INVAL;
        }
        let end = match s.start_lba.checked_add(s.size_lba) {
            Some(v) => v,
            None => return E_INVAL,
        };
        if s.start_lba < prev_end || end - 1 > last_usable {
            return E_INVAL; // overlap, below usable, or past usable
        }
        // A partition of all-zero type GUID is "unused"; reject in a layout.
        if s.type_guid.iter().all(|&b| b == 0) {
            return E_INVAL;
        }
        prev_end = end;
    }
    0
}

/// Write the full GPT (protective MBR + primary header + primary entries +
/// backup entries + backup header) for `specs` onto (kind,index), routing every
/// sector write through blk_rw_core (so the same boot-disk lockout applies).
/// Returns 0 on success or a negative errno.
fn write_gpt(kind: u8, index: u8, disk_sectors: u64, specs: &[PartSpec]) -> i64 {
    if disk_sectors < 96 {
        return BLKMGR_E_RANGE; // too small to hold primary+backup GPT structures
    }
    // 16 KiB entry array from the shared heap (too large for the kernel stack).
    let arr = unsafe { kmalloc(PART_MAX * 128) } as *mut u8;
    if arr.is_null() {
        return E_FAULT;
    }
    let ret = (|| -> i64 {
        // SAFETY: kmalloc gave PART_MAX*128 writable bytes we own exclusively.
        let arr_slice = unsafe { core::slice::from_raw_parts_mut(arr, PART_MAX * 128) };
        let parr_crc = build_entry_array(specs, arr_slice);

        let mut disk_guid = [0u8; 16];
        unsafe { csprng_bytes(disk_guid.as_mut_ptr() as *mut c_void, 16) };

        let entry_sectors = (PART_MAX * 128 / SECTOR) as u64; // 32
        let primary_entry_lba: u64 = 2;
        let backup_entry_lba: u64 = disk_sectors - 1 - entry_sectors; // disk-33
        let primary_hdr_lba: u64 = 1;
        let backup_hdr_lba: u64 = disk_sectors - 1;

        // Protective MBR (LBA 0).
        let mut mbr = [0u8; SECTOR];
        build_protective_mbr(disk_sectors, &mut mbr);
        if blk_rw_core(kind, index, 0, 1, mbr.as_mut_ptr(), true) != 1 {
            return -1;
        }
        // Primary + backup entry arrays.
        if blk_rw_core(kind, index, primary_entry_lba, entry_sectors as u32, arr, true) != entry_sectors as i64 {
            return -1;
        }
        if blk_rw_core(kind, index, backup_entry_lba, entry_sectors as u32, arr, true) != entry_sectors as i64 {
            return -1;
        }
        // Primary header (LBA 1).
        let mut hdr = [0u8; SECTOR];
        build_gpt_header(&mut hdr, disk_sectors, primary_hdr_lba, backup_hdr_lba, primary_entry_lba, parr_crc, &disk_guid);
        if blk_rw_core(kind, index, primary_hdr_lba, 1, hdr.as_mut_ptr(), true) != 1 {
            return -1;
        }
        // Backup header (last LBA).
        let mut bhdr = [0u8; SECTOR];
        build_gpt_header(&mut bhdr, disk_sectors, backup_hdr_lba, primary_hdr_lba, backup_entry_lba, parr_crc, &disk_guid);
        if blk_rw_core(kind, index, backup_hdr_lba, 1, bhdr.as_mut_ptr(), true) != 1 {
            return -1;
        }
        0
    })();
    unsafe { kfree(arr as *mut c_void) };
    ret
}

extern "C" {
    fn kfree(p: *mut c_void);
}

// ---------------------------------------------------------------------------
// Stage 2: the two-phase nonce token. ONE slot is enough: a repartition is a
// deliberate, serialised, human-driven act, and syscall bodies run under the
// BKL so there is never concurrent access to this slot. A second PREPARE simply
// supersedes the first (its nonce becomes the only valid one).
// ---------------------------------------------------------------------------
static TOK_VALID: AtomicU32 = AtomicU32::new(0);
static TOK_N0: AtomicU64 = AtomicU64::new(0);
static TOK_N1: AtomicU64 = AtomicU64::new(0);
static TOK_KINDIDX: AtomicU32 = AtomicU32::new(0);
static TOK_LAYOUT: AtomicU64 = AtomicU64::new(0);
static TOK_CUR: AtomicU64 = AtomicU64::new(0);

fn nonce_pack(nonce: &[u8; 16]) -> (u64, u64) {
    let mut n0 = [0u8; 8];
    let mut n1 = [0u8; 8];
    n0.copy_from_slice(&nonce[0..8]);
    n1.copy_from_slice(&nonce[8..16]);
    (u64::from_le_bytes(n0), u64::from_le_bytes(n1))
}

/// PREPARE core (kernel buffers). Resolves the device, refuses the boot disk,
/// validates the layout, then mints a nonce bound to (device, layout hash,
/// current-table hash) and returns the token via `out`. Returns 0 or negative.
fn part_prepare_core(kind: u8, index: u8, specs: &[PartSpec], out: &mut PartToken) -> i64 {
    let r = match resolve(kind, index) {
        Some(r) => r,
        None => return BLKMGR_E_NODEV,
    };
    let perm = write_permitted(&r, kind, index);
    if perm != 0 {
        return perm;
    }
    let v = validate_layout(specs, r.sectors);
    if v != 0 {
        return v;
    }
    let lh = layout_hash(specs);
    let ch = current_table_hash(kind, index);
    let mut nonce = [0u8; 16];
    unsafe { csprng_bytes(nonce.as_mut_ptr() as *mut c_void, 16) };
    let (n0, n1) = nonce_pack(&nonce);

    TOK_VALID.store(0, Ordering::Release);
    TOK_N0.store(n0, Ordering::Release);
    TOK_N1.store(n1, Ordering::Release);
    TOK_KINDIDX.store(((kind as u32) << 8) | index as u32, Ordering::Release);
    TOK_LAYOUT.store(lh, Ordering::Release);
    TOK_CUR.store(ch, Ordering::Release);
    TOK_VALID.store(1, Ordering::Release);

    out.nonce = nonce;
    out.layout_hash = lh;
    out.cur_hash = ch;
    out.dev_sectors = r.sectors;
    0
}

/// APPLY core (kernel buffers). Requires a live token whose nonce, device and
/// layout hash all match, and whose bound current-table hash still matches the
/// disk NOW (so a disk changed since PREPARE is refused). Single use: the token
/// is consumed on any nonce-matching call. Returns 0 or negative.
fn part_write_core(kind: u8, index: u8, specs: &[PartSpec], nonce: &[u8; 16]) -> i64 {
    let (n0, n1) = nonce_pack(nonce);
    if TOK_VALID.load(Ordering::Acquire) == 0
        || TOK_N0.load(Ordering::Acquire) != n0
        || TOK_N1.load(Ordering::Acquire) != n1
    {
        return BLKMGR_E_NONCE;
    }
    // The nonce matched a live token: consume it now so it can never be replayed,
    // whatever the outcome below.
    TOK_VALID.store(0, Ordering::Release);

    if TOK_KINDIDX.load(Ordering::Acquire) != (((kind as u32) << 8) | index as u32) {
        return BLKMGR_E_NONCE; // nonce was for a different device
    }
    let r = match resolve(kind, index) {
        Some(r) => r,
        None => return BLKMGR_E_NODEV,
    };
    let perm = write_permitted(&r, kind, index);
    if perm != 0 {
        return perm;
    }
    if layout_hash(specs) != TOK_LAYOUT.load(Ordering::Acquire) {
        return BLKMGR_E_LAYOUT; // APPLY layout differs from the PREPARE'd one
    }
    if current_table_hash(kind, index) != TOK_CUR.load(Ordering::Acquire) {
        return BLKMGR_E_DEVCHANGED; // disk changed under the UI since PREPARE
    }
    let v = validate_layout(specs, r.sectors);
    if v != 0 {
        return v;
    }
    if write_gpt(kind, index, r.sectors, specs) != 0 {
        return -1;
    }
    1
}

// ===========================================================================
// SYSCALL WRAPPERS (Ring 3 entry points). Each does the root check, the user
// copies through copy_to_user/copy_from_user, and calls the *_core above so the
// safety gate is identical to the oracle's path.
// ===========================================================================

/// SYS_BLK_ENUM(buf, max, elem_size) -> count | negative. One unified list of
/// every block device (fixed ATA/AHCI + removable USB + scratch), with is_boot
/// and is_removable stamped by the kernel. Not root-gated: enumeration is
/// read-only and Settings/Files already expose the same facts.
#[no_mangle]
pub unsafe extern "C" fn blk_enum_rs(buf: *mut c_void, max: i32, elem_size: i32) -> i64 {
    if elem_size as usize != core::mem::size_of::<BlkDev>() {
        return E_INVAL; // stale userland build; refuse to misalign
    }
    if buf.is_null() || max <= 0 {
        return E_INVAL;
    }

    let mut list = [BlkDev {
        kind: 0,
        index: 0,
        is_boot: 0,
        is_removable: 0,
        sector_size: 0,
        sectors: 0,
        model: [0u8; 40],
    }; ENUM_CAP];
    let mut n = 0usize;

    // Fixed + removable via the unified installer enumerator (already dedups
    // AHCI-into-ATA and stamps is_boot).
    let mut tgts = [InstTgt { kind: 0, index: 0, is_boot: 0, _pad: 0, sectors: 0 }; 16];
    let tn = inst_enumerate_targets(tgts.as_mut_ptr(), 16);
    let mut i = 0i32;
    while i < tn && n < ENUM_CAP {
        let t = tgts[i as usize];
        let mut d = BlkDev {
            kind: t.kind,
            index: t.index,
            is_boot: t.is_boot,
            is_removable: (t.kind == KIND_USB) as u8,
            sector_size: SECTOR as u32,
            sectors: t.sectors,
            model: [0u8; 40],
        };
        fill_model(&mut d);
        list[n] = d;
        n += 1;
        i += 1;
    }

    // Scratch device, if present, always last.
    if scratch_base() != 0 && n < ENUM_CAP {
        let mut d = BlkDev {
            kind: KIND_SCRATCH,
            index: 0,
            is_boot: 0,
            is_removable: 0,
            sector_size: SECTOR as u32,
            sectors: scratch_sectors(),
            model: [0u8; 40],
        };
        let label = b"SCRATCH (RAM)";
        d.model[..label.len()].copy_from_slice(label);
        list[n] = d;
        n += 1;
    }

    let want = core::cmp::min(n, max as usize);
    let bytes = want * core::mem::size_of::<BlkDev>();
    if bytes > 0 && copy_to_user(buf, list.as_ptr() as *const c_void, bytes) != 0 {
        return E_FAULT;
    }
    want as i64
}

fn fill_model(d: &mut BlkDev) {
    match d.kind {
        KIND_ATA => {
            let ch = d.index >> 1;
            let u = d.index & 1;
            unsafe { inst_ata_serial(ch, u, d.model.as_mut_ptr()) };
        }
        KIND_AHCI => {
            unsafe { inst_ahci_serial(d.index as i32, d.model.as_mut_ptr()) };
        }
        KIND_USB => {
            // No IDENTIFY string accessor for USB MSC; a stable label is enough
            // for the picker and avoids inventing a serial.
            let label = b"USB Mass Storage";
            d.model[..label.len()].copy_from_slice(label);
        }
        _ => {}
    }
    // Guarantee NUL termination whatever the accessor wrote.
    d.model[39] = 0;
}

/// SYS_BLK_READ(kind, index, lba, count, buf) -> sectors | negative. Root only.
#[no_mangle]
pub unsafe extern "C" fn sys_blk_read_rs(kind: i32, index: i32, lba: u64, count: u32, buf: *mut c_void) -> i64 {
    if syscall_caller_euid() != 0 {
        return E_PERM;
    }
    if !(0..=255).contains(&kind) || !(0..=255).contains(&index) {
        return E_INVAL;
    }
    if count == 0 {
        return 0;
    }
    if count > BLK_IO_MAX_SECTORS {
        return E_INVAL;
    }
    if buf.is_null() {
        return E_INVAL;
    }
    let bytes = count as usize * SECTOR;
    let bounce = kmalloc(bytes) as *mut u8;
    if bounce.is_null() {
        return E_FAULT;
    }
    let r = blk_rw_core(kind as u8, index as u8, lba, count, bounce, false);
    if r < 0 {
        kfree(bounce as *mut c_void);
        return r;
    }
    let out_bytes = r as usize * SECTOR;
    let rc = if out_bytes > 0 { copy_to_user(buf, bounce as *const c_void, out_bytes) } else { 0 };
    kfree(bounce as *mut c_void);
    if rc != 0 {
        return E_FAULT;
    }
    r
}

/// SYS_BLK_WRITE(kind, index, lba, count, buf) -> sectors | negative. Root only.
/// The device is resolved and the boot-disk / busy check runs BEFORE the user
/// payload is read, so a refused write reads no user memory and touches no disk.
#[no_mangle]
pub unsafe extern "C" fn sys_blk_write_rs(kind: i32, index: i32, lba: u64, count: u32, buf: *const c_void) -> i64 {
    if syscall_caller_euid() != 0 {
        return E_PERM;
    }
    if !(0..=255).contains(&kind) || !(0..=255).contains(&index) {
        return E_INVAL;
    }
    if count == 0 {
        return 0;
    }
    if count > BLK_IO_MAX_SECTORS {
        return E_INVAL;
    }
    if buf.is_null() {
        return E_INVAL;
    }
    // Resolve + permission FIRST, before reading a byte of user memory.
    let r = match resolve(kind as u8, index as u8) {
        Some(r) => r,
        None => return BLKMGR_E_NODEV,
    };
    let perm = write_permitted(&r, kind as u8, index as u8);
    if perm != 0 {
        return perm;
    }
    let bytes = count as usize * SECTOR;
    let bounce = kmalloc(bytes) as *mut u8;
    if bounce.is_null() {
        return E_FAULT;
    }
    if copy_from_user(bounce as *mut c_void, buf, bytes) != 0 {
        kfree(bounce as *mut c_void);
        return E_FAULT;
    }
    let w = blk_rw_core(kind as u8, index as u8, lba, count, bounce, true);
    kfree(bounce as *mut c_void);
    w
}

/// #404 Stage 4b: KERNEL-BUFFER unified device I/O for the ext2 aux-mount driver
/// (fs/ext2.c e2_dev_read/e2_dev_write). This is the ONLY sector path that
/// reaches an AHCI/USB/RAM-scratch second volume, so the aux ext2 driver routes
/// here instead of blk_read/blk_write. It is NOT a syscall: `buf` is a kernel
/// pointer (no copy_from_user, no euid gate; the caller is already in Ring 0),
/// but it DOES run the same write_permitted() boot-disk lockout on writes, so an
/// aux write can never be steered onto the boot medium even by a caller bug.
/// Returns sectors transferred (>0) or a negative sentinel, matching blk_read.
#[no_mangle]
pub extern "C" fn blkmgr_dev_rw_c(kind: u8, index: u8, lba: u64, count: u32,
                                  buf: *mut c_void, write: i32) -> i64 {
    if buf.is_null() {
        return E_INVAL;
    }
    if write != 0 {
        let r = match resolve(kind, index) {
            Some(r) => r,
            None => return BLKMGR_E_NODEV,
        };
        let perm = write_permitted(&r, kind, index);
        if perm != 0 {
            return perm;
        }
    }
    blk_rw_core(kind, index, lba, count, buf as *mut u8, write != 0)
}

fn copy_specs_from_user(u_layout: *const c_void, nparts: i32, out: &mut [PartSpec; PART_MAX]) -> i64 {
    if nparts <= 0 || nparts as usize > PART_MAX {
        return E_INVAL;
    }
    let bytes = nparts as usize * core::mem::size_of::<PartSpec>();
    let rc = unsafe { copy_from_user(out.as_mut_ptr() as *mut c_void, u_layout, bytes) };
    if rc != 0 {
        return E_FAULT;
    }
    nparts as i64
}

/// SYS_PART_PREPARE(kind, index, layout, nparts, out_token) -> 0 | negative.
/// Root only. Mints the two-phase nonce; writes nothing.
#[no_mangle]
pub unsafe extern "C" fn sys_part_prepare_rs(
    kind: i32,
    index: i32,
    u_layout: *const c_void,
    nparts: i32,
    u_token: *mut c_void,
) -> i64 {
    if syscall_caller_euid() != 0 {
        return E_PERM;
    }
    if !(0..=255).contains(&kind) || !(0..=255).contains(&index) {
        return E_INVAL;
    }
    if u_layout.is_null() || u_token.is_null() {
        return E_INVAL;
    }
    let mut specs = [PartSpec { type_guid: [0; 16], start_lba: 0, size_lba: 0, name: [0; 40] }; PART_MAX];
    let cr = copy_specs_from_user(u_layout, nparts, &mut specs);
    if cr < 0 {
        return cr;
    }
    let mut tok = PartToken { nonce: [0; 16], layout_hash: 0, cur_hash: 0, dev_sectors: 0 };
    let r = part_prepare_core(kind as u8, index as u8, &specs[..nparts as usize], &mut tok);
    if r != 0 {
        return r;
    }
    if copy_to_user(u_token, &tok as *const PartToken as *const c_void, core::mem::size_of::<PartToken>()) != 0 {
        return E_FAULT;
    }
    0
}

/// SYS_PART_WRITE(kind, index, layout, nparts, nonce) -> 1 | negative. Root only.
/// The APPLY half: echoes the PREPARE nonce and re-presents the identical layout,
/// or it is refused. Writes only via the gated blk_rw_core path.
#[no_mangle]
pub unsafe extern "C" fn sys_part_write_rs(
    kind: i32,
    index: i32,
    u_layout: *const c_void,
    nparts: i32,
    u_nonce: *const c_void,
) -> i64 {
    if syscall_caller_euid() != 0 {
        return E_PERM;
    }
    if !(0..=255).contains(&kind) || !(0..=255).contains(&index) {
        return E_INVAL;
    }
    if u_layout.is_null() || u_nonce.is_null() {
        return E_INVAL;
    }
    let mut nonce = [0u8; 16];
    if copy_from_user(nonce.as_mut_ptr() as *mut c_void, u_nonce, 16) != 0 {
        return E_FAULT;
    }
    let mut specs = [PartSpec { type_guid: [0; 16], start_lba: 0, size_lba: 0, name: [0; 40] }; PART_MAX];
    let cr = copy_specs_from_user(u_layout, nparts, &mut specs);
    if cr < 0 {
        return cr;
    }
    part_write_core(kind as u8, index as u8, &specs[..nparts as usize], &nonce)
}

// ===========================================================================
// STAGE 0 ORACLE. Marker-gated boot self-test. Proves, on the SCRATCH device:
//  - raw sector round-trip,
//  - PREPARE/APPLY GPT round-trip (re-parsed with parttbl's own parser),
//  - the nonce refusals FIRE (wrong nonce, wrong layout, single-use replay),
//  - and that a WRITE aimed at the real BOOT disk is REFUSED and changes nothing.
// Returns 0 iff every check passed; writes passed/total to the out params.
// ===========================================================================

// parttbl parser (same crate). Re-declared as extern "C" so this file does not
// depend on parttbl's Rust type paths.
#[repr(C)]
struct GptHdr {
    ent_lba: u64,
    num: u32,
    esz: u32,
    per_sec: u32,
    _pad: u32,
}
#[repr(C)]
struct Scan {
    consumed: u32,
    found_lin: u32,
    lin_lba: u64,
}
extern "C" {
    fn parttbl_gpt_hdr_rs(sec: *const u8, len: u32, out: *mut GptHdr) -> i32;
    fn parttbl_gpt_sec_scan_rs(
        sec: *const u8,
        len: u32,
        esz: u32,
        per_sec: u32,
        remaining: u32,
        io_fallback: *mut u32,
        out: *mut Scan,
    ) -> i32;
}

// The Linux filesystem-data type GUID parttbl scans for (matches parttbl.rs
// GUID_LINUX). Used so the oracle's second partition is one parttbl will find.
const GUID_LINUX: [u8; 16] = [
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4,
];
// A distinct (non-ESP, non-Linux) type GUID for the first partition, so the
// layout has two clearly different entries. Value is arbitrary-but-fixed.
const GUID_DATA: [u8; 16] = [
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7,
];

fn spec(type_guid: [u8; 16], start: u64, size: u64, name: &[u8]) -> PartSpec {
    let mut s = PartSpec { type_guid, start_lba: start, size_lba: size, name: [0; 40] };
    let n = core::cmp::min(name.len(), 39);
    s.name[..n].copy_from_slice(&name[..n]);
    s
}

#[no_mangle]
pub extern "C" fn blkmgr_selftest_rs(out_passed: *mut u32, out_total: *mut u32) -> i32 {
    let mut passed: u32 = 0;
    let mut total: u32 = 0;
    macro_rules! check {
        ($cond:expr, $label:expr) => {{
            total += 1;
            if $cond {
                passed += 1;
            } else {
                unsafe {
                    kprintf(b"[DISKMGR] oracle FAIL: %s\n\0".as_ptr(), ($label).as_ptr());
                }
            }
        }};
    }

    if scratch_base() == 0 {
        unsafe { kprintf(b"[DISKMGR] oracle: scratch device absent, cannot run\n\0".as_ptr()) };
        if !out_passed.is_null() {
            unsafe { *out_passed = 0 };
        }
        if !out_total.is_null() {
            unsafe { *out_total = 1 };
        }
        return -1;
    }
    let sec = scratch_sectors();

    // --- (a) raw round-trip on scratch, away from LBA 0..2 ------------------
    let mut wbuf = [0u8; SECTOR * 4];
    let mut i = 0usize;
    while i < wbuf.len() {
        wbuf[i] = (0xA5u32.wrapping_add(i as u32) & 0xFF) as u8;
        i += 1;
    }
    let wr = blk_rw_core(KIND_SCRATCH, 0, 10, 4, wbuf.as_mut_ptr(), true);
    let mut rbuf = [0u8; SECTOR * 4];
    let rd = blk_rw_core(KIND_SCRATCH, 0, 10, 4, rbuf.as_mut_ptr(), false);
    check!(wr == 4 && rd == 4 && wbuf == rbuf, b"raw sector round-trip\0");

    // --- Layouts. A: two partitions; B: same but partition 2 shifted -------
    let esp_start: u64 = 2048;
    let esp_size: u64 = 1024;
    let lin_start_a: u64 = 4096;
    let lin_start_b: u64 = 4608; // differs from A -> different layout hash
    let lin_size: u64 = sec.saturating_sub(4096 + 64); // leave backup GPT room
    let layout_a = [
        spec(GUID_DATA, esp_start, esp_size, b"DATA"),
        spec(GUID_LINUX, lin_start_a, lin_size.saturating_sub(0), b"ROOT"),
    ];
    let layout_b = [
        spec(GUID_DATA, esp_start, esp_size, b"DATA"),
        spec(GUID_LINUX, lin_start_b, lin_size.saturating_sub(512), b"ROOT"),
    ];

    // --- (b) PREPARE mints a non-zero nonce --------------------------------
    let mut tok = PartToken { nonce: [0; 16], layout_hash: 0, cur_hash: 0, dev_sectors: 0 };
    let pr = part_prepare_core(KIND_SCRATCH, 0, &layout_a, &mut tok);
    check!(pr == 0 && tok.nonce.iter().any(|&b| b != 0), b"PREPARE mints nonce\0");

    // --- (c) APPLY with a WRONG nonce is refused ---------------------------
    let mut bad = tok.nonce;
    bad[0] ^= 0xFF;
    let ac = part_write_core(KIND_SCRATCH, 0, &layout_a, &bad);
    check!(ac == BLKMGR_E_NONCE, b"APPLY wrong-nonce refused\0");

    // --- (d) APPLY with a DIFFERENT layout but the right nonce is refused ---
    // (bad nonce above did NOT consume the token: only a nonce MATCH consumes.)
    let ad = part_write_core(KIND_SCRATCH, 0, &layout_b, &tok.nonce);
    check!(ad == BLKMGR_E_LAYOUT, b"APPLY layout-mismatch refused\0");

    // (d) consumed the token, so re-PREPARE for the real apply.
    let pr2 = part_prepare_core(KIND_SCRATCH, 0, &layout_a, &mut tok);
    check!(pr2 == 0, b"re-PREPARE after layout-mismatch\0");

    // --- (e) APPLY with the right nonce + layout succeeds -------------------
    let ae = part_write_core(KIND_SCRATCH, 0, &layout_a, &tok.nonce);
    check!(ae == 1, b"APPLY correct succeeds\0");

    // --- (f) re-parse the written GPT with parttbl's own parser ------------
    let mut hdrsec = [0u8; SECTOR];
    let rh = blk_rw_core(KIND_SCRATCH, 0, 1, 1, hdrsec.as_mut_ptr(), false);
    let mut gh = GptHdr { ent_lba: 0, num: 0, esz: 0, per_sec: 0, _pad: 0 };
    let hok = rh == 1 && unsafe { parttbl_gpt_hdr_rs(hdrsec.as_ptr(), SECTOR as u32, &mut gh) } == 0;
    check!(hok && gh.num == PART_MAX as u32 && gh.esz == 128, b"GPT header re-parses\0");

    // Scan the first entry sector; parttbl finds the Linux partition and its LBA.
    let mut entsec = [0u8; SECTOR];
    let re = blk_rw_core(KIND_SCRATCH, 0, gh.ent_lba, 1, entsec.as_mut_ptr(), false);
    let mut fb: u32 = 0;
    let mut scan = Scan { consumed: 0, found_lin: 0, lin_lba: 0 };
    if re == 1 && hok {
        unsafe {
            parttbl_gpt_sec_scan_rs(entsec.as_ptr(), SECTOR as u32, gh.esz, gh.per_sec, gh.num, &mut fb, &mut scan);
        }
    }
    check!(scan.found_lin == 1 && scan.lin_lba == lin_start_a, b"GPT scan finds ROOT at expected LBA\0");

    // --- (g) the consumed token cannot be replayed -------------------------
    let ag = part_write_core(KIND_SCRATCH, 0, &layout_a, &tok.nonce);
    check!(ag == BLKMGR_E_NONCE, b"APPLY replay refused (single-use)\0");

    // --- (h) a WRITE aimed at the real BOOT disk is REFUSED, touches nothing.
    let bkind = unsafe { inst_boot_kind() } as u8;
    let bindex = unsafe { inst_boot_index() } as u8;
    // Snapshot boot LBA 0 (read is allowed on the boot disk).
    let mut before = [0u8; SECTOR];
    let rb0 = blk_rw_core(bkind, bindex, 0, 1, before.as_mut_ptr(), false);
    // Attempt a write; a real payload, to prove the refusal fires before I/O.
    let mut junk = [0x5Au8; SECTOR];
    let bw = blk_rw_core(bkind, bindex, 0, 1, junk.as_mut_ptr(), true);
    check!(bw == BLKMGR_E_BOOTDISK, b"WRITE to boot disk refused\0");
    // Re-read and confirm the boot sector is byte-identical.
    let mut after = [0u8; SECTOR];
    let ra0 = blk_rw_core(bkind, bindex, 0, 1, after.as_mut_ptr(), false);
    check!(rb0 == 1 && ra0 == 1 && before == after, b"boot disk unchanged after refusal\0");

    if !out_passed.is_null() {
        unsafe { *out_passed = passed };
    }
    if !out_total.is_null() {
        unsafe { *out_total = total };
    }
    if passed == total {
        0
    } else {
        -1
    }
}

// ===========================================================================
// Stage 3 (#404 disk-mgr): SYS_MKFS - create a FAT (FAT16/FAT32) or ext2
// filesystem INSIDE a partition. This finally implements what installer.h:190
// (installer_format_partition) only ever promised as a prototype.
//
// SAFETY: the caller names a device by (kind,index) and a GPT PARTITION by
// index; nothing is a raw whole-disk LBA range, so a mkfs can never land on a
// partition table by accident. The device is re-resolved in the kernel, the
// boot disk + busy removable are REFUSED (write_permitted, the same gate as
// SYS_BLK_WRITE / SYS_PART_WRITE), the partition is read back from the GPT the
// Stage-2 writer created, and EVERY metadata write is double-bounded: first to
// the partition's own [start, start+size) by part_write_bounded, then to the
// whole device by blk_rw_core. A write cannot escape the partition.
//
// The on-disk structures mirror the EXISTING read drivers so the result mounts
// with them unchanged: FAT against fat.c's fat_mount_lba BPB parse + FAT-type
// -by-cluster-count rule (fat.c:1357-1449), ext2 against ext2.c's ext2_mount
// superblock/group-descriptor/inode offsets (ext2.c:834-1075) and its
// filetype directory entries (ext2.c:1382-1410). The oracle re-parses both with
// that same offset math, and the throwaway-VM run additionally mounts the dumped
// images with real Linux fsck.fat / fsck.ext2 / mount, an independent checker.
// ===========================================================================

const MKFS_FAT: i32 = 1;
const MKFS_EXT2: i32 = 2;

const BLKMGR_E_NOPART: i64 = -707; // partition index missing / GPT unreadable / out of range
const BLKMGR_E_FSTYPE: i64 = -708; // unknown fs_kind
const BLKMGR_E_TOOSMALL: i64 = -709; // partition too small for the requested fs
const BLKMGR_E_MKFS: i64 = -710; // a device I/O during formatting failed

// Little-endian scalar reads from a byte slice (parse side of put_le*).
fn rd16le(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn rd32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
fn rd64le(b: &[u8], o: usize) -> u64 {
    let mut a = [0u8; 8];
    a.copy_from_slice(&b[o..o + 8]);
    u64::from_le_bytes(a)
}
fn wr16le(b: &mut [u8], o: usize, v: u16) {
    b[o..o + 2].copy_from_slice(&v.to_le_bytes());
}

// --- bounded partition I/O -------------------------------------------------
// rel_lba is PARTITION-relative. Refused if it would cross the partition end,
// so mkfs writes can only ever touch this partition; the write then routes
// through blk_rw_core, which re-applies the boot-disk lockout and the
// whole-device bound. Returns sectors written (>0) or a negative sentinel.
fn part_write_bounded(
    kind: u8,
    index: u8,
    pstart: u64,
    psize: u64,
    rel_lba: u64,
    count: u32,
    buf: *mut u8,
) -> i64 {
    let end = match rel_lba.checked_add(count as u64) {
        Some(v) => v,
        None => return BLKMGR_E_RANGE,
    };
    if end > psize {
        return BLKMGR_E_RANGE;
    }
    let abs = match pstart.checked_add(rel_lba) {
        Some(v) => v,
        None => return BLKMGR_E_RANGE,
    };
    blk_rw_core(kind, index, abs, count, buf, true)
}

/// Zero `count` partition-relative sectors from `rel_lba` in <=64-sector chunks.
fn part_zero(kind: u8, index: u8, pstart: u64, psize: u64, rel_lba: u64, count: u64) -> i64 {
    const CHUNK: usize = 64;
    let z = unsafe { kmalloc(CHUNK * SECTOR) } as *mut u8;
    if z.is_null() {
        return E_FAULT;
    }
    unsafe { core::ptr::write_bytes(z, 0, CHUNK * SECTOR) };
    let mut done: u64 = 0;
    let mut rc: i64 = 0;
    while done < count {
        let n = core::cmp::min(CHUNK as u64, count - done) as u32;
        let w = part_write_bounded(kind, index, pstart, psize, rel_lba + done, n, z);
        if w != n as i64 {
            rc = if w < 0 { w } else { BLKMGR_E_MKFS };
            break;
        }
        done += n as u64;
    }
    unsafe { kfree(z as *mut c_void) };
    rc
}

// --- GPT partition resolution ----------------------------------------------
// Read the device's GPT (written by Stage 2) and return (start_lba, sectors)
// for partition `part_index`. An unused (all-zero type GUID) or out-of-range
// entry, or a missing/garbage GPT, is refused. This is what makes SYS_MKFS take
// a PARTITION rather than a raw range.
fn resolve_partition(
    kind: u8,
    index: u8,
    part_index: i32,
    dev_sectors: u64,
) -> Result<(u64, u64), i64> {
    if part_index < 0 || part_index as usize >= PART_MAX {
        return Err(BLKMGR_E_NOPART);
    }
    let mut hdr = [0u8; SECTOR];
    if blk_rw_core(kind, index, 1, 1, hdr.as_mut_ptr(), false) != 1 {
        return Err(BLKMGR_E_MKFS);
    }
    if &hdr[0..8] != b"EFI PART" {
        return Err(BLKMGR_E_NOPART);
    }
    let entry_lba = rd64le(&hdr, 72);
    let num = rd32le(&hdr, 80);
    let esz = rd32le(&hdr, 84);
    if esz < 128 || esz > SECTOR as u32 || num == 0 || (part_index as u32) >= num {
        return Err(BLKMGR_E_NOPART);
    }
    let per_sec = SECTOR as u32 / esz;
    if per_sec == 0 {
        return Err(BLKMGR_E_NOPART);
    }
    let sec_of = entry_lba + (part_index as u32 / per_sec) as u64;
    let idx_in = (part_index as u32 % per_sec) as usize;
    let mut esec = [0u8; SECTOR];
    if blk_rw_core(kind, index, sec_of, 1, esec.as_mut_ptr(), false) != 1 {
        return Err(BLKMGR_E_MKFS);
    }
    let base = idx_in * esz as usize;
    if esec[base..base + 16].iter().all(|&b| b == 0) {
        return Err(BLKMGR_E_NOPART); // unused entry
    }
    let first = rd64le(&esec, base + 32);
    let last = rd64le(&esec, base + 40);
    if first < 34 || last < first || last >= dev_sectors {
        return Err(BLKMGR_E_NOPART);
    }
    Ok((first, last - first + 1))
}

// ===========================================================================
// FAT mkfs (FAT16 + FAT32). The read driver auto-detects the type from the
// cluster count (fat.c:1395-1400), so the whole contract is: lay out a BPB
// whose computed cluster count lands in the band for the FAT-entry width we
// actually write. We pick sectors_per_cluster to hit the band and refuse a
// partition too small for the requested type rather than silently produce a
// FAT12 the caller did not ask for.
// ===========================================================================
struct FatGeom {
    is32: bool,
    spc: u32,
    reserved: u32,
    root_entries: u32,
    fat_size: u32,
    root_dir_sectors: u32,
    data_start: u32,
    clusters: u32,
}

fn fat16_try(total: u32, spc: u32) -> Option<FatGeom> {
    let reserved: u32 = 1;
    let num_fats: u32 = 2;
    let root_entries: u32 = 512;
    let root_dir_sectors = (root_entries * 32 + 511) / 512;
    if total <= reserved + root_dir_sectors {
        return None;
    }
    // fatgen103 FATSz for FAT16.
    let tmp1 = total - (reserved + root_dir_sectors);
    let tmp2 = 256 * spc + num_fats;
    let fat_size = (tmp1 + (tmp2 - 1)) / tmp2;
    let data_start = reserved + num_fats * fat_size + root_dir_sectors;
    if total <= data_start {
        return None;
    }
    let clusters = (total - data_start) / spc;
    if clusters < 4085 || clusters >= 65525 {
        return None;
    }
    Some(FatGeom { is32: false, spc, reserved, root_entries, fat_size, root_dir_sectors, data_start, clusters })
}

fn fat32_try(total: u32, spc: u32) -> Option<FatGeom> {
    let reserved: u32 = 32;
    let num_fats: u32 = 2;
    if total <= reserved {
        return None;
    }
    let tmp1 = total - reserved;
    let tmp2 = (256 * spc + num_fats) / 2;
    if tmp2 == 0 {
        return None;
    }
    let fat_size = (tmp1 + (tmp2 - 1)) / tmp2;
    let data_start = reserved + num_fats * fat_size;
    if total <= data_start {
        return None;
    }
    let clusters = (total - data_start) / spc;
    if clusters < 65525 {
        return None;
    }
    Some(FatGeom { is32: true, spc, reserved, root_entries: 0, fat_size, root_dir_sectors: 0, data_start, clusters })
}

fn fat_choose(total: u32, force32: bool) -> Option<FatGeom> {
    if !force32 {
        for &spc in &[1u32, 2, 4, 8, 16, 32, 64] {
            if let Some(g) = fat16_try(total, spc) {
                return Some(g);
            }
        }
    }
    for &spc in &[1u32, 2, 4, 8, 16, 32, 64, 128] {
        if let Some(g) = fat32_try(total, spc) {
            return Some(g);
        }
    }
    None
}

// Write an 11-byte 8.3-style volume label from an ASCII label (space padded,
// upper-cased). Returns true if a non-empty label was produced.
fn fat_pack_label(label: &[u8], out: &mut [u8; 11]) -> bool {
    *out = [b' '; 11];
    let mut n = 0usize;
    for &c in label {
        if c == 0 || n >= 11 {
            break;
        }
        let u = if c >= b'a' && c <= b'z' { c - 32 } else { c };
        out[n] = u;
        n += 1;
    }
    n > 0
}

fn fat_mkfs(kind: u8, index: u8, pstart: u64, psize: u64, label: &[u8], force32: bool) -> i64 {
    if psize > u32::MAX as u64 {
        return BLKMGR_E_TOOSMALL; // 32-bit FAT sector counts
    }
    let total = psize as u32;
    let g = match fat_choose(total, force32) {
        Some(g) => g,
        None => return BLKMGR_E_TOOSMALL,
    };
    let num_fats: u32 = 2;

    // Zero every metadata sector so no stale bytes are read as structure: the
    // reserved area, both FATs and (FAT16) the root directory region.
    let meta_end = g.data_start; // reserved + FATs + rootdir (FAT16) / reserved+FATs (FAT32)
    let z = part_zero(kind, index, pstart, psize, 0, meta_end as u64);
    if z != 0 {
        return z;
    }
    // FAT32: also zero the first data cluster (the root cluster).
    if g.is32 {
        let z2 = part_zero(kind, index, pstart, psize, g.data_start as u64, g.spc as u64);
        if z2 != 0 {
            return z2;
        }
    }

    let mut vol = [0u8; 11];
    let have_label = fat_pack_label(label, &mut vol);
    let mut volid = [0u8; 4];
    unsafe { csprng_bytes(volid.as_mut_ptr() as *mut c_void, 4) };

    // --- boot sector -------------------------------------------------------
    let mut bs = [0u8; SECTOR];
    bs[0] = 0xEB;
    bs[1] = 0x3C;
    bs[2] = 0x90;
    bs[3..11].copy_from_slice(b"MSWIN4.1");
    wr16le(&mut bs, 11, 512); // bytes per sector
    bs[13] = g.spc as u8;
    wr16le(&mut bs, 14, g.reserved as u16);
    bs[16] = num_fats as u8;
    wr16le(&mut bs, 17, g.root_entries as u16);
    // total sectors 16 vs 32
    if !g.is32 && total < 0x10000 {
        wr16le(&mut bs, 19, total as u16);
    } else {
        wr16le(&mut bs, 19, 0);
    }
    bs[21] = 0xF8; // media: fixed disk
    if !g.is32 {
        wr16le(&mut bs, 22, g.fat_size as u16);
    } else {
        wr16le(&mut bs, 22, 0);
    }
    wr16le(&mut bs, 24, 63); // sectors per track (cosmetic)
    wr16le(&mut bs, 26, 255); // heads (cosmetic)
    // hidden sectors @28 = the partition start LBA
    bs[28..32].copy_from_slice(&(pstart as u32).to_le_bytes());
    bs[32..36].copy_from_slice(&total.to_le_bytes()); // total sectors 32

    if !g.is32 {
        // FAT16 extended BPB at offset 36.
        bs[36] = 0x80; // drive number
        bs[38] = 0x29; // extended boot signature
        bs[39..43].copy_from_slice(&volid);
        bs[43..54].copy_from_slice(&vol);
        bs[54..62].copy_from_slice(b"FAT16   ");
    } else {
        // FAT32 extended BPB at offset 36.
        bs[36..40].copy_from_slice(&g.fat_size.to_le_bytes()); // fat_size_32
        wr16le(&mut bs, 40, 0); // ext flags: FAT mirroring on
        wr16le(&mut bs, 42, 0); // fs version
        bs[44..48].copy_from_slice(&2u32.to_le_bytes()); // root cluster
        wr16le(&mut bs, 48, 1); // fs info sector
        wr16le(&mut bs, 50, 6); // backup boot sector
        bs[64] = 0x80; // drive number
        bs[66] = 0x29; // extended boot signature
        bs[67..71].copy_from_slice(&volid);
        bs[71..82].copy_from_slice(&vol);
        bs[82..90].copy_from_slice(b"FAT32   ");
    }
    bs[510] = 0x55;
    bs[511] = 0xAA;
    if part_write_bounded(kind, index, pstart, psize, 0, 1, bs.as_mut_ptr()) != 1 {
        return BLKMGR_E_MKFS;
    }

    // --- FAT32 FSInfo + backup boot ---------------------------------------
    if g.is32 {
        let mut fsi = [0u8; SECTOR];
        fsi[0..4].copy_from_slice(&0x4161_5252u32.to_le_bytes()); // lead sig "RRaA"
        fsi[484..488].copy_from_slice(&0x6141_7272u32.to_le_bytes()); // struct sig "rrAa"
        // free count / next free: unknown -> 0xFFFFFFFF is legal; be precise:
        // free clusters = total clusters - 1 (root uses cluster 2).
        let free = g.clusters.saturating_sub(1);
        fsi[488..492].copy_from_slice(&free.to_le_bytes());
        fsi[492..496].copy_from_slice(&3u32.to_le_bytes()); // next free hint
        fsi[508..512].copy_from_slice(&0xAA55_0000u32.to_le_bytes()); // trail sig
        if part_write_bounded(kind, index, pstart, psize, 1, 1, fsi.as_mut_ptr()) != 1 {
            return BLKMGR_E_MKFS;
        }
        // Backup boot sector (6) + backup FSInfo (7).
        if part_write_bounded(kind, index, pstart, psize, 6, 1, bs.as_mut_ptr()) != 1 {
            return BLKMGR_E_MKFS;
        }
        if part_write_bounded(kind, index, pstart, psize, 7, 1, fsi.as_mut_ptr()) != 1 {
            return BLKMGR_E_MKFS;
        }
    }

    // --- FAT tables: entry 0/1 (+ entry 2 EOC for the FAT32 root cluster) ---
    let mut fat0 = [0u8; SECTOR];
    if !g.is32 {
        // FAT16: FAT[0]=0xFFF8 (media|0xFF00), FAT[1]=0xFFFF (EOC).
        fat0[0] = 0xF8;
        fat0[1] = 0xFF;
        fat0[2] = 0xFF;
        fat0[3] = 0xFF;
    } else {
        // FAT32: 28-bit entries. FAT[0]=0x0FFFFFF8, FAT[1]=0x0FFFFFFF,
        // FAT[2]=0x0FFFFFFF (root cluster chain terminates at itself).
        fat0[0..4].copy_from_slice(&0x0FFF_FFF8u32.to_le_bytes());
        fat0[4..8].copy_from_slice(&0x0FFF_FFFFu32.to_le_bytes());
        fat0[8..12].copy_from_slice(&0x0FFF_FFFFu32.to_le_bytes());
    }
    for f in 0..num_fats {
        let fat_lba = g.reserved + f * g.fat_size;
        if part_write_bounded(kind, index, pstart, psize, fat_lba as u64, 1, fat0.as_mut_ptr()) != 1 {
            return BLKMGR_E_MKFS;
        }
    }

    // --- root directory: an optional volume-label entry -------------------
    if have_label {
        let mut root = [0u8; SECTOR];
        root[0..11].copy_from_slice(&vol);
        root[11] = 0x08; // ATTR_VOLUME_ID
        let root_lba = if g.is32 { g.data_start } else { g.reserved + num_fats * g.fat_size };
        if part_write_bounded(kind, index, pstart, psize, root_lba as u64, 1, root.as_mut_ptr()) != 1 {
            return BLKMGR_E_MKFS;
        }
    }
    0
}

// ===========================================================================
// ext2 mkfs (block size 1024, inode size 256, NON-sparse so every group carries
// a superblock + GDT backup - the simplest layout that is uniform per group and
// that fsck.ext2 accepts). Multi-group. Mirrors ext2.c's read side: superblock
// at block 1 (ext2.c:837), GDT at first_data_block+1 (ext2.c:891), 32-byte group
// descriptors with bg_inode_table at +8 (ext2.c:963), inode 2 = root at inode
// offsets from ext2.c:1037-1074, and filetype directory entries (ext2.c:1391).
// ===========================================================================
const E2_BS: u32 = 1024;
const E2_INODE_SIZE: u32 = 256;
const E2_BPG: u32 = 8192; // block_size * 8
const E2_FIRST_DATA: u32 = 1;
const E2_ROOT_INO: u32 = 2;
const E2_LF_INO: u32 = 11;
const E2_FIRST_INO: u32 = 11;
const E2_FT_DIR: u8 = 2;

/// Write one 1024-byte ext2 block (2 sectors) at absolute fs block `block`.
fn e2_write_block(kind: u8, index: u8, pstart: u64, psize: u64, block: u32, buf: *mut u8) -> i64 {
    part_write_bounded(kind, index, pstart, psize, block as u64 * 2, 2, buf)
}

fn ext2_mkfs(kind: u8, index: u8, pstart: u64, psize: u64, label: &[u8]) -> i64 {
    let blocks_count: u32 = (psize / 2) as u32; // whole 1KB blocks
    if blocks_count < 64 {
        return BLKMGR_E_TOOSMALL;
    }
    let usable = blocks_count - E2_FIRST_DATA; // blocks the groups cover
    let groups = (usable + E2_BPG - 1) / E2_BPG;
    let gdt_blocks = (groups * 32 + E2_BS - 1) / E2_BS;

    // Inodes: ~1 per 16 blocks, at least 16, per-group a multiple of 8 (bitmap)
    // and of block_size/inode_size = 4 (so the table is whole blocks).
    let desired = core::cmp::max(16u32, blocks_count / 16);
    let mut ipg = (desired + groups - 1) / groups;
    ipg = (ipg + 7) & !7u32; // multiple of 8 (implies multiple of 4)
    if ipg < 8 {
        ipg = 8;
    }
    let itb = ipg / 4; // inode-table blocks per group (ipg*256/1024)
    let inodes_count = ipg * groups;

    // Per-group metadata overhead (non-sparse: sb + GDT in every group).
    let meta = 1 + gdt_blocks + 1 + 1 + itb; // sb, gdt, bbmap, ibmap, itable
    // Group 0 also holds the root dir + lost+found data blocks.
    let g0_extra: u32 = 2;

    // Smallest group must still hold its own metadata (+ the two dir blocks in g0).
    let last_group_blocks = usable - (groups - 1) * E2_BPG;
    if last_group_blocks <= meta || (groups == 1 && last_group_blocks <= meta + g0_extra) {
        return BLKMGR_E_TOOSMALL;
    }

    // Group start block and per-group layout (uniform, non-sparse).
    let gstart = |g: u32| -> u32 { E2_FIRST_DATA + g * E2_BPG };
    let bbmap_blk = |g: u32| -> u32 { gstart(g) + 1 + gdt_blocks };
    let ibmap_blk = |g: u32| -> u32 { gstart(g) + 1 + gdt_blocks + 1 };
    let itab_blk = |g: u32| -> u32 { gstart(g) + 1 + gdt_blocks + 2 };
    let gblocks = |g: u32| -> u32 {
        if g == groups - 1 {
            last_group_blocks
        } else {
            E2_BPG
        }
    };

    let root_dir_block = itab_blk(0) + itb; // first data block of group 0
    let lf_block = root_dir_block + 1;

    // Free counts.
    let mut total_free_blocks: u32 = 0;
    let mut total_free_inodes: u32 = 0;
    for g in 0..groups {
        let used = meta + if g == 0 { g0_extra } else { 0 };
        total_free_blocks += gblocks(g) - used;
        let used_ino = if g == 0 { E2_LF_INO } else { 0 }; // inodes 1..11 in group 0
        total_free_inodes += ipg - used_ino;
    }

    // --- build the group descriptor table (identical in every group) ------
    let gdt_bytes = gdt_blocks as usize * E2_BS as usize;
    let gdt = unsafe { kmalloc(gdt_bytes) } as *mut u8;
    if gdt.is_null() {
        return E_FAULT;
    }
    let cleanup_gdt = |gdt: *mut u8| unsafe { kfree(gdt as *mut c_void) };
    unsafe { core::ptr::write_bytes(gdt, 0, gdt_bytes) };
    {
        let gs = unsafe { core::slice::from_raw_parts_mut(gdt, gdt_bytes) };
        for g in 0..groups as usize {
            let o = g * 32;
            gs[o..o + 4].copy_from_slice(&bbmap_blk(g as u32).to_le_bytes());
            gs[o + 4..o + 8].copy_from_slice(&ibmap_blk(g as u32).to_le_bytes());
            gs[o + 8..o + 12].copy_from_slice(&itab_blk(g as u32).to_le_bytes());
            let used = meta + if g == 0 { g0_extra } else { 0 };
            let free_b = gblocks(g as u32) - used;
            let used_ino = if g == 0 { E2_LF_INO } else { 0 };
            let free_i = ipg - used_ino;
            wr16le(gs, o + 12, free_b as u16);
            wr16le(gs, o + 14, free_i as u16);
            wr16le(gs, o + 16, if g == 0 { 2 } else { 0 }); // used_dirs_count
        }
    }

    // --- build the superblock (1024 bytes) --------------------------------
    let mut sb = [0u8; E2_BS as usize];
    sb[0..4].copy_from_slice(&inodes_count.to_le_bytes()); // s_inodes_count
    sb[4..8].copy_from_slice(&blocks_count.to_le_bytes()); // s_blocks_count
    sb[8..12].copy_from_slice(&0u32.to_le_bytes()); // s_r_blocks_count
    sb[12..16].copy_from_slice(&total_free_blocks.to_le_bytes()); // s_free_blocks_count
    sb[16..20].copy_from_slice(&total_free_inodes.to_le_bytes()); // s_free_inodes_count
    sb[20..24].copy_from_slice(&E2_FIRST_DATA.to_le_bytes()); // s_first_data_block
    sb[24..28].copy_from_slice(&0u32.to_le_bytes()); // s_log_block_size (1024<<0)
    sb[28..32].copy_from_slice(&0u32.to_le_bytes()); // s_log_frag_size
    sb[32..36].copy_from_slice(&E2_BPG.to_le_bytes()); // s_blocks_per_group
    sb[36..40].copy_from_slice(&E2_BPG.to_le_bytes()); // s_frags_per_group
    sb[40..44].copy_from_slice(&ipg.to_le_bytes()); // s_inodes_per_group
    wr16le(&mut sb, 52, 0); // s_mnt_count
    wr16le(&mut sb, 54, 0xFFFF); // s_max_mnt_count = -1 (never force)
    wr16le(&mut sb, 56, 0xEF53); // s_magic
    wr16le(&mut sb, 58, 1); // s_state = EXT2_VALID_FS (clean)
    wr16le(&mut sb, 60, 1); // s_errors = continue
    wr16le(&mut sb, 62, 0); // s_minor_rev_level
    sb[76..80].copy_from_slice(&1u32.to_le_bytes()); // s_rev_level = DYNAMIC (rev 1)
    sb[84..88].copy_from_slice(&E2_FIRST_INO.to_le_bytes()); // s_first_ino
    wr16le(&mut sb, 88, E2_INODE_SIZE as u16); // s_inode_size
    wr16le(&mut sb, 90, 0); // s_block_group_nr (primary)
    sb[92..96].copy_from_slice(&0u32.to_le_bytes()); // s_feature_compat
    sb[96..100].copy_from_slice(&0x2u32.to_le_bytes()); // s_feature_incompat = FILETYPE
    sb[100..104].copy_from_slice(&0u32.to_le_bytes()); // s_feature_ro_compat (non-sparse)
    unsafe { csprng_bytes(sb[104..120].as_mut_ptr() as *mut c_void, 16) }; // s_uuid
    {
        // s_volume_name (16 bytes).
        let mut n = 0usize;
        while n < 16 && n < label.len() && label[n] != 0 {
            sb[120 + n] = label[n];
            n += 1;
        }
    }

    // --- write every group's metadata -------------------------------------
    let bmap = unsafe { kmalloc(E2_BS as usize) } as *mut u8;
    let ibmap = unsafe { kmalloc(E2_BS as usize) } as *mut u8;
    let itbuf = unsafe { kmalloc(E2_BS as usize) } as *mut u8;
    if bmap.is_null() || ibmap.is_null() || itbuf.is_null() {
        cleanup_gdt(gdt);
        unsafe {
            if !bmap.is_null() { kfree(bmap as *mut c_void); }
            if !ibmap.is_null() { kfree(ibmap as *mut c_void); }
            if !itbuf.is_null() { kfree(itbuf as *mut c_void); }
        }
        return E_FAULT;
    }

    let mut fail: i64 = 0;
    'groups: for g in 0..groups {
        // superblock copy (block_group_nr = g for backups).
        wr16le(&mut sb, 90, g as u16);
        if e2_write_block(kind, index, pstart, psize, gstart(g), sb.as_mut_ptr()) != 2 {
            fail = BLKMGR_E_MKFS;
            break;
        }
        // GDT copy.
        for b in 0..gdt_blocks {
            let src = unsafe { gdt.add(b as usize * E2_BS as usize) };
            if e2_write_block(kind, index, pstart, psize, gstart(g) + 1 + b, src) != 2 {
                fail = BLKMGR_E_MKFS;
                break 'groups;
            }
        }
        // block bitmap.
        unsafe { core::ptr::write_bytes(bmap, 0, E2_BS as usize) };
        let bslice = unsafe { core::slice::from_raw_parts_mut(bmap, E2_BS as usize) };
        let used = meta + if g == 0 { g0_extra } else { 0 };
        for bit in 0..used {
            bslice[(bit / 8) as usize] |= 1 << (bit % 8);
        }
        // padding bits beyond this group's real blocks -> used.
        for bit in gblocks(g)..(E2_BPG) {
            bslice[(bit / 8) as usize] |= 1 << (bit % 8);
        }
        if e2_write_block(kind, index, pstart, psize, bbmap_blk(g), bmap) != 2 {
            fail = BLKMGR_E_MKFS;
            break;
        }
        // inode bitmap.
        unsafe { core::ptr::write_bytes(ibmap, 0, E2_BS as usize) };
        let islice = unsafe { core::slice::from_raw_parts_mut(ibmap, E2_BS as usize) };
        if g == 0 {
            for bit in 0..E2_LF_INO {
                // inodes 1..11 (bits 0..10)
                islice[(bit / 8) as usize] |= 1 << (bit % 8);
            }
        }
        for bit in ipg..(E2_BS * 8) {
            islice[(bit / 8) as usize] |= 1 << (bit % 8);
        }
        if e2_write_block(kind, index, pstart, psize, ibmap_blk(g), ibmap) != 2 {
            fail = BLKMGR_E_MKFS;
            break;
        }
        // inode table: zero it, then (group 0) patch inode 2 and inode 11.
        for tb in 0..itb {
            unsafe { core::ptr::write_bytes(itbuf, 0, E2_BS as usize) };
            if g == 0 {
                let tslice = unsafe { core::slice::from_raw_parts_mut(itbuf, E2_BS as usize) };
                // inode N lives at byte (N-1)*inode_size within the table.
                let per_block = E2_BS / E2_INODE_SIZE; // 4
                // root inode 2 -> table index 1
                let root_idx = E2_ROOT_INO - 1;
                if root_idx / per_block == tb {
                    let off = (root_idx % per_block) as usize * E2_INODE_SIZE as usize;
                    e2_fill_inode(tslice, off, 0x41ED, 3, 1024, root_dir_block);
                }
                let lf_idx = E2_LF_INO - 1;
                if lf_idx / per_block == tb {
                    let off = (lf_idx % per_block) as usize * E2_INODE_SIZE as usize;
                    e2_fill_inode(tslice, off, 0x41C0, 2, 1024, lf_block);
                }
            }
            if e2_write_block(kind, index, pstart, psize, itab_blk(g) + tb, itbuf) != 2 {
                fail = BLKMGR_E_MKFS;
                break 'groups;
            }
        }
    }

    // --- group 0 data blocks: root dir + lost+found dir -------------------
    if fail == 0 {
        // root directory: ".", "..", "lost+found".
        unsafe { core::ptr::write_bytes(itbuf, 0, E2_BS as usize) };
        let rd = unsafe { core::slice::from_raw_parts_mut(itbuf, E2_BS as usize) };
        e2_dirent(rd, 0, E2_ROOT_INO, 12, E2_FT_DIR, b".");
        e2_dirent(rd, 12, E2_ROOT_INO, 12, E2_FT_DIR, b"..");
        e2_dirent(rd, 24, E2_LF_INO, (E2_BS - 24) as u16, E2_FT_DIR, b"lost+found");
        if e2_write_block(kind, index, pstart, psize, root_dir_block, itbuf) != 2 {
            fail = BLKMGR_E_MKFS;
        }
    }
    if fail == 0 {
        // lost+found directory: ".", "..".
        unsafe { core::ptr::write_bytes(itbuf, 0, E2_BS as usize) };
        let ld = unsafe { core::slice::from_raw_parts_mut(itbuf, E2_BS as usize) };
        e2_dirent(ld, 0, E2_LF_INO, 12, E2_FT_DIR, b".");
        e2_dirent(ld, 12, E2_ROOT_INO, (E2_BS - 12) as u16, E2_FT_DIR, b"..");
        if e2_write_block(kind, index, pstart, psize, lf_block, itbuf) != 2 {
            fail = BLKMGR_E_MKFS;
        }
    }

    cleanup_gdt(gdt);
    unsafe {
        kfree(bmap as *mut c_void);
        kfree(ibmap as *mut c_void);
        kfree(itbuf as *mut c_void);
    }
    fail
}

/// Fill a 256-byte on-disk ext2 inode at `off` in `buf` as a single-block
/// directory. Offsets are ext2.c's read side (ext2.c:1037-1074).
fn e2_fill_inode(buf: &mut [u8], off: usize, mode: u16, links: u16, size: u32, block0: u32) {
    wr16le(buf, off + 0, mode); // i_mode
    buf[off + 4..off + 8].copy_from_slice(&size.to_le_bytes()); // i_size
    wr16le(buf, off + 26, links); // i_links_count
    buf[off + 28..off + 32].copy_from_slice(&2u32.to_le_bytes()); // i_blocks (1024/512)
    buf[off + 40..off + 44].copy_from_slice(&block0.to_le_bytes()); // i_block[0]
}

/// Write an ext2 directory entry (ext2.c:1391 offsets) at `off` in `buf`.
fn e2_dirent(buf: &mut [u8], off: usize, ino: u32, rec_len: u16, ftype: u8, name: &[u8]) {
    buf[off..off + 4].copy_from_slice(&ino.to_le_bytes());
    wr16le(buf, off + 4, rec_len);
    buf[off + 6] = name.len() as u8;
    buf[off + 7] = ftype;
    buf[off + 8..off + 8 + name.len()].copy_from_slice(name);
}

// --- mkfs dispatch + syscall ----------------------------------------------
fn mkfs_core(kind: u8, index: u8, part_index: i32, fstype: i32, label: &[u8]) -> i64 {
    let r = match resolve(kind, index) {
        Some(r) => r,
        None => return BLKMGR_E_NODEV,
    };
    let perm = write_permitted(&r, kind, index);
    if perm != 0 {
        return perm; // boot disk / busy: refused before the GPT is even read
    }
    let (pstart, psize) = match resolve_partition(kind, index, part_index, r.sectors) {
        Ok(v) => v,
        Err(e) => return e,
    };
    match fstype {
        MKFS_FAT => {
            let force32 = psize.saturating_mul(SECTOR as u64) >= 512 * 1024 * 1024;
            fat_mkfs(kind, index, pstart, psize, label, force32)
        }
        MKFS_EXT2 => ext2_mkfs(kind, index, pstart, psize, label),
        _ => BLKMGR_E_FSTYPE,
    }
}

/// SYS_MKFS(kind, index, part_index, fstype, label) -> 0 | negative. Root only.
/// `label` is an optional NUL-terminated volume label (NULL = none). The device
/// is re-resolved and the boot disk / busy removable refused before any write;
/// the partition is read from the on-disk GPT so a raw range can never be given.
#[no_mangle]
pub unsafe extern "C" fn sys_mkfs_rs(
    kind: i32,
    index: i32,
    part_index: i32,
    fstype: i32,
    u_label: *const c_void,
) -> i64 {
    if syscall_caller_euid() != 0 {
        return E_PERM;
    }
    if !(0..=255).contains(&kind) || !(0..=255).contains(&index) {
        return E_INVAL;
    }
    if fstype != MKFS_FAT && fstype != MKFS_EXT2 {
        return BLKMGR_E_FSTYPE;
    }
    // Copy the optional label (bounded). The argtab validates the string; a NULL
    // pointer is skipped centrally, so treat NULL as "no label".
    let mut lbuf = [0u8; 32];
    if !u_label.is_null() {
        if copy_from_user(lbuf.as_mut_ptr() as *mut c_void, u_label, 31) != 0 {
            return E_FAULT;
        }
        lbuf[31] = 0;
    }
    let n = lbuf.iter().position(|&b| b == 0).unwrap_or(lbuf.len());
    mkfs_core(kind as u8, index as u8, part_index, fstype, &lbuf[..n])
}

// ===========================================================================
// STAGE 3 ORACLE. Marker-gated (same /DISKMGR.TST). On the SCRATCH device only:
// partition it, mkfs FAT16 + ext2 (multi-group) + FAT32, re-parse each with the
// same offset math the C read drivers use, and prove the refusals FIRE (boot
// disk, bad partition index). Returns 0 iff every check passed.
// ===========================================================================
#[no_mangle]
pub extern "C" fn blkmgr_mkfs_selftest_rs(out_passed: *mut u32, out_total: *mut u32) -> i32 {
    let mut passed: u32 = 0;
    let mut total: u32 = 0;
    macro_rules! check {
        ($cond:expr, $label:expr) => {{
            total += 1;
            if $cond {
                passed += 1;
            } else {
                unsafe { kprintf(b"[DISKMGR] mkfs oracle FAIL: %s\n\0".as_ptr(), ($label).as_ptr()); }
            }
        }};
    }
    if scratch_base() == 0 {
        unsafe { kprintf(b"[DISKMGR] mkfs oracle: scratch absent, cannot run\n\0".as_ptr()) };
        if !out_passed.is_null() { unsafe { *out_passed = 0 }; }
        if !out_total.is_null() { unsafe { *out_total = 1 }; }
        return -1;
    }
    let sec = scratch_sectors();

    // --- Phase A: FAT16 + ext2 (multi-group) -------------------------------
    // p1 FAT16 8 MiB, p2 ext2 the rest.
    let p1_start: u64 = 2048;
    let p1_size: u64 = 16384; // 8 MiB
    let p2_start: u64 = p1_start + p1_size;
    let p2_size: u64 = sec.saturating_sub(p2_start + 40); // leave backup-GPT room
    let layout_a = [
        spec(GUID_DATA, p1_start, p1_size, b"FATP"),
        spec(GUID_LINUX, p2_start, p2_size, b"EXTP"),
    ];
    check!(write_gpt(KIND_SCRATCH, 0, sec as u64, &layout_a) == 0, b"phase A: write GPT\0");

    let mf = mkfs_core(KIND_SCRATCH, 0, 0, MKFS_FAT, b"MYFAT16");
    check!(mf == 0, b"mkfs FAT16 on p1\0");
    let me = mkfs_core(KIND_SCRATCH, 0, 1, MKFS_EXT2, b"myext2");
    check!(me == 0, b"mkfs ext2 on p2\0");

    check!(verify_fat(KIND_SCRATCH, 0, p1_start, p1_size, false), b"FAT16 re-parses + label\0");
    check!(verify_ext2(KIND_SCRATCH, 0, p2_start), b"ext2 re-parses (sb/gdt/root/dirents)\0");

    // --- Phase B: FAT32 on a large repartition -----------------------------
    let b_start: u64 = 2048;
    let b_size: u64 = sec.saturating_sub(b_start + 40);
    let layout_b = [spec(GUID_DATA, b_start, b_size, b"BIGFAT")];
    check!(write_gpt(KIND_SCRATCH, 0, sec as u64, &layout_b) == 0, b"phase B: write GPT\0");
    // Force FAT32 by passing a huge apparent size is not how mkfs_core decides;
    // call fat_mkfs directly with force32 so the FAT32 path is exercised even on
    // a scratch smaller than the 512 MiB auto-threshold.
    let mf32 = fat_mkfs(KIND_SCRATCH, 0, b_start, b_size, b"MYFAT32", true);
    check!(mf32 == 0, b"mkfs FAT32 on big partition\0");
    check!(verify_fat(KIND_SCRATCH, 0, b_start, b_size, true), b"FAT32 re-parses + label\0");

    // --- Refusals ----------------------------------------------------------
    // Restore a 2-partition table so a valid partition index exists.
    let _ = write_gpt(KIND_SCRATCH, 0, sec as u64, &layout_a);
    // (a) bad partition index refused.
    let bad = mkfs_core(KIND_SCRATCH, 0, 7, MKFS_EXT2, b"x");
    check!(bad == BLKMGR_E_NOPART, b"mkfs bad partition index refused\0");
    // (b) unknown fs type refused.
    let bft = mkfs_core(KIND_SCRATCH, 0, 0, 99, b"x");
    check!(bft == BLKMGR_E_FSTYPE, b"mkfs unknown fstype refused\0");
    // (c) mkfs aimed at the real BOOT disk refused, touches nothing.
    let bkind = unsafe { inst_boot_kind() } as u8;
    let bindex = unsafe { inst_boot_index() } as u8;
    let mut before = [0u8; SECTOR];
    let rb = blk_rw_core(bkind, bindex, 0, 1, before.as_mut_ptr(), false);
    let bm = mkfs_core(bkind, bindex, 0, MKFS_EXT2, b"x");
    check!(bm == BLKMGR_E_BOOTDISK, b"mkfs on boot disk refused\0");
    let mut after = [0u8; SECTOR];
    let ra = blk_rw_core(bkind, bindex, 0, 1, after.as_mut_ptr(), false);
    check!(rb == 1 && ra == 1 && before == after, b"boot disk unchanged after mkfs refusal\0");

    if !out_passed.is_null() { unsafe { *out_passed = passed }; }
    if !out_total.is_null() { unsafe { *out_total = total }; }
    if passed == total { 0 } else { -1 }
}

/// Re-parse a FAT partition the way fat.c's fat_mount_lba would, and confirm the
/// volume-label entry we wrote is in the root directory. `want32` asserts the
/// driver would classify it FAT32, else FAT16.
fn verify_fat(kind: u8, index: u8, pstart: u64, psize: u64, want32: bool) -> bool {
    let mut bs = [0u8; SECTOR];
    if blk_rw_core(kind, index, pstart, 1, bs.as_mut_ptr(), false) != 1 {
        return false;
    }
    if bs[510] != 0x55 || bs[511] != 0xAA {
        return false;
    }
    let bps = rd16le(&bs, 11) as u32;
    let spc = bs[13] as u32;
    let reserved = rd16le(&bs, 14) as u32;
    let num_fats = bs[16] as u32;
    let root_entries = rd16le(&bs, 17) as u32;
    if bps != 512 || spc == 0 || num_fats != 2 {
        return false;
    }
    let total16 = rd16le(&bs, 19) as u32;
    let total32 = rd32le(&bs, 32);
    let total = if total16 != 0 { total16 } else { total32 };
    let fat16 = rd16le(&bs, 22) as u32;
    let fat_size = if fat16 != 0 { fat16 } else { rd32le(&bs, 36) };
    let root_dir_sectors = (root_entries * 32 + bps - 1) / bps;
    let root_start = reserved + num_fats * fat_size;
    let data_start = root_start + root_dir_sectors;
    if total <= data_start {
        return false;
    }
    let clusters = (total - data_start) / spc;
    let is32 = clusters >= 65525;
    if is32 != want32 {
        return false;
    }
    // Find the volume-label entry (attr 0x08) in the root directory.
    if !is32 {
        let mut found = false;
        for s in 0..root_dir_sectors {
            let mut dir = [0u8; SECTOR];
            if blk_rw_core(kind, index, pstart + (root_start + s) as u64, 1, dir.as_mut_ptr(), false) != 1 {
                return false;
            }
            let mut e = 0usize;
            while e + 32 <= SECTOR {
                if dir[e] != 0 && dir[e] != 0xE5 && dir[e + 11] == 0x08 {
                    found = true;
                }
                e += 32;
            }
        }
        found
    } else {
        // FAT32: FAT[2] must be a valid EOC and cluster 2 (root) holds the label.
        let mut fat = [0u8; SECTOR];
        if blk_rw_core(kind, index, pstart + reserved as u64, 1, fat.as_mut_ptr(), false) != 1 {
            return false;
        }
        let e2 = rd32le(&fat, 8) & 0x0FFF_FFFF;
        if e2 < 0x0FFF_FFF8 {
            return false;
        }
        let mut dir = [0u8; SECTOR];
        if blk_rw_core(kind, index, pstart + data_start as u64, 1, dir.as_mut_ptr(), false) != 1 {
            return false;
        }
        let mut e = 0usize;
        let mut found = false;
        while e + 32 <= SECTOR {
            if dir[e] != 0 && dir[e] != 0xE5 && dir[e + 11] == 0x08 {
                found = true;
            }
            e += 32;
        }
        found
    }
}

/// Re-parse an ext2 partition the way ext2.c's ext2_mount + ext2_read_inode +
/// ext2_dirblock_find would, and confirm root inode 2 is a directory whose block
/// holds ".", ".." and "lost+found".
fn verify_ext2(kind: u8, index: u8, pstart: u64) -> bool {
    // Superblock at fs block 1 == LBA pstart + 2.
    let mut sb = [0u8; SECTOR * 2];
    if blk_rw_core(kind, index, pstart + 2, 2, sb.as_mut_ptr(), false) != 2 {
        return false;
    }
    if rd16le(&sb, 56) != 0xEF53 {
        return false;
    }
    let log_bs = rd32le(&sb, 24);
    let bs = 1024u32 << log_bs;
    let ipg = rd32le(&sb, 40);
    let isize = rd16le(&sb, 88) as u32;
    let first_data = rd32le(&sb, 20);
    if bs != 1024 || ipg == 0 || isize != 256 || first_data != 1 {
        return false;
    }
    let spb = bs / 512; // sectors per block = 2
    // Group descriptor table at block first_data+1 == block 2.
    let gdt_block = first_data + 1;
    let mut gd = [0u8; SECTOR * 2];
    if blk_rw_core(kind, index, pstart + gdt_block as u64 * spb as u64, spb, gd.as_mut_ptr(), false) != spb as i64 {
        return false;
    }
    let inode_table = rd32le(&gd, 8); // group 0 bg_inode_table
    // Root inode 2: byte offset within the inode table.
    let idx = E2_ROOT_INO - 1; // 1
    let byte_off = inode_table as u64 * bs as u64 + idx as u64 * isize as u64;
    let blk_of = (byte_off / bs as u64) as u32;
    let off_in = (byte_off % bs as u64) as usize;
    let mut ib = [0u8; SECTOR * 2];
    if blk_rw_core(kind, index, pstart + blk_of as u64 * spb as u64, spb, ib.as_mut_ptr(), false) != spb as i64 {
        return false;
    }
    let mode = rd16le(&ib, off_in + 0);
    if mode & 0xF000 != 0x4000 {
        return false; // not a directory
    }
    let root_blk = rd32le(&ib, off_in + 40); // i_block[0]
    if root_blk == 0 {
        return false;
    }
    let mut rd = [0u8; SECTOR * 2];
    if blk_rw_core(kind, index, pstart + root_blk as u64 * spb as u64, spb, rd.as_mut_ptr(), false) != spb as i64 {
        return false;
    }
    // Walk the directory block (ext2.c:1391 offsets); require ., .., lost+found.
    let mut dot = false;
    let mut dotdot = false;
    let mut lf = false;
    let mut o = 0usize;
    while o + 8 <= bs as usize {
        let ino = rd32le(&rd, o);
        let rec = rd16le(&rd, o + 4) as usize;
        let nlen = rd[o + 6] as usize;
        if rec < 8 || o + rec > bs as usize {
            break;
        }
        if ino != 0 && o + 8 + nlen <= bs as usize {
            let name = &rd[o + 8..o + 8 + nlen];
            if name == b"." {
                dot = true;
            } else if name == b".." {
                dotdot = true;
            } else if name == b"lost+found" {
                lf = true;
            }
        }
        o += rec;
    }
    dot && dotdot && lf
}

// ===========================================================================
// Stage 3 host-verification hook. Lay down a CLEAN FAT16 + (multi-group) ext2
// on the scratch device and dump both partition images to files on the ext2
// root, so the throwaway VM's disk can be examined on the host with real Linux
// fsck.fat / fsck.ext2 / mount - an independent checker, stronger than our own
// read driver. Marker-gated (only runs when /DISKMGR.TST armed the scratch dev)
// and never touches a real disk. Returns 0 on success or a negative code.
// ===========================================================================
extern "C" {
    fn ext2_write_file(path: *const u8, data: *const c_void, len: u32) -> i32;
    fn ext2_is_mounted() -> i32;
}

fn mkfs_dump_one(path: &[u8], start: u64, size: u64) -> i32 {
    let bytes = (size as usize).saturating_mul(SECTOR);
    let buf = unsafe { kmalloc(bytes) } as *mut u8;
    if buf.is_null() {
        return -10;
    }
    let mut done: u64 = 0;
    while done < size {
        let n = core::cmp::min(256u64, size - done) as u32;
        let dst = unsafe { buf.add(done as usize * SECTOR) };
        if blk_rw_core(KIND_SCRATCH, 0, start + done, n, dst, false) != n as i64 {
            unsafe { kfree(buf as *mut c_void) };
            return -11;
        }
        done += n as u64;
    }
    let rc = unsafe { ext2_write_file(path.as_ptr(), buf as *const c_void, bytes as u32) };
    unsafe { kfree(buf as *mut c_void) };
    rc
}

#[no_mangle]
pub extern "C" fn blkmgr_mkfs_dump_rs() -> i32 {
    if scratch_base() == 0 {
        return -1;
    }
    if unsafe { ext2_is_mounted() } == 0 {
        return -2;
    }
    let sec = scratch_sectors();
    // Modest, host-mountable sizes: FAT16 4 MiB, ext2 10 MiB (2 block groups).
    let p1s: u64 = 2048;
    let p1z: u64 = 8192; // 4 MiB
    let p2s: u64 = 10240;
    let p2z: u64 = 20480; // 10 MiB
    if p2s + p2z + 40 > sec {
        return -3;
    }
    let layout = [
        spec(GUID_DATA, p1s, p1z, b"FATP"),
        spec(GUID_LINUX, p2s, p2z, b"EXTP"),
    ];
    if write_gpt(KIND_SCRATCH, 0, sec as u64, &layout) != 0 {
        return -4;
    }
    if mkfs_core(KIND_SCRATCH, 0, 0, MKFS_FAT, b"MYFAT16") != 0 {
        return -5;
    }
    if mkfs_core(KIND_SCRATCH, 0, 1, MKFS_EXT2, b"myext2") != 0 {
        return -6;
    }
    let r1 = mkfs_dump_one(b"/MKFS_FAT.IMG\0", p1s, p1z);
    if r1 != 0 {
        return -7;
    }
    let r2 = mkfs_dump_one(b"/MKFS_EXT2.IMG\0", p2s, p2z);
    if r2 != 0 {
        return -8;
    }
    0
}

// ===========================================================================
// STAGE 4 (#404 disk-mgr): general MOUNT / UNMOUNT with a mount table.
//
// SCOPE, HONEST (see fs/blkmgr.h header). The two BOOT mounts (FAT ESP g_fat_fs
// + ext2 root g_ext2) are NOT touched: ext2_mount is a destructive singleton
// over the running root (blame.md 2026-09-19), so a general multi-mount VFS over
// those globals is out of scope. Instead this is a SELF-CONTAINED aux mount
// table whose file I/O routes through blk_rw_core / part_write_bounded (the same
// choke point the mkfs oracle uses), so it works on the RAM scratch AND real
// devices and can never reach the boot singletons or the global VFS resolver.
//
// FULLY GENERAL in this cut: SYS_MOUNT_LIST (lists both boot mounts + every aux
// mount); SYS_MOUNT / SYS_UMOUNT lifecycle of an aux mount for FAT16/FAT32/ext2,
// with the full safety model; a self-contained FAT (16/32) file read+write layer
// through a mount, PROVEN by the oracle.
// DEFERRED to Stage 4b (documented): wiring the global VFS path resolver so
// userland file syscalls resolve through an aux mount (the large VFS-table change
// the plan keeps last), and ext2 write-THROUGH-mount (ext2 aux mounts are
// browse/metadata only in this cut). Neither is needed for the kernel floor.
// ===========================================================================

const MNT_F_BOOT: u8 = 0x01;
const MNT_F_RO: u8 = 0x02;
const MNT_F_DYNAMIC: u8 = 0x04;
const MNT_F_ROOT: u8 = 0x08;

const MNT_FS_FAT16: u8 = 1;
const MNT_FS_FAT32: u8 = 2;
const MNT_FS_EXT2: u8 = 3;

// New Stage-4 error sentinels, continuing the -70x block.
const BLKMGR_E_BADPATH: i64 = -711; // mount path malformed / not under /MNT/
const BLKMGR_E_INUSE: i64 = -712; // path already mounted
const BLKMGR_E_FULL: i64 = -713; // aux mount table full
const BLKMGR_E_NOMNT: i64 = -714; // no such mount (umount) / not mountable
const BLKMGR_E_FSBAD: i64 = -715; // partition holds no recognisable FAT/ext2

const MNT_AUX_MAX: usize = 8; // aux (dynamic) mount slots
const MNT_LIST_CAP: usize = 32; // upper bound SYS_MOUNT_LIST assembles
const MNT_FILE_MAX: usize = 64 * 1024; // bounded file size through a FAT mount

// mount_ent_t (72 bytes) - one mount, as SYS_MOUNT_LIST reports it. Locked
// against fs/blkmgr.h by _Static_assert and against SZ_MOUNT_ENT in argtab.rs.
#[repr(C)]
#[derive(Clone, Copy)]
struct MountEnt {
    path: [u8; 32],
    fstype: [u8; 8],
    kind: u8,
    index: u8,
    part_index: u8,
    flags: u8,
    _pad: u32,
    total_bytes: u64,
    free_bytes: u64,
    start_lba: u64,
}

const MOUNT_ENT_ZERO: MountEnt = MountEnt {
    path: [0u8; 32],
    fstype: [0u8; 8],
    kind: 0xFF,
    index: 0,
    part_index: 0xFF,
    flags: 0,
    _pad: 0,
    total_bytes: 0,
    free_bytes: 0,
    start_lba: 0,
};

// Internal aux-table slot. Carries the parsed FS geometry so file I/O needs no
// re-parse. Not FFI: fields are whatever the FAT/ext2 layer needs.
#[derive(Clone, Copy)]
struct MountSlot {
    used: bool,
    kind: u8,
    index: u8,
    part_index: u8,
    fstype: u8, // MNT_FS_*
    pstart: u64,
    psize: u64,
    // FAT geometry (partition-relative sectors); ext2 leaves these 0.
    spc: u32,
    reserved: u32,
    fat_size: u32,
    num_fats: u32,
    root_entries: u32,
    root_start: u32,       // first root-dir sector (FAT16)
    root_dir_sectors: u32, // FAT16 root region length
    data_start: u32,       // first data-cluster sector
    clusters: u32,         // count of data clusters
    root_cluster: u32,     // FAT32 root cluster
    is32: bool,
    total_bytes: u64,
    path: [u8; 32],
}

const MOUNT_SLOT_ZERO: MountSlot = MountSlot {
    used: false,
    kind: 0,
    index: 0,
    part_index: 0,
    fstype: 0,
    pstart: 0,
    psize: 0,
    spc: 0,
    reserved: 0,
    fat_size: 0,
    num_fats: 0,
    root_entries: 0,
    root_start: 0,
    root_dir_sectors: 0,
    data_start: 0,
    clusters: 0,
    root_cluster: 0,
    is32: false,
    total_bytes: 0,
    path: [0u8; 32],
};

static mut MNT_TABLE: [MountSlot; MNT_AUX_MAX] = [MOUNT_SLOT_ZERO; MNT_AUX_MAX];

extern "C" {
    // fs/blkmgr_mnt.c: aux-table lock (SMP correctness of MNT_TABLE) + boot-mount
    // accessors over the C globals g_fat_fs / g_ext2.
    fn blkmgr_mnt_lock_c();
    fn blkmgr_mnt_unlock_c();
    fn blkmgr_esp_mounted_c() -> i32;
    fn blkmgr_esp_part_lba_c() -> u32;
    fn blkmgr_esp_total_bytes_c() -> u64;
    fn blkmgr_esp_free_bytes_c() -> u64;
    fn blkmgr_root_ext2_mounted_c() -> i32;
    fn blkmgr_root_is_ext2_c() -> i32;
    fn blkmgr_root_part_lba_c() -> u32;
    fn blkmgr_root_total_bytes_c() -> u64;
}

// --- bounded partition-relative read (mirror of part_write_bounded) ---------
fn part_read_bounded(kind: u8, index: u8, pstart: u64, psize: u64, rel_lba: u64, count: u32, buf: *mut u8) -> i64 {
    let end = match rel_lba.checked_add(count as u64) {
        Some(v) => v,
        None => return BLKMGR_E_RANGE,
    };
    if end > psize {
        return BLKMGR_E_RANGE;
    }
    let abs = match pstart.checked_add(rel_lba) {
        Some(v) => v,
        None => return BLKMGR_E_RANGE,
    };
    blk_rw_core(kind, index, abs, count, buf, false)
}

// ---------------------------------------------------------------------------
// FS detection + geometry parse. Returns a filled slot (used=false, path unset)
// or an error. Reads ONLY (no lock held here; blk_rw_core may block).
// ---------------------------------------------------------------------------
fn parse_fs(kind: u8, index: u8, pstart: u64, psize: u64) -> Result<MountSlot, i64> {
    // ext2? superblock is at partition byte offset 1024 (block 1). The 16-bit
    // magic 0xEF53 lives at superblock offset 56, i.e. sector (pstart+2) byte 56.
    let mut sb = [0u8; SECTOR];
    if part_read_bounded(kind, index, pstart, psize, 2, 1, sb.as_mut_ptr()) == 1 {
        if rd16le(&sb, 56) == 0xEF53 {
            let log_bs = rd32le(&sb, 24);
            let block_size = 1024u32.checked_shl(log_bs).unwrap_or(0);
            let blocks_count = rd32le(&sb, 4);
            if block_size == 0 || blocks_count == 0 {
                return Err(BLKMGR_E_FSBAD);
            }
            let mut s = MOUNT_SLOT_ZERO;
            s.kind = kind;
            s.index = index;
            s.pstart = pstart;
            s.psize = psize;
            s.fstype = MNT_FS_EXT2;
            s.total_bytes = blocks_count as u64 * block_size as u64;
            return Ok(s);
        }
    }
    // FAT? boot sector at partition-relative sector 0.
    let mut bs = [0u8; SECTOR];
    if part_read_bounded(kind, index, pstart, psize, 0, 1, bs.as_mut_ptr()) != 1 {
        return Err(BLKMGR_E_FSBAD);
    }
    if bs[510] != 0x55 || bs[511] != 0xAA {
        return Err(BLKMGR_E_FSBAD);
    }
    let bps = rd16le(&bs, 11) as u32;
    let spc = bs[13] as u32;
    let reserved = rd16le(&bs, 14) as u32;
    let num_fats = bs[16] as u32;
    let root_entries = rd16le(&bs, 17) as u32;
    if bps != 512 || spc == 0 || num_fats == 0 {
        return Err(BLKMGR_E_FSBAD);
    }
    let total16 = rd16le(&bs, 19) as u32;
    let total32 = rd32le(&bs, 32);
    let total = if total16 != 0 { total16 } else { total32 };
    let fat16 = rd16le(&bs, 22) as u32;
    let fat_size = if fat16 != 0 { fat16 } else { rd32le(&bs, 36) };
    let root_dir_sectors = (root_entries * 32 + bps - 1) / bps;
    let root_start = reserved + num_fats * fat_size;
    let data_start = root_start + root_dir_sectors;
    if fat_size == 0 || total <= data_start {
        return Err(BLKMGR_E_FSBAD);
    }
    let clusters = (total - data_start) / spc;
    let is32 = clusters >= 65525;
    let root_cluster = if is32 { rd32le(&bs, 44) } else { 0 };
    let mut s = MOUNT_SLOT_ZERO;
    s.kind = kind;
    s.index = index;
    s.pstart = pstart;
    s.psize = psize;
    s.fstype = if is32 { MNT_FS_FAT32 } else { MNT_FS_FAT16 };
    s.spc = spc;
    s.reserved = reserved;
    s.fat_size = fat_size;
    s.num_fats = num_fats;
    s.root_entries = root_entries;
    s.root_start = root_start;
    s.root_dir_sectors = root_dir_sectors;
    s.data_start = data_start;
    s.clusters = clusters;
    s.root_cluster = root_cluster;
    s.is32 = is32;
    s.total_bytes = clusters as u64 * spc as u64 * SECTOR as u64;
    Ok(s)
}

// ---------------------------------------------------------------------------
// Self-contained FAT (16/32) file layer, ROOT DIRECTORY + 8.3 names only. All
// I/O routes through part_read_bounded / part_write_bounded (blk_rw_core), so
// it never touches g_fat_fs and works on the scratch device. Bounded file size.
// ---------------------------------------------------------------------------

// One FAT entry value for `cluster` (masked to 28 bits on FAT32).
fn fat_get(s: &MountSlot, cluster: u32) -> Result<u32, i64> {
    let width = if s.is32 { 4usize } else { 2usize };
    let byte_off = cluster as usize * width;
    let rel = s.reserved as u64 + (byte_off / SECTOR) as u64;
    let within = byte_off % SECTOR;
    let mut buf = [0u8; SECTOR];
    if part_read_bounded(s.kind, s.index, s.pstart, s.psize, rel, 1, buf.as_mut_ptr()) != 1 {
        return Err(BLKMGR_E_MKFS);
    }
    Ok(if s.is32 {
        rd32le(&buf, within) & 0x0FFF_FFFF
    } else {
        rd16le(&buf, within) as u32
    })
}

// Write `value` into the FAT entry for `cluster` in every FAT copy.
fn fat_set(s: &MountSlot, cluster: u32, value: u32) -> i64 {
    let width = if s.is32 { 4usize } else { 2usize };
    let byte_off = cluster as usize * width;
    let sec_in_fat = (byte_off / SECTOR) as u32;
    let within = byte_off % SECTOR;
    for f in 0..s.num_fats {
        let rel = (s.reserved + f * s.fat_size + sec_in_fat) as u64;
        let mut buf = [0u8; SECTOR];
        if part_read_bounded(s.kind, s.index, s.pstart, s.psize, rel, 1, buf.as_mut_ptr()) != 1 {
            return BLKMGR_E_MKFS;
        }
        if s.is32 {
            let old = rd32le(&buf, within) & 0xF000_0000; // preserve top 4 reserved bits
            put_le32(&mut buf, within, old | (value & 0x0FFF_FFFF));
        } else {
            wr16le(&mut buf, within, value as u16);
        }
        if part_write_bounded(s.kind, s.index, s.pstart, s.psize, rel, 1, buf.as_mut_ptr()) != 1 {
            return BLKMGR_E_MKFS;
        }
    }
    0
}

fn fat_is_eoc(s: &MountSlot, v: u32) -> bool {
    if s.is32 {
        v >= 0x0FFF_FFF8
    } else {
        v >= 0xFFF8
    }
}

// Partition-relative first sector of a data cluster.
fn cluster_sector(s: &MountSlot, cluster: u32) -> u64 {
    s.data_start as u64 + (cluster as u64 - 2) * s.spc as u64
}

// Allocate a chain of `n` free clusters, link them, EOC-terminate the last, and
// return the first cluster. n >= 1.
fn fat_alloc_chain(s: &MountSlot, n: u32) -> Result<u32, i64> {
    let mut chain = [0u32; (MNT_FILE_MAX / SECTOR) + 2]; // bounded by MNT_FILE_MAX
    if n as usize > chain.len() {
        return Err(BLKMGR_E_TOOSMALL);
    }
    let mut found = 0u32;
    let mut c = 2u32;
    let last_cluster = s.clusters + 1; // clusters are numbered 2..=clusters+1
    while c <= last_cluster && found < n {
        if fat_get(s, c)? == 0 {
            chain[found as usize] = c;
            found += 1;
        }
        c += 1;
    }
    if found < n {
        return Err(BLKMGR_E_TOOSMALL); // not enough free clusters
    }
    for i in 0..n {
        let next = if i + 1 < n {
            chain[(i + 1) as usize]
        } else if s.is32 {
            0x0FFF_FFFF
        } else {
            0xFFFF
        };
        let rc = fat_set(s, chain[i as usize], next);
        if rc != 0 {
            return Err(rc);
        }
    }
    Ok(chain[0])
}

// Iterate every 32-byte root-directory entry, calling `f(abs-safe rel_sector,
// offset, &sector_bytes)`. Returns Ok(Some((rel_sector, offset))) if `f` returns
// true for a slot (the caller then re-reads/writes that sector), else Ok(None).
// Handles FAT16 fixed region and the FAT32 root cluster chain (bounded).
fn root_dir_find_slot<F: FnMut(&[u8; SECTOR], usize) -> bool>(
    s: &MountSlot,
    mut f: F,
) -> Result<Option<u64>, i64> {
    if !s.is32 {
        for sec in 0..s.root_dir_sectors {
            let rel = (s.root_start + sec) as u64;
            let mut buf = [0u8; SECTOR];
            if part_read_bounded(s.kind, s.index, s.pstart, s.psize, rel, 1, buf.as_mut_ptr()) != 1 {
                return Err(BLKMGR_E_MKFS);
            }
            let mut off = 0usize;
            while off + 32 <= SECTOR {
                if f(&buf, off) {
                    return Ok(Some(rel));
                }
                off += 32;
            }
        }
        Ok(None)
    } else {
        let mut cl = s.root_cluster;
        let mut guard = 0u32;
        while cl >= 2 && !fat_is_eoc(s, cl) && guard < 4096 {
            for so in 0..s.spc {
                let rel = cluster_sector(s, cl) + so as u64;
                let mut buf = [0u8; SECTOR];
                if part_read_bounded(s.kind, s.index, s.pstart, s.psize, rel, 1, buf.as_mut_ptr()) != 1 {
                    return Err(BLKMGR_E_MKFS);
                }
                let mut off = 0usize;
                while off + 32 <= SECTOR {
                    if f(&buf, off) {
                        return Ok(Some(rel));
                    }
                    off += 32;
                }
            }
            cl = fat_get(s, cl)?;
            guard += 1;
        }
        Ok(None)
    }
}

// Pack an ASCII name into an 8.3 directory name field (11 bytes, space padded,
// upper-cased). Rejects names that do not fit 8.3.
fn pack_83(name: &[u8], out: &mut [u8; 11]) -> bool {
    *out = [b' '; 11];
    let mut i = 0usize; // name part
    let mut dot = None;
    for (k, &c) in name.iter().enumerate() {
        if c == 0 {
            break;
        }
        if c == b'.' {
            dot = Some(k);
            break;
        }
    }
    let (base, ext): (&[u8], &[u8]) = match dot {
        Some(d) => {
            let mut e_end = name.len();
            for (k, &c) in name.iter().enumerate().skip(d + 1) {
                if c == 0 {
                    e_end = k;
                    break;
                }
            }
            (&name[..d], &name[d + 1..e_end])
        }
        None => {
            let mut b_end = name.len();
            for (k, &c) in name.iter().enumerate() {
                if c == 0 {
                    b_end = k;
                    break;
                }
            }
            (&name[..b_end], &[][..])
        }
    };
    if base.is_empty() || base.len() > 8 || ext.len() > 3 {
        return false;
    }
    for &c in base {
        let u = if c >= b'a' && c <= b'z' { c - 32 } else { c };
        out[i] = u;
        i += 1;
    }
    let mut j = 8usize;
    for &c in ext {
        let u = if c >= b'a' && c <= b'z' { c - 32 } else { c };
        out[j] = u;
        j += 1;
    }
    true
}

// Look up a file by 8.3 name in the root directory; returns (first_cluster, size).
fn fat_lookup(s: &MountSlot, name11: &[u8; 11]) -> Result<Option<(u32, u32)>, i64> {
    let mut result: Option<(u32, u32)> = None;
    root_dir_find_slot(s, |buf, off| {
        let d = &buf[off..off + 32];
        if d[0] == 0x00 || d[0] == 0xE5 {
            return false;
        }
        if d[11] & 0x08 != 0 {
            return false; // volume label
        }
        if &d[0..11] == &name11[..] {
            let lo = rd16le(d, 26) as u32;
            let hi = rd16le(d, 20) as u32;
            let first = if s.is32 { (hi << 16) | lo } else { lo };
            let size = rd32le(d, 28);
            result = Some((first, size));
            return true;
        }
        false
    })?;
    Ok(result)
}

// Write (create or overwrite) a root-directory file. Bounded size. Returns the
// number of bytes written (>=0) or a negative sentinel.
fn fat_write_file(s: &MountSlot, name: &[u8], data: &[u8]) -> i64 {
    if data.len() > MNT_FILE_MAX {
        return BLKMGR_E_TOOSMALL;
    }
    let mut n11 = [0u8; 11];
    if !pack_83(name, &mut n11) {
        return BLKMGR_E_BADPATH;
    }
    // Refuse overwrite in this cut (keeps free-cluster bookkeeping simple): a
    // name that already exists is an error rather than a leak of its old chain.
    match fat_lookup(s, &n11) {
        Ok(Some(_)) => return BLKMGR_E_INUSE,
        Ok(None) => {}
        Err(e) => return e,
    }
    let cluster_bytes = s.spc as usize * SECTOR;
    let nclusters = if data.is_empty() {
        0
    } else {
        ((data.len() + cluster_bytes - 1) / cluster_bytes) as u32
    };
    let first = if nclusters > 0 {
        match fat_alloc_chain(s, nclusters) {
            Ok(c) => c,
            Err(e) => return e,
        }
    } else {
        0
    };
    // Write data cluster by cluster, following the freshly-built chain.
    if nclusters > 0 {
        let cbuf = unsafe { kmalloc(cluster_bytes) } as *mut u8;
        if cbuf.is_null() {
            return E_FAULT;
        }
        let mut cl = first;
        let mut pos = 0usize;
        let mut rc: i64 = 0;
        for _ in 0..nclusters {
            unsafe { core::ptr::write_bytes(cbuf, 0, cluster_bytes) };
            let take = core::cmp::min(cluster_bytes, data.len() - pos);
            unsafe {
                core::ptr::copy_nonoverlapping(data.as_ptr().add(pos), cbuf, take);
            }
            let rel = cluster_sector(s, cl);
            if part_write_bounded(s.kind, s.index, s.pstart, s.psize, rel, s.spc, cbuf) != s.spc as i64 {
                rc = BLKMGR_E_MKFS;
                break;
            }
            pos += take;
            match fat_get(s, cl) {
                Ok(nx) => cl = nx,
                Err(e) => {
                    rc = e;
                    break;
                }
            }
        }
        unsafe { kfree(cbuf as *mut c_void) };
        if rc != 0 {
            return rc;
        }
    }
    // Create the directory entry in a free root slot.
    let dirsec = match root_dir_find_slot(s, |buf, off| buf[off] == 0x00 || buf[off] == 0xE5) {
        Ok(Some(rel)) => rel,
        Ok(None) => return BLKMGR_E_FULL, // root directory full
        Err(e) => return e,
    };
    let mut dbuf = [0u8; SECTOR];
    if part_read_bounded(s.kind, s.index, s.pstart, s.psize, dirsec, 1, dbuf.as_mut_ptr()) != 1 {
        return BLKMGR_E_MKFS;
    }
    let mut off = 0usize;
    while off + 32 <= SECTOR {
        if dbuf[off] == 0x00 || dbuf[off] == 0xE5 {
            break;
        }
        off += 32;
    }
    if off + 32 > SECTOR {
        return BLKMGR_E_FULL;
    }
    for k in 0..32 {
        dbuf[off + k] = 0;
    }
    dbuf[off..off + 11].copy_from_slice(&n11);
    dbuf[off + 11] = 0x20; // ATTR_ARCHIVE
    wr16le(&mut dbuf, off + 26, (first & 0xFFFF) as u16); // first cluster low
    wr16le(&mut dbuf, off + 20, ((first >> 16) & 0xFFFF) as u16); // first cluster high
    put_le32(&mut dbuf, off + 28, data.len() as u32); // file size
    if part_write_bounded(s.kind, s.index, s.pstart, s.psize, dirsec, 1, dbuf.as_mut_ptr()) != 1 {
        return BLKMGR_E_MKFS;
    }
    data.len() as i64
}

// Read a root-directory file by name into `out` (up to out.len()). Returns bytes
// copied, BLKMGR_E_NOMNT if the file is absent, or a negative sentinel.
fn fat_read_file(s: &MountSlot, name: &[u8], out: &mut [u8]) -> i64 {
    let mut n11 = [0u8; 11];
    if !pack_83(name, &mut n11) {
        return BLKMGR_E_BADPATH;
    }
    let (first, size) = match fat_lookup(s, &n11) {
        Ok(Some(v)) => v,
        Ok(None) => return BLKMGR_E_NOMNT,
        Err(e) => return e,
    };
    let want = core::cmp::min(size as usize, out.len());
    if want == 0 || first < 2 {
        return 0;
    }
    let cluster_bytes = s.spc as usize * SECTOR;
    let cbuf = unsafe { kmalloc(cluster_bytes) } as *mut u8;
    if cbuf.is_null() {
        return E_FAULT;
    }
    let mut cl = first;
    let mut pos = 0usize;
    let mut guard = 0u32;
    let mut rc: i64 = 0;
    while pos < want && cl >= 2 && !fat_is_eoc(s, cl) && guard < 1_000_000 {
        let rel = cluster_sector(s, cl);
        if part_read_bounded(s.kind, s.index, s.pstart, s.psize, rel, s.spc, cbuf) != s.spc as i64 {
            rc = BLKMGR_E_MKFS;
            break;
        }
        let take = core::cmp::min(cluster_bytes, want - pos);
        unsafe {
            core::ptr::copy_nonoverlapping(cbuf, out.as_mut_ptr().add(pos), take);
        }
        pos += take;
        match fat_get(s, cl) {
            Ok(nx) => cl = nx,
            Err(e) => {
                rc = e;
                break;
            }
        }
        guard += 1;
    }
    unsafe { kfree(cbuf as *mut c_void) };
    if rc != 0 {
        rc
    } else {
        pos as i64
    }
}

// ---------------------------------------------------------------------------
// Path helpers + aux-table cores. Dynamic mount points are STRUCTURALLY confined
// to a "/MNT/" prefix, so a dynamic mount can never shadow "/" or "/boot", and
// SYS_UMOUNT can never name a boot mount.
// ---------------------------------------------------------------------------
fn path_len(p: &[u8; 32]) -> usize {
    p.iter().position(|&b| b == 0).unwrap_or(32)
}

fn path_is_mnt(p: &[u8; 32]) -> bool {
    let n = path_len(p);
    // "/MNT/x" -> at least 6 chars, prefix "/MNT/", at least one trailing char.
    if n < 6 || n > 31 {
        return false;
    }
    if &p[0..5] != b"/MNT/" {
        return false;
    }
    // No embedded control chars; printable ASCII only.
    for &c in &p[0..n] {
        if c < 0x20 || c > 0x7E {
            return false;
        }
    }
    true
}

fn path_eq(a: &[u8; 32], b: &[u8; 32]) -> bool {
    a[..] == b[..]
}

fn fstype_str(fstype: u8) -> &'static [u8] {
    match fstype {
        MNT_FS_FAT16 => b"fat16",
        MNT_FS_FAT32 => b"fat32",
        MNT_FS_EXT2 => b"ext2",
        _ => b"?",
    }
}

fn set_fstype_field(dst: &mut [u8; 8], src: &[u8]) {
    *dst = [0u8; 8];
    let n = core::cmp::min(src.len(), 7);
    dst[..n].copy_from_slice(&src[..n]);
}

/// Mount `part_index` of device (kind,index) at `path` (a /MNT/... path already
/// copied into a fixed buffer). Parses geometry OUTSIDE the lock (device I/O may
/// block), then takes the lock only to reserve a slot. All safety gates first.
fn mount_core(kind: u8, index: u8, part_index: i32, path: &[u8; 32]) -> i64 {
    if !path_is_mnt(path) {
        return BLKMGR_E_BADPATH;
    }
    let r = match resolve(kind, index) {
        Some(r) => r,
        None => return BLKMGR_E_NODEV,
    };
    // Never mount from the boot disk in this cut (its partitions are already the
    // live root/ESP; a redundant aux mount of them buys nothing and the boot-disk
    // lockout is the load-bearing invariant everywhere else). Structural refusal.
    if r.is_boot {
        return BLKMGR_E_BOOTDISK;
    }
    if kind == KIND_USB && unsafe { hotplug_vol_busy(index as i32) } > 0 {
        return BLKMGR_E_BUSY;
    }
    let (pstart, psize) = match resolve_partition(kind, index, part_index, r.sectors) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let mut slot = match parse_fs(kind, index, pstart, psize) {
        Ok(s) => s,
        Err(e) => return e,
    };
    slot.part_index = part_index as u8;
    slot.path = *path;
    slot.used = true;
    // Reserve a slot under the lock (no device I/O held).
    unsafe { blkmgr_mnt_lock_c() };
    let mut rc: i64 = BLKMGR_E_FULL;
    let table = unsafe { &mut *core::ptr::addr_of_mut!(MNT_TABLE) };
    let mut collide = false;
    for e in table.iter() {
        if e.used && path_eq(&e.path, path) {
            collide = true;
        }
    }
    if collide {
        rc = BLKMGR_E_INUSE;
    } else {
        for e in table.iter_mut() {
            if !e.used {
                *e = slot;
                rc = 0;
                break;
            }
        }
    }
    unsafe { blkmgr_mnt_unlock_c() };
    rc
}

/// Unmount an aux mount by path. Refuses any path that is not a /MNT/ dynamic
/// mount (so a boot mount can never be targeted), and any path not present.
fn umount_core(path: &[u8; 32]) -> i64 {
    if !path_is_mnt(path) {
        return BLKMGR_E_BADPATH; // covers "/", "/boot" and every boot path
    }
    unsafe { blkmgr_mnt_lock_c() };
    let table_um = unsafe { &mut *core::ptr::addr_of_mut!(MNT_TABLE) };
    let mut rc: i64 = BLKMGR_E_NOMNT;
    for e in table_um.iter_mut() {
        if e.used && path_eq(&e.path, path) {
            *e = MOUNT_SLOT_ZERO;
            rc = 0;
            break;
        }
    }
    unsafe { blkmgr_mnt_unlock_c() };
    rc
}

/// Copy the aux table into `out` and return the count. Snapshots under the lock.
fn aux_snapshot(out: &mut [MountSlot; MNT_AUX_MAX]) -> usize {
    unsafe { blkmgr_mnt_lock_c() };
    let table = unsafe { &*core::ptr::addr_of!(MNT_TABLE) };
    let mut n = 0usize;
    for e in table.iter() {
        if e.used {
            out[n] = *e;
            n += 1;
        }
    }
    unsafe { blkmgr_mnt_unlock_c() };
    n
}

/// Assemble the full mount list (boot mounts + aux mounts) into `out`; returns
/// the count. No device I/O and no blocking (boot accessors read C globals).
fn mount_list_core(out: &mut [MountEnt]) -> usize {
    let mut n = 0usize;
    let cap = out.len();
    let boot_kind = unsafe { inst_boot_kind() } as i64;
    let boot_index = unsafe { inst_boot_index() } as i64;
    let root_ext2 = unsafe { blkmgr_root_ext2_mounted_c() } != 0;

    // ext2 root at "/".
    if root_ext2 && n < cap {
        let mut e = MOUNT_ENT_ZERO;
        e.path[0] = b'/';
        set_fstype_field(&mut e.fstype, b"ext2");
        e.kind = boot_kind as u8;
        e.index = boot_index as u8;
        e.part_index = 0xFF;
        e.flags = MNT_F_BOOT | MNT_F_ROOT;
        e.total_bytes = unsafe { blkmgr_root_total_bytes_c() };
        e.start_lba = unsafe { blkmgr_root_part_lba_c() } as u64;
        out[n] = e;
        n += 1;
    }
    // FAT ESP: "/boot" when ext2 is the root, else it IS the root "/".
    if unsafe { blkmgr_esp_mounted_c() } != 0 && n < cap {
        let mut e = MOUNT_ENT_ZERO;
        if root_ext2 {
            e.path[..5].copy_from_slice(b"/boot");
        } else {
            e.path[0] = b'/';
        }
        set_fstype_field(&mut e.fstype, b"fat");
        e.kind = boot_kind as u8;
        e.index = boot_index as u8;
        e.part_index = 0xFF;
        e.flags = MNT_F_BOOT | if root_ext2 { 0 } else { MNT_F_ROOT };
        e.total_bytes = unsafe { blkmgr_esp_total_bytes_c() };
        e.free_bytes = unsafe { blkmgr_esp_free_bytes_c() };
        e.start_lba = unsafe { blkmgr_esp_part_lba_c() } as u64;
        out[n] = e;
        n += 1;
    }
    // Dynamic aux mounts.
    let mut snap = [MOUNT_SLOT_ZERO; MNT_AUX_MAX];
    let m = aux_snapshot(&mut snap);
    for slot in snap.iter().take(m) {
        if n >= cap {
            break;
        }
        let mut e = MOUNT_ENT_ZERO;
        let pl = path_len(&slot.path);
        e.path[..pl].copy_from_slice(&slot.path[..pl]);
        set_fstype_field(&mut e.fstype, fstype_str(slot.fstype));
        e.kind = slot.kind;
        e.index = slot.index;
        e.part_index = slot.part_index;
        e.flags = MNT_F_DYNAMIC | if slot.fstype == MNT_FS_EXT2 { MNT_F_RO } else { 0 };
        e.total_bytes = slot.total_bytes;
        e.start_lba = slot.pstart;
        out[n] = e;
        n += 1;
    }
    n
}

// ---------------------------------------------------------------------------
// Syscall wrappers (Ring 3). Root-only enforced HERE; the argtab validated the
// user pointers at entry, and copy_to_user / copy_from_user contain any fault.
// ---------------------------------------------------------------------------
#[no_mangle]
pub extern "C" fn sys_mount_list_rs(u_buf: *mut c_void, max: i32, elem_size: i32) -> i64 {
    if elem_size as usize != core::mem::size_of::<MountEnt>() {
        return E_INVAL;
    }
    if max <= 0 || u_buf.is_null() {
        return 0;
    }
    let want = core::cmp::min(max as usize, MNT_LIST_CAP);
    let mut ents = [MOUNT_ENT_ZERO; MNT_LIST_CAP];
    let n = mount_list_core(&mut ents);
    let out_n = core::cmp::min(n, want);
    if out_n > 0 {
        let bytes = out_n * core::mem::size_of::<MountEnt>();
        if unsafe { copy_to_user(u_buf, ents.as_ptr() as *const c_void, bytes) } != 0 {
            return E_FAULT;
        }
    }
    out_n as i64
}

fn copy_path_from_user(u_path: *const c_void) -> Result<[u8; 32], i64> {
    if u_path.is_null() {
        return Err(BLKMGR_E_BADPATH);
    }
    let mut buf = [0u8; 32];
    if unsafe { copy_from_user(buf.as_mut_ptr() as *mut c_void, u_path, 31) } != 0 {
        return Err(E_FAULT);
    }
    buf[31] = 0;
    Ok(buf)
}

#[no_mangle]
pub extern "C" fn sys_mount_rs(kind: i32, index: i32, part_index: i32, u_path: *const c_void) -> i64 {
    if unsafe { syscall_caller_euid() } != 0 {
        return E_PERM;
    }
    if !(0..=255).contains(&kind) || !(0..=255).contains(&index) {
        return E_INVAL;
    }
    let path = match copy_path_from_user(u_path) {
        Ok(p) => p,
        Err(e) => return e,
    };
    mount_core(kind as u8, index as u8, part_index, &path)
}

#[no_mangle]
pub extern "C" fn sys_umount_rs(u_path: *const c_void) -> i64 {
    if unsafe { syscall_caller_euid() } != 0 {
        return E_PERM;
    }
    let path = match copy_path_from_user(u_path) {
        Ok(p) => p,
        Err(e) => return e,
    };
    umount_core(&path)
}

// ---------------------------------------------------------------------------
// #404 Stage 4b: KERNEL-callable mount/unmount + scratch prep, for the boot VFS
// oracle (blkmgr_vfs_ext2_selftest in fdlayer.c). These take a KERNEL string,
// not a user pointer, and mount ONLY the scratch device, so they can never be
// steered at a real disk from a boot self-test.
// ---------------------------------------------------------------------------
fn kpath_to_buf(u_path: *const u8) -> Option<[u8; 32]> {
    if u_path.is_null() {
        return None;
    }
    let plen = unsafe { kstr_len(u_path, 32) };
    if plen == 0 || plen > 31 {
        return None;
    }
    let mut path = [0u8; 32];
    for i in 0..plen {
        path[i] = unsafe { *u_path.add(i) };
    }
    Some(path)
}

#[no_mangle]
pub extern "C" fn blkmgr_mount_scratch_c(part_index: i32, u_path: *const u8) -> i64 {
    match kpath_to_buf(u_path) {
        Some(p) => mount_core(KIND_SCRATCH, 0, part_index, &p),
        None => BLKMGR_E_BADPATH,
    }
}

#[no_mangle]
pub extern "C" fn blkmgr_umount_kernel_c(u_path: *const u8) -> i64 {
    match kpath_to_buf(u_path) {
        Some(p) => umount_core(&p),
        None => BLKMGR_E_BADPATH,
    }
}

/// Lay down a fresh 2-partition GPT (FAT p1, ext2 p2) on the scratch and mkfs
/// ext2 on p2, so the VFS oracle has a known-good ext2 aux partition. Returns
/// the ext2 partition index (1) or a negative error.
#[no_mangle]
pub extern "C" fn blkmgr_vfs_scratch_prep_rs() -> i32 {
    if scratch_base() == 0 {
        return -1;
    }
    let sec = scratch_sectors();
    let p1_start: u64 = 2048;
    let p1_size: u64 = 16384;
    let p2_start: u64 = p1_start + p1_size;
    let p2_size: u64 = sec.saturating_sub(p2_start + 40);
    let layout = [
        spec(GUID_DATA, p1_start, p1_size, b"FATP"),
        spec(GUID_LINUX, p2_start, p2_size, b"EXTP"),
    ];
    if write_gpt(KIND_SCRATCH, 0, sec, &layout) != 0 {
        return -2;
    }
    if mkfs_core(KIND_SCRATCH, 0, 1, MKFS_EXT2, b"vfstest") != 0 {
        return -3;
    }
    1
}

// ===========================================================================
// #404 Stage 4b: the C face the VFS path resolver (proc/fdlayer.c) needs.
//
// These let the kernel's ordinary file syscalls (open/read/write/readdir on a
// "/MNT/<name>/..." path) resolve THROUGH an aux mount without fdlayer.c ever
// touching the Rust mount table directly:
//   - blkmgr_aux_resolve_c()  : longest-prefix match a kernel path against the
//     aux table; returns the device coords + fstype + where the mount-relative
//     part of the path begins.
//   - blkmgr_aux_fat_*_c()    : the FAT (16/32) root-directory file ops the
//     resolver routes a FAT aux mount to, reusing the SAME self-contained FAT
//     layer the Stage-4 oracle proved. ext2 aux mounts are served by the REAL
//     ext2 driver (fs/ext2.c ext2_*_on), not from here.
// ===========================================================================

// aux_mnt_info_t mirror (fs/blkmgr.h). sizeof-locked below.
#[repr(C)]
#[derive(Clone, Copy)]
struct AuxMntInfo {
    found: u8,
    fstype: u8,   // MNT_FS_FAT16 / FAT32 / EXT2
    kind: u8,
    index: u8,
    rel_off: u32, // byte offset into the caller path where the mount-relative part starts
    pstart: u64,
    psize: u64,
}
const _: () = assert!(core::mem::size_of::<AuxMntInfo>() == 24);

// Length of a NUL-terminated kernel C string, capped.
unsafe fn kstr_len(p: *const u8, cap: usize) -> usize {
    let mut n = 0usize;
    while n < cap && *p.add(n) != 0 {
        n += 1;
    }
    n
}

/// Does the aux mount whose path buffer is `mp` (len `mlen`) prefix-match the
/// caller path `path[0..plen]`? A match is an EXACT equal, or the path continues
/// with '/', so "/MNT/test" matches "/MNT/test" and "/MNT/test/hello" but never
/// "/MNT/testing".
fn aux_prefix_match(mp: &[u8; 32], mlen: usize, path: &[u8], plen: usize) -> bool {
    if plen < mlen {
        return false;
    }
    if &mp[..mlen] != &path[..mlen] {
        return false;
    }
    plen == mlen || path[mlen] == b'/'
}

/// Longest-prefix resolve `u_path` (a KERNEL C string) against the aux table.
/// Returns 1 and fills *out on a match, 0 otherwise. No user memory involved.
#[no_mangle]
pub extern "C" fn blkmgr_aux_resolve_c(u_path: *const u8, out: *mut c_void) -> i32 {
    if u_path.is_null() || out.is_null() {
        return 0;
    }
    let plen = unsafe { kstr_len(u_path, 512) };
    // Read the path bytes into a local buffer (bounded; mount paths are <=31).
    let mut path = [0u8; 512];
    for i in 0..plen {
        path[i] = unsafe { *u_path.add(i) };
    }
    let mut snap = [MOUNT_SLOT_ZERO; MNT_AUX_MAX];
    let m = aux_snapshot(&mut snap);
    let mut best: Option<(usize, MountSlot)> = None;
    for s in snap.iter().take(m) {
        let mlen = path_len(&s.path);
        if aux_prefix_match(&s.path, mlen, &path[..plen], plen) {
            match best {
                Some((bl, _)) if bl >= mlen => {}
                _ => best = Some((mlen, *s)),
            }
        }
    }
    let (mlen, s) = match best {
        Some(v) => v,
        None => return 0,
    };
    let info = AuxMntInfo {
        found: 1,
        fstype: s.fstype,
        kind: s.kind,
        index: s.index,
        rel_off: mlen as u32,
        pstart: s.pstart,
        psize: s.psize,
    };
    unsafe { core::ptr::write(out as *mut AuxMntInfo, info) };
    1
}

// Find the aux slot for a mount path (a KERNEL C string, e.g. "/MNT/test").
fn aux_slot_for_kpath(u_mnt: *const u8) -> Option<MountSlot> {
    if u_mnt.is_null() {
        return None;
    }
    let plen = unsafe { kstr_len(u_mnt, 32) };
    let mut want = [0u8; 32];
    if plen == 0 || plen > 31 {
        return None;
    }
    for i in 0..plen {
        want[i] = unsafe { *u_mnt.add(i) };
    }
    let mut snap = [MOUNT_SLOT_ZERO; MNT_AUX_MAX];
    let m = aux_snapshot(&mut snap);
    for s in snap.iter().take(m) {
        if path_len(&s.path) == plen && s.path[..plen] == want[..plen] {
            return Some(*s);
        }
    }
    None
}

// Strip a leading '/' from a relative-path kernel string and return its bytes.
// The Rust FAT layer is root-dir + 8.3 only, so only a single bare name works.
unsafe fn rel_name_bytes(u_rel: *const u8, out: &mut [u8; 64]) -> usize {
    if u_rel.is_null() {
        return 0;
    }
    let mut p = u_rel;
    if *p == b'/' {
        p = p.add(1);
    }
    let n = core::cmp::min(kstr_len(p, 64), 63);
    for i in 0..n {
        out[i] = *p.add(i);
    }
    n
}

/// FAT aux: read a root-directory file (mount path `u_mnt`, relative `u_rel`)
/// into `buf[0..cap]`. Returns bytes copied, negative on error.
#[no_mangle]
pub extern "C" fn blkmgr_aux_fat_read_c(u_mnt: *const u8, u_rel: *const u8,
                                        buf: *mut c_void, cap: u32) -> i64 {
    let s = match aux_slot_for_kpath(u_mnt) {
        Some(s) => s,
        None => return BLKMGR_E_NOMNT,
    };
    let mut name = [0u8; 64];
    let nn = unsafe { rel_name_bytes(u_rel, &mut name) };
    if nn == 0 {
        return BLKMGR_E_BADPATH;
    }
    let out = unsafe { core::slice::from_raw_parts_mut(buf as *mut u8, cap as usize) };
    fat_read_file(&s, &name[..nn], out)
}

/// FAT aux: write (create; overwrite refused) a root-directory file. Returns
/// bytes written, negative on error.
#[no_mangle]
pub extern "C" fn blkmgr_aux_fat_write_c(u_mnt: *const u8, u_rel: *const u8,
                                         buf: *const c_void, len: u32) -> i64 {
    let s = match aux_slot_for_kpath(u_mnt) {
        Some(s) => s,
        None => return BLKMGR_E_NOMNT,
    };
    let mut name = [0u8; 64];
    let nn = unsafe { rel_name_bytes(u_rel, &mut name) };
    if nn == 0 {
        return BLKMGR_E_BADPATH;
    }
    let data = unsafe { core::slice::from_raw_parts(buf as *const u8, len as usize) };
    fat_write_file(&s, &name[..nn], data)
}

/// FAT aux: iterate the ROOT directory. `*pos` is a caller cursor (start 0), a
/// 32-byte-entry index into the flattened root directory. Fills name (8.3, up to
/// name_max), *is_dir, *size, advances *pos. Returns 0 (entry) or -1 (end).
#[no_mangle]
pub extern "C" fn blkmgr_aux_fat_readdir_c(u_mnt: *const u8, pos: *mut u32,
                                           name_out: *mut u8, name_max: i32,
                                           is_dir: *mut i32, size_out: *mut u32) -> i32 {
    let s = match aux_slot_for_kpath(u_mnt) {
        Some(s) => s,
        None => return -1,
    };
    if pos.is_null() || name_out.is_null() || name_max < 13 {
        return -1;
    }
    let mut idx = unsafe { *pos };
    let mut dbuf = [0u8; SECTOR];
    let per_sec = (SECTOR / 32) as u32;
    // Total number of 32-byte root slots to scan. FAT16: root_dir_sectors.
    // FAT32: follow the root cluster chain, but cap the walk.
    loop {
        // Locate the sector + in-sector offset for entry `idx`.
        let (rel, off) = if !s.is32 {
            let sec = idx / per_sec;
            if sec >= s.root_dir_sectors {
                return -1;
            }
            (s.root_start as u64 + sec as u64, (idx % per_sec) as usize * 32)
        } else {
            // Walk the root cluster chain to the cluster holding entry `idx`.
            let ents_per_cluster = per_sec * s.spc;
            let cl_index = idx / ents_per_cluster;
            let within = idx % ents_per_cluster;
            let mut cl = s.root_cluster;
            let mut steps = 0u32;
            while steps < cl_index {
                match fat_get(&s, cl) {
                    Ok(nx) if nx >= 2 && !fat_is_eoc(&s, nx) => cl = nx,
                    _ => return -1,
                }
                steps += 1;
            }
            let sec_in_cl = within / per_sec;
            (cluster_sector(&s, cl) + sec_in_cl as u64, (within % per_sec) as usize * 32)
        };
        if part_read_bounded(s.kind, s.index, s.pstart, s.psize, rel, 1, dbuf.as_mut_ptr()) != 1 {
            return -1;
        }
        let b = &dbuf[off..off + 32];
        idx += 1;
        if b[0] == 0x00 {
            unsafe { *pos = idx };
            return -1; // end of directory
        }
        if b[0] == 0xE5 {
            continue; // deleted
        }
        let attr = b[11];
        if attr & 0x08 != 0 {
            continue; // volume label
        }
        if attr & 0x0F == 0x0F {
            continue; // LFN entry
        }
        // Build an 8.3 name "NAME.EXT" (lowercase not applied; matches on-disk).
        let mut nm = [0u8; 13];
        let mut w = 0usize;
        for k in 0..8 {
            if b[k] == b' ' {
                break;
            }
            nm[w] = b[k];
            w += 1;
        }
        if b[8] != b' ' {
            nm[w] = b'.';
            w += 1;
            for k in 8..11 {
                if b[k] == b' ' {
                    break;
                }
                nm[w] = b[k];
                w += 1;
            }
        }
        let cap = core::cmp::min(w, (name_max - 1) as usize);
        for k in 0..cap {
            unsafe { *name_out.add(k) = nm[k] };
        }
        unsafe { *name_out.add(cap) = 0 };
        if !is_dir.is_null() {
            unsafe { *is_dir = if attr & 0x10 != 0 { 1 } else { 0 } };
        }
        if !size_out.is_null() {
            unsafe { *size_out = rd32le(&dbuf, off + 28) };
        }
        unsafe { *pos = idx };
        return 0;
    }
}

// ===========================================================================
// STAGE 4 ORACLE. Marker-gated (/DISKMGR.TST). On the SCRATCH device only:
// partition + mkfs FAT16, MOUNT it at /MNT/scratch, write a file through the
// mount, read it back and compare, confirm SYS_MOUNT_LIST reflects the mount,
// UNMOUNT, confirm the list no longer shows it, and prove the refusals FIRE
// (unmount of "/" refused; mount of the real boot disk refused; a bad path
// refused). Runs the CORE functions directly (no euid/user-pointer path), like
// the mkfs oracle. Returns 0 iff every check passed.
// ===========================================================================
#[no_mangle]
pub extern "C" fn blkmgr_mount_selftest_rs(out_passed: *mut u32, out_total: *mut u32) -> i32 {
    let mut passed: u32 = 0;
    let mut total: u32 = 0;
    macro_rules! check {
        ($cond:expr, $label:expr) => {{
            total += 1;
            if $cond {
                passed += 1;
            } else {
                unsafe { kprintf(b"[DISKMGR] mount oracle FAIL: %s\n\0".as_ptr(), ($label).as_ptr()); }
            }
        }};
    }
    if scratch_base() == 0 {
        unsafe { kprintf(b"[DISKMGR] mount oracle: scratch absent, cannot run\n\0".as_ptr()) };
        if !out_passed.is_null() { unsafe { *out_passed = 0 }; }
        if !out_total.is_null() { unsafe { *out_total = 1 }; }
        return -1;
    }
    let sec = scratch_sectors();

    // Fresh 2-partition table: p1 FAT16 8 MiB, p2 ext2 the rest.
    let p1_start: u64 = 2048;
    let p1_size: u64 = 16384;
    let p2_start: u64 = p1_start + p1_size;
    let p2_size: u64 = sec.saturating_sub(p2_start + 40);
    let layout = [
        spec(GUID_DATA, p1_start, p1_size, b"FATP"),
        spec(GUID_LINUX, p2_start, p2_size, b"EXTP"),
    ];
    check!(write_gpt(KIND_SCRATCH, 0, sec as u64, &layout) == 0, b"write GPT\0");
    check!(mkfs_core(KIND_SCRATCH, 0, 0, MKFS_FAT, b"MNTFAT") == 0, b"mkfs FAT16 p1\0");
    check!(mkfs_core(KIND_SCRATCH, 0, 1, MKFS_EXT2, b"mntext2") == 0, b"mkfs ext2 p2\0");

    // MOUNT the FAT partition at /MNT/scratch.
    let mut mp = [0u8; 32];
    mp[..12].copy_from_slice(b"/MNT/scratch");
    check!(mount_core(KIND_SCRATCH, 0, 0, &mp) == 0, b"mount FAT p1 at /MNT/scratch\0");

    // SYS_MOUNT_LIST reflects the aux mount.
    let mut ents = [MOUNT_ENT_ZERO; MNT_LIST_CAP];
    let n = mount_list_core(&mut ents);
    let mut found = false;
    for e in ents.iter().take(n) {
        if &e.path[..12] == b"/MNT/scratch" && (e.flags & MNT_F_DYNAMIC) != 0 {
            found = true;
        }
    }
    check!(found, b"mount list shows /MNT/scratch\0");

    // Read/write a file THROUGH the mount. Snapshot the slot the way the future
    // file syscalls will, then exercise the FAT file layer.
    let mut snap = [MOUNT_SLOT_ZERO; MNT_AUX_MAX];
    let m = aux_snapshot(&mut snap);
    let mut slot_opt: Option<MountSlot> = None;
    for s in snap.iter().take(m) {
        if &s.path[..12] == b"/MNT/scratch" {
            slot_opt = Some(*s);
        }
    }
    check!(slot_opt.is_some(), b"aux slot resolvable\0");
    if let Some(slot) = slot_opt {
        let payload = b"MayteraOS disk manager stage 4: mount read/write proof.\n";
        let w = fat_write_file(&slot, b"HELLO.TXT", payload);
        check!(w == payload.len() as i64, b"write file through mount\0");
        let mut rb = [0u8; 128];
        let r = fat_read_file(&slot, b"HELLO.TXT", &mut rb);
        check!(r == payload.len() as i64, b"read file length matches\0");
        check!(&rb[..payload.len()] == &payload[..], b"read file content matches\0");
        // A second file, to exercise a fresh dirent + chain.
        let w2 = fat_write_file(&slot, b"README.MD", b"second file");
        check!(w2 == 11, b"second file write\0");
        let mut rb2 = [0u8; 32];
        let r2 = fat_read_file(&slot, b"README.MD", &mut rb2);
        check!(r2 == 11 && &rb2[..11] == b"second file", b"second file read\0");
        // Absent file refused.
        let rmiss = fat_read_file(&slot, b"NOPE.TXT", &mut rb2);
        check!(rmiss == BLKMGR_E_NOMNT, b"absent file refused\0");
    }

    // UNMOUNT and confirm the list no longer shows it.
    check!(umount_core(&mp) == 0, b"unmount /MNT/scratch\0");
    let n2 = mount_list_core(&mut ents);
    let mut still = false;
    for e in ents.iter().take(n2) {
        if &e.path[..12] == b"/MNT/scratch" {
            still = true;
        }
    }
    check!(!still, b"mount list no longer shows it after umount\0");
    // Double unmount refused (now absent).
    check!(umount_core(&mp) == BLKMGR_E_NOMNT, b"double unmount refused\0");

    // --- Refusals ----------------------------------------------------------
    // (a) unmount of the boot ROOT "/" refused as a bad path (structural).
    let mut root = [0u8; 32];
    root[0] = b'/';
    check!(umount_core(&root) == BLKMGR_E_BADPATH, b"unmount / refused\0");
    // (b) unmount of "/boot" (the ESP) refused as a bad path.
    let mut esp = [0u8; 32];
    esp[..5].copy_from_slice(b"/boot");
    check!(umount_core(&esp) == BLKMGR_E_BADPATH, b"unmount /boot refused\0");
    // (c) mount at a non-/MNT path refused.
    let mut bad = [0u8; 32];
    bad[..5].copy_from_slice(b"/data");
    check!(mount_core(KIND_SCRATCH, 0, 0, &bad) == BLKMGR_E_BADPATH, b"mount /data (not /MNT) refused\0");
    // (d) mount of the real BOOT disk refused, touches nothing.
    let bkind = unsafe { inst_boot_kind() } as u8;
    let bindex = unsafe { inst_boot_index() } as u8;
    let mut before = [0u8; SECTOR];
    let rb = blk_rw_core(bkind, bindex, 0, 1, before.as_mut_ptr(), false);
    let bm = mount_core(bkind, bindex, 0, &mp);
    check!(bm == BLKMGR_E_BOOTDISK, b"mount of boot disk refused\0");
    let mut after = [0u8; SECTOR];
    let ra = blk_rw_core(bkind, bindex, 0, 1, after.as_mut_ptr(), false);
    check!(rb == 1 && ra == 1 && before == after, b"boot disk unchanged after mount refusal\0");
    // (e) ext2 partition mounts (metadata) and shows fstype ext2.
    let mut ep = [0u8; 32];
    ep[..9].copy_from_slice(b"/MNT/ext2");
    check!(mount_core(KIND_SCRATCH, 0, 1, &ep) == 0, b"mount ext2 p2 at /MNT/ext2\0");
    let n3 = mount_list_core(&mut ents);
    let mut e2ok = false;
    for e in ents.iter().take(n3) {
        if &e.path[..9] == b"/MNT/ext2" && &e.fstype[..4] == b"ext2" {
            e2ok = true;
        }
    }
    check!(e2ok, b"ext2 aux mount lists with fstype ext2\0");
    let _ = umount_core(&ep);

    if !out_passed.is_null() { unsafe { *out_passed = passed }; }
    if !out_total.is_null() { unsafe { *out_total = total }; }
    if passed == total { 0 } else { -1 }
}
