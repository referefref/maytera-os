// rustkern/iso9660.rs - #196 / #404: ISO 9660 (+ Joliet) parsing in Rust.
//
// WHY RUST, AND WHY HERE
// ----------------------
// This is the layer that interprets bytes off a CD image the user downloaded
// from the internet. Every field below (record length, extent length, name
// length, the name bytes themselves) is attacker-controlled. This tree has
// shipped three heap over-reads in exactly this shape of C parser: #476
// (ext2_lookup trusted an on-disk rec_len/name_len), #490 (FAT LFN), #597.
// Every read here goes through a Rust slice that spans exactly the caller's
// buffer, so an over-long rec_len or name_len is a rejected record, not a read
// past the end of the heap block.
//
// SCOPE, MEASURED AGAINST THE REAL TARGET IMAGES
// ----------------------------------------------
// The two Command & Conquer Red Alert discs this was built for carry:
//   sector 16: Primary Volume Descriptor   (type 1, "CD001")
//   sector 17: a SECOND type-1 PVD          (a duplicate; harmless, ignored)
//   sector 18: Supplementary Volume Descriptor, escape "%/@" = Joliet UCS-2 L1
//   sector 19: terminator (type 255)
// and NO Rock Ridge (no SUSP "SP" entry in the root record's system-use area,
// checked on both discs). The primary tree carries plain 8.3 names, which is
// what a DOS guest needs, so Joliet is a display nicety, not a requirement, and
// Rock Ridge is not implemented because nothing on the target needs it.
//
// Multi-extent files (the 0x80 "not final" flag) are NOT supported. The largest
// file on either disc is MAIN.MIX at 454,605,294 bytes, well under the ~4 GiB
// single-extent limit, and iso_dirrec_at_rs reports the flag so the C caller can
// refuse rather than silently return a truncated file.
//
// The C references (iso_vd_parse_c / iso_dirrec_at_c / iso_name_decode_c in
// dos/diskimg.c) are kept verbatim for rollback and for the boot differential;
// -DRUST_ISO9660 selects which one the live symbols route to.

/// Result of parsing one volume descriptor sector. Mirrors iso_vol_t in
/// dos/diskimg.h; sizeof-locked there with _Static_assert.
#[repr(C)]
pub struct IsoVol {
    pub root_lba: u32,       // LBA of the root directory extent
    pub root_len: u32,       // length of the root directory extent, bytes
    pub block_size: u32,     // logical block size from the descriptor
    pub kind: u32,           // 1 = primary (type 1), 2 = Joliet SVD (type 2)
    pub joliet_ucs: u32,     // Joliet level 1/2/3 (0 when kind != 2)
    pub _pad: u32,
    /// Volume identifier, the RAW 32 bytes at descriptor offset 40, exactly as
    /// they sit on the disc: space-padded, NOT NUL-terminated, and NOT trimmed.
    ///
    /// Kept raw on purpose. On a PRIMARY descriptor these are d-characters
    /// (ASCII); on a Joliet SUPPLEMENTARY descriptor the same field is UCS-2
    /// BIG-ENDIAN, so the same 32 bytes mean different things depending on
    /// `kind` and only the caller knows which it asked for. Decoding here would
    /// have to guess. dos/diskimg.c decodes the PRIMARY one into the 11-char
    /// label DOS reports; nothing decodes the Joliet one today.
    ///
    /// This is what makes a disc IDENTIFIABLE to a guest. Without it the whole
    /// descriptor was parsed and the name on the disc was dropped on the floor,
    /// so INT 21h 4Eh with a volume-label mask had nothing to answer with.
    pub volid: [u8; 32],
}

/// One directory record. Mirrors iso_dirrec_t in dos/diskimg.h.
/// `name_off` is a byte offset INTO THE CALLER'S BUFFER, so the caller can copy
/// the raw name without this side allocating.
#[repr(C)]
pub struct IsoDirRec {
    pub next: u32,       // buffer offset of the following record
    pub lba: u32,        // extent LBA
    pub len: u32,        // extent length in bytes
    pub is_dir: u32,     // 1 if the directory bit (0x02) is set
    pub multi: u32,      // 1 if the "not final extent" bit (0x80) is set
    pub name_off: u32,   // offset of the raw name bytes within the buffer
    pub name_len: u32,   // raw on-disk name length
    pub _pad: u32,
}

// THE FFI SIZE LOCK, BOTH ENDS. dos/diskimg.c carries a _Static_assert that the
// C mirror is 56 bytes; this is the other half. Without it, a field added on
// this side only would leave the C caller allocating a 24-byte IsoVol that this
// code writes 56 bytes into, which is a stack smash a differential test cannot
// see because both arms would be handed the same undersized buffer.
const _: () = assert!(core::mem::size_of::<IsoVol>() == 56);
const _: () = assert!(core::mem::size_of::<IsoDirRec>() == 32);

