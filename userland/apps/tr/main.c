// tr - Translate characters
// Usage: tr <set1> <set2>
// Replaces characters in set1 with corresponding characters in set2

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: tr <set1> <set2>\n");
        _exit(1);
    }

    const char *set1 = argv[1];
    const char *set2 = argv[2];
    int len1 = strlen(set1);
    int len2 = strlen(set2);

    char buf[4096];
    int n;

    while ((n = read(0, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            for (int j = 0; j < len1; j++) {
                if (c == set1[j]) {
                    // Use corresponding char from set2, or last char if set2 is shorter
                    c = (j < len2) ? set2[j] : set2[len2 - 1];
                    break;
                }
            }
            putchar(c);
        }
    }

    return 0;
}
