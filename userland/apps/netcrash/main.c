// netcrash - regression trigger for the #NETDROP fault-kills-the-NIC bug.
//
// Sleeps long enough for DHCP to bind and the desktop to settle, then takes a
// deliberate user-mode page fault (NULL deref). On a kernel where
// crashhandler_report() leaves the e1000 in crash context, the NIC goes
// carrier=0 permanently at this instant and never comes back. On a fixed
// kernel the process dies and the NIC keeps running.
//
// NOT SHIPPED (see build/unshipped-apps.list). Test tool only.
#include "../../libc/stdio.h"
#include "../../libc/unistd.h"

int main(void) {
    printf("[NETCRASH] armed; sleeping 90s before the deliberate fault\n");
    for (int i = 0; i < 9; i++) {
        sleep(10);
        printf("[NETCRASH] t+%ds\n", (i + 1) * 10);
    }
    printf("[NETCRASH] FAULTING NOW (NULL deref)\n");
    volatile int *p = (volatile int *)0;
    return *p;
}
