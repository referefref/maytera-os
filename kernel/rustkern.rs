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
#[path = "rustkern/certname.rs"] mod certname;   // #tls-certfix: X.509 name/usage/pathlen policy
#[path = "rustkern/chacha20.rs"] mod chacha20;
#[path = "rustkern/checksum.rs"] mod checksum;
#[path = "rustkern/clipboard.rs"] mod clipboard;
#[path = "rustkern/conn.rs"] mod conn;
#[path = "rustkern/conring.rs"] mod conring;   // #745 (task #70): async console ring + overflow policy
#[path = "rustkern/dhcp.rs"] mod dhcp;
#[path = "rustkern/dns.rs"] mod dns;
#[path = "rustkern/drvmap.rs"] mod drvmap;   // #739: drive-letter policy for mounted disk images
#[path = "rustkern/ed25519.rs"] mod ed25519;
#[path = "rustkern/elf.rs"] mod elf;
#[path = "rustkern/elevate.rs"] mod elevate;   // #745: system-wide install elevation policy
#[path = "rustkern/exfat.rs"] mod exfat;
#[path = "rustkern/ext2.rs"] mod ext2;
#[path = "rustkern/ext2extent.rs"] mod ext2extent;
#[path = "rustkern/ext2fsck.rs"] mod ext2fsck;
#[path = "rustkern/fat.rs"] mod fat;
#[path = "rustkern/fetchown.rs"] mod fetchown;   // #745 (task #36): async HTTP job slot ownership
#[path = "rustkern/fbown.rs"] mod fbown;   // #745 (task #59): framebuffer claim ownership + lifetime
#[path = "rustkern/fsperm.rs"] mod fsperm;
#[path = "rustkern/ghash.rs"] mod ghash;
#[path = "rustkern/hidmap.rs"] mod hidmap;   // #763: HID usage -> PS/2 set-1, shared by USB and Bluetooth HID
#[path = "rustkern/hmac.rs"] mod hmac;
#[path = "rustkern/http.rs"] mod http;
#[path = "rustkern/http2.rs"] mod http2;
#[path = "rustkern/icmp.rs"] mod icmp;
#[path = "rustkern/gfsjournal.rs"] mod gfsjournal;   // #711: GraphFS tamper-evident journal
#[path = "rustkern/gfsfold.rs"] mod gfsfold;   // #711 slice 2: GraphFS fold (nodes, edges, contracts)
#[path = "rustkern/guestfs.rs"] mod guestfs;   // #708: DOS/Win16 guest filesystem identity + gate
#[path = "rustkern/inflate.rs"] mod inflate;
#[path = "rustkern/iso9660.rs"] mod iso9660;   // #196: ISO 9660 + Joliet parse (untrusted disc images)
#[path = "rustkern/aiguard.rs"] mod aiguard;   // #745: LLM prompt-injection screen
#[path = "rustkern/jpeg.rs"] mod jpeg;
#[path = "rustkern/md4.rs"] mod md4;
#[path = "rustkern/md5.rs"] mod md5;
#[path = "rustkern/mono.rs"] mod mono;
#[path = "rustkern/netstat.rs"] mod netstat;   // #745: structured net status + non-blocking probe
#[path = "rustkern/mp4.rs"] mod mp4;
#[path = "rustkern/parttbl.rs"] mod parttbl;
#[path = "rustkern/instdisk.rs"] mod instdisk;
#[path = "rustkern/pe.rs"] mod pe;
#[path = "rustkern/pgrp.rs"] mod pgrp;   // #745 (local 82): POSIX process-group + session policy
#[path = "rustkern/pollsys.rs"] mod pollsys;   // #745 (local 82): poll(2) over the VFS fd layer
#[path = "rustkern/png.rs"] mod png;
#[path = "rustkern/pwpolicy.rs"] mod pwpolicy;   // password strength + breached-password policy
#[path = "rustkern/permpath.rs"] mod permpath;   // #674: POSIX path resolution for perms_check()
#[path = "rustkern/ptwalk.rs"] mod ptwalk;   // #647: live page-table hierarchy walk
#[path = "rustkern/proc_mem.rs"] mod proc_mem;
#[path = "rustkern/procinfo.rs"] mod procinfo;
#[path = "rustkern/procreap.rs"] mod procreap;   // #745 (task 37): which zombie process slots may be reclaimed
#[path = "rustkern/sched_age.rs"] mod sched_age;
#[path = "rustkern/schedwatch.rs"] mod schedwatch;   // #67: SMP livelock diagnostic + run-queue placement policy
#[path = "rustkern/seccore.rs"] mod seccore;
#[path = "rustkern/sha256.rs"] mod sha256;
#[path = "rustkern/loginmode.rs"] mod loginmode; // #745: LOGIN.CFG parse + compose
#[path = "rustkern/sessionid.rs"] mod sessionid; // #745: session lock + account uid policy
#[path = "rustkern/spawnid.rs"] mod spawnid;   // #692: spawn identity policy
#[path = "rustkern/sntp.rs"] mod sntp;   // #797: SNTP reply validation + civil time
#[path = "rustkern/sshdcfg.rs"] mod sshdcfg;   // #697: sshd listen + auth policy
#[path = "rustkern/sha512.rs"] mod sha512;
#[path = "rustkern/taskmgr.rs"] mod taskmgr;
#[path = "rustkern/theme.rs"] mod theme;
#[path = "rustkern/tls12.rs"] mod tls12;
#[path = "rustkern/tlspool.rs"] mod tlspool;
#[path = "rustkern/tls_parse.rs"] mod tls_parse;
#[path = "rustkern/tls_suite.rs"] mod tls_suite;   // #tls-suitefix: the offered cipher suite list, shared by ClientHello and ServerHello
#[path = "rustkern/userconf.rs"] mod userconf;   // #683: per-user config paths
#[path = "rustkern/url.rs"] mod url;
#[path = "rustkern/usb_desc.rs"] mod usb_desc;
#[path = "rustkern/vfs_path.rs"] mod vfs_path;
#[path = "rustkern/wav.rs"] mod wav;
#[path = "rustkern/xattr.rs"] mod xattr;
#[path = "rustkern/xdr.rs"] mod xdr;

