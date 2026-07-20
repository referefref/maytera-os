// yes - Repeatedly output a string (default "y")
// Usage: yes [string]

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    const char *str = "y";
    if (argc >= 2) {
        str = argv[1];
    }

    int len = strlen(str);

    for (;;) {
        write(1, str, len);
        write(1, "\n", 1);
    }

    return 0;
}
