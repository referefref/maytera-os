// rustkern/escrowundo.rs - #246/#305 Stage 5 (part B): the PURE reversibility
// policy for the escrow ROLLBACK/UNDO engine.
//
// See docs/CONTRACT_ENFORCEMENT_PLAN.md section 3f (Stage 5) and
// docs/CONTRACT_ARCHITECTURE.md section 7 ("Rollback, and its honest boundary").
//
// New kernel logic with no C twin to strangle, so Rust per the 2026-07-16 rule.
// There is no performance argument for C: every function here is a small integer
// classification on the COLD escrow record / contract-close chokepoint.
//
// THE SPLIT (mirrors rustkern/capdev.rs policy + fs/escrow_device.c state, and
// fs/graphfs/fold.c): THIS file is the STATELESS policy. Given an escrow FS
// mutation op class it decides whether that op is REVERSIBLE right now and what
// its INVERSE action is. The STATE (the per-contract undo log) and the
// entanglement glue (the fat_rename/fat_delete/fat_exists FS primitives, the
// process_t PCB, path canonicalisation, kprintf) live in fs/escrow_undo.c, which
// is C for the same entanglement reason fold.c / escrow_device.c are: it touches
// the C FS layer and PCB that Rust does not reach in this tree.
//
// HONESTY (docs/CONTRACT_ARCHITECTURE.md section 7). Effects divide into
// reversible / compensatable / irreversible, and the undo surface must never
// lie. This FIRST slice records and reverts only the two CLEANLY and
// NON-DESTRUCTIVELY reversible FS mutations: a MOVE (rename) and a MKDIR of an
// empty directory. A file CREATE or an OVERWRITE is deliberately classed
// NOT-reversible here: undoing a create by deleting is a data-loss risk if the
// bytes matter, and undoing an overwrite needs the pre-image snapshot the
// journal-plus-snapshot design (section 7) will add. Claiming to undo those now
// would be the exact "system that claims complete undo and quietly cannot
// deliver" section 7 forbids. DELETE is never recorded because an escrow actor
// is never granted delete (the no-delete invariant).

// Escrow FS-mutation op classes this policy knows. MIRRORED in fs/escrow_undo.h.
pub const ESCU_OP_RENAME: u32 = 1; // a move: from -> to
pub const ESCU_OP_MKDIR: u32 = 2; //  a directory create
pub const ESCU_OP_CREATE: u32 = 3; // a new-file create (NOT reversible in this slice)
pub const ESCU_OP_OVERWRITE: u32 = 4; // an overwrite of existing bytes (needs a snapshot)
pub const ESCU_OP_DELETE: u32 = 5; //  never recorded (no delete grant is ever issued)

// Inverse actions the C replay engine performs. MIRRORED in fs/escrow_undo.h.
pub const ESCU_INV_NONE: u32 = 0; //        nothing to do / not reversible now
pub const ESCU_INV_RENAME_BACK: u32 = 1; // move `to` back to `from`, iff `from` is free
pub const ESCU_INV_RMDIR_EMPTY: u32 = 2; // remove the created dir, iff it is empty

/// Is this op one the engine can reverse right now, cleanly and without any risk
/// of data loss? Only RENAME and MKDIR qualify in this slice.
#[no_mangle]
pub extern "C" fn escu_is_reversible_rs(op: u32) -> u32 {
    match op {
        ESCU_OP_RENAME | ESCU_OP_MKDIR => 1,
        _ => 0,
    }
}

/// The inverse action for an op. The engine consults this rather than
/// hard-coding the mapping at each replay site, so the policy has ONE home.
#[no_mangle]
pub extern "C" fn escu_inverse_rs(op: u32) -> u32 {
    match op {
        ESCU_OP_RENAME => ESCU_INV_RENAME_BACK,
        ESCU_OP_MKDIR => ESCU_INV_RMDIR_EMPTY,
        _ => ESCU_INV_NONE,
    }
}

/// Boot self-test of the pure policy: the reversibility set and the inverse
/// mapping are exactly what the C engine relies on. Returns 0 on PASS, the
/// negative code of the FIRST failed assertion otherwise. Writes the number of
/// assertions run to *out_checks when non-null. Driven by fs/escrow_undo.c
/// escrow_undo_selftest().
#[no_mangle]
pub extern "C" fn escu_policy_selftest_rs(out_checks: *mut u32) -> i32 {
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
    // reversible set: RENAME and MKDIR yes, everything else no.
    chk!(escu_is_reversible_rs(ESCU_OP_RENAME) == 1, -1);
    chk!(escu_is_reversible_rs(ESCU_OP_MKDIR) == 1, -2);
    chk!(escu_is_reversible_rs(ESCU_OP_CREATE) == 0, -3);
    chk!(escu_is_reversible_rs(ESCU_OP_OVERWRITE) == 0, -4);
    chk!(escu_is_reversible_rs(ESCU_OP_DELETE) == 0, -5);
    chk!(escu_is_reversible_rs(0) == 0, -6);
    // inverse mapping.
    chk!(escu_inverse_rs(ESCU_OP_RENAME) == ESCU_INV_RENAME_BACK, -7);
    chk!(escu_inverse_rs(ESCU_OP_MKDIR) == ESCU_INV_RMDIR_EMPTY, -8);
    chk!(escu_inverse_rs(ESCU_OP_CREATE) == ESCU_INV_NONE, -9);
    chk!(escu_inverse_rs(ESCU_OP_OVERWRITE) == ESCU_INV_NONE, -10);
    chk!(escu_inverse_rs(ESCU_OP_DELETE) == ESCU_INV_NONE, -11);
    if !out_checks.is_null() {
        unsafe {
            *out_checks = checks;
        }
    }
    fail
}
