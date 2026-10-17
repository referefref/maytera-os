// rustkern/sha1.rs - SHA-1 (FIPS 180-4 section 6.1) as a SHARED kernel
// primitive: the streaming init/update/final context, a one-shot, and the hash
// half of HMAC-SHA1 (the construction itself stays in rustkern/hmac.rs).
//
// READ THIS FIRST: SHA-1 IS COLLISION-BROKEN, AND THIS FILE IS NOT AN
// ENDORSEMENT OF IT.
// ---------------------------------------------------------------------------
// SHAttered (Stevens, Bursztein, Karpman, Albertini, Markov, 2017) produced a
// real colliding PDF pair, and chosen-prefix collisions followed ("SHA-1 is a
// Shambles", Leurent and Peyrin, 2020). Nothing in this system may use SHA-1
// for a signature, a certificate, a code-signing digest, a content address, or
// any other purpose where an attacker chooses part of the message. Those
// callers have rustkern/sha256.rs and rustkern/sha512.rs, and they must use
// them.
//
// This primitive exists for exactly one reason: RFC 6238 TOTP. RFC 6238 (via
// RFC 4226) specifies HMAC-SHA1 as the default and it is what every
// authenticator app a user already has on their phone actually speaks, so a
// kernel-side secrets store that cannot compute HMAC-SHA1 cannot verify the
// codes those apps generate. The choice is HMAC-SHA1 or no interoperable TOTP.
//
// The collision attacks do not break HMAC-SHA1. HMAC's security rests on the
// keyed compression function behaving as a PRF, not on collision resistance of
// the bare hash, and there is no practical forgery or key-recovery attack on
// HMAC-SHA1. "SHA-1 as a digest over attacker-chosen data" and "SHA-1 inside
// HMAC under a secret key" are two different questions, and only the second one
// is being answered yes here. If TOTP ever moves to HMAC-SHA256/512, both are
// already in the tree and this file loses its only reason to exist.
//
// WHY RUST, AND WHY THERE IS NO -DRUST_SHA1 STRANGLER FLAG
// ---------------------------------------------------------------------------
// New kernel code is Rust unless there is a stated performance reason
// (CLAUDE.md); there is none here, a TOTP step compresses two 64-byte blocks
// twice. More importantly this is NEW code, not a port: the kernel had no SHA-1
// at all (measured: `grep -rni sha1 kernel/` found one comment in
// net/ssh/ssh2_server.c about NOT supporting ssh-rsa). There is therefore no C
// twin to be a drop-in for, nothing for a -DRUST_SHA1 flag to switch between,
// and no [RUST-DIFF] differential to run, because a differential needs two
// implementations.
//
// What replaces the differential is a KNOWN-ANSWER TEST, wired in two places so
// that neither is the only one:
//   * sha1_selftest_rs() at the bottom of this file runs the published vectors
//     plus the padding-boundary and streaming cases on EVERY boot; main.c
//     prints the verdict to serial and to /BOOTLOG.TXT.
//   * kernel/crypto/sha1_hmac_kat.sh compiles THIS FILE for the host and
//     replays the full set including the one-million-'a' vector, and PROVES it
//     can go red by mutating a copy (a round constant, a rotate amount, and the
//     padding byte). It is picked up automatically by crypto/run-all-kats.sh.
//
// The padding boundary (a message whose length leaves fewer than 9 bytes in the
// final block, so the length field needs an extra block) is where hand-written
// SHA-1 goes wrong, and a bug there is invisible on "abc". Both harnesses cover
// 55, 56, 57, 63, 64 and 65 bytes for that reason.
//
// SOFT-FLOAT NOTE: integer only, no float anywhere, so the kernel's
// -mno-sse / soft-float target constraint is irrelevant to this file.

// ---------------------------------------------------------------------------
// The streaming context. Field ORDER is part of the FFI contract: the C mirror
// is sha1_ctx_t in crypto/sha1.h, which carries a _Static_assert pinning
// sizeof == 96. The layout is chosen to have no padding at all:
//     len      u64        8 bytes at offset 0  (align 8 satisfied)
//     h        [u32; 5]  20 bytes at offset 8
//     buf_len  u32        4 bytes at offset 28
//     buf      [u8; 64]  64 bytes at offset 32
//   total 96, align 8, no interior or tail padding.
// `len` counts MESSAGE BYTES consumed (not bits, not blocks); the bit length
// written into the padding is len * 8 as a big-endian u64, per FIPS 180-4.
// ---------------------------------------------------------------------------
#[repr(C)]
pub struct Sha1Ctx {
    len: u64,
    h: [u32; 5],
    buf_len: u32,
    buf: [u8; 64],
}

