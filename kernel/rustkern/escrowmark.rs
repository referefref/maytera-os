// rustkern/escrowmark.rs - #246/#305: the PURE policy for the escrow actor
// MARK: who inherits it, who may take it off, and what a marked actor's delete
// is allowed to become.
//
// WHY THIS FILE EXISTS. Three escapes from the escrow contract were MEASURED on
// 2026-10-03 and two of them were real:
//
//   1. SELF-EXIT. A Stage-1 (SELF) actor may call SYS_ESCROW_EXIT and walk out
//      of its own contract. That is correct for the self-service class, and
//      WRONG for an AI actor: "the AI must not destroy your files" cannot be a
//      promise the AI itself may cancel. The LOCKED class already refuses it
//      (fs/escrow_guard.c), so the fix is to make the AI path LOCKED and to put
//      the rule in ONE place, which is `escmark_may_self_exit_rs` below.
//
//   2. SPAWN. `kernel/proc/process.c` init_proc() memsets the whole process_t,
//      so a brand-new process starts with escrow_active == 0. fork() and clone()
//      copy the mark through the whole-struct memcpy, and execve() keeps the
//      SAME process_t so the mark survives it, but SPAWN builds a FRESH
//      process_t and therefore DROPPED the mark. A marked actor could spawn a
//      helper and the helper was an ordinary uid-0 process. `escmark_inherit_rs`
//      is the rule that closes it: a child of a marked actor is marked, and is
//      LOCKED whatever the parent was, because an actor must not be able to
//      launder its way out of a contract by spawning a copy that can self-exit.
//
//   3. The userland denylist. `aicap_deny_capability()` had zero callers while
//      its own comment called itself "how the no-delete promise is enforced".
//      That is a documentation defect, not a kernel one: the enforcement moved
//      to Ring 0 and the comment was never updated. Nothing in THIS file can fix
//      a comment, but `escmark_delete_disposition_rs` is the real rule it should
//      have been pointing at all along.
//
// NEW KERNEL LOGIC WITH NO C TWIN, so Rust per the 2026-07-16 rule. There is no
// performance argument for C: every function here is a handful of integer
// compares on the COLD process-creation and FS-delete chokepoints. The STATE
// (process_t, the GraphFS grant tables, the filesystem) stays in C, exactly as
// rustkern/escrowundo.rs splits from fs/escrow_undo.c.

// --- the three mark states. MIRRORED in fs/escrow_guard.h -------------------
pub const ACTIVE_NONE: u8 = 0; //   an ordinary process
pub const ACTIVE_SELF: u8 = 1; //   Stage 1 self-service: may leave via _EXIT
pub const ACTIVE_LOCKED: u8 = 2; // MANDATORY: may NOT clear its own mark
/// RECYCLE-ONLY, added 2026-10-03 for the owner's actual requirement.
///
/// WHY A FOURTH STATE AND NOT ONE OF THE THREE ABOVE. The SELF and LOCKED
/// states are SCOPED CONTRACTS: they carry a GraphFS grant naming one directory
/// prefix, and every write outside it is refused. That is right for a bounded
/// task ("organise the photos on this drive") and completely wrong for the
/// general AI assistant, which the user asks to write files anywhere they like.
/// Marking the assistant LOCKED would have bound it to one directory and broken
/// it; leaving it unmarked, which is the state it shipped in, meant its delete
/// tool reached a raw unlink and destroyed the file.
///
/// RECYCLE is the orthogonal half: deletes are recoverable, writes are NOT
/// scope-restricted, there is no grant and therefore no authority of any kind
/// is conferred. It is a RESTRICTION a process carries, not a permission, which
/// is why it is safe for the kernel to apply it automatically at launch and why
/// nothing in Ring 3 can ask to be let out of it.
///
/// It is IRREVOCABLE and INHERITED, like LOCKED, because an assistant that
/// could drop the restriction, or launder a delete through a program it spawns,
/// would not be restricted at all.
pub const ACTIVE_RECYCLE: u8 = 3;

// --- how a marked actor's attempted DELETE is dispositioned -----------------
// MIRRORED in fs/escrow_guard.h as ESCROW_DISP_*.
//
// This is the clarification the owner made on 2026-10-03, and it changes the
// shape of the guarantee: "AI can delete files but it must be announced to the
// user AND they can be undeleted". So a delete is NOT refused. It SUCCEEDS from
// the caller's point of view and is REDIRECTED into recoverable storage. What
// is binding is that the AI cannot DESTROY, not that it cannot delete.
pub const DISP_DENY: u32 = 0; //    refuse outright (nothing recoverable is possible)
pub const DISP_RECYCLE: u32 = 1; // move into the Recycle Bin, announce, report success
pub const DISP_ALLOW: u32 = 2; //   not an actor: the kernel's business as usual

