// rustkern/argtab.rs - #503 / MAYTERA-SEC-2026-0016 THE SYSCALL POINTER CHOKE POINT
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #503 / MAYTERA-SEC-2026-0016 - THE SYSCALL POINTER CHOKE POINT (Rust).
//
// New kernel code, so Rust per the 2026-07-16 rule. This is not a port: there
// is no C twin and no strangler flag, because there was never any C here to
// strangle. That is the whole finding of #500 - 171 user-pointer arguments
// across 239 dispatcher cases, and only five validated call sites tree-wide.
//
// WHY THE TABLE IS IN RUST AND NOT C. The bug class this closes is "a valid
// base with an attacker-chosen length". The arithmetic that decides how many
// bytes to check is therefore the load-bearing part, and it is exactly the part
// C will silently wrap for you: `count * elem_size` overflowing to a small
// number turns a 4GB check into an 8-byte one and the check passes. Every
// length computation below is a checked_mul/checked_add that returns EFAULT on
// overflow rather than a wrapped value. This tree has already shipped that class
// twice (MAYTERA-SEC-2026-0014 total_length-ihl underflow; #489 p_filesz
// underflow), so the bounds living in the types is the point, not decoration.
//
// WHAT THIS DOES NOT DO, stated plainly:
//   - It is NOT TOCTOU-safe. It makes a pointer valid-at-entry. A sibling
//     thread can unmap the range between this check and the handler's memcpy.
//     The real fix is copy_from_user() under the check, which is functional for
//     the first time (#500) but is NOT adopted by the handlers. Do not read
//     this table as a claim of TOCTOU safety.
//   - A syscall with NO descriptor is NOT validated. syscall_validate_args()
//     returns 0 for it. The table is adopted incrementally; the undeclared set
//     is the tools/syscall-ptr-lint allowlist, which is a DEBT LEDGER of open
//     holes, not a set of accepted exceptions.
//
// TWO DELIBERATE JUDGEMENTS, both of which trade a little strictness for not
// breaking the desktop, and both of which are honest about what they give up:
//
//  1. NULL IS SKIPPED, NOT REJECTED. Many handlers here take optional out-params
//     and NULL-check them themselves (`if (tp) *tp = ...` in SYS_GET_MEM_INFO,
//     `if (meta)` in SYS_FONT_GLYPH, `if (!buf) return 0;` in
//     SYS_GET_CPU_PER_CORE). Rejecting NULL centrally would turn every one of
//     those legal calls into -EFAULT and break live callers. NULL is also not
//     the threat: the hole being closed is Ring-3 naming KERNEL memory, and
//     NULL is not kernel memory. A NULL that the handler fails to check faults
//     in Ring 0 exactly as it does today - pre-existing, not a regression, and
//     out of scope here.
//
//  2. LENGTHS ARE VALIDATED AT FULL 64-BIT WIDTH, unmasked, even where the
//     handler casts the argument down (e.g. SYS_RECV casts arg3 to uint16_t).
//     The asymmetry is deliberate: validating MORE than the handler uses can
//     only reject absurd calls, while masking to match the cast would validate
//     LESS than the handler might use and that is a hole. Strict in the safe
//     direction.
// ===========================================================================

// Mirrors security/validate.h. Locked against drift by _Static_assert in
// proc/syscall_argtab_lock.c - if those defines ever change, the build fails
// rather than this table silently asking for the wrong access.
use crate::procinfo::PI_NAME_MAX;

const ACCESS_READ: u32 = 1 << 0;
const ACCESS_WRITE: u32 = 1 << 1;
const ACCESS_USER: u32 = 1 << 3;
const ACCESS_READ_USER: u32 = ACCESS_READ | ACCESS_USER;
const ACCESS_RW_USER: u32 = ACCESS_READ | ACCESS_WRITE | ACCESS_USER;

const VALIDATE_OK: i32 = 0;
const EFAULT: i32 = -14;

extern "C" {
    // security/validate.c. Walks the CALLER's CR3 and demands the U/S bit (and
    // the W bit for writes) on EVERY page of the range. Not presence, and not
    // an address range: USER_SPACE_START is 0x400000, which is the kernel's own
    // load address, so a range test proves nothing on this OS.
    fn validate_user_ptr(ptr: *const u8, size: usize, access: u32) -> i32;
    fn validate_user_string(s: *const u8, max_len: usize) -> i32;
}

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    /// Scalar, or a pointer the kernel never dereferences.
    None,
    /// User buffer the kernel READS. Unvalidated: info leak / kernel-memory read.
    R,
    /// User buffer the kernel WRITES. Unvalidated: ARBITRARY WRITE. Worse.
    W,
    /// NUL-terminated user string, bounded scan.
    Str,
}

