// rustkern/jpeg.rs - #404 Phase W + batch-1 gui/jpeg.c seams: header parse, dequant + IDCT
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #404 Phase W: gui/jpeg.c pure JPEG header-parse seam (jpeg_parse_headers_rs).
//
// Tier 2 image decoder, the THIRD after bmp/png and a genuinely REACHABLE set
// of OOBs in the C reference (album art / Image Viewer / browser <img> / Files
// previews all feed image_load_jpeg with untrusted bytes). The seam is the
// marker/segment walk from after SOI up to and INCLUDING the SOS scan header:
// SOI/APPn/COM/DQT/DHT/SOF0/SOS. It fills a JpegHdr out-struct; the entropy /
// Huffman-build / IDCT / MCU-upsample / color-convert decode STAYS C (a later
// seam), gated behind this parse.
//
// This is a byte-for-byte drop-in for jpeg_parse_headers_c on well-formed input
// AND on every generic-malformed input (truncation, bad marker order, bad sig).
// It DIVERGES only where the C walks into a REACHABLE OOB, which it confines:
//   (A) SOF0 comp_qt UNVALIDATED in C -> decode_block indexes quant[comp_qt][k]
//       (quant is [4][64]); comp_qt is a full byte (0..255) -> past-allocation
//       heap OOB READ. Rust rejects comp_qt >= 4.
//   (B) SOS comp_dc/comp_ac UNVALIDATED in C (0..15) -> decode_huff indexes
//       huff_fast[dc][comp_dc] ([2] deep) -> past-allocation heap OOB READ.
//       Rust rejects comp_dc/comp_ac >= 2.
//   (C) DHT sum-of-16-count-bytes (up to 4080) written into huff_vals[.][.][256]
//       with NO clamp in C -> heap OOB WRITE (the severe one). Rust rejects
//       total > 256 before writing a single value byte.
// comp_h/comp_v are left UNvalidated to match C byte-for-byte: garbage sampling
// factors do NOT cause an OOB in the C entropy decoder (the block loops are
// gated by `bx<h`/`by<v` and every target/pixel write is bounds-checked), so
// confining them would be a false divergence, not a safety win.
// ===========================================================================

const JPEG_SUCCESS_RS: i32 = 0;
const JPEG_ERR_NULL_PTR_RS: i32 = -1;
const JPEG_ERR_INVALID_SIG_RS: i32 = -2;
const JPEG_ERR_UNSUPPORTED_RS: i32 = -3;
const JPEG_ERR_CORRUPT_RS: i32 = -5;
const JPEG_ERR_TOO_SMALL_RS: i32 = -6;

const JPEG_MARKER_SOF0_RS: i32 = 0xFFC0;
const JPEG_MARKER_DHT_RS: i32 = 0xFFC4;
const JPEG_MARKER_SOS_RS: i32 = 0xFFDA;
const JPEG_MARKER_EOI_RS: i32 = 0xFFD9;
const JPEG_MARKER_DQT_RS: i32 = 0xFFDB;
const JPEG_MARKER_DRI_RS: i32 = 0xFFDD;

const JPEG_MAX_WIDTH_RS: i32 = 4096;
const JPEG_MAX_HEIGHT_RS: i32 = 4096;

// #[repr(C)] mirror of jpeg_hdr_t (gui/jpeg.h). Field order + types IDENTICAL to
// the C struct; sizeof asserted == 1512 on the C side (_Static_assert in
// gui/jpeg.c) so this FFI struct can never silently drift.
#[repr(C)]
pub struct JpegHdr {
    width: u32,
    height: u32,
    components: i32,
    comp_id: [i32; 4],
    comp_h: [i32; 4],
    comp_v: [i32; 4],
    comp_qt: [i32; 4],
    comp_dc: [i32; 4],
    comp_ac: [i32; 4],
    mcu_width: i32,
    mcu_height: i32,
    mcu_count_x: i32,
    mcu_count_y: i32,
    restart_interval: i32,
    quant_valid: [i32; 4],
    huff_valid: [[i32; 2]; 2],
    entropy_pos: u32,
    status: i32,
    quant: [[u8; 64]; 4],
    huff_bits: [[[u8; 16]; 2]; 2],
    huff_vals: [[[u8; 256]; 2]; 2],
}

