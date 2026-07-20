// rm - Remove a file
// Usage: rm <path>

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: rm <path>\n");
        _exit(1);
    }

    int ret = unlink(argv[1]);
    if (ret < 0) {
        printf("rm: cannot remove '%s': error %d\n", argv[1], ret);
        _exit(1);
    }

    return 0;
}
