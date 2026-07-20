// echo - print arguments
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

int main(int argc, char **argv) {
    int newline = 1;
    int start = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        start = 2;
    }
    for (int i = start; i < argc; i++) {
        if (i > start) write(1, " ", 1);
        write(1, argv[i], (long)strlen(argv[i]));
    }
    if (newline) write(1, "\n", 1);
    return 0;
}
