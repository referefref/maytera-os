// stat - Display file information
// Usage: stat <path>

#include "../../libc/maytera.h"

// Seek whence constants
#define SEEK_SET 0
#define SEEK_END 2

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: stat <path>\n");
        _exit(1);
    }

    int fd = open(argv[1], 0);  // O_RDONLY
    if (fd < 0) {
        printf("stat: cannot open '%s': error %d\n", argv[1], fd);
        _exit(1);
    }

    // Seek to end to determine file size
    long size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        printf("stat: cannot determine size of '%s': error %ld\n", argv[1], size);
        close(fd);
        _exit(1);
    }

    printf("  File: %s\n", argv[1]);
    printf("  Size: %ld bytes\n", size);

    close(fd);
    return 0;
}
