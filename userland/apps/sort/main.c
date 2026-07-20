// sort - Sort lines alphabetically
// Usage: sort [file]

#include "../../libc/maytera.h"

#define MAX_LINES 1000
#define MAX_LINE_LEN 256

static char lines[MAX_LINES][MAX_LINE_LEN];

int main(int argc, char **argv) {
    int fd = 0; // default stdin

    if (argc >= 2) {
        fd = open(argv[1], 0);
        if (fd < 0) {
            printf("sort: cannot open '%s'\n", argv[1]);
            _exit(1);
        }
    }

    // Read all lines
    int num_lines = 0;
    int pos = 0;
    char buf[4096];
    int n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || pos >= MAX_LINE_LEN - 1) {
                lines[num_lines][pos] = '\0';
                num_lines++;
                pos = 0;
                if (num_lines >= MAX_LINES) break;
            } else {
                lines[num_lines][pos++] = buf[i];
            }
        }
        if (num_lines >= MAX_LINES) break;
    }

    // Handle last line without trailing newline
    if (pos > 0 && num_lines < MAX_LINES) {
        lines[num_lines][pos] = '\0';
        num_lines++;
    }

    if (fd > 0) close(fd);

    // Bubble sort
    for (int i = 0; i < num_lines - 1; i++) {
        for (int j = 0; j < num_lines - 1 - i; j++) {
            if (strcmp(lines[j], lines[j + 1]) > 0) {
                char tmp[MAX_LINE_LEN];
                memcpy(tmp, lines[j], MAX_LINE_LEN);
                memcpy(lines[j], lines[j + 1], MAX_LINE_LEN);
                memcpy(lines[j + 1], tmp, MAX_LINE_LEN);
            }
        }
    }

    // Print sorted lines
    for (int i = 0; i < num_lines; i++) {
        printf("%s\n", lines[i]);
    }

    return 0;
}
