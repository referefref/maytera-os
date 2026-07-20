// tee - Duplicate stdin to stdout and a file
// Usage: tee <file>

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: tee <file>\n");
        _exit(1);
    }

    int fd = open(argv[1], 1); // open for writing (O_WRONLY or create)
    if (fd < 0) {
        printf("tee: cannot open '%s' for writing\n", argv[1]);
        _exit(1);
    }

    char buf[4096];
    int n;

    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, n);  // write to stdout
        write(fd, buf, n); // write to file
    }

    close(fd);
    return 0;
}
