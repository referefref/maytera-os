// hello.c - Minimal userland test for MayteraOS
// Tests basic syscall functionality

// Syscall numbers
#define SYS_EXIT  0
#define SYS_WRITE 13

// Raw syscall for write
static long syscall3(long num, long arg1, long arg2, long arg3) {
    long ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

// Raw syscall for exit
static void syscall1(long num, long arg1) {
    __asm__ volatile (
        "syscall"
        :: "a"(num), "D"(arg1)
        : "rcx", "r11", "memory"
    );
}

void _start(void) {
    // Write "Hello from userland!\n" to stdout (fd=1)
    const char msg[] = "Hello from userland!\n";
    syscall3(SYS_WRITE, 1, (long)msg, sizeof(msg) - 1);

    // Exit with code 0
    syscall1(SYS_EXIT, 0);

    // Should never reach here
    while(1) {}
}
