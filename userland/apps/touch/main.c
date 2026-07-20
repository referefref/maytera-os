// touch - Create an empty file or update its timestamp
// Usage: touch <path>

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: touch <path>\n");
        _exit(1);
    }

    // O_CREAT | O_WRONLY = 0x0041
    int fd = open(argv[1], 0x0041);
    if (fd < 0) {
        printf("touch: cannot touch '%s': error %d\n", argv[1], fd);
        _exit(1);
    }

    close(fd);
    return 0;
}
