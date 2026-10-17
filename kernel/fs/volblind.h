// fs/volblind.h - [volaudit] #246 runtime detector: "the ROOT filesystem was
// asked about a path that lives on a mounted REMOVABLE VOLUME".
//
// The design argument, the four historical instances, and the reason this is a
// runtime check rather than only a build-time lint are all in
// rustkern/volblind.rs, which owns the verdict. Read that first.
//
// SHORT VERSION. g_fat_fs routes the ext2 root, the FAT ESP, /SMB, /NFS and a
// mounted disk image. It does NOT route a hot-plugged removable volume; only
// hotplug_fs_or_root() / hotplug_volume_for_path() (drivers/hotplug.h) do. Any
// code that hands g_fat_fs a path under /USB<n> gets "absent" for a file that
// is plainly there, and then does something wrong with a straight face: an
// existence test takes a permission gate's weaker branch, a read fails, a
// write lands on a different file of the same spelling in the root namespace.
//
// HOW TO USE IT. You do not call this from new code to make new code correct.
// Route through hotplug_fs_or_root() and the detector will never see you. The
// hooks live in the public wrappers in fs/fat.c, so every caller in the tree
// is covered by thirteen call sites rather than by each caller remembering.
//
// WHAT IT COSTS WHEN NOTHING IS PLUGGED IN. hotplug_resolve_path() walks
// HOTPLUG_MAX_DEVICES table entries and `continue`s on the first field of each
// when the slot is not an active mounted device, so with no removable volume
// present this is a handful of loads and no call. It is a pointer comparison
// away from free on the overwhelmingly common path: fs != &g_fat_fs returns
// immediately, and so does a path that does not start with '/'.

#ifndef VOLBLIND_H
#define VOLBLIND_H

#include "../types.h"
// fat.h, not a forward declaration: fat_fs_t is an ANONYMOUS struct typedef
// (fs/fat.h:166), so `struct fat_fs *` would be a different, incomplete type
// and the hook's prototype would silently disagree with its definition.
#include "fat.h"

// Total blind asks this boot, reported or silently counted. 0 is the expected
// value on every machine with nothing plugged in, and on a machine with a
// volume plugged in where every exercised path routes correctly. Read it to
// turn "the code looks right" into a number.
uint64_t volblind_total(void);

// Distinct call sites reported this boot (bounded; see VB_SITES_MAX).
uint32_t volblind_sites(void);

// The boot self-test over the verdict and the de-duplication table. Prints
// PASS/FAIL with a CHECK COUNT either way, for the reason stated in
// rustkern/volblind.rs: a self-test that only speaks on failure cannot be
// told apart from one that never runs.
void volblind_selftest(void);

// THE HOOK. `what` names the operation for the report ("fat_open"), `fs` is
// the handle the caller passed, `path` is the path the caller passed, `caller`
// is an opaque identity for de-duplication and is never dereferenced.
//
// Returns VB_OK (0) or VB_BLIND (1). The return exists so a caller that wants
// to FAIL on a blind ask can, but no shipping caller does today: this reports
// and carries on, for exactly the reason sync/noblock.c gives. An assertion
// that bricks a user's boot is an assertion that gets compiled out, and then
// it protects nobody.
int volblind_check(const char *what, fat_fs_t *fs, const char *path,
                   void *caller);

// The form the fs/fat.c wrappers use. __builtin_return_address(0) is the
// WRAPPER's caller, which is the site a human needs to fix, not the wrapper.
#define VOLBLIND_CHECK(what, fs, path) \
    volblind_check((what), (fs), (path), __builtin_return_address(0))

#endif // VOLBLIND_H
