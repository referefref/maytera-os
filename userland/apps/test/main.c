// test - evaluate conditional expressions (POSIX test/[ command)
// Usage: test EXPRESSION
//    or: [ EXPRESSION ]
// Returns exit code 0 (true) or 1 (false).
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "syscall.h"

static int str_to_int(const char *s) {
    int neg = 0;
    int val = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return neg ? -val : val;
}

// Check if a file exists by trying to open it
static int file_exists(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd >= 0) { close(fd); return 1; }
    return 0;
}

// Evaluate a test expression. Returns 0 = true, 1 = false.
static int evaluate(int argc, char **argv, int start, int end) {
    int count = end - start;

    if (count == 0) return 1;  // empty expression is false

    // Unary: ! EXPR (negate)
    if (count >= 2 && strcmp(argv[start], "!") == 0) {
        return evaluate(argc, argv, start + 1, end) == 0 ? 1 : 0;
    }

    // Unary string tests
    if (count == 2) {
        const char *op = argv[start];
        const char *val = argv[start + 1];

        // -z STRING: true if string is empty
        if (strcmp(op, "-z") == 0) return (val[0] == '\0') ? 0 : 1;
        // -n STRING: true if string is non-empty
        if (strcmp(op, "-n") == 0) return (val[0] != '\0') ? 0 : 1;

        // File tests
        if (strcmp(op, "-e") == 0) return file_exists(val) ? 0 : 1;
        if (strcmp(op, "-f") == 0) return file_exists(val) ? 0 : 1;  // Simplified: same as -e
        if (strcmp(op, "-d") == 0) {
            // Try opening as directory
            int fd = open(val, O_RDONLY);
            if (fd < 0) return 1;
            // Try readdir; if it works, it is a directory
            typedef struct { char name[256]; unsigned int type; unsigned int size; } de_t;
            de_t de;
            long r = syscall2(19 /* SYS_READDIR */, fd, (long)&de);
            close(fd);
            return (r == 0) ? 0 : 1;  // readdir succeeded = is a directory
        }
    }

    // Single value: true if non-empty string
    if (count == 1) {
        return (argv[start][0] != '\0') ? 0 : 1;
    }

    // Binary operators (count == 3)
    if (count == 3) {
        const char *left = argv[start];
        const char *op   = argv[start + 1];
        const char *right = argv[start + 2];

        // String comparison
        if (strcmp(op, "=") == 0)  return (strcmp(left, right) == 0) ? 0 : 1;
        if (strcmp(op, "!=") == 0) return (strcmp(left, right) != 0) ? 0 : 1;

        // Integer comparison
        if (strcmp(op, "-eq") == 0) return (str_to_int(left) == str_to_int(right)) ? 0 : 1;
        if (strcmp(op, "-ne") == 0) return (str_to_int(left) != str_to_int(right)) ? 0 : 1;
        if (strcmp(op, "-lt") == 0) return (str_to_int(left) <  str_to_int(right)) ? 0 : 1;
        if (strcmp(op, "-le") == 0) return (str_to_int(left) <= str_to_int(right)) ? 0 : 1;
        if (strcmp(op, "-gt") == 0) return (str_to_int(left) >  str_to_int(right)) ? 0 : 1;
        if (strcmp(op, "-ge") == 0) return (str_to_int(left) >= str_to_int(right)) ? 0 : 1;
    }

    // Unknown expression
    fprintf(stderr, "test: unknown expression\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 1) return 1;

    // Detect if invoked as '['
    const char *prog = argv[0];
    int plen = strlen(prog);
    int bracket_mode = (plen > 0 && prog[plen - 1] == '[');

    int expr_start = 1;
    int expr_end = argc;

    if (bracket_mode) {
        // Last argument must be ']'
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(stderr, "[: missing ']'\n");
            return 2;
        }
        expr_end = argc - 1;
    }

    return evaluate(argc, argv, expr_start, expr_end);
}
