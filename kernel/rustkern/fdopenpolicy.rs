// rustkern/fdopenpolicy.rs - #747: ONE answer, for every filesystem family in
// the kernel, to the two questions an open() has to settle before it hands back
// a descriptor:
//
//     1. does this open DESTROY the file's existing bytes?
//     2. does this descriptor need to hold the WHOLE file, so that it can read
//        what it opened and overwrite part of it without losing the rest?
//
// NEW kernel code, so Rust per the 2026-07-16 rule. There is no C twin to
// strangle and no -DRUST_* flag: before this module the answers were re-derived
// at every site that creates an fd, and the sites DISAGREED.
//
// ===========================================================================
// WHY THIS EXISTS, measured not assumed
// ---------------------------------------------------------------------------
// fs/vfs.h's open_mode_decode() already gave the kernel ONE decoding of the
// flags WORD (#746). It did not give it one decoding of the CONSEQUENCE, and
// four fd families then drew different conclusions from the same `open_mode_t`:
//
//   * ext2 (proc/fdlayer.c, the e2fd family) was fixed twice, at #746 and again
//     at "#745 local 109", and is correct today: an O_RDWR or plain-O_WRONLY
//     open of an existing file loads the whole file into its buffer, starts
//     CLEAN (nothing owed to the medium) and writes at a position.
//
//   * SMB and NFS (the smbfd family) tested the raw bits inline:
//         want_write = (flags & 0x1) || (flags & 0x2) || (flags & 0x40) ||
//                      (flags & 0x200);
//     and then started an EMPTY buffer for every one of them. A plain O_RDWR
//     open of an existing file, with no truncation requested anywhere, uploaded
//     whatever was written as the ENTIRE new file contents on close. Writing 4
//     bytes to a 1 MB document left a 4-byte document, and read() on that same
//     descriptor returned -1, so the caller could not even detect it by reading
//     its own file back.
//
//   * The /MNT aux mounts (the auxfd family) used `om.can_write`, which is the
//     right QUESTION asked of the wrong ANSWER: it also started an empty buffer
//     AND set the dirty flag at open, so on an ext2 aux mount merely opening an
//     existing file O_RDWR and closing it, with no write at all, truncated it
//     to zero.
//
// The flags were decoded once and the meaning was decoded four times. This
// module is the second half of #746.
//
// ===========================================================================
// THE RULE, stated once
// ---------------------------------------------------------------------------
// POSIX shortens a file only when something asks it to. The only thing in an
// open() that asks is O_TRUNC. So `owes_empty` is `m.trunc` and NOTHING ELSE,
// and no other field of the plan may be used by a caller to decide whether to
// empty a file. That is deliberately the same single notion of truncation
// fs/vfs.h already decodes; this module does not add a second one.
//
// ===========================================================================
// WHY `native_append` IS A PARAMETER AND NOT A FIFTH OPINION
// ---------------------------------------------------------------------------
// "Keep the existing bytes" is a requirement on the OPEN. HOW a backend can
// honour it is a property of the BACKEND, and the two genuinely differ:
//
//   * ext2 has ext2_append_file(), so an O_WRONLY|O_APPEND fd can keep an
//     append-only buffer and have the backend add it to the existing file.
//     native_append = 1.
//
//   * SMB, NFS and the /MNT aux mounts commit by REPLACING the whole file
//     (smb_vfs_write_whole / nfs_vfs_write_whole / ext2_write_file_on /
//     blkmgr_aux_fat_write_c). For them an append-only buffer IS a destructive
//     rewrite, so the only way to honour O_APPEND without losing the file is
//     to hold the whole file. native_append = 0.
//
// So the parameter describes a capability the caller HAS, not a policy it
// prefers, and with native_append = 1 this module reproduces the ext2 branch's
// hand-written condition exactly, which is what lets ext2 be routed through it
// with zero behaviour change (proved by fd_open_plan_selftest_rs below and by
// the boot differential in proc/fdlayer.c).
// ===========================================================================

/// Mirror of `open_mode_t` in fs/vfs.h, field for field, in declaration order.
/// Locked by `_Static_assert` on the C side (proc/syscall_argtab_lock.c) and by
/// a size assertion below, per this codebase's existing FFI convention.
///
/// This is READ ONLY from Rust. The decoding stays in fs/vfs.h's
/// open_mode_decode(), which is the one place the flags WORD is interpreted;
/// duplicating that bit arithmetic here would recreate the exact drift this
/// module exists to end.
#[repr(C)]
pub struct OpenMode {
    pub can_read: i32,
    pub can_write: i32,
    pub rdwr: i32,
    pub trunc: i32,
    pub append: i32,
    pub create: i32,
    pub excl: i32,
}

// 7 x i32. If fs/vfs.h gains a field, this fails to compile rather than
// silently reading the wrong offsets.
const _: () = assert!(core::mem::size_of::<OpenMode>() == 28);

