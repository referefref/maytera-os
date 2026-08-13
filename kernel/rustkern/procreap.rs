// rustkern/procreap.rs - #745 (task 37): WHICH ZOMBIE PROCESS SLOTS MAY BE
// RECLAIMED. The policy that decides it, and nothing else.
//
// Rust per the 2026-07-16 rule: this is the decision, it is pure, and it is
// exactly where the bug was. The C keeps the table walk and the teardown
// (cleanup_proc_slot touches kernel stacks, page tables and the mm refcount,
// none of which Rust models); it hands this a flat snapshot and gets back a
// bitmask of slots it may free. proc_mem_account_rs is the same shape.
//
// ===========================================================================
// THE DEFECT THIS REMOVES, MEASURED ON GOLDEN BUILD 1845 BEFORE THE CHANGE
// ---------------------------------------------------------------------------
// Every SYS_HTTP_FETCH_START spawns a kernel worker process ("httpfetch",
// proc/syscall.c) to run the transfer off the caller's thread. init_proc()
// sets ppid from current_proc, so the worker becomes a CHILD OF THE RING 3
// PROCESS THAT MADE THE SYSCALL - a process that does not know the worker
// exists, never learns its pid and never wait()s for it. proc_exit() leaves it
// a ZOMBIE, and the old policy here reclaimed a zombie only if it had NO LIVE
// PARENT. The caller is alive, so the slot was never reclaimed.
//
// MAX_PROCESSES is 64. A throwaway VM booted from golden 1845 sat at 23 slots
// used; the 41st fetch of the boot filled the table, and from then on
// proc_create_ex() returned -1, sys_http_fetch_start() returned -1, and the
// App Store's http_get_live() turned that into "Couldn't reach the App Store
// repository" WITHOUT EMITTING A SINGLE PACKET. Measured: iterations 1..49 OK,
// iteration 50 onward rc=-1 with `used=64/64 zombies=49 httpfetch_zombies=49`.
// On a desktop boot (compositor + widgets + services, ~35 live processes) the
// budget is smaller still, which is why the machine works after a reboot and
// stops working after it has been up a while.
//
// THE FIX IS TO STOP LYING ABOUT PARENTHOOD, not to grow the table. A kernel
// worker thread is not the child of whichever Ring 3 process happened to enter
// the syscall; it is the kernel's. proc_create_ex() now marks such a process
// F_DETACHED, and this policy treats a detached zombie as reclaimable, the
// same way it already treats an exited CLONE_THREAD thread (#430) whose join
// path is the futex, not wait().
// ===========================================================================

// PROC_STATE_* mirror (kernel/proc/process.h). Only these three are load
// bearing here; the rest are "alive" for the purpose of a parent lookup.
const ST_UNUSED: u32 = 0;
const ST_ZOMBIE: u32 = 5;

// flags bits, mirrored by PROC_REAP_F_* in kernel/proc/process.h.
const F_SHARES_VM: u32 = 1 << 0; // a CLONE_THREAD thread (#430)
const F_DETACHED: u32 = 1 << 1; // a kernel worker: nobody will ever wait() it

/// One process-table slot, flattened. Layout locked against the C side by a
/// `_Static_assert` on `sizeof(proc_reap_ent_t)` in kernel/proc/process.c.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProcReapEnt {
    pub pid: u32,
    pub ppid: u32,
    pub state: u32,
    pub flags: u32,
}

const _: () = assert!(core::mem::size_of::<ProcReapEnt>() == 16);

// The C table is MAX_PROCESSES == 64 slots and the result is a u64 bitmask,
// so 64 is a hard ceiling here. process.c static-asserts the same bound.
const MAX_SLOTS: usize = 64;

/// Pure policy. Returns a bitmask of slot indices whose process_t may be torn
/// down and returned to the free pool.
///
/// A slot qualifies iff ALL of:
///   * it is not slot 0 (the idle process / boot thread), and not `skip`
///     (the running process, which is mid-exit on its own kernel stack);
///   * its state is ZOMBIE;
///   * and nobody will ever wait() for it, which is true when any of:
///       - ppid <= 1        (init/idle never call wait),
///       - F_SHARES_VM      (#430: a thread is joined via the futex),
///       - F_DETACHED       (task 37: a kernel worker),
///       - no live parent   (a real orphan: the slot holding ppid is gone or
///                           is itself a zombie).
fn scan(ents: &[ProcReapEnt], skip: i32) -> u64 {
    let mut mask: u64 = 0;
    for i in 1..ents.len() {
        if i as i32 == skip {
            continue;
        }
        let z = &ents[i];
        if z.state != ST_ZOMBIE {
            continue;
        }
        let unwaited = z.ppid <= 1
            || (z.flags & (F_SHARES_VM | F_DETACHED)) != 0
            || !parent_alive(ents, z.ppid);
        if unwaited {
            mask |= 1u64 << i;
        }
    }
    mask
}

