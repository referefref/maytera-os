// rustkern/hidrepd.rs - #162: HID REPORT DESCRIPTOR parse, for the Consumer
// (usage page 0x0C) media keys: Volume Increment, Volume Decrement, Mute.
//
// WHY THIS EXISTS AT ALL, AND WHAT WAS MEASURED FIRST
// ==========================================================================
// #162 asks for volume up / volume down / mute to work as system-global
// hotkeys. Before designing anything, the question "which transport actually
// delivers a byte for those keys today?" was settled by reading the code and
// the owner's own real-hardware capture (<workspace>
// USBLOG.TXT, iMac14,4). The answer was NONE OF THEM:
//
//   PS/2      cpu/isr.c's extended-make switch has cases for the arrows, the
//             cursor block, both GUI keys, keypad Enter, keypad / and
//             PrintScreen. It has NO case for E0 30 (volume up), E0 2E
//             (volume down) or E0 20 (mute), so those three bytes were
//             decoded, matched nothing, and produced c = 0: nothing entered
//             the cooked ring.
//
//   USB HID   drivers/usb_hid.c speaks BOOT PROTOCOL ONLY. The boot keyboard
//             report has no consumer usages in it by definition, and the
//             REPORT DESCRIPTOR - the only thing that can tell you a consumer
//             collection exists - was never fetched by this kernel at all
//             (no GET_DESCRIPTOR(0x22) call existed anywhere). An interface
//             whose bInterfaceProtocol is neither 1 nor 2 was bound as
//             HID_DEVICE_UNKNOWN, given no DMA page and no TD, and logged
//             "input not routed".
//
//   BT HID    bt/hid.c translates boot-protocol usages through hidmap.rs.
//             Same story: no consumer page.
//
// So #162 is a DRIVER FEATURE, not a routing fix. This module is the part
// that had to be built rather than re-wired.
//
// THE MEASUREMENT THAT SHAPES THE DESIGN
// ==========================================================================
// The owner's keyboard on the real iMac is 05ac:024f, and its captured
// configuration descriptor is:
//
//   IFACE 0 alt 0: class 0x03 sub 0x01 proto 0x01 (boot-keyboard) EP 0x81 mps 8
//   IFACE 1 alt 0: class 0x03 sub 0x01 proto 0x02 (boot-MOUSE)    EP 0x82 mps 16
//
// A keyboard with no trackpad has no mouse. Interface 1 is the media/consumer
// interface, and it declares boot-mouse protocol. Today the kernel believes
// that byte: USBLOG shows "slot 3 iface 1 DCI 5: bound as mouse", SET_PROTOCOL
// (boot) OK, and its 16-byte reports parsed as {buttons,x,y,wheel}. That is
// both why the media keys do nothing AND a standing risk of phantom pointer
// input from a keyboard.
//
// The lesson that follows is the whole reason this file is a parser and not a
// table of VID/PIDs: bInterfaceProtocol IS NOT TRUSTWORTHY. The report
// descriptor is the device's own statement of what it sends, and it is the
// only statement that can be checked. So we fetch it and parse it.
//
// WHY A GENERAL PARSER AND NOT A HARDCODED OFFSET
// ==========================================================================
// Consumer controls appear in the wild in two structurally different shapes,
// and a device may use either:
//
//   ARRAY    one N-bit field carrying a usage VALUE, with Usage Minimum /
//            Usage Maximum declaring the range. Report is [id, usage_lo,
//            usage_hi]. Releases are reported as usage 0.
//
//   VARIABLE one BIT PER CONTROL, declared by an explicit Usage list.
//            Report is [id, bitmap].
//
// A hardcoded "byte 1 == 0xE9" works on exactly one of those and silently
// does nothing on the other. A guessed constant does not fail loudly: it
// ships a feature that renders, runs and does nothing. Both shapes are
// parsed here and both are covered by hidrepd_selftest_rs().
//
// NEW kernel code, so Rust per the 2026-07-16 rule. There is no `_c` arm and
// no [RUST-DIFF] differential because there is nothing to differ FROM: no C
// implementation of this has ever existed. What stands in for a differential
// is hidrepd_selftest_rs(), a vector test over real descriptor shapes, run on
// every boot with one line of output. Say which kind of evidence you have.
//
// UNTRUSTED INPUT. Every byte here came off the wire from whatever device is
// plugged in. There is no allocation, no recursion, and every index is
// bounds-checked by construction (slice iteration + `get()`), so a malicious
// or merely broken descriptor can waste at most a bounded walk.

#![allow(dead_code)]

// ---------------------------------------------------------------------------
// Consumer (0x0C) usages we care about. GREPPED FROM THE HID USAGE TABLES,
// not remembered: HUT 1.4 section 15 (Consumer Page), "Volume Increment"
// 0x00E9, "Volume Decrement" 0x00EA, "Mute" 0x00E2.
// ---------------------------------------------------------------------------
pub const USAGE_PAGE_GENERIC_DESKTOP: u32 = 0x01;
pub const USAGE_PAGE_KEYBOARD: u32 = 0x07;
pub const USAGE_PAGE_CONSUMER: u32 = 0x0C;

pub const CONSUMER_VOLUME_UP: u32 = 0x00E9;
pub const CONSUMER_VOLUME_DOWN: u32 = 0x00EA;
pub const CONSUMER_MUTE: u32 = 0x00E2;

