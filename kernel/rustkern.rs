// rustkern.rs - MayteraOS Rust-in-kernel, LIVE build (#404 / #478/#479).
//
// Phase A (b791, merged) proved Rust can live in the real kernel image and boot
// byte-behavior-identically WITHOUT changing any C behavior: a #[panic_handler]
// routed to the C kernel's loud logging+halt path (rust_kernel_panic in main.c)
// plus two trivial, unreferenced leaf probes, all present in the image but
// unreferenced by C.
//
// Phase B (b792, this change) folds the IP checksum through Rust for real: the
// proven PoC port `ip_checksum_rs` (byte-for-byte identical to the C over
// 207,507 vectors + a live HTTP 200 in the PoC) is added below, and net/ip.c
// routes the live `ip_checksum` symbol to it under -DRUST_IP_CHECKSUM (the
// strangler flag, set in the Makefile CFLAGS). A boot-time differential
// self-test in net/ip.c re-proves ip_checksum_rs == ip_checksum_c on this exact
// build before the network stack is used. The original C stays as
// ip_checksum_c for trivial rollback (drop the flag).
//
// Build (pinned): rustc 1.97.0, target x86_64-unknown-none, -C panic=abort.
// That built-in target already matches the C kernel ABI byte-for-byte
// (code-model=kernel, red zone disabled, -mmx/-sse/-sse2 + soft-float,
// panic-strategy=abort), so no custom target JSON is needed.
#![no_std]

use core::panic::PanicInfo;

extern "C" {
    // Defined in main.c. Logs loudly (serial via kprintf + /PANIC.TXT via
    // panic_log_write) and halts the CPU. Never returns (`-> !`).
    fn rust_kernel_panic(msg: *const u8) -> !;
}

// panic=abort: no unwinding, no eh_personality. A Rust-side panic must not
// silently spin (that was the anti-pattern this phase removes): route it to the
// kernel's real fatal path so it logs and halts loudly.
#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    // SAFETY: rust_kernel_panic is a C function that never returns and only
    // reads the passed NUL-terminated static byte string. We pass a message
    // rather than formatting PanicInfo to avoid pulling in no_std fmt in a
    // fatal, possibly-corrupt context. This handler is unreachable in Phase A
    // (no Rust code path can panic yet); it exists so a FUTURE Rust panic is
    // loud, not a freeze.
    unsafe { rust_kernel_panic(b"rust panic (rustkern)\0".as_ptr()); }
}

/// Trivial pure-logic probe: proves Rust executes in Ring 0 and that the
/// C -> Rust FFI calling convention is correct. Unreferenced in Phase A.
/// # Safety: none; operates only on its scalar arguments.
#[no_mangle]
pub extern "C" fn rust_probe(a: u32, b: u32) -> u32 {
    a.wrapping_add(b)
}

/// Returns a magic marker ('RUST' little-endian) so the C side could print a
/// value that could only come from this Rust object file. Unreferenced in
/// Phase A.
#[no_mangle]
pub extern "C" fn rust_marker() -> u32 {
    0x52555354 // 'R','U','S','T'
}


// ===========================================================================
// SUBSYSTEM MODULES (#404 / #526).
//
// This file used to be 9,566 lines holding all 83 exports, and four agents
// edited it concurrently in two days. A near-miss whole-file push was one
// command from silently deleting six live TLS 1.2 functions, and a mono_*
// block appeared to be clobbered mid-task (it was really an edit racing a
// rebuild). Splitting per subsystem removes the collision surface rather
// than managing it: two agents on different subsystems now touch different
// files.
//
// This is ONE crate compiled from this root in ONE rustc invocation, so the
// modules are a source-layout change ONLY. `#[no_mangle]` exports keep their
// exact C symbol names whatever module they live in, so the FFI surface is
// byte-identical and every `extern` declaration on the C side is untouched.
// rust-symbols.manifest + tools/rust-symbol-gate enforce that, and FAIL THE
// BUILD if any export ever goes missing again.
//
// Modules are private (`mod`, not `pub mod`): #[no_mangle] gives the
// functions external linkage regardless, and nothing here is a Rust-facing
// library API. Cross-module items are shared with an explicit
// `use crate::<module>::<item>` at the point of use, so every dependency
// between subsystems is written down.
// ===========================================================================
#[path = "rustkern/aes.rs"] mod aes;
#[path = "rustkern/argtab.rs"] mod argtab;
#[path = "rustkern/common.rs"] mod common;
#[path = "rustkern/arp.rs"] mod arp;
#[path = "rustkern/bmp.rs"] mod bmp;
#[path = "rustkern/cert_b64.rs"] mod cert_b64;
#[path = "rustkern/certverify.rs"] mod certverify;
#[path = "rustkern/chacha20.rs"] mod chacha20;
#[path = "rustkern/checksum.rs"] mod checksum;
#[path = "rustkern/clipboard.rs"] mod clipboard;
#[path = "rustkern/conn.rs"] mod conn;
#[path = "rustkern/dhcp.rs"] mod dhcp;
#[path = "rustkern/dns.rs"] mod dns;
#[path = "rustkern/ed25519.rs"] mod ed25519;
#[path = "rustkern/elf.rs"] mod elf;
#[path = "rustkern/exfat.rs"] mod exfat;
#[path = "rustkern/ext2.rs"] mod ext2;
#[path = "rustkern/fat.rs"] mod fat;
#[path = "rustkern/hmac.rs"] mod hmac;
#[path = "rustkern/http.rs"] mod http;
#[path = "rustkern/http2.rs"] mod http2;
#[path = "rustkern/icmp.rs"] mod icmp;
#[path = "rustkern/inflate.rs"] mod inflate;
#[path = "rustkern/jpeg.rs"] mod jpeg;
#[path = "rustkern/md4.rs"] mod md4;
#[path = "rustkern/md5.rs"] mod md5;
#[path = "rustkern/mono.rs"] mod mono;
#[path = "rustkern/mp4.rs"] mod mp4;
#[path = "rustkern/parttbl.rs"] mod parttbl;
#[path = "rustkern/pe.rs"] mod pe;
#[path = "rustkern/png.rs"] mod png;
#[path = "rustkern/proc_mem.rs"] mod proc_mem;
#[path = "rustkern/procinfo.rs"] mod procinfo;
#[path = "rustkern/sha256.rs"] mod sha256;
#[path = "rustkern/sha512.rs"] mod sha512;
#[path = "rustkern/taskmgr.rs"] mod taskmgr;
#[path = "rustkern/theme.rs"] mod theme;
#[path = "rustkern/tls12.rs"] mod tls12;
#[path = "rustkern/tls_parse.rs"] mod tls_parse;
#[path = "rustkern/url.rs"] mod url;
#[path = "rustkern/usb_desc.rs"] mod usb_desc;
#[path = "rustkern/vfs_path.rs"] mod vfs_path;
#[path = "rustkern/wav.rs"] mod wav;
#[path = "rustkern/xattr.rs"] mod xattr;
#[path = "rustkern/xdr.rs"] mod xdr;
