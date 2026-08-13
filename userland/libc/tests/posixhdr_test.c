// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// posixhdr_test.c - #745 (local 72) battery for the tier-1 POSIX headers added
// to userland/libc: getopt, libgen, sys/uio, sys/utsname, sys/file, utime,
// ftw, endian, byteswap, sys/param, alloca.
//
// This harness is compiled FREESTANDING against the real libc headers, exactly
// as the shipping build compiles them (-nostdinc -I<libc>), and is linked
// against the SHIPPING objects, not against copies. What it does use the host
// for is the things a hosted process has and MayteraOS provides through
// syscalls: printf, malloc, and a real pipe() for the readv/writev arm.
//
// Three subsystems need a stand-in for the kernel, and each stand-in is
// declared here rather than borrowed from glibc so that the STRUCT LAYOUTS the
// unit under test compiled against are the ones the stand-in fills in:
//   * ftw    - an in-memory directory tree behind opendir/readdir/closedir and
//              stat, which deliberately emits "." and ".." so the walk is
//              proved to skip them.
//   * uname  - a syscall2() stub answering SYS_GET_VERSION, so the version
//              PARSE is what is under test.
//   * stat   - see ftw above; sys/stat.o is not linked into this binary.
#include "getopt.h"
#include "libgen.h"
#include "ftw.h"
#include "utime.h"
#include "endian.h"
#include "byteswap.h"
#include "alloca.h"
#include "sys/uio.h"
#include "sys/utsname.h"
#include "sys/file.h"
#include "sys/param.h"
#include "sys/stat.h"
#include "dirent.h"
#include "string.h"
#include "errno.h"

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned long);
extern void free(void *);
extern int pipe(int fds[2]);
extern long read(int fd, void *buf, unsigned long n);
extern long write(int fd, const void *buf, unsigned long n);
extern int close(int fd);

// The host's glibc (2.36) has no strlcpy, and the shipping string.c cannot be
// linked in here because it calls the hand-written memcpy_fast/memset_fast
// assembly. sys/utsname.c uses strlcpy (the shared primitive, as it should),
// so the harness supplies the BSD-standard one: copy with truncation, always
// NUL terminate, return the length it wanted. strlcpy is not the unit under
// test; uname's version parse is.
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t n = 0;
    while (src[n]) n++;
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        size_t i;
        for (i = 0; i < c; i++) dst[i] = src[i];
        dst[c] = '\0';
    }
    return n;
}

static int fails = 0;
static int checks = 0;

#define CK(cond, msg) do { checks++; if (!(cond)) { \
    printf("FAIL  %-34s  line %d\n", (msg), __LINE__); fails++; } } while (0)
#define CKSTR(got, want, msg) do { checks++; \
    const char *g_ = (got), *w_ = (want); \
    if (!g_ || strcmp(g_, w_) != 0) { \
        printf("FAIL  %-34s  line %d: got \"%s\" want \"%s\"\n", (msg), __LINE__, \
               g_ ? g_ : "(null)", w_); fails++; } } while (0)
#define CKINT(got, want, msg) do { checks++; long g_ = (long)(got), w_ = (long)(want); \
    if (g_ != w_) { printf("FAIL  %-34s  line %d: got %ld want %ld\n", (msg), \
                           __LINE__, g_, w_); fails++; } } while (0)

// ===========================================================================
// Kernel stand-ins
// ===========================================================================

// --- version, for uname ----------------------------------------------------
static const char *g_version_str = "1.9.0 (build 1234)";
static int g_version_fail = 0;

long syscall0(long n);
long syscall1(long n, long a);
long syscall2(long n, long a, long b);

long syscall0(long n) { (void)n; return -1; }
long syscall1(long n, long a) { (void)n; (void)a; return -1; }
long syscall2(long n, long a, long b) {
    if (n == 246) {   // SYS_GET_VERSION
        char *dst = (char *)a;
        int cap = (int)b, i = 0;
        if (g_version_fail) return -1;
        while (g_version_str[i] && i < cap - 1) { dst[i] = g_version_str[i]; i++; }
        dst[i] = 0;
        return i;
    }
    return -1;
}

// --- an in-memory file tree, for ftw ---------------------------------------
typedef struct { const char *path; int isdir; int openfail; } fnode_t;

static const fnode_t g_fs[] = {
    { "/r",         1, 0 },
    { "/r/a",       1, 0 },
    { "/r/a/f1",    0, 0 },
    { "/r/a/b",     1, 0 },
    { "/r/a/b/f2",  0, 0 },
    { "/r/dnr",     1, 1 },   // a directory opendir() refuses: must be FTW_DNR
    { "/r/f3",      0, 0 },
};
#define NFS_NODES ((int)(sizeof(g_fs) / sizeof(g_fs[0])))

static const fnode_t *fs_find(const char *p) {
    int i;
    for (i = 0; i < NFS_NODES; i++)
        if (strcmp(g_fs[i].path, p) == 0) return &g_fs[i];
    return 0;
}

