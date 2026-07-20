// primes - #279 generalization test: a Ring-3 compute-bound app whose name does
// NOT start with "spin". Launched via RC `launchap`, the one-shot migratable
// flag must route it to an application processor regardless of its name.
// Counts primes forever; touches a syscall (uptime_ms) once per sweep so the AP
// syscall path is exercised too.
#include "../../libc/maytera.h"
int main(void) {
    volatile unsigned long count = 0;
    unsigned long n = 2;
    for (;;) {
        int is_prime = 1;
        for (unsigned long d = 2; d * d <= n; d++) {
            if (n % d == 0) { is_prime = 0; break; }
        }
        if (is_prime) count++;
        n++;
        if ((n & 0xFFFFF) == 0) { count ^= uptime_ms(); n = 2; }  // periodic syscall + wrap
    }
    return 0;
}
