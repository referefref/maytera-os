// spindisk - #279 SMP deadlock harness: a Ring-3 loop that does BLOCKING DISK
// syscalls (open/read/close) every iteration. Name starts "spin" so it is
// routed to the migratable queue -> runs on an Application Processor. Used to
// reproduce the whole-kernel-BKL disk-I/O deadlock (files+recycle hang).
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"

static char buf[8192];

int main(void) {
    volatile unsigned long iters = 0;
    for (;;) {
        int fd = sys_open("/APPS/CALC.ELF", 0);
        if (fd >= 0) {
            while (sys_read(fd, buf, sizeof(buf)) > 0) { /* drain whole file */ }
            sys_close(fd);
        }
        iters++;
    }
    return 0;
}