/// What mark does a NEWLY CREATED process get, given its creating process's
/// mark? `parent_active` is the creator's `process_t.escrow_active`.
///
/// THE RULE, and the reason for each half:
///
/// - An unmarked parent produces an unmarked child. Every ordinary spawn in the
///   system, by the shell, the compositor, the installer and the kernel's own
///   workers, is this case, and it must stay byte-for-byte what it was.
///
/// - A marked parent of EITHER class produces a LOCKED child. Not "the same
///   class": LOCKED. A SELF actor that spawned a SELF child would have gained
///   nothing it did not already have (it can exit itself), but a SELF actor that
///   spawns a child which then exits and deletes has laundered the contract
///   through a process boundary. Promoting to LOCKED costs the honest
///   self-service case nothing it can observe (its child was going to work
///   inside the same scope anyway) and closes the laundering path by
///   construction rather than by a check someone has to remember to write.
///
/// Returns the child's `escrow_active`.
#[no_mangle]
pub extern "C" fn escmark_inherit_rs(parent_active: u8) -> u8 {
    match parent_active {
        ACTIVE_SELF | ACTIVE_LOCKED => ACTIVE_LOCKED,
        // RECYCLE stays RECYCLE and is NOT promoted to LOCKED. Promoting it
        // would hand the child a scope restriction against a grant it does not
        // have, so every write it made would be refused: the child would be
        // broken rather than contained. RECYCLE is already irrevocable, so
        // there is nothing to harden.
        ACTIVE_RECYCLE => ACTIVE_RECYCLE,
        _ => ACTIVE_NONE,
    }
}

/// Does a process in this state have its WRITES restricted to a granted scope?
///
/// 1 = yes (a scoped contract: SELF or LOCKED), 0 = no. This exists so that the
/// FS guard has one place to ask, rather than each branch re-deriving "which of
/// the mark states are contracts" and one of them getting it wrong. A RECYCLE
/// process writes wherever its ordinary filesystem permissions allow, exactly
/// like an unmarked process; only its DELETES behave differently.
#[no_mangle]
pub extern "C" fn escmark_write_scoped_rs(active: u8) -> u32 {
    match active {
        ACTIVE_SELF | ACTIVE_LOCKED => 1,
        _ => 0,
    }
}

/// Is `path` a program the KERNEL binds into RECYCLE mode at launch?
///
/// THIS IS THE LIST THAT MAKES THE GUARANTEE NON-VOLUNTARY. The alternative
/// design was a syscall the AI host calls on itself at startup, and that would
/// have been strictly weaker: a program that simply never calls it is
/// unrestricted, and "the AI chooses to restrict itself" is the thing the owner
/// specifically did not ask for. Here the kernel decides, from the program it
/// was asked to launch, before the program's first instruction runs.
///
/// HONEST LIMITS, because a path list is a path list:
///
/// - It binds the SHIPPING AI assistant. It does not bind an arbitrary binary
///   somebody writes that talks to a model API itself. In a system where every
///   Ring-3 process can run as uid 0, nothing could.
/// - A copy of the assistant under a different name is not matched. That is a
///   smaller hole than it looks: the assistant is bound, so anything IT spawns
///   inherits the mark, including a copy of itself. The unmatched case is a
///   human deliberately renaming the program, which is a human choosing to
///   turn the protection off for that copy, not an AI escaping it.
/// - The match is CASE-INSENSITIVE on the canonical absolute path the loader was
///   given. MEASURED on golden 2551 by loop-mounting p2 and listing /APPS: the
///   installed entries are `AICHAT` and `AIDEMO`, both uppercase, and
///   `AICHAT_APP_PATH` in the compositor is the literal "/APPS/AICHAT". So an
///   exact match would have worked for the shipping image. It is
///   case-insensitive anyway, because the App Store's own package comment
///   (userland/apps/appstore/main.c) says the aichat package installs BOTH
///   `/APPS/AICHAT` and `/APPS/aichat`, and `aidemo`'s Makefile TARGET is
///   lowercase while `aibench`'s is uppercase. A protection that depends on
///   which spelling a user happened to install through is not a protection.
///
/// # Safety
/// `path` must be a NUL-terminated C string, or null.
#[no_mangle]
pub unsafe extern "C" fn escmark_is_ai_program_rs(path: *const u8) -> u32 {
    if path.is_null() {
        return 0;
    }
    // The shipping AI surfaces. Keep this short and keep it exact: every entry
    // is a program whose FILE DELETIONS are driven by model output.
    const AI: [&[u8]; 3] = [b"/APPS/AICHAT", b"/APPS/AIDEMO", b"/APPS/AIBENCH"];
    let mut i = 0usize;
    while i < AI.len() {
        let want = AI[i];
        let mut k = 0usize;
        let mut same = true;
        while k < want.len() {
            // SAFETY: the caller guarantees a NUL-terminated string; we stop at
            // the first mismatch, and a NUL is a mismatch against any byte of
            // `want`, so we never read past the terminator.
            let c = unsafe { *path.add(k) };
            // ASCII case fold. `want` is uppercase, so folding the candidate's
            // lowercase letters up is enough; nothing else is touched, so '/'
            // and '.' still have to match exactly.
            let cu = if c.is_ascii_lowercase() { c - 32 } else { c };
            if cu != want[k] {
                same = false;
                break;
            }
            k += 1;
        }
        // SAFETY: as above. If every byte matched, index `want.len()` is at most
        // the position of the NUL, which is readable.
        if same && unsafe { *path.add(want.len()) } == 0 {
            return 1;
        }
        i += 1;
    }
    0
}

