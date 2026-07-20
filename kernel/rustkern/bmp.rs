// rustkern/bmp.rs - #404 Phase U BMP image decoder (Tier-2 untrusted input)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ============================================================================
// #404 Phase U port: BMP image decoder (Tier-2 untrusted-input; the FIRST image
// decoder ported). Image decoders are the densest OOB surface in a kernel: the
// pixel bytes are attacker-supplyable (a wallpaper BMP on the FAT ESP, a
// downloaded/preview image), and the classic BMP bugs are (i) a width*height*bpp
// INTEGER OVERFLOW that undersizes the output alloc -> OOB WRITE while decoding,
// (ii) a lying bfOffBits / row stride that reads PAST the input buffer, (iii)
// absurd/unsupported dimensions. gui/image.c's image_load_bmp is LIVE and
// reachable: the desktop decodes the wallpaper + BOOT BMP through it at boot, so
// a correctly rendered wallpaper is live proof of a pixel-correct decode here.
//
// Faithful, memory-safe drop-in for bmp_decode_c (gui/image.c) - the pure decode
// seam lifted verbatim out of image_load_bmp: parse BITMAPFILEHEADER (BM magic,
// bfOffBits) + BITMAPINFOHEADER (width, height, bpp, compression), validate, and
// decode 24/32bpp uncompressed rows (bottom-up or top-down) BGR(A) -> 0x00RRGGBB
// into a caller-BOUNDED output buffer (out_px, out_cap_px pixels). No alloc, no
// GUI: image_load_bmp keeps the kmalloc + framebuffer work in C and calls this
// twice (out_px=NULL to validate + size, then to decode into the exact buffer).
//
// SECURITY (HONEST - this C is one of the SAFER references, arp/dns/url tier, NOT
// dhcp/ext2/fat reachable): the classic undersized-alloc OOB WRITE is NOT
// reachable in the live C, because image_load_bmp sizes the output as a 64-bit
// (size_t)width*height*4 (no overflow for int32 dims) and kmalloc rejects
// anything the 256MB heap cannot hold; and the input reads are bounded by the
// pixel-data gate for sane dims. The residual defect the Rust removes BY
// CONSTRUCTION is the C's uint32 row_size / pixel_data_size arithmetic (and the
// uint32 data_offset+pixel_data_size gate): a crafted header with width*bpp or
// row_size*height >= 2^32 WRAPS the gate, so the C ACCEPTS an impossible
// (multi-GB) header at parse and defers the rejection to a doomed kmalloc, while
// this Rust computes the TRUE span in u64 (no wrap) and rejects the lying header
// at parse. It also (b) confines the negation UB the C has on height == INT32_MIN
// (wrapping_neg is defined) and (c) bounds every pixel-source read via a slice
// spanning exactly `len`. On real (well-formed) BMPs it is byte-for-byte
// identical to the C - proven pixel-identical on the live 1080x720 boot.bmp plus
// a 2,000,000-vector offline differential, 0 mismatches. LATENT/defense-in-depth
// integer-overflow-class removal, not a live-OOB fix (honest).
//
// No #[repr(C)] struct crosses this FFI (all scalars + out-pointers), so there is
// no struct-size drift to lock; the extern prototype is asserted-by-use in
// gui/image.c (a mismatched signature fails the C compile).
// ============================================================================

use crate::common::{elf_rd_u16, elf_rd_u32};

const IMAGE_SUCCESS_RS: i32 = 0;
const IMAGE_ERR_NULL_PTR_RS: i32 = -1;
const IMAGE_ERR_INVALID_SIG_RS: i32 = -2;
const IMAGE_ERR_UNSUPPORTED_RS: i32 = -3;
const IMAGE_ERR_NOMEM_RS: i32 = -4;
const IMAGE_ERR_CORRUPT_RS: i32 = -5;
const IMAGE_ERR_TOO_SMALL_RS: i32 = -6;
const BMP_SIGNATURE_RS: u16 = 0x4D42; // "BM" little-endian