#[derive(Clone, Copy)]
enum Len {
    None,
    /// Byte count known at compile time (a struct sizeof, locked in C).
    Fixed(u32),
    /// Byte count is in argN (1-6).
    Arg(u8),
    /// Element count is in argN (1-6); u16 is bytes per element.
    Elems(u8, u16),
    /// Maximum scan length for a string.
    StrMax(u32),
    /// Element count is in argN, but the HANDLER CLAMPS it to at most `cap`
    /// elements before writing (`if (max > CAP) max = CAP;`). Validating the
    /// raw, unclamped count would reject a legal call: a caller is entitled to
    /// pass max=1000 against a 64-row buffer precisely BECAUSE the handler
    /// clamps. So validate min(argN, cap) elements, which is exactly the set of
    /// bytes the handler can reach. This is not a weakening: every byte the
    /// handler may write is still proven user-writable.
    ElemsClamped(u8, u16, u16),
    /// Byte count is argI * argJ * elem: two independent dimension args
    /// (SYS_WIN_DRAW_IMAGE takes w in arg4 and h in arg5).
    Mul2(u8, u8, u16),
    /// Byte count is (argI & 0xFFFF) * ((argI >> 16) & 0xFFFF) * elem: two
    /// 16-bit dimensions the dispatcher unpacks from ONE arg (SYS_WIN_BLIT).
    /// The unpack is duplicated from the dispatcher, so it is asserted against
    /// the real thing by a negative control rather than trusted.
    Packed16(u8, u16),
}

#[derive(Clone, Copy)]
struct A {
    kind: Kind,
    len: Len,
    /// The handler runs SANITIZE_USER_PTR on this argument, so the address it
    /// finally dereferences is NOT the raw arg. See sanitize_user_ptr() below.
    /// Validate the address the handler will actually touch; validating the raw
    /// value would reject the browser's own calls.
    sx: bool,
}

const NONE: A = A { kind: Kind::None, len: Len::None, sx: false };
const fn wf(n: u32) -> A { A { kind: Kind::W, len: Len::Fixed(n), sx: false } }
const fn rf(n: u32) -> A { A { kind: Kind::R, len: Len::Fixed(n), sx: false } }
const fn wa(i: u8) -> A { A { kind: Kind::W, len: Len::Arg(i), sx: false } }
const fn ra(i: u8) -> A { A { kind: Kind::R, len: Len::Arg(i), sx: false } }
const fn we(i: u8, e: u16) -> A { A { kind: Kind::W, len: Len::Elems(i, e), sx: false } }
const fn wec(i: u8, e: u16, cap: u16) -> A {
    A { kind: Kind::W, len: Len::ElemsClamped(i, e, cap), sx: false }
}
const fn wm2(i: u8, j: u8, e: u16) -> A { A { kind: Kind::W, len: Len::Mul2(i, j, e), sx: false } }
const fn rm2(i: u8, j: u8, e: u16) -> A { A { kind: Kind::R, len: Len::Mul2(i, j, e), sx: false } }
const fn rp16(i: u8, e: u16) -> A { A { kind: Kind::R, len: Len::Packed16(i, e), sx: false } }
const fn s(max: u32) -> A { A { kind: Kind::Str, len: Len::StrMax(max), sx: false } }

/// Mark an argument as one the handler SANITIZE_USER_PTR's. Wraps a normal
/// descriptor so the two facts stay visibly separate: WHAT the buffer is, and
/// the 32-bit-compat shim the handler applies to reach it.
const fn sx(a: A) -> A { A { kind: a.kind, len: a.len, sx: true } }

/// EXACT mirror of SANITIZE_USER_PTR in proc/syscall.c. Locked to the C by
/// USER_PTR_SX_PREFIX + a _Static_assert in proc/syscall_argtab_lock.c.
///
/// WHY THIS EXISTS AT ALL: user.ld loads apps at 0x80000000, exactly the 2^31
/// boundary, which is NEGATIVE as a signed 32-bit value. A user pointer that
/// round-trips through a 32-bit int arrives sign-extended as 0xFFFFFFFF8xxxxxxx,
/// and three handlers (sys_win_blit, sys_win_draw_image, sys_decode_image) mask
/// it back down. The browser depends on this (CHANGELOG: "Pointer args
/// SANITIZE_USER_PTR'd (browser loads at 0x80000000)").
///
/// So the validator MUST apply the identical transform, or it would prove things
/// about an address the handler never touches and reject every real call. This
/// is not a weakening: the masked address still has to pass the full U/S (+W)
/// walk of the caller's CR3, and Ring 3 could have passed the masked value
/// directly anyway. The invariant kept here is the one that matters: VALIDATE
/// THE ADDRESS THE HANDLER WILL DEREFERENCE.
const fn sanitize_user_ptr(p: u64) -> u64 {
    if (p & 0xFFFF_FFFF_0000_0000) == 0xFFFF_FFFF_0000_0000 {
        p & 0xFFFF_FFFF
    } else {
        p
    }
}

// String scan bounds. A path/name that is not NUL-terminated inside PATH_MAX is
// not a legal path, it is an overread attempt. TEXT_MAX is generous because
// SYS_WIN_DRAW_* strings are arbitrary app content and a false reject there
// silently blanks text on screen.
const PATH_MAX: u32 = 4096;
const TEXT_MAX: u32 = 65536;

