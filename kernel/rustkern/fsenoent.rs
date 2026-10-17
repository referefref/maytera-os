// rustkern/fsenoent.rs - #aicappersist: WHY did an ext2 create or lookup answer
// ENOENT, when the three answers are indistinguishable in the caller's -2.
//
// WHY THIS EXISTS
//
// A Ring-3 `open(path, O_WRONLY|O_CREAT|O_TRUNC)` on the ext2 root can return
// -2 for reasons that are not the same bug and do not have the same fix:
//
//   1. the path never reached the kernel at all (sc_path_from_user failed, and
//      proc/fdlayer.c maps that to -MOS_ENOENT),
//   2. the parent directory really is absent, which makes -2 CORRECT and the
//      CALLER wrong,
//   3. the parent directory EXISTS on the medium but the lookup that the guard
//      performed did not find it, which makes -2 a lie.
//
// Three agents in a row have now reasoned about which of those it was, from a
// bare return code, and two reached opposite conclusions on the same image
// (docs/CONTRACT_COVERAGE_PLAN.md 7o and 7p, one of them retracted). The number
// cannot tell them apart, so the kernel has to say.
//
// WHAT IT PRINTS IS A DISCRIMINATOR, NOT A COMPLAINT. The C fact-gatherer
// (ext2_enoent_facts) RE-RESOLVES the parent twice after the guard has already
// refused. So a line that says the parent resolves NOW, having just been refused
// as absent, is positive evidence of a TRANSIENT lookup failure; a line that
// says it still does not resolve is positive evidence that the refusal was
// right and the caller asked for a path whose parent is genuinely missing. The
// hypothesis can therefore lose, which is the only reason to build the thing.
//
// RUST, per the standing directive (CLAUDE.md): this is new kernel logic and
// there is no performance argument for C. It runs only on an error path, does no
// I/O of its own, and takes no lock. The C side keeps only the parts that have
// to be C: reading file-static kernel state (g_ext2, g_root_ext2) and calling
// static functions inside fs/ext2.c, which is glue over existing C, not policy.
//
// CONCURRENCY. Plain relaxed atomics, no lock, exactly as rustkern/cfgread.rs
// argues: this decides whether to LOG. The worst outcome of a race is one line
// too many or one too few, and taking a lock on a filesystem error path to
// protect a log counter would be a worse trade.

use core::sync::atomic::{AtomicU32, Ordering};

// ---------------------------------------------------------------------------
// Call sites. Kept in step with kernel/fs/fsenoent.h by fsenoent_abi_check_rs.
// ---------------------------------------------------------------------------
pub const FSE_SITE_PATHCOPY: u32 = 1; // sys_open_k: sc_path_from_user refused
pub const FSE_SITE_PARENTGUARD: u32 = 2; // fdlayer: ext2_parent_dir_exists said no
pub const FSE_SITE_NOCREATE: u32 = 3; // fdlayer: absent and no O_CREAT
pub const FSE_SITE_READWHOLE: u32 = 4; // fat_read_file: ext2 whole-file read missed

// ---------------------------------------------------------------------------
// Verdicts. The whole point of the module: each one implies a DIFFERENT fix.
// ---------------------------------------------------------------------------
pub const FSE_V_UNKNOWN: u32 = 0;
pub const FSE_V_NOT_MOUNTED: u32 = 1; // g_ext2.mounted == 0: the root fs vanished
pub const FSE_V_ROOT_INO_LOST: u32 = 2; // "/" no longer resolves to inode 2
pub const FSE_V_TRANSIENT: u32 = 3; // guard refused, a re-resolve NOW succeeds
pub const FSE_V_FLAPPING: u32 = 4; // the two re-resolves disagree with each other
pub const FSE_V_PARENT_UNREADABLE: u32 = 5; // parent inode found but unreadable
pub const FSE_V_PARENT_NOT_DIR: u32 = 6; // parent exists and is not a directory
pub const FSE_V_CORRECT_ABSENT: u32 = 7; // parent really is absent: -2 is RIGHT
pub const FSE_V_PATHCOPY: u32 = 8; // the path string never reached the kernel

pub const EXT2_ROOT_INO: u32 = 2;

/// Mirror of fsenoent_facts_t (kernel/fs/fsenoent.h). The C side owns the
/// gathering because the state is C-file-static; this side owns what it MEANS.
/// Layout is sizeof-locked on the C side with a _Static_assert.
#[repr(C)]
pub struct FsEnoentFacts {
    pub site: u32,
    pub mounted: u32,
    pub root_ext2: u32,
    pub root_ino: u32,
    pub parent_ino_1: u32,
    pub parent_ino_2: u32,
    pub parent_mode: u32,
    pub inode_rc: i32,
    pub guard_said: u32,
    pub uid: u32,
}