// Is `child` a direct child of `dir`?
static int fs_is_child(const char *dir, const char *child) {
    size_t dl = strlen(dir);
    const char *rest;
    if (strncmp(child, dir, dl) != 0) return 0;
    if (child[dl] != '/') return 0;
    rest = child + dl + 1;
    if (*rest == '\0') return 0;
    return strchr(rest, '/') == 0;
}

int stat(const char *path, struct stat *st);
int stat(const char *path, struct stat *st) {
    const fnode_t *n = fs_find(path);
    if (!n) { errno = ENOENT; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_mode  = n->isdir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
    st->st_nlink = 1;
    return 0;
}

struct DIR { const char *dir; int pos; struct dirent ent; };

static int g_open_dirs = 0;      // proves only one directory is open at a time
static int g_max_open_dirs = 0;

DIR *opendir(const char *name);
DIR *opendir(const char *name) {
    const fnode_t *n = fs_find(name);
    DIR *d;
    if (!n || !n->isdir || n->openfail) { errno = ENOENT; return 0; }
    d = (DIR *)malloc(sizeof(*d));
    if (!d) return 0;
    d->dir = n->path;
    d->pos = -2;                 // -2 and -1 emit "." and ".."
    g_open_dirs++;
    if (g_open_dirs > g_max_open_dirs) g_max_open_dirs = g_open_dirs;
    return d;
}

struct dirent *readdir(DIR *d);
struct dirent *readdir(DIR *d) {
    if (!d) return 0;
    if (d->pos == -2) { d->pos = -1; strcpy(d->ent.d_name, ".");  d->ent.d_type = DT_DIR; return &d->ent; }
    if (d->pos == -1) { d->pos = 0;  strcpy(d->ent.d_name, ".."); d->ent.d_type = DT_DIR; return &d->ent; }
    while (d->pos < NFS_NODES) {
        const fnode_t *n = &g_fs[d->pos++];
        if (fs_is_child(d->dir, n->path)) {
            const char *base = n->path + strlen(d->dir) + 1;
            strcpy(d->ent.d_name, base);
            d->ent.d_type = n->isdir ? DT_DIR : DT_REG;
            return &d->ent;
        }
    }
    return 0;
}

int closedir(DIR *d);
int closedir(DIR *d) { if (d) { g_open_dirs--; free(d); } return 0; }

// ===========================================================================
// getopt
// ===========================================================================

static void reset_getopt(void) { optind = 1; opterr = 0; optopt = '?'; optarg = 0; }

static void test_getopt_short(void) {
    printf("--- getopt: short options\n");

    {   // plain short options, then an operand
        char *av[] = { "prog", "-a", "-b", "file", 0 };
        reset_getopt();
        CKINT(getopt(4, av, "ab"), 'a', "short: first -a");
        CKINT(getopt(4, av, "ab"), 'b', "short: then -b");
        CKINT(getopt(4, av, "ab"), -1,  "short: end");
        CKINT(optind, 3, "short: optind at operand");
        CKSTR(av[optind], "file", "short: operand is file");
    }
    {   // clustered
        char *av[] = { "prog", "-abc", 0 };
        reset_getopt();
        CKINT(getopt(2, av, "abc"), 'a', "clustered: a");
        CKINT(getopt(2, av, "abc"), 'b', "clustered: b");
        CKINT(getopt(2, av, "abc"), 'c', "clustered: c");
        CKINT(getopt(2, av, "abc"), -1,  "clustered: end");
        CKINT(optind, 2, "clustered: optind");
    }
    {   // argument glued on
        char *av[] = { "prog", "-ofile", 0 };
        reset_getopt();
        CKINT(getopt(2, av, "o:"), 'o', "glued arg: returns o");
        CKSTR(optarg, "file", "glued arg: optarg");
        CKINT(getopt(2, av, "o:"), -1, "glued arg: end");
        CKINT(optind, 2, "glued arg: optind");
    }
    {   // argument in the next word
        char *av[] = { "prog", "-o", "file", 0 };
        reset_getopt();
        CKINT(getopt(3, av, "o:"), 'o', "separate arg: returns o");
        CKSTR(optarg, "file", "separate arg: optarg");
        CKINT(optind, 3, "separate arg: optind");
        CKINT(getopt(3, av, "o:"), -1, "separate arg: end");
    }
    {   // clustered, last one takes the next word
        char *av[] = { "prog", "-abo", "VAL", 0 };
        reset_getopt();
        CKINT(getopt(3, av, "abo:"), 'a', "cluster+arg: a");
        CKINT(getopt(3, av, "abo:"), 'b', "cluster+arg: b");
        CKINT(getopt(3, av, "abo:"), 'o', "cluster+arg: o");
        CKSTR(optarg, "VAL", "cluster+arg: optarg");
        CKINT(getopt(3, av, "abo:"), -1, "cluster+arg: end");
    }
    {   // unknown option
        char *av[] = { "prog", "-x", 0 };
        reset_getopt();
        CKINT(getopt(2, av, "ab"), '?', "unknown: returns ?");
        CKINT(optopt, 'x', "unknown: optopt is x");
        CKINT(getopt(2, av, "ab"), -1, "unknown: end");
    }
    {   // unknown option inside a cluster: the rest of the cluster still parses
        char *av[] = { "prog", "-axb", 0 };
        reset_getopt();
        CKINT(getopt(2, av, "ab"), 'a', "unknown in cluster: a");
        CKINT(getopt(2, av, "ab"), '?', "unknown in cluster: ?");
        CKINT(optopt, 'x', "unknown in cluster: optopt");
        CKINT(getopt(2, av, "ab"), 'b', "unknown in cluster: b");
        CKINT(getopt(2, av, "ab"), -1, "unknown in cluster: end");
    }
    {   // missing required argument
        char *av[] = { "prog", "-o", 0 };
        reset_getopt();
        CKINT(getopt(2, av, "o:"), '?', "missing arg: returns ?");
        CKINT(optopt, 'o', "missing arg: optopt");
    }
    {   // leading ':' turns a missing argument into ':'
        char *av[] = { "prog", "-o", 0 };
        reset_getopt();
        CKINT(getopt(2, av, ":o:"), ':', "missing arg, colon: returns :");
        CKINT(optopt, 'o', "missing arg, colon: optopt");
    }
    {   // optional argument: glued only
        char *av[] = { "prog", "-a", 0 };
        reset_getopt();
        CKINT(getopt(2, av, "a::"), 'a', "optional arg absent: a");
        CK(optarg == 0, "optional arg absent: optarg NULL");
    }
    {
        char *av[] = { "prog", "-aVAL", 0 };
        reset_getopt();
        CKINT(getopt(2, av, "a::"), 'a', "optional arg glued: a");
        CKSTR(optarg, "VAL", "optional arg glued: optarg");
    }
    {   // an optional argument is NEVER taken from the next word
        char *av[] = { "prog", "-a", "VAL", 0 };
        reset_getopt();
        CKINT(getopt(3, av, "a::"), 'a', "optional arg next word: a");
        CK(optarg == 0, "optional arg next word: optarg NULL");
        CKINT(getopt(3, av, "a::"), -1, "optional arg next word: end");
        CKSTR(av[optind], "VAL", "optional arg next word: VAL is operand");
    }
    {   // "-" on its own is an operand, not an option
        char *av[] = { "prog", "-", "-a", 0 };
        reset_getopt();
        CKINT(getopt(3, av, "a"), 'a', "lone dash: -a still parsed");
        CKINT(getopt(3, av, "a"), -1, "lone dash: end");
        CKSTR(av[optind], "-", "lone dash: - is the operand");
    }
}

static void test_getopt_ddash_and_order(void) {
    printf("--- getopt: \"--\", permutation and ordering\n");

    {   // THE case that silently eats a filename if it is wrong
        char *av[] = { "prog", "-a", "--", "-b", "file", 0 };
        reset_getopt();
        CKINT(getopt(5, av, "ab"), 'a', "ddash: -a");
        CKINT(getopt(5, av, "ab"), -1, "ddash: stops at --");
        // "--" is CONSUMED by leaving optind pointing past it, not by being
        // deleted from argv. Verified against glibc's getopt, which gives
        // optind==3 and an unmodified argv for exactly this command line. The
        // invariant that matters to a caller is the one checked here: nothing
        // from optind onwards is the "--", and both operands survive intact.
        CKINT(optind, 3, "ddash: optind past --");
        CKSTR(av[optind], "-b", "ddash: -b survives as operand");
        CKSTR(av[optind + 1], "file", "ddash: file survives");
        {
            int i, saw = 0;
            for (i = optind; i < 5; i++) if (strcmp(av[i], "--") == 0) saw = 1;
            CK(!saw, "ddash: -- not left among the operands");
        }
    }
    {   // "--" with an operand before it
        char *av[] = { "prog", "file1", "-a", "--", "-b", 0 };
        reset_getopt();
        CKINT(getopt(5, av, "ab"), 'a', "ddash+perm: -a found after operand");
        CKINT(getopt(5, av, "ab"), -1, "ddash+perm: end");
        CKINT(optind, 3, "ddash+perm: optind");
        CKSTR(av[3], "file1", "ddash+perm: file1 first operand");
        CKSTR(av[4], "-b", "ddash+perm: -b second operand");
    }
    {   // permutation: an option after an operand is still found
        char *av[] = { "prog", "file", "-a", "bar", 0 };
        reset_getopt();
        CKINT(getopt(4, av, "a"), 'a', "permute: -a found after operand");
        CKINT(getopt(4, av, "a"), -1, "permute: end");
        CKINT(optind, 2, "permute: optind at first operand");
        CKSTR(av[2], "file", "permute: operands in order (file)");
        CKSTR(av[3], "bar", "permute: operands in order (bar)");
    }
    {   // '+' means POSIX ordering: stop at the first operand
        char *av[] = { "prog", "file", "-a", 0 };
        reset_getopt();
        CKINT(getopt(3, av, "+a"), -1, "plus: stops at first operand");
        CKINT(optind, 1, "plus: optind unmoved");
        CKSTR(av[1], "file", "plus: argv not permuted");
    }
    {   // '-' means operands come back as option 1
        char *av[] = { "prog", "file", "-a", 0 };
        reset_getopt();
        CKINT(getopt(3, av, "-a"), 1, "minus: operand returned as 1");
        CKSTR(optarg, "file", "minus: operand in optarg");
        CKINT(getopt(3, av, "-a"), 'a', "minus: then -a");
        CKINT(getopt(3, av, "-a"), -1, "minus: end");
    }
    {   // an option argument that looks like "--" is still an argument
        char *av[] = { "prog", "-o", "--", 0 };
        reset_getopt();
        CKINT(getopt(3, av, "o:"), 'o', "arg looks like ddash: o");
        CKSTR(optarg, "--", "arg looks like ddash: optarg is --");
        CKINT(getopt(3, av, "o:"), -1, "arg looks like ddash: end");
    }
    {   // no arguments at all
        char *av[] = { "prog", 0 };
        reset_getopt();
        CKINT(getopt(1, av, "ab"), -1, "empty: end");
        CKINT(optind, 1, "empty: optind");
    }
}

static int g_flag_target = 0;

static void test_getopt_long(void) {
    static const struct option lo[] = {
        { "verbose", no_argument,       0,               'v' },
        { "version", no_argument,       0,               'V' },
        { "output",  required_argument, 0,               'o' },
        { "level",   optional_argument, 0,               'l' },
        { "setflag", no_argument,       &g_flag_target,  42  },
        { 0, 0, 0, 0 }
    };
    printf("--- getopt_long\n");

    {
        char *av[] = { "prog", "--verbose", 0 };
        int li = -1;
        reset_getopt();
        CKINT(getopt_long(2, av, "vo:", lo, &li), 'v', "long: --verbose");
        CKINT(li, 0, "long: longindex");
        CKINT(getopt_long(2, av, "vo:", lo, &li), -1, "long: end");
    }
    {   // --opt=value
        char *av[] = { "prog", "--output=FILE", 0 };
        reset_getopt();
        CKINT(getopt_long(2, av, "vo:", lo, 0), 'o', "long: --output=FILE");
        CKSTR(optarg, "FILE", "long: =value optarg");
        CKINT(optind, 2, "long: =value optind");
    }
    {   // --opt value
        char *av[] = { "prog", "--output", "FILE", 0 };
        reset_getopt();
        CKINT(getopt_long(3, av, "vo:", lo, 0), 'o', "long: --output FILE");
        CKSTR(optarg, "FILE", "long: next-word optarg");
        CKINT(optind, 3, "long: next-word optind");
    }
    {   // --opt= with an empty value is an empty string, NOT NULL
        char *av[] = { "prog", "--output=", 0 };
        reset_getopt();
        CKINT(getopt_long(2, av, "vo:", lo, 0), 'o', "long: --output= empty");
        CK(optarg != 0 && optarg[0] == '\0', "long: empty value not NULL");
    }
    {   // unambiguous abbreviation
        char *av[] = { "prog", "--verb", 0 };
        reset_getopt();
        CKINT(getopt_long(2, av, "vo:", lo, 0), 'v', "long: abbreviation");
    }
    {   // ambiguous abbreviation
        char *av[] = { "prog", "--ver", 0 };
        reset_getopt();
        CKINT(getopt_long(2, av, "vo:", lo, 0), '?', "long: ambiguous");
        CKINT(optopt, 0, "long: ambiguous optopt is 0");
    }
    {   // unrecognized long option
        char *av[] = { "prog", "--nope", 0 };
        reset_getopt();
        CKINT(getopt_long(2, av, "vo:", lo, 0), '?', "long: unrecognized");
        CKINT(getopt_long(2, av, "vo:", lo, 0), -1, "long: unrecognized then end");
    }
    {   // required argument missing
        char *av[] = { "prog", "--output", 0 };
        reset_getopt();
        CKINT(getopt_long(2, av, "vo:", lo, 0), '?', "long: missing arg");
    }
    {   // no_argument given one anyway
        char *av[] = { "prog", "--verbose=x", 0 };
        reset_getopt();
        CKINT(getopt_long(2, av, "vo:", lo, 0), '?', "long: unwanted arg");
    }
    {   // flag indirection returns 0 and stores val
        char *av[] = { "prog", "--setflag", 0 };
        int li = -1;
        reset_getopt();
        g_flag_target = 0;
        CKINT(getopt_long(2, av, "vo:", lo, &li), 0, "long: flag returns 0");
        CKINT(g_flag_target, 42, "long: flag stored");
        CKINT(li, 4, "long: flag longindex");
    }
    {   // optional argument on a long option: =value only
        char *av[] = { "prog", "--level=3", "--level", "9", 0 };
        reset_getopt();
        CKINT(getopt_long(4, av, "vo:", lo, 0), 'l', "long: --level=3");
        CKSTR(optarg, "3", "long: optional =value");
        CKINT(getopt_long(4, av, "vo:", lo, 0), 'l', "long: --level bare");
        CK(optarg == 0, "long: optional bare is NULL");
    }
    {   // short and long mixed, plus permutation
        char *av[] = { "prog", "file", "-v", "--output=X", 0 };
        reset_getopt();
        CKINT(getopt_long(4, av, "vo:", lo, 0), 'v', "mixed: -v");
        CKINT(getopt_long(4, av, "vo:", lo, 0), 'o', "mixed: --output=X");
        CKSTR(optarg, "X", "mixed: optarg");
        CKINT(getopt_long(4, av, "vo:", lo, 0), -1, "mixed: end");
        CKSTR(av[optind], "file", "mixed: operand");
    }
    {   // "--" still terminates when long options are in play
        char *av[] = { "prog", "--verbose", "--", "--output=X", 0 };
        reset_getopt();
        CKINT(getopt_long(4, av, "vo:", lo, 0), 'v', "long+ddash: -v");
        CKINT(getopt_long(4, av, "vo:", lo, 0), -1, "long+ddash: end");
        CKSTR(av[optind], "--output=X", "long+ddash: operand untouched");
    }
    {   // getopt_long_only accepts a single dash for a long name
        char *av[] = { "prog", "-verbose", 0 };
        reset_getopt();
        CKINT(getopt_long_only(2, av, "vo:", lo, 0), 'v', "long_only: -verbose");
    }
    {   // ... and a single dash short option still works under long_only
        char *av[] = { "prog", "-v", 0 };
        reset_getopt();
        CKINT(getopt_long_only(2, av, "vo:", lo, 0), 'v', "long_only: -v short");
    }
}

static void test_getopt_diag(void) {
    char *av[] = { "prog", "-x", 0 };
    printf("--- getopt: opterr diagnostic (ONE line on stderr below is EXPECTED)\n");
    optind = 1; opterr = 1; optarg = 0;
    CKINT(getopt(2, av, "ab"), '?', "diag: still returns ?");
    // Silence must also work.
    optind = 1; opterr = 0;
    CKINT(getopt(2, av, "ab"), '?', "diag: silent mode returns ?");
}

// ===========================================================================
// libgen
// ===========================================================================

static void bn(const char *in, const char *want, const char *msg) {
    char buf[64];
    strcpy(buf, in);
    CKSTR(basename(buf), want, msg);
}
static void dn(const char *in, const char *want, const char *msg) {
    char buf[64];
    strcpy(buf, in);
    CKSTR(dirname(buf), want, msg);
}

static void test_libgen(void) {
    printf("--- libgen\n");
    bn("/usr/lib",  "lib", "basename /usr/lib");
    bn("/usr/",     "usr", "basename /usr/");
    bn("usr",       "usr", "basename usr");
    bn("/",         "/",   "basename /");
    bn("///",       "/",   "basename ///");
    bn("",          ".",   "basename empty");
    bn("a/b/c",     "c",   "basename a/b/c");
    bn("a/b/c///",  "c",   "basename a/b/c///");
    bn(".",         ".",   "basename .");
    bn("..",        "..",  "basename ..");
    CKSTR(basename(0), ".", "basename NULL");

    dn("/usr/lib",  "/usr", "dirname /usr/lib");
    dn("/usr/",     "/",    "dirname /usr/");
    dn("usr",       ".",    "dirname usr");
    dn("/",         "/",    "dirname /");
    dn("///",       "/",    "dirname ///");
    dn("",          ".",    "dirname empty");
    dn("a/b/c",     "a/b",  "dirname a/b/c");
    dn("a/b/",      "a",    "dirname a/b/");
    dn(".",         ".",    "dirname .");
    dn("..",        ".",    "dirname ..");
    dn("/usr",      "/",    "dirname /usr");
    dn("//foo//bar//", "//foo", "dirname //foo//bar//");
    CKSTR(dirname(0), ".", "dirname NULL");
}

// ===========================================================================
// sys/uio
// ===========================================================================

static void test_uio(void) {
    int fds[2];
    char b0[8], b1[8], b2[8];
    struct iovec iov[3];
    char got[16];
    ssize_t n;

    printf("--- sys/uio\n");

    CKINT(pipe(fds), 0, "uio: pipe created");

    iov[0].iov_base = (void *)"abc"; iov[0].iov_len = 3;
    iov[1].iov_base = (void *)"";    iov[1].iov_len = 0;   // must be skipped
    iov[2].iov_base = (void *)"de";  iov[2].iov_len = 2;
    n = writev(fds[1], iov, 3);
    CKINT(n, 5, "uio: writev total");
    memset(got, 0, sizeof(got));
    CKINT(read(fds[0], got, 5), 5, "uio: bytes readable");
    CKSTR(got, "abcde", "uio: writev content and order");

    CKINT(write(fds[1], "0123456789", 10), 10, "uio: seeded pipe");
    memset(b0, 0, sizeof(b0)); memset(b1, 0, sizeof(b1)); memset(b2, 0, sizeof(b2));
    iov[0].iov_base = b0; iov[0].iov_len = 3;
    iov[1].iov_base = b1; iov[1].iov_len = 3;
    iov[2].iov_base = b2; iov[2].iov_len = 4;
    n = readv(fds[0], iov, 3);
    CKINT(n, 10, "uio: readv total");
    CKSTR(b0, "012",  "uio: readv iov 0");
    CKSTR(b1, "345",  "uio: readv iov 1");
    CKSTR(b2, "6789", "uio: readv iov 2");

    CKINT(writev(fds[1], iov, 0), 0, "uio: iovcnt 0 writes nothing");
    CKINT(readv(fds[0], iov, 0),  0, "uio: iovcnt 0 reads nothing");

    errno = 0;
    CKINT(writev(fds[1], iov, -1), -1, "uio: negative iovcnt refused");
    CKINT(errno, EINVAL, "uio: negative iovcnt EINVAL");
    errno = 0;
    CKINT(writev(fds[1], iov, IOV_MAX + 1), -1, "uio: iovcnt over IOV_MAX");
    CKINT(errno, EINVAL, "uio: over IOV_MAX EINVAL");

    {   // a length sum that cannot fit in ssize_t must be refused BEFORE any
        // byte is written, or the caller cannot tell what happened.
        struct iovec big[2];
        char one;
        big[0].iov_base = &one; big[0].iov_len = (size_t)0x7fffffffffffffffL;
        big[1].iov_base = &one; big[1].iov_len = 2;
        errno = 0;
        CKINT(writev(fds[1], big, 2), -1, "uio: overflowing sum refused");
        CKINT(errno, EINVAL, "uio: overflowing sum EINVAL");
    }

    close(fds[1]);
    memset(b0, 0, sizeof(b0));
    iov[0].iov_base = b0; iov[0].iov_len = 3;
    CKINT(readv(fds[0], iov, 1), 0, "uio: readv at EOF returns 0");
    close(fds[0]);
}

// ===========================================================================
// sys/utsname
// ===========================================================================

static void test_uname(void) {
    struct utsname u;
    printf("--- sys/utsname\n");

    g_version_fail = 0;
    g_version_str = "1.9.0 (build 1234)";
    CKINT(uname(&u), 0, "uname: succeeds");
    CKSTR(u.sysname,  "MayteraOS", "uname: sysname");
    CKSTR(u.release,  "1.9.0",     "uname: release from kernel string");
    CKSTR(u.version,  "build 1234","uname: version from kernel string");
    CKSTR(u.machine,  "x86_64",    "uname: machine");
    CKSTR(u.nodename, "",          "uname: nodename empty, not invented");

    // A differently shaped version string must not produce a fabricated split.
    g_version_str = "2.0.0-rc1";
    CKINT(uname(&u), 0, "uname: odd shape still succeeds");
    CKSTR(u.release, "2.0.0-rc1", "uname: odd shape release");
    CKSTR(u.version, "2.0.0-rc1", "uname: odd shape version falls back whole");

    // If the kernel will not say, uname FAILS rather than guessing.
    g_version_fail = 1;
    errno = 0;
    CKINT(uname(&u), -1, "uname: fails when kernel will not answer");
    CKINT(errno, EIO, "uname: EIO on failure");
    g_version_fail = 0;

    errno = 0;
    CKINT(uname(0), -1, "uname: NULL refused");
    CKINT(errno, EFAULT, "uname: NULL is EFAULT");
}

// ===========================================================================
// sys/file, utime - the two that must never claim success
// ===========================================================================

static void test_refusals(void) {
    struct utimbuf ub;
    struct timeval tv[2];
    printf("--- sys/file + utime (must always fail)\n");

    errno = 0;
    CKINT(flock(0, LOCK_EX), -1, "flock: refuses");
    CKINT(errno, ENOSYS, "flock: ENOSYS");
    errno = 0;
    CKINT(flock(0, LOCK_UN), -1, "flock: unlock refuses too");
    CKINT(errno, ENOSYS, "flock: unlock ENOSYS");

    ub.actime = 1; ub.modtime = 2;
    errno = 0;
    CKINT(utime("/r/f3", &ub), -1, "utime: refuses");
    CKINT(errno, ENOSYS, "utime: ENOSYS");
    errno = 0;
    CKINT(utime("/r/f3", 0), -1, "utime: NULL times refuses");
    CKINT(errno, ENOSYS, "utime: NULL times ENOSYS");

    tv[0].tv_sec = 1; tv[0].tv_usec = 0;
    tv[1].tv_sec = 2; tv[1].tv_usec = 0;
    errno = 0;
    CKINT(utimes("/r/f3", tv), -1, "utimes: refuses");
    CKINT(errno, ENOSYS, "utimes: ENOSYS");
}

// ===========================================================================
// ftw / nftw
// ===========================================================================

#define MAX_VISITS 32
static char  v_path[MAX_VISITS][64];
static int   v_type[MAX_VISITS];
static int   v_base[MAX_VISITS];
static int   v_level[MAX_VISITS];
static int   v_n = 0;
static int   v_stop_at = -1;

static int cb3(const char *p, const struct stat *st, int t) {
    (void)st;
    if (v_n < MAX_VISITS) {
        strcpy(v_path[v_n], p); v_type[v_n] = t; v_base[v_n] = -1; v_level[v_n] = -1;
    }
    v_n++;
    if (v_stop_at >= 0 && v_n - 1 == v_stop_at) return 77;
    return 0;
}

static int cb4(const char *p, const struct stat *st, int t, struct FTW *f) {
    (void)st;
    if (v_n < MAX_VISITS) {
        strcpy(v_path[v_n], p); v_type[v_n] = t;
        v_base[v_n] = f->base; v_level[v_n] = f->level;
    }
    v_n++;
    if (v_stop_at >= 0 && v_n - 1 == v_stop_at) return 77;
    return 0;
}

static void expect_visit(int i, const char *path, int type, int base, int level,
                         const char *msg) {
    if (i >= v_n) { checks++; printf("FAIL  %-34s  visit %d missing\n", msg, i); fails++; return; }
    CKSTR(v_path[i], path, msg);
    CKINT(v_type[i], type, msg);
    if (base >= 0)  CKINT(v_base[i], base, msg);
    if (level >= 0) CKINT(v_level[i], level, msg);
}

static void test_ftw(void) {
    printf("--- ftw / nftw\n");

    // Pre-order, with the level and base the callback is promised.
    v_n = 0; v_stop_at = -1; g_max_open_dirs = 0;
    CKINT(nftw("/r", cb4, 8, 0), 0, "nftw: walk completes");
    CKINT(v_n, 7, "nftw: visit count");
    expect_visit(0, "/r",        FTW_D,   1, 0, "nftw pre 0 /r");
    expect_visit(1, "/r/a",      FTW_D,   3, 1, "nftw pre 1 /r/a");
    expect_visit(2, "/r/a/f1",   FTW_F,   5, 2, "nftw pre 2 /r/a/f1");
    expect_visit(3, "/r/a/b",    FTW_D,   5, 2, "nftw pre 3 /r/a/b");
    expect_visit(4, "/r/a/b/f2", FTW_F,   7, 3, "nftw pre 4 /r/a/b/f2");
    expect_visit(5, "/r/dnr",    FTW_DNR, 3, 1, "nftw pre 5 /r/dnr");
    expect_visit(6, "/r/f3",     FTW_F,   3, 1, "nftw pre 6 /r/f3");
    CKINT(g_max_open_dirs, 1, "nftw: only one directory open at a time");

    // "." and ".." are emitted by the stand-in readdir and must never be
    // visited: if they were, the walk would recurse for ever.
    {
        int i, saw_dot = 0;
        for (i = 0; i < v_n && i < MAX_VISITS; i++) {
            const char *b = strrchr(v_path[i], '/');
            b = b ? b + 1 : v_path[i];
            if (strcmp(b, ".") == 0 || strcmp(b, "..") == 0) saw_dot = 1;
        }
        CK(!saw_dot, "nftw: . and .. skipped");
    }

    // Post-order.
    v_n = 0; v_stop_at = -1;
    CKINT(nftw("/r", cb4, 8, FTW_DEPTH), 0, "nftw depth: completes");
    CKINT(v_n, 7, "nftw depth: visit count");
    expect_visit(0, "/r/a/f1",   FTW_F,   -1, -1, "nftw post 0");
    expect_visit(1, "/r/a/b/f2", FTW_F,   -1, -1, "nftw post 1");
    expect_visit(2, "/r/a/b",    FTW_DP,  -1, -1, "nftw post 2 dir after contents");
    expect_visit(3, "/r/a",      FTW_DP,  -1, -1, "nftw post 3");
    expect_visit(4, "/r/dnr",    FTW_DNR, -1, -1, "nftw post 4");
    expect_visit(5, "/r/f3",     FTW_F,   -1, -1, "nftw post 5");
    expect_visit(6, "/r",        FTW_DP,  -1, -1, "nftw post 6 root last");

    // FTW_PHYS is accepted (there are no symlinks for it to change anything).
    v_n = 0;
    CKINT(nftw("/r", cb4, 8, FTW_PHYS), 0, "nftw: FTW_PHYS accepted");
    CKINT(v_n, 7, "nftw: FTW_PHYS same walk");

    // The three-argument ftw().
    v_n = 0; v_stop_at = -1;
    CKINT(ftw("/r", cb3, 8), 0, "ftw: walk completes");
    CKINT(v_n, 7, "ftw: visit count");
    expect_visit(0, "/r", FTW_D, -1, -1, "ftw: root first");

    // Walking a plain file visits exactly it.
    v_n = 0;
    CKINT(ftw("/r/f3", cb3, 8), 0, "ftw: file root completes");
    CKINT(v_n, 1, "ftw: file root one visit");
    expect_visit(0, "/r/f3", FTW_F, -1, -1, "ftw: file root type");

    // A non-zero callback stops the walk at once and its value is returned.
    v_n = 0; v_stop_at = 2;
    CKINT(nftw("/r", cb4, 8, 0), 77, "nftw: callback value returned");
    CKINT(v_n, 3, "nftw: walk stopped immediately");
    v_stop_at = -1;

    // Refusals, which must be errors and not silent no-ops.
    errno = 0;
    CKINT(nftw("/r", cb4, 8, FTW_CHDIR), -1, "nftw: FTW_CHDIR refused");
    CKINT(errno, EINVAL, "nftw: FTW_CHDIR EINVAL");
    errno = 0;
    CKINT(nftw("/r", cb4, 8, FTW_MOUNT), -1, "nftw: FTW_MOUNT refused");
    CKINT(errno, EINVAL, "nftw: FTW_MOUNT EINVAL");
    errno = 0;
    CKINT(nftw("/r", cb4, 0, 0), -1, "nftw: nopenfd 0 refused");
    CKINT(errno, EINVAL, "nftw: nopenfd 0 EINVAL");
    errno = 0;
    CKINT(nftw("/nosuch", cb4, 8, 0), -1, "nftw: missing root refused");
    CKINT(errno, ENOENT, "nftw: missing root ENOENT");
    errno = 0;
    CKINT(nftw(0, cb4, 8, 0), -1, "nftw: NULL path refused");
    CKINT(nftw("/r", 0, 8, 0), -1, "nftw: NULL callback refused");
}

// ===========================================================================
// endian / byteswap / sys/param / alloca
// ===========================================================================

static void test_endian_param(void) {
    volatile uint32_t x32 = 0x01020304u;
    volatile uint16_t x16 = 0x0102u;
    volatile uint64_t x64 = 0x0102030405060708uL;
    char *a;

    printf("--- endian / byteswap / sys/param / alloca\n");

    CK(bswap_16(x16) == 0x0201u, "bswap_16");
    CK(bswap_32(x32) == 0x04030201u, "bswap_32");
    CK(bswap_64(x64) == 0x0807060504030201uL, "bswap_64");

    CK(htobe16(x16) == 0x0201u, "htobe16");
    CK(htole16(x16) == 0x0102u, "htole16 identity");
    CK(be16toh((uint16_t)0x0201u) == 0x0102u, "be16toh");
    CK(le16toh(x16) == 0x0102u, "le16toh identity");

    CK(htobe32(x32) == 0x04030201u, "htobe32");
    CK(htole32(x32) == 0x01020304u, "htole32 identity");
    CK(be32toh(htobe32(x32)) == x32, "be32toh round trip");

    CK(htobe64(x64) == 0x0807060504030201uL, "htobe64");
    CK(htole64(x64) == 0x0102030405060708uL, "htole64 identity");
    CK(be64toh(htobe64(x64)) == x64, "be64toh round trip");

    CK(BYTE_ORDER == LITTLE_ENDIAN, "BYTE_ORDER is little endian");

    CKINT(MIN(3, 7), 3, "MIN");
    CKINT(MAX(3, 7), 7, "MAX");
    CKINT(howmany(10, 4), 3, "howmany");
    CKINT(roundup(10, 4), 12, "roundup");
    CKINT(rounddown(10, 4), 8, "rounddown");
    CK(powerof2(64) && !powerof2(63), "powerof2");
    CKINT(PATH_MAX, 1024, "PATH_MAX matches kernel SC_PATH_MAX");
    CKINT(NAME_MAX, 255, "NAME_MAX matches kernel dirent");
    CKINT(NBBY, 8, "NBBY");

    a = (char *)alloca(32);
    CK(a != 0, "alloca returns memory");
    memset(a, 0x5a, 32);
    CK(a[0] == 0x5a && a[31] == 0x5a, "alloca memory is writable");
}

int main(void) {
    printf("=== #745 (local 72) tier-1 POSIX header battery ===\n");
    test_getopt_short();
    test_getopt_ddash_and_order();
    test_getopt_long();
    test_getopt_diag();
    test_libgen();
    test_uio();
    test_uname();
    test_refusals();
    test_ftw();
    test_endian_param();
    printf("\n%d checks, %d failures\n", checks, fails);
    printf(fails == 0 ? "posixhdr_test: PASS\n" : "posixhdr_test: FAIL\n");
    return fails == 0 ? 0 : 1;
}