const ISO_SECT: u32 = 2048;

fn rd32le(s: &[u8], at: usize) -> u32 {
    // Callers below always bounds-check `at + 4 <= s.len()` first; this helper
    // indexes the slice, so a mistake panics loudly rather than reading past it.
    (s[at] as u32) | ((s[at + 1] as u32) << 8) | ((s[at + 2] as u32) << 16) | ((s[at + 3] as u32) << 24)
}

fn rd16le(s: &[u8], at: usize) -> u32 {
    (s[at] as u32) | ((s[at + 1] as u32) << 8)
}

/// Parse one 2048-byte volume-descriptor sector.
///
/// Returns 1 and fills `*out` for a Primary (type 1) or a Joliet Supplementary
/// (type 2 with a recognised escape sequence) descriptor; 0 for any other valid
/// descriptor the caller should skip (terminator, boot record, non-Joliet SVD);
/// -1 if the sector is not a volume descriptor at all (bad "CD001" or short).
///
/// # Safety
/// `sec` must point to at least `len` readable bytes and `out` must be a
/// caller-owned, aligned IsoVol. Nothing is written to `*out` on a non-1 return.
#[no_mangle]
pub unsafe extern "C" fn iso_vd_parse_rs(sec: *const u8, len: u32, out: *mut IsoVol) -> i32 {
    if sec.is_null() || out.is_null() {
        return -1;
    }
    // SAFETY: the caller's contract is `len` contiguous readable bytes at `sec`
    // (live callers pass a 2048-byte buffer filled by imgfile_read). The slice
    // spans exactly that extent and every index below goes through it, so no
    // field of the disc-controlled descriptor can steer a read outside it.
    let s: &[u8] = core::slice::from_raw_parts(sec, len as usize);

    // A descriptor needs the 190-byte header plus the 34-byte root record that
    // starts at offset 156. Anything shorter is not parseable.
    if s.len() < 190 {
        return -1;
    }
    if !(s[1] == b'C' && s[2] == b'D' && s[3] == b'0' && s[4] == b'0' && s[5] == b'1') {
        return -1;
    }

    let ty = s[0];
    let mut joliet_ucs = 0u32;
    match ty {
        1 => {}
        2 => {
            // Joliet is signalled by an escape sequence at offset 88:
            //   %/@ = UCS-2 level 1, %/C = level 2, %/E = level 3.
            // Any other SVD escape is some other supplementary encoding we do
            // not decode, so report "skip" rather than mis-decoding names.
            if s[88] == b'%' && s[89] == b'/' {
                joliet_ucs = match s[90] {
                    b'@' => 1,
                    b'C' => 2,
                    b'E' => 3,
                    _ => 0,
                };
            }
            if joliet_ucs == 0 {
                return 0;
            }
        }
        _ => return 0, // boot record (0), partition (3), terminator (255), ...
    }

    // Logical block size at offset 128 (16-bit both-endian; take the LE half).
    let bs = rd16le(s, 128);
    // ISO 9660 permits 512/1024/2048; everything real uses 2048 and the rest of
    // this reader assumes 2048-byte addressing. Reject the others loudly rather
    // than compute wrong offsets.
    if bs != ISO_SECT {
        return 0;
    }

    // Root directory record: 34 bytes at offset 156. Extent LBA at +2, length
    // at +10, both 32-bit both-endian (LE half first).
    let root_lba = rd32le(s, 156 + 2);
    let root_len = rd32le(s, 156 + 10);
    if root_len == 0 {
        return 0;
    }

    let o = &mut *out;
    o.root_lba = root_lba;
    o.root_len = root_len;
    o.block_size = bs;
    o.kind = if ty == 1 { 1 } else { 2 };
    o.joliet_ucs = joliet_ucs;
    o._pad = 0;
    // Volume identifier: 32 bytes at offset 40. The `s.len() < 190` guard above
    // already proves 72 <= len, so this slice cannot run off the end.
    o.volid.copy_from_slice(&s[40..72]);
    1
}

