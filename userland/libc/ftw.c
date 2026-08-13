// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// ftw.c - ftw()/nftw(). The full contract, and every refusal, is in ftw.h.
//
// ONE DESIGN DECISION WORTH THE COMMENT. Each directory is read to completion
// into a heap-held name list and CLOSED before any child is descended into.
// That is not just to honour nopenfd cheaply: rewinddir() in this libc is a
// documented stub, so a DIR* cannot be re-positioned, and holding directories
// open across the recursion would also mean the fd count scaled with depth.
// Reading then closing sidesteps both, at the cost of one allocation per
// directory, which for a file tree walk is nothing.
#include "ftw.h"
#include "dirent.h"
#include "sys/stat.h"
#include "sys/param.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"

typedef struct {
    char *path;                 // shared PATH_MAX buffer, built up as we descend
    int   flags;
    int (*fn3)(const char *, const struct stat *, int);
    int (*fn4)(const char *, const struct stat *, int, struct FTW *);
} ftw_ctx;

static int ftw_call(ftw_ctx *c, const struct stat *st, int type, int base, int level) {
    if (c->fn4) {
        struct FTW f;
        f.base  = base;
        f.level = level;
        return c->fn4(c->path, st, type, &f);
    }
    return c->fn3(c->path, st, type);
}

// Walk the tree rooted at c->path (which is plen bytes long, NUL terminated).
// Returns 0, the callback's non-zero value, or -1 with errno set.
static int ftw_walk(ftw_ctx *c, size_t plen, int base, int level) {
    struct stat st;
    int type, rc;
    char **names = 0;
    int n = 0, cap = 0, i;

    if (level >= FTW_MAX_DEPTH) { errno = ELOOP; return -1; }

    memset(&st, 0, sizeof(st));
    if (stat(c->path, &st) != 0) {
        memset(&st, 0, sizeof(st));   // POSIX: contents undefined for FTW_NS
        type = FTW_NS;
    } else if (S_ISDIR(st.st_mode)) {
        type = FTW_D;
    } else {
        type = FTW_F;
    }

    if (type == FTW_D) {
        // Read the whole directory now, then close it. See the file header.
        DIR *d = opendir(c->path);
        if (!d) {
            type = FTW_DNR;
        } else {
            struct dirent *e;
            while ((e = readdir(d)) != 0) {
                size_t nl;
                char *copy;
                if (e->d_name[0] == '.' &&
                    (e->d_name[1] == '\0' ||
                     (e->d_name[1] == '.' && e->d_name[2] == '\0')))
                    continue;                       // skip "." and ".."
                if (n == cap) {
                    int ncap = cap ? cap * 2 : 16;
                    char **nn = (char **)realloc(names, (size_t)ncap * sizeof(char *));
                    if (!nn) { closedir(d); goto oom; }
                    names = nn; cap = ncap;
                }
                nl = strlen(e->d_name);
                copy = (char *)malloc(nl + 1);
                if (!copy) { closedir(d); goto oom; }
                memcpy(copy, e->d_name, nl + 1);
                names[n++] = copy;
            }
            closedir(d);
        }
    }

    // Pre-order visit, unless the caller asked for post-order.
    if (!(c->flags & FTW_DEPTH)) {
        rc = ftw_call(c, &st, type, base, level);
        if (rc != 0) goto done_rc;
    }

    for (i = 0; i < n; i++) {
        size_t nl = strlen(names[i]);
        size_t at = plen;
        if (at > 0 && c->path[at - 1] != '/') {
            if (at + 1 >= (size_t)PATH_MAX) { errno = ENAMETOOLONG; rc = -1; goto done_rc; }
            c->path[at++] = '/';
        }
        if (at + nl + 1 > (size_t)PATH_MAX) { errno = ENAMETOOLONG; rc = -1; goto done_rc; }
        memcpy(c->path + at, names[i], nl + 1);
        rc = ftw_walk(c, at + nl, (int)at, level + 1);
        c->path[plen] = '\0';           // restore for the next sibling
        if (rc != 0) goto done_rc;
    }

    if (c->flags & FTW_DEPTH) {
        rc = ftw_call(c, &st, type == FTW_D ? FTW_DP : type, base, level);
        if (rc != 0) goto done_rc;
    }

    rc = 0;
done_rc:
    for (i = 0; i < n; i++) free(names[i]);
    free(names);
    return rc;

oom:
    for (i = 0; i < n; i++) free(names[i]);
    free(names);
    errno = ENOMEM;
    return -1;
}

static int ftw_start(const char *dirpath,
                     int (*fn3)(const char *, const struct stat *, int),
                     int (*fn4)(const char *, const struct stat *, int, struct FTW *),
                     int nopenfd, int flags) {
    ftw_ctx c;
    struct stat probe;
    size_t len;
    int base, i, rc;

    if (!dirpath || (!fn3 && !fn4)) { errno = EINVAL; return -1; }
    // Exactly one directory descriptor is ever open, so any budget of one or
    // more is satisfiable; zero or negative is a caller error.
    if (nopenfd < 1) { errno = EINVAL; return -1; }
    // Refuse what cannot be honoured, rather than ignore it. See ftw.h.
    if (flags & ~(FTW_PHYS | FTW_DEPTH)) { errno = EINVAL; return -1; }

    len = strlen(dirpath);
    if (len == 0 || len + 1 > (size_t)PATH_MAX) { errno = ENAMETOOLONG; return -1; }

    // Fail before calling the callback at all if the root is not there. A
    // caller that gets 0 back has walked something.
    if (stat(dirpath, &probe) != 0) { errno = ENOENT; return -1; }

    c.path = (char *)malloc((size_t)PATH_MAX + 1);
    if (!c.path) { errno = ENOMEM; return -1; }
    memcpy(c.path, dirpath, len + 1);
    c.flags = flags;
    c.fn3   = fn3;
    c.fn4   = fn4;

    // base of the root is the offset of its own last component.
    base = 0;
    for (i = 0; i < (int)len; i++) if (dirpath[i] == '/') base = i + 1;
    if (base >= (int)len) base = 0;    // trailing slash: point at the whole thing

    rc = ftw_walk(&c, len, base, 0);
    free(c.path);
    return rc;
}

int ftw(const char *dirpath,
        int (*fn)(const char *fpath, const struct stat *sb, int typeflag),
        int nopenfd) {
    return ftw_start(dirpath, fn, 0, nopenfd, 0);
}

int nftw(const char *dirpath,
         int (*fn)(const char *fpath, const struct stat *sb, int typeflag,
                   struct FTW *ftwbuf),
         int nopenfd, int flags) {
    return ftw_start(dirpath, 0, fn, nopenfd, flags);
}
