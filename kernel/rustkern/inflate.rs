// rustkern/inflate.rs - #404 Phase X / #502 DEFLATE/INFLATE decompression core
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #404 Phase X (#502): DEFLATE/INFLATE decompression core - the reachable
// untrusted-input SEAM shared by PNG IDAT (gui/png.c), the gzip/tar.gz/zip
// archiver (#321) and any gzip content-encoding path. This is the canonical
// LZ77 OOB surface:
//   * a bad DISTANCE code points BEFORE the output start (OOB READ + wrong data)
//   * a bad LENGTH writes PAST the output buffer end (OOB WRITE)
//   * bad Huffman code-length tables overrun the symbol tables (OOB R/W)
//   * a truncated/hostile bitstream must terminate, not read past input.
// Faithful, byte-for-byte port of gui/png.c inflate() (kept verbatim as
// inflate_c). image_load_png routes inflate() -> inflate_rs under -DRUST_INFLATE.
//
// SECURITY (proven OFFLINE, see [RUST-SEC] inflate + CHANGELOG): the C
// reference is a puff-style decoder that ALREADY validates every reachable OOB
// surface: the back-reference copy checks BOTH `dist > out_pos` (no read before
// output start) AND `out_pos + len > dst_cap` (no write past the buffer); the
// literal write checks `out_pos >= dst_cap`; the stored block checks input AND
// output bounds; the code-length RLE guards every repeat with `n < hlit+hdist`;
// the Huffman symbol index is bounded by construction (the canonical decode
// invariant code >= first holds, so index+(code-first) stays in [0, total));
// input exhaustion returns -1 through getbit and is propagated. ASan over the
// hostile corpus finds NO OOB in inflate_c. Verdict: LATENT / defense-in-depth,
// NOT a reachable-OOB fix. The Rust wins by construction anyway: every window
// read + output write + input bit-read is a bounds-checked slice access (a
// logic slip HALTS loudly via the panic handler instead of corrupting the heap),
// and pos/dist/len use explicit width so nothing wraps. Drop -DRUST_INFLATE to
// roll straight back to the C core.
// ===========================================================================

// The C inflate returns PNG_ERR_INFLATE (=-8) on any malformed-stream reject and
// PNG_SUCCESS (=0, reused from PNG_SUCCESS_RS above) on success.
use crate::png::PNG_SUCCESS_RS;

const PNG_ERR_INFLATE_RS: i32 = -8;

// RFC 1951 length/distance base + extra-bit tables. Byte-identical to the C
// length_base / length_extra / dist_base / dist_extra in gui/png.c.
static INF_LENGTH_BASE: [i32; 29] = [
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
];
static INF_LENGTH_EXTRA: [i32; 29] = [
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
];
static INF_DIST_BASE: [i32; 30] = [
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577,
];
static INF_DIST_EXTRA: [i32; 30] = [
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
];
// Code-length code order (RFC 1951 3.2.7), identical to the C code_order[19].
static INF_CODE_ORDER: [usize; 19] = [
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
];

// Bit reader over exactly `data` (len == src_len). Mirrors inflate_state_t.
struct InfState<'a> {
    data: &'a [u8],
    pos: usize,
    bit_buf: u32,
    bit_count: i32,
}

impl<'a> InfState<'a> {
    // Mirror of getbit(): pulls the next byte only when the bit buffer is empty
    // and only if `pos` is still in bounds (returns -1 on input exhaustion).
    #[inline]
    fn getbit(&mut self) -> i32 {
        if self.bit_count == 0 {
            if self.pos >= self.data.len() {
                return -1;
            }
            self.bit_buf = self.data[self.pos] as u32;
            self.pos += 1;
            self.bit_count = 8;
        }
        let bit = (self.bit_buf & 1) as i32;
        self.bit_buf >>= 1;
        self.bit_count -= 1;
        bit
    }

