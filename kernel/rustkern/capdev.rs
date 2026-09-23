// rustkern/capdev.rs - #246/#305 CAP_SCOPE_DEVICE: the PURE device-identity
// logic for Stage 4 of docs/CONTRACT_ENFORCEMENT_PLAN.md.
//
// This is the honest home the plan pointed at (kernel/rust/src/capability.rs was
// a TODO stub in a directory the build never compiles - the crate root is
// kernel/rustkern.rs, which only `mod`s files under kernel/rustkern/). New
// kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// There is no performance argument for C here: every function below is a handful
// of integer folds on the COLD escrow enter / per-write chokepoint.
//
// THE SPLIT (mirrors fs/graphfs/fold.c C-state + rustkern/gfsfold.rs policy):
// this file is the STATELESS policy - it derives a device fingerprint from the
// stable descriptor fields the hotplug/usb_msc layer already exposes, derives a
// logical-device key, classifies the confidence of that identity, and decides a
// trust-on-first-use verdict. The STATE (the KNOWN_HOSTS/TOFU store, the per-
// contract binding table, /CONFIG persistence) and the hotplug glue live in
// fs/escrow_device.c, which is C for the same entanglement reason fold.c is: it
// touches the C hotplug device tables, the C process_t and fat I/O, none of
// which Rust reaches in this tree.
//
// HONESTY (docs/CONTRACT_ARCHITECTURE.md section 9). The identity we can build
// from the enumerated USB mass-storage descriptors is VID/PID + vendor/product/
// revision strings + capacity. That is a MODEL-class identity: it distinguishes
// a SanDisk-16G from a Kingston-32G, and it changes if the device re-enumerates
// as something else (the BadUSB signal), but it is NOT a unique serial and two
// identical sticks of the same model+size are indistinguishable by it. We do
// NOT read the SCSI VPD page 0x80 unit serial (that is driver work in
// usb_msc.c, deliberately out of scope for this stage), so cap_device_confidence
// reports MODEL, never SERIAL. The approval surface must state that confidence,
// not overstate it. A false claim of uniqueness is worse than an honest "cannot
// be distinguished from another of its model".

// TOFU verdicts. MIRRORED in fs/escrow_device.h.
pub const CAP_TOFU_MISMATCH: i32 = -1; // known logical key, DIFFERENT fingerprint
pub const CAP_TOFU_NEW: i32 = 0; //     first sight of this logical key
pub const CAP_TOFU_OK: i32 = 1; //      known logical key, SAME fingerprint

// Confidence classes. MIRRORED in fs/escrow_device.h.
pub const CAP_DEV_CONF_NONE: u32 = 0; //   no stable identity at all
pub const CAP_DEV_CONF_MODEL: u32 = 1; //  VID/PID+strings+capacity, spoofable, not unique
pub const CAP_DEV_CONF_SERIAL: u32 = 2; // a reported unique serial (NOT produced here)

// FNV-1a 64-bit. A stable, order-fixed, non-cryptographic fold: it need only be
// deterministic and collision-resistant enough that two different descriptor
// sets almost never share a fingerprint. It is NOT a security hash - the
// security property is that a Ring-3 process cannot choose the DESCRIPTORS a
// device reports, so it cannot steer this fold to a target value from userland
// (the inputs come from the enumerated hardware, read kernel-side).
const FNV_OFFSET: u64 = 0xcbf2_9ce4_8422_2325;
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;

#[inline]
fn fnv_byte(h: u64, b: u8) -> u64 {
    (h ^ (b as u64)).wrapping_mul(FNV_PRIME)
}

#[inline]
fn fnv_u64(mut h: u64, v: u64) -> u64 {
    let mut i = 0;
    while i < 8 {
        h = fnv_byte(h, ((v >> (i * 8)) & 0xff) as u8);
        i += 1;
    }
    h
}