struct Desc {
    num: u16,
    args: [A; 6],
}

// Struct byte counts. EVERY constant here is locked by a _Static_assert in
// proc/syscall_argtab_lock.c (public structs) or proc/syscall.c (structs private
// to that TU). If a struct grows and this number does not, the validator would
// check fewer bytes than the kernel writes, which is a hole; the build fails
// instead. Values are compiler ground truth (nm -S on a probe TU built with the
// real kernel CFLAGS), not hand-computed padding guesses.
const SZ_DEVINFO_SYSINFO: u32 = 160;
const SZ_DEVINFO_PCI: u16 = 76;
const SZ_DEVINFO_USB: u16 = 16;
const SZ_DEVINFO_IRQ: u16 = 16;
const SZ_PROC_INFO: u16 = 64;
const SZ_WM_WINDOW_INFO: u16 = 96;
const SZ_CRON_JOB: u16 = 128;
const SZ_SC_USER_INFO: u16 = 140;
const SZ_FB_INFO_USER: u32 = 24;
const SZ_KEY_EVENT: u32 = 24;
const SZ_K_STAT: u32 = 88;
const SZ_DIRENT: u32 = 264;
// #503 batch 2: proc/procinfo.h rows. Each already carries its own
// _Static_assert at its definition; syscall_argtab_lock.c re-asserts them here
// so a change to procinfo.h fails the build pointing at THIS table.
const SZ_HANDLE_INFO: u16 = 112;
const SZ_SVC_INFO: u16 = 80;
const SZ_PROC_DETAIL: u32 = 112;
const SZ_TCP_CONN_INFO: u16 = 24;
// Row caps the procinfo handlers clamp `max` to before writing. Mirrored from
// PI_MAX_HANDLES / TM_PI_MAX_CONNS / PI_MAX_SVCS and locked in the same place.
const CAP_HANDLES: u16 = 64;
const CAP_CONNS: u16 = 64;
const CAP_SVCS: u16 = 32;
// #503 batch 3. All from nm -S on a probe TU built with the real kernel CFLAGS,
// not hand-computed padding. Locked in syscall_argtab_lock.c (public) or at the
// struct definition in proc/syscall.c (private to that TU).
const SZ_GUI_EVENT: u32 = 32;
const SZ_NET_INFO: u32 = 88;
const SZ_SC_DISK_INFO: u32 = 72;
const SZ_PRINTER_CFG: u16 = 172;
// net/ipp.h PRINT_MAX_PRINTERS: print_list() cannot write more rows than the
// static table has, so the clamp is real even though the handler's loop bound
// is `n < max` (g_printer_count can never exceed this).
const CAP_PRINTERS: u16 = 8;
// SYS_FONT_GLYPH arg3 is int meta[5]: the handler writes meta[0..4] (width,
// height, xoff, yoff, advance) and nothing more.
const SZ_FONT_GLYPH_META: u32 = 20;
// The bound sys_svc_control itself scans the name to is procinfo.h PI_NAME_MAX,
// which this file ALREADY defines for the procinfo builders (see PI_NAME_MAX
// above). Reused rather than restated: a second copy of the same number is a
// second thing to forget to update.