/// Rust port of bmp_decode_c (gui/image.c). Pure: no alloc, no GUI, no FS/IO.
/// Same contract: IMAGE_SUCCESS(0) or a negative IMAGE_ERR_*; out_w/out_h receive
/// the dimensions; out_px NULL = validate + report dims only (no decode); else
/// decode into out_px[0..width*height) (rejects IMAGE_ERR_NOMEM if that exceeds
/// out_cap_px). Byte-identical to the C on well-formed BMPs.
#[no_mangle]
pub extern "C" fn bmp_decode_rs(
    buf: *const u8,
    len: u32,
    out_px: *mut u32,
    out_cap_px: u32,
    out_w: *mut u32,
    out_h: *mut u32,
) -> i32 {
    if buf.is_null() || out_w.is_null() || out_h.is_null() {
        return IMAGE_ERR_NULL_PTR_RS;
    }
    // SAFETY: out_w/out_h are non-null (checked) and each point to a writable u32
    // (the C caller passes &uint32_t locals). Zeroed before any early return so
    // the C contract "dims cleared on entry" holds on every path.
    unsafe {
        *out_w = 0;
        *out_h = 0;
    }

    if len < 54 {
        return IMAGE_ERR_TOO_SMALL_RS; // file header (14) + info header (40)
    }

    // SAFETY: the caller (image_load_bmp) guarantees `buf` points to at least
    // `len` contiguous readable bytes (the FAT/download buffer it was handed). We
    // span exactly `len` and index ONLY through this slice, so every header field
    // and pixel-source read is bounds-checked by Rust: a lying bfOffBits, width,
    // height or row stride can only hit a guard or a checked index proven in range
    // by the pixel_data_size gate, never an OOB read.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(buf, len as usize) };

    // File header: signature@0, data_offset@10. Info header @14: header_size@14,
    // width@18(i32), height@22(i32), bpp@28(u16), compression@30(u32). len>=54
    // makes every one of these reads in-bounds (elf_rd_* returns Some).
    if elf_rd_u16(s, 0).unwrap_or(0) != BMP_SIGNATURE_RS {
        return IMAGE_ERR_INVALID_SIG_RS;
    }
    let data_offset = elf_rd_u32(s, 10).unwrap_or(0);
    let header_size = elf_rd_u32(s, 14).unwrap_or(0);
    if header_size < 40 {
        return IMAGE_ERR_UNSUPPORTED_RS;
    }
    let width_raw = elf_rd_u32(s, 18).unwrap_or(0) as i32;
    let height_raw = elf_rd_u32(s, 22).unwrap_or(0) as i32;
    let bpp = elf_rd_u16(s, 28).unwrap_or(0);
    let compression = elf_rd_u32(s, 30).unwrap_or(0);

    let width = width_raw;
    let mut height = height_raw;
    let mut top_down = false;
    if height < 0 {
        // C does `height = -height` (signed-overflow UB on INT32_MIN);
        // wrapping_neg is defined and yields the same INT32_MIN, which the
        // `height <= 0` guard below then rejects - identical accept/reject.
        height = height.wrapping_neg();
        top_down = true;
    }
    if width <= 0 || height <= 0 {
        return IMAGE_ERR_CORRUPT_RS;
    }
    if bpp != 24 && bpp != 32 {
        return IMAGE_ERR_UNSUPPORTED_RS;
    }
    if compression != 0 && compression != 3 {
        return IMAGE_ERR_UNSUPPORTED_RS; // only BI_RGB / BI_BITFIELDS
    }
    if compression == 3 && bpp != 32 {
        return IMAGE_ERR_UNSUPPORTED_RS;
    }
    if data_offset >= len {
        return IMAGE_ERR_CORRUPT_RS;
    }

    // CHECKED (u64) size arithmetic - the memory-safety win. The C computes
    // row_size and pixel_data_size in uint32 (wraps on a crafted >=~2^30 width or
    // >=~2^32-byte pixel span) and gates in uint32, so a lying header can wrap the
    // input-bounds gate. width,height are in (0, 2^31) here, so every product fits
    // u64 with no wrap and the gate is on the TRUE byte span. On sane (real)
    // images this equals the C exactly (identical decode); on a wrapping header
    // the Rust rejects at parse where the C would defer to (and fail at) alloc.
    let bytes_per_pixel = (bpp / 8) as u64;
    let width_u = width as u64;
    let height_u = height as u64;
    let row_size_unpadded = width_u * bytes_per_pixel;
    let row_padding = (4 - (row_size_unpadded % 4)) % 4;
    let row_size = row_size_unpadded + row_padding;
    let pixel_data_size = row_size * height_u;
    if (data_offset as u64) + pixel_data_size > (len as u64) {
        return IMAGE_ERR_CORRUPT_RS;
    }

    // SAFETY: out_w/out_h non-null (checked). Report dims, mirroring the C which
    // sets them before the dims-only early return and the capacity check.
    unsafe {
        *out_w = width as u32;
        *out_h = height as u32;
    }
    if out_px.is_null() {
        return IMAGE_SUCCESS_RS; // validate + dimensions only, no decode
    }

    // Output-capacity gate: mirrors image_load_bmp's 64-bit
    // (size_t)width*height*4 allocation sizing. width_u*height_u < 2^62 (no wrap).
    if width_u * height_u > (out_cap_px as u64) {
        return IMAGE_ERR_NOMEM_RS;
    }

    let total_px = (width_u * height_u) as usize;
    // SAFETY: out_px is non-null and points to at least out_cap_px writable u32
    // (the C caller's kmalloc(out_cap_px*4) buffer); total_px <= out_cap_px by the
    // gate just above, so this slice never exceeds the caller allocation and every
    // out[..] write below is a checked index within it.
    let out: &mut [u32] = unsafe { core::slice::from_raw_parts_mut(out_px, total_px) };

    let data_off = data_offset as usize;
    let rs = row_size as usize;
    let bpp_b = bytes_per_pixel as usize;
    let w = width as usize;
    let h = height as usize;

    // Bottom-up (or top-down) BGR(A) -> 0x00RRGGBB, byte-identical to the C loop
    // (both branches take src_pixel[0..2] as b,g,r; 32bpp ignores alpha). Every
    // s[..] read index sp+2 <= data_off + pixel_data_size - 1 < len (proven by the
    // gate) and every out[..] write index < total_px, so neither can OOB nor panic
    // on any input that reached here.
    let mut y = 0usize;
    while y < h {
        let src_y = if top_down { y } else { h - 1 - y };
        let row_base = data_off + src_y * rs;
        let dst_base = y * w;
        let mut x = 0usize;
        while x < w {
            let sp = row_base + x * bpp_b;
            let b = s[sp] as u32;
            let g = s[sp + 1] as u32;
            let r = s[sp + 2] as u32;
            out[dst_base + x] = (r << 16) | (g << 8) | b;
            x += 1;
        }
        y += 1;
    }
    IMAGE_SUCCESS_RS
}
