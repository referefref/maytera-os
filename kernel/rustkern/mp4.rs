// rustkern/mp4.rs - #404 Phase Z / #505 ISO-BMFF / MP4 (M4A) sample-table parse
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #404 / #505 Phase Z: media/aac.c ISO-BMFF / MP4 (M4A) sample-table parse
// (Tier 2, untrusted FILE input). The music player plays an audio file BY PATH
// via the Ring-3 SYS_PLAY_WAV syscall -> sys_play_wav -> audio_play_file_async
// -> audio_play_file (fat_read_file: attacker-controlled bytes from disk) ->
// audio_decode_open -> aac_create -> mp4_parse. So a crafted .m4a on disk (or
// downloaded) reaches this hand-written atom/box parser.
//
// SECURITY (REACHABLE, ASan-proven - the class of ext2/fat/elf/jpeg/png): the C
// mp4_parse reads the ISO-BMFF sample-table COUNTS (nsamp from stsz, nchunks
// from stco/co64, nstsc from stsc) as trusted lengths and then walks stsz_tab /
// stco_tab / stsc_tab by those counts with NO per-entry bound against the atom
// body or the buffer. `nstsc` is read with NO clamp at all; `nsamp`/`nchunks`
// are clamped only to <= 10,000,000. A crafted stsc/stco/stsz whose declared
// count far exceeds its actual table body drives be32(d + tab + i*stride) far
// past the kmalloc'd file buffer -> heap OOB READ (CWE-125). Additionally a runt
// stsz/stco/stsc atom at end-of-buffer over-reads its own count field
// (be32(stsz+4), be32(stsz+8), be32(stco+4), be32(stsc+4)). mp4_parse_rs slices
// EXACTLY buf_size bytes and every table/field read is a bounds-checked index:
// an over-long count simply terminates the walk (confine) instead of reading
// past the buffer. media/aac.c routes the live mp4_parse here under -DRUST_MP4;
// the atom-descent structure + emitted frame table are byte-identical to the C
// on every well-formed .m4a (differential + offline pre-flight).
//
// Mp4Frame mirrors aac_frame_t {u32 offset; u32 size;}; Mp4ParseResult is the
// #[repr(C)] out summary (sizeof asserted == 16 in aac.c). PURE: allocates
// nothing, mutates nothing, retains nothing; frames are written into a
// caller-provided, capacity-bounded array (NULL = count-only probe pass).

#[repr(C)]
pub struct Mp4Frame {
    pub offset: u32,
    pub size: u32,
}

#[repr(C)]
pub struct Mp4ParseResult {
    pub nframes: u32,
    pub asc_off: u32,
    pub asc_len: u32,
    pub _pad: u32,
}

// Bounds-checked big-endian reads over the exact-size input slice. None = the
// field would leave the slice, which on the checked path can only cause a
// reject / walk-termination, never an out-of-bounds read.
#[inline]
fn mp4_rd_be32(s: &[u8], off: usize) -> Option<u32> {
    if off.checked_add(4)? > s.len() {
        return None;
    }
    Some(((s[off] as u32) << 24)
        | ((s[off + 1] as u32) << 16)
        | ((s[off + 2] as u32) << 8)
        | (s[off + 3] as u32))
}
#[inline]
fn mp4_rd_be64(s: &[u8], off: usize) -> Option<u64> {
    let hi = mp4_rd_be32(s, off)? as u64;
    let lo = mp4_rd_be32(s, off + 4)? as u64;
    Some((hi << 32) | lo)
}