/// Does a child created by a parent in this state need the parent's contract
/// identity copied across? Exactly when the inherited mark is non-zero. Split
/// out so the C glue in init_proc() has a single boolean to branch on and
/// cannot accidentally copy a scope string onto an unmarked process.
#[no_mangle]
pub extern "C" fn escmark_inherits_rs(parent_active: u8) -> u32 {
    if escmark_inherit_rs(parent_active) == ACTIVE_NONE {
        0
    } else {
        1
    }
}

/// May a process in this state clear its own mark (SYS_ESCROW_EXIT)?
///
/// 1 = yes, 0 = no. ONE definition, so the Stage-1 self-service allowance and
/// the Stage-2 mandatory refusal cannot drift apart in two files.
#[no_mangle]
pub extern "C" fn escmark_may_self_exit_rs(active: u8) -> u32 {
    match active {
        ACTIVE_SELF => 1,
        _ => 0,
    }
}

/// How should a DELETE attempted by a process in this state be dispositioned?
///
/// `active`      the caller's escrow_active
/// `is_dir`      1 if the target is a directory (rmdir), 0 for a file (unlink)
/// `recoverable` 1 if the Recycle Bin is available to receive the file RIGHT
///               NOW (mounted, resolvable, writable). The C glue answers this;
///               the policy only decides what to do with the answer.
///
/// THE RULE:
///
/// - Not an actor -> ALLOW. An ordinary process's delete is not this layer's
///   business and the FS path must be unchanged for it.
///
/// - An actor deleting a FILE, with a usable bin -> RECYCLE. The call succeeds,
///   the bytes are kept, the user is told, and either the user or the AI can put
///   it back. This is the owner's stated requirement.
///
/// - An actor deleting a file with NO usable bin -> DENY. Fail closed. If the
///   kernel cannot promise the bytes are recoverable it does not let them go.
///   A delete that silently became unrecoverable because the bin was missing
///   would be exactly the kind of quiet hole this work exists to remove.
///
/// - An actor removing a DIRECTORY -> DENY. NAMED, DELIBERATE EXCEPTION. The
///   Recycle Bin is a FLAT store of files with a per-file index row; it has no
///   representation for a directory tree, so there is no recoverable form to
///   redirect an rmdir into. Refusing is the honest answer. Note that this costs
///   an actor very little in practice: ext2's rmdir already refuses a non-empty
///   directory, so the only thing being refused here is removing an EMPTY one.
#[no_mangle]
pub extern "C" fn escmark_delete_disposition_rs(active: u8, is_dir: u32, recoverable: u32) -> u32 {
    if active == ACTIVE_NONE {
        return DISP_ALLOW;
    }
    if is_dir != 0 {
        // A DIRECTORY. The two mark classes answer differently, and the reason
        // is what each one is for.
        //
        // A SCOPED CONTRACT (SELF/LOCKED) is refused, unchanged. Such a
        // contract is narrow and short-lived by construction, removing a
        // directory is not part of any contract issued today, and refusing is
        // the conservative answer for an actor that was handed a specific job.
        //
        // RECYCLE mode is ALLOWED, and that is not a weakening: ext2's rmdir
        // refuses a non-empty directory, so the only thing being permitted is
        // removing an EMPTY one, which contains no bytes to lose. Refusing it
        // would break ordinary tidying the user asked the assistant to do, in
        // exchange for protecting nothing. If rmdir ever stops refusing
        // non-empty directories, THIS LINE becomes wrong and must change with
        // it; that dependency is the reason it is written down here rather than
        // assumed.
        return if active == ACTIVE_RECYCLE {
            DISP_ALLOW
        } else {
            DISP_DENY
        };
    }
    if recoverable != 0 {
        DISP_RECYCLE
    } else {
        DISP_DENY
    }
}

// --- how a marked actor's attempted FULL-FILE REPLACEMENT is dispositioned ---
// MIRRORED in fs/escrow_guard.h as ESCROW_REPL_*.
//
// WHY THIS IS A SEPARATE RULE FROM THE DELETE ONE. A full-file replacement
// (SYS_PKG_WRITE, which takes a path, a buffer and a length and replaces the
// whole named file) is not a delete at any layer, so the delete chokepoint
// never sees it, exactly as a rename-over and an O_TRUNC open never did. It
// also differs from both of those in one way that changes the answer: it can
// still FAIL before a byte is written, so the pre-image must be a COPY and the
// original must be left in place. See escrow_guard.c for that argument in full.
pub const REPL_SKIP: u32 = 0; //     nothing is at risk: proceed untouched
pub const REPL_SNAPSHOT: u32 = 1; // COPY the current contents to the bin first
pub const REPL_DENY: u32 = 2; //     no recoverable form is possible: refuse