/// Step one directory record inside a directory extent buffer.
///
/// `buf`/`buflen` is the directory extent (or the part of it currently held),
/// `pos` is the byte offset to read at. Returns:
///   1  a record was parsed; `out` is filled and `out.next` is where to continue
///   0  end of directory (no more records in this buffer)
///  -1  the buffer/arguments are unusable
///
/// A zero record length means "no more records in this logical sector"; the
/// record stream resumes at the next 2048-byte boundary, and that advance is
/// reported through `out.next` with a return of 1 and `name_len == 0`, so the
/// caller never has to reimplement the padding rule. A record that claims to
/// extend past the buffer, or a name that claims to extend past the record, is
/// treated as end-of-directory rather than trusted.
///
/// # Safety
/// `buf` must point to at least `buflen` readable bytes; `out` must be a
/// caller-owned aligned IsoDirRec.
#[no_mangle]
pub unsafe extern "C" fn iso_dirrec_at_rs(
    buf: *const u8,
    buflen: u32,
    pos: u32,
    out: *mut IsoDirRec,
) -> i32 {
    if buf.is_null() || out.is_null() {
        return -1;
    }
    // SAFETY: as above, the slice spans exactly the caller's `buflen` bytes and
    // is the only way any byte below is reached.
    let s: &[u8] = core::slice::from_raw_parts(buf, buflen as usize);
    let o = &mut *out;
    o.next = 0;
    o.lba = 0;
    o.len = 0;
    o.is_dir = 0;
    o.multi = 0;
    o.name_off = 0;
    o.name_len = 0;
    o._pad = 0;

    let p = pos as usize;
    if p >= s.len() {
        return 0;
    }

    let reclen = s[p] as usize;
    if reclen == 0 {
        // Directory records never straddle a logical sector; the tail of the
        // sector is zero padding. Jump to the next sector boundary.
        let next = ((p / ISO_SECT as usize) + 1) * ISO_SECT as usize;
        if next <= p || next >= s.len() {
            return 0;
        }
        o.next = next as u32;
        return 1; // name_len == 0 signals "padding skipped, nothing to report"
    }

    // A record is 33 fixed bytes plus the name. Both the record and its name
    // must fit inside the buffer, or the record is not trustworthy.
    if reclen < 33 || p + reclen > s.len() {
        return 0;
    }
    let namelen = s[p + 32] as usize;
    if namelen == 0 || 33 + namelen > reclen {
        return 0;
    }

    o.next = (p + reclen) as u32;
    o.lba = rd32le(s, p + 2);
    o.len = rd32le(s, p + 10);
    let flags = s[p + 25];
    o.is_dir = if (flags & 0x02) != 0 { 1 } else { 0 };
    o.multi = if (flags & 0x80) != 0 { 1 } else { 0 };
    o.name_off = (p + 33) as u32;
    o.name_len = namelen as u32;
    1
}

/// Decode an on-disc directory-record name into a printable NUL-terminated
/// ASCII name in `out`.
///
/// `joliet` != 0 decodes UCS-2 big-endian (two bytes per character), which is
/// how Joliet stores names; a code point outside ASCII becomes '_' because the
/// rest of the OS uses 8-bit names throughout. `joliet` == 0 copies the bytes.
///
/// In both cases the ISO 9660 version suffix ";1" is stripped, and a trailing
/// '.' left by a file with an empty extension is stripped, matching what every
/// other ISO reader shows. Returns the decoded length (excluding the NUL), 0 if
/// nothing decodable, or -1 on bad arguments. Never writes `outcap` or more.
///
/// The "self" (0x00) and "parent" (0x01) records have a single-byte name that
/// is not text; they decode to length 0 so the caller can drop them with one
/// test instead of duplicating the rule.
///
/// # Safety
/// `src` must have `srclen` readable bytes, `out` must have `outcap` writable
/// bytes.
#[no_mangle]
pub unsafe extern "C" fn iso_name_decode_rs(
    src: *const u8,
    srclen: u32,
    joliet: i32,
    out: *mut u8,
    outcap: u32,
) -> i32 {
    if src.is_null() || out.is_null() || outcap == 0 {
        return -1;
    }
    // SAFETY: both slices span exactly the caller-declared extents; every read
    // and every write below is indexed through them, so a hostile srclen cannot
    // walk off either buffer.
    let s: &[u8] = core::slice::from_raw_parts(src, srclen as usize);
    let d: &mut [u8] = core::slice::from_raw_parts_mut(out, outcap as usize);

    d[0] = 0;
    if s.is_empty() {
        return 0;
    }
    // Special "." (0x00) and ".." (0x01) entries.
    if s.len() == 1 && (s[0] == 0 || s[0] == 1) {
        return 0;
    }

    let mut n = 0usize; // bytes written, always < d.len()
    if joliet != 0 {
        // UCS-2BE. An odd trailing byte is malformed; drop it rather than read
        // one byte past the pair.
        let pairs = s.len() / 2;
        let mut i = 0usize;
        while i < pairs {
            let cp = ((s[i * 2] as u32) << 8) | (s[i * 2 + 1] as u32);
            if cp == 0 {
                break;
            }
            let c = if cp >= 0x20 && cp < 0x7F { cp as u8 } else { b'_' };
            if n + 1 >= d.len() {
                break;
            }
            d[n] = c;
            n += 1;
            i += 1;
        }
    } else {
        let mut i = 0usize;
        while i < s.len() {
            let c = s[i];
            if c == 0 {
                break;
            }
            if n + 1 >= d.len() {
                break;
            }
            // Control bytes cannot appear in a legal d-character name; map them
            // so a crafted name can never emit a terminal escape or a newline
            // into the serial log or the UI.
            d[n] = if c >= 0x20 && c < 0x7F { c } else { b'_' };
            n += 1;
            i += 1;
        }
    }

    // Strip the ";<version>" suffix.
    let mut k = 0usize;
    while k < n {
        if d[k] == b';' {
            n = k;
            break;
        }
        k += 1;
    }
    // Strip one trailing '.' (a name with an empty extension is stored "FOO.").
    if n > 0 && d[n - 1] == b'.' {
        n -= 1;
    }

    d[n] = 0;
    n as i32
}