// ---------------------------------------------------------------------------
// THE TABLE. One line per syscall, one judgement per argument, each read off
// the dispatcher cast and the handler's own use of the buffer. Ordered by
// syscall number.
//
// BATCH 1 SCOPE: WRITE arguments first, because an unvalidated write is an
// arbitrary write while an unvalidated read is "only" an info leak. Reads and
// strings that ride along on the same syscall are declared here too, since a
// descriptor covers all six args at once.
// ---------------------------------------------------------------------------
static TAB: &[Desc] = &[
    // --- fs ---------------------------------------------------------------
    // sys_wait(-1, (int *)arg1) - exit-status out-param, optional (POSIX).
    Desc { num: 3, args: [wf(4), NONE, NONE, NONE, NONE, NONE] },
    // sys_open((const char *)arg1, (int)arg2)
    Desc { num: 10, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },
    // sys_read((int)arg1, (void *)arg2, (size_t)arg3) - WRITE of arg3 bytes.
    Desc { num: 12, args: [NONE, wa(3), NONE, NONE, NONE, NONE] },
    // sys_write((int)arg1, (const void *)arg2, (size_t)arg3) - READ of arg3.
    Desc { num: 13, args: [NONE, ra(3), NONE, NONE, NONE, NONE] },
    // sys_stat_path((const char *)arg1, (void *)arg2) - arg2 is k_stat_t *.
    Desc { num: 15, args: [s(PATH_MAX), wf(SZ_K_STAT), NONE, NONE, NONE, NONE] },
    // sys_mkdir((const char *)arg1, (int)arg2)
    Desc { num: 16, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },
    Desc { num: 17, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] }, // SYS_RMDIR
    Desc { num: 18, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] }, // SYS_UNLINK
    // sys_readdir((int)arg1, (void *)arg2) - arg2 is the TU-local dirent_t
    // { char name[256]; uint32_t type; uint32_t size; } = 264 bytes.
    Desc { num: 19, args: [NONE, wf(SZ_DIRENT), NONE, NONE, NONE, NONE] },
    // sys_rename((const char *)arg1, (const char *)arg2)
    Desc { num: 70, args: [s(PATH_MAX), s(PATH_MAX), NONE, NONE, NONE, NONE] },
    // sys_getcwd((char *)arg1, (uint64_t)arg2)
    Desc { num: 99, args: [wa(2), NONE, NONE, NONE, NONE, NONE] },
    Desc { num: 100, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] }, // SYS_CHDIR
    // sys_chmod((const char *)arg1, (uint16_t)arg2)
    Desc { num: 128, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },
    // sys_chown((const char *)arg1, uid, gid)
    Desc { num: 129, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },

    // --- process ----------------------------------------------------------
    // sys_exec((const char *)arg1)
    Desc { num: 2, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },
    // pipe_create -> user_pipefd[0..1]: int[2] = 8 bytes, unconditional write.
    Desc { num: 92, args: [wf(8), NONE, NONE, NONE, NONE, NONE] },
    // sys_spawn_args((const char *)arg1, NULL, 0) - argv is NOT taken from user.
    Desc { num: 196, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },

    // --- ipc / shm --------------------------------------------------------
    // sys_shm_map((int)arg1, (void **)arg2)
    Desc { num: 171, args: [NONE, wf(8), NONE, NONE, NONE, NONE] },
    // sys_shm_info((int)arg1, (size_t *)arg2, (uint32_t *)arg3)
    Desc { num: 174, args: [NONE, wf(8), wf(4), NONE, NONE, NONE] },
    // sys_ipc_register_name((const char *)arg1, (int)arg2)
    Desc { num: 180, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },
    // sys_ipc_lookup_name((const char *)arg1)
    Desc { num: 181, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },

    // --- gui / input ------------------------------------------------------
    // sys_win_create((const char *)arg1, x, y, w, h) - title string.
    Desc { num: 30, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },
    // sys_win_draw_text(win, x, y, (const char *)arg4, colour)
    Desc { num: 33, args: [NONE, NONE, NONE, s(TEXT_MAX), NONE, NONE] },
    // sys_win_get_size((int)arg1, (int *)arg2, (int *)arg3)
    Desc { num: 38, args: [NONE, wf(4), wf(4), NONE, NONE, NONE] },
    // sys_win_get_pos((int)arg1, (int *)arg2, (int *)arg3)
    Desc { num: 150, args: [NONE, wf(4), wf(4), NONE, NONE, NONE] },
    // sys_wm_get_windows((wm_window_info_t *)arg1, (int)arg2)
    Desc { num: 155, args: [we(2, SZ_WM_WINDOW_INFO), NONE, NONE, NONE, NONE, NONE] },
    // sys_fb_info((fb_info_user_t *)arg1)
    Desc { num: 201, args: [wf(SZ_FB_INFO_USER), NONE, NONE, NONE, NONE, NONE] },
    // sys_get_mouse((int32_t *)arg1, (int32_t *)arg2, (uint32_t *)arg3)
    Desc { num: 210, args: [wf(4), wf(4), wf(4), NONE, NONE, NONE] },
    // sys_get_key((key_event_t *)arg1)
    Desc { num: 212, args: [wf(SZ_KEY_EVENT), NONE, NONE, NONE, NONE, NONE] },
    // ttf_draw_string(x, y, (const char *)arg3, len, colour)
    Desc { num: 221, args: [NONE, NONE, s(TEXT_MAX), NONE, NONE, NONE] },
    // ttf_measure_string((const char *)arg1, (int)arg2)
    Desc { num: 222, args: [s(TEXT_MAX), NONE, NONE, NONE, NONE, NONE] },
    // sys_win_draw_text_small(win, x, y, (const char *)arg4, colour)
    Desc { num: 232, args: [NONE, NONE, NONE, s(TEXT_MAX), NONE, NONE] },
    // sys_win_draw_text_ttf(win, x, y, (const char *)arg4, colour|size)
    Desc { num: 235, args: [NONE, NONE, NONE, s(TEXT_MAX), NONE, NONE] },
    // sys_get_global_mouse((int32_t *)arg1, (int32_t *)arg2, (uint32_t *)arg3)
    Desc { num: 264, args: [wf(4), wf(4), wf(4), NONE, NONE, NONE] },
    // ttf_face_name((int)arg1, (char *)arg2, (int)arg3 cap)
    Desc { num: 308, args: [NONE, wa(3), NONE, NONE, NONE, NONE] },
    // ttf_get_metrics_f(face, size, &out[0], &out[1], &out[2]) - int[3] = 12.
    Desc { num: 310, args: [NONE, wf(12), NONE, NONE, NONE, NONE] },
    // sys_win_draw_text_ttf_ex(win, xy, (const char *)arg3, face|size, colour)
    Desc { num: 311, args: [NONE, NONE, s(TEXT_MAX), NONE, NONE, NONE] },

    // --- users / auth -----------------------------------------------------
    // sys_passwd_change((const char *)arg1, (const char *)arg2, (const char *)arg3)
    Desc { num: 130, args: [s(PATH_MAX), s(PATH_MAX), s(PATH_MAX), NONE, NONE, NONE] },
    // sys_su((const char *)arg1, (const char *)arg2)
    Desc { num: 131, args: [s(PATH_MAX), s(PATH_MAX), NONE, NONE, NONE, NONE] },
    // sys_adduser((const char *)arg1, uid, gid, (const char *)arg4, (const char *)arg5)
    Desc { num: 132, args: [s(PATH_MAX), NONE, NONE, s(PATH_MAX), s(PATH_MAX), NONE] },
    // sys_delete_user((const char *)arg1)
    Desc { num: 159, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },
    // sys_list_users((sc_user_info_t *)arg1, (int)arg2)
    Desc { num: 190, args: [we(2, SZ_SC_USER_INFO), NONE, NONE, NONE, NONE, NONE] },
    // sys_authenticate((const char *)arg1, (const char *)arg2)
    Desc { num: 191, args: [s(PATH_MAX), s(PATH_MAX), NONE, NONE, NONE, NONE] },

    // --- misc / sys -------------------------------------------------------
    // pmm totals: (unsigned long *)arg1, (unsigned long *)arg2, both optional.
    Desc { num: 194, args: [wf(8), wf(8), NONE, NONE, NONE, NONE] },
    // sys_play_wav((const char *)arg1)
    Desc { num: 192, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },
    // version string copy into (char *)arg1, cap (int)arg2.
    Desc { num: 246, args: [wa(2), NONE, NONE, NONE, NONE, NONE] },
    // sys_bootlog_write((const char *)arg1)
    Desc { num: 298, args: [s(TEXT_MAX), NONE, NONE, NONE, NONE, NONE] },

    // --- proc / device introspection (#487) --------------------------------
    // proc_snapshot((proc_info_t *)arg1, (int)arg2)
    Desc { num: 238, args: [we(2, SZ_PROC_INFO), NONE, NONE, NONE, NONE, NONE] },
    // devinfo_pci_list((devinfo_pci_t *)arg1, (int)arg2)
    Desc { num: 272, args: [we(2, SZ_DEVINFO_PCI), NONE, NONE, NONE, NONE, NONE] },
    // devinfo_usb_list((devinfo_usb_t *)arg1, (int)arg2)
    Desc { num: 273, args: [we(2, SZ_DEVINFO_USB), NONE, NONE, NONE, NONE, NONE] },
    // devinfo_irq_list((devinfo_irq_t *)arg1, (int)arg2)
    Desc { num: 274, args: [we(2, SZ_DEVINFO_IRQ), NONE, NONE, NONE, NONE, NONE] },
    // devinfo_sysinfo((devinfo_sysinfo_t *)arg1)
    Desc { num: 275, args: [wf(SZ_DEVINFO_SYSINFO), NONE, NONE, NONE, NONE, NONE] },
    // cron_add((const cron_job_t *)arg1) - kernel READS the job out of user mem.
    Desc { num: 276, args: [rf(SZ_CRON_JOB as u32), NONE, NONE, NONE, NONE, NONE] },
    // cron_list((cron_job_t *)arg1, (int)arg2)
    Desc { num: 277, args: [we(2, SZ_CRON_JOB), NONE, NONE, NONE, NONE, NONE] },

    // --- net --------------------------------------------------------------
    // tcp_send_kcr3((int)arg1, (const void *)arg2, (uint16_t)arg3)
    Desc { num: 62, args: [NONE, ra(3), NONE, NONE, NONE, NONE] },
    // tcp_recv_kcr3((int)arg1, (void *)arg2, (uint16_t)arg3)
    Desc { num: 63, args: [NONE, wa(3), NONE, NONE, NONE, NONE] },
    // sys_dns_start((const char *)arg1, (uint32_t *)arg2)
    Desc { num: 215, args: [s(PATH_MAX), wf(4), NONE, NONE, NONE, NONE] },
    // sys_dns_poll((uint32_t *)arg1)
    Desc { num: 216, args: [wf(4), NONE, NONE, NONE, NONE, NONE] },
    // net_format_info((char *)arg1, (unsigned long)arg2)
    Desc { num: 243, args: [wa(2), NONE, NONE, NONE, NONE, NONE] },
    // sys_http_fetch_start((const char *)arg1)
    Desc { num: 255, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },
    // sys_http_fetch_poll((int)arg1, (int *)arg2, (uint32_t *)arg3) - both optional.
    Desc { num: 256, args: [NONE, wf(4), wf(4), NONE, NONE, NONE] },
    // sys_http_fetch_read((int)arg1, (char *)arg2, (uint32_t)arg3)
    Desc { num: 257, args: [NONE, wa(3), NONE, NONE, NONE, NONE] },
    // sys_http_post_start((const char *)arg1, (const char *)arg2, (const char *)arg3)
    Desc { num: 265, args: [s(PATH_MAX), s(TEXT_MAX), s(TEXT_MAX), NONE, NONE, NONE] },
    // sys_http_post_poll((int)arg1, (int *)arg2, (uint32_t *)arg3) - both optional.
    Desc { num: 266, args: [NONE, wf(4), wf(4), NONE, NONE, NONE] },
    // sys_http_post_read((int)arg1, (char *)arg2, (uint32_t)arg3)
    Desc { num: 267, args: [NONE, wa(3), NONE, NONE, NONE, NONE] },

    // --- #503 batch 2: image decode (the one that matters this week) -------
    // sys_decode_image((const void *)arg1, (uint32_t)arg2 len, (uint32_t)arg3
    //   target, (void *)arg4 out, (uint32_t)arg5 out_cap, (int *)arg6 dims)
    //
    // WHY THIS ONE FIRST: arg1 is attacker-controlled image bytes handed to the
    // Ring-0 decoders, and that path has already produced two real bugs
    // (MAYTERA-SEC-2026-0013, a ~259KB JPEG heap OOB WRITE; and a PNG IHDR
    // overflow). The RSS reader is now growing inline feed images, which makes
    // arg1 REMOTE untrusted input rather than merely local untrusted input.
    //
    // All three pointers are SANITIZE_USER_PTR'd by the handler, hence sx().
    // arg6 is int[2]: the handler writes dims[0] and dims[1] unconditionally
    // once it succeeds, and nothing else. 8 bytes, not "an int".
    Desc { num: 253, args: [sx(ra(2)), NONE, NONE, sx(wa(5)), NONE, sx(wf(8))] },

    // --- #503 batch 2: proc/service introspection (#487) -------------------
    // These five already validate correctly INSIDE the handler (procinfo.c was
    // written that way deliberately). Declaring them here does not add a second
    // check so much as move the claim into the one place the lint can see: the
    // handler's private check is invisible to the ledger, and a future refactor
    // could drop it silently. Each descriptor is deliberately the SAME set of
    // bytes the handler proves, including the clamp, so the two cannot disagree.
    //
    // sys_proc_handles((uint32_t)arg1 pid, (void *)arg2, (int)arg3 max)
    Desc { num: 318, args: [NONE, wec(3, SZ_HANDLE_INFO, CAP_HANDLES), NONE, NONE, NONE, NONE] },
    // sys_net_conns((uint32_t)arg1 pid, (void *)arg2, (int)arg3 max)
    Desc { num: 319, args: [NONE, wec(3, SZ_TCP_CONN_INFO, CAP_CONNS), NONE, NONE, NONE, NONE] },
    // sys_svc_list((void *)arg1, (int)arg2 max)
    Desc { num: 320, args: [wec(2, SZ_SVC_INFO, CAP_SVCS), NONE, NONE, NONE, NONE, NONE] },
    // sys_svc_control((const char *)arg1 name, (int)arg2 action) - the handler
    // scans to PI_NAME_MAX, so the descriptor scans to PI_NAME_MAX.
    Desc { num: 321, args: [s(PI_NAME_MAX as u32), NONE, NONE, NONE, NONE, NONE] },
    // sys_proc_detail((uint32_t)arg1 pid, (void *)arg2 out)
    Desc { num: 322, args: [NONE, wf(SZ_PROC_DETAIL), NONE, NONE, NONE, NONE] },

    // --- #503 batch 3: the other two SANITIZE_USER_PTR handlers ------------
    // sys_win_blit(handle, x, y, src_w|src_h packed in arg4, (uint32_t *)arg5)
    // The dispatcher unpacks src_w = arg4 & 0xFFFF, src_h = (arg4 >> 16) &
    // 0xFFFF, and the scaler reads src_buffer[sy*src_w + sx] for sy < src_h and
    // sx < src_w: exactly src_w*src_h*4 bytes. Hence Packed16(4, 4).
    Desc { num: 35, args: [NONE, NONE, NONE, NONE, sx(rp16(4, 4)), NONE] },
    // sys_win_draw_image(handle, x, y, (int)arg4 w, (int)arg5 h, (uint32_t *)arg6)
    // Reads src[ry*w + rx] for ry < h, rx < w = w*h*4 bytes, from two separate
    // dimension args. Hence Mul2(4, 5, 4).
    Desc { num: 254, args: [NONE, NONE, NONE, NONE, NONE, sx(rm2(4, 5, 4))] },

    // --- #503 batch 3: fixed-size out-params --------------------------------
    // sys_win_get_event((int)arg1, (void *)arg2, (int)arg3) - arg2 is gui_event_t*.
    Desc { num: 36, args: [NONE, wf(SZ_GUI_EVENT), NONE, NONE, NONE, NONE] },
    // sys_get_net_info((void *)arg1, (uint64_t)arg2 len) - writes exactly one
    // net_info_t. The handler already refuses len < sizeof(net_info_t), so the
    // bytes it can write are a constant, not arg2.
    Desc { num: 146, args: [wf(SZ_NET_INFO), NONE, NONE, NONE, NONE, NONE] },
    // sys_get_disk_info((int)arg1 idx, (void *)arg2) - zeroes then fills one
    // sc_disk_info_t unconditionally.
    Desc { num: 199, args: [NONE, wf(SZ_SC_DISK_INFO), NONE, NONE, NONE, NONE] },
    // sys_print_list((void *)arg1, (int)arg2 max) -> print_list(), which writes
    // at most min(max, g_printer_count <= PRINT_MAX_PRINTERS) rows.
    Desc { num: 291, args: [wec(2, SZ_PRINTER_CFG, CAP_PRINTERS), NONE, NONE, NONE, NONE, NONE] },
    // SYS_FONT_GLYPH: arg3 = int meta[5] (optional, NULL-checked by the handler);
    // arg4 = glyph bitmap out, arg5 = its capacity. The handler copies
    // width*height bytes ONLY when cap >= width*height, so cap bounds every byte
    // it can write and is the honest length to validate.
    Desc { num: 309, args: [NONE, NONE, wf(SZ_FONT_GLYPH_META), wa(5), NONE, NONE] },

    // --- #503 batch 3: string-only syscalls ---------------------------------
    // Each of these hands Ring-3 char* straight to a kernel consumer. An
    // unvalidated string is an overread: the kernel walks memory the caller
    // named until it happens to find a NUL, and copies what it passes.
    // sys_net_set_static((const char *)arg1 ip, arg2 mask, arg3 gw)
    Desc { num: 217, args: [s(PATH_MAX), s(PATH_MAX), s(PATH_MAX), NONE, NONE, NONE] },
    // win16_launch((const char *)arg1)
    Desc { num: 237, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },
    // dos_launch((const char *)arg1)
    Desc { num: 240, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },
    // sys_net_mount((const char *)arg1 server, arg2 share, arg3 user, arg4 pass)
    Desc { num: 269, args: [s(PATH_MAX), s(PATH_MAX), s(PATH_MAX), s(PATH_MAX), NONE, NONE] },
    // sys_net_list_shares((const char *)arg1 server, (char *)arg2 out, (uint32_t)arg3 maxlen)
    Desc { num: 270, args: [s(PATH_MAX), wa(3), NONE, NONE, NONE, NONE] },
    // sys_net_unmount((const char *)arg1 server, (const char *)arg2 share)
    Desc { num: 271, args: [s(PATH_MAX), s(PATH_MAX), NONE, NONE, NONE, NONE] },
    // sys_print_job((const char *)arg1 printer, arg2 title, arg3 text) - the
    // text is arbitrary document content, so it gets the generous TEXT_MAX bound
    // rather than PATH_MAX; a false reject here silently drops a print job.
    Desc { num: 292, args: [s(PATH_MAX), s(PATH_MAX), s(TEXT_MAX), NONE, NONE, NONE] },
    // sys_print_add((const char *)arg1 name, arg2 host, (int)arg3 port,
    //   (const char *)arg4 queue, (int)arg5 make_default)
    Desc { num: 293, args: [s(PATH_MAX), s(PATH_MAX), NONE, s(PATH_MAX), NONE, NONE] },
    // sys_print_remove((const char *)arg1)
    Desc { num: 294, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },
    // sys_print_image((const char *)arg1 printer, (const char *)arg2 path)
    Desc { num: 296, args: [s(PATH_MAX), s(PATH_MAX), NONE, NONE, NONE, NONE] },
    // sys_print_screen((const char *)arg1 printer)
    Desc { num: 297, args: [s(PATH_MAX), NONE, NONE, NONE, NONE, NONE] },

    // --- #503 batch 3: the deferred-write syscall ---------------------------
    // proc_set_tid_address((uint32_t *)arg1). This descriptor is NOT sufficient
    // on its own and is deliberately not treated as if it were: what the kernel
    // stores here it WRITES AT THREAD EXIT, arbitrarily later, so proving the
    // pointer good at dispatch proves nothing about the moment of the write.
    // The real checks are in proc/process.c (refuse to store) and proc/thread.c
    // (re-validate immediately before the write). This entry is the cheap
    // fail-fast that gives a bad pointer an error at the syscall instead of a
    // silently dropped write later.
    Desc { num: 112, args: [wf(4), NONE, NONE, NONE, NONE, NONE] },

    // --- #503: the syscall the ledger could not see -------------------------
    // proc_wait((int)arg1 pid, (int *)arg2 status). SYS_WAITPID was invisible to
    // the inventory and to the lint because its dispatcher label was spelled
    // `case 98:` rather than `case SYS_WAITPID:`, so it was never among the 109
    // and never in the allowlist: an undeclared user `int *` out-param hiding
    // behind a naming convention. The label is fixed in proc/syscall.c; this is
    // the descriptor it should always have had. Same shape as SYS_WAIT (num 3):
    // the status out-param is optional (POSIX), so NULL is skipped, not rejected.
    Desc { num: 98, args: [NONE, wf(4), NONE, NONE, NONE, NONE] },
];

