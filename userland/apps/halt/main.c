// halt - power off the machine via ACPI (#432)
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("Halting MayteraOS...\n");
    poweroff();   // SYS_POWEROFF (206)
    printf("halt: ACPI power-off not available on this machine\n");
    return 1;
}