    // Mirror of getbits(): assembles n LSB-first bits, -1 if any bit exhausts.
    #[inline]
    fn getbits(&mut self, n: i32) -> i32 {
        let mut value: i32 = 0;
        let mut i = 0;
        while i < n {
            let bit = self.getbit();
            if bit < 0 {
                return -1;
            }
            value |= bit << i;
            i += 1;
        }
        value
    }

    // Mirror of decode_symbol(): canonical-Huffman decode. The returned index
    // `index + (code - first)` is provably in [0, total) <= [0, 288) because the
    // loop only returns when code - count < first (so code - first < count, i.e.
    // index + (code-first) < index + count <= total) and the invariant code >=
    // first holds inductively (we only advance when code >= first + count). Rust
    // bounds-checks the slice access regardless (defense in depth: a table logic
    // slip HALTS loudly, never reads OOB).
    fn decode_symbol(&mut self, h: &InfHuffman) -> i32 {
        let mut code: i32 = 0;
        let mut first: i32 = 0;
        let mut index: i32 = 0;
        let mut len = 1usize;
        while len < 16 {
            let bit = self.getbit();
            if bit < 0 {
                return -1;
            }
            code = (code << 1) | bit;
            let count = h.counts[len] as i32;
            if code - count < first {
                return h.symbols[(index + (code - first)) as usize] as i32;
            }
            index += count;
            first = (first + count) << 1;
            len += 1;
        }
        -1 // invalid / over-long code
    }
}

// Mirror of huffman_t (counts[16], symbols[288]).
struct InfHuffman {
    counts: [u16; 16],
    symbols: [u16; 288],
}

impl InfHuffman {
    fn new() -> Self {
        InfHuffman { counts: [0u16; 16], symbols: [0u16; 288] }
    }

    // Mirror of build_huffman(). `lengths` has at least `n` entries. Only lengths
    // in 1..=15 are counted/placed; offsets[l] advances at most `total` (<= n <=
    // 288) times so every symbols[] write is in-bounds by construction.
    fn build(&mut self, lengths: &[i32], n: usize) {
        self.counts = [0u16; 16];
        let mut i = 0;
        while i < n {
            let l = lengths[i];
            if l > 0 && l < 16 {
                self.counts[l as usize] += 1;
            }
            i += 1;
        }
        let mut offsets = [0u16; 16];
        let mut k = 1;
        while k < 16 {
            offsets[k] = offsets[k - 1] + self.counts[k - 1];
            k += 1;
        }
        i = 0;
        while i < n {
            let l = lengths[i];
            if l > 0 && l < 16 {
                let li = l as usize;
                self.symbols[offsets[li] as usize] = i as u16;
                offsets[li] += 1;
            }
            i += 1;
        }
    }
}

