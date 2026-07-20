// rustkern/aes.rs - #404 Phase J / #492 AES block encrypt + decrypt cores
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// Phase J port (#404 / #492): AES block encrypt + decrypt cores.
//
// Faithful, memory-safe Rust drop-ins for the pure per-block AES cores of
// crypto/aes.c (aes_encrypt_block_c / aes_decrypt_block_c). Like the SHA/MD/
// ChaCha20 ports, ONLY the flat data crosses the FFI boundary: the expanded
// round-key array (uint32 words) + the round count Nr + the 16-byte in/out
// blocks. The aes_ctx_t struct itself never crosses FFI, so the key schedule
// (aes_set_encrypt_key / aes_key_expansion) stays entirely in C for this step;
// only the block encrypt/decrypt route to Rust. crypto/aes.c dispatches the live
// aes_encrypt_block() / aes_decrypt_block() symbols here under -DRUST_AES (else
// the C cores).
//
// IMPLEMENTATION SHAPE (matches the C byte-for-byte): the C is the plain S-box
// round form, NOT a T-table (Te0..Te3/Td0..Td3) implementation, so this Rust is
// the same plain S-box form (identical SubBytes/ShiftRows/MixColumns/AddRoundKey
// leaves). The round keys are big-endian uint32 words exactly as the C stores
// them (rk[i] holds column i's 4 bytes as (b0<<24)|(b1<<16)|(b2<<8)|b3), and
// AddRoundKey for round r XORs rk[4*r + col] into state column col high-byte
// first, matching add_round_key(). The state is column-major state[4*col + row]
// exactly as the C indexes it. GF(2^8) helpers xtime()/gf_mul() and the forward/
// inverse S-boxes are reproduced verbatim from the C tables/functions, so the
// output is byte-for-byte identical to the C on every (state, round-key, Nr).
// Nr is 10/12/14 for AES-128/192/256; the round-key array length is 4*(Nr+1)
// words (<= 60, the aes_ctx_t rk[60]). Boot-time aes_rust_selftest proves it
// against the FIPS-197 AES-128 (and AES-256 Appendix-C) KAT through the LIVE
// aes_set_encrypt_key/aes_encrypt_block/aes_decrypt_block path plus a large
// random (state, round-keys, Nr) differential vs the C cores, and RDTSC-benches
// both. AES backs TLS AES-GCM (crypto/crypto.c) and SSH aes-ctr (net/ssh/*).

// AES forward S-box (FIPS 197, identical to the C sbox[256]).
const AES_SBOX: [u8; 256] = [
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5,
    0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
    0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a,
    0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
    0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85,
    0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17,
    0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9,
    0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
    0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94,
    0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68,
    0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
];

// AES inverse S-box (FIPS 197, identical to the C inv_sbox[256]).
const AES_INV_SBOX: [u8; 256] = [
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38,
    0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb,
    0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87,
    0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb,
    0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2, 0x23, 0x3d,
    0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2,
    0x76, 0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25,
    0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16,
    0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92,
    0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda,
    0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a,
    0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06,
    0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02,
    0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b,
    0x3a, 0x91, 0x11, 0x41, 0x4f, 0x67, 0xdc, 0xea,
    0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85,
    0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e,
    0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89,
    0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b,
    0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20,
    0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31,
    0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f,
    0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d,
    0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef,
    0xa0, 0xe0, 0x3b, 0x4d, 0xae, 0x2a, 0xf5, 0xb0,
    0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26,
    0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d,
];

// Multiply by 2 in GF(2^8) (C: xtime()). The u8 `<< 1` drops the shifted-out top
// bit exactly as the C uint8_t truncation does.
#[inline(always)]
fn aes_xtime(x: u8) -> u8 {
    (x << 1) ^ (((x >> 7) & 1).wrapping_mul(0x1b))
}

// General GF(2^8) multiply (C: gf_mul()). Verbatim bit-by-bit shift-and-add with
// the 0x1b reduction; all `<<= 1` truncate at 8 bits exactly as the C does.
#[inline(always)]
fn aes_gf_mul(mut a: u8, mut b: u8) -> u8 {
    let mut p: u8 = 0;
    let mut i = 0;
    while i < 8 {
        if b & 1 != 0 {
            p ^= a;
        }
        let hi = a & 0x80;
        a <<= 1;
        if hi != 0 {
            a ^= 0x1b;
        }
        b >>= 1;
        i += 1;
    }
    p
}