// Bounds-checked big-endian cursor over a slice of exactly `len` bytes. byte()
// and u16() return the C read_byte/read_u16 semantics: the value, or -1 at EOF.
// Every read is a bounds-checked index, so nothing can leave the buffer.
struct JCur<'a> {
    d: &'a [u8],
    pos: usize,
}
impl<'a> JCur<'a> {
    #[inline]
    fn byte(&mut self) -> i32 {
        if self.pos >= self.d.len() {
            -1
        } else {
            let b = self.d[self.pos] as i32;
            self.pos += 1;
            b
        }
    }
    #[inline]
    fn u16(&mut self) -> i32 {
        let hi = self.byte();
        let lo = self.byte();
        if hi < 0 || lo < 0 {
            -1
        } else {
            (hi << 8) | lo
        }
    }
}

fn jpeg_dqt_rs(r: &mut JCur, h: &mut JpegHdr) -> i32 {
    let mut l = r.u16();
    if l < 0 {
        return JPEG_ERR_CORRUPT_RS;
    }
    l -= 2;
    while l > 0 {
        let info = r.byte();
        if info < 0 {
            return JPEG_ERR_CORRUPT_RS;
        }
        l -= 1;
        let prec = info >> 4;
        let idx = (info & 0x0F) as usize;
        if idx >= 4 {
            return JPEG_ERR_CORRUPT_RS;
        }
        if prec != 0 {
            return JPEG_ERR_UNSUPPORTED_RS; // 16-bit not supported
        }
        for i in 0..64usize {
            let v = r.byte();
            if v < 0 {
                return JPEG_ERR_CORRUPT_RS;
            }
            h.quant[idx][i] = v as u8;
        }
        h.quant_valid[idx] = 1;
        l -= 64;
    }
    JPEG_SUCCESS_RS
}

fn jpeg_dht_rs(r: &mut JCur, h: &mut JpegHdr) -> i32 {
    let mut l = r.u16();
    if l < 0 {
        return JPEG_ERR_CORRUPT_RS;
    }
    l -= 2;
    while l > 0 {
        let info = r.byte();
        if info < 0 {
            return JPEG_ERR_CORRUPT_RS;
        }
        l -= 1;
        let dc = ((info >> 4) & 1) as usize;
        let idx = (info & 0x0F) as usize;
        if idx >= 2 {
            return JPEG_ERR_CORRUPT_RS;
        }
        let mut total: i32 = 0;
        for i in 0..16usize {
            let count = r.byte();
            if count < 0 {
                return JPEG_ERR_CORRUPT_RS;
            }
            h.huff_bits[dc][idx][i] = count as u8;
            total += count;
        }
        l -= 16;
        // CONFINEMENT (C bug (C)): huff_vals[dc][idx] is [256]; the C writes
        // `total` (up to 4080) entries with no clamp -> heap OOB WRITE. Reject.
        if total > 256 {
            return JPEG_ERR_CORRUPT_RS;
        }
        // CONFINEMENT (MAYTERA-SEC-2026-00XX): the count-sum gate above is NOT
        // sufficient, and shipping it alone left a live ~259 KB OOB WRITE.
        // build_huffman (plain C, just OUTSIDE this seam's boundary since b814)
        // walks `code` unbounded for a NON-CANONICAL bits[]. bits[0]=255 sums to
        // 255, passes `total > 256`, and then writes 261,118 bytes past
        // huff_fast[dc][idx][1024]. Reject any table whose code space overflows.
        //
        // This mirrors build_huffman's own guard exactly, so the two agree on
        // every input: a table is canonical iff, at each length len=i+1, the
        // running code count after adding bits[i] never exceeds (1 << len).
        // Deliberately NOT an expansion or a narrowing relative to the fixed C.
        {
            let mut code: i32 = 0;
            for i in 0..16usize {
                let len = (i + 1) as u32;
                code += h.huff_bits[dc][idx][i] as i32;
                if code > (1i32 << len) {
                    return JPEG_ERR_CORRUPT_RS;
                }
                code <<= 1;
            }
        }
        for i in 0..(total as usize) {
            let v = r.byte();
            if v < 0 {
                return JPEG_ERR_CORRUPT_RS;
            }
            h.huff_vals[dc][idx][i] = v as u8;
        }
        l -= total;
        h.huff_valid[dc][idx] = 1;
    }
    JPEG_SUCCESS_RS
}

