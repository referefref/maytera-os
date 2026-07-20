// reboot - restart the machine via ACPI (#432)
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("Rebooting MayteraOS...\n");
    reboot();   // SYS_REBOOT (207)
    // Only reached if ACPI reboot is unavailable.
    printf("reboot: ACPI reboot not available on this machine\n");
    return 1;
}
