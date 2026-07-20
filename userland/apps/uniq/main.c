// uniq - Remove adjacent duplicate lines
// Usage: uniq [file]

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    int fd = 0; // default stdin

    if (argc >= 2) {
        fd = open(argv[1], 0);
        if (fd < 0) {
            printf("uniq: cannot open '%s'\n", argv[1]);
            _exit(1);
        }
    }

    char prev[1024];
    char line[1024];
    int prev_valid = 0;
    int line_pos = 0;
    char buf[4096];
    int n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || line_pos >= (int)sizeof(line) - 1) {
                line[line_pos] = '\0';
                if (!prev_valid || strcmp(line, prev) != 0) {
                    printf("%s\n", line);
                    memcpy(prev, line, line_pos + 1);
                    prev_valid = 1;
                }
                line_pos = 0;
            } else {
                line[line_pos++] = buf[i];
            }
        }
    }

    // Handle last line without trailing newline
    if (line_pos > 0) {
        line[line_pos] = '\0';
        if (!prev_valid || strcmp(line, prev) != 0) {
            printf("%s\n", line);
        }
    }

    if (fd > 0) close(fd);
    return 0;
}