// Generic Desktop usages that identify a top-level collection's role.
const GD_MOUSE: u32 = 0x02;
const GD_KEYBOARD: u32 = 0x06;
// Consumer page, "Consumer Control" application collection.
const CONSUMER_CONTROL: u32 = 0x01;

// Bits in HidConsMap::have and in the value hidrepd_decode_rs() returns.
pub const HIDCONS_VOL_UP: u8 = 1 << 0;
pub const HIDCONS_VOL_DOWN: u8 = 1 << 1;
pub const HIDCONS_MUTE: u8 = 1 << 2;

// HidConsMap::kind
pub const HIDCONS_KIND_NONE: u8 = 0;
pub const HIDCONS_KIND_VARIABLE: u8 = 1; // one bit per control
pub const HIDCONS_KIND_ARRAY: u8 = 2; // N-bit usage-value field

// ---------------------------------------------------------------------------
// The parse result. #[repr(C)] and sizeof-locked on the C side with a
// _Static_assert, per the established FFI pattern.
// ---------------------------------------------------------------------------
#[repr(C)]
#[derive(Clone, Copy)]
pub struct HidConsMap {
    /// Bitmask of HIDCONS_* that this interface can actually report. 0 means
    /// "this interface has no volume controls" and every other field is
    /// meaningless.
    pub have: u8,
    /// Report ID carrying them. 0 means the descriptor declared NO Report ID
    /// item, in which case reports have no leading ID byte. This distinction
    /// is not cosmetic: getting it wrong shifts every field by one byte.
    pub report_id: u8,
    /// HIDCONS_KIND_*.
    pub kind: u8,
    /// Did the descriptor declare a top-level Consumer Control collection?
    pub top_consumer: u8,
    /// Did it declare a top-level Generic Desktop Mouse collection? An
    /// interface that says "boot mouse" in its interface descriptor but has
    /// no Mouse collection here is not a mouse, whatever the byte says.
    pub top_mouse: u8,
    /// Likewise for a Generic Desktop Keyboard collection.
    pub top_keyboard: u8,
    /// Total INPUT report length in bytes for `report_id`, EXCLUDING the ID
    /// byte. Diagnostic only; the decode never relies on it.
    pub report_bytes: u8,
    pub _pad: u8,
    /// VARIABLE kind: bit offset of each control within the report payload
    /// (i.e. after the report-ID byte, if any). 0xFFFF = not present.
    pub bit_vol_up: u16,
    pub bit_vol_down: u16,
    pub bit_mute: u16,
    /// ARRAY kind: the usage-value field(s).
    pub arr_bit_off: u16,
    pub arr_bit_size: u16,
    pub arr_count: u16,
}

const NO_BIT: u16 = 0xFFFF;

// Layout lock. The C side keeps one of these per HID device and never reads a
// field it did not get from here, but a silent size change would still shift
// the surrounding struct; drivers/usb_hid.h carries the matching
// _Static_assert so the two cannot drift apart unnoticed.
const _: () = assert!(core::mem::size_of::<HidConsMap>() == 20);


impl HidConsMap {
    pub const fn empty() -> HidConsMap {
        HidConsMap {
            have: 0,
            report_id: 0,
            kind: HIDCONS_KIND_NONE,
            top_consumer: 0,
            top_mouse: 0,
            top_keyboard: 0,
            report_bytes: 0,
            _pad: 0,
            bit_vol_up: NO_BIT,
            bit_vol_down: NO_BIT,
            bit_mute: NO_BIT,
            arr_bit_off: 0,
            arr_bit_size: 0,
            arr_count: 0,
        }
    }
}

// ---------------------------------------------------------------------------
// Item encoding (HID 1.11 section 6.2.2).
// ---------------------------------------------------------------------------
const ITEM_TYPE_MAIN: u8 = 0;
const ITEM_TYPE_GLOBAL: u8 = 1;
const ITEM_TYPE_LOCAL: u8 = 2;

const MAIN_INPUT: u8 = 0x8;
const MAIN_OUTPUT: u8 = 0x9;
const MAIN_COLLECTION: u8 = 0xA;
const MAIN_FEATURE: u8 = 0xB;
const MAIN_END_COLLECTION: u8 = 0xC;

const GLOBAL_USAGE_PAGE: u8 = 0x0;
const GLOBAL_REPORT_SIZE: u8 = 0x7;
const GLOBAL_REPORT_ID: u8 = 0x8;
const GLOBAL_REPORT_COUNT: u8 = 0x9;
const GLOBAL_PUSH: u8 = 0xA;
const GLOBAL_POP: u8 = 0xB;

const LOCAL_USAGE: u8 = 0x0;
const LOCAL_USAGE_MIN: u8 = 0x1;
const LOCAL_USAGE_MAX: u8 = 0x2;

// INPUT item data bit 1: 0 = Array, 1 = Variable.
const IOF_VARIABLE: u32 = 1 << 1;

// Bounded working limits. A descriptor that exceeds any of these is parsed as
// far as the limit and no further; it can never make us allocate or spin.
const MAX_LOCAL_USAGES: usize = 64;
const MAX_REPORT_IDS: usize = 16;
const MAX_GLOBAL_STACK: usize = 8;

#[derive(Clone, Copy)]
struct GlobalState {
    usage_page: u32,
    report_size: u32,
    report_count: u32,
    report_id: u8,
}