// ===========================================================================
// #653: security-event record formatting. NEW kernel logic, so it is Rust per
// the standing rule; this is the pure half (bytes in, bytes out, no I/O, no
// locks), which is exactly the part that belongs in Rust. The worker that
// performs the actual filesystem writes stays in C because it is entangled
// with the C wait-queue, VFS and thread-creation APIs.
//
// Every write below is bounds-checked against the caller's buffer, so a long
// or unterminated detail string cannot overrun. That matters: the detail text
// can originate from a security event triggered by hostile input.
// ===========================================================================

fn sec_put(out: &mut [u8], pos: &mut usize, b: &[u8]) {
    for &c in b {
        if *pos + 1 >= out.len() { return; }
        out[*pos] = c;
        *pos += 1;
    }
}

fn sec_put_u64(out: &mut [u8], pos: &mut usize, mut v: u64) {
    let mut tmp = [0u8; 20];
    let mut n = 0usize;
    if v == 0 { tmp[0] = b'0'; n = 1; }
    while v > 0 { tmp[n] = b'0' + (v % 10) as u8; v /= 10; n += 1; }
    while n > 0 { n -= 1; let c = tmp[n]; sec_put(out, pos, &[c]); }
}

// Severity classes match security.c's existing switch so the two cannot drift:
// 1 = CRITICAL, 2 = WARNING, 3 = INFO. Event ids are audit_event_t ordinals.
#[no_mangle]
pub extern "C" fn sec_event_severity(event: u32) -> u32 {
    match event {
        1 | 5 | 6 => 1,        // STACK_SMASH, EXEC_VIOLATION, MEMORY_VIOLATION
        0 | 2 | 4 => 2,        // SYSCALL_FAIL, PTR_INVALID, PERMISSION_DENIED
        // #697: AUTH_SUCCESS and AUTH_FAIL are both WARNING. A successful remote
        // login is as worth surfacing as a denied one: an audit trail that
        // records only failures cannot answer "who got in". AUTH_PROBE (a
        // refused credential OFFER) is INFO via the default arm below, because
        // an ordinary client offers every key it has and a toast per offer
        // would train the user to ignore all of them.
        7 | 8 => 2,            // AUTH_SUCCESS, AUTH_FAIL
        12 => 2,               // #745 AI_INJECTION: a refused LLM request
        _ => 3,                // OVERFLOW, AUTH_PROBE and anything unknown
    }
}