/// Classify. Pure: no state, no I/O, no allocation.
///
/// ORDER MATTERS AND IS NOT ARBITRARY. "Not mounted" subsumes every other
/// answer (an unmounted volume resolves nothing), and a lost root inode
/// subsumes a missing parent, so the broadest cause is named first. Otherwise a
/// vanished filesystem would be reported, truthfully but uselessly, as a
/// missing parent directory, which is exactly the misdiagnosis this module
/// exists to stop.
fn classify_facts(f: &FsEnoentFacts) -> u32 {
    if f.site == FSE_SITE_PATHCOPY {
        return FSE_V_PATHCOPY;
    }
    if f.mounted == 0 {
        return FSE_V_NOT_MOUNTED;
    }
    if f.root_ino != EXT2_ROOT_INO {
        return FSE_V_ROOT_INO_LOST;
    }
    // The two re-resolves are the discriminator. Both ran AFTER the guard had
    // already answered "absent".
    if f.parent_ino_1 != f.parent_ino_2 {
        return FSE_V_FLAPPING;
    }
    if f.parent_ino_1 == 0 {
        return FSE_V_CORRECT_ABSENT;
    }
    if f.inode_rc != 0 {
        return FSE_V_PARENT_UNREADABLE;
    }
    if (f.parent_mode & 0xF000) != 0x4000 {
        return FSE_V_PARENT_NOT_DIR;
    }
    // The parent exists, is readable, and is a directory, and the guard refused
    // it anyway. That is the lie.
    FSE_V_TRANSIENT
}

// ---------------------------------------------------------------------------
// Rate limit. One line per (site, verdict) pair, plus a hard overall cap.
//
// Per-PAIR rather than per-path deliberately: the question being answered is
// "which of the four causes is happening", and the tenth path to hit the same
// cause adds nothing while it does push the earlier, more informative lines out
// of a bounded boot log (the fault rustkern/cfgread.rs was written for).
// ---------------------------------------------------------------------------
const FSE_SITES: usize = 5; // index by site, 0 unused
const FSE_VERDICTS: usize = 9;
const FSE_TOTAL_CAP: u32 = 24;

struct Tab {
    seen: [AtomicU32; FSE_SITES * FSE_VERDICTS],
    total: AtomicU32,
}

#[allow(clippy::declare_interior_mutable_const)]
const Z: AtomicU32 = AtomicU32::new(0);

static LIVE: Tab = Tab {
    seen: [Z; FSE_SITES * FSE_VERDICTS],
    total: AtomicU32::new(0),
};
static TEST: Tab = Tab {
    seen: [Z; FSE_SITES * FSE_VERDICTS],
    total: AtomicU32::new(0),
};

fn should_log_in(t: &Tab, site: u32, verdict: u32) -> i32 {
    if site as usize >= FSE_SITES || verdict as usize >= FSE_VERDICTS {
        return 0;
    }
    if t.total.load(Ordering::Relaxed) >= FSE_TOTAL_CAP {
        return 0;
    }
    let i = (site as usize) * FSE_VERDICTS + verdict as usize;
    if t.seen[i].load(Ordering::Relaxed) != 0 {
        return 0;
    }
    t.seen[i].store(1, Ordering::Relaxed);
    t.total.fetch_add(1, Ordering::Relaxed);
    1
}

// ---------------------------------------------------------------------------
// C entry points
// ---------------------------------------------------------------------------

/// Classify one gathered fact set. Returns an FSE_V_* code.
///
/// # Safety
/// `f` must be a valid, aligned, initialised pointer to one fsenoent_facts_t.
#[no_mangle]
pub unsafe extern "C" fn fsenoent_classify_rs(f: *const FsEnoentFacts) -> u32 {
    if f.is_null() {
        return FSE_V_UNKNOWN;
    }
    classify_facts(&*f)
}

/// Should this (site, verdict) be printed? First occurrence only, then never,
/// and never more than FSE_TOTAL_CAP lines in total for the whole boot.
#[no_mangle]
pub extern "C" fn fsenoent_should_log_rs(site: u32, verdict: u32) -> i32 {
    should_log_in(&LIVE, site, verdict)
}

/// Boot-time ABI lock: C hands in its own constants and this compares them.
/// Swapping two verdict codes would silently relabel a vanished filesystem as a
/// correct refusal, which is the one mistake this module exists to prevent, so
/// the constants are checked rather than trusted.
#[no_mangle]
pub extern "C" fn fsenoent_abi_check_rs(
    facts_size: u32,
    site_pathcopy: u32,
    site_parentguard: u32,
    site_nocreate: u32,
    site_readwhole: u32,
    v_not_mounted: u32,
    v_transient: u32,
    v_correct_absent: u32,
    v_pathcopy: u32,
) -> i32 {
    let good = facts_size as usize == core::mem::size_of::<FsEnoentFacts>()
        && site_pathcopy == FSE_SITE_PATHCOPY
        && site_parentguard == FSE_SITE_PARENTGUARD
        && site_nocreate == FSE_SITE_NOCREATE
        && site_readwhole == FSE_SITE_READWHOLE
        && v_not_mounted == FSE_V_NOT_MOUNTED
        && v_transient == FSE_V_TRANSIENT
        && v_correct_absent == FSE_V_CORRECT_ABSENT
        && v_pathcopy == FSE_V_PATHCOPY;
    if good {
        0
    } else {
        -1
    }
}

