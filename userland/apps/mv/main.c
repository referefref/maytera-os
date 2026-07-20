// mv - Move or rename a file
// Usage: mv <source> <dest>
// Tries rename() first; falls back to copy + unlink if rename fails.

#include "../../libc/maytera.h"

#define BUF_SIZE 4096

static int copy_file(const char *src, const char *dst) {
    int src_fd = open(src, 0);  // O_RDONLY
    if (src_fd < 0) return src_fd;

    int dst_fd = open(dst, 0x0241);  // O_WRONLY | O_CREAT | O_TRUNC
    if (dst_fd < 0) {
        close(src_fd);
        return dst_fd;
    }

    char buf[BUF_SIZE];
    long bytes_read;

    while ((bytes_read = read(src_fd, buf, BUF_SIZE)) > 0) {
        long written = write(dst_fd, buf, bytes_read);
        if (written < 0) {
            close(src_fd);
            close(dst_fd);
            return (int)written;
        }
    }

    close(src_fd);
    close(dst_fd);

    if (bytes_read < 0) return (int)bytes_read;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: mv <source> <dest>\n");
        _exit(1);
    }

    // Try atomic rename first
    int ret = rename(argv[1], argv[2]);
    if (ret == 0) return 0;

    // Fallback: copy then remove source
    ret = copy_file(argv[1], argv[2]);
    if (ret < 0) {
        printf("mv: cannot move '%s' to '%s': error %d\n", argv[1], argv[2], ret);
        _exit(1);
    }

    ret = unlink(argv[1]);
    if (ret < 0) {
        printf("mv: copied but cannot remove source '%s': error %d\n", argv[1], ret);
        _exit(1);
    }

    return 0;
}
