// rmdir - Remove an empty directory
// Usage: rmdir <path>

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: rmdir <path>\n");
        _exit(1);
    }

    int ret = rmdir(argv[1]);
    if (ret < 0) {
        printf("rmdir: cannot remove '%s': error %d\n", argv[1], ret);
        _exit(1);
    }

    return 0;
}