/// Faithful Rust port of gui/png.c inflate() (RFC 1951 DEFLATE). Decompresses a
/// RAW DEFLATE stream (the caller strips the 2-byte zlib header) from `src`
/// (len `src_len`) into `dst` (cap `dst_cap`), writing the produced length to
/// `*dst_len`. Returns PNG_SUCCESS (0) or PNG_ERR_INFLATE (-8), byte-for-byte
/// identical accept/reject + output to the C inflate_c on every stream.
/// # Safety: `src` must point to >= `src_len` readable bytes and `dst` to >=
/// `dst_cap` writable bytes (the exact contract of the C inflate). `dst_len` may
/// be null (then the produced length is not written).
#[no_mangle]
pub extern "C" fn inflate_rs(
    src: *const u8,
    src_len: u32,
    dst: *mut u8,
    dst_cap: u32,
    dst_len: *mut u32,
) -> i32 {
    if dst.is_null() {
        return PNG_ERR_INFLATE_RS;
    }
    // SAFETY: the caller (image_load_png / the boot self-test) guarantees `src`
    // points to at least `src_len` readable bytes and `dst` to at least `dst_cap`
    // writable bytes (identical contract to the C inflate). We span EXACTLY those
    // lengths; every read/write below is a bounds-checked slice access. `src`
    // null with len 0 is treated as an empty stream.
    let data: &[u8] = if src.is_null() || src_len == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(src, src_len as usize) }
    };
    let out: &mut [u8] = unsafe { core::slice::from_raw_parts_mut(dst, dst_cap as usize) };

    let mut s = InfState { data, pos: 0, bit_buf: 0, bit_count: 0 };
    let mut out_pos: u32 = 0;

    loop {
        let bfinal = s.getbit();
        if bfinal < 0 {
            return PNG_ERR_INFLATE_RS;
        }
        let btype = s.getbits(2);
        if btype < 0 {
            return PNG_ERR_INFLATE_RS;
        }

        if btype == 0 {
            // Stored (uncompressed) block. Align to byte, read LEN/NLEN, copy.
            s.bit_count = 0;
            if s.pos + 4 > s.data.len() {
                return PNG_ERR_INFLATE_RS;
            }
            let len: u32 = (s.data[s.pos] as u32) | ((s.data[s.pos + 1] as u32) << 8);
            s.pos += 4; // skip LEN + NLEN
            if s.pos + len as usize > s.data.len() {
                return PNG_ERR_INFLATE_RS; // input bound
            }
            if out_pos + len > dst_cap {
                return PNG_ERR_INFLATE_RS; // output bound
            }
            let sp = s.pos;
            let dp = out_pos as usize;
            out[dp..dp + len as usize].copy_from_slice(&s.data[sp..sp + len as usize]);
            s.pos += len as usize;
            out_pos += len;
        } else if btype == 1 || btype == 2 {
            let mut lit_huff = InfHuffman::new();
            let mut dist_huff = InfHuffman::new();
            let mut lit_lengths = [0i32; 288];
            let mut dist_lengths = [0i32; 32];

            if btype == 1 {
                // Fixed Huffman code lengths (RFC 1951 3.2.6).
                let mut i = 0usize;
                while i <= 143 { lit_lengths[i] = 8; i += 1; }
                while i <= 255 { lit_lengths[i] = 9; i += 1; }
                while i <= 279 { lit_lengths[i] = 7; i += 1; }
                while i <= 287 { lit_lengths[i] = 8; i += 1; }
                let mut j = 0usize;
                while j < 32 { dist_lengths[j] = 5; j += 1; }
            } else {
                // Dynamic Huffman: read HLIT/HDIST/HCLEN, build the code-length
                // code, then RLE-decode the lit+dist length arrays.
                let hlit = s.getbits(5) + 257;
                let hdist = s.getbits(5) + 1;
                let hclen = s.getbits(4) + 4;
                if hlit < 0 || hdist < 0 || hclen < 0 {
                    return PNG_ERR_INFLATE_RS;
                }
                let mut code_lengths = [0i32; 19];
                let mut i = 0i32;
                while i < hclen {
                    // hclen <= 19 so INF_CODE_ORDER[i] is in-bounds; the target
                    // index code_order[i] is <= 18 (code_lengths[19]).
                    code_lengths[INF_CODE_ORDER[i as usize]] = s.getbits(3);
                    i += 1;
                }
                let mut code_huff = InfHuffman::new();
                code_huff.build(&code_lengths, 19);

                let mut combined = [0i32; 288 + 32];
                let mut n: i32 = 0;
                let total = hlit + hdist; // <= 320
                while n < total {
                    let sym = s.decode_symbol(&code_huff);
                    if sym < 0 {
                        return PNG_ERR_INFLATE_RS;
                    }
                    if sym < 16 {
                        combined[n as usize] = sym; // n < total <= 320, in-bounds
                        n += 1;
                    } else if sym == 16 {
                        let mut count = s.getbits(2) + 3;
                        if n == 0 {
                            return PNG_ERR_INFLATE_RS; // no previous length
                        }
                        let val = combined[(n - 1) as usize];
                        while count > 0 && n < total {
                            combined[n as usize] = val;
                            n += 1;
                            count -= 1;
                        }
                    } else if sym == 17 {
                        let mut count = s.getbits(3) + 3;
                        while count > 0 && n < total {
                            combined[n as usize] = 0;
                            n += 1;
                            count -= 1;
                        }
                    } else if sym == 18 {
                        let mut count = s.getbits(7) + 11;
                        while count > 0 && n < total {
                            combined[n as usize] = 0;
                            n += 1;
                            count -= 1;
                        }
                    }
                }
                // Split combined[] into lit (hlit) and dist (hdist) length arrays.
                let hl = hlit as usize;  // in [256, 288]
                let hd = hdist as usize; // in [0, 32]
                let mut i = 0usize;
                while i < hl { lit_lengths[i] = combined[i]; i += 1; }
                while i < 288 { lit_lengths[i] = 0; i += 1; }
                let mut j = 0usize;
                while j < hd { dist_lengths[j] = combined[hl + j]; j += 1; }
                while j < 32 { dist_lengths[j] = 0; j += 1; }
            }

            lit_huff.build(&lit_lengths, 288);
            dist_huff.build(&dist_lengths, 32);

            // Decode literals + length/distance back-references until end-of-block.
            loop {
                let sym = s.decode_symbol(&lit_huff);
                if sym < 0 {
                    return PNG_ERR_INFLATE_RS;
                }
                if sym < 256 {
                    // Literal byte.
                    if out_pos >= dst_cap {
                        return PNG_ERR_INFLATE_RS;
                    }
                    out[out_pos as usize] = sym as u8;
                    out_pos += 1;
                } else if sym == 256 {
                    break; // end of block
                } else {
                    // Length/distance pair.
                    let ls = sym - 257;
                    if ls >= 29 {
                        return PNG_ERR_INFLATE_RS;
                    }
                    let lsi = ls as usize;
                    let mut length = INF_LENGTH_BASE[lsi];
                    if INF_LENGTH_EXTRA[lsi] > 0 {
                        let extra = s.getbits(INF_LENGTH_EXTRA[lsi]);
                        if extra < 0 {
                            return PNG_ERR_INFLATE_RS;
                        }
                        length += extra;
                    }
                    let dsym = s.decode_symbol(&dist_huff);
                    if dsym < 0 || dsym >= 30 {
                        return PNG_ERR_INFLATE_RS;
                    }
                    let dsi = dsym as usize;
                    let mut dist = INF_DIST_BASE[dsi];
                    if INF_DIST_EXTRA[dsi] > 0 {
                        let extra = s.getbits(INF_DIST_EXTRA[dsi]);
                        if extra < 0 {
                            return PNG_ERR_INFLATE_RS;
                        }
                        dist += extra;
                    }
                    // THE LZ77 back-reference bounds (densest OOB surface):
                    //   dist > out_pos      => would read before the output start
                    //   out_pos+length>cap  => would write past the output buffer
                    // Copy byte-wise so overlapping (dist < length) runs replicate
                    // exactly as DEFLATE requires (and as the C for-loop does).
                    if dist as u32 > out_pos {
                        return PNG_ERR_INFLATE_RS;
                    }
                    if out_pos + length as u32 > dst_cap {
                        return PNG_ERR_INFLATE_RS;
                    }
                    let mut i = 0i32;
                    while i < length {
                        let from = (out_pos - dist as u32) as usize;
                        out[out_pos as usize] = out[from];
                        out_pos += 1;
                        i += 1;
                    }
                }
            }
        } else {
            return PNG_ERR_INFLATE_RS; // invalid block type (3)
        }

        if bfinal != 0 {
            break;
        }
    }

    if !dst_len.is_null() {
        // SAFETY: dst_len non-null (checked); a caller-owned writable u32.
        unsafe { *dst_len = out_pos; }
    }
    PNG_SUCCESS_RS
}
