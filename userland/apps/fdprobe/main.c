// fdprobe: #793 regression probe for kernel/proc/fdlayer.c's legacy fd
// table (LEGACY_MAX_FDS). That table is GLOBAL (system-wide), not
// per-process, so it can be exhausted by ordinary background system
// activity alone, and a file that fails sys_open() this way looks
// EXACTLY like "this specific file is broken" to whichever app hits it,
// when the file itself is perfectly fine. See CHANGELOG.md / blame.md
// 2026-08-08 (#793) and PORT-STATUS.md "Phase 12" (userland/apps/
// assaultcube) for the full writeup of how this was found while
// diagnosing an untextured AssaultCube.
//
// Two independent tests, both real bugs would show differently in:
//
//   fdprobe <path> <n>        open+close <path> n times in a strict loop.
//                              Recycling works iff this stays 100% ok.
//   fdprobe <path> <n> nc     open <path> n times WITHOUT closing, to find
//                              the live concurrent-open ceiling directly.
//   fdprobe <n> nc            round-robins across several distinct real
//                              AssaultCube content files instead of
//                              reopening one path (separates "the global
//                              pool is small" from "this one path has a
//                              single-open lock"); requires the AssaultCube
//                              content tier (packages/) to be staged, which
//                              every shipping golden carries per #627.
//
// Pre-fix (LEGACY_MAX_FDS=16, #444): open-close always 100%; no-close capped
// at 13 on an idle boot, sometimes 0 (immediate -EMFILE) on a busier one.
// Post-fix (LEGACY_MAX_FDS=128): no-close should get close to 125.
//
// No em-dashes per repo style.
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

static const char *g_multi_paths[] = {
    "packages/misc/notexture.jpg",
    "packages/misc/muzzleflash.jpg",
    "packages/misc/menu.jpg",
    "packages/misc/icon.png",
    "packages/misc/base.png",
    "packages/misc/smoke.png",
    "packages/misc/explosion.png",
    "packages/misc/blood.png",
    "packages/misc/scorch.png",
    "packages/misc/bullethole.png",
};
#define N_MULTI_PATHS ((int)(sizeof(g_multi_paths) / sizeof(g_multi_paths[0])))

int main(int argc, char **argv) {
    // Single-path form: fdprobe <path> <n> [nc]
    // Multi-path form:  fdprobe <n> [nc]        (argv[1] parses as a number)
    const char *single_path = NULL;
    int n = 200;
    int noclose = 0;
    int argi = 1;

    if (argc > 1 && atoi(argv[1]) == 0 && argv[1][0] != '0') {
        // argv[1] is not numeric: treat it as an explicit single path.
        single_path = argv[1];
        argi = 2;
    }
    if (argc > argi) n = atoi(argv[argi]);
    if (argc > argi + 1 && !strcmp(argv[argi + 1], "nc")) noclose = 1;

    int ok = 0, fail = 0, first_fail = -1, min_fd = -1, max_fd = -1;
    for (int i = 0; i < n; i++) {
        const char *path = single_path ? single_path
                                        : g_multi_paths[i % N_MULTI_PATHS];
        int fd = sys_open(path, 0);
        if (fd < 0) {
            fail++;
            if (first_fail < 0) first_fail = i;
            printf("fdprobe: FAIL at iter=%d path=%s rc=%d\n", i, path, fd);
            if (noclose) break;
            continue;
        }
        ok++;
        if (min_fd < 0 || fd < min_fd) min_fd = fd;
        if (fd > max_fd) max_fd = fd;
        if (!noclose) {
            int rc = sys_close(fd);
            if (rc < 0) {
                printf("fdprobe: sys_close(%d) FAILED rc=%d at iter=%d\n",
                       fd, rc, i);
            }
        }
    }
    printf("fdprobe: path=%s mode=%s n=%d ok=%d fail=%d first_fail_at=%d "
           "fd_range=[%d,%d]\n",
           single_path ? single_path : "<multi>",
           noclose ? "no-close" : "open-close",
           n, ok, fail, first_fail, min_fd, max_fd);
    return fail ? 1 : 0;
}