/// Per-(report-kind, report-id) bit cursor. Input, Output and Feature reports
/// each have their OWN independent bit space, which is why this is keyed by
/// kind as well as id; sharing one cursor across all three silently corrupts
/// every offset in a descriptor that declares an Output report (every keyboard
/// with LEDs does).
struct Cursors {
    ids: [u8; MAX_REPORT_IDS],
    bits: [u32; MAX_REPORT_IDS],
    n: usize,
}

impl Cursors {
    fn new() -> Cursors {
        Cursors {
            ids: [0; MAX_REPORT_IDS],
            bits: [0; MAX_REPORT_IDS],
            n: 0,
        }
    }
    /// Returns the current cursor for `id` and advances it by `bits`.
    fn take_and_advance(&mut self, id: u8, bits: u32) -> u32 {
        for i in 0..self.n {
            if self.ids[i] == id {
                let cur = self.bits[i];
                self.bits[i] = self.bits[i].saturating_add(bits);
                return cur;
            }
        }
        if self.n < MAX_REPORT_IDS {
            self.ids[self.n] = id;
            self.bits[self.n] = bits;
            self.n += 1;
        }
        0
    }
    fn get(&self, id: u8) -> u32 {
        for i in 0..self.n {
            if self.ids[i] == id {
                return self.bits[i];
            }
        }
        0
    }
}

/// Read one item's unsigned data field. `n` is 0, 1, 2 or 4 bytes.
fn item_data_u32(d: &[u8], at: usize, n: usize) -> u32 {
    let mut v: u32 = 0;
    let mut i = 0;
    while i < n {
        match d.get(at + i) {
            Some(b) => v |= (*b as u32) << (8 * i),
            None => return v,
        }
        i += 1;
    }
    v
}

/// The safe core. `d` is exactly the bytes the device supplied.
fn parse_core(d: &[u8], out: &mut HidConsMap) {
    *out = HidConsMap::empty();

    let mut g = GlobalState {
        usage_page: 0,
        report_size: 0,
        report_count: 0,
        report_id: 0,
    };
    let mut gstack: [GlobalState; MAX_GLOBAL_STACK] = [g; MAX_GLOBAL_STACK];
    let mut gsp: usize = 0;

    // Local item state, cleared after EVERY main item (HID 1.11 6.2.2.8).
    let mut usages: [u32; MAX_LOCAL_USAGES] = [0; MAX_LOCAL_USAGES];
    let mut nusages: usize = 0;
    let mut usage_min: u32 = 0;
    let mut usage_max: u32 = 0;
    let mut have_range = false;

    let mut depth: i32 = 0;
    let mut saw_report_id = false;

    let mut cur_in = Cursors::new();
    let mut cur_out = Cursors::new();
    let mut cur_feat = Cursors::new();

    let mut i: usize = 0;
    while i < d.len() {
        let head = d[i];
        // Long item (HID 1.11 6.2.2.3). We do not interpret any long item;
        // skip it correctly so the rest of the descriptor still parses.
        if head == 0xFE {
            let dsize = *d.get(i + 1).unwrap_or(&0) as usize;
            i += 3 + dsize;
            continue;
        }
        let bsize_code = (head & 0x03) as usize;
        let bsize = if bsize_code == 3 { 4 } else { bsize_code };
        let btype = (head >> 2) & 0x03;
        let btag = (head >> 4) & 0x0F;
        let data_at = i + 1;
        let data = item_data_u32(d, data_at, bsize);
        // Advance now; every `continue` below is then safe.
        i += 1 + bsize;

        match btype {
            ITEM_TYPE_GLOBAL => match btag {
                GLOBAL_USAGE_PAGE => g.usage_page = data,
                GLOBAL_REPORT_SIZE => g.report_size = data,
                GLOBAL_REPORT_COUNT => g.report_count = data,
                GLOBAL_REPORT_ID => {
                    g.report_id = data as u8;
                    saw_report_id = true;
                }
                GLOBAL_PUSH => {
                    if gsp < MAX_GLOBAL_STACK {
                        gstack[gsp] = g;
                        gsp += 1;
                    }
                }
                GLOBAL_POP => {
                    if gsp > 0 {
                        gsp -= 1;
                        g = gstack[gsp];
                    }
                }
                _ => {}
            },

            ITEM_TYPE_LOCAL => {
                // A Usage/UsageMin/UsageMax given as 4 bytes (and, by common
                // convention, 2 bytes inside a page-scoped context) carries
                // its own page in the high 16 bits. A 1-byte form always
                // takes the current global Usage Page. Getting THIS wrong is
                // how a parser "finds" 0xE9 on the keyboard page and fires
                // the volume key on the letter 'r'.
                let full = if bsize == 4 {
                    data
                } else {
                    (g.usage_page << 16) | (data & 0xFFFF)
                };
                match btag {
                    LOCAL_USAGE => {
                        if nusages < MAX_LOCAL_USAGES {
                            usages[nusages] = full;
                            nusages += 1;
                        }
                    }
                    LOCAL_USAGE_MIN => {
                        usage_min = full;
                        have_range = true;
                    }
                    LOCAL_USAGE_MAX => {
                        usage_max = full;
                        have_range = true;
                    }
                    _ => {}
                }
            }

            ITEM_TYPE_MAIN => {
                match btag {
                    MAIN_COLLECTION => {
                        // Only a TOP-LEVEL (depth 0) collection names the
                        // interface's role. A nested Physical collection
                        // inside a mouse says nothing about the interface.
                        if depth == 0 {
                            let u = if nusages > 0 {
                                usages[0]
                            } else {
                                (g.usage_page << 16) | 0
                            };
                            let page = u >> 16;
                            let usage = u & 0xFFFF;
                            if page == USAGE_PAGE_CONSUMER && usage == CONSUMER_CONTROL {
                                out.top_consumer = 1;
                            }
                            if page == USAGE_PAGE_GENERIC_DESKTOP && usage == GD_MOUSE {
                                out.top_mouse = 1;
                            }
                            if page == USAGE_PAGE_GENERIC_DESKTOP && usage == GD_KEYBOARD {
                                out.top_keyboard = 1;
                            }
                        }
                        depth += 1;
                    }
                    MAIN_END_COLLECTION => {
                        if depth > 0 {
                            depth -= 1;
                        }
                    }
                    MAIN_INPUT => {
                        let total = g.report_size.saturating_mul(g.report_count);
                        let base = cur_in.take_and_advance(g.report_id, total);
                        if data & IOF_VARIABLE != 0 {
                            note_variable(out, &g, base, &usages, nusages, usage_min,
                                          usage_max, have_range);
                        } else {
                            note_array(out, &g, base, &usages, nusages, usage_min,
                                       usage_max, have_range);
                        }
                    }
                    MAIN_OUTPUT => {
                        let total = g.report_size.saturating_mul(g.report_count);
                        cur_out.take_and_advance(g.report_id, total);
                    }
                    MAIN_FEATURE => {
                        let total = g.report_size.saturating_mul(g.report_count);
                        cur_feat.take_and_advance(g.report_id, total);
                    }
                    _ => {}
                }
                // Local state is cleared by EVERY main item, including
                // Collection and End Collection.
                nusages = 0;
                usage_min = 0;
                usage_max = 0;
                have_range = false;
            }

            _ => {}
        }
    }

    if !saw_report_id {
        out.report_id = 0;
    }
    if out.have != 0 {
        let bits = cur_in.get(out.report_id);
        let bytes = (bits + 7) / 8;
        out.report_bytes = if bytes > 255 { 255 } else { bytes as u8 };
    } else {
        // Nothing found: make the whole struct unambiguously empty so a
        // caller that ignores `have` still cannot act on stale offsets.
        let tc = out.top_consumer;
        let tm = out.top_mouse;
        let tk = out.top_keyboard;
        *out = HidConsMap::empty();
        out.top_consumer = tc;
        out.top_mouse = tm;
        out.top_keyboard = tk;
    }
}

