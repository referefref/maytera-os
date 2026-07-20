// sed - minimal stream editor (literal s/find/replace/[g])
// Supports a single substitution command: s/FIND/REPLACE/[g]
// FIND is a literal string (no regex). Without trailing 'g' only the first
// match on each line is replaced; with 'g' every match on the line is.
// Reads from the named file, or from stdin when no file is given.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "errno.h"
#include "fcntl.h"

static char in_buf[262144];   // up to 256 KB of input
static char out_buf[4096];
static int  out_len = 0;

static void out_flush(void) {
    int off = 0;
    while (off < out_len) {
        long w = write(1, out_buf + off, out_len - off);
        if (w <= 0) break;
        off += w;
    }
    out_len = 0;
}

static void out_ch(char c) {
    if (out_len >= (int)sizeof(out_buf)) out_flush();
    out_buf[out_len++] = c;
}

static void out_str(const char *s, int n) {
    for (int i = 0; i < n; i++) out_ch(s[i]);
}

// Apply the substitution to one line [line, line+len) (no newline included).
static void sub_line(const char *line, int len,
                     const char *find, int flen,
                     const char *repl, int rlen, int global) {
    int i = 0;
    int done = 0;   // already replaced once (for non-global)
    while (i < len) {
        if ((global || !done) && flen > 0 && i + flen <= len &&
            memcmp(line + i, find, flen) == 0) {
            out_str(repl, rlen);
            i += flen;
            done = 1;
        } else {
            out_ch(line[i++]);
        }
    }
}

int main(int argc, char **argv) {
    // Locate the s-command and optional file argument.
    const char *expr = 0;
    const char *path = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == 's' && argv[i][1] == '/' && !expr) expr = argv[i];
        else if (!path) path = argv[i];
    }
    if (!expr) {
        fputs("usage: sed s/find/replace/[g] [file]\n", stderr);
        return 1;
    }

    // Parse s/FIND/REPLACE/[g] honoring backslash escapes before delimiters.
    char find[1024], repl[1024];
    int fl = 0, rl = 0;
    const char *p = expr + 2;       // skip "s/"
    while (*p && *p != '/' && fl < (int)sizeof(find) - 1) {
        if (*p == '\\' && p[1]) p++;
        find[fl++] = *p++;
    }
    if (*p != '/') { fputs("sed: malformed s-command\n", stderr); return 1; }
    find[fl] = 0;
    p++;                            // skip second '/'
    while (*p && *p != '/' && rl < (int)sizeof(repl) - 1) {
        if (*p == '\\' && p[1]) { p++; if (*p == 'n') { repl[rl++]='\n'; p++; continue; } }
        repl[rl++] = *p++;
    }
    repl[rl] = 0;
    int global = 0;
    if (*p == '/') { p++; if (*p == 'g') global = 1; }
    if (fl == 0) { fputs("sed: empty pattern\n", stderr); return 1; }

    // Read all input.
    int fd = 0;
    if (path) {
        char abspath[512];
        const char *target = path;
        if (path[0] != '/') {
            char cwd[256];
            if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
            int j = 0;
            for (int i = 0; cwd[i] && j < (int)sizeof(abspath) - 1; i++) abspath[j++] = cwd[i];
            if (j > 0 && abspath[j-1] != '/' && j < (int)sizeof(abspath)-1) abspath[j++] = '/';
            for (int k = 0; path[k] && j < (int)sizeof(abspath)-1; k++) abspath[j++] = path[k];
            abspath[j] = 0;
            target = abspath;
        }
        fd = open(target, O_RDONLY);
        if (fd < 0) { perror(path); return 1; }
    }

    int total = 0;
    for (;;) {
        long n = read(fd, in_buf + total, sizeof(in_buf) - 1 - total);
        if (n <= 0) break;
        total += (int)n;
        if (total >= (int)sizeof(in_buf) - 1) break;
    }
    if (fd != 0) close(fd);

    // Process line by line, preserving line terminators.
    int start = 0;
    for (int i = 0; i <= total; i++) {
        if (i == total || in_buf[i] == '\n') {
            sub_line(in_buf + start, i - start, find, fl, repl, rl, global);
            if (i < total) out_ch('\n');
            start = i + 1;
        }
    }
    out_flush();
    return 0;
}