fn jpeg_sof0_rs(r: &mut JCur, h: &mut JpegHdr) -> i32 {
    let l = r.u16();
    if l < 0 {
        return JPEG_ERR_CORRUPT_RS;
    }
    let prec = r.byte();
    if prec != 8 {
        return JPEG_ERR_UNSUPPORTED_RS; // 8-bit only
    }
    let height = r.u16();
    let width = r.u16();
    let comps = r.byte();
    if width <= 0 || height <= 0 {
        return JPEG_ERR_CORRUPT_RS;
    }
    if width > JPEG_MAX_WIDTH_RS || height > JPEG_MAX_HEIGHT_RS {
        return JPEG_ERR_UNSUPPORTED_RS;
    }
    if comps != 1 && comps != 3 {
        return JPEG_ERR_UNSUPPORTED_RS;
    }
    h.width = width as u32;
    h.height = height as u32;
    h.components = comps;
    let mut max_h = 1i32;
    let mut max_v = 1i32;
    for i in 0..(comps as usize) {
        h.comp_id[i] = r.byte();
        let sampling = r.byte();
        h.comp_h[i] = sampling >> 4;
        h.comp_v[i] = sampling & 0x0F;
        h.comp_qt[i] = r.byte();
        // CONFINEMENT (C bug (A)): comp_qt indexes quant[comp_qt][k] (quant is
        // [4][64]) in the downstream C decode_block. Reject the reachable
        // over-index (>= 4). Negative (EOF-truncated) is left to match C: a
        // truncated SOF0 has no following SOS, so no decode runs.
        if h.comp_qt[i] >= 4 {
            return JPEG_ERR_CORRUPT_RS;
        }
        if h.comp_h[i] > max_h {
            max_h = h.comp_h[i];
        }
        if h.comp_v[i] > max_v {
            max_v = h.comp_v[i];
        }
    }
    h.mcu_width = max_h * 8;
    h.mcu_height = max_v * 8;
    h.mcu_count_x = (width + h.mcu_width - 1) / h.mcu_width;
    h.mcu_count_y = (height + h.mcu_height - 1) / h.mcu_height;
    JPEG_SUCCESS_RS
}

fn jpeg_sos_rs(r: &mut JCur, h: &mut JpegHdr) -> i32 {
    let l = r.u16();
    if l < 0 {
        return JPEG_ERR_CORRUPT_RS;
    }
    let ns = r.byte();
    if ns != h.components {
        return JPEG_ERR_CORRUPT_RS;
    }
    for _ in 0..(ns as usize) {
        let id = r.byte();
        let tables = r.byte();
        let ncomp = h.components as usize;
        for j in 0..ncomp {
            if h.comp_id[j] == id {
                h.comp_dc[j] = tables >> 4;
                h.comp_ac[j] = tables & 0x0F;
                // CONFINEMENT (C bug (B)): comp_dc/comp_ac index
                // huff_fast[dc][comp_dc] ([2] deep) in the downstream C
                // decode_huff. Reject the reachable over-index (>= 2). This
                // gates a SCAN that WILL run (entropy_pos>0 follows), so unlike
                // comp_qt the negative EOF case cannot reach decode either; a
                // truncated `tables` (-1) leaves both < 2, matching C.
                if h.comp_dc[j] >= 2 || h.comp_ac[j] >= 2 {
                    return JPEG_ERR_CORRUPT_RS;
                }
                break;
            }
        }
    }
    r.byte(); // Ss
    r.byte(); // Se
    r.byte(); // Ah/Al
    JPEG_SUCCESS_RS
}

