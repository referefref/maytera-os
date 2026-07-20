// rustkern/sha512.rs - #404 Phase F / #488 SHA-512 block compression core
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// Phase F port (#404 / #488): SHA-512 block compression core.
//
// Faithful, memory-safe Rust drop-in for the pure 80-round SHA-512 block
// compression leaf of crypto/sha512.c (sha512_transform_c). Same shape as the
// SHA-256 port but 64-bit: it consumes ONLY the 8 working state words (u64) and
// one 128-byte message block, so the sha512_ctx_t struct never crosses the FFI
// boundary (init/update/final stay in C and pass ctx->state). The big-endian
// 64-bit message schedule W[0..80], the eight working vars, the K512 round
// constants and the FIPS 180-4 round function (SHA-512 rotate amounts) are
// reproduced exactly, with every add wrapping at 64 bits as C unsigned overflow
// does, so the updated state is byte-for-byte identical to the C on every input
// (boot-time sha512_rust_selftest proves it over NIST KATs through the live
// sha512 API plus 20000 random state+block differential vectors). crypto/sha512.c
// routes the live sha512_transform() dispatcher here under -DRUST_SHA512.

// SHA-512 round constants (FIPS 180-4, identical to the C K512[80]).
const SHA512_K: [u64; 80] = [
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc,
    0x3956c25bf348b538, 0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118,
    0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694,
    0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4,
    0xc6e00bf33da88fc2, 0xd5a79147930aa725, 0x06ca6351e003826f, 0x142929670a0e6e70,
    0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30,
    0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8,
    0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3,
    0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b,
    0xca273eceea26619c, 0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178,
    0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c,
    0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
];

#[no_mangle]
pub extern "C" fn sha512_transform_rs(state: *mut u64, block: *const u8) {
    // SAFETY: The caller (crypto/sha512.c sha512_transform dispatcher) guarantees
    // `state` points to exactly 8 readable+writable u64 (ctx->state) and `block`
    // to exactly 128 readable bytes (a full 1024-bit message block). We build one
    // slice over each of those exact extents and from here on touch memory ONLY
    // through them, so every access is bounds-checked by Rust: reads never exceed
    // 128 block bytes and writes never exceed the 8 state words. Neither pointer is
    // retained past this call.
    let (st, blk): (&mut [u64], &[u8]) = unsafe {
        (
            core::slice::from_raw_parts_mut(state, 8),
            core::slice::from_raw_parts(block, 128),
        )
    };

    // Big-endian 64-bit message schedule (matches the C W[i] = block[..]<<56 |...).
    let mut w = [0u64; 80];
    let mut i = 0usize;
    while i < 16 {
        w[i] = ((blk[i * 8] as u64) << 56)
            | ((blk[i * 8 + 1] as u64) << 48)
            | ((blk[i * 8 + 2] as u64) << 40)
            | ((blk[i * 8 + 3] as u64) << 32)
            | ((blk[i * 8 + 4] as u64) << 24)
            | ((blk[i * 8 + 5] as u64) << 16)
            | ((blk[i * 8 + 6] as u64) << 8)
            | (blk[i * 8 + 7] as u64);
        i += 1;
    }
    while i < 80 {
        // SIG1_64(x)=ROTR(x,19)^ROTR(x,61)^(x>>6); SIG0_64(x)=ROTR(x,1)^ROTR(x,8)^(x>>7)
        let s1 = w[i - 2].rotate_right(19) ^ w[i - 2].rotate_right(61) ^ (w[i - 2] >> 6);
        let s0 = w[i - 15].rotate_right(1) ^ w[i - 15].rotate_right(8) ^ (w[i - 15] >> 7);
        w[i] = s1
            .wrapping_add(w[i - 7])
            .wrapping_add(s0)
            .wrapping_add(w[i - 16]);
        i += 1;
    }

    let mut a = st[0];
    let mut b = st[1];
    let mut c = st[2];
    let mut d = st[3];
    let mut e = st[4];
    let mut f = st[5];
    let mut g = st[6];
    let mut h = st[7];

    // Main compression loop (FIPS 180-4, identical arithmetic to the C, all
    // additions wrapping at 64 bits exactly as C unsigned overflow does).
    let mut r = 0usize;
    while r < 80 {
        // EP1_64(e)=ROTR(e,14)^ROTR(e,18)^ROTR(e,41); CH(e,f,g)=(e&f)^(~e&g)
        let ep1 = e.rotate_right(14) ^ e.rotate_right(18) ^ e.rotate_right(41);
        let ch = (e & f) ^ ((!e) & g);
        let t1 = h
            .wrapping_add(ep1)
            .wrapping_add(ch)
            .wrapping_add(SHA512_K[r])
            .wrapping_add(w[r]);
        // EP0_64(a)=ROTR(a,28)^ROTR(a,34)^ROTR(a,39); MAJ(a,b,c)=(a&b)^(a&c)^(b&c)
        let ep0 = a.rotate_right(28) ^ a.rotate_right(34) ^ a.rotate_right(39);
        let maj = (a & b) ^ (a & c) ^ (b & c);
        let t2 = ep0.wrapping_add(maj);
        h = g;
        g = f;
        f = e;
        e = d.wrapping_add(t1);
        d = c;
        c = b;
        b = a;
        a = t1.wrapping_add(t2);
        r += 1;
    }

    st[0] = st[0].wrapping_add(a);
    st[1] = st[1].wrapping_add(b);
    st[2] = st[2].wrapping_add(c);
    st[3] = st[3].wrapping_add(d);
    st[4] = st[4].wrapping_add(e);
    st[5] = st[5].wrapping_add(f);
    st[6] = st[6].wrapping_add(g);
    st[7] = st[7].wrapping_add(h);
}
