// cp - Copy a file
// Usage: cp <source> <dest>

#include "../../libc/maytera.h"

#define BUF_SIZE 4096

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: cp <source> <dest>\n");
        _exit(1);
    }

    int src_fd = open(argv[1], 0);  // O_RDONLY = 0
    if (src_fd < 0) {
        printf("cp: cannot open '%s': error %d\n", argv[1], src_fd);
        _exit(1);
    }

    // O_WRONLY | O_CREAT | O_TRUNC = 0x0241
    int dst_fd = open(argv[2], 0x0241);
    if (dst_fd < 0) {
        printf("cp: cannot create '%s': error %d\n", argv[2], dst_fd);
        close(src_fd);
        _exit(1);
    }

    char buf[BUF_SIZE];
    long bytes_read;

    while ((bytes_read = read(src_fd, buf, BUF_SIZE)) > 0) {
        long written = write(dst_fd, buf, bytes_read);
        if (written < 0) {
            printf("cp: write error: %ld\n", written);
            close(src_fd);
            close(dst_fd);
            _exit(1);
        }
    }

    if (bytes_read < 0) {
        printf("cp: read error: %ld\n", bytes_read);
        close(src_fd);
        close(dst_fd);
        _exit(1);
    }

    close(src_fd);
    close(dst_fd);
    return 0;
}
