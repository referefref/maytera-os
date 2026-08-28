// ipc.c - Inter-Process Communication Initialization for MayteraOS

#include "ipc.h"
#include "../serial.h"

void ipc_init(void) {
    kprintf("[IPC] Initializing Inter-Process Communication...\n");

    // Initialize shared memory subsystem
    shm_init();

    // Initialize message passing subsystem
    msg_init();

    kprintf("[IPC] IPC initialization complete\n");
}

// ############################################################################
// # ZERO CALLERS. THIS IS A REAL, RING-3-TRIGGERABLE LEAK - AND WIRING IT IN  #
// # AS IT STANDS WOULD REPLACE THAT LEAK WITH A DOUBLE FREE. DO NOT DO IT     #
// # BLIND.                                                                    #
// ############################################################################
//
// Audited #404 (2026-08-23). ipc.h says "Called when a process terminates".
// It is not, and it never has been. This function is also the ONLY caller of
// shm_cleanup_process() and msg_cleanup_process(), so the entire per-process
// IPC teardown path is dead, not just its entry point.
//
// WHAT LEAKS TODAY, all of it reachable by an unprivileged app in a loop:
//   * shm_regions[] slots. Create a region, exit, and the slot is burned until
//     reboot. SHM_MAX_REGIONS is a static array, so this is slot exhaustion,
//     the same shape as the async-HTTP DoS that proc/process.c calls "a trivial
//     denial of service from an unprivileged app".
//   * msg_channels[] slots, and chan->server_conn leaks its kmalloc outright.
//   * Stale MSG_CONN_ACTIVE connections owned by dead pids stay in the table.
//     That is a CORRECTNESS bug, not just a leak: connections are matched by
//     owner_pid, so a recycled pid inherits a dead process's connections.
//
// WHY YOU CANNOT JUST CALL IT FROM proc_exit(). The SHM backing frames are
// ALREADY freed on exit by a different path: cleanup_proc_slot() ->
// vmm_destroy_user_space(), which walks the tables and frees every leaf that is
// PRESENT and USER via vmm_free_user_page_cow() (mm/vmm.c). shm_attach() maps
// its frames with VMM_USER_RW/VMM_USER_RO, so they match that test and are
// returned to the PMM already, while shm_regions[i].phys_addr still points at
// them. Adding shm_cleanup_process()'s pmm_free_pages() on top of that is a
// DOUBLE FREE, which is strictly worse than the slot leak it fixes. Verified by
// reading both paths, not inferred.
//
// SO THE FIX IS A SMALL TICKET, NOT A ONE-LINE PATCH. In order:
//   1. Settle SHM frame ownership against vmm_destroy_user_space() (most
//      likely: release the slot here, and do NOT free the frames here).
//   2. Make both cleanups thread-group aware. Connections are registered with
//      whatever pid called sys_msg_connect, which may be a worker thread, so a
//      flat pid match would let one exiting pthread destroy the whole
//      process's channels. Use tgid, under the !shares_vm guard.
//   3. Demote the two unconditional per-exit kprintf()s (shm.c, msg.c) to
//      debug-only; they would fire on EVERY process exit under cli().
//   4. THEN call it from proc_exit(), next to async_http_proc_exit(), which is
//      the chokepoint every termination path funnels through. Locking is fine:
//      this path takes no locks and only reaches kfree/pmm_free_pages, both
//      irqsave and non-blocking, so it is safe under the cli() held there.
//
// Do not delete this function. It is the only description in the tree of what
// per-process IPC teardown should do, and deleting it would leave the leak with
// nothing pointing at it.
void ipc_cleanup_process(uint32_t pid) {
    // Clean up shared memory
    shm_cleanup_process(pid);

    // Clean up message channels
    msg_cleanup_process(pid);
}
