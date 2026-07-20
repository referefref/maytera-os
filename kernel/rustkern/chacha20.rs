// rustkern/chacha20.rs - #404 Phase I / #491 ChaCha20 block (keystream) core
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// Phase I port (#404 / #491): ChaCha20 block (keystream) core.
//
// Faithful, memory-safe Rust drop-in for the pure 20-round (10 double-round)
// ChaCha20 block function of crypto/chacha20.c (chacha20_block_c). It consumes
// ONLY the 16-word (u32) input state and writes the 64-byte serialized keystream
// block, so the chacha20_ctx_t struct never crosses the FFI boundary:
// init / setkey / counter-increment / XOR-stream all stay in C and pass
// ctx->state. crypto/chacha20.c routes the live block core here under
// -DRUST_CHACHA20 (else chacha20_block_c).
//
// FAITHFULNESS to the C (RFC 8439 section 2.3): the working state is the input
// copied verbatim; ten identical double-rounds each apply the four COLUMN
// quarter-rounds (0,4,8,12),(1,5,9,13),(2,6,10,14),(3,7,11,15) then the four
// DIAGONAL quarter-rounds (0,5,10,15),(1,6,11,12),(2,7,8,13),(3,4,9,14); the
// original input words are added back; and the 16 result words are serialized
// LITTLE-endian to the 64 output bytes (matching the C store32_le loop, and the
// same little-endian word convention the C QUARTERROUND / load32_le use). The
// QUARTERROUND is a += b; d ^= a; d = ROTL(d,16); c += d; b ^= c; b = ROTL(b,12);
// a += b; d ^= a; d = ROTL(d,8); c += d; b ^= c; b = ROTL(b,7) - every add
// wrapping at 32 bits exactly as C unsigned overflow does, every ROTL a
// rotate_left. Boot-time chacha20_rust_selftest proves it against the RFC 8439
// section 2.3.2 known-answer keystream block through the live chacha20 API plus a
// large random-state differential vs chacha20_block_c.
#[no_mangle]
pub extern "C" fn chacha20_block_rs(input: *const u32, out: *mut u8) {
    // SAFETY: The caller (crypto/chacha20.c block-core dispatcher) guarantees
    // `input` points to exactly 16 readable u32 (ctx->state, the ChaCha20 input
    // block) and `out` to exactly 64 writable bytes (the keystream block). We
    // build one slice over each of those exact extents and from here on touch
    // memory ONLY through them, so every access is bounds-checked by Rust: reads
    // never exceed the 16 input words and writes never exceed the 64 output bytes.
    // Neither pointer is retained past this call.
    let (inp, ob): (&[u32], &mut [u8]) = unsafe {
        (
            core::slice::from_raw_parts(input, 16),
            core::slice::from_raw_parts_mut(out, 64),
        )
    };

    // Working state = input copied verbatim (C: output[i] = input[i]).
    let mut x = [0u32; 16];
    let mut i = 0usize;
    while i < 16 {
        x[i] = inp[i];
        i += 1;
    }

    // ChaCha20 quarter-round on x[a],x[b],x[c],x[d] (RFC 8439). Identical to the
    // C QUARTERROUND macro: wrapping adds, rotate_left by 16/12/8/7.
    macro_rules! qr {
        ($a:expr, $b:expr, $c:expr, $d:expr) => {
            x[$a] = x[$a].wrapping_add(x[$b]); x[$d] ^= x[$a]; x[$d] = x[$d].rotate_left(16);
            x[$c] = x[$c].wrapping_add(x[$d]); x[$b] ^= x[$c]; x[$b] = x[$b].rotate_left(12);
            x[$a] = x[$a].wrapping_add(x[$b]); x[$d] ^= x[$a]; x[$d] = x[$d].rotate_left(8);
            x[$c] = x[$c].wrapping_add(x[$d]); x[$b] ^= x[$c]; x[$b] = x[$b].rotate_left(7);
        };
    }

    // 20 rounds = 10 double-rounds (4 column quarter-rounds then 4 diagonal),
    // in the exact order of the C chacha20_block_internal loop.
    let mut r = 0usize;
    while r < 10 {
        qr!(0, 4, 8, 12);
        qr!(1, 5, 9, 13);
        qr!(2, 6, 10, 14);
        qr!(3, 7, 11, 15);
        qr!(0, 5, 10, 15);
        qr!(1, 6, 11, 12);
        qr!(2, 7, 8, 13);
        qr!(3, 4, 9, 14);
        r += 1;
    }

    // Add the original input words back, then serialize LITTLE-endian to bytes
    // (C: output[i] += input[i]; store32_le(out + i*4, output[i])).
    let mut i = 0usize;
    while i < 16 {
        let v = x[i].wrapping_add(inp[i]);
        ob[i * 4] = (v & 0xff) as u8;
        ob[i * 4 + 1] = ((v >> 8) & 0xff) as u8;
        ob[i * 4 + 2] = ((v >> 16) & 0xff) as u8;
        ob[i * 4 + 3] = ((v >> 24) & 0xff) as u8;
        i += 1;
    }
}
