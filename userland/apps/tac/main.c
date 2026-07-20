// tac - concatenate and print files in reverse (line by line)
// Usage: tac [FILE...]
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "errno.h"

#define BUF_SIZE 65536

static char buf[BUF_SIZE];

static int tac_fd(int fd) {
    // Read entire file into buffer
    int total = 0;
    long n;
    while (total < BUF_SIZE - 1) {
        n = read(fd, buf + total, BUF_SIZE - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';

    if (total == 0) return 0;

    // Find all line starts
    int line_starts[4096];
    int nlines = 0;
    line_starts[nlines++] = 0;
    for (int i = 0; i < total; i++) {
        if (buf[i] == '\n' && i + 1 < total) {
            if (nlines < 4096)
                line_starts[nlines++] = i + 1;
        }
    }

    // Print lines in reverse order
    for (int i = nlines - 1; i >= 0; i--) {
        int start = line_starts[i];
        int end = (i + 1 < nlines) ? line_starts[i + 1] : total;
        write(1, buf + start, end - start);
        // Ensure each line ends with newline
        if (end > start && buf[end - 1] != '\n') {
            write(1, "\n", 1);
        }
    }
    return 0;
}

static int tac_path(const char *path) {
    char full[512];
    const char *target = path;
    if (path[0] != '/' && path[0] != '\0') {
        char cwd_buf[256];
        if (!getcwd(cwd_buf, sizeof(cwd_buf))) cwd_buf[0] = 0;
        int j = 0;
        for (int i = 0; cwd_buf[i] && j < 510; i++) full[j++] = cwd_buf[i];
        if (j > 0 && full[j-1] != '/' && j < 511) full[j++] = '/';
        for (int k = 0; path[k] && j < 511; k++) full[j++] = path[k];
        full[j] = 0;
        target = full;
    }
    int fd = open(target, O_RDONLY);
    if (fd < 0) { perror(path); return 1; }
    int rc = tac_fd(fd);
    close(fd);
    return rc;
}

int main(int argc, char **argv) {
    if (argc <= 1) return tac_fd(0);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0)
            rc |= tac_fd(0);
        else
            rc |= tac_path(argv[i]);
    }
    return rc;
}