// The layout lock, Rust side. The C side pins the same number independently in
// crypto/sha1.h, so a field reordering here fails the Rust compile and a
// divergence between the two fails the C compile. One assert on one side would
// only be half a lock.
const _: () = assert!(core::mem::size_of::<Sha1Ctx>() == 96);
const _: () = assert!(core::mem::align_of::<Sha1Ctx>() == 8);

// FIPS 180-4 section 5.3.1 initial hash value H(0).
const SHA1_H0: [u32; 5] = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0];

const SHA1_BLOCK: usize = 64;
const SHA1_DIGEST: usize = 20;

/// One 512-bit block through the FIPS 180-4 section 6.1.2 compression function.
/// Takes the block by fixed-size reference, so there is no length to get wrong
/// and no bounds check to elide: every index below is provably in range.
/// `wrapping_add` everywhere because the spec is mod 2^32 arithmetic, and
/// `rotate_left` for ROTL; neither can panic.
fn sha1_compress(h: &mut [u32; 5], block: &[u8; SHA1_BLOCK]) {
    // Message schedule W[0..80]: the first 16 words are the block read
    // big-endian, the rest are the XOR feedback rotated left by one.
    let mut w = [0u32; 80];
    let mut i = 0usize;
    while i < 16 {
        w[i] = u32::from_be_bytes([
            block[i * 4],
            block[i * 4 + 1],
            block[i * 4 + 2],
            block[i * 4 + 3],
        ]);
        i += 1;
    }
    while i < 80 {
        w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        i += 1;
    }

    let mut a = h[0];
    let mut b = h[1];
    let mut c = h[2];
    let mut d = h[3];
    let mut e = h[4];

    i = 0;
    while i < 80 {
        // The four 20-round groups: Ch, Parity, Maj, Parity with their K.
        // Written as literals rather than named constants used as match
        // patterns: blame.md records a Rust `const` in a match pattern
        // silently becoming a BINDING, which matches everything.
        let (f, k) = if i < 20 {
            ((b & c) | ((!b) & d), 0x5a827999u32)
        } else if i < 40 {
            (b ^ c ^ d, 0x6ed9eba1u32)
        } else if i < 60 {
            ((b & c) | (b & d) | (c & d), 0x8f1bbcdcu32)
        } else {
            (b ^ c ^ d, 0xca62c1d6u32)
        };
        let t = a
            .rotate_left(5)
            .wrapping_add(f)
            .wrapping_add(e)
            .wrapping_add(k)
            .wrapping_add(w[i]);
        e = d;
        d = c;
        c = b.rotate_left(30);
        b = a;
        a = t;
        i += 1;
    }

    h[0] = h[0].wrapping_add(a);
    h[1] = h[1].wrapping_add(b);
    h[2] = h[2].wrapping_add(c);
    h[3] = h[3].wrapping_add(d);
    h[4] = h[4].wrapping_add(e);

    // The schedule holds message material; wipe it rather than leave 320 bytes
    // of it in the frame the next callee reuses.
    sha1_zeroize_words(&mut w);
}

/// Volatile zeroing so the compiler cannot elide the wipe. Same idiom as
/// hmac.rs hmac_zeroize (see its comment); duplicated as a words/bytes pair
/// rather than made generic to keep this file free of trait machinery.
#[inline]
fn sha1_zeroize_words(b: &mut [u32]) {
    for x in b.iter_mut() {
        // SAFETY: `x` is a valid, uniquely borrowed, aligned u32 inside `b`.
        unsafe {
            core::ptr::write_volatile(x, 0u32);
        }
    }
}

#[inline]
fn sha1_zeroize_bytes(b: &mut [u8]) {
    for x in b.iter_mut() {
        // SAFETY: `x` is a valid, uniquely borrowed, aligned u8 inside `b`.
        unsafe {
            core::ptr::write_volatile(x, 0u8);
        }
    }
}