/// A parent counts as alive only if some slot is in use, is NOT itself a
/// zombie, and carries that pid. A zombie parent cannot reap anybody.
fn parent_alive(ents: &[ProcReapEnt], ppid: u32) -> bool {
    ents.iter()
        .any(|p| p.state != ST_UNUSED && p.state != ST_ZOMBIE && p.pid == ppid)
}

/// C entry point. `ents` must point at `n` entries (n <= 64).
///
/// # Safety
/// `ents` must be a valid, readable array of `n` `ProcReapEnt`, and `n` must
/// not exceed 64. Called only from process.c's reap_orphan_zombies(), which
/// builds the array on its own stack from the live table.
#[no_mangle]
pub unsafe extern "C" fn proc_reap_scan_rs(ents: *const ProcReapEnt, n: u32, skip: i32) -> u64 {
    if ents.is_null() || n == 0 || n as usize > MAX_SLOTS {
        return 0;
    }
    let s = core::slice::from_raw_parts(ents, n as usize);
    scan(s, skip)
}

/// Self-test, called from process.c at boot under the same differential
/// discipline as the other seams. Returns 0 on success, or the number of the
/// first failing case (1-based) so a failure names itself on the serial log.
///
/// Case 4 is the REGRESSION CASE: a zombie whose parent is alive, marked
/// F_DETACHED. Before this change the policy said "keep", which is the bug
/// that filled the process table; if that case ever goes back to "keep" the
/// App Store stops loading again after ~40 fetches.
#[no_mangle]
pub extern "C" fn proc_reap_selftest_rs() -> u32 {
    fn e(pid: u32, ppid: u32, state: u32, flags: u32) -> ProcReapEnt {
        ProcReapEnt { pid, ppid, state, flags }
    }
    // slot 0 is idle (pid 0), slot 1 a live app (pid 10).
    let base = [
        e(0, 0, 2, 0),  // idle, RUNNING
        e(10, 0, 1, 0), // live app, READY
    ];

    // 1: a zombie child of a LIVE parent, not detached, not a thread -> KEEP.
    let mut t = [base[0], base[1], e(11, 10, ST_ZOMBIE, 0)];
    if scan(&t, -1) != 0 {
        return 1;
    }
    // 2: same, but the parent is gone -> REAP (the pre-existing orphan rule).
    t[1] = e(10, 0, ST_UNUSED, 0);
    if scan(&t, -1) != (1u64 << 2) {
        return 2;
    }
    // 3: a zombie whose parent is ITSELF a zombie -> REAP.
    t[1] = e(10, 0, ST_ZOMBIE, 0);
    if scan(&t, -1) & (1u64 << 2) == 0 {
        return 3;
    }
    // 4: REGRESSION CASE - detached kernel worker, parent alive -> REAP.
    let t4 = [base[0], base[1], e(11, 10, ST_ZOMBIE, F_DETACHED)];
    if scan(&t4, -1) != (1u64 << 2) {
        return 4;
    }
    // 5: #430 thread zombie, parent alive -> REAP.
    let t5 = [base[0], base[1], e(11, 10, ST_ZOMBIE, F_SHARES_VM)];
    if scan(&t5, -1) != (1u64 << 2) {
        return 5;
    }
    // 6: `skip` is honoured even for an otherwise reclaimable slot.
    if scan(&t4, 2) != 0 {
        return 6;
    }
    // 7: slot 0 is never reclaimed, even if it somehow reads as a zombie.
    let t7 = [e(0, 0, ST_ZOMBIE, F_DETACHED), base[1]];
    if scan(&t7, -1) != 0 {
        return 7;
    }
    // 8: a live (non-zombie) detached worker is NOT reclaimable.
    let t8 = [base[0], base[1], e(11, 10, 1, F_DETACHED)];
    if scan(&t8, -1) != 0 {
        return 8;
    }
    // 9: the real shape that broke the App Store - 60 detached zombies all
    //    parented to one live app. Every one of them must come back.
    let mut big = [e(0, 0, 2, 0); MAX_SLOTS];
    big[1] = e(10, 0, 1, 0);
    for i in 2..MAX_SLOTS {
        big[i] = e(100 + i as u32, 10, ST_ZOMBIE, F_DETACHED);
    }
    let want = !0u64 << 2; // every slot from 2 upward
    if scan(&big, -1) != want {
        return 9;
    }
    0
}
