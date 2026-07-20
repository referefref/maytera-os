// rustkern/png.rs - #404 Phase V PNG parse seams (gui/png.c)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #404 Phase V: PNG parse seams (gui/png.c). Two classic-OOB seams around the
// C zlib/inflate core (which stays C, a later port):
//   1. png_parse_ihdr_rs - IHDR field validation + bpp/scanline_len/raw_size
//      computed with CHECKED (u64) arithmetic. The C reference computes these in
//      uint32 (wrapping on a crafted >= ~2^30 width), letting an impossible IHDR
//      through with tiny wrapped sizes; downstream the height-row / width-column
//      decode loops then walk far past the wrapped-small heap allocations =>
//      REACHABLE heap OOB read + OOB WRITE. This seam REJECTS the impossible
//      header at parse (before any alloc/loop), confining the class.
//   2. png_defilter_rs - the per-scanline None/Sub/Up/Average/Paeth
//      reconstruction over the inflated buffer, the densest OOB-WRITE surface.
//      Every input read is bounds-checked against inflated_len and every output
//      write against out_cap (a slice of exactly the given lengths). The C
//      png_defilter_c has NEITHER bound (verbatim reference).
// Boot-time png_rust_selftest (gui/png.c) proves these == the C on THIS build.
// ===========================================================================

pub(crate) const PNG_SUCCESS_RS: i32 = 0;
const PNG_ERR_NULL_PTR_RS: i32 = -1;
const PNG_ERR_UNSUPPORTED_RS: i32 = -3;
const PNG_ERR_CORRUPT_RS: i32 = -5;

const PNG_COLOR_GRAYSCALE_RS: u32 = 0;
const PNG_COLOR_RGB_RS: u32 = 2;
const PNG_COLOR_GRAYSCALE_A_RS: u32 = 4;
const PNG_COLOR_RGBA_RS: u32 = 6;

// #[repr(C)] mirror of the C PngInfo (gui/png.h). All u32 => 32 bytes, no
// padding; the C side locks sizeof==32 with a _Static_assert.
#[repr(C)]
pub struct PngInfoRs {
    width: u32,
    height: u32,
    bit_depth: u32,
    color_type: u32,
    interlace: u32,
    bpp: u32,
    scanline_len: u32,
    raw_size: u32,
}

// Big-endian u32 read (PNG is big-endian, unlike the little-endian elf_rd_u32).
#[inline]
fn png_rd_be32(s: &[u8], off: usize) -> u32 {
    ((s[off] as u32) << 24)
        | ((s[off + 1] as u32) << 16)
        | ((s[off + 2] as u32) << 8)
        | (s[off + 3] as u32)
}