// Fold `len` bytes at `ptr`. A trailing NUL and any bytes past it are ignored so
// that a vendor/product field padded with spaces or NULs by different drivers
// folds identically (INQUIRY strings are space-padded; USB string descriptors
// are not). We stop at the first NUL and skip trailing ASCII spaces so the SAME
// physical device folds the SAME way regardless of padding.
#[inline]
fn fnv_field(mut h: u64, ptr: *const u8, len: u32) -> u64 {
    if ptr.is_null() || len == 0 {
        return fnv_byte(h, 0); // still perturb, so "" differs from a real field
    }
    // SAFETY: caller guarantees `ptr` points to at least `len` readable bytes
    // (the hotplug/usb_msc fixed-size char arrays). Read-only, not retained.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(ptr, len as usize) };
    // Effective length: up to the first NUL.
    let mut eff = 0usize;
    while eff < s.len() && s[eff] != 0 {
        eff += 1;
    }
    // Trim trailing spaces.
    while eff > 0 && s[eff - 1] == b' ' {
        eff -= 1;
    }
    let mut i = 0usize;
    while i < eff {
        h = fnv_byte(h, s[i]);
        i += 1;
    }
    // Length terminator so "ab"+"c" cannot collide with "a"+"bc".
    fnv_byte(h, 0)
}

/// The FULL device identity fingerprint. Folds every stable descriptor field in
/// a FIXED order: vid, pid, capacity(bytes), block_size, vendor, product,
/// revision. This is the value a CAP_SCOPE_DEVICE grant binds to and that is
/// re-derived on every guarded write (so a re-enumeration with any changed
/// descriptor - the BadUSB signal - yields a different value and fails closed).
/// Never returns 0 for a real device (FNV offset basis is non-zero and the
/// folds only ever multiply/xor), so fs/escrow_device.c uses 0 as "no device".
#[no_mangle]
pub extern "C" fn cap_device_fp_rs(
    vid: u32,
    pid: u32,
    cap_bytes: u64,
    block_size: u32,
    vendor: *const u8,
    vendor_len: u32,
    product: *const u8,
    product_len: u32,
    revision: *const u8,
    revision_len: u32,
) -> u64 {
    let mut h = FNV_OFFSET;
    h = fnv_u64(h, vid as u64);
    h = fnv_u64(h, pid as u64);
    h = fnv_u64(h, cap_bytes);
    h = fnv_u64(h, block_size as u64);
    h = fnv_field(h, vendor, vendor_len);
    h = fnv_field(h, product, product_len);
    h = fnv_field(h, revision, revision_len);
    // Guarantee non-zero (0 is the "no device" sentinel on the C side). The
    // offset basis is non-zero and FNV never maps a non-zero state to 0 for a
    // single more byte, but pin it defensively.
    if h == 0 {
        FNV_PRIME
    } else {
        h
    }
}

/// The LOGICAL device key: the "which stick is this" identity a user recognises,
/// folded from VID/PID + product only. It is STABLE across a remount (those
/// fields do not change when the same stick is unplugged and replugged) and
/// INDEPENDENT of revision/capacity, so a device that keeps its model string but
/// changes firmware/capacity/interface (the classic BadUSB reflash) presents the
/// SAME logical key with a DIFFERENT full fingerprint - which is exactly the
/// "same logical device, changed identity" alarm the TOFU store raises.
#[no_mangle]
pub extern "C" fn cap_device_logical_rs(
    vid: u32,
    pid: u32,
    product: *const u8,
    product_len: u32,
) -> u64 {
    let mut h = FNV_OFFSET;
    h = fnv_u64(h, vid as u64);
    h = fnv_u64(h, pid as u64);
    h = fnv_field(h, product, product_len);
    if h == 0 {
        FNV_PRIME
    } else {
        h
    }
}

/// The confidence class of the identity we could build. `has_unique_serial` is
/// 1 only if a genuine per-unit serial was captured (never, in this stage, so
/// the caller passes 0 and gets MODEL). This is the value the approval/audit
/// surface must state so it never overclaims uniqueness.
#[no_mangle]
pub extern "C" fn cap_device_confidence_rs(has_unique_serial: u32) -> u32 {
    if has_unique_serial != 0 {
        CAP_DEV_CONF_SERIAL
    } else {
        CAP_DEV_CONF_MODEL
    }
}

/// The trust-on-first-use verdict. `known` is 0 if the logical key is unseen, 1
/// if the store already holds a fingerprint for it (`stored_fp`). NEW on first
/// sight, OK on an exact match, MISMATCH on a changed fingerprint for a known
/// logical key. The caller (fs/escrow_device.c) records on NEW and NEVER
/// overwrites the trusted fingerprint on MISMATCH (a mismatch must not be able
/// to launder itself into the trusted store).
#[no_mangle]
pub extern "C" fn cap_tofu_verdict_rs(known: u32, stored_fp: u64, cur_fp: u64) -> i32 {
    if known == 0 {
        return CAP_TOFU_NEW;
    }
    if stored_fp == cur_fp {
        CAP_TOFU_OK
    } else {
        CAP_TOFU_MISMATCH
    }
}

