// spin - #279 3b-3 SMP test app: a pure Ring-3 compute loop that makes NO
// syscalls and never returns (so crt0 never calls _exit). Because it never
// enters the kernel, it can run on an application processor with zero shared-
// state contention - proving user code executes on a 2nd core (per-core meter).
int main(void) {
    volatile unsigned long x = 0;
    for (;;) {
        x = x + 1;
        x = x ^ (x << 3);
    }
    return 0;
}