/// #184: is the ROOT DIRECTORY EXTENT this descriptor points at actually inside
/// the image file?
///
/// WHY THIS EXISTS, AND WHAT IT IS NOT. `iso_vd_parse_rs()` above answers "are
/// these 2048 bytes a well-formed volume descriptor". That is a question about
/// the SECTOR, and it is the only question `iso_probe()` in dos/diskimg.c used
/// to ask. It is not the same question as "is there a disc here". Measured on
/// 2026-08-20 (#184): the first 64 KiB of a 607 MiB Discworld II ISO still
/// contains a perfectly valid PVD at sector 16, so that 64 KiB fragment PROBED
/// AS AN ISO, mounted with no error, and was presented to the user as
/// `E: CD-ROM TRUNC.ISO 64 KB ISO 9660 +Joliet` - a disc that reports success
/// and contains nothing, because its root directory lives at LBA 311051, about
/// 607 MB past the end of the file. A mount that cannot list its own root is
/// not a mount, and saying so at mount time is the difference between one clear
/// refusal and an unbounded number of confusing failures later.
///
/// DELIBERATELY THE ROOT EXTENT ONLY, not the descriptor's declared volume size
/// (offset 80). The root extent is UNAMBIGUOUSLY fatal: if it is past EOF there
/// is no directory to read and every path lookup on the disc must fail. A short
/// declared volume size is not: real rips routinely drop trailing padding
/// sectors and still work for every file that matters, so refusing on that would
/// reject images that are fine today. dos/diskimg.c logs the declared-size
/// shortfall instead of refusing on it.
///
/// All arithmetic is checked: `root_lba` and `block_size` both come off the
/// disc, and `root_lba * block_size` overflows a u32 for any lba above 2 Mi
/// sectors, which is only 4 GB of nominal disc. An overflowed product would
/// wrap to a small number and pass a naive bound, which is the same "the check
/// passed because the arithmetic broke" shape as #476.
///
/// Returns 1 when the extent fits, 0 when it does not (or the numbers are
/// unusable). Pure integer logic, no pointers, so there is no unsafe block and
/// nothing for a caller to get wrong.
#[no_mangle]
pub extern "C" fn iso_root_within_rs(
    root_lba: u32,
    root_len: u32,
    block_size: u32,
    image_size: u64,
) -> i32 {
    if block_size == 0 || root_len == 0 || image_size == 0 {
        return 0;
    }
    let base = match (root_lba as u64).checked_mul(block_size as u64) {
        Some(v) => v,
        None => return 0,
    };
    let end = match base.checked_add(root_len as u64) {
        Some(v) => v,
        None => return 0,
    };
    if end > image_size {
        0
    } else {
        1
    }
}

/// #184: the descriptor's declared volume size in BYTES, or 0 if unusable.
///
/// Read here rather than in C for the same reason every other field is: the
/// numbers are attacker-controlled and the multiply can overflow. This is only
/// used to LOG a shortfall (see the doc comment above on why a short volume is
/// not a refusal), so an unusable answer is reported as 0 and the caller says
/// nothing rather than guessing.
///
/// # Safety
/// `sec` must point to at least `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn iso_vd_declared_bytes_rs(sec: *const u8, len: u32) -> u64 {
    if sec.is_null() || len < 190 {
        return 0;
    }
    // SAFETY: caller contract is `len` contiguous readable bytes at `sec`; the
    // slice spans exactly that and both reads below are inside the 190 bytes
    // the guard above already proved are present.
    let s: &[u8] = core::slice::from_raw_parts(sec, len as usize);
    let blocks = rd32le(s, 80) as u64;   // volume space size, 32-bit both-endian
    let bs = rd16le(s, 128) as u64;      // logical block size
    match blocks.checked_mul(bs) {
        Some(v) => v,
        None => 0,
    }
}
