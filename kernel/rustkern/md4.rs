// rustkern/md4.rs - #404 Phase H / #490 MD4 block compression core
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// Phase H port (#404 / #490): MD4 block compression core.
//
// Faithful, memory-safe Rust drop-in for the pure 48-step (3-round) MD4 block-
// compression leaf of crypto/md4.c (md4_transform_c). Same struct-free shape as
// the MD5/SHA ports: it consumes ONLY the 4 working state words (u32) and one
// 64-byte message block, so the md4_ctx_t struct never crosses the FFI boundary
// (init/update/final stay in C and pass ctx->state). MD4 backs NTLM password
// hashing (net/smb.c) and is a COLD path.
//
// ENDIANNESS: identical to MD5 - the message words are decoded LITTLE-endian
// (X[i] = block[i*4] | block[i*4+1]<<8 | ...), matching the C x[i] decode; the
// digest is emitted little-endian by md4_final (that stays entirely in C).
//
// STRUCTURE vs MD5: MD4 has 3 rounds (F / G / H) of 16 steps = 48 steps, NOT
// MD5's 4x16=64. The step form also differs: MD4 REPLACES the register with the
// rotated sum (a = ROTL(a + f + x + K, s)) with NO trailing `+ b`, whereas MD5
// does `b += ROTL(...)`. The three round additive constants are 0 (round 1),
// 0x5A827999 (round 2), 0x6ED9EBA1 (round 3). Rounds 2 and 3 use fixed message-
// word index PERMUTATIONS (not a modular formula), so the 48 steps below are
// unrolled in the EXACT order and with the EXACT shift amounts of the C
// FF/GG/HH macro sequence. Being unrolled the same way as the C, perf is at
// parity (unlike the rolled MD5 loop). All adds are wrapping_add (C unsigned
// overflow) and all rotations rotate_left. Boot-time md4_rust_selftest proves it
// over RFC 1320 KATs through the live md4() API plus a large random state+block
// differential; crypto/md4.c routes the live md4_transform() dispatcher here
// under -DRUST_MD4.
#[no_mangle]
pub extern "C" fn md4_transform_rs(state: *mut u32, block: *const u8) {
    // SAFETY: The caller (crypto/md4.c md4_transform dispatcher) guarantees
    // `state` points to exactly 4 readable+writable u32 (ctx->state) and `block`
    // to exactly 64 readable bytes (a full 512-bit message block). We build one
    // slice over each of those exact extents and from here on touch memory ONLY
    // through them, so every access is bounds-checked by Rust: reads never exceed
    // 64 block bytes and writes never exceed the 4 state words. Neither pointer is
    // retained past this call.
    let (st, blk): (&mut [u32], &[u8]) = unsafe {
        (
            core::slice::from_raw_parts_mut(state, 4),
            core::slice::from_raw_parts(block, 64),
        )
    };

    // LITTLE-endian message schedule (matches the C x[i] = block[i*4] |
    // block[i*4+1]<<8 | block[i*4+2]<<16 | block[i*4+3]<<24).
    let mut x = [0u32; 16];
    let mut i = 0usize;
    while i < 16 {
        x[i] = (blk[i * 4] as u32)
            | ((blk[i * 4 + 1] as u32) << 8)
            | ((blk[i * 4 + 2] as u32) << 16)
            | ((blk[i * 4 + 3] as u32) << 24);
        i += 1;
    }

    let mut a = st[0];
    let mut b = st[1];
    let mut c = st[2];
    let mut d = st[3];

    // Round 1 step: a = ROTL(a + F(b,c,d) + x, s), F = (b&c) | (~b&d).
    macro_rules! ff {
        ($a:ident,$b:ident,$c:ident,$d:ident,$x:expr,$s:expr) => {
            $a = $a
                .wrapping_add(($b & $c) | ((!$b) & $d))
                .wrapping_add($x)
                .rotate_left($s);
        };
    }
    // Round 2 step: a = ROTL(a + G(b,c,d) + x + 0x5A827999, s),
    //   G = (b&c) | (b&d) | (c&d).
    macro_rules! gg {
        ($a:ident,$b:ident,$c:ident,$d:ident,$x:expr,$s:expr) => {
            $a = $a
                .wrapping_add(($b & $c) | ($b & $d) | ($c & $d))
                .wrapping_add($x)
                .wrapping_add(0x5A827999u32)
                .rotate_left($s);
        };
    }
    // Round 3 step: a = ROTL(a + H(b,c,d) + x + 0x6ED9EBA1, s), H = b^c^d.
    macro_rules! hh {
        ($a:ident,$b:ident,$c:ident,$d:ident,$x:expr,$s:expr) => {
            $a = $a
                .wrapping_add($b ^ $c ^ $d)
                .wrapping_add($x)
                .wrapping_add(0x6ED9EBA1u32)
                .rotate_left($s);
        };
    }

    // Round 1 (x[0..15] in order; shifts 3,7,11,19).
    ff!(a, b, c, d, x[0], 3);  ff!(d, a, b, c, x[1], 7);  ff!(c, d, a, b, x[2], 11);  ff!(b, c, d, a, x[3], 19);
    ff!(a, b, c, d, x[4], 3);  ff!(d, a, b, c, x[5], 7);  ff!(c, d, a, b, x[6], 11);  ff!(b, c, d, a, x[7], 19);
    ff!(a, b, c, d, x[8], 3);  ff!(d, a, b, c, x[9], 7);  ff!(c, d, a, b, x[10], 11); ff!(b, c, d, a, x[11], 19);
    ff!(a, b, c, d, x[12], 3); ff!(d, a, b, c, x[13], 7); ff!(c, d, a, b, x[14], 11); ff!(b, c, d, a, x[15], 19);

    // Round 2 (index perm 0,4,8,12,1,5,9,13,...; shifts 3,5,9,13).
    gg!(a, b, c, d, x[0], 3);  gg!(d, a, b, c, x[4], 5);  gg!(c, d, a, b, x[8], 9);   gg!(b, c, d, a, x[12], 13);
    gg!(a, b, c, d, x[1], 3);  gg!(d, a, b, c, x[5], 5);  gg!(c, d, a, b, x[9], 9);   gg!(b, c, d, a, x[13], 13);
    gg!(a, b, c, d, x[2], 3);  gg!(d, a, b, c, x[6], 5);  gg!(c, d, a, b, x[10], 9);  gg!(b, c, d, a, x[14], 13);
    gg!(a, b, c, d, x[3], 3);  gg!(d, a, b, c, x[7], 5);  gg!(c, d, a, b, x[11], 9);  gg!(b, c, d, a, x[15], 13);

    // Round 3 (index perm 0,8,4,12,2,10,6,14,...; shifts 3,9,11,15).
    hh!(a, b, c, d, x[0], 3);  hh!(d, a, b, c, x[8], 9);  hh!(c, d, a, b, x[4], 11);  hh!(b, c, d, a, x[12], 15);
    hh!(a, b, c, d, x[2], 3);  hh!(d, a, b, c, x[10], 9); hh!(c, d, a, b, x[6], 11);  hh!(b, c, d, a, x[14], 15);
    hh!(a, b, c, d, x[1], 3);  hh!(d, a, b, c, x[9], 9);  hh!(c, d, a, b, x[5], 11);  hh!(b, c, d, a, x[13], 15);
    hh!(a, b, c, d, x[3], 3);  hh!(d, a, b, c, x[11], 9); hh!(c, d, a, b, x[7], 11);  hh!(b, c, d, a, x[15], 15);

    st[0] = st[0].wrapping_add(a);
    st[1] = st[1].wrapping_add(b);
    st[2] = st[2].wrapping_add(c);
    st[3] = st[3].wrapping_add(d);
}
