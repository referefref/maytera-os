// rustkern/sha256.rs - #404 Phase E / #487 SHA-256 block compression core
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// Phase E port (#404 / #487): SHA-256 block compression core.
//
// Faithful, memory-safe Rust drop-in for the pure 64-round SHA-256 block
// compression leaf of crypto/sha256.c (sha256_transform_c). It consumes ONLY
// the 8 working state words and one 64-byte message block, so the sha256_ctx_t
// struct never crosses the FFI boundary: init/update/final stay in C and pass
// ctx->state. Big-endian message schedule W[0..64], the eight working vars, the
// K round constants and the FIPS 180-4 round function are reproduced exactly,
// so the updated state is byte-for-byte identical to the C on every input
// (boot-time sha256_rust_selftest proves it over NIST KATs through the live API
// plus 20000 random state+block differential vectors). crypto/sha256.c routes
// the live sha256_transform() dispatcher here under -DRUST_SHA256.

// SHA-256 round constants (FIPS 180-4, identical to the C K[64]).
const SHA256_K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

#[no_mangle]
pub extern "C" fn sha256_transform_rs(state: *mut u32, block: *const u8) {
    // SAFETY: The caller (crypto/sha256.c sha256_transform dispatcher) guarantees
    // `state` points to exactly 8 readable+writable u32 (ctx->state) and `block`
    // to exactly 64 readable bytes (a full 512-bit message block). We build one
    // slice over each of those exact extents and from here on touch memory ONLY
    // through them, so every access is bounds-checked by Rust: reads never exceed
    // 64 block bytes and writes never exceed the 8 state words. Neither pointer is
    // retained past this call.
    let (st, blk): (&mut [u32], &[u8]) = unsafe {
        (
            core::slice::from_raw_parts_mut(state, 8),
            core::slice::from_raw_parts(block, 64),
        )
    };

    // Big-endian message schedule (matches the C W[i] = block[..]<<24 | ...).
    let mut w = [0u32; 64];
    let mut i = 0usize;
    while i < 16 {
        w[i] = ((blk[i * 4] as u32) << 24)
            | ((blk[i * 4 + 1] as u32) << 16)
            | ((blk[i * 4 + 2] as u32) << 8)
            | (blk[i * 4 + 3] as u32);
        i += 1;
    }
    while i < 64 {
        // SIG1(x) = ROTR(x,17)^ROTR(x,19)^(x>>10); SIG0(x)=ROTR(x,7)^ROTR(x,18)^(x>>3)
        let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
        let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
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
    // additions wrapping at 32 bits exactly as C unsigned overflow does).
    let mut r = 0usize;
    while r < 64 {
        // EP1(e) = ROTR(e,6)^ROTR(e,11)^ROTR(e,25); CH(e,f,g)=(e&f)^(~e&g)
        let ep1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
        let ch = (e & f) ^ ((!e) & g);
        let t1 = h
            .wrapping_add(ep1)
            .wrapping_add(ch)
            .wrapping_add(SHA256_K[r])
            .wrapping_add(w[r]);
        // EP0(a) = ROTR(a,2)^ROTR(a,13)^ROTR(a,22); MAJ(a,b,c)=(a&b)^(a&c)^(b&c)
        let ep0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
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
