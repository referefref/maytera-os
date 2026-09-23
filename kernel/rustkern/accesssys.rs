// rustkern/accesssys.rs - #dosperm: the SYS_ACCESS handler body.
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16
// rule. It DECIDES nothing: the decision is perms_check() in fs/perms.c, which
// is the single place in this kernel that answers "may this identity touch
// this path" (POSIX component traversal via rustkern/permpath.rs). All this
// does is make that one answer reachable from Ring 3 without having to attempt
// the operation and infer the reason from its failure code.
//
// WHY THAT INFERENCE WAS NOT GOOD ENOUGH, measured. The DOS interpreter is one
// implementation compiled twice: in-kernel from kernel/dos/*.c, and in Ring 3
// by userland/apps/dosring3, which relinks those same sources. Its INT 21h
// AH=3Ch create asks the #708 guest gate, and the gate calls perms_check().
// Ring 3 could not call perms_check(), so shim/kshim.c stubbed the gate open
// and let the kernel's own open() refuse the write later on. That put the SAME
// guest, running the SAME source file, on two different answers:
//
//   in-kernel : gate denies -> [GUESTFS-DENY] ... reason=PERMS -> DOS error 5
//   Ring 3    : gate allows -> fat_write_file() -> EACCES from open() ->
//               [int21:dos] "FAILED on the medium" -> DOS error 3
//
// DOS error 3 is "path not found". Red Alert, launched by the default non-root
// desktop identity against its own root-owned /DOS/RA, retried that create
// 112,423 times in 150 seconds (96% of the whole serial log) rather than give
// up, which is a rational thing to do when you are told a path does not exist.
//
// FAIL CLOSED. Every path out of this function that is not an explicit
// perms_check() success returns -1. A null path, a zero mode, a mode with bits
// outside R|W|X: all denied. There is no branch here that can answer "yes"
// without perms_check() having said so.

// fs/perms.h access bits, which are the POSIX ones. Same names and same values
// as rustkern/guestfs.rs states for the same reason: this file names the
// contract it is coded against rather than importing it from a private module.
const ACCESS_MASK: i32 = 7; // R_OK(4) | W_OK(2) | X_OK(1)

extern "C" {
    // fs/perms.c. 0 to allow, negative to deny. Applies the #674 POSIX path
    // resolution and the uid-0 bypass. `path` must be NUL-terminated and is
    // already bounced into kernel memory by the C wrapper.
    fn perms_check(path: *const u8, uid: u32, gid: u32, access: i32) -> i32;
}

/// SYS_ACCESS handler body. `path` is a kernel-side, NUL-terminated copy made
/// by sys_access() in proc/syscall.c; `euid`/`egid` are the CALLING process's
/// effective credentials, read there.
///
/// Returns 0 if the access is permitted, -1 otherwise.
///
/// F_OK (mode 0) IS DELIBERATELY NOT OFFERED. Existence is a different
/// question with a different answer path (SYS_STAT), and answering it here
/// from perms_check() alone would report "permitted" for a path that is not
/// there, which is worse than not answering at all.
///
/// # Safety
/// `path` must be a valid NUL-terminated kernel pointer or null.
#[no_mangle]
pub extern "C" fn rk_access(path: *const u8, mode: i32, euid: u32, egid: u32) -> i64 {
    if path.is_null() {
        return -1;
    }
    if mode == 0 || (mode & !ACCESS_MASK) != 0 {
        return -1;
    }
    if unsafe { perms_check(path, euid, egid, mode) } == 0 {
        0
    } else {
        -1
    }
}
