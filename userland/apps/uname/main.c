// uname - Print system information for MayteraOS

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    int show_all = 0;

    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        show_all = 1;
    }

    if (show_all) {
        printf("MayteraOS maytera 1.8.2 x86_64\n");
    } else {
        printf("MayteraOS\n");
    }

    return 0;
}