/// How should a FULL-FILE REPLACEMENT of `path` by a process in this state be
/// dispositioned?
///
/// `active`      the caller's escrow_active
/// `exists`      1 if something is already at the target path
/// `is_dir`      1 if that something is a DIRECTORY
/// `cur_len`     the current length in bytes of the file being replaced
/// `recoverable` 1 if the Recycle Bin can receive a copy RIGHT NOW. The C glue
///               answers this; the policy only decides what to do with it.
///
/// THE RULE, and why each branch is what it is:
///
/// - Not an actor -> SKIP. An ordinary process's package write is not this
///   layer's business, and the App Store, `updated` and `otaupd` must be
///   byte-for-byte unaffected. This branch is why the cost of the whole hook
///   for an unmarked caller is one `process_t` byte.
///
/// - Nothing at the path -> SKIP. A replacement that creates a new file
///   destroys nothing. There is no pre-image to keep and manufacturing a
///   zero-byte bin entry for every package install would make the bin useless.
///
/// - A DIRECTORY at the path -> SKIP, deliberately, and NOT deny. The
///   filesystem already refuses to write a file over a directory, so the
///   refusal exists and is the filesystem's to give. Denying here as well would
///   put a bin entry, or a second refusal, on a path that was never going to
///   destroy anything. This differs from the rename-over route, which DENIES a
///   directory destination, because `rename` over a directory is a call the
///   filesystem would otherwise carry out.
///
/// - An EMPTY file -> SKIP. Replacing zero bytes loses zero bytes. Same reason
///   the O_TRUNC route skips it: without this, every reinstall over a
///   zero-length placeholder drops an empty entry in the user's bin.
///
/// - A non-empty file with NO usable bin -> DENY. Fail closed. This is the
///   same answer the delete, truncate, rename-over and shrink routes give, and
///   for the same reason: if the kernel cannot promise the bytes are
///   recoverable it does not let them go.
///
/// - Otherwise -> SNAPSHOT. The current contents are COPIED into the Recycle
///   Bin and the original is left where it is, and then the replacement
///   proceeds. A COPY and not a MOVE: see escrow_guard.c.
#[no_mangle]
pub extern "C" fn escmark_replace_disposition_rs(
    active: u8,
    exists: u32,
    is_dir: u32,
    cur_len: u32,
    recoverable: u32,
) -> u32 {
    if active == ACTIVE_NONE {
        return REPL_SKIP;
    }
    if exists == 0 || is_dir != 0 || cur_len == 0 {
        return REPL_SKIP;
    }
    if recoverable != 0 {
        REPL_SNAPSHOT
    } else {
        REPL_DENY
    }
}

// --- how a marked actor's attempted PARTIAL OVERWRITE is dispositioned ------
// MIRRORED in fs/escrow_guard.h as ESCROW_OVW_*.
//
// WHY THIS IS A SEPARATE RULE AGAIN, and why the thing it keys on is the write
// RANGE rather than the call.
//
// Every other route in this family is decided by the CALL: an unlink, a rmdir,
// a rename, an O_TRUNC flag, an ftruncate length, a whole-file replacement.
// Each of those announces, in the call itself, that bytes are going away. A
// plain write() announces nothing of the kind, which is exactly why
// docs/AI_ESCROW_PROMISE.md listed it as the one remaining hole: at the moment
// a file is OPENED for writing, a writer that will overwrite cannot be told
// from one that will only append or write nothing.
//
// That is true, and it is a statement about OPEN TIME ONLY. By the time a
// write() arrives, the position and the length are both known, and the
// question "will this destroy bytes that exist" has an exact answer:
// `pos < orig_len`. So the hole is not closed by snapshotting at open, which
// would copy every file an AI merely opens; it is closed by deciding at the
// FIRST write that lands inside the original extent, and never again on that
// descriptor.
//
// `orig_len` is the length the file had BEFORE this descriptor touched it, not
// its current length. That distinction is the whole rule: a descriptor that has
// appended 1 MB still has a 40-byte original extent to protect, and one that
// has already been captured has none.
pub const OVW_SKIP: u32 = 0; //     nothing that exists is at risk: proceed
pub const OVW_SNAPSHOT: u32 = 1; // keep a COPY of the pre-image, then proceed
pub const OVW_DENY: u32 = 2; //     no recoverable form is possible: refuse