/// Rust port of jpeg_parse_headers_c (gui/jpeg.c). Walks the JPEG marker stream
/// from after SOI up to and including the SOS scan header, filling `out`. Returns
/// JPEG_SUCCESS(0) with out.entropy_pos = the first entropy byte on a scan, or
/// JPEG_SUCCESS with entropy_pos==0 when no SOS is found (matching the C's
/// historical no-pixels success), or a negative JPEG_ERR_* on a header error or
/// a confined OOB attempt. PURE: never decodes/allocates; every input read is a
/// bounds-checked index into a slice of exactly `len` bytes.
#[no_mangle]
pub extern "C" fn jpeg_parse_headers_rs(data: *const u8, len: u32, out: *mut JpegHdr) -> i32 {
    // Matches C `if (!data || !out) return JPEG_ERR_NULL_PTR;`.
    if data.is_null() || out.is_null() {
        return JPEG_ERR_NULL_PTR_RS;
    }
    // Build the result in a zeroed local (POD, all-integer => all-zero is valid),
    // matching the C `memset(out, 0, sizeof(*out))`. Written to *out on return.
    let mut h: JpegHdr = unsafe { core::mem::zeroed() };

    // SAFETY: the caller (image_load_jpeg / the boot self-test) guarantees `data`
    // points to at least `len` readable bytes (the JPEG file buffer). We span
    // EXACTLY `len` and index only via bounds-checked slice access below, so no
    // read can leave the buffer even when a segment length or table count
    // (attacker-controlled) is nonsense.
    let n = len as usize;
    let s: &[u8] = unsafe { core::slice::from_raw_parts(data, n) };

    let rc = jpeg_parse_headers_walk(s, n, &mut h);
    h.status = rc;
    // SAFETY: out is non-null (checked) and points to a writable JpegHdr (the C
    // caller passes &jpeg_hdr_t, sizeof-locked ==1512 to match this #[repr(C)]).
    unsafe {
        core::ptr::write(out, h);
    }
    rc
}

// The parse itself, over the already-built slice. Returns the status code; the
// caller stamps out.status and writes out.
fn jpeg_parse_headers_walk(s: &[u8], n: usize, h: &mut JpegHdr) -> i32 {
    if n < 4 {
        return JPEG_ERR_TOO_SMALL_RS;
    }
    if s[0] != 0xFF || s[1] != 0xD8 {
        return JPEG_ERR_INVALID_SIG_RS;
    }
    let mut r = JCur { d: s, pos: 2 }; // skip SOI
    while r.pos < n {
        let mut marker = r.byte();
        if marker != 0xFF {
            continue;
        }
        loop {
            marker = r.byte();
            if marker != 0xFF {
                break;
            }
        }
        if marker < 0 {
            break;
        }
        let full_marker = 0xFF00 | marker;
        match full_marker {
            JPEG_MARKER_SOF0_RS => {
                let rr = jpeg_sof0_rs(&mut r, h);
                if rr != JPEG_SUCCESS_RS {
                    return rr;
                }
            }
            JPEG_MARKER_DHT_RS => {
                let rr = jpeg_dht_rs(&mut r, h);
                if rr != JPEG_SUCCESS_RS {
                    return rr;
                }
            }
            JPEG_MARKER_DQT_RS => {
                let rr = jpeg_dqt_rs(&mut r, h);
                if rr != JPEG_SUCCESS_RS {
                    return rr;
                }
            }
            JPEG_MARKER_DRI_RS => {
                let _ = r.u16(); // length
                h.restart_interval = r.u16();
            }
            JPEG_MARKER_SOS_RS => {
                let rr = jpeg_sos_rs(&mut r, h);
                if rr != JPEG_SUCCESS_RS {
                    return rr;
                }
                h.entropy_pos = r.pos as u32; // entropy-coded data begins here
                return JPEG_SUCCESS_RS;
            }
            JPEG_MARKER_EOI_RS => {
                return JPEG_SUCCESS_RS; // no scan; entropy_pos stays 0
            }
            _ => {
                if marker >= 0xE0 && marker <= 0xEF {
                    let slen = r.u16(); // APPn
                    if slen >= 2 {
                        r.pos = r.pos.saturating_add((slen - 2) as usize);
                    }
                } else if marker == 0xFE {
                    let slen = r.u16(); // Comment
                    if slen >= 2 {
                        r.pos = r.pos.saturating_add((slen - 2) as usize);
                    }
                }
            }
        }
    }
    JPEG_SUCCESS_RS // no SOS found (matches the C no-pixels SUCCESS)
}

