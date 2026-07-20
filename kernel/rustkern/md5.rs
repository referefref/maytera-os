// rustkern/md5.rs - #404 Phase G / #489 MD5 block compression core
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// Phase G port (#404 / #489): MD5 block compression core.
//
// Faithful, memory-safe Rust drop-in for the pure 64-round MD5 block-
// compression leaf of crypto/md5.c (md5_transform_c). Same struct-free shape as
// the SHA ports: it consumes ONLY the 4 working state words (u32) and one
// 64-byte message block, so the md5_ctx_t struct never crosses the FFI boundary
// (init/update/final stay in C and pass ctx->state).
//
// ENDIANNESS (the one thing that differs from the SHA ports): MD5 decodes the
// message words LITTLE-endian (M[i] = block[i*4] | block[i*4+1]<<8 | ...),
// exactly the reverse of SHA-256/512's big-endian schedule and matching the C
// `x[i] = block[..] | (block[..] << 8) | ...`. The digest is likewise emitted
// little-endian by md5_final (that byte-ordering stays entirely in C; this leaf
// only touches the 4 state words).
//
// The 4 auxiliary functions F/G/H/I, the per-step left-rotation amounts, the
// T (sine) constant table and the message-word index per round are the RFC 1321
// reference. The single-loop variable rotation below is provably identical to
// the C's FF/GG/HH/II argument-permutation form (each step reads the current
// (b,c,d), rotates the running sum by s, adds b, and shifts the words), so the
// updated state is byte-for-byte identical to the C on every input (boot-time
// md5_rust_selftest proves it over RFC 1321 KATs through the live md5() API plus
// a large random state+block differential). crypto/md5.c routes the live
// md5_transform() dispatcher here under -DRUST_MD5.

// MD5 per-step left-rotation amounts (RFC 1321: 4 rounds x the same 4 values).
const MD5_S: [u32; 64] = [
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
];

// MD5 T constants K[i] = floor(2^32 * abs(sin(i+1))). Listed in the exact order
// the C md5_transform_c passes them as the `ac` argument (round 1 -> 4), which
// is index order K[0..64].
const MD5_K: [u32; 64] = [
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
    0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
    0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
    0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
];

#[no_mangle]
pub extern "C" fn md5_transform_rs(state: *mut u32, block: *const u8) {
    // SAFETY: The caller (crypto/md5.c md5_transform dispatcher) guarantees
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
    let mut m = [0u32; 16];
    let mut i = 0usize;
    while i < 16 {
        m[i] = (blk[i * 4] as u32)
            | ((blk[i * 4 + 1] as u32) << 8)
            | ((blk[i * 4 + 2] as u32) << 16)
            | ((blk[i * 4 + 3] as u32) << 24);
        i += 1;
    }

    let mut a = st[0];
    let mut b = st[1];
    let mut c = st[2];
    let mut d = st[3];

    // Single-loop form, provably identical to the C FF/GG/HH/II sequence:
    //   f, g index selected per round; sum = a + f + K[r] + m[g];
    //   then rotate the words (a<-d, d<-c, c<-b) and b <- b + ROTL(sum, s[r]).
    // All adds wrap at 32 bits exactly as C unsigned overflow does.
    let mut r = 0usize;
    while r < 64 {
        let (f, g) = if r < 16 {
            // F(b,c,d) = (b & c) | (~b & d)
            ((b & c) | ((!b) & d), r)
        } else if r < 32 {
            // G(b,c,d) = (b & d) | (c & ~d)
            ((b & d) | (c & (!d)), (5 * r + 1) % 16)
        } else if r < 48 {
            // H(b,c,d) = b ^ c ^ d
            (b ^ c ^ d, (3 * r + 5) % 16)
        } else {
            // I(b,c,d) = c ^ (b | ~d)
            (c ^ (b | (!d)), (7 * r) % 16)
        };
        let sum = a
            .wrapping_add(f)
            .wrapping_add(MD5_K[r])
            .wrapping_add(m[g]);
        a = d;
        d = c;
        c = b;
        b = b.wrapping_add(sum.rotate_left(MD5_S[r]));
        r += 1;
    }

    st[0] = st[0].wrapping_add(a);
    st[1] = st[1].wrapping_add(b);
    st[2] = st[2].wrapping_add(c);
    st[3] = st[3].wrapping_add(d);
}