// AddRoundKey (C: add_round_key). `base` is the word index of the first of the 4
// round-key words for this round (C passes &rk[4*round]); each word XORs into one
// state column, high byte first.
#[inline(always)]
fn aes_add_round_key(state: &mut [u8; 16], rk: &[u32], base: usize) {
    let mut i = 0usize;
    while i < 4 {
        let k = rk[base + i];
        state[4 * i] ^= ((k >> 24) & 0xff) as u8;
        state[4 * i + 1] ^= ((k >> 16) & 0xff) as u8;
        state[4 * i + 2] ^= ((k >> 8) & 0xff) as u8;
        state[4 * i + 3] ^= (k & 0xff) as u8;
        i += 1;
    }
}

// SubBytes / InvSubBytes (C: sub_bytes / inv_sub_bytes).
#[inline(always)]
fn aes_sub_bytes(state: &mut [u8; 16]) {
    let mut i = 0usize;
    while i < 16 {
        state[i] = AES_SBOX[state[i] as usize];
        i += 1;
    }
}
#[inline(always)]
fn aes_inv_sub_bytes(state: &mut [u8; 16]) {
    let mut i = 0usize;
    while i < 16 {
        state[i] = AES_INV_SBOX[state[i] as usize];
        i += 1;
    }
}

// ShiftRows (C: shift_rows) - column-major state[4*col+row].
#[inline(always)]
fn aes_shift_rows(state: &mut [u8; 16]) {
    // Row 1: shift left by 1.
    let temp = state[1];
    state[1] = state[5];
    state[5] = state[9];
    state[9] = state[13];
    state[13] = temp;
    // Row 2: shift left by 2.
    let temp = state[2];
    state[2] = state[10];
    state[10] = temp;
    let temp = state[6];
    state[6] = state[14];
    state[14] = temp;
    // Row 3: shift left by 3 (= right by 1).
    let temp = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7] = state[3];
    state[3] = temp;
}

// InvShiftRows (C: inv_shift_rows).
#[inline(always)]
fn aes_inv_shift_rows(state: &mut [u8; 16]) {
    // Row 1: shift right by 1.
    let temp = state[13];
    state[13] = state[9];
    state[9] = state[5];
    state[5] = state[1];
    state[1] = temp;
    // Row 2: shift right by 2.
    let temp = state[2];
    state[2] = state[10];
    state[10] = temp;
    let temp = state[6];
    state[6] = state[14];
    state[14] = temp;
    // Row 3: shift right by 3 (= left by 1).
    let temp = state[3];
    state[3] = state[7];
    state[7] = state[11];
    state[11] = state[15];
    state[15] = temp;
}

// MixColumns (C: mix_columns).
#[inline(always)]
fn aes_mix_columns(state: &mut [u8; 16]) {
    let mut i = 0usize;
    while i < 4 {
        let a = state[4 * i];
        let b = state[4 * i + 1];
        let c = state[4 * i + 2];
        let d = state[4 * i + 3];
        state[4 * i] = aes_xtime(a) ^ aes_xtime(b) ^ b ^ c ^ d;
        state[4 * i + 1] = a ^ aes_xtime(b) ^ aes_xtime(c) ^ c ^ d;
        state[4 * i + 2] = a ^ b ^ aes_xtime(c) ^ aes_xtime(d) ^ d;
        state[4 * i + 3] = aes_xtime(a) ^ a ^ b ^ c ^ aes_xtime(d);
        i += 1;
    }
}

// InvMixColumns (C: inv_mix_columns).
#[inline(always)]
fn aes_inv_mix_columns(state: &mut [u8; 16]) {
    let mut i = 0usize;
    while i < 4 {
        let a = state[4 * i];
        let b = state[4 * i + 1];
        let c = state[4 * i + 2];
        let d = state[4 * i + 3];
        state[4 * i] = aes_gf_mul(a, 0x0e) ^ aes_gf_mul(b, 0x0b) ^ aes_gf_mul(c, 0x0d) ^ aes_gf_mul(d, 0x09);
        state[4 * i + 1] = aes_gf_mul(a, 0x09) ^ aes_gf_mul(b, 0x0e) ^ aes_gf_mul(c, 0x0b) ^ aes_gf_mul(d, 0x0d);
        state[4 * i + 2] = aes_gf_mul(a, 0x0d) ^ aes_gf_mul(b, 0x09) ^ aes_gf_mul(c, 0x0e) ^ aes_gf_mul(d, 0x0b);
        state[4 * i + 3] = aes_gf_mul(a, 0x0b) ^ aes_gf_mul(b, 0x0d) ^ aes_gf_mul(c, 0x09) ^ aes_gf_mul(d, 0x0e);
        i += 1;
    }
}