/// What the descriptor must be set up to do. Every field is 0 or 1.
#[repr(C)]
pub struct FdOpenPlan {
    /// The descriptor must hold the WHOLE file in one buffer and read and write
    /// it at ONE shared position. 0 means an append-only write buffer (or a
    /// streaming writer) is sufficient.
    pub whole_file: i32,
    /// The buffer must be FILLED from the file's current contents before the
    /// descriptor is handed back. Always 0 when `owes_empty` is 1: there is
    /// nothing to preserve in a file the open just emptied.
    pub preload: i32,
    /// THE ONLY FIELD THAT MAY MAKE A CALLER DESTROY DATA. 1 exactly when
    /// O_TRUNC was asked for.
    pub owes_empty: i32,
    /// O_APPEND: every write lands at end-of-file.
    pub at_end: i32,
    /// The descriptor must refuse read(). An O_WRONLY fd that holds the whole
    /// file could physically serve reads; POSIX forbids it, and handing a
    /// caller bytes its own open said it could not have is how a write-only
    /// descriptor becomes an information leak.
    pub noread: i32,
}

const _: () = assert!(core::mem::size_of::<FdOpenPlan>() == 20);

/// Decide the plan for an open of an EXISTING REGULAR FILE.
///
/// `native_append`: 1 if the backend can add bytes to an existing file without
/// rewriting it, 0 if its only commit is "replace the whole file".
///
/// Returns 0 on success, -1 if either pointer is null (and then writes nothing,
/// so a caller that ignores the return value cannot act on uninitialised
/// memory: it will have an unmodified, caller-initialised plan).
///
/// A read-only open gets an all-zero plan: no buffer, nothing owed, and the
/// caller's own read path handles it. That is correct and is why there is no
/// separate "kind" enum here.
#[no_mangle]
pub extern "C" fn fd_open_plan_rs(m: *const OpenMode, native_append: i32,
                                  out: *mut FdOpenPlan) -> i32 {
    if m.is_null() || out.is_null() {
        return -1;
    }
    // SAFETY: both pointers are non-null and the C side passes an
    // `open_mode_t` whose layout is locked to OpenMode by _Static_assert, and
    // an `fd_open_plan_t` locked to FdOpenPlan the same way.
    let m = unsafe { &*m };

    let can_write = m.can_write != 0;
    let rdwr = m.rdwr != 0;
    let trunc = m.trunc != 0;
    // open_mode_decode() has already cleared `append` when O_TRUNC is set
    // (POSIX: truncate at open, then append == plain sequential writing), so
    // this does not have to re-settle that interaction. It is re-asserted in
    // the self-test, because a future change to the decoder that stopped doing
    // it would otherwise silently change the plan.
    let append = m.append != 0;

    // THE CONDITION. Read it as: a writer needs the whole file unless it is
    // either emptying the file anyway, or it is a pure appender on a backend
    // that can append for real.
    let whole_file = can_write && (rdwr || (!trunc && (!append || native_append == 0)));

    let plan = FdOpenPlan {
        whole_file: whole_file as i32,
        // `!trunc`, not `preload = whole_file`: an O_RDWR|O_TRUNC fd still needs
        // the whole-file buffer (it must be able to read back what it writes),
        // but it must NOT be filled from a file the open has just emptied.
        preload: (whole_file && !trunc) as i32,
        owes_empty: trunc as i32,
        at_end: append as i32,
        noread: (m.can_read == 0) as i32,
    };
    unsafe { *out = plan };
    0
}

