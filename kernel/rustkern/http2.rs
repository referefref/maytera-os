// rustkern/http2.rs - #404 batch-1 HTTP/2 frame framing (net/http2.c)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ============================================================================
// #404 BATCH-1 SEAM 2/3: HTTP/2 frame framing (net/http2.c http2_get)
// ============================================================================
// http2_frame_next_rs - #404 HTTP/2 frame framing seam (paste into rustkern.rs)
//
// Bounded HTTP/2 frame iterator over a received byte buffer. Validates the
// 9-byte frame header (24-bit length, 8-bit type, 8-bit flags, 31-bit stream
// id with the reserved bit masked off), bounds the 24-bit frame length against
// the remaining buffer, and computes the per-type meaningful-payload extent
// (DATA/HEADERS padding strip + HEADERS PRIORITY strip) with EVERY derived read
// bounds-checked. Never reads outside buf[0..len].
//
// Returns:  1 = one frame decoded, *pos advanced past header + payload
//           0 = clean end (*pos == len, nothing left)
//          -1 = malformed / cannot decode safely: truncated header (< 9 bytes
//               left), frame length past buffer, or a PADDED frame too short to
//               hold its pad-length byte (the C original reads it anyway).
//
// Drop-in for the inline framing in net/http2.c http2_get(): assemble the
// 9-byte header + `flen` payload bytes into one contiguous buffer and call this
// with pos=0; use out.data_off / out.data_len for the HEADERS/DATA fragment.

#[repr(C)]
pub struct H2Frame {
    pub length: u32,      // 24-bit frame payload length
    pub stream_id: u32,   // 31-bit stream id (reserved bit masked off)
    pub hdr_off: u32,     // offset of the 9-byte header within buf
    pub payload_off: u32, // hdr_off + 9
    pub data_off: u32,    // meaningful payload start (after pad/priority strip)
    pub data_len: u32,    // meaningful payload length
    pub type_: u8,
    pub flags: u8,
    pub pad_len: u8,
    pub _reserved: u8,
}
// sizeof locked to 28 bytes / align 4 (mirrored by a _Static_assert in the C ref).

#[no_mangle]
pub extern "C" fn http2_frame_next_rs(
    buf: *const u8,
    len: u32,
    pos_io: *mut u32,
    out: *mut H2Frame,
) -> i32 {
    if buf.is_null() || pos_io.is_null() || out.is_null() {
        return -1;
    }
    let len = len as usize;
    // SAFETY: the caller guarantees `buf` points to `len` readable bytes. We
    // build a slice of EXACTLY len and only ever index inside it; every index
    // below is guarded by an explicit bound check, so no slice panic can fire.
    let data: &[u8] = unsafe { core::slice::from_raw_parts(buf, len) };
    let pos = unsafe { *pos_io } as usize;

    if pos == len {
        return 0; // clean end
    }
    if pos > len {
        return -1; // caller cursor corrupt
    }
    let avail = len - pos;
    if avail < 9 {
        return -1; // truncated frame header
    }

    let flen = ((data[pos] as u32) << 16)
        | ((data[pos + 1] as u32) << 8)
        | (data[pos + 2] as u32);
    let ftype = data[pos + 3];
    let fflags = data[pos + 4];
    let stream = (((data[pos + 5] as u32) << 24)
        | ((data[pos + 6] as u32) << 16)
        | ((data[pos + 7] as u32) << 8)
        | (data[pos + 8] as u32))
        & 0x7fff_ffff;

    // frame-length-vs-buffer bound: 9 + flen must fit in the remaining buffer.
    let need = match (flen as usize).checked_add(9) {
        Some(v) => v,
        None => return -1,
    };
    if need > avail {
        return -1; // frame length claims payload past the buffer
    }

    let payload_off = pos + 9;
    let flen_us = flen as usize;

    let mut data_off = payload_off as u32;
    let mut data_len = flen;
    let mut pad_len: u8 = 0u8;

    match ftype {
        0x00 => {
            // DATA
            if (fflags & 0x08) != 0 {
                // PADDED: the pad-length byte must actually be present.
                if flen_us < 1 {
                    return -1; // bound the C original lacks
                }
                pad_len = data[payload_off];
                let off: u32 = 1;
                // checked_add so off + pad_len can never wrap.
                let consumed = (off as u64).checked_add(pad_len as u64).unwrap_or(u64::MAX);
                if consumed <= flen as u64 {
                    data_off = payload_off as u32 + off;
                    data_len = flen - off - pad_len as u32;
                } else {
                    data_off = payload_off as u32;
                    data_len = 0; // padding exceeds frame -> empty payload
                }
            }
        }
        0x01 => {
            // HEADERS
            let mut off: u32 = 0;
            if (fflags & 0x08) != 0 {
                if flen_us < 1 {
                    return -1; // bound the C original lacks
                }
                pad_len = data[payload_off];
                off += 1;
            }
            if (fflags & 0x20) != 0 {
                off += 5; // PRIORITY: exclusive+dep(4) + weight(1)
            }
            let consumed = (off as u64).checked_add(pad_len as u64).unwrap_or(u64::MAX);
            if consumed <= flen as u64 {
                data_off = payload_off as u32 + off;
                data_len = flen - off - pad_len as u32;
            } else {
                data_off = payload_off as u32;
                data_len = 0; // pad/priority exceed frame -> empty block
            }
        }
        _ => {
            // all other frame types: raw payload bounds already set
        }
    }

    // SAFETY: `out` is non-null (checked above) and, per the C ABI contract,
    // points to a caller-owned H2Frame.
    unsafe {
        (*out).length = flen;
        (*out).stream_id = stream;
        (*out).hdr_off = pos as u32;
        (*out).payload_off = payload_off as u32;
        (*out).data_off = data_off;
        (*out).data_len = data_len;
        (*out).type_ = ftype;
        (*out).flags = fflags;
        (*out).pad_len = pad_len;
        (*out)._reserved = 0;
        *pos_io = (pos + need) as u32;
    }
    1
}
