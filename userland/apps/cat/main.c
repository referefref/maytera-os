// cat - concatenate files to stdout
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "errno.h"
#include "fcntl.h"

static int cat_fd(int fd) {
    char buf[4096];
    for (;;) {
        long n = read(fd, buf, sizeof(buf));
        if (n == 0) return 0;
        if (n < 0) { perror("read"); return 1; }
        long off = 0;
        while (off < n) {
            long w = write(1, buf + off, n - off);
            if (w <= 0) { perror("write"); return 1; }
            off += w;
        }
    }
}

static int cat_path(const char *path) {
    char buf[512];
    const char *target = path;
    if (path[0] != '\0' && path[0] != '/') {
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
        int j = 0;
        for (int i = 0; cwd[i] && j < (int)sizeof(buf) - 1; i++) buf[j++] = cwd[i];
        if (j > 0 && buf[j - 1] != '/' && j < (int)sizeof(buf) - 1) buf[j++] = '/';
        for (int k = 0; path[k] && j < (int)sizeof(buf) - 1; k++) buf[j++] = path[k];
        buf[j] = 0;
        target = buf;
    }
    int fd = open(target, O_RDONLY);
    if (fd < 0) { perror(path); return 1; }
    int rc = cat_fd(fd);
    close(fd);
    return rc;
}

int main(int argc, char **argv) {
    if (argc <= 1) return cat_fd(0);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            if (cat_fd(0) != 0) rc = 1;
        } else {
            if (cat_path(argv[i]) != 0) rc = 1;
        }
    }
    return rc;
}
