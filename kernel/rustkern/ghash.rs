// rustkern/ghash.rs - #615 Phase: GHASH (GF(2^128) multiply) for AES-GCM.
//
// WHY THIS EXISTS (measured, not assumed). Build 975 instrumented the App Store
// download path with rdtsc region counters. Over a 103,563,185-byte OpenArena
// install on VM <vmid>, every 2-second sample during the download read:
//
//   [DLPROF] ... gcm=42%/3549KB gh=40%/228130 poll=2%/332 tcprx=0% ...
//
// i.e. AES-GCM was 42% of one core and the bit-serial GHASH inside it was 40%,
// while the App Store process itself was measured at 41-51% of that core. In
// other words essentially ALL of the CPU the installer burned during the
// download was one function: crypto/crypto.c's ghash_multiply(), which is the
// textbook shift-and-XOR form - 128 iterations of a 16-byte loop for every 16
// bytes of TLS record. At ~8,500 cycles per 16-byte block that is ~530 cycles
// per byte, and it capped the download at ~1.65 MB/s on an idle gigabit LAN.
//
// THE ALGORITHM. This is the standard 4-bit (Shoup) table method: precompute
// H * n for each 4-bit nibble value n into two u64 tables, then reduce the
// multiply to 32 nibble steps of a handful of 64-bit shifts and XORs. Same
// field, same bit order, identical output; roughly an order of magnitude less
// work. The 128-bit field element is held as two big-endian u64 halves exactly
// as the byte-serial C holds its 16-byte array, and "multiply by x" in GCM's
// reversed bit order is the right-shift-with-0xe1-reduction that the C does
// byte-wise.
//
// TRUST. The C bit-serial ghash_multiply_c() is KEPT and is the reference: this
// is a genuinely INDEPENDENT algorithm, not a transliteration, so the boot-time
// ghash_rust_selftest() differential in crypto/crypto.c is a real cross-check
// and not the both-arms-share-the-bug trap. It runs 4,096 random (H, X) pairs
// plus the NIST GCM test vectors through both and refuses to agree quietly.
// Drop -DRUST_GHASH from the Makefile to roll straight back to the C.

/// Precomputed per-key GHASH tables. 16 entries of a 128-bit field element,
/// split into big-endian high/low u64 halves. Layout is locked on the C side
/// with a _Static_assert (crypto/crypto.h).
#[repr(C)]
pub struct GhashKey {
    hh: [u64; 16],
    hl: [u64; 16],
}

// Reduction lookup for the low nibble shifted out by each 4-bit step. Entry n
// is the 16-bit reduction polynomial contribution of nibble n, positioned so it
// XORs into the top 16 bits of the high half.
const LAST4: [u64; 16] = [
    0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
    0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0,
];

#[inline(always)]
fn be64(b: &[u8; 16], off: usize) -> u64 {
    let mut v: u64 = 0;
    let mut i = 0;
    while i < 8 {
        v = (v << 8) | b[off + i] as u64;
        i += 1;
    }
    v
}

#[inline(always)]
fn put_be64(v: u64, b: &mut [u8; 16], off: usize) {
    let mut i = 0;
    while i < 8 {
        b[off + i] = (v >> (56 - 8 * i)) as u8;
        i += 1;
    }
}

fn key_init(k: &mut GhashKey, h: &[u8; 16]) {
    let mut vh = be64(h, 0);
    let mut vl = be64(h, 8);

    k.hh[0] = 0;
    k.hl[0] = 0;
    k.hh[8] = vh;   // H * 1
    k.hl[8] = vl;

    // H * x, H * x^2, H * x^3 (one right-shift-with-reduction each).
    let mut i = 4usize;
    while i > 0 {
        let t = if (vl & 1) != 0 { 0xe100_0000_0000_0000u64 } else { 0 };
        vl = (vh << 63) | (vl >> 1);
        vh = (vh >> 1) ^ t;
        k.hh[i] = vh;
        k.hl[i] = vl;
        i >>= 1;
    }

    // Every remaining nibble value is the XOR of the power-of-two entries.
    let mut i = 2usize;
    while i <= 8 {
        let bh = k.hh[i];
        let bl = k.hl[i];
        let mut j = 1usize;
        while j < i {
            k.hh[i + j] = bh ^ k.hh[j];
            k.hl[i + j] = bl ^ k.hl[j];
            j += 1;
        }
        i *= 2;
    }
}

