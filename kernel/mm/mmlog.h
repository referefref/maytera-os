// mmlog.h - #dosmem: one place that decides how a memory-management ANOMALY
// reaches a machine with no serial port.
//
// WHY THIS HEADER EXISTS AT ALL. mm/ had 132 kprintf calls and ZERO
// bootlog_write calls (demand.c 51/0, vmm.c 46/1, pmm.c 20/0, heap.c 15/0,
// fault.c 11/0, measured 2026-09-03). kprintf goes to serial. The owner's
// iMac14,4 and his ASUS have no serial port, so every out-of-memory, every
// failed page-table allocation, every double free and every VMA overlap this
// kernel has ever detected on his hardware was written to a wire that is not
// connected to anything. His /BOOTLOG.TXT from build 2346 contains none of
// them. This is the FIFTH subsystem found in that state (net/wget.c 42/0 and
// net/https.c 74/4 the day before), and every previous fix promoted ONE line
// and left the subsystem alone.
//
// TWO DECISIONS ARE BAKED IN HERE SO NO CALL SITE HAS TO MAKE THEM AGAIN.
//
// 1. bootlog_fault_write(), not bootlog_write(). Every site below can fire
//    while holding pmm_lock, heap_lock or mm->vma_lock with interrupts off,
//    or from inside the #PF handler. bootlog_write() calls kprintf(), which
//    takes g_console_lock, and its flush enters fat/ext2 -> blk_write ->
//    usb_msc_transport, which is the exact deadlock 240dc9f and #745 fixed.
//    bootlog_fault_write() takes no lock, allocates nothing, touches no
//    filesystem, and a later safe context (or the 2 s heartbeat) flushes it.
//    An mm anomaly reported from an unsafe context is the ONLY kind there is.
//
// 2. A PER-SITE CAP, because an unbounded promotion is worse than none. The
//    fault ring is 2048 bytes. A heap allocator failing in a retry loop would
//    fill it in milliseconds and EVICT the one report that mattered, which
//    would make this change a regression rather than a fix. Each site reports
//    its first MM_ANOMALY_MAX occurrences, stamped with its own occurrence
//    number, and then says so once and goes quiet. The occurrence number is
//    the part that keeps it honest: "#1" and "#7 of the first 8" are
//    different situations and a reader can tell them apart.
#ifndef MM_MMLOG_H
#define MM_MMLOG_H

#include "../fs/bootlog.h"

#define MM_ANOMALY_MAX 8u

// Use exactly like bootlog_fault_write(), from anywhere in mm/. The counter is
// a function-local static, so every call SITE gets its own budget and a noisy
// one cannot silence a quiet one.
#define MM_ANOMALY(fmt, ...)                                                   \
    do {                                                                       \
        static unsigned _mm_anom_n = 0;                                        \
        unsigned _n = ++_mm_anom_n;                                            \
        if (_n <= MM_ANOMALY_MAX) {                                            \
            bootlog_fault_write("[MM#%u] " fmt, _n, ##__VA_ARGS__);            \
            if (_n == MM_ANOMALY_MAX)                                          \
                bootlog_fault_write("[MM#%u] further reports from this site "  \
                                    "are suppressed", _n);                     \
        }                                                                      \
    } while (0)

#endif // MM_MMLOG_H
