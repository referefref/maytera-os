// rustkern/usb_desc.rs - #404 driver tier: USB Audio Class configuration-descriptor parse
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #404 driver tier: USB Audio Class (UAC) configuration-descriptor parse.
// Rust drop-in for uac_parse_config_c (drivers/usb_audio.c), the pure
// descriptor walk of uac_parse_config(), called from uac_probe() on every USB
// audio device enumeration.
//
// UNTRUSTED input: the configuration descriptor is whatever bytes the attached
// USB device chose to return. drivers/xhci.c stages it in a 512-byte static
// buffer and passes the device-declared wTotalLength (clamped to 512) as len.
// ===========================================================================

// Mirrors the live `uac` selection fields (int/int/int/uint32/uint32) plus
// best_bits, which the live kprintf reports as "%d-bit". sizeof-locked to 24
// by the C side's _Static_assert.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct UsbUacSel {
    pub if_num: i32,
    pub alt: i32,
    pub ep_addr: i32,
    pub max_packet: u32,
    pub interval: u32,
    pub best_bits: i32,
}

// Safe core. `d` is exactly the `len` bytes the device supplied.
//
// Fidelity rule: the C reads cfg[i+2], cfg[i+3], cfg[i+6] (INTERFACE),
// cfg[i+2] (CS_INTERFACE), cfg[i+2..=i+6] (ENDPOINT) guarded ONLY by
// `blen >= 2`. `d.get(k)` returns None exactly when the corresponding C read
// would leave the len-byte buffer, so this matches the C bit-for-bit on every
// input where the C is memory-safe, and rejects precisely where it is not.
// Well-formed descriptors are unaffected: a real INTERFACE is bLength=9 and a
// real ENDPOINT is bLength=7, so every field read lies inside the descriptor.
fn uac_parse_config_core(d: &[u8], out: &mut UsbUacSel) -> i32 {
    let len = d.len();
    let mut cur_if: i32 = -1;
    let mut cur_alt: i32 = -1;
    let mut cur_subclass: i32 = -1;
    let mut cur_bits: i32 = 0;
    let mut found = false;
    let mut best_bits: i32 = 0;

    // i <= len <= i32::MAX (checked by the caller), so i+2 / i+6 cannot
    // overflow usize; the walk is monotonic because blen >= 2 on every step.
    let mut i: usize = 0;
    while i + 2 <= len {
        let blen = d[i] as usize;
        let btype = d[i + 1];
        if blen < 2 || i + blen > len {
            break;
        }

        if btype == 0x04 {
            // INTERFACE descriptor (real bLength = 9).
            match (d.get(i + 2), d.get(i + 3), d.get(i + 6)) {
                (Some(&a), Some(&b), Some(&c)) => {
                    cur_if = a as i32;
                    cur_alt = b as i32;
                    cur_subclass = c as i32; // bInterfaceSubClass (2 = AudioStreaming)
                    cur_bits = 0;
                }
                _ => break, // C would over-read here
            }
        } else if btype == 0x24 {
            // CS_INTERFACE.
            let subtype = match d.get(i + 2) {
                Some(&s) => s,
                None => break, // C would over-read here
            };
            if subtype == 0x02 && blen >= 7 {
                // FORMAT_TYPE: blen>=7 already implies i+6 < len, so this
                // get() never fails; kept bounds-checked for uniformity.
                match d.get(i + 6) {
                    Some(&b) => cur_bits = b as i32, // bBitResolution
                    None => break,
                }
            }
        } else if btype == 0x05 {
            // ENDPOINT descriptor (real bLength = 7).
            let (ep_addr, attrs, m0, m1, interval) = match (
                d.get(i + 2),
                d.get(i + 3),
                d.get(i + 4),
                d.get(i + 5),
                d.get(i + 6),
            ) {
                (Some(&a), Some(&b), Some(&c), Some(&e), Some(&f)) => {
                    (a as i32, b as i32, c as i32, e as i32, f as i32)
                }
                _ => break, // C would over-read here
            };
            let mps = m0 | (m1 << 8);
            let is_iso = (attrs & 0x03) == 0x01;
            let is_out = (ep_addr & 0x80) == 0;
            if is_iso && is_out && cur_subclass == 0x02 && cur_alt != 0 {
                // candidate: prefer 16-bit, else first iso OUT found
                let better = !found || (cur_bits == 16 && best_bits != 16);
                if better {
                    out.if_num = cur_if;
                    out.alt = cur_alt;
                    out.ep_addr = ep_addr;
                    out.max_packet = (mps & 0x7FF) as u32;
                    out.interval = interval as u32;
                    out.best_bits = cur_bits;
                    best_bits = cur_bits;
                    found = true;
                }
            }
        }
        i += blen;
    }
    if found {
        0
    } else {
        -1
    }
}

/// C ABI entry. Drop-in for uac_parse_config_c(): returns 0 and fills `out` on
/// success, -1 on reject (leaving `out` as the caller left it, exactly like
/// the C, whose caller memsets the `uac` struct before probing).
///
/// # Safety
/// `cfg` must point to at least `len` readable bytes and `out` to a writable
/// `UsbUacSel`. Null pointers and len <= 0 are rejected before any deref, so a
/// negative `len` can never reach the `len as usize` cast.
#[no_mangle]
pub unsafe extern "C" fn uac_parse_config_rs(
    cfg: *const u8,
    len: i32,
    out: *mut UsbUacSel,
) -> i32 {
    if cfg.is_null() || out.is_null() || len <= 0 {
        return -1;
    }
    // SAFETY: len > 0 and non-null checked above. The caller (uac_probe) hands
    // us the xHCI config staging buffer and the exact byte count it requested
    // from the device, so `len` bytes are initialized and readable for the
    // duration of the call, and nothing else mutates them (enumeration is
    // single-threaded per slot).
    let d = core::slice::from_raw_parts(cfg, len as usize);
    uac_parse_config_core(d, &mut *out)
}