fn mul(k: &GhashKey, x: &mut [u8; 16]) {
    let lo0 = (x[15] & 0x0f) as usize;
    let mut zh = k.hh[lo0];
    let mut zl = k.hl[lo0];

    let mut i: isize = 15;
    while i >= 0 {
        let b = x[i as usize];
        let lo = (b & 0x0f) as usize;
        let hi = ((b >> 4) & 0x0f) as usize;

        if i != 15 {
            let rem = (zl & 0x0f) as usize;
            zl = (zh << 60) | (zl >> 4);
            zh >>= 4;
            zh ^= LAST4[rem] << 48;
            zh ^= k.hh[lo];
            zl ^= k.hl[lo];
        }

        let rem = (zl & 0x0f) as usize;
        zl = (zh << 60) | (zl >> 4);
        zh >>= 4;
        zh ^= LAST4[rem] << 48;
        zh ^= k.hh[hi];
        zl ^= k.hl[hi];

        i -= 1;
    }

    put_be64(zh, x, 0);
    put_be64(zl, x, 8);
}

// ---------------------------------------------------------------------------
// C FFI. Pointers come from the C AES-GCM context; every one is dereferenced
// exactly once per call and no length is taken from the wire.
// ---------------------------------------------------------------------------

/// Build the per-key nibble tables from the 16-byte hash subkey H.
///
/// # Safety
/// `key` must point to a writable `GhashKey` (256 bytes) and `h` to 16
/// readable bytes. Both are fields of the caller's `aes_gcm_ctx_t`.
#[no_mangle]
pub unsafe extern "C" fn ghash_key_init_rs(key: *mut GhashKey, h: *const u8) {
    if key.is_null() || h.is_null() {
        return;
    }
    let mut hb = [0u8; 16];
    let mut i = 0;
    while i < 16 {
        hb[i] = *h.add(i);
        i += 1;
    }
    key_init(&mut *key, &hb);
}

/// x = x * H in GF(2^128), in place.
///
/// # Safety
/// `key` must point to a `GhashKey` initialised by `ghash_key_init_rs`, and
/// `x` to 16 readable+writable bytes.
#[no_mangle]
pub unsafe extern "C" fn ghash_mul_rs(key: *const GhashKey, x: *mut u8) {
    if key.is_null() || x.is_null() {
        return;
    }
    let mut b = [0u8; 16];
    let mut i = 0;
    while i < 16 {
        b[i] = *x.add(i);
        i += 1;
    }
    mul(&*key, &mut b);
    let mut i = 0;
    while i < 16 {
        *x.add(i) = b[i];
        i += 1;
    }
}

/// s = GHASH(s, data) over `len` bytes: for each whole 16-byte block XOR it
/// into s and multiply by H; a trailing partial block is XORed over its own
/// length only (zero padding) and then multiplied. Byte-for-byte the same
/// contract as the C `ghash_update_c()`.
///
/// # Safety
/// `key` must be initialised, `s` must point to 16 readable+writable bytes and
/// `data` to `len` readable bytes (`data` may be null only when `len` is 0).
#[no_mangle]
pub unsafe extern "C" fn ghash_update_rs(key: *const GhashKey, s: *mut u8,
                                         data: *const u8, len: usize) {
    if key.is_null() || s.is_null() {
        return;
    }
    let k = &*key;
    let mut st = [0u8; 16];
    let mut i = 0;
    while i < 16 {
        st[i] = *s.add(i);
        i += 1;
    }

    let mut off = 0usize;
    while len - off >= 16 {
        let mut j = 0;
        while j < 16 {
            st[j] ^= *data.add(off + j);
            j += 1;
        }
        mul(k, &mut st);
        off += 16;
    }
    if off < len {
        let rem = len - off;
        let mut j = 0;
        while j < rem {
            st[j] ^= *data.add(off + j);
            j += 1;
        }
        mul(k, &mut st);
    }

    let mut i = 0;
    while i < 16 {
        *s.add(i) = st[i];
        i += 1;
    }
}
