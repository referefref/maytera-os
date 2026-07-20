// arena_rs.rs - MayteraOS USERLAND (Ring-3) Rust bootstrap for Maytera Arena.
//
// Task #491 Stage 0: get Rust into the Arena userland app build reproducibly
// with ZERO functional change, proven end to end, then STOP. This is the exact
// userland analog of the kernel Rust port's Phase A (#478): a real Rust object
// is compiled for the USERLAND ABI and linked into the Arena ELF, exercising
// no_std + alloc + FFI, but changing NO game behavior.
//
// BUILD (pinned, see rust-toolchain.toml -> rustc 1.97.0):
//   rustc --edition 2021 --crate-type staticlib --target x86_64-unknown-none \
//         -C opt-level=2 -C panic=abort \
//         -C code-model=large -C relocation-model=static
//
// WHY THESE FLAGS (userland ABI, NOT the kernel ABI):
//   * target x86_64-unknown-none  : freestanding, precompiled `core` + `alloc`
//     ship for it, no build-std needed (mirrors the kernel port's simplicity).
//   * -C code-model=large         : the Arena C app is built -mcmodel=large and
//     LINKED AT 0x80000000 (user.ld). The default/small code model would emit
//     R_X86_64_32S relocations that OVERFLOW at 0x80000000 (2^31 does not fit a
//     signed 32-bit field) -> "relocation truncated to fit R_X86_64_32S". Large
//     uses movabs / R_X86_64_64 (no truncation), exactly like the C build.
//   * -C relocation-model=static  : the app is -fno-pic/-fno-pie, statically
//     linked. Match it so no GOT/PLT indirection is introduced.
//   * -C panic=abort              : no unwinding tables, no eh_personality.
//   NOTE on float model: the precompiled `core` for x86_64-unknown-none is
//   built soft-float while the Arena C is -msse/-msse2 (hardware float). Stage 0
//   crosses the FFI boundary with INTEGERS ONLY (u32), so the float ABI is never
//   exercised and the integer ABI is identical. Stage 1 (BSP geometry, which
//   passes f32 across FFI) must revisit this: either keep the FFI integer/
//   bit-pattern only, or move to a custom target JSON + `-Zbuild-std` compiled
//   with +sse so the hardware-float ABI matches. Documented in ARENA_BSP_PLAN.md.

#![no_std]

extern crate alloc;

// #491 Stage 1: the GoldSrc BSP v30 parser + WAD3 texture decoder. Compiled as
// part of THIS crate (single crate root, `rustc arena_rs.rs`), so `mod bsp;`
// pulls in bsp.rs. It shares this file's #[global_allocator] + #[panic_handler].
mod bsp;

use core::alloc::{GlobalAlloc, Layout};
use core::panic::PanicInfo;
use alloc::vec::Vec;

