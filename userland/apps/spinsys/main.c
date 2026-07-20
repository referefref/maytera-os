// spinsys - #279 3b-3C SMP test: a Ring-3 loop that MAKES SYSCALLS (uptime_ms)
// every iteration. Routed to the migratable queue (name starts "spin") so an AP
// runs it. Proves the whole-kernel BKL lets a syscall-making app run on an AP
// without corrupting/deadlocking the BSP.
#include "../../libc/maytera.h"
int main(void) {
    volatile unsigned long x = 0;
    for (;;) {
        x += uptime_ms();      // SYSCALL each iteration (exercises AP syscall path)
        x ^= (x << 3);
    }
    return 0;
}
