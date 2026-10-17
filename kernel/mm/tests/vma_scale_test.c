// vma_scale_test.c - #245: drive the real kernel VMA list the way a growing
// userland heap drives it, and report the DETERMINISTIC node-visit count.
//
// THE WORKLOAD IS NOT INVENTED. userland/libc/stdlib.c ensure_mapped() maps
// only the page-aligned gap it needs, at an EXPLICIT address equal to the end
// of the previous mapping (heap_mapped), starting from HEAP_START =
// 0x8040000000. So a heap growing to S bytes issues S/4096 adjacent,
// same-protection, explicit-address SYS_MMAP calls, and each newly mapped page
// is touched almost immediately afterwards, taking one #PF that mm_fault()
// resolves with vma_find(). This loop is that sequence, one page at a time,
// against the genuine do_mmap() and vma_find() cut out of mm/demand.c.
//
// Reported per run:
//   pages   mappings requested (= heap size / 4 KB)
//   vmas    mm->vma_count at the end: the length of the list every walk walks
//   steps   VMA nodes visited, summed over every walk (the gate metric)
//   spp     steps per page. CONSTANT means per-operation cost is independent of
//           how much is already mapped. Growing means the whole thing is
//           quadratic in heap size.
//   secs    wall clock, printed for information only.

#include "vma_scale_shim.h"
#include "vma_scale_body.inc"

#include <time.h>

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    unsigned long mb = (argc > 1) ? strtoul(argv[1], NULL, 10) : 32;
    uint64_t pages = ((uint64_t)mb << 20) / 4096;

    mm_struct_t mm;
    memset(&mm, 0, sizeof mm);

    const uint64_t HEAP_START = 0x8040000000ULL;   // userland/libc/stdlib.c:84
    uint64_t mmaps = 0, faults = 0, fails = 0;

    g_vmaw_steps = 0;
    double t0 = now_s();
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t want = HEAP_START + i * 4096ULL;
        uint64_t got = do_mmap(&mm, want, 4096, 3 /* PROT_READ|PROT_WRITE */, 0,
                               NULL, 0);
        if (got != want) { fails++; break; }
        mmaps++;
        // The first touch of that page. mm_fault() resolves a not-present user
        // fault by calling exactly this, under the mm lock, before
        // handle_lazy_fault() does its one page allocation.
        vma_t *v = vma_find(&mm, want);
        if (!v) { fails++; break; }
        faults++;
    }
    double t1 = now_s();

    unsigned long long spp = mmaps ? (unsigned long long)(g_vmaw_steps / mmaps) : 0;
    printf("mb=%lu pages=%llu mmaps=%llu faults=%llu vmas=%u fails=%llu "
           "steps=%llu spp=%llu secs=%.4f\n",
           mb, (unsigned long long)pages, (unsigned long long)mmaps,
           (unsigned long long)faults, mm.vma_count, (unsigned long long)fails,
           (unsigned long long)g_vmaw_steps, spp, t1 - t0);
    return fails ? 1 : 0;
}