#[no_mangle]
pub extern "C" fn sec_event_name(event: u32) -> *const u8 {
    let s: &'static [u8] = match event {
        0 => b"SYSCALL_FAIL\0",
        1 => b"STACK_SMASH\0",
        2 => b"PTR_INVALID\0",
        3 => b"OVERFLOW\0",
        4 => b"PERMISSION_DENIED\0",
        5 => b"EXEC_VIOLATION\0",
        6 => b"MEMORY_VIOLATION\0",
        7 => b"AUTH_SUCCESS\0",     // #697
        8 => b"AUTH_FAIL\0",        // #697
        9 => b"AUTH_PROBE\0",       // #697
        // #785 appended SERVICE_STATE as ordinal 10 without a name, so every
        // service start/stop has been logging as UNKNOWN. Named here because
        // leaving a hole next to a new entry is how the next one gets missed.
        10 => b"SERVICE_STATE\0",
        // #745: elevation raised / cancelled / refused / GRANTED. INFO
        // severity via the default arm of sec_event_severity(): see the
        // comment on seclog_report_elevation() in security/security.c.
        11 => b"ELEVATION\0",
        // #745: an outbound LLM request refused, or allowed with a note, by the
        // prompt-injection screen. WARNING (see sec_event_severity): a blocked
        // request is a thing the user asked for that did not happen, so it must
        // be visible rather than buried at INFO.
        12 => b"AI_INJECTION\0",
        _ => b"UNKNOWN\0",
    };
    s.as_ptr()
}

// Format one record as: "t=<ticks> <SEVERITY> <EVENT> pid=<pid> <detail>\n"
// Returns the number of bytes written (0 if the caller gave no room).
#[no_mangle]
pub unsafe extern "C" fn sec_fmt_record(event: u32, pid: u32, ts: u64,
                                        detail: *const u8, detail_len: usize,
                                        out: *mut u8, out_cap: usize) -> usize {
    if out.is_null() || out_cap < 2 { return 0; }
    let o = core::slice::from_raw_parts_mut(out, out_cap);
    let mut pos = 0usize;

    sec_put(o, &mut pos, b"t=");
    sec_put_u64(o, &mut pos, ts);
    sec_put(o, &mut pos, b" ");
    sec_put(o, &mut pos, match sec_event_severity(event) {
        1 => b"CRITICAL" as &[u8],
        2 => b"WARNING"  as &[u8],
        _ => b"INFO"     as &[u8],
    });
    sec_put(o, &mut pos, b" ");

    let np = sec_event_name(event);
    let mut i = 0usize;
    while *np.add(i) != 0 && i < 32 {
        let c = *np.add(i);
        sec_put(o, &mut pos, &[c]);
        i += 1;
    }

    sec_put(o, &mut pos, b" pid=");
    sec_put_u64(o, &mut pos, pid as u64);

    if !detail.is_null() && detail_len > 0 {
        sec_put(o, &mut pos, b" ");
        let d = core::slice::from_raw_parts(detail, detail_len);
        for &raw in d {
            if raw == 0 { break; }
            // Newlines would break the one-record-per-line format; pipes would
            // break the notification spool's "S|title|body" framing.
            let c = if raw == b'\n' || raw == b'\r' || raw == b'|' { b' ' } else { raw };
            sec_put(o, &mut pos, &[c]);
        }
    }
    sec_put(o, &mut pos, b"\n");
    pos
}