/// Self-test. PROVES the classifier separates the cases it claims to separate,
/// and that the rate limit both fires once and then stops, against a table of
/// its own so the live counters are untouched.
///
/// This is the RED proof for the instrument: a classifier that answered
/// CORRECT_ABSENT for every input would pass a run in which nothing was wrong
/// and would also pass the run where everything was, which is precisely the
/// trap that produced the retracted ENOENT report.
#[no_mangle]
pub extern "C" fn fsenoent_selftest_rs(out_checks: *mut u32) -> i32 {
    let mut n: u32 = 0;
    let mut ok = true;
    let mut chk = |cond: bool| {
        n += 1;
        if !cond {
            ok = false;
        }
    };

    let base = FsEnoentFacts {
        site: FSE_SITE_PARENTGUARD,
        mounted: 1,
        root_ext2: 1,
        root_ino: EXT2_ROOT_INO,
        parent_ino_1: 12,
        parent_ino_2: 12,
        parent_mode: 0x41ED, // directory, 0755
        inode_rc: 0,
        guard_said: 0,
        uid: 0,
    };

    // A parent that exists, reads, and is a directory, refused anyway: the lie.
    chk(classify_facts(&base) == FSE_V_TRANSIENT);

    // The same fact set with the volume gone must NOT read as a missing parent.
    let mut f = FsEnoentFacts { mounted: 0, ..copy_of(&base) };
    chk(classify_facts(&f) == FSE_V_NOT_MOUNTED);

    // Root inode lost outranks a parent answer.
    f = FsEnoentFacts { root_ino: 0, ..copy_of(&base) };
    chk(classify_facts(&f) == FSE_V_ROOT_INO_LOST);

    // Genuinely absent parent: the refusal is CORRECT and must say so.
    f = FsEnoentFacts { parent_ino_1: 0, parent_ino_2: 0, ..copy_of(&base) };
    chk(classify_facts(&f) == FSE_V_CORRECT_ABSENT);

    // Two re-resolves disagreeing is its own finding, not "absent".
    f = FsEnoentFacts { parent_ino_1: 0, parent_ino_2: 12, ..copy_of(&base) };
    chk(classify_facts(&f) == FSE_V_FLAPPING);

    // Found but unreadable, and found but not a directory, are distinct.
    f = FsEnoentFacts { inode_rc: -1, ..copy_of(&base) };
    chk(classify_facts(&f) == FSE_V_PARENT_UNREADABLE);
    f = FsEnoentFacts { parent_mode: 0x81A4, ..copy_of(&base) };
    chk(classify_facts(&f) == FSE_V_PARENT_NOT_DIR);

    // A path that never arrived is classified from the site alone, because
    // there is nothing else to look at: no path means no parent to resolve.
    f = FsEnoentFacts { site: FSE_SITE_PATHCOPY, mounted: 0, ..copy_of(&base) };
    chk(classify_facts(&f) == FSE_V_PATHCOPY);

    // Rate limit: first one logs, the second identical pair does not, and a
    // different verdict at the same site still gets its line.
    chk(should_log_in(&TEST, FSE_SITE_PARENTGUARD, FSE_V_TRANSIENT) == 1);
    chk(should_log_in(&TEST, FSE_SITE_PARENTGUARD, FSE_V_TRANSIENT) == 0);
    chk(should_log_in(&TEST, FSE_SITE_PARENTGUARD, FSE_V_CORRECT_ABSENT) == 1);
    chk(should_log_in(&TEST, FSE_SITE_READWHOLE, FSE_V_TRANSIENT) == 1);
    // Out-of-range input is refused rather than indexing the table.
    chk(should_log_in(&TEST, 99, FSE_V_TRANSIENT) == 0);
    chk(should_log_in(&TEST, FSE_SITE_PARENTGUARD, 99) == 0);

    if !out_checks.is_null() {
        unsafe { *out_checks = n };
    }
    if ok {
        0
    } else {
        -1
    }
}

/// `FsEnoentFacts` is deliberately not `Clone`/`Copy` (it mirrors a C struct the
/// C side owns), so the self-test builds its variants through this instead of
/// deriving traits onto the FFI type.
fn copy_of(f: &FsEnoentFacts) -> FsEnoentFacts {
    FsEnoentFacts {
        site: f.site,
        mounted: f.mounted,
        root_ext2: f.root_ext2,
        root_ino: f.root_ino,
        parent_ino_1: f.parent_ino_1,
        parent_ino_2: f.parent_ino_2,
        parent_mode: f.parent_mode,
        inode_rc: f.inode_rc,
        guard_said: f.guard_said,
        uid: f.uid,
    }
}