// ============================================================================
// #404 BATCH-1 SEAM 1/3: JPEG dequant + inverse-DCT (gui/jpeg.c decode_block)
// ============================================================================
// ===========================================================================
// #404 Phase (JPEG entropy): gui/jpeg.c dequant + inverse-DCT seam.
// PASTE into rustkern.rs (after the jpeg_parse_headers_rs block). rustkern.rs
// already provides the #![no_std] attribute and the #[panic_handler]; add ONLY
// the code below (no extra panic handler).
//
// Seam = the fixed 64-coefficient -> 64-sample transform at the tail of
// decode_block(): scatter the zigzag-order coefficients into a natural-order
// 8x8 block, multiply each by its quantizer, run the integer inverse DCT in
// place. Byte-for-byte identical to jpeg_dequant_idct_c (the verbatim stb_image
// idct_block + the decode_block dequant scatter) over 3,200,000 offline
// differential vectors (0 mismatches: real-quant realistic blocks + uniform +
// extreme incl. INT_MIN/MAX). All indices are bounds-checked slice indexes so
// nothing can leave the 64-entry block; the DSP arithmetic uses wrapping_* to
// match C two's-complement i32 EXACTLY. That last point is the security win:
// the C IDCT has a REACHABLE signed-integer-overflow (CWE-190, UNDEFINED
// BEHAVIOR) on large coefficient products (a crafted JPEG can carry ac_val up
// to +-32767 * quant up to 255); on the current gcc -O2 x86-64 build it wraps
// benignly (hence 0 mismatch) but the compiler is licensed to miscompile it.
// wrapping_* makes the Rust well-defined and identical to the observed wrap.
// This seam has NO reachable memory OOB (ASan-clean over 3.2M vectors): the
// confinement is defense-in-depth, the overflow-UB removal is the real fix.
// ===========================================================================

// zigzag[k] -> natural 8x8 block position. Same 64 constants as gui/jpeg.c.
static JPEG_ZIGZAG_RS: [usize; 64] = [
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
];

// One 8-point inverse-DCT butterfly. Mirrors the C IDCT_1D macro EXACTLY, with
// i32 wrapping ops. Returns the macro post-state (x0,x1,x2,x3,t0,t1,t2,t3).
#[inline]
fn jpeg_idct_1d(
    s0: i32, s1: i32, s2: i32, s3: i32, s4: i32, s5: i32, s6: i32, s7: i32,
) -> (i32, i32, i32, i32, i32, i32, i32, i32) {
    let mut p2 = s2;
    let mut p3 = s6;
    let mut p1 = p2.wrapping_add(p3).wrapping_mul(2217);
    let t2a = p1.wrapping_add(p3.wrapping_mul(-7567));
    let t3a = p1.wrapping_add(p2.wrapping_mul(3135));
    p2 = s0;
    p3 = s4;
    let mut t0 = p2.wrapping_add(p3).wrapping_mul(4096);
    let mut t1 = p2.wrapping_sub(p3).wrapping_mul(4096);
    let x0 = t0.wrapping_add(t3a);
    let x3 = t0.wrapping_sub(t3a);
    let x1 = t1.wrapping_add(t2a);
    let x2 = t1.wrapping_sub(t2a);
    t0 = s7;
    t1 = s5;
    let mut t2 = s3;
    let mut t3 = s1;
    p3 = t0.wrapping_add(t2);
    let mut p4 = t1.wrapping_add(t3);
    p1 = t0.wrapping_add(t3);
    p2 = t1.wrapping_add(t2);
    let p5 = p3.wrapping_add(p4).wrapping_mul(4816);
    t0 = t0.wrapping_mul(1223);
    t1 = t1.wrapping_mul(8410);
    t2 = t2.wrapping_mul(12586);
    t3 = t3.wrapping_mul(6149);
    p1 = p5.wrapping_add(p1.wrapping_mul(-3685));
    p2 = p5.wrapping_add(p2.wrapping_mul(-10497));
    p3 = p3.wrapping_mul(-8034);
    p4 = p4.wrapping_mul(-1597);
    t3 = t3.wrapping_add(p1).wrapping_add(p4);
    t2 = t2.wrapping_add(p2).wrapping_add(p3);
    t1 = t1.wrapping_add(p2).wrapping_add(p4);
    t0 = t0.wrapping_add(p1).wrapping_add(p3);
    (x0, x1, x2, x3, t0, t1, t2, t3)
}