/// Absorb a byte slice into the context. The whole of the streaming logic lives
/// here; sha1_update() is the FFI wrapper. Splitting it this way means
/// sha1_final() can reuse the identical buffering path for the padding instead
/// of open-coding a second, subtly different one, which is where the classic
/// "works for short messages" padding bug comes from.
fn sha1_absorb(c: &mut Sha1Ctx, src: &[u8]) {
    c.len = c.len.wrapping_add(src.len() as u64);

    // Defensive clamp: buf_len is part of a caller-owned struct, so a caller
    // that hands us an uninitialised or corrupted ctx must not be able to turn
    // that into an out-of-range slice index. Clamping keeps the digest wrong
    // (garbage in, garbage out) but keeps memory safe.
    let mut bl = c.buf_len as usize;
    if bl > SHA1_BLOCK {
        bl = SHA1_BLOCK;
    }

    let mut off = 0usize;
    if bl > 0 {
        let take = core::cmp::min(SHA1_BLOCK - bl, src.len());
        c.buf[bl..bl + take].copy_from_slice(&src[..take]);
        bl += take;
        off = take;
        if bl == SHA1_BLOCK {
            let block = c.buf;
            sha1_compress(&mut c.h, &block);
            bl = 0;
        }
    }

    // Whole blocks straight out of the caller's buffer. chunks_exact yields
    // only full 64-byte chunks and hands the tail back via remainder(), so
    // there is no arithmetic here that can run off the end.
    let mut it = src[off..].chunks_exact(SHA1_BLOCK);
    for ch in &mut it {
        let mut block = [0u8; SHA1_BLOCK];
        block.copy_from_slice(ch);
        sha1_compress(&mut c.h, &block);
    }

    let rem = it.remainder();
    if !rem.is_empty() {
        c.buf[..rem.len()].copy_from_slice(rem);
        bl = rem.len();
    }
    c.buf_len = bl as u32;
}

/// SHA-1 init. `ctx` is a caller-owned sha1_ctx_t (96 bytes, 8-aligned).
///
/// # Safety
/// `ctx` must be null or point to at least 96 writable bytes aligned to 8, for
/// the duration of the call. A null pointer is a no-op rather than a Ring-0
/// dereference, because this is reachable from glue that may itself be C.
#[no_mangle]
pub extern "C" fn sha1_init(ctx: *mut u8) {
    if ctx.is_null() {
        return;
    }
    // SAFETY: non-null and, per the contract above, a valid 96-byte 8-aligned
    // sha1_ctx_t. The reference does not outlive this call and nothing else
    // aliases it (the caller owns the struct).
    let c = unsafe { &mut *(ctx as *mut Sha1Ctx) };
    c.len = 0;
    c.h = SHA1_H0;
    c.buf_len = 0;
    c.buf = [0u8; SHA1_BLOCK];
}

/// SHA-1 update: absorb `len` bytes at `data`.
///
/// # Safety
/// `ctx` obeys the sha1_init contract and has been initialised by sha1_init.
/// `data` must point to `len` readable bytes (it is ignored when `len` is 0).
/// Neither pointer is retained past this call.
#[no_mangle]
pub extern "C" fn sha1_update(ctx: *mut u8, data: *const u8, len: usize) {
    if ctx.is_null() || len == 0 || data.is_null() {
        return;
    }
    // SAFETY: both pointers are non-null; the caller guarantees the ctx extent
    // (96 bytes) and that `data` has `len` readable bytes. From here on every
    // access goes through the slice/reference, so it is bounds-checked.
    let c = unsafe { &mut *(ctx as *mut Sha1Ctx) };
    let src = unsafe { core::slice::from_raw_parts(data, len) };
    sha1_absorb(c, src);
}

