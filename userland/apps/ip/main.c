// ip - show verbose network interface / IP / DNS / link information.
// Thin wrapper over SYS_NET_INFO; the kernel builds the report (it owns the
// netdev + stack state). Usage: ip
#include "../../libc/stdio.h"
#include "../../libc/syscall.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[2048];
    int n = net_info(buf, sizeof(buf));
    if (n <= 0) {
        printf("ip: no network information available\n");
        return 1;
    }
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    buf[n] = 0;
    printf("%s", buf);
    return 0;
}
