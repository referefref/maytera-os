// wc - Word, line, and byte count
// Usage: wc [file]

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    int fd = 0; // default stdin
    const char *filename = NULL;

    if (argc >= 2) {
        filename = argv[1];
        fd = open(filename, 0);
        if (fd < 0) {
            printf("wc: cannot open '%s'\n", filename);
            _exit(1);
        }
    }

    int lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[4096];
    int n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        bytes += n;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') lines++;
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r') {
                in_word = 0;
            } else {
                if (!in_word) words++;
                in_word = 1;
            }
        }
    }

    if (filename) {
        printf("%d %d %d %s\n", lines, words, bytes, filename);
    } else {
        printf("%d %d %d\n", lines, words, bytes);
    }

    if (fd > 0) close(fd);
    return 0;
}