/// A VARIABLE input item: `report_count` fields of `report_size` bits, the
/// i-th carrying the i-th declared usage. Per HID 1.11 6.2.2.8, if fewer
/// usages are declared than there are fields, the LAST usage repeats; if a
/// Usage Minimum/Maximum range is declared, it enumerates them in order.
fn note_variable(out: &mut HidConsMap, g: &GlobalState, base: u32, usages: &[u32],
                 nusages: usize, usage_min: u32, usage_max: u32, have_range: bool) {
    if g.report_size == 0 {
        return;
    }
    let count = g.report_count;
    let mut idx: u32 = 0;
    while idx < count {
        let u = if nusages > 0 {
            let k = if (idx as usize) < nusages { idx as usize } else { nusages - 1 };
            usages[k]
        } else if have_range {
            let cand = usage_min.saturating_add(idx);
            if cand > usage_max {
                usage_max
            } else {
                cand
            }
        } else {
            idx += 1;
            continue;
        };
        if (u >> 16) == USAGE_PAGE_CONSUMER {
            let off = base + idx * g.report_size;
            if off <= 0xFFFE {
                let slot = match u & 0xFFFF {
                    CONSUMER_VOLUME_UP => Some(HIDCONS_VOL_UP),
                    CONSUMER_VOLUME_DOWN => Some(HIDCONS_VOL_DOWN),
                    CONSUMER_MUTE => Some(HIDCONS_MUTE),
                    _ => None,
                };
                if let Some(bit) = slot {
                    // A device that declares the SAME control in two places
                    // keeps the FIRST, so a later Feature/duplicate cannot
                    // move a field we already located.
                    if out.kind == HIDCONS_KIND_NONE || out.kind == HIDCONS_KIND_VARIABLE {
                        out.kind = HIDCONS_KIND_VARIABLE;
                        if out.have == 0 {
                            out.report_id = g.report_id;
                        }
                        if g.report_id == out.report_id {
                            match bit {
                                HIDCONS_VOL_UP => {
                                    if out.bit_vol_up == NO_BIT {
                                        out.bit_vol_up = off as u16;
                                        out.have |= HIDCONS_VOL_UP;
                                    }
                                }
                                HIDCONS_VOL_DOWN => {
                                    if out.bit_vol_down == NO_BIT {
                                        out.bit_vol_down = off as u16;
                                        out.have |= HIDCONS_VOL_DOWN;
                                    }
                                }
                                _ => {
                                    if out.bit_mute == NO_BIT {
                                        out.bit_mute = off as u16;
                                        out.have |= HIDCONS_MUTE;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        idx += 1;
    }
}

/// An ARRAY input item: `report_count` fields of `report_size` bits, each
/// carrying a usage VALUE drawn from the declared Usage Minimum..Maximum
/// range (or the explicit usage list). Release is reported as value 0.
fn note_array(out: &mut HidConsMap, g: &GlobalState, base: u32, usages: &[u32],
              nusages: usize, usage_min: u32, usage_max: u32, have_range: bool) {
    if g.report_size == 0 || g.report_count == 0 {
        return;
    }
    // Only a CONSUMER-page array can carry these usages.
    let mut covered: u8 = 0;
    if have_range && (usage_min >> 16) == USAGE_PAGE_CONSUMER
        && (usage_max >> 16) == USAGE_PAGE_CONSUMER
    {
        let lo = usage_min & 0xFFFF;
        let hi = usage_max & 0xFFFF;
        if lo <= CONSUMER_VOLUME_UP && CONSUMER_VOLUME_UP <= hi {
            covered |= HIDCONS_VOL_UP;
        }
        if lo <= CONSUMER_VOLUME_DOWN && CONSUMER_VOLUME_DOWN <= hi {
            covered |= HIDCONS_VOL_DOWN;
        }
        if lo <= CONSUMER_MUTE && CONSUMER_MUTE <= hi {
            covered |= HIDCONS_MUTE;
        }
    }
    // Some descriptors list the array's members explicitly instead of using a
    // range. Honour that too rather than silently finding nothing.
    for k in 0..nusages {
        let u = usages[k];
        if (u >> 16) != USAGE_PAGE_CONSUMER {
            continue;
        }
        match u & 0xFFFF {
            CONSUMER_VOLUME_UP => covered |= HIDCONS_VOL_UP,
            CONSUMER_VOLUME_DOWN => covered |= HIDCONS_VOL_DOWN,
            CONSUMER_MUTE => covered |= HIDCONS_MUTE,
            _ => {}
        }
    }
    if covered == 0 {
        return;
    }
    // A VARIABLE mapping already found is more precise; do not overwrite it.
    if out.kind == HIDCONS_KIND_VARIABLE {
        return;
    }
    if out.kind == HIDCONS_KIND_ARRAY {
        return; // keep the first array field found
    }
    if base > 0xFFFF || g.report_size > 0xFFFF || g.report_count > 0xFFFF {
        return;
    }
    out.kind = HIDCONS_KIND_ARRAY;
    out.report_id = g.report_id;
    out.arr_bit_off = base as u16;
    out.arr_bit_size = g.report_size as u16;
    out.arr_count = g.report_count as u16;
    out.have = covered;
}

/// Read `nbits` (<= 32) starting at bit `off`, LSB-first within each byte and
/// little-endian across bytes, which is how HID lays out report fields.
fn read_bits(payload: &[u8], off: u32, nbits: u32) -> u32 {
    if nbits == 0 || nbits > 32 {
        return 0;
    }
    let mut v: u32 = 0;
    let mut k: u32 = 0;
    while k < nbits {
        let b = off + k;
        let byte = (b >> 3) as usize;
        let bit = (b & 7) as u32;
        let got = match payload.get(byte) {
            Some(x) => (*x as u32 >> bit) & 1,
            None => 0,
        };
        v |= got << k;
        k += 1;
    }
    v
}

fn decode_core(m: &HidConsMap, buf: &[u8]) -> u8 {
    if m.have == 0 || m.kind == HIDCONS_KIND_NONE {
        return 0;
    }
    // Strip the report-ID byte if and only if the descriptor declared one.
    let payload: &[u8] = if m.report_id != 0 {
        match buf.first() {
            Some(id) if *id == m.report_id => &buf[1..],
            _ => return 0, // a different report on the same endpoint
        }
    } else {
        buf
    };

    let mut out: u8 = 0;
    if m.kind == HIDCONS_KIND_VARIABLE {
        if m.bit_vol_up != NO_BIT && read_bits(payload, m.bit_vol_up as u32, 1) != 0 {
            out |= HIDCONS_VOL_UP;
        }
        if m.bit_vol_down != NO_BIT && read_bits(payload, m.bit_vol_down as u32, 1) != 0 {
            out |= HIDCONS_VOL_DOWN;
        }
        if m.bit_mute != NO_BIT && read_bits(payload, m.bit_mute as u32, 1) != 0 {
            out |= HIDCONS_MUTE;
        }
    } else {
        let mut k: u32 = 0;
        while k < m.arr_count as u32 {
            let off = m.arr_bit_off as u32 + k * m.arr_bit_size as u32;
            let v = read_bits(payload, off, m.arr_bit_size as u32);
            match v {
                CONSUMER_VOLUME_UP => out |= HIDCONS_VOL_UP,
                CONSUMER_VOLUME_DOWN => out |= HIDCONS_VOL_DOWN,
                CONSUMER_MUTE => out |= HIDCONS_MUTE,
                _ => {}
            }
            k += 1;
        }
    }
    out & m.have
}

// ===========================================================================
// FFI
// ===========================================================================

/// Parse a HID report descriptor. `desc`/`len` are the raw bytes as returned
/// by GET_DESCRIPTOR(Report). `out` is filled in every case, including
/// failure (then `have` == 0). Returns 1 if any volume control was found.
///
/// # Safety
/// `desc` must point to `len` readable bytes; `out` must be a valid
/// HidConsMap. Both are supplied by drivers/usb_hid.c from its own storage.
#[no_mangle]
pub unsafe extern "C" fn hidrepd_parse_rs(desc: *const u8, len: u32,
                                          out: *mut HidConsMap) -> i32 {
    if out.is_null() {
        return 0;
    }
    if desc.is_null() || len == 0 || len > 4096 {
        *out = HidConsMap::empty();
        return 0;
    }
    let d = core::slice::from_raw_parts(desc, len as usize);
    let mut m = HidConsMap::empty();
    parse_core(d, &mut m);
    *out = m;
    if m.have != 0 {
        1
    } else {
        0
    }
}

/// Decode one interrupt-IN report into a bitmask of HIDCONS_* currently held.
///
/// # Safety
/// `map` must be a HidConsMap previously filled by hidrepd_parse_rs; `buf`
/// must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn hidrepd_decode_rs(map: *const HidConsMap, buf: *const u8,
                                           len: u32) -> u8 {
    if map.is_null() || buf.is_null() || len == 0 || len > 256 {
        return 0;
    }
    let m = *map;
    let b = core::slice::from_raw_parts(buf, len as usize);
    decode_core(&m, b)
}

// ===========================================================================
// SELF-TEST
// ===========================================================================
// No VM can produce a USB consumer-control device: QEMU's `usb-kbd` presents a
// single boot-keyboard interface with no consumer collection, and there is no
// QEMU device that presents one. So the parser would otherwise ship having
// NEVER EXECUTED on a real descriptor - the zero-callers trap this tree keeps
// rediscovering. These vectors are the standing proof, run on every boot.
//
// The vectors are the two real shapes, byte-assembled from the HID item
// encoding, plus the cases that decide whether the parser is honest rather
// than merely lucky:
//   1. ARRAY with Usage Min/Max and a Report ID  (the Apple/consumer shape)
//   2. VARIABLE bitmap with a Report ID
//   3. VARIABLE bitmap with NO Report ID (must not eat a leading byte)
//   4. A boot-mouse descriptor (must find NOTHING and must set top_mouse)
//   5. A keyboard descriptor with an OUTPUT (LED) item, proving Input and
//      Output bit spaces are tracked separately - share them and every offset
//      after the LED item is wrong
//   6. Consumer usage 0xE9 on the WRONG page (Keyboard page) - must NOT match

const VEC_ARRAY_ID1: &[u8] = &[
    0x05, 0x0C, //        Usage Page (Consumer)
    0x09, 0x01, //        Usage (Consumer Control)
    0xA1, 0x01, //        Collection (Application)
    0x85, 0x01, //          Report ID (1)
    0x15, 0x00, //          Logical Minimum (0)
    0x26, 0x3C, 0x02, //    Logical Maximum (0x23C)
    0x19, 0x00, //          Usage Minimum (0)
    0x2A, 0x3C, 0x02, //    Usage Maximum (0x23C)
    0x75, 0x10, //          Report Size (16)
    0x95, 0x01, //          Report Count (1)
    0x81, 0x00, //          Input (Data, Array, Absolute)
    0xC0, //              End Collection
];

const VEC_VAR_ID2: &[u8] = &[
    0x05, 0x0C, //        Usage Page (Consumer)
    0x09, 0x01, //        Usage (Consumer Control)
    0xA1, 0x01, //        Collection (Application)
    0x85, 0x02, //          Report ID (2)
    0x09, 0xE9, //          Usage (Volume Increment)
    0x09, 0xEA, //          Usage (Volume Decrement)
    0x09, 0xE2, //          Usage (Mute)
    0x15, 0x00, //          Logical Minimum (0)
    0x25, 0x01, //          Logical Maximum (1)
    0x75, 0x01, //          Report Size (1)
    0x95, 0x03, //          Report Count (3)
    0x81, 0x02, //          Input (Data, Variable, Absolute)
    0xC0, //              End Collection
];

const VEC_VAR_NOID: &[u8] = &[
    0x05, 0x0C, //        Usage Page (Consumer)
    0x09, 0x01, //        Usage (Consumer Control)
    0xA1, 0x01, //        Collection (Application)
    0x09, 0xE2, //          Usage (Mute)
    0x09, 0xE9, //          Usage (Volume Increment)
    0x09, 0xEA, //          Usage (Volume Decrement)
    0x15, 0x00, //          Logical Minimum (0)
    0x25, 0x01, //          Logical Maximum (1)
    0x75, 0x01, //          Report Size (1)
    0x95, 0x03, //          Report Count (3)
    0x81, 0x02, //          Input (Data, Variable, Absolute)
    0xC0, //              End Collection
];

const VEC_BOOT_MOUSE: &[u8] = &[
    0x05, 0x01, //        Usage Page (Generic Desktop)
    0x09, 0x02, //        Usage (Mouse)
    0xA1, 0x01, //        Collection (Application)
    0x09, 0x01, //          Usage (Pointer)
    0xA1, 0x00, //          Collection (Physical)
    0x05, 0x09, //            Usage Page (Button)
    0x19, 0x01, //            Usage Minimum (1)
    0x29, 0x03, //            Usage Maximum (3)
    0x15, 0x00, //            Logical Minimum (0)
    0x25, 0x01, //            Logical Maximum (1)
    0x95, 0x03, //            Report Count (3)
    0x75, 0x01, //            Report Size (1)
    0x81, 0x02, //            Input (Data, Variable, Absolute)
    0x95, 0x01, //            Report Count (1)
    0x75, 0x05, //            Report Size (5)
    0x81, 0x01, //            Input (Constant)
    0x05, 0x01, //            Usage Page (Generic Desktop)
    0x09, 0x30, //            Usage (X)
    0x09, 0x31, //            Usage (Y)
    0x15, 0x81, //            Logical Minimum (-127)
    0x25, 0x7F, //            Logical Maximum (127)
    0x75, 0x08, //            Report Size (8)
    0x95, 0x02, //            Report Count (2)
    0x81, 0x06, //            Input (Data, Variable, Relative)
    0xC0, //                End Collection
    0xC0, //              End Collection
];

// A keyboard: 8 bits of modifiers, 8 reserved, then an OUTPUT (LED) item of
// 5+3 bits, then the 6-byte keycode array. Then a SECOND application
// collection carrying the consumer controls as a VARIABLE bitmap on the SAME
// report id 0. If Output shared the Input cursor, the consumer bits would be
// located 8 bits late and every volume key would read as the wrong control.
const VEC_KBD_WITH_LED_AND_CONSUMER: &[u8] = &[
    0x05, 0x01, //        Usage Page (Generic Desktop)
    0x09, 0x06, //        Usage (Keyboard)
    0xA1, 0x01, //        Collection (Application)
    0x05, 0x07, //          Usage Page (Keyboard)
    0x19, 0xE0, //          Usage Minimum (224)
    0x29, 0xE7, //          Usage Maximum (231)
    0x15, 0x00, //          Logical Minimum (0)
    0x25, 0x01, //          Logical Maximum (1)
    0x75, 0x01, //          Report Size (1)
    0x95, 0x08, //          Report Count (8)
    0x81, 0x02, //          Input (Data, Variable, Absolute)   bits 0..7
    0x95, 0x01, //          Report Count (1)
    0x75, 0x08, //          Report Size (8)
    0x81, 0x01, //          Input (Constant)                   bits 8..15
    0x95, 0x05, //          Report Count (5)
    0x75, 0x01, //          Report Size (1)
    0x05, 0x08, //          Usage Page (LEDs)
    0x19, 0x01, //          Usage Minimum (1)
    0x29, 0x05, //          Usage Maximum (5)
    0x91, 0x02, //          Output (Data, Variable, Absolute)  OUTPUT space
    0x95, 0x01, //          Report Count (1)
    0x75, 0x03, //          Report Size (3)
    0x91, 0x01, //          Output (Constant)                  OUTPUT space
    0x95, 0x06, //          Report Count (6)
    0x75, 0x08, //          Report Size (8)
    0x05, 0x07, //          Usage Page (Keyboard)
    0x19, 0x00, //          Usage Minimum (0)
    0x29, 0x65, //          Usage Maximum (101)
    0x81, 0x00, //          Input (Data, Array)                bits 16..63
    0xC0, //              End Collection
    0x05, 0x0C, //        Usage Page (Consumer)
    0x09, 0x01, //        Usage (Consumer Control)
    0xA1, 0x01, //        Collection (Application)
    0x09, 0xE9, //          Usage (Volume Increment)
    0x09, 0xEA, //          Usage (Volume Decrement)
    0x75, 0x01, //          Report Size (1)
    0x95, 0x02, //          Report Count (2)
    0x81, 0x02, //          Input (Data, Variable, Absolute)   bits 64..65
    0xC0, //              End Collection
];

// Usage 0xE9 on the KEYBOARD page (0x07), not Consumer. HID keyboard usage
// 0xE9 does not exist as a media key; a parser that ignores the page would
// match it and fire volume-up on a key that is not volume-up.
const VEC_WRONG_PAGE: &[u8] = &[
    0x05, 0x01, //        Usage Page (Generic Desktop)
    0x09, 0x06, //        Usage (Keyboard)  <- a Keyboard collection is GD/0x06
    0xA1, 0x01, //        Collection (Application)
    0x05, 0x07, //          Usage Page (Keyboard)
    0x09, 0xE9, //          Usage (0xE9 on the KEYBOARD page, NOT Consumer)
    0x75, 0x01, //          Report Size (1)
    0x95, 0x01, //          Report Count (1)
    0x81, 0x02, //          Input (Data, Variable, Absolute)
    0xC0, //              End Collection
];

struct Tally {
    checks: u32,
    fails: u32,
    first_fail: u32,   // 1-based index of the first failing check, 0 = none
}

fn check(cond: bool, t: &mut Tally) {
    t.checks += 1;
    if !cond {
        t.fails += 1;
        if t.first_fail == 0 {
            t.first_fail = t.checks;
        }
    }
}

/// Run the vector test. Returns the number of FAILURES; `out_checks` receives
/// the number of assertions made, so "0 failures" can be told apart from "the
/// test did not run"; `out_first_fail` receives the 1-BASED INDEX of the first
/// failing check (0 = none).
///
/// The index is not decoration. The FIRST run of this test on a booted machine
/// reported "1 of 36 failed" and working out which one meant counting
/// assertions by hand. (It was check 35, and the fault was in the test vector,
/// not the parser: VEC_WRONG_PAGE declared its application collection on the
/// Keyboard page instead of Generic Desktop. The parser was right to say that
/// is not a Keyboard collection.) A gate that names its rule costs nothing.
#[no_mangle]
pub unsafe extern "C" fn hidrepd_selftest_rs(out_checks: *mut u32,
                                             out_first_fail: *mut u32) -> i32 {
    let mut t = Tally { checks: 0, fails: 0, first_fail: 0 };
    let mut m = HidConsMap::empty();

    // ---- 1. ARRAY with a Report ID -----------------------------------------
    parse_core(VEC_ARRAY_ID1, &mut m);
    check(m.have == (HIDCONS_VOL_UP | HIDCONS_VOL_DOWN | HIDCONS_MUTE), &mut t);
    check(m.kind == HIDCONS_KIND_ARRAY, &mut t);
    check(m.report_id == 1, &mut t);
    check(m.arr_bit_off == 0, &mut t);
    check(m.arr_bit_size == 16, &mut t);
    check(m.top_consumer == 1 && m.top_mouse == 0, &mut t);
    // Volume up: report id 1, usage 0x00E9 little-endian.
    check(decode_core(&m, &[0x01, 0xE9, 0x00]) == HIDCONS_VOL_UP, &mut t);
    check(decode_core(&m, &[0x01, 0xEA, 0x00]) == HIDCONS_VOL_DOWN, &mut t);
    check(decode_core(&m, &[0x01, 0xE2, 0x00]) == HIDCONS_MUTE, &mut t);
    // Release, and a report for a DIFFERENT id, must both read as nothing.
    check(decode_core(&m, &[0x01, 0x00, 0x00]) == 0
              && decode_core(&m, &[0x02, 0xE9, 0x00]) == 0, &mut t);

    // ---- 2. VARIABLE bitmap with a Report ID -------------------------------
    parse_core(VEC_VAR_ID2, &mut m);
    check(m.have == (HIDCONS_VOL_UP | HIDCONS_VOL_DOWN | HIDCONS_MUTE), &mut t);
    check(m.kind == HIDCONS_KIND_VARIABLE, &mut t);
    check(m.report_id == 2, &mut t);
    check(m.bit_vol_up == 0 && m.bit_vol_down == 1 && m.bit_mute == 2, &mut t);
    check(m.top_consumer == 1, &mut t);
    check(decode_core(&m, &[0x02, 0x01]) == HIDCONS_VOL_UP, &mut t);
    check(decode_core(&m, &[0x02, 0x02]) == HIDCONS_VOL_DOWN, &mut t);
    check(decode_core(&m, &[0x02, 0x04]) == HIDCONS_MUTE, &mut t);
    check(decode_core(&m, &[0x02, 0x00]) == 0, &mut t);

    // ---- 3. VARIABLE bitmap with NO Report ID ------------------------------
    // The payload starts at byte 0. A parser that assumes an ID byte reads the
    // bitmap out of byte 1 and finds nothing, for ever, silently.
    parse_core(VEC_VAR_NOID, &mut m);
    check(m.report_id == 0, &mut t);
    check(m.bit_mute == 0 && m.bit_vol_up == 1 && m.bit_vol_down == 2, &mut t);
    check(decode_core(&m, &[0x02]) == HIDCONS_VOL_UP, &mut t);
    check(decode_core(&m, &[0x01]) == HIDCONS_MUTE, &mut t);

    // ---- 4. A real boot mouse: find nothing, and SAY it is a mouse ---------
    parse_core(VEC_BOOT_MOUSE, &mut m);
    check(m.have == 0, &mut t);
    check(m.kind == HIDCONS_KIND_NONE, &mut t);
    check(m.top_mouse == 1, &mut t);
    check(m.top_consumer == 0, &mut t);

    // ---- 5. Input/Output bit spaces are independent ------------------------
    parse_core(VEC_KBD_WITH_LED_AND_CONSUMER, &mut m);
    check(m.top_keyboard == 1, &mut t);
    check(m.top_consumer == 1, &mut t);
    check(m.have == (HIDCONS_VOL_UP | HIDCONS_VOL_DOWN), &mut t);
    // 8 modifier bits + 8 constant + 48 keycode array = 64 INPUT bits. The two
    // OUTPUT (LED) items contribute 8 bits to the OUTPUT space and must NOT
    // move these. 64 and 65, not 72 and 73.
    check(m.bit_vol_up == 64 && m.bit_vol_down == 65, &mut t);
    check(decode_core(&m, &[0, 0, 0, 0, 0, 0, 0, 0, 0x02]) == HIDCONS_VOL_DOWN,
          &mut t);

    // ---- 6. The right code on the wrong page must not match ----------------
    parse_core(VEC_WRONG_PAGE, &mut m);
    check(m.have == 0, &mut t);
    check(m.top_keyboard == 1, &mut t);

    // ---- 7. Adversarial input: truncated, and an all-0xFF descriptor -------
    // Neither may fire a control; both must simply terminate.
    parse_core(&VEC_ARRAY_ID1[0..7], &mut m);
    check(m.have == 0, &mut t);
    let junk = [0xFFu8; 64];
    parse_core(&junk, &mut m);
    check(m.have == 0, &mut t);

    if !out_checks.is_null() {
        *out_checks = t.checks;
    }
    if !out_first_fail.is_null() {
        *out_first_fail = t.first_fail;
    }
    t.fails as i32
}
