// mkdir - Create a directory
// Usage: mkdir <path>

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: mkdir <path>\n");
        _exit(1);
    }

    int ret = mkdir(argv[1], 0755);
    if (ret < 0) {
        printf("mkdir: cannot create directory '%s': error %d\n", argv[1], ret);
        _exit(1);
    }

    return 0;
}
