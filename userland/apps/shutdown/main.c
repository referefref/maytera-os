// shutdown - power off (or, with -r, reboot) the machine via ACPI (#432)
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"
#include "../../libc/string.h"

int main(int argc, char **argv) {
    int do_reboot = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--reboot") == 0) do_reboot = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--halt") == 0 ||
                 strcmp(argv[i], "-P") == 0 || strcmp(argv[i], "--poweroff") == 0) do_reboot = 0;
    }
    if (do_reboot) {
        printf("Rebooting MayteraOS...\n");
        reboot();
        printf("shutdown: ACPI reboot not available\n");
    } else {
        printf("Powering off MayteraOS...\n");
        poweroff();
        printf("shutdown: ACPI power-off not available\n");
    }
    return 1;
}
