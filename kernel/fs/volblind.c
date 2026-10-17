// fs/volblind.c - [volaudit] #246. The sixty lines of C the detector needs:
// the fat_fs_t pointer comparison, the hotplug path question, and the varargs
// report. The verdict, the de-duplication and the self-test are in
// rustkern/volblind.rs; see that file for why this exists at all.
//
// WHY THIS HALF IS C AND NOT RUST. It does three things Rust cannot do here
// without inventing an FFI shim around each: compare a `fat_fs_t *` against
// the extern `g_fat_fs` object, call hotplug_resolve_path() which returns a
// borrowed `const char *` into the caller's own buffer, and call
// bootlog_fault_write(), a C varargs function. Every DECISION crosses into
// Rust as an integer. This is the same split drivers/hotplug.c uses for
// path_route_rs() and the same one fs/recycle_fs.c argues for in its header.

#include "volblind.h"
#include "fat.h"
#include "bootlog.h"
#include "../drivers/hotplug.h"

extern fat_fs_t g_fat_fs;
extern int kprintf(const char *fmt, ...);

// rustkern/volblind.rs
extern int      volblind_verdict_rs(int fs_is_root, int matched);
extern uint64_t volblind_count_rs(void);
extern uint64_t volblind_total_rs(void);
extern int      volblind_site_is_new_rs(uint64_t caller);
extern uint32_t volblind_sites_rs(void);
extern uint32_t volblind_selftest_rs(uint32_t *out_checks);

// Must match VB_OK / VB_BLIND in rustkern/volblind.rs. The Rust self-test
// fails if the verdict's meaning moves, so the two cannot drift silently.
#define VB_OK     0
#define VB_BLIND  1

uint64_t volblind_total(void) { return volblind_total_rs(); }
uint32_t volblind_sites(void) { return volblind_sites_rs(); }

void volblind_selftest(void) {
    uint32_t checks = 0;
    uint32_t fails = volblind_selftest_rs(&checks);
    if (fails == 0) {
        kprintf("[VOLBLIND] detector self-test PASS (%u checks)\n", checks);
        bootlog_write("[VOLBLIND] detector self-test PASS (%u checks)", checks);
    } else {
        kprintf("[VOLBLIND] *** detector self-test FAIL: %u/%u ***\n", fails, checks);
        bootlog_write("[VOLBLIND] *** detector self-test FAIL %u/%u ***", fails, checks);
    }
}

int volblind_check(const char *what, fat_fs_t *fs, const char *path,
                   void *caller) {
    // Two cheap rejects first, in this order, because this sits on the hot
    // path of every FAT file operation in the kernel.
    if (fs != &g_fat_fs) return VB_OK;          // a volume's own handle: fine
    if (!path || path[0] != '/') return VB_OK;  // relative/NULL: not our question

    // "Does this path lie under the mount point of a MOUNTED removable
    // volume?" asked of the ONE place that owns that question. Not a second
    // copy of the prefix match: a hand-rolled strncmp here is how "/USB1"
    // would come to match "/USB10", which is the off-by-one
    // hotplug_path_split_rs() was written to own.
    const char *rel = 0;
    if (hotplug_resolve_path(path, &rel) < 0) return VB_OK;

    if (volblind_verdict_rs(1, 1) != VB_BLIND) return VB_OK;   // cannot happen;
        // asked anyway so the live path and the self-tested function are the
        // same function. A detector whose shipping path bypasses its own
        // tested verdict is a detector with an untested shipping path.

    uint64_t n = volblind_count_rs();
    int fresh = volblind_site_is_new_rs((uint64_t)(uintptr_t)caller);

    if (fresh == 1) {
        // bootlog_fault_write() and not kprintf(): it takes no lock, allocates
        // nothing and touches no filesystem, and it mirrors to serial AND to
        // the RAM ring that a later safe context drains to /BOOTLOG.TXT. Both
        // of the owner's real machines have no usable serial console, and a
        // week of #426-class findings had to be reconstructed from fragments
        // for exactly that reason. Same call, same justification, as
        // sync/noblock.c.
        bootlog_fault_write("[VOLBLIND] #246 BLIND ASK: %s(&g_fat_fs, \"%s\") "
                            "- that path is on a mounted REMOVABLE VOLUME, "
                            "which g_fat_fs cannot see. Route through "
                            "hotplug_fs_or_root(). caller=%p (blind ask #%lu) "
                            "addr2line -e kernel.elf %p",
                            what ? what : "?", path, caller,
                            (unsigned long)n, caller);
    } else if (fresh < 0) {
        bootlog_fault_write("[VOLBLIND] site table full; further NEW sites are "
                            "counted but not printed (total blind asks %lu)",
                            (unsigned long)n);
    }
    return VB_BLIND;
}
