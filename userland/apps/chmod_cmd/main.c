// chmod - Change file permissions for MayteraOS

#include "../../libc/maytera.h"
#include "../../libc/unistd.h"

// Parse octal mode string (e.g., "0755" or "755")
static int parse_mode(const char *s, mode_t *mode) {
    mode_t val = 0;
    while (*s) {
        if (*s < '0' || *s > '7') return -1;
        val = (val << 3) | (*s - '0');
        s++;
    }
    *mode = val;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: chmod <mode> <file> [file2 ...]\n");
        printf("  mode: octal permission bits (e.g., 0755, 0644)\n");
        return 1;
    }

    mode_t mode;
    if (parse_mode(argv[1], &mode) != 0) {
        printf("chmod: invalid mode '%s'\n", argv[1]);
        return 1;
    }

    int errors = 0;
    for (int i = 2; i < argc; i++) {
        if (chmod(argv[i], mode) != 0) {
            printf("chmod: cannot change permissions of '%s': permission denied\n", argv[i]);
            errors++;
        }
    }

    return errors ? 1 : 0;
}