/// Resolve a Len to a byte count, or None if the caller's numbers cannot
/// describe a sane range (which is itself an EFAULT, not a "skip").
///
/// checked_mul is the point of this function existing. `count * elem_size` is
/// precisely where a wrapped multiply turns an enormous request into a tiny
/// check that then passes.
fn len_bytes(l: Len, raw: &[u64; 6]) -> Option<u64> {
    match l {
        Len::None => Some(0),
        Len::Fixed(n) => Some(n as u64),
        Len::StrMax(n) => Some(n as u64),
        Len::Arg(i) => {
            if i < 1 || i > 6 {
                return None;
            }
            Some(raw[(i - 1) as usize])
        }
        Len::Elems(i, e) => {
            if i < 1 || i > 6 {
                return None;
            }
            raw[(i - 1) as usize].checked_mul(e as u64)
        }
        Len::ElemsClamped(i, e, cap) => {
            if i < 1 || i > 6 {
                return None;
            }
            let n = raw[(i - 1) as usize];
            let capped = if n > cap as u64 { cap as u64 } else { n };
            capped.checked_mul(e as u64)
        }
        Len::Mul2(i, j, e) => {
            if i < 1 || i > 6 || j < 1 || j > 6 {
                return None;
            }
            raw[(i - 1) as usize]
                .checked_mul(raw[(j - 1) as usize])
                .and_then(|v| v.checked_mul(e as u64))
        }
        Len::Packed16(i, e) => {
            if i < 1 || i > 6 {
                return None;
            }
            // Same unpack the dispatcher performs, both fields masked to 16 bits
            // so this cannot overflow before the checked_mul regardless.
            let v = raw[(i - 1) as usize];
            let w = v & 0xFFFF;
            let h = (v >> 16) & 0xFFFF;
            w.checked_mul(h).and_then(|v| v.checked_mul(e as u64))
        }
    }
}