/// How should a write of `count` bytes at `pos` by a process in this state be
/// dispositioned, on a file whose length before this descriptor opened it was
/// `orig_len`?
///
/// `active`       the caller's escrow_active
/// `orig_len`     the file's length BEFORE this descriptor wrote anything. 0
///                means the file did not exist, was empty, or was emptied by
///                an O_TRUNC that has already been captured by its own route.
/// `pos`          the byte offset this write starts at
/// `count`        how many bytes it writes
/// `already_kept` 1 if a pre-image for THIS descriptor has already been kept
/// `recoverable`  1 if the Recycle Bin can receive a copy right now. The C glue
///                answers this; the policy only decides what to do with it.
///
/// THE RULE, branch by branch:
///
/// - Not an actor -> SKIP. The cost of this whole hook for the compositor, the
///   shell, Files, every app and every /CONFIG writer is one `process_t` byte
///   read at the call site, and this is the branch that guarantees it.
///
/// - Already kept -> SKIP. ONE copy per descriptor, not one per write(). A
///   text editor saving a file issues many writes; a bin with one entry per
///   write would be unusable and would multiply the cost by the write count.
///
/// - Nothing was there (`orig_len == 0`) -> SKIP. A create, an empty file, or
///   a file an O_TRUNC route has already captured. Writing into zero bytes
///   loses zero bytes.
///
/// - A zero-length write -> SKIP. POSIX makes it a successful no-op and it
///   changes nothing.
///
/// - `pos >= orig_len` -> SKIP. THE ARM THAT KEEPS THE PRICE HONEST. Every
///   byte this write touches is past the end of what was there, so it is an
///   append or a hole-extension and it destroys nothing. An append-only writer,
///   including one that appends through an O_RDWR descriptor by seeking to the
///   end, therefore pays NOTHING: no copy, no read, no bin entry. This is the
///   arm /APPS/ESCRVT's E9d and E9e measure.
///
/// - Existing bytes are at risk and there is no usable bin -> DENY. Fail
///   closed, the same answer the delete, truncate, rename-over, shrink and
///   replace routes all give: if the kernel cannot promise the bytes are
///   recoverable, it does not let them go.
///
/// - Otherwise -> SNAPSHOT. A COPY, never a move: the descriptor is open and
///   the caller expects its file to still be there.
///
/// NOTE ON `pos + count` OVERFLOW: both are u64 and the only comparison made is
/// `pos >= orig_len`, so there is no sum to overflow. A deliberately enormous
/// `pos` lands in the SKIP arm, which is correct (it writes past the end) and
/// is also the arm the filesystem will refuse for its own reasons.
#[no_mangle]
pub extern "C" fn escmark_overwrite_disposition_rs(
    active: u8,
    orig_len: u64,
    pos: u64,
    count: u64,
    already_kept: u32,
    recoverable: u32,
) -> u32 {
    if active == ACTIVE_NONE {
        return OVW_SKIP;
    }
    if already_kept != 0 || orig_len == 0 || count == 0 {
        return OVW_SKIP;
    }
    if pos >= orig_len {
        return OVW_SKIP;
    }
    if recoverable != 0 {
        OVW_SNAPSHOT
    } else {
        OVW_DENY
    }
}

// --- what a VICTIM LOOKUP means, for the four routes that have to LOOK -------
// MIRRORED in fs/escrow_guard.h as ESCROW_VIC_*.
//
// WHY THIS RULE EXISTS, measured on 2026-10-04 (#246, usbescrow).
//
// Four of the five non-delete destruction routes have to answer "is there a
// file at this path, and how big is it" before they can decide anything. All
// four asked `fat_exists(&g_fat_fs, path)`, and `g_fat_fs` routes the ext2
// root, the FAT ESP, /SMB, /NFS and a mounted disk image but NOT a hot-plugged
// removable volume: that path split lives in drivers/hotplug.c. So for a file
// on a plugged-in USB stick the lookup said ABSENT, the route concluded
// "nothing to capture", and the destruction went ahead with no copy kept and
// nothing announced. FAILING OPEN, which is the one direction this subsystem
// must never fail in. MEASURED on a booted VM with a synthetic stick before
// this rule existed: a truncating open, an ftruncate shrink, a partial write
// and a rename-over all destroyed bytes on the volume and the kernel printed
// not one line about any of them.
//
// THE INCONSISTENCY THAT MADE IT WORTH A RULE RATHER THAN A PATCH. Elsewhere in
// this same subsystem an unreachable medium is REFUSED: escrow_shrink_victim,
// escrow_replace_victim and escrow_overwrite_victim all deny outright on /SMB,
// /NFS and /MNT because "the one thing that must not happen is a destructive
// path staying destructive because it was out of reach". The lookup gate did
// the exact opposite on a removable volume. Two opposite answers to the same
// question in one subsystem is not a judgement call, it is one of them being
// wrong, and the fail-closed one is the one that matches the promise.
//
// So: a volume that MATCHED a removable mount but cannot be read (an exFAT
// stick the kernel mounts but cannot serve files from) is UNREACHABLE and the
// route must refuse. A volume that was read and has nothing at the path is
// ABSENT, which is not a failure at all: the file genuinely is not there and
// there is nothing to keep.
pub const VIC_ABSENT: u32 = 0; //      nothing is there: proceed, keep nothing
pub const VIC_PRESENT: u32 = 1; //     a file is there: the route's rule decides
pub const VIC_UNREACHABLE: u32 = 2; // nothing can read it: REFUSE, change nothing

/// What should a route do with the result of its victim lookup?
///
/// `active`   the caller's escrow_active
/// `stat_rc`  what the volume-aware lookup returned: 1 present, 0 absent,
///            NEGATIVE for "this path is on a volume nothing here can read".
///            fs/recycle_fs.c `recycle_victim_stat()` is that lookup; it is one
///            function because a second copy of "which volume serves this path"
///            is how one copy comes to disagree with the other about where a
///            user's file is.
///
/// An unmarked process gets ABSENT whatever the lookup said, so the cost of the
/// whole gate for every ordinary process stays one `process_t` byte plus this
/// compare, and in particular an unmarked process is never REFUSED anything
/// because of a medium it was always allowed to write.
#[no_mangle]
pub extern "C" fn escmark_victim_disposition_rs(active: u8, stat_rc: i32) -> u32 {
    if active == ACTIVE_NONE {
        return VIC_ABSENT;
    }
    if stat_rc < 0 {
        return VIC_UNREACHABLE;
    }
    if stat_rc == 1 {
        VIC_PRESENT
    } else {
        VIC_ABSENT
    }
}