// Integer inverse DCT (8x8) in place. Verbatim port of stb_image idct_block:
// pass 1 over columns (>>10, +512 bias), pass 2 over rows (>>17, +65536 bias).
// Output = signed spatial samples WITHOUT the +128 level shift (caller adds it).
fn jpeg_idct_block_rs(block: &mut [i32; 64]) {
    let mut val = [0i32; 64];
    let mut i = 0usize;
    while i < 8 {
        let (mut x0, mut x1, mut x2, mut x3, t0, t1, t2, t3) = jpeg_idct_1d(
            block[i], block[i + 8], block[i + 16], block[i + 24],
            block[i + 32], block[i + 40], block[i + 48], block[i + 56],
        );
        x0 = x0.wrapping_add(512);
        x1 = x1.wrapping_add(512);
        x2 = x2.wrapping_add(512);
        x3 = x3.wrapping_add(512);
        val[i] = x0.wrapping_add(t3) >> 10;
        val[i + 56] = x0.wrapping_sub(t3) >> 10;
        val[i + 8] = x1.wrapping_add(t2) >> 10;
        val[i + 48] = x1.wrapping_sub(t2) >> 10;
        val[i + 16] = x2.wrapping_add(t1) >> 10;
        val[i + 40] = x2.wrapping_sub(t1) >> 10;
        val[i + 24] = x3.wrapping_add(t0) >> 10;
        val[i + 32] = x3.wrapping_sub(t0) >> 10;
        i += 1;
    }
    i = 0;
    while i < 8 {
        let b = i * 8;
        let (mut x0, mut x1, mut x2, mut x3, t0, t1, t2, t3) = jpeg_idct_1d(
            val[b], val[b + 1], val[b + 2], val[b + 3],
            val[b + 4], val[b + 5], val[b + 6], val[b + 7],
        );
        x0 = x0.wrapping_add(65536);
        x1 = x1.wrapping_add(65536);
        x2 = x2.wrapping_add(65536);
        x3 = x3.wrapping_add(65536);
        block[b] = x0.wrapping_add(t3) >> 17;
        block[b + 7] = x0.wrapping_sub(t3) >> 17;
        block[b + 1] = x1.wrapping_add(t2) >> 17;
        block[b + 6] = x1.wrapping_sub(t2) >> 17;
        block[b + 2] = x2.wrapping_add(t1) >> 17;
        block[b + 5] = x2.wrapping_sub(t1) >> 17;
        block[b + 3] = x3.wrapping_add(t0) >> 17;
        block[b + 4] = x3.wrapping_sub(t0) >> 17;
        i += 1;
    }
}

/// Dequantize (zigzag scatter * quantizer) then inverse-DCT one 8x8 block.
/// Drop-in for jpeg_dequant_idct_c. coeff_zz: 64 i32 in ZIGZAG order (coeff_zz[0]
/// = accumulated DC prediction, coeff_zz[k] = decoded AC at zigzag index k, 0
/// elsewhere). quant: 64 u8 quantizers in ZIGZAG order. out: 64 i32 spatial
/// samples in NATURAL order (no +128 level shift).
///
/// # Safety
/// The caller guarantees coeff_zz and quant point to >= 64 readable elements and
/// out to >= 64 writable i32; the sole caller decode_block satisfies this with
/// fixed [64] stack arrays. We reconstitute owned slices of EXACTLY 64 elements,
/// so every subsequent index (zigzag scatter, both IDCT passes, the final copy)
/// is a bounds-checked slice index that cannot leave the 64-entry block. The DSP
/// multiplies/adds use wrapping_* so no arithmetic can panic-abort.
#[no_mangle]
pub extern "C" fn jpeg_dequant_idct_rs(
    coeff_zz: *const i32,
    quant: *const u8,
    out: *mut i32,
) {
    let cz = unsafe { core::slice::from_raw_parts(coeff_zz, 64) };
    let qt = unsafe { core::slice::from_raw_parts(quant, 64) };
    let mut block = [0i32; 64];
    let mut k = 0usize;
    while k < 64 {
        let pos = JPEG_ZIGZAG_RS[k];
        block[pos] = cz[k].wrapping_mul(qt[k] as i32);
        k += 1;
    }
    jpeg_idct_block_rs(&mut block);
    let outs = unsafe { core::slice::from_raw_parts_mut(out, 64) };
    outs.copy_from_slice(&block);
}
