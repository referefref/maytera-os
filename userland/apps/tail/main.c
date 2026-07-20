// tail - Print last N lines of a file
// Usage: tail [-n N] [file]

#include "../../libc/maytera.h"

#define MAX_FILE_SIZE 65536

static char filebuf[MAX_FILE_SIZE];

int main(int argc, char **argv) {
    int num_lines = 10;
    int fd = 0; // default stdin
    int argi = 1;

    // Parse -n option
    if (argi < argc && strcmp(argv[argi], "-n") == 0) {
        argi++;
        if (argi >= argc) {
            printf("tail: option requires an argument -- 'n'\n");
            _exit(1);
        }
        num_lines = atoi(argv[argi]);
        argi++;
    }

    // Open file if specified
    if (argi < argc) {
        fd = open(argv[argi], 0);
        if (fd < 0) {
            printf("tail: cannot open '%s'\n", argv[argi]);
            _exit(1);
        }
    }

    // Read entire file into buffer
    int total = 0;
    int n;
    while (total < MAX_FILE_SIZE && (n = read(fd, filebuf + total, MAX_FILE_SIZE - total)) > 0) {
        total += n;
    }

    if (fd > 0) close(fd);

    // Count newlines from the end to find where to start printing
    int count = 0;
    int start = total;
    for (int i = total - 1; i >= 0; i--) {
        if (filebuf[i] == '\n') {
            count++;
            if (count == num_lines + 1) {
                start = i + 1;
                break;
            }
        }
    }
    if (count <= num_lines) start = 0;

    // Print from start to end
    write(1, filebuf + start, total - start);
    return 0;
}
