// rustkern/wav.rs - #404 batch-2 RIFF/WAVE header parse (media/wav.c)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #404 batch-2 seam 1/3: media/wav.c RIFF/WAVE header-parse (wav_parse_header_rs).
// Drop-in for wav_parse_header_c: walks the RIFF container (RIFF/WAVE magic,
// 4-byte id + 4-byte LE size chunk headers), reads the fmt/data chunk fields,
// applies the data-size-vs-buffer clamp, validates, and fills a #[repr(C)]
// WavInfo. PURE: never allocates/copies/decodes; the PCM sample decode stays in
// C behind this parse. Every input read is a bounds-checked slice access over a
// span of EXACTLY `len` bytes, so no chunk size or fmt length (all attacker-
// controlled) can push a read past the buffer. (Latent/defense-in-depth: the
// live C wav_create walk is already bounded; this removes the class.)
// ===========================================================================
pub const WAV_OK: i32 = 0;
pub const WAV_ERR_NULL: i32 = -1;
pub const WAV_ERR_TOO_SMALL: i32 = -2;
pub const WAV_ERR_NOT_WAVE: i32 = -3;
pub const WAV_ERR_MISSING_CHUNK: i32 = -4;
pub const WAV_ERR_FLOAT: i32 = -5;
pub const WAV_ERR_UNSUPPORTED_FMT: i32 = -6;
pub const WAV_ERR_UNSUPPORTED_PARAMS: i32 = -7;

const WAV_FMT_PCM: u16 = 0x0001;
const WAV_FMT_IEEE_FLOAT: u16 = 0x0003;
const WAV_FMT_EXTENSIBLE: u16 = 0xFFFE;

// #[repr(C)] mirror of the C WavInfo (media/wav.c). All fields explicit; size is
// asserted == 20 at compile time so the FFI struct can never silently drift.
#[repr(C)]
pub struct WavInfo {
    format: u16,
    channels: u16,
    sample_rate: u32,
    bits_per_sample: u16,
    reserved: u16,
    data_off: u32,
    data_size: u32,
}
const _: () = assert!(core::mem::size_of::<WavInfo>() == 20);

#[inline]
fn wav_rd_le16(s: &[u8], at: usize) -> u16 {
    (s[at] as u16) | ((s[at + 1] as u16) << 8)
}
#[inline]
fn wav_rd_le32(s: &[u8], at: usize) -> u32 {
    (s[at] as u32)
        | ((s[at + 1] as u32) << 8)
        | ((s[at + 2] as u32) << 16)
        | ((s[at + 3] as u32) << 24)
}

/// Rust port of wav_parse_header_c (media/wav.c wav_create() walk + validation).
/// Fills `out` and returns WAV_OK(0) or a negative WAV_ERR_*. Byte-for-byte
/// equivalent to the C for every well-formed AND malformed input.
#[no_mangle]
pub extern "C" fn wav_parse_header_rs(data: *const u8, len: u32, out: *mut WavInfo) -> i32 {
    if data.is_null() || out.is_null() {
        return WAV_ERR_NULL;
    }
    let n = len as usize;
    // SAFETY: the caller (wav_create / the boot self-test) guarantees `data`
    // points to at least `len` readable bytes (the .wav file buffer). We span
    // EXACTLY `len` and index only via bounds-checked slice access below, so no
    // read leaves the buffer even when a chunk size or fmt length is nonsense.
    let s: &[u8] = unsafe { core::slice::from_raw_parts(data, n) };

    let mut info = WavInfo {
        format: 0,
        channels: 0,
        sample_rate: 0,
        bits_per_sample: 0,
        reserved: 0,
        data_off: 0,
        data_size: 0,
    };
    let rc = wav_parse_walk(s, &mut info);
    // SAFETY: out is non-null (checked) and points to a writable WavInfo (the C
    // caller passes &WavInfo, sizeof-locked == 20 to match this #[repr(C)]). We
    // write unconditionally, matching the C which zeroes then fills *out.
    unsafe {
        core::ptr::write(out, info);
    }
    rc
}