/// Rust port of crypto/aes.c aes_encrypt_block_c(). `rk` is the expanded round-
/// key array (>= 4*(nr+1) big-endian u32 words), `nr` the round count (10/12/14),
/// `input`/`out` the 16-byte plaintext/ciphertext blocks. Byte-for-byte identical
/// to the C core. crypto/aes.c routes the live aes_encrypt_block() here under
/// -DRUST_AES.
#[no_mangle]
pub extern "C" fn aes_encrypt_block_rs(rk: *const u32, nr: i32, input: *const u8, out: *mut u8) {
    let nr = nr as usize;
    // SAFETY: The caller (crypto/aes.c aes_encrypt_block dispatcher) guarantees
    // `rk` points to at least 4*(nr+1) readable u32 round-key words (ctx->rk,
    // sized rk[60] >= 4*(14+1)=60), `input` to exactly 16 readable bytes and
    // `out` to exactly 16 writable bytes. We build one slice over each of those
    // exact extents; from here on every access to round keys / input / output
    // goes through a slice or the local `state` array, so it is bounds-checked by
    // Rust. None of the pointers is retained past this call.
    let (rks, inb, outb): (&[u32], &[u8], &mut [u8]) = unsafe {
        (
            core::slice::from_raw_parts(rk, 4 * (nr + 1)),
            core::slice::from_raw_parts(input, 16),
            core::slice::from_raw_parts_mut(out, 16),
        )
    };

    let mut state = [0u8; 16];
    state.copy_from_slice(inb);

    // Initial round key addition.
    aes_add_round_key(&mut state, rks, 0);

    // Main rounds.
    let mut round = 1usize;
    while round < nr {
        aes_sub_bytes(&mut state);
        aes_shift_rows(&mut state);
        aes_mix_columns(&mut state);
        aes_add_round_key(&mut state, rks, 4 * round);
        round += 1;
    }

    // Final round (no MixColumns).
    aes_sub_bytes(&mut state);
    aes_shift_rows(&mut state);
    aes_add_round_key(&mut state, rks, 4 * nr);

    outb.copy_from_slice(&state);
}

/// Rust port of crypto/aes.c aes_decrypt_block_c(). Same argument contract as the
/// encrypt core; runs the inverse cipher (InvShiftRows/InvSubBytes/AddRoundKey/
/// InvMixColumns) in the C's round order. Byte-for-byte identical to the C.
/// crypto/aes.c routes the live aes_decrypt_block() here under -DRUST_AES.
#[no_mangle]
pub extern "C" fn aes_decrypt_block_rs(rk: *const u32, nr: i32, input: *const u8, out: *mut u8) {
    let nr = nr as usize;
    // SAFETY: identical contract to aes_encrypt_block_rs above: `rk` has at least
    // 4*(nr+1) readable u32 words, `input` 16 readable bytes, `out` 16 writable
    // bytes. All accesses go through the slices / local `state`, bounds-checked by
    // Rust; no pointer is retained past this call.
    let (rks, inb, outb): (&[u32], &[u8], &mut [u8]) = unsafe {
        (
            core::slice::from_raw_parts(rk, 4 * (nr + 1)),
            core::slice::from_raw_parts(input, 16),
            core::slice::from_raw_parts_mut(out, 16),
        )
    };

    let mut state = [0u8; 16];
    state.copy_from_slice(inb);

    // Initial round key addition (last round key).
    aes_add_round_key(&mut state, rks, 4 * nr);

    // Main rounds (in reverse): nr-1 down to 1.
    let mut round = nr - 1;
    while round > 0 {
        aes_inv_shift_rows(&mut state);
        aes_inv_sub_bytes(&mut state);
        aes_add_round_key(&mut state, rks, 4 * round);
        aes_inv_mix_columns(&mut state);
        round -= 1;
    }

    // Final round.
    aes_inv_shift_rows(&mut state);
    aes_inv_sub_bytes(&mut state);
    aes_add_round_key(&mut state, rks, 0);

    outb.copy_from_slice(&state);
}
