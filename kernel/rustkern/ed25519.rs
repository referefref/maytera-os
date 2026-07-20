// rustkern/ed25519.rs - #404 batch-3 ed25519 point decode (crypto/ed25519.c unpack25519)
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #404 batch-3: ed25519 point-decode (crypto/ed25519.c unpack25519). Decode the
// 32-byte compressed-point y-coordinate into gf[16] + clear the sign bit. Curve
// sqrt / scalarmult / field math STAY in C. Defense-in-depth (fixed-size read,
// no reachable OOB). Routed live under -DRUST_ED25519_DECODE.
// ===========================================================================
#[no_mangle]
pub extern "C" fn unpack25519_rs(o: *mut i64, n: *const u8) {
    if o.is_null() || n.is_null() {
        return;
    }
    // SAFETY: caller passes >=32 readable bytes at n (fixed pubkey) and >=16
    // writable i64 at o (a gf). Slices of EXACTLY 32/16 bound every index.
    let nn = unsafe { core::slice::from_raw_parts(n, 32) };
    let oo = unsafe { core::slice::from_raw_parts_mut(o, 16) };
    for i in 0..16 {
        oo[i] = nn[2 * i] as i64 + ((nn[2 * i + 1] as i64) << 8);
    }
    oo[15] &= 0x7fff;
}