fn wav_parse_walk(s: &[u8], out: &mut WavInfo) -> i32 {
    let n = s.len();
    if n < 44 {
        return WAV_ERR_TOO_SMALL;
    }
    // magic: n >= 44 guarantees indices 0..=11 exist.
    if !(s[0] == b'R' && s[1] == b'I' && s[2] == b'F' && s[3] == b'F'
        && s[8] == b'W' && s[9] == b'A' && s[10] == b'V' && s[11] == b'E')
    {
        return WAV_ERR_NOT_WAVE;
    }

    let mut fmt: u16 = 0;
    let mut channels: i32 = 0;    // int semantics matching C (uint16 -> int)
    let mut sample_rate: i32 = 0; // int semantics matching C (uint32 -> int, 2's-comp)
    let mut bits: i32 = 0;
    let mut data_off: u32 = 0;
    let mut data_size: u32 = 0;
    let mut have_fmt = false;
    let mut have_data = false;

    let mut off: usize = 12; // skip "RIFF" + size + "WAVE"
    while off + 8 <= n {
        // off+8 <= n => bytes off..off+7 (id + csize) are in-bounds.
        let csize = wav_rd_le32(s, off + 4);
        let body = off + 8;
        let id = (s[off], s[off + 1], s[off + 2], s[off + 3]);
        if id == (b'f', b'm', b't', b' ') {
            if body + 16 <= n {
                fmt = wav_rd_le16(s, body);
                channels = wav_rd_le16(s, body + 2) as i32;
                sample_rate = wav_rd_le32(s, body + 4) as i32;
                bits = wav_rd_le16(s, body + 14) as i32;
                if fmt == WAV_FMT_EXTENSIBLE && body + 26 <= n {
                    let cb = wav_rd_le16(s, body + 16);
                    if cb >= 2 && body + 24 + 2 <= n {
                        fmt = wav_rd_le16(s, body + 24);
                    }
                }
                have_fmt = true;
            }
        } else if id == (b'd', b'a', b't', b'a') {
            data_off = body as u32;
            data_size = csize;
            // clamp to the actual buffer (truncated file). checked_add on usize;
            // body <= n (from off+8<=n) so no underflow in (n - data_off).
            if (data_off as usize).checked_add(data_size as usize).map_or(true, |v| v > n) {
                data_size = (n - data_off as usize) as u32;
            }
            have_data = true;
        }
        // Word-aligned chunk advance: off = body + csize + (csize & 1). Every
        // offset add is checked; on (impossible at 64-bit) overflow, end the walk
        // exactly as C would (off would exceed size, failing off+8<=size).
        let pad = (csize & 1) as usize;
        let next = (csize as usize)
            .checked_add(pad)
            .and_then(|x| body.checked_add(x));
        off = match next {
            Some(v) => v, // v >= body >= off+8 > off, so the loop always progresses
            None => break,
        };
    }

    if !have_fmt || !have_data {
        return WAV_ERR_MISSING_CHUNK;
    }
    if fmt == WAV_FMT_IEEE_FLOAT {
        return WAV_ERR_FLOAT;
    }
    if fmt != WAV_FMT_PCM {
        return WAV_ERR_UNSUPPORTED_FMT;
    }
    if channels <= 0
        || sample_rate <= 0
        || (bits != 8 && bits != 16 && bits != 24 && bits != 32)
    {
        return WAV_ERR_UNSUPPORTED_PARAMS;
    }

    out.format = fmt;
    out.channels = channels as u16;
    out.sample_rate = sample_rate as u32;
    out.bits_per_sample = bits as u16;
    out.reserved = 0;
    out.data_off = data_off;
    out.data_size = data_size;
    WAV_OK
}
