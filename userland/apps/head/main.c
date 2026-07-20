// head - Print first N lines of a file
// Usage: head [-n N] [file]

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    int num_lines = 10;
    int fd = 0; // default stdin
    int argi = 1;

    // Parse -n option
    if (argi < argc && strcmp(argv[argi], "-n") == 0) {
        argi++;
        if (argi >= argc) {
            printf("head: option requires an argument -- 'n'\n");
            _exit(1);
        }
        num_lines = atoi(argv[argi]);
        argi++;
    }

    // Open file if specified
    if (argi < argc) {
        fd = open(argv[argi], 0);
        if (fd < 0) {
            printf("head: cannot open '%s'\n", argv[argi]);
            _exit(1);
        }
    }

    char buf[4096];
    int lines_printed = 0;
    int n;

    while (lines_printed < num_lines && (n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n && lines_printed < num_lines; i++) {
            putchar(buf[i]);
            if (buf[i] == '\n') lines_printed++;
        }
    }

    if (fd > 0) close(fd);
    return 0;
}