/// Boot self-test of the pure policy. Returns 0 on PASS, the negative code of
/// the FIRST failed assertion otherwise; writes the number of assertions run to
/// `*out_checks` when non-null. Driven by fs/recycle_fs.c recycle_selftest().
///
/// # Safety
/// `out_checks` must be null or point to a writable u32.
#[no_mangle]
pub unsafe extern "C" fn escmark_policy_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut checks: u32 = 0;
    let mut fail: i32 = 0;
    macro_rules! chk {
        ($cond:expr, $code:expr) => {{
            checks += 1;
            if !($cond) && fail == 0 {
                fail = $code;
            }
        }};
    }

    // --- inheritance: the SPAWN escape ------------------------------------
    chk!(escmark_inherit_rs(ACTIVE_NONE) == ACTIVE_NONE, -1);
    chk!(escmark_inherit_rs(ACTIVE_SELF) == ACTIVE_LOCKED, -2);
    chk!(escmark_inherit_rs(ACTIVE_LOCKED) == ACTIVE_LOCKED, -3);
    // A garbage value must not become authority to escape: anything that is not
    // a known marked state is treated as unmarked, never as "not locked".
    chk!(escmark_inherit_rs(99) == ACTIVE_NONE, -4);
    chk!(escmark_inherits_rs(ACTIVE_NONE) == 0, -5);
    chk!(escmark_inherits_rs(ACTIVE_SELF) == 1, -6);
    chk!(escmark_inherits_rs(ACTIVE_LOCKED) == 1, -7);

    // --- self-exit: the ANNOUNCE-AND-LEAVE escape --------------------------
    chk!(escmark_may_self_exit_rs(ACTIVE_SELF) == 1, -8);
    chk!(escmark_may_self_exit_rs(ACTIVE_LOCKED) == 0, -9);
    chk!(escmark_may_self_exit_rs(ACTIVE_NONE) == 0, -10);

    // --- delete disposition -------------------------------------------------
    // a non-actor is untouched, whatever the bin is doing
    chk!(escmark_delete_disposition_rs(ACTIVE_NONE, 0, 0) == DISP_ALLOW, -11);
    chk!(escmark_delete_disposition_rs(ACTIVE_NONE, 1, 1) == DISP_ALLOW, -12);
    // an actor's file delete with a usable bin is REDIRECTED, not refused
    chk!(escmark_delete_disposition_rs(ACTIVE_SELF, 0, 1) == DISP_RECYCLE, -13);
    chk!(escmark_delete_disposition_rs(ACTIVE_LOCKED, 0, 1) == DISP_RECYCLE, -14);
    // ... and REFUSED if the bytes could not be kept. Fail closed.
    chk!(escmark_delete_disposition_rs(ACTIVE_LOCKED, 0, 0) == DISP_DENY, -15);
    // a directory is the named exception, with or without a bin
    chk!(escmark_delete_disposition_rs(ACTIVE_LOCKED, 1, 1) == DISP_DENY, -16);
    chk!(escmark_delete_disposition_rs(ACTIVE_SELF, 1, 1) == DISP_DENY, -17);

    // --- RECYCLE mode: the general AI assistant ----------------------------
    // irrevocable, inherited as itself, and NOT promoted to a scoped contract
    chk!(escmark_inherit_rs(ACTIVE_RECYCLE) == ACTIVE_RECYCLE, -20);
    chk!(escmark_inherits_rs(ACTIVE_RECYCLE) == 1, -21);
    chk!(escmark_may_self_exit_rs(ACTIVE_RECYCLE) == 0, -22);
    // writes are NOT scope-restricted in RECYCLE mode, and ARE in a contract
    chk!(escmark_write_scoped_rs(ACTIVE_RECYCLE) == 0, -23);
    chk!(escmark_write_scoped_rs(ACTIVE_NONE) == 0, -24);
    chk!(escmark_write_scoped_rs(ACTIVE_SELF) == 1, -25);
    chk!(escmark_write_scoped_rs(ACTIVE_LOCKED) == 1, -26);
    // a file delete is redirected, and refused if nothing can receive it
    chk!(escmark_delete_disposition_rs(ACTIVE_RECYCLE, 0, 1) == DISP_RECYCLE, -27);
    chk!(escmark_delete_disposition_rs(ACTIVE_RECYCLE, 0, 0) == DISP_DENY, -28);
    // an EMPTY directory is allowed in RECYCLE mode and refused in a contract
    chk!(escmark_delete_disposition_rs(ACTIVE_RECYCLE, 1, 1) == DISP_ALLOW, -29);

    // --- the kernel's AI program list ---------------------------------------
    // SAFETY: every pointer below is a real NUL-terminated byte literal.
    unsafe {
        let a = b"/APPS/AICHAT\0";
        chk!(escmark_is_ai_program_rs(a.as_ptr()) == 1, -30);
        // a PREFIX of a listed name must not match, or /APPS/AI would bind
        let p = b"/APPS/AICH\0";
        chk!(escmark_is_ai_program_rs(p.as_ptr()) == 0, -31);
        // nor must a longer name that merely starts with one
        let l = b"/APPS/AICHATX\0";
        chk!(escmark_is_ai_program_rs(l.as_ptr()) == 0, -32);
        // CASE-INSENSITIVE: the App Store installs a lowercase alias
        let lc = b"/APPS/aichat\0";
        chk!(escmark_is_ai_program_rs(lc.as_ptr()) == 1, -36);
        let mc = b"/APPS/AiChat\0";
        chk!(escmark_is_ai_program_rs(mc.as_ptr()) == 1, -37);
        // ... but folding must not make unrelated bytes match: a path whose
        // separator differs is a different path
        let sep = b"/APPS.AICHAT\0";
        chk!(escmark_is_ai_program_rs(sep.as_ptr()) == 0, -38);
        // an ordinary program is untouched
        let o = b"/APPS/FILES\0";
        chk!(escmark_is_ai_program_rs(o.as_ptr()) == 0, -33);
        chk!(escmark_is_ai_program_rs(core::ptr::null()) == 0, -34);
        // the empty string must not match anything
        let e = b"\0";
        chk!(escmark_is_ai_program_rs(e.as_ptr()) == 0, -35);
    }

    // --- full-file replacement (SYS_PKG_WRITE): the #246 pkgwrite route ----
    // A NON-ACTOR is untouched in every combination, including the one where a
    // naive implementation would snapshot: an existing non-empty file with a
    // usable bin. This is the assertion that the App Store is unaffected.
    chk!(escmark_replace_disposition_rs(ACTIVE_NONE, 1, 0, 4096, 1) == REPL_SKIP, -40);
    chk!(escmark_replace_disposition_rs(ACTIVE_NONE, 0, 0, 0, 0) == REPL_SKIP, -41);
    // An actor replacing an existing non-empty file keeps a COPY first.
    chk!(escmark_replace_disposition_rs(ACTIVE_RECYCLE, 1, 0, 1, 1) == REPL_SNAPSHOT, -42);
    chk!(escmark_replace_disposition_rs(ACTIVE_LOCKED, 1, 0, 4096, 1) == REPL_SNAPSHOT, -43);
    chk!(escmark_replace_disposition_rs(ACTIVE_SELF, 1, 0, 4096, 1) == REPL_SNAPSHOT, -44);
    // ... and is REFUSED when nothing can receive the copy. Fail closed.
    chk!(escmark_replace_disposition_rs(ACTIVE_RECYCLE, 1, 0, 4096, 0) == REPL_DENY, -45);
    chk!(escmark_replace_disposition_rs(ACTIVE_LOCKED, 1, 0, 1, 0) == REPL_DENY, -46);
    // Creating a NEW file destroys nothing, bin or no bin.
    chk!(escmark_replace_disposition_rs(ACTIVE_RECYCLE, 0, 0, 0, 1) == REPL_SKIP, -47);
    chk!(escmark_replace_disposition_rs(ACTIVE_RECYCLE, 0, 0, 0, 0) == REPL_SKIP, -48);
    // An EMPTY file has no pre-image worth keeping, so no bin entry is made
    // and, critically, a missing bin does not refuse the write either.
    chk!(escmark_replace_disposition_rs(ACTIVE_RECYCLE, 1, 0, 0, 1) == REPL_SKIP, -49);
    chk!(escmark_replace_disposition_rs(ACTIVE_RECYCLE, 1, 0, 0, 0) == REPL_SKIP, -50);
    // A DIRECTORY is SKIPPED, not denied: the filesystem's refusal is the
    // refusal. Asserted for both bin states so the branch cannot be read as
    // depending on the bin.
    chk!(escmark_replace_disposition_rs(ACTIVE_RECYCLE, 1, 1, 4096, 1) == REPL_SKIP, -51);
    chk!(escmark_replace_disposition_rs(ACTIVE_LOCKED, 1, 1, 4096, 0) == REPL_SKIP, -52);
    // An UNKNOWN mark value must not become a way out: anything non-zero is
    // treated as marked here, which is the fail-closed direction (the inherit
    // rule treats garbage as unmarked, which is fail-closed for AUTHORITY;
    // this rule treats it as marked, which is fail-closed for DESTRUCTION).
    chk!(escmark_replace_disposition_rs(99, 1, 0, 4096, 1) == REPL_SNAPSHOT, -53);
    chk!(escmark_replace_disposition_rs(99, 1, 0, 4096, 0) == REPL_DENY, -54);

    // --- partial overwrite (a plain write into existing bytes): #246 partialwrite
    // A NON-ACTOR is untouched in the one combination a naive implementation
    // would snapshot: an overwrite of the middle of an existing file with a
    // usable bin. This is the assertion that every ordinary app is unaffected.
    chk!(escmark_overwrite_disposition_rs(ACTIVE_NONE, 4096, 0, 8, 0, 1) == OVW_SKIP, -60);
    chk!(escmark_overwrite_disposition_rs(ACTIVE_NONE, 4096, 2048, 8, 0, 0) == OVW_SKIP, -61);
    // An actor overwriting bytes that exist keeps a COPY first, from byte 0 and
    // from the middle alike.
    chk!(escmark_overwrite_disposition_rs(ACTIVE_RECYCLE, 53, 8, 8, 0, 1) == OVW_SNAPSHOT, -62);
    chk!(escmark_overwrite_disposition_rs(ACTIVE_LOCKED, 53, 0, 4, 0, 1) == OVW_SNAPSHOT, -63);
    chk!(escmark_overwrite_disposition_rs(ACTIVE_SELF, 1, 0, 1, 0, 1) == OVW_SNAPSHOT, -64);
    // The LAST byte of the original extent is still an existing byte.
    chk!(escmark_overwrite_disposition_rs(ACTIVE_RECYCLE, 53, 52, 1, 0, 1) == OVW_SNAPSHOT, -65);
    // ... and is REFUSED when nothing can receive the copy. Fail closed.
    chk!(escmark_overwrite_disposition_rs(ACTIVE_RECYCLE, 53, 8, 8, 0, 0) == OVW_DENY, -66);
    chk!(escmark_overwrite_disposition_rs(ACTIVE_LOCKED, 53, 0, 1, 0, 0) == OVW_DENY, -67);
    // AN APPEND PAYS NOTHING. pos exactly AT the end, and past it, and with no
    // bin at all: all SKIP, so an append is never refused for want of a bin.
    chk!(escmark_overwrite_disposition_rs(ACTIVE_RECYCLE, 53, 53, 5, 0, 1) == OVW_SKIP, -68);
    chk!(escmark_overwrite_disposition_rs(ACTIVE_RECYCLE, 53, 99, 5, 0, 1) == OVW_SKIP, -69);
    chk!(escmark_overwrite_disposition_rs(ACTIVE_RECYCLE, 53, 53, 5, 0, 0) == OVW_SKIP, -70);
    // A CREATE or an already-captured O_TRUNC has orig_len 0: nothing to lose,
    // and critically not a refusal either, bin or no bin.
    chk!(escmark_overwrite_disposition_rs(ACTIVE_RECYCLE, 0, 0, 4096, 0, 1) == OVW_SKIP, -71);
    chk!(escmark_overwrite_disposition_rs(ACTIVE_RECYCLE, 0, 0, 4096, 0, 0) == OVW_SKIP, -72);
    // ONE COPY PER DESCRIPTOR: once kept, every later write is free, including
    // one that would otherwise be refused for want of a bin.
    chk!(escmark_overwrite_disposition_rs(ACTIVE_RECYCLE, 53, 0, 8, 1, 1) == OVW_SKIP, -73);
    chk!(escmark_overwrite_disposition_rs(ACTIVE_RECYCLE, 53, 0, 8, 1, 0) == OVW_SKIP, -74);
    // A zero-length write is a no-op and must not cost a copy.
    chk!(escmark_overwrite_disposition_rs(ACTIVE_RECYCLE, 53, 0, 0, 0, 1) == OVW_SKIP, -75);
    // An UNKNOWN mark value is treated as MARKED here, which is the fail-closed
    // direction for DESTRUCTION (the same choice the replace rule makes).
    chk!(escmark_overwrite_disposition_rs(99, 53, 8, 8, 0, 1) == OVW_SNAPSHOT, -76);
    chk!(escmark_overwrite_disposition_rs(99, 53, 8, 8, 0, 0) == OVW_DENY, -77);
    // A 4 GB offset must not alias: u64 throughout, so a position above 2^32
    // inside a (hypothetically) larger original extent is still an overwrite.
    chk!(escmark_overwrite_disposition_rs(ACTIVE_RECYCLE, 0x1_0000_0000u64,
                                         0xFFFF_FFFFu64, 8, 0, 1) == OVW_SNAPSHOT, -78);

    // --- the victim-lookup rule (usbescrow) --------------------------------
    // An unmarked process is never refused and never costs a capture, whatever
    // the medium said, including the unreachable case.
    chk!(escmark_victim_disposition_rs(ACTIVE_NONE, 1) == VIC_ABSENT, -80);
    chk!(escmark_victim_disposition_rs(ACTIVE_NONE, 0) == VIC_ABSENT, -81);
    chk!(escmark_victim_disposition_rs(ACTIVE_NONE, -1) == VIC_ABSENT, -82);
    // A marked actor: present means the route's own rule decides next.
    chk!(escmark_victim_disposition_rs(ACTIVE_RECYCLE, 1) == VIC_PRESENT, -83);
    chk!(escmark_victim_disposition_rs(ACTIVE_LOCKED, 1) == VIC_PRESENT, -84);
    chk!(escmark_victim_disposition_rs(ACTIVE_SELF, 1) == VIC_PRESENT, -85);
    // ABSENT is not a failure: there is genuinely nothing to keep. This is the
    // branch the four routes used to reach for a file on a USB stick.
    chk!(escmark_victim_disposition_rs(ACTIVE_RECYCLE, 0) == VIC_ABSENT, -86);
    // UNREACHABLE is a REFUSAL, matching what /SMB, /NFS and /MNT already get.
    chk!(escmark_victim_disposition_rs(ACTIVE_RECYCLE, -1) == VIC_UNREACHABLE, -87);
    chk!(escmark_victim_disposition_rs(ACTIVE_LOCKED, -9) == VIC_UNREACHABLE, -88);
    // An UNKNOWN mark value is treated as MARKED, the fail-closed direction for
    // destruction, exactly as the replace and overwrite rules treat it.
    chk!(escmark_victim_disposition_rs(99, -1) == VIC_UNREACHABLE, -89);
    chk!(escmark_victim_disposition_rs(99, 1) == VIC_PRESENT, -90);

    if !out_checks.is_null() {
        unsafe {
            *out_checks = checks;
        }
    }
    fail
}
