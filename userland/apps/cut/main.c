// cut - Extract fields from lines
// Usage: cut -d<delim> -f<field> [file]

#include "../../libc/maytera.h"

int main(int argc, char **argv) {
    char delim = '\t'; // default delimiter
    int field = 1;     // default field (1-based)
    int fd = 0;        // default stdin
    int argi = 1;

    // Parse options
    while (argi < argc && argv[argi][0] == '-') {
        if (argv[argi][1] == 'd') {
            // Delimiter: -d<char> or -d <char>
            if (argv[argi][2]) {
                delim = argv[argi][2];
            } else {
                argi++;
                if (argi >= argc) { printf("cut: missing delimiter\n"); _exit(1); }
                delim = argv[argi][0];
            }
        } else if (argv[argi][1] == 'f') {
            // Field: -f<num> or -f <num>
            if (argv[argi][2]) {
                field = atoi(&argv[argi][2]);
            } else {
                argi++;
                if (argi >= argc) { printf("cut: missing field number\n"); _exit(1); }
                field = atoi(argv[argi]);
            }
        }
        argi++;
    }

    // Open file if specified
    if (argi < argc) {
        fd = open(argv[argi], 0);
        if (fd < 0) {
            printf("cut: cannot open '%s'\n", argv[argi]);
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
                // Extract field
                int cur_field = 1;
                int start = 0;
                for (int j = 0; j <= line_pos; j++) {
                    if (line[j] == delim || line[j] == '\0') {
                        if (cur_field == field) {
                            line[j] = '\0';
                            printf("%s\n", &line[start]);
                            break;
                        }
                        cur_field++;
                        start = j + 1;
                    }
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
        int cur_field = 1;
        int start = 0;
        for (int j = 0; j <= line_pos; j++) {
            if (line[j] == delim || line[j] == '\0') {
                if (cur_field == field) {
                    line[j] = '\0';
                    printf("%s\n", &line[start]);
                    break;
                }
                cur_field++;
                start = j + 1;
            }
        }
    }

    if (fd > 0) close(fd);
    return 0;
}