/// Pure self-test of the device-identity policy. Returns 0 on PASS, or a
/// negative code identifying the first failed property. Called from the boot
/// self-test (fs/escrow_device.c escdev_selftest) and safe to call anywhere
/// (allocates nothing, touches no state).
#[no_mangle]
pub extern "C" fn cap_device_selftest_rs() -> i32 {
    // Two well-formed model fields.
    let vend_a = b"SanDisk\0";
    let prod_a = b"Cruzer Blade\0";
    let rev_a = b"1.00\0";
    let prod_b = b"DataTraveler\0";
    let rev_b = b"2.00\0";

    let fp_a = cap_device_fp_rs(
        0x0781, 0x5567, 16_000_000_000, 512,
        vend_a.as_ptr(), vend_a.len() as u32,
        prod_a.as_ptr(), prod_a.len() as u32,
        rev_a.as_ptr(), rev_a.len() as u32,
    );
    // 1. Deterministic: same inputs -> same fingerprint.
    let fp_a2 = cap_device_fp_rs(
        0x0781, 0x5567, 16_000_000_000, 512,
        vend_a.as_ptr(), vend_a.len() as u32,
        prod_a.as_ptr(), prod_a.len() as u32,
        rev_a.as_ptr(), rev_a.len() as u32,
    );
    if fp_a == 0 || fp_a != fp_a2 {
        return -1;
    }
    // 2. Padding-insensitive: a space-padded product folds the same as its
    //    NUL-terminated form (drivers differ in padding).
    let prod_a_pad = b"Cruzer Blade   ";
    let fp_a_pad = cap_device_fp_rs(
        0x0781, 0x5567, 16_000_000_000, 512,
        vend_a.as_ptr(), vend_a.len() as u32,
        prod_a_pad.as_ptr(), prod_a_pad.len() as u32,
        rev_a.as_ptr(), rev_a.len() as u32,
    );
    if fp_a_pad != fp_a {
        return -2;
    }
    // 3. A DIFFERENT device (different VID/PID/product) -> different fingerprint
    //    AND different logical key.
    let fp_b = cap_device_fp_rs(
        0x0951, 0x1666, 32_000_000_000, 512,
        vend_a.as_ptr(), vend_a.len() as u32,
        prod_b.as_ptr(), prod_b.len() as u32,
        rev_b.as_ptr(), rev_b.len() as u32,
    );
    if fp_b == fp_a {
        return -3;
    }
    let log_a = cap_device_logical_rs(0x0781, 0x5567, prod_a.as_ptr(), prod_a.len() as u32);
    let log_b = cap_device_logical_rs(0x0951, 0x1666, prod_b.as_ptr(), prod_b.len() as u32);
    if log_a == 0 || log_a == log_b {
        return -4;
    }
    // 4. BadUSB reflash: SAME VID/PID/product (same logical key) but changed
    //    revision + capacity -> SAME logical key, DIFFERENT full fingerprint.
    let fp_a_badusb = cap_device_fp_rs(
        0x0781, 0x5567, 64_000_000_000, 512,
        vend_a.as_ptr(), vend_a.len() as u32,
        prod_a.as_ptr(), prod_a.len() as u32,
        rev_b.as_ptr(), rev_b.len() as u32,
    );
    let log_a_badusb =
        cap_device_logical_rs(0x0781, 0x5567, prod_a.as_ptr(), prod_a.len() as u32);
    if log_a_badusb != log_a {
        return -5;
    }
    if fp_a_badusb == fp_a {
        return -6;
    }
    // 5. TOFU verdicts.
    if cap_tofu_verdict_rs(0, 0, fp_a) != CAP_TOFU_NEW {
        return -7;
    }
    if cap_tofu_verdict_rs(1, fp_a, fp_a) != CAP_TOFU_OK {
        return -8;
    }
    if cap_tofu_verdict_rs(1, fp_a, fp_a_badusb) != CAP_TOFU_MISMATCH {
        return -9;
    }
    // 6. Confidence is honestly MODEL (never SERIAL) when no serial captured.
    if cap_device_confidence_rs(0) != CAP_DEV_CONF_MODEL {
        return -10;
    }
    0
}