/// SHA-1 final: pad per FIPS 180-4 section 5.1.1 and write the 20-byte digest.
///
/// # Safety
/// `ctx` obeys the sha1_init contract; `out` must point to at least 20 writable
/// bytes. The context's message buffer is wiped before returning.
#[no_mangle]
pub extern "C" fn sha1_final(ctx: *mut u8, out: *mut u8) {
    if ctx.is_null() || out.is_null() {
        return;
    }
    // SAFETY: as sha1_update, plus `out` has 20 writable bytes per the contract.
    let c = unsafe { &mut *(ctx as *mut Sha1Ctx) };
    let dst = unsafe { core::slice::from_raw_parts_mut(out, SHA1_DIGEST) };

    // The bit length is of the MESSAGE, so it must be read before the padding
    // is absorbed (absorbing advances c.len).
    let bits = c.len.wrapping_mul(8);

    // 0x80, then zeros until the block has 56 bytes, then the 8-byte
    // big-endian bit length. At most 1 + 63 + 8 = 72 bytes, so one 128-byte
    // scratch covers every case including the one where a second block is
    // needed (buf_len >= 56).
    let bl = core::cmp::min(c.buf_len as usize, SHA1_BLOCK);
    let zeros = (SHA1_BLOCK + 56 - (bl + 1) % SHA1_BLOCK) % SHA1_BLOCK;
    let mut pad = [0u8; 128];
    pad[0] = 0x80;
    let lenpos = 1 + zeros;
    pad[lenpos..lenpos + 8].copy_from_slice(&bits.to_be_bytes());
    sha1_absorb(c, &pad[..lenpos + 8]);

    let mut i = 0usize;
    while i < 5 {
        dst[i * 4..i * 4 + 4].copy_from_slice(&c.h[i].to_be_bytes());
        i += 1;
    }

    // The tail buffer can hold message bytes, and under HMAC those bytes are
    // the key XOR ipad. Wipe it. `h` is not wiped: after final it IS the
    // digest, which the caller is being handed anyway.
    sha1_zeroize_bytes(&mut c.buf);
    c.buf_len = 0;
}

/// One-shot SHA-1 over `len` bytes at `data` into 20 bytes at `out`. Same shape
/// as the kernel's md5()/sha256() one-shots so call sites read the same.
///
/// # Safety
/// `data` must point to `len` readable bytes (ignored when `len` is 0) and
/// `out` to 20 writable bytes.
#[no_mangle]
pub extern "C" fn sha1(data: *const u8, len: usize, out: *mut u8) {
    let mut ctx = Sha1Ctx {
        len: 0,
        h: SHA1_H0,
        buf_len: 0,
        buf: [0u8; SHA1_BLOCK],
    };
    let p = &mut ctx as *mut Sha1Ctx as *mut u8;
    sha1_init(p);
    // SAFETY: forwarding the caller's own contract for `data`/`len` unchanged;
    // `p` is our own stack ctx, which is exactly 96 bytes and 8-aligned.
    sha1_update(p, data, len);
    sha1_final(p, out);
}

// ===========================================================================
// Boot self-test (called from main.c; main.c prints the verdict to serial AND
// to /BOOTLOG.TXT, because serial is silent in GUI mode and absent entirely on
// the two real machines whose evidence matters).
//
// This is a KNOWN-ANSWER test, not a differential: there is no C twin (see the
// file header). It deliberately runs the cases that a SHA-1 bug hides in:
//   * the published RFC 3174 / FIPS 180-2 vectors, so the algorithm itself is
//     pinned to the standard rather than to our own output;
//   * empty input, which exercises "pad a block that has no message in it";
//   * 55, 56, 57, 63, 64 and 65 bytes, the length-padding boundary, where 56
//     and 63 force the length field into a SECOND block;
//   * the same message fed in several update() calls, which is the only thing
//     that proves the streaming buffer rather than the compressor;
//   * HMAC-SHA1 RFC 2202 TC1/TC2/TC6/TC7 through hmac_sha1_rs, TC6/TC7 being
//     the cases whose key is longer than the block (the K' = H(key) branch).
// The one-million-'a' vector is NOT here: it is ~15625 compressions and this
// runs on every boot of every machine. crypto/sha1_hmac_kat.sh runs it on the
// host, where it costs nothing anyone is waiting for.
//
// Returns the number of FAILED checks (0 = pass), or -1 if zero checks ran,
// which is the right way round: a PASS with no checks is vacuous and that is
// the failure mode a self-test nobody has watched go red actually has.
// ===========================================================================