fn lookup(num: u64) -> Option<&'static Desc> {
    if num > u16::MAX as u64 {
        return None;
    }
    let n = num as u16;
    let mut i = 0usize;
    while i < TAB.len() {
        if TAB[i].num == n {
            return Some(&TAB[i]);
        }
        i += 1;
    }
    None
}

/// Validate every declared pointer argument of `num` against the CALLING
/// process's address space. 0 if all pointers are acceptable, -EFAULT if any is
/// not. A syscall with no descriptor returns 0 (NOT validated) - see the module
/// note on the debt ledger.
#[no_mangle]
pub extern "C" fn syscall_validate_args(
    num: u64,
    a1: u64,
    a2: u64,
    a3: u64,
    a4: u64,
    a5: u64,
    a6: u64,
) -> i32 {
    let d = match lookup(num) {
        Some(d) => d,
        None => return 0,
    };
    let raw: [u64; 6] = [a1, a2, a3, a4, a5, a6];

    let mut i = 0usize;
    while i < 6 {
        let a = d.args[i];
        if a.kind == Kind::None {
            i += 1;
            continue;
        }
        // Apply the handler's own 32-bit-compat shim first where it declares
        // one, so what gets validated is what gets dereferenced.
        let p = if a.sx { sanitize_user_ptr(raw[i]) } else { raw[i] };
        // NULL is skipped, not rejected: see judgement (1) at the top.
        if p == 0 {
            i += 1;
            continue;
        }

        let n = match len_bytes(a.len, &raw) {
            Some(n) => n,
            None => return EFAULT, // overflowing length: reject, never wrap.
        };

        let rc = match a.kind {
            Kind::Str => {
                // SAFETY: validate_user_string only walks the caller's page
                // tables and reads bytes it has just proven are present and
                // user-accessible. It never trusts p.
                unsafe { validate_user_string(p as *const u8, n as usize) }
            }
            Kind::R | Kind::W => {
                if n == 0 {
                    // A zero-length buffer is dereferenced by nobody.
                    i += 1;
                    continue;
                }
                if n > usize::MAX as u64 {
                    return EFAULT;
                }
                let access = if a.kind == Kind::W { ACCESS_RW_USER } else { ACCESS_READ_USER };
                // SAFETY: validate_user_ptr does not dereference p. It walks the
                // caller's CR3 and reports the effective U/S + R/W bits.
                unsafe { validate_user_ptr(p as *const u8, n as usize, access) }
            }
            Kind::None => VALIDATE_OK,
        };

        if rc != VALIDATE_OK {
            return EFAULT;
        }
        i += 1;
    }
    0
}

/// Does `num` have a descriptor? Exposed for the in-kernel self-test and for
/// the lint's cross-check that the ledger matches the built kernel.
#[no_mangle]
pub extern "C" fn syscall_desc_covers(num: u64) -> i32 {
    match lookup(num) {
        Some(_) => 1,
        None => 0,
    }
}

/// How many syscalls the table declares. The boot banner prints this so the
/// coverage claim is checkable against the running kernel rather than the docs.
#[no_mangle]
pub extern "C" fn syscall_desc_count() -> u32 {
    TAB.len() as u32
}
