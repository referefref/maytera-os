// rustkern/getrandom.rs - SYS_GETRANDOM: fill a user buffer with kernel CSPRNG
// output. (getrandom syscall, #no-ticket.)
//
// NEW kernel code, so Rust per the 2026-07-16 rule. There is no C twin and no
// strangler flag: syscall 448 never existed before this change, so there is no
// C to strangle. The handler is deliberately thin: all cryptographic work lives
// in crypto/csprng.c (the HMAC-DRBG, NIST SP 800-90A) and all user-memory
// safety lives in security/validate.c (copy_to_user, atomic entry-check + copy
// with an exception-table fixup) and in the argtab descriptor for number 448
// (rustkern/argtab.rs, wa(2): the [buf, buf+len) range is proven user-writable
// at entry). This file adds NEITHER a second RNG NOR a hand-rolled user copy;
// it wires the two together, which is the whole point of the syscall.
//
// WHY A BOUNCE BUFFER. csprng_bytes() writes to a kernel address. Handing it a
// user pointer directly would (a) run the DRBG's HMAC-SHA256 loop straight into
// user memory under Ring 0, defeating copy_to_user's fault containment, and (b)
// be unsound under SMAP. So the DRBG fills a small kernel stack buffer and
// copy_to_user moves each chunk out, chunked so the stack cost is fixed
// regardless of len. The bounce buffer is zeroed on every exit so DRBG output
// is never left in freed stack for a later frame to read.
//
// CONCURRENCY. csprng_bytes() mutates the global DRBG state (g_K/g_V) without a
// lock. That is safe here because every syscall body runs under the Big Kernel
// Lock (BKL), so two Ring-3 callers can never be inside csprng_bytes() at once;
// this introduces no race the existing /dev/urandom read path did not already
// have. If the BKL is ever narrowed off the syscall body, csprng_bytes() needs
// its own lock at that time (a note left in crypto/csprng.c is the right place),
// not a second RNG here.

use core::ffi::c_void;

extern "C" {
    // crypto/csprng.c. HMAC-DRBG generate; always succeeds, never blocks, seeds
    // itself lazily on first use. Reseeds itself on a call-count / tick budget.
    fn csprng_bytes(buf: *mut c_void, len: usize);
    // security/validate.c. Atomic entry-check-AND-copy into the caller's user
    // pages with an exception-table fixup; returns non-zero on fault.
    fn copy_to_user(dest: *mut c_void, src: *const c_void, size: usize) -> i32;
}

const EFAULT: i64 = -14;
const EINVAL: i64 = -22;

// getrandom() flag bits we recognise (sys/random.h). Our CSPRNG is always
// seeded and never blocks, so GRND_RANDOM and GRND_NONBLOCK do not change what
// we return; they are accepted for source compatibility. GRND_INSECURE likewise
// draws from the same DRBG. Any OTHER bit is rejected with EINVAL, matching
// Linux getrandom(2), so a caller passing garbage flags learns about it rather
// than silently getting bytes it did not ask the kernel to characterise.
const GRND_NONBLOCK: u64 = 0x0001;
const GRND_RANDOM: u64 = 0x0002;
const GRND_INSECURE: u64 = 0x0004;
const GRND_KNOWN: u64 = GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE;

// Upper bound on a single request. Linux caps a urandom read at 32 MiB per
// call; a MayteraOS request larger than 64 MiB is not a real one, and the fill
// loop is the one place an attacker-chosen length costs work per chunk, so it is
// rejected rather than serviced. The argtab descriptor already refuses to
// validate a range this large as user memory, but bounding it here too keeps the
// handler correct even if the descriptor is ever loosened.
const GETRANDOM_MAX: u64 = 64 * 1024 * 1024;

/// SYS_GETRANDOM(buf, len, flags). Fills up to `len` bytes of `buf` with CSPRNG
/// output. Returns the number of bytes written (== len on success), or a
/// negative errno. Standard getrandom(2) semantics for our always-ready source:
/// it fills the whole buffer in one call and never returns a short count.
#[no_mangle]
pub unsafe extern "C" fn sys_getrandom_rs(buf: *mut c_void, len: u64, flags: u64) -> i64 {
    if flags & !GRND_KNOWN != 0 {
        return EINVAL;
    }
    if len == 0 {
        return 0;
    }
    if len > GETRANDOM_MAX {
        return EINVAL;
    }

    let mut chunk = [0u8; 256];
    let mut done: u64 = 0;
    while done < len {
        let n = core::cmp::min((len - done) as usize, chunk.len());
        csprng_bytes(chunk.as_mut_ptr() as *mut c_void, n);
        let dst = (buf as *mut u8).add(done as usize) as *mut c_void;
        if copy_to_user(dst, chunk.as_ptr() as *const c_void, n) != 0 {
            zero(&mut chunk);
            return EFAULT;
        }
        done += n as u64;
    }
    zero(&mut chunk);
    len as i64
}

// Zero without being optimised away. write_volatile per byte is the no_std
// idiom; the buffer is tiny (256 bytes) so the cost is irrelevant next to not
// leaking DRBG output into a reused stack frame.
fn zero(b: &mut [u8]) {
    for x in b.iter_mut() {
        unsafe { core::ptr::write_volatile(x, 0) };
    }
}

/// Boot self-test (called from main.c, same family as kformat_selftest /
/// win16_localheap_selftest). Draws two 32-byte samples from the SAME source
/// SYS_GETRANDOM uses and returns the number of checks passed:
///   1. the first sample is not all-zero (the source produced something),
///   2. the first sample is not a single repeated byte (it has variety),
///   3. the two samples differ (successive draws are not identical).
/// The caller treats < 3 as FAIL. This proves the CSPRNG feeding the syscall is
/// live, non-constant and non-repeating AT RUNTIME on the machine that boots
/// this kernel, which is the property a build-time gate cannot show.
#[no_mangle]
pub extern "C" fn getrandom_selftest_rs() -> u32 {
    let mut a = [0u8; 32];
    let mut b = [0u8; 32];
    unsafe {
        csprng_bytes(a.as_mut_ptr() as *mut c_void, a.len());
        csprng_bytes(b.as_mut_ptr() as *mut c_void, b.len());
    }
    let mut checks = 0u32;
    if a.iter().any(|&x| x != 0) {
        checks += 1;
    }
    if a.iter().any(|&x| x != a[0]) {
        checks += 1;
    }
    if a != b {
        checks += 1;
    }
    zero(&mut a);
    zero(&mut b);
    checks
}