extern "C" {
    // The HMAC-SHA1 one-shot (rustkern/hmac.rs), which drives the three
    // functions above through the shared RFC 2104 construction.
    fn hmac_sha1_rs(key: *const u8, key_len: usize, data: *const u8, data_len: usize, mac: *mut u8);
}

#[inline]
fn sha1_nib(ch: u8) -> u8 {
    if ch >= b'a' {
        ch - b'a' + 10
    } else {
        ch - b'0'
    }
}

/// Compare a 20-byte digest against a 40-character lowercase hex literal.
fn sha1_hexeq(d: &[u8; SHA1_DIGEST], hex: &[u8; 40]) -> bool {
    let mut i = 0usize;
    while i < SHA1_DIGEST {
        let want = (sha1_nib(hex[i * 2]) << 4) | sha1_nib(hex[i * 2 + 1]);
        if d[i] != want {
            return false;
        }
        i += 1;
    }
    true
}

/// One-shot helper over a slice, for the self-test's own use.
fn sha1_of(msg: &[u8]) -> [u8; SHA1_DIGEST] {
    let mut out = [0u8; SHA1_DIGEST];
    sha1(msg.as_ptr(), msg.len(), out.as_mut_ptr());
    out
}

#[no_mangle]
pub extern "C" fn sha1_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut checks = 0u32;
    let mut fails = 0i32;

    // Bounded scratch: 200 bytes is the largest buffer here on purpose. A
    // 16 KB array in a Rust self-test once overflowed a kernel stack
    // (blame.md, #212), and this runs on the boot path.
    let mut msg = [0u8; 200];

    // ---- 1. Published vectors (FIPS 180-2 / RFC 3174). ----
    {
        checks += 1;
        if !sha1_hexeq(&sha1_of(b"abc"), b"a9993e364706816aba3e25717850c26c9cd0d89d") {
            fails += 1;
        }
        checks += 1;
        if !sha1_hexeq(&sha1_of(b""), b"da39a3ee5e6b4b0d3255bfef95601890afd80709") {
            fails += 1;
        }
        // The 448-bit ("abcdbcde...") vector: 56 bytes, so the length field
        // lands in a second block. This one IS published, which is why it is
        // the boundary case worth having in the kernel.
        checks += 1;
        if !sha1_hexeq(
            &sha1_of(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            b"84983e441c3bd26ebaae4aa1f95129e5e54670f1",
        ) {
            fails += 1;
        }
    }

    // ---- 2. Padding boundary: 55/56/57/63/64/65 bytes of 'a'. These
    // expectations are not in a standard; they were reproduced with an
    // independent implementation (Python hashlib) and are stated as such. A
    // correct SHA-1 is the only thing that produces all of them. ----
    {
        let cases: [(usize, &[u8; 40]); 6] = [
            (55, b"c1c8bbdc22796e28c0e15163d20899b65621d65a"),
            (56, b"c2db330f6083854c99d4b5bfb6e8f29f201be699"),
            (57, b"f08f24908d682555111be7ff6f004e78283d989a"),
            (63, b"03f09f5b158a7a8cdad920bddc29b81c18a551f5"),
            (64, b"0098ba824b5c16427bd7a1122a5a442a25ec644d"),
            (65, b"11655326c708d70319be2610e8a57d9a5b959d3b"),
        ];
        for &(n, want) in cases.iter() {
            let mut i = 0usize;
            while i < n {
                msg[i] = b'a';
                i += 1;
            }
            checks += 1;
            if !sha1_hexeq(&sha1_of(&msg[..n]), want) {
                fails += 1;
            }
        }
    }

    // ---- 3. Streaming: the same 56-byte published message split across
    // several update() calls in awkward sizes must give the same digest. This
    // is the only check that tests the buffering rather than the compressor. ----
    {
        let m: &[u8] = b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        for step in [1usize, 2, 3, 7, 31, 55, 64].iter() {
            let mut ctx = Sha1Ctx {
                len: 0,
                h: SHA1_H0,
                buf_len: 0,
                buf: [0u8; SHA1_BLOCK],
            };
            let p = &mut ctx as *mut Sha1Ctx as *mut u8;
            sha1_init(p);
            let mut off = 0usize;
            while off < m.len() {
                let take = core::cmp::min(*step, m.len() - off);
                sha1_update(p, m[off..].as_ptr(), take);
                off += take;
            }
            let mut d = [0u8; SHA1_DIGEST];
            sha1_final(p, d.as_mut_ptr());
            checks += 1;
            if !sha1_hexeq(&d, b"84983e441c3bd26ebaae4aa1f95129e5e54670f1") {
                fails += 1;
            }
        }
    }

    // ---- 4. Multi-block streaming over 200 pseudo-random bytes, one-shot vs
    // 17-byte chunks, both against an independently computed expectation. ----
    {
        let mut i = 0usize;
        while i < 200 {
            msg[i] = ((i * 7 + 3) % 256) as u8;
            i += 1;
        }
        let want = b"892b673ca3c696ab13ab8aab3cf3abfbc3aaeb3b";
        checks += 1;
        if !sha1_hexeq(&sha1_of(&msg[..200]), want) {
            fails += 1;
        }

        let mut ctx = Sha1Ctx {
            len: 0,
            h: SHA1_H0,
            buf_len: 0,
            buf: [0u8; SHA1_BLOCK],
        };
        let p = &mut ctx as *mut Sha1Ctx as *mut u8;
        sha1_init(p);
        let mut off = 0usize;
        while off < 200 {
            let take = core::cmp::min(17usize, 200 - off);
            sha1_update(p, msg[off..].as_ptr(), take);
            off += take;
        }
        let mut d = [0u8; SHA1_DIGEST];
        sha1_final(p, d.as_mut_ptr());
        checks += 1;
        if !sha1_hexeq(&d, want) {
            fails += 1;
        }
    }

    // ---- 5. HMAC-SHA1, RFC 2202 TC1/TC2/TC6/TC7. TC6 and TC7 use an 80-byte
    // key, longer than the 64-byte block, so they are the cases that exercise
    // the K' = H(key) branch of hmac_construct. This is also the only place in
    // the kernel that proves the shared HMAC construction drives THIS hash
    // correctly. ----
    {
        let mut mac = [0u8; SHA1_DIGEST];
        let key20 = [0x0bu8; 20];
        let key80 = [0xaau8; 80];
        let jefe = b"Jefe";
        let d1 = b"Hi There";
        let d2 = b"what do ya want for nothing?";
        let d6 = b"Test Using Larger Than Block-Size Key - Hash Key First";
        let d7 = b"Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data";

        // SAFETY: every call passes a real key/data extent and a 20-byte mac
        // buffer, which is hmac_sha1_rs's documented contract.
        unsafe {
            hmac_sha1_rs(key20.as_ptr(), 20, d1.as_ptr(), d1.len(), mac.as_mut_ptr());
        }
        checks += 1;
        if !sha1_hexeq(&mac, b"b617318655057264e28bc0b6fb378c8ef146be00") {
            fails += 1;
        }

        // SAFETY: as above.
        unsafe {
            hmac_sha1_rs(jefe.as_ptr(), 4, d2.as_ptr(), d2.len(), mac.as_mut_ptr());
        }
        checks += 1;
        if !sha1_hexeq(&mac, b"effcdf6ae5eb2fa2d27416d5f184df9c259a7c79") {
            fails += 1;
        }

        // SAFETY: as above.
        unsafe {
            hmac_sha1_rs(key80.as_ptr(), 80, d6.as_ptr(), d6.len(), mac.as_mut_ptr());
        }
        checks += 1;
        if !sha1_hexeq(&mac, b"aa4ae5e15272d00e95705637ce8a3b55ed402112") {
            fails += 1;
        }

        // SAFETY: as above.
        unsafe {
            hmac_sha1_rs(key80.as_ptr(), 80, d7.as_ptr(), d7.len(), mac.as_mut_ptr());
        }
        checks += 1;
        if !sha1_hexeq(&mac, b"e8e99d0f45237d786d6bbaa7965c7808bbff1a91") {
            fails += 1;
        }
    }

    sha1_zeroize_bytes(&mut msg);

    if !out_checks.is_null() {
        // SAFETY: non-null and, per the C declaration in main.c, a writable u32.
        unsafe {
            *out_checks = checks;
        }
    }
    if checks == 0 {
        return -1;
    }
    fails
}
