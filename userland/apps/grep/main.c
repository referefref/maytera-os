// grep - Search for pattern in file or stdin
// Usage: grep <pattern> [file]

#include "../../libc/maytera.h"

// Simple substring search (no regex)
static char *my_strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: grep <pattern> [file]\n");
        _exit(1);
    }

    const char *pattern = argv[1];
    int fd = 0; // default stdin

    if (argc >= 3) {
        fd = open(argv[2], 0);
        if (fd < 0) {
            printf("grep: cannot open '%s'\n", argv[2]);
            _exit(1);
        }
    }

    char buf[4096];
    char line[1024];
    int line_pos = 0;
    int n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || line_pos >= (int)sizeof(line) - 1) {
                line[line_pos] = '\0';
                if (my_strstr(line, pattern)) {
                    printf("%s\n", line);
                }
                line_pos = 0;
            } else {
                line[line_pos++] = buf[i];
            }
        }
    }

    // Handle last line without trailing newline
    if (line_pos > 0) {
        line[line_pos] = '\0';
        if (my_strstr(line, pattern)) {
            printf("%s\n", line);
        }
    }

    if (fd > 0) close(fd);
    return 0;
}
