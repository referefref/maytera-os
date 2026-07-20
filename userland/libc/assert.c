// assert.c - assertion failure handler for MayteraOS userland (#422).
#include "assert.h"
#include "stdio.h"
#include "stdlib.h"

void __assert_fail(const char *expr, const char *file, int line, const char *func) {
    fprintf(stderr, "assertion failed: %s (%s:%d in %s)\n",
            expr ? expr : "?", file ? file : "?", line, func ? func : "?");
    fflush(stderr);
    abort();
}