// Faithful port of aac.c mp4_find(): scan [start,end) for the first box whose
// FourCC == `ty`, returning (body_offset, body_len). `end` is always <= s.len().
fn mp4_find_rs(s: &[u8], start: usize, end: usize, ty: &[u8; 4]) -> Option<(usize, usize)> {
    let end = if end > s.len() { s.len() } else { end };
    let mut i = start;
    while i + 8 <= end {
        // i+8 <= end <= len, so this read is in-bounds.
        let sz32 = mp4_rd_be32(s, i)?;
        let mut sz: u64 = sz32 as u64;
        let mut hdr: usize = 8;
        if sz == 1 {
            // 64-bit extended size at i+8.
            if i + 16 > end {
                break;
            }
            sz = mp4_rd_be64(s, i + 8)?;
            hdr = 16;
        } else if sz == 0 {
            sz = (end - i) as u64; // extends to end of container
        }
        if sz < hdr as u64 || (i as u64) + sz > end as u64 {
            break;
        }
        if &s[i + 4..i + 8] == ty {
            return Some((i + hdr, (sz as usize) - hdr));
        }
        // sz >= hdr >= 8, so i strictly increases and stays <= end: terminates.
        i += sz as usize;
    }
    None
}

// Faithful port of aac.c desc_len(): MPEG-4 expandable descriptor length (each
// byte low 7 bits, high bit = continue), at most 4 bytes. Advances *p.
fn mp4_desc_len_rs(s: &[u8], p: &mut usize, end: usize) -> u32 {
    let mut len: u32 = 0;
    let mut n = 0;
    while *p < end && n < 4 {
        let b = s[*p]; // *p < end <= len: in-bounds.
        *p += 1;
        len = (len << 7) | ((b & 0x7F) as u32);
        n += 1;
        if b & 0x80 == 0 {
            break;
        }
    }
    len
}

// Faithful port of aac.c mp4_extract_asc(): pull the AudioSpecificConfig out of
// an esds payload [start,end). Returns (asc_offset_in_buf, asc_len). Every byte
// read is guarded by `p < end` (end <= len), so no read leaves the buffer.
fn mp4_extract_asc_rs(s: &[u8], start: usize, end: usize) -> Option<(usize, usize)> {
    let end = if end > s.len() { s.len() } else { end };
    let mut p = start.checked_add(4)?; // skip version/flags
    if p >= end || s[p] != 0x03 {
        return None;
    }
    p += 1;
    mp4_desc_len_rs(s, &mut p, end);
    p = p.checked_add(3)?; // ES_ID(2) + flags(1)
    if p >= end || s[p] != 0x04 {
        return None;
    }
    p += 1;
    mp4_desc_len_rs(s, &mut p, end);
    p = p.checked_add(1 + 1 + 3 + 4 + 4)?; // objType,streamType,buf,maxBR,avgBR
    if p >= end || s[p] != 0x05 {
        return None;
    }
    p += 1;
    let l = mp4_desc_len_rs(s, &mut p, end) as usize;
    if l == 0 || p.checked_add(l)? > end {
        return None;
    }
    Some((p, l))
}