// -- userland libc FFI (ONE shared heap) -----------------------------------
// The Rust global allocator wraps the SAME userland libc heap the C app uses
// (stdlib.c malloc/free/realloc). Rust and C therefore share ONE heap, exactly
// like the kernel port wraps kmalloc/kfree. malloc() is documented to return
// 16-byte-aligned blocks (stdlib.c), which satisfies every alignment Stage 0/1
// data types need; the >16 path below is a safety net for over-aligned types.
extern "C" {
    fn malloc(size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
    fn realloc(ptr: *mut u8, size: usize) -> *mut u8;
    // Never returns; the userland libc abort() terminates THIS Ring-3 process
    // (NOT the kernel). Used by the panic handler so a Rust panic is loud and
    // process-fatal instead of a silent freeze.
    fn abort() -> !;
}

const WORD: usize = core::mem::size_of::<usize>();

struct LibcAllocator;

// SAFETY: every method upholds the GlobalAlloc contract by delegating to the
// userland libc heap. For align <= 16 we return malloc()'s block directly
// (malloc guarantees 16-byte alignment per stdlib.c). For larger alignment we
// over-allocate and store the raw base pointer in the word just before the
// aligned pointer, so dealloc/realloc can recover it. Pointers are never used
// after free and sizes never wrap (Layout guarantees a valid, non-overflowing
// size/align pair).
unsafe impl GlobalAlloc for LibcAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let align = layout.align();
        let size = layout.size();
        if align <= 16 {
            malloc(size)
        } else {
            // over-allocate: room for alignment slack + one stored base word.
            let total = match size.checked_add(align).and_then(|v| v.checked_add(WORD)) {
                Some(t) => t,
                None => return core::ptr::null_mut(),
            };
            let raw = malloc(total);
            if raw.is_null() {
                return core::ptr::null_mut();
            }
            let raw_addr = raw as usize;
            let aligned = (raw_addr + WORD + align - 1) & !(align - 1);
            *((aligned - WORD) as *mut usize) = raw_addr;
            aligned as *mut u8
        }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if layout.align() <= 16 {
            free(ptr);
        } else {
            let base = *((ptr as usize - WORD) as *const usize);
            free(base as *mut u8);
        }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if layout.align() <= 16 {
            realloc(ptr, new_size)
        } else {
            // Over-aligned blocks cannot be realloc()'d in place safely (the
            // stored base word would move): allocate fresh, copy, free old.
            let new_layout = match Layout::from_size_align(new_size, layout.align()) {
                Ok(l) => l,
                Err(_) => return core::ptr::null_mut(),
            };
            let new_ptr = self.alloc(new_layout);
            if !new_ptr.is_null() {
                let copy = if new_size < layout.size() { new_size } else { layout.size() };
                core::ptr::copy_nonoverlapping(ptr, new_ptr, copy);
                self.dealloc(ptr, layout);
            }
            new_ptr
        }
    }
}

#[global_allocator]
static ALLOCATOR: LibcAllocator = LibcAllocator;

// panic=abort: no unwinding. A Rust panic in Ring-3 routes to the userland
// libc abort() (process-fatal, loud), NOT the kernel kpanic. This must never
// silently spin (the no-busy-wait discipline, #426).
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // SAFETY: abort() is the userland libc function; it never returns and only
    // terminates the current process. We pass no formatted message to avoid
    // pulling no_std fmt into a fatal path.
    unsafe { abort() }
}

/// Stage 0 smoke test. Proves, from Ring-3, that no_std + alloc + FFI + bounds
/// checking all work in the Arena ELF, and returns a deterministic magic value
/// so the C side can confirm the Rust code actually ran (not a constant folded
/// away by C).
///
/// Computation (all in Rust):
///   * "2 + 2": push 2 and 2 into an alloc::Vec, then push 1..=10.
///   * Vec sum  = 4 + 55 = 59 = 0x3B  (proves the heap allocator round-trips).
///   * bounds-checked slice read of a small array supplies the high bytes.
///   * magic = 0xA5_5A_E7_<low byte of the Vec sum> = 0xA55AE73B.
///
/// # Safety
/// None: `extern "C"` with no arguments, touches only its own stack + the
/// shared heap via the global allocator. Safe to call any number of times.
#[no_mangle]
pub extern "C" fn arena_rs_selftest() -> u32 {
    let mut v: Vec<u32> = Vec::new();
    v.push(2);
    v.push(2); // the "2 + 2"
    for i in 1..=10u32 {
        v.push(i);
    }
    let vsum: u32 = v.iter().sum(); // 4 + 55 = 59 = 0x3B

    // bounds-checked slice op (a Rust `arr[7]` would panic if OOB; here in range)
    let arr: [u32; 4] = [0xA5, 0x5A, 0xE7, 0x00];
    let sl: &[u32] = &arr[..];

    (sl[0] << 24) | (sl[1] << 16) | (sl[2] << 8) | (vsum & 0xFF)
}
