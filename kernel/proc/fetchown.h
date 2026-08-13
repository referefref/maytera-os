// proc/fetchown.h - #745 (task #36): the C view of rustkern/fetchown.rs, the
// ownership + lifetime state machine for the async HTTP job slots.
//
// The job RECORDS (url, response body, HTTP status, progress) stay in
// proc/syscall.c, in C, where the worker threads and the network stack that
// fill them already live. What moved to Rust is the part that was missing and
// is pure decision: WHO may touch a slot, and WHEN a slot goes back in the
// pool. See rustkern/fetchown.rs for the slot word layout and the two defects
// this closes.
//
// There is no C twin and therefore no -DRUST_* strangler flag: this is new
// logic, not a port, so the rollback is reverting the commit rather than
// dropping a define.

#ifndef MAYTERA_PROC_FETCHOWN_H
#define MAYTERA_PROC_FETCHOWN_H

#include "../types.h"

// Table ids. Must match the TAB_* constants in rustkern/fetchown.rs; the boot
// assertion in fetchown_boot_check() locks the SLOT COUNTS, which is the part
// that could silently drift.
#define FETCHOWN_TAB_FETCH 0u   // g_async_fetch[ASYNC_FETCH_MAX]
#define FETCHOWN_TAB_POST  1u   // g_async_post[ASYNC_POST_MAX]

// Mirrors FetchownStats in rustkern/fetchown.rs. sizeof-locked on both sides:
// a field added on one side and not the other fails the build here and the
// `const _: () = assert!(...)` there.
typedef struct {
    uint32_t slots;            // slots in this table
    uint32_t owned;            // slots with a live owner
    uint32_t orphaned;         // owner gone, worker still running
    uint32_t refused_notowner; // global: ownership violations refused
    uint32_t refused_noslot;   // global: bad/free index refused
    uint32_t exit_released;    // global: slots reclaimed by the exit hook
} fetchown_stats_t;
_Static_assert(sizeof(fetchown_stats_t) == 24,
               "fetchown_stats_t must stay layout-identical to FetchownStats "
               "in rustkern/fetchown.rs");
_Static_assert(sizeof(uint32_t) == 4, "fetchown FFI assumes 32-bit u32");

int  fetchown_slots_rs(uint32_t tab);
int  fetchown_claim_rs(uint32_t tab, uint32_t owner);
int  fetchown_abandon_rs(uint32_t tab, uint32_t slot);
int  fetchown_check_rs(uint32_t tab, uint32_t slot, uint32_t owner);
int  fetchown_owned_by_rs(uint32_t tab, uint32_t slot, uint32_t owner);
int  fetchown_release_rs(uint32_t tab, uint32_t slot);
int  fetchown_orphan_rs(uint32_t tab, uint32_t slot);
int  fetchown_worker_done_rs(uint32_t tab, uint32_t slot);
uint32_t fetchown_owner_rs(uint32_t tab, uint32_t slot);
void fetchown_note_exit_release_rs(void);
int  fetchown_stats_rs(uint32_t tab, fetchown_stats_t *out);
int  fetchown_selftest_rs(void);

// Release every async HTTP job slot owned by the exiting process group.
// Defined in proc/syscall.c (it owns the job records), called from proc_exit()
// in proc/process.c. Runs under cli() on the dying process's own stack.
void async_http_proc_exit(uint32_t owner);

// Boot-time: run the state-machine self-test and assert the Rust and C slot
// counts agree. Defined in proc/syscall.c, called from main.c.
void fetchown_boot_check(void);

#endif // MAYTERA_PROC_FETCHOWN_H