/// Rust port of media/aac.c mp4_parse(): descend moov/trak/mdia/minf/stbl to the
/// first AAC track, extract the AudioSpecificConfig, then walk stsz + stco/co64
/// + stsc to build the per-sample (offset,size) frame table. Two-phase to match
/// the C's kmalloc(nsamp) then fill: pass frames==NULL for a count-only probe
/// (out.nframes = frames it WOULD produce), then a fill pass with a capacity ==
/// that count. Returns 1 on success (out filled), 0 on failure - identical
/// accept/reject + identical frame table to mp4_parse_c on every well-formed
/// input. Confines the reachable sample-table over-reads by construction.
/// # Safety: `buf` must point to >= `buf_size` readable bytes (the file buffer
/// aac_create kmalloc'd). `frames`, when non-null, must be a writable array of
/// at least `frames_cap` Mp4Frame. `out`, when non-null, is a writable
/// Mp4ParseResult. All are the C ABI contract from aac.c. No pointer is retained.
#[no_mangle]
pub extern "C" fn mp4_parse_rs(
    buf: *const u8,
    buf_size: usize,
    frames: *mut Mp4Frame,
    frames_cap: u32,
    out: *mut Mp4ParseResult,
) -> i32 {
    if buf.is_null() {
        return 0;
    }
    // SAFETY: caller guarantees `buf` spans >= `buf_size` readable bytes; the
    // slice covers EXACTLY those bytes so every index below is bounds-checked.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(buf, buf_size) };
    // SAFETY: `frames` non-null => a writable array of >= `frames_cap` elems.
    let mut fr: Option<&mut [Mp4Frame]> = if frames.is_null() {
        None
    } else {
        Some(unsafe { core::slice::from_raw_parts_mut(frames, frames_cap as usize) })
    };

    let n = buf_size;

    let (moov, moov_len) = match mp4_find_rs(s, 0, n, b"moov") {
        Some(v) => v,
        None => return 0,
    };
    let search = moov;
    let search_end = moov + moov_len;

    let mut stbl_found: Option<(usize, usize)> = None;
    let mut asc_found: Option<(usize, usize)> = None;
    let mut tpos = search;
    while let Some((trak, trak_len)) = mp4_find_rs(s, tpos, search_end, b"trak") {
        if let Some((mdia, mdia_len)) = mp4_find_rs(s, trak, trak + trak_len, b"mdia") {
            if let Some((minf, minf_len)) = mp4_find_rs(s, mdia, mdia + mdia_len, b"minf") {
                if let Some((sb, sb_len)) = mp4_find_rs(s, minf, minf + minf_len, b"stbl") {
                    if let Some((stsd, stsd_len)) = mp4_find_rs(s, sb, sb + sb_len, b"stsd") {
                        // esds either inside mp4a (stsd+8..), or directly in stsd.
                        //
                        // Mirrors the C's if/else EXACTLY (media/aac.c:196-213):
                        // when mp4a IS present the esds search is CONFINED to it
                        // and there is NO fallback to scanning stsd; the C only
                        // scans stsd when mp4a is ABSENT.
                        //
                        // #404 drift audit 2 (2026-07-16): this was a
                        // `.and_then(..).or_else(..)` chain. `.and_then` yields
                        // None both when mp4a is missing AND when mp4a is present
                        // but holds no esds, so `.or_else` fired in BOTH cases and
                        // the seam accepted files the C rejects (and could select a
                        // different trak on multi-trak files). 22,571/600,000 (3.8%)
                        // of vectors, every one in the clean domain where the C
                        // provably reads nothing out of bounds, so not one could be
                        // excused as a memory-safety confinement. It was an
                        // EXPANSION shipped under a confinement label. The port's
                        // contract is identical behavior; this restores it.
                        let ce = match mp4_find_rs(s, stsd + 8, stsd + stsd_len, b"mp4a") {
                            Some((esds, esds_len)) => {
                                mp4_find_rs(s, esds + 28, esds + esds_len, b"esds")
                            }
                            None => mp4_find_rs(s, stsd + 8, stsd + stsd_len, b"esds"),
                        };
                        if let Some((ce_off, ce_len)) = ce {
                            if let Some((a_off, a_len)) =
                                mp4_extract_asc_rs(s, ce_off, ce_off + ce_len)
                            {
                                stbl_found = Some((sb, sb_len));
                                asc_found = Some((a_off, a_len));
                                break;
                            }
                        }
                    }
                }
            }
        }
        tpos = trak + trak_len;
    }

    let (stbl, stbl_len) = match stbl_found {
        Some(v) => v,
        None => return 0,
    };
    let (asc_off, asc_len) = match asc_found {
        Some(v) => v,
        None => return 0,
    };

    // --- sample sizes (stsz) --- (be32(stsz+4)/be32(stsz+8) bounds-checked)
    let (stsz, _stsz_len) = match mp4_find_rs(s, stbl, stbl + stbl_len, b"stsz") {
        Some(v) => v,
        None => return 0,
    };
    let uniform = match mp4_rd_be32(s, stsz + 4) {
        Some(v) => v,
        None => return 0,
    };
    let nsamp = match mp4_rd_be32(s, stsz + 8) {
        Some(v) => v,
        None => return 0,
    };
    if nsamp == 0 || nsamp > 10_000_000 {
        return 0;
    }

    // --- chunk offsets (stco / co64) ---
    let (stco, _stco_len, is64) = match mp4_find_rs(s, stbl, stbl + stbl_len, b"stco") {
        Some((o, l)) => (o, l, false),
        None => match mp4_find_rs(s, stbl, stbl + stbl_len, b"co64") {
            Some((o, l)) => (o, l, true),
            None => return 0,
        },
    };
    let nchunks = match mp4_rd_be32(s, stco + 4) {
        Some(v) => v,
        None => return 0,
    };
    if nchunks == 0 || nchunks > 10_000_000 {
        return 0;
    }

    // --- sample-to-chunk (stsc) --- (nstsc has NO clamp, exactly like the C;
    // but every table read below is bounds-checked, so an inflated nstsc just
    // terminates the run-length scan instead of over-reading.)
    let (stsc, _stsc_len) = match mp4_find_rs(s, stbl, stbl + stbl_len, b"stsc") {
        Some(v) => v,
        None => return 0,
    };
    let nstsc = match mp4_rd_be32(s, stsc + 4) {
        Some(v) => v,
        None => return 0,
    };

    let stsz_tab = stsz + 12;
    let stco_tab = stco + 8;
    let stsc_tab = stsc + 8;

    let mut nframes: u32 = 0;
    let mut sidx: u32 = 0; // global sample index
    let mut chunk: u32 = 0;
    while chunk < nchunks && sidx < nsamp {
        // samples-per-chunk for this chunk (1-based in stsc).
        let mut spc: u32 = 1;
        let mut e: u32 = 0;
        while e < nstsc {
            let first = match mp4_rd_be32(s, stsc_tab + (e as usize) * 12) {
                Some(v) => v,
                None => break,
            };
            let this_spc = match mp4_rd_be32(s, stsc_tab + (e as usize) * 12 + 4) {
                Some(v) => v,
                None => break,
            };
            let next_first = if e + 1 < nstsc {
                match mp4_rd_be32(s, stsc_tab + ((e as usize) + 1) * 12) {
                    Some(v) => v,
                    None => 0xFFFF_FFFF,
                }
            } else {
                0xFFFF_FFFF
            };
            if chunk + 1 >= first && chunk + 1 < next_first {
                spc = this_spc;
                break;
            }
            e += 1;
        }
        let mut off: u64 = if is64 {
            match mp4_rd_be64(s, stco_tab + (chunk as usize) * 8) {
                Some(v) => v,
                None => break,
            }
        } else {
            match mp4_rd_be32(s, stco_tab + (chunk as usize) * 4) {
                Some(v) => v as u64,
                None => break,
            }
        };
        let mut k: u32 = 0;
        while k < spc && sidx < nsamp {
            let sz0 = if uniform != 0 {
                uniform
            } else {
                match mp4_rd_be32(s, stsz_tab + (sidx as usize) * 4) {
                    Some(v) => v,
                    None => break,
                }
            };
            let sz = if off + (sz0 as u64) > buf_size as u64 { 0 } else { sz0 };
            if let Some(ref mut arr) = fr {
                let idx = nframes as usize;
                if idx < arr.len() {
                    arr[idx].offset = off as u32;
                    arr[idx].size = sz;
                }
            }
            nframes += 1;
            off += sz as u64;
            k += 1;
            sidx += 1;
        }
        chunk += 1;
    }

    if nframes == 0 {
        return 0;
    }
    if !out.is_null() {
        // SAFETY: out non-null => writable Mp4ParseResult (POD writes only).
        unsafe {
            let o = &mut *out;
            o.nframes = nframes;
            o.asc_off = asc_off as u32;
            o.asc_len = asc_len as u32;
            o._pad = 0;
        }
    }
    1
}