/// Rust port of png_parse_ihdr_c (gui/png.c). Validates the 13-byte IHDR payload
/// and computes bpp/scanline_len/raw_size. Byte-identical to the C on well-formed
/// IHDRs; on a crafted header whose width*bpp or (scanline_len+1)*height would
/// overflow u32, it REJECTS (PNG_ERR_UNSUPPORTED) where the C's uint32 math wraps
/// and ACCEPTS. Does NOT reject zero dims (image_load_png does that later,
/// unchanged).
#[no_mangle]
pub extern "C" fn png_parse_ihdr_rs(ihdr: *const u8, len: u32, out: *mut PngInfoRs) -> i32 {
    if ihdr.is_null() || out.is_null() {
        return PNG_ERR_NULL_PTR_RS;
    }
    // SAFETY: out is non-null (checked) and points to a writable PngInfoRs (the C
    // caller passes &PngInfo, sizeof-locked ==32 to match this #[repr(C)]).
    // Zeroed on entry so every early-return path leaves out cleared, matching the
    // C `memset(out,0,sizeof(*out))`.
    unsafe {
        *out = PngInfoRs {
            width: 0, height: 0, bit_depth: 0, color_type: 0,
            interlace: 0, bpp: 0, scanline_len: 0, raw_size: 0,
        };
    }
    if len < 13 {
        return PNG_ERR_CORRUPT_RS;
    }
    // SAFETY: the caller guarantees `ihdr` points to at least `len` (>=13)
    // readable bytes (the IHDR chunk payload inside the PNG buffer). We span
    // exactly `len` and index only 0..=12, all in-bounds and bounds-checked.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(ihdr, len as usize) };

    let width = png_rd_be32(s, 0);
    let height = png_rd_be32(s, 4);
    let bit_depth = s[8] as u32;
    let color_type = s[9] as u32;
    let interlace = s[12] as u32;

    if bit_depth != 8 {
        return PNG_ERR_UNSUPPORTED_RS;
    }
    if color_type != PNG_COLOR_RGB_RS
        && color_type != PNG_COLOR_RGBA_RS
        && color_type != PNG_COLOR_GRAYSCALE_RS
        && color_type != PNG_COLOR_GRAYSCALE_A_RS
    {
        return PNG_ERR_UNSUPPORTED_RS;
    }
    if interlace != 0 {
        return PNG_ERR_UNSUPPORTED_RS;
    }
    let bpp: u32 = match color_type {
        PNG_COLOR_GRAYSCALE_RS => 1,
        PNG_COLOR_GRAYSCALE_A_RS => 2,
        PNG_COLOR_RGB_RS => 3,
        PNG_COLOR_RGBA_RS => 4,
        _ => return PNG_ERR_UNSUPPORTED_RS,
    };

    // CHECKED (u64) size math - the memory-safety win. The C computes these in
    // uint32 (wraps on a crafted width); u64 here never wraps, and if the true
    // value exceeds u32 we REJECT rather than hand a wrapped-small size back to
    // the (uint32) allocators. On sane images width*bpp and (scanline+1)*height
    // both fit u32 => identical scanline_len/raw_size to the C (identical decode).
    let scanline_len_u64 = (width as u64) * (bpp as u64);
    if scanline_len_u64 > (u32::MAX as u64) {
        return PNG_ERR_UNSUPPORTED_RS;
    }
    let raw_size_u64 = (scanline_len_u64 + 1) * (height as u64);
    if raw_size_u64 > (u32::MAX as u64) {
        return PNG_ERR_UNSUPPORTED_RS;
    }

    // SAFETY: out non-null (checked); write the validated fields.
    unsafe {
        *out = PngInfoRs {
            width,
            height,
            bit_depth,
            color_type,
            interlace,
            bpp,
            scanline_len: scanline_len_u64 as u32,
            raw_size: raw_size_u64 as u32,
        };
    }
    PNG_SUCCESS_RS
}

// Paeth predictor - byte-identical to the C paeth_predictor (returns u8 of the
// selected neighbor). a/b/c are 0..=255 so `as u8` is a no-op truncation.
#[inline]
fn png_paeth_rs(a: i32, b: i32, c: i32) -> u8 {
    let p = a + b - c;
    let pa = if p > a { p - a } else { a - p };
    let pb = if p > b { p - b } else { b - p };
    let pc = if p > c { p - c } else { c - p };
    if pa <= pb && pa <= pc {
        a as u8
    } else if pb <= pc {
        b as u8
    } else {
        c as u8
    }
}

// Reconstruct one scanline in place within `out` (byte-identical to the C
// unfilter_scanline). `cur` is the row base in out; `prev` is the previous row's
// base (already reconstructed) or None for row 0. All indices are checked against
// the callers-verified bounds. filter values outside 0..=4 are a no-op (the raw
// copy stays), matching the C switch's absent default.
fn png_unfilter_row_rs(out: &mut [u8], cur: usize, prev: Option<usize>, len: usize, bpp: usize, filter: u8) {
    match filter {
        1 => {
            // Sub
            let mut i = bpp;
            while i < len {
                let v = out[cur + i].wrapping_add(out[cur + i - bpp]);
                out[cur + i] = v;
                i += 1;
            }
        }
        2 => {
            // Up
            if let Some(p) = prev {
                let mut i = 0;
                while i < len {
                    let v = out[cur + i].wrapping_add(out[p + i]);
                    out[cur + i] = v;
                    i += 1;
                }
            }
        }
        3 => {
            // Average
            let mut i = 0;
            while i < len {
                let a = if i >= bpp { out[cur + i - bpp] as i32 } else { 0 };
                let b = match prev { Some(p) => out[p + i] as i32, None => 0 };
                let v = out[cur + i].wrapping_add(((a + b) / 2) as u8);
                out[cur + i] = v;
                i += 1;
            }
        }
        4 => {
            // Paeth
            let mut i = 0;
            while i < len {
                let a = if i >= bpp { out[cur + i - bpp] as i32 } else { 0 };
                let b = match prev { Some(p) => out[p + i] as i32, None => 0 };
                let c = match prev {
                    Some(p) => if i >= bpp { out[p + i - bpp] as i32 } else { 0 },
                    None => 0,
                };
                let v = out[cur + i].wrapping_add(png_paeth_rs(a, b, c));
                out[cur + i] = v;
                i += 1;
            }
        }
        _ => {} // None (0) or unknown -> raw bytes unchanged
    }
}