/// Boot self-test. Returns the number of MISMATCHES, so 0 is the pass value and
/// a caller that forgets to check still sees a non-zero count in the log.
///
/// These are ABSOLUTE assertions, not a differential against a C twin: #433's
/// lesson in blame.md is that a differential whose two arms share the same
/// wrong assumption scores a meaningless pass, and the wrong assumption is
/// exactly what this module removes.
///
/// The table is written from POSIX plus the two measured defects:
///   * "#745 local 109" (ext2): O_WRONLY with NEITHER O_TRUNC NOR O_APPEND
///     must preserve.
///   * #747 (SMB/NFS//MNT): the same, PLUS O_RDWR, PLUS O_APPEND on a
///     replace-only backend.
#[no_mangle]
pub extern "C" fn fd_open_plan_selftest_rs() -> u32 {
    // Flag values, from fs/vfs.h. Spelled here so this self-test fails if the
    // two files ever disagree about which bit is which.
    const O_RDONLY: i32 = 0x0000;
    const O_WRONLY: i32 = 0x0001;
    const O_RDWR: i32 = 0x0002;
    const O_CREAT: i32 = 0x0040;
    const O_TRUNC: i32 = 0x0200;
    const O_APPEND: i32 = 0x0400;

    // The decoder lives in C (fs/vfs.h). Re-implementing it here would be the
    // drift this module exists to end, so the self-test asks C for it.
    extern "C" {
        fn open_mode_decode_c(flags: i32, out: *mut OpenMode);
    }

    let mut bad: u32 = 0;
    let mut check = |flags: i32, native_append: i32,
                     want: (i32, i32, i32, i32, i32)| {
        let mut m = OpenMode { can_read: 0, can_write: 0, rdwr: 0, trunc: 0,
                               append: 0, create: 0, excl: 0 };
        unsafe { open_mode_decode_c(flags, &mut m) };
        let mut p = FdOpenPlan { whole_file: -1, preload: -1, owes_empty: -1,
                                 at_end: -1, noread: -1 };
        if fd_open_plan_rs(&m, native_append, &mut p) != 0 {
            bad += 1;
            return;
        }
        if (p.whole_file, p.preload, p.owes_empty, p.at_end, p.noread) != want {
            bad += 1;
        }
    };

    // ---- native_append = 1 (ext2). This column IS the shipping ext2
    // behaviour; every row here must stay as it is or ext2 changes.
    // (whole_file, preload, owes_empty, at_end, noread)
    check(O_RDONLY, 1, (0, 0, 0, 0, 0));
    check(O_WRONLY, 1, (1, 1, 0, 0, 1));                 // #745 local 109
    check(O_RDWR, 1, (1, 1, 0, 0, 0));                   // #746
    check(O_WRONLY | O_TRUNC, 1, (0, 0, 1, 0, 1));
    check(O_RDWR | O_TRUNC, 1, (1, 0, 1, 0, 0));
    check(O_WRONLY | O_APPEND, 1, (0, 0, 0, 1, 1));      // ext2_append_file
    check(O_RDWR | O_APPEND, 1, (1, 1, 0, 1, 0));
    check(O_WRONLY | O_CREAT, 1, (1, 1, 0, 0, 1));
    check(O_WRONLY | O_CREAT | O_TRUNC, 1, (0, 0, 1, 0, 1));

    // ---- native_append = 0 (SMB, NFS, /MNT: replace-only backends).
    check(O_RDONLY, 0, (0, 0, 0, 0, 0));
    check(O_WRONLY, 0, (1, 1, 0, 0, 1));
    check(O_RDWR, 0, (1, 1, 0, 0, 0));                   // #747, the report
    check(O_WRONLY | O_TRUNC, 0, (0, 0, 1, 0, 1));
    check(O_RDWR | O_TRUNC, 0, (1, 0, 1, 0, 0));
    // THE ROW THAT DIFFERS BETWEEN THE COLUMNS, and the reason
    // `native_append` is a parameter: on a replace-only backend an
    // append-only buffer is a destructive rewrite.
    check(O_WRONLY | O_APPEND, 0, (1, 1, 0, 1, 1));
    check(O_RDWR | O_APPEND, 0, (1, 1, 0, 1, 0));
    check(O_WRONLY | O_CREAT, 0, (1, 1, 0, 0, 1));

    // O_TRUNC on a nominally read-only open. open_mode_decode() turns that into
    // "may write, STILL may read" and the /CONFIG writers depend on it, so the
    // plan must empty the file and must not take the read away.
    check(O_RDONLY | O_TRUNC, 1, (0, 0, 1, 0, 0));
    check(O_RDONLY | O_TRUNC, 0, (0, 0, 1, 0, 0));

    // O_TRUNC|O_APPEND together: the decoder clears append. If it ever stops,
    // at_end would become 1 here and this row goes red, which is the point.
    check(O_WRONLY | O_TRUNC | O_APPEND, 0, (0, 0, 1, 0, 1));
    check(O_WRONLY | O_TRUNC | O_APPEND, 1, (0, 0, 1, 0, 1));

    // Access mode 3 is not legal POSIX; the decoder reads it as read-write
    // because that is the only reading of it that cannot LOSE data. The plan
    // must follow, or the "cannot lose data" claim stops being true one layer
    // down.
    check(O_WRONLY | O_RDWR, 0, (1, 1, 0, 0, 0));

    // Null arguments must be refused, not acted on.
    let mut p = FdOpenPlan { whole_file: 7, preload: 7, owes_empty: 7,
                             at_end: 7, noread: 7 };
    if fd_open_plan_rs(core::ptr::null(), 0, &mut p) != -1 { bad += 1; }
    if p.whole_file != 7 { bad += 1; }   // must not have been touched
    let m = OpenMode { can_read: 1, can_write: 0, rdwr: 0, trunc: 0,
                       append: 0, create: 0, excl: 0 };
    if fd_open_plan_rs(&m, 0, core::ptr::null_mut()) != -1 { bad += 1; }

    bad
}