/// Rust port of png_defilter_c (gui/png.c). Reconstructs all None/Sub/Up/Average/
/// Paeth scanlines of the inflated buffer into `out` (packed stride*height, no
/// filter bytes). BOUNDS-CHECKED: every input read is confined to inflated_len
/// and every output write to out_cap (the C reference has NEITHER bound and walks
/// past both buffers on a wrapped stride or truncated input). Byte-identical to
/// the C on well-formed inputs; rejects (PNG_ERR_CORRUPT) any row that would read
/// past inflated_len or write past out_cap.
#[no_mangle]
pub extern "C" fn png_defilter_rs(
    inflated: *const u8,
    inflated_len: u32,
    width: u32,
    height: u32,
    bpp: u32,
    out: *mut u8,
    out_cap: u32,
) -> i32 {
    if inflated.is_null() || out.is_null() {
        return PNG_ERR_NULL_PTR_RS;
    }
    // Checked stride: width*bpp in u64 (the C uses uint32, which can wrap). A
    // stride that would not fit u32 cannot be a real scanline -> reject.
    let stride_u64 = (width as u64) * (bpp as u64);
    if stride_u64 > (u32::MAX as u64) {
        return PNG_ERR_CORRUPT_RS;
    }
    let stride = stride_u64 as usize;
    let bpp = bpp as usize;
    let h = height as usize;

    // SAFETY: the caller guarantees `inflated` has at least `inflated_len`
    // readable bytes and `out` at least `out_cap` writable bytes (the C caller's
    // kmalloc'd raw / defiltered buffers). We span exactly those lengths and index
    // only through the slices, so every read/write below is bounds-checked.
    let in_s: &[u8] = unsafe { core::slice::from_raw_parts(inflated, inflated_len as usize) };
    let out_s: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(out, out_cap as usize) };

    let mut y = 0usize;
    while y < h {
        // Input row = filter byte + stride bytes at y*(stride+1). Bound it.
        let in_off = match y.checked_mul(stride + 1) {
            Some(v) => v,
            None => return PNG_ERR_CORRUPT_RS,
        };
        let in_end = match in_off.checked_add(1 + stride) {
            Some(v) => v,
            None => return PNG_ERR_CORRUPT_RS,
        };
        if in_end > in_s.len() {
            return PNG_ERR_CORRUPT_RS;
        }
        // Output row = stride bytes at y*stride. Bound it.
        let out_off = y * stride; // y<h, stride fits u32 => fits usize, no overflow
        let out_end = match out_off.checked_add(stride) {
            Some(v) => v,
            None => return PNG_ERR_CORRUPT_RS,
        };
        if out_end > out_s.len() {
            return PNG_ERR_CORRUPT_RS;
        }

        let filter = in_s[in_off];
        // Copy the raw (still-filtered) scanline into the output row.
        out_s[out_off..out_off + stride].copy_from_slice(&in_s[in_off + 1..in_off + 1 + stride]);
        // Reconstruct in place, referencing the previous output row for y>0.
        let prev = if y > 0 { Some((y - 1) * stride) } else { None };
        png_unfilter_row_rs(out_s, out_off, prev, stride, bpp, filter);
        y += 1;
    }
    PNG_SUCCESS_RS
}
