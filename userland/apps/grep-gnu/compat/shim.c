/* shim.c - MayteraOS compatibility glue for the GNU grep 2.5.4 port.
 *
 * Provides the handful of libc facilities grep needs that the MayteraOS
 * freestanding libc lacks, plus an exit() override that flushes stdio.
 *
 * NOTE on exit(): MayteraOS exit() calls sys_exit directly and crt0 calls
 * _exit() after main, so atexit handlers never run and the line-buffered
 * stdout is not guaranteed to flush.  Because objects listed on the link line
 * override archive (libc.a) members, defining exit()/atexit() here makes them
 * take precedence, giving grep correct flush-on-exit semantics.  This is fully
 * contained in the grep binary and does not change libc.a.
 */
#include <stddef.h>
#include <stdio.h>

extern void _exit(int status) __attribute__((noreturn));

/* ---- atexit / exit ---- */
#define MAX_ATEXIT 32
static void (*atexit_fns[MAX_ATEXIT])(void);
static int atexit_n = 0;

int atexit(void (*fn)(void))
{
    if (atexit_n >= MAX_ATEXIT)
        return -1;
    atexit_fns[atexit_n++] = fn;
    return 0;
}

void exit(int status) __attribute__((noreturn));
void exit(int status)
{
    int i;
    for (i = atexit_n - 1; i >= 0; i--)
        atexit_fns[i]();
    fflush(stdout);
    fflush(stderr);
    _exit(status);
}

/* close_stdout: grep registers this via atexit (declared in closeout.h).
 * We do not compile gnulib closeout.c; a flush is sufficient here. */
void close_stdout(void)
{
    fflush(stdout);
    fflush(stderr);
}

/* grep calls close_stdout_set_status() to record the exit status closeout.c
 * would use on a write error.  We flush directly in close_stdout(), so this is
 * a no-op placeholder to satisfy the reference. */
void close_stdout_set_status(int status)
{
    (void)status;
}

/* ---- stdio helpers missing from the MayteraOS libc ----
 * libc provides fputc()/fgetc() but not the putc()/getc() spellings that
 * error.c and exclude.c use. */
int putc(int c, FILE *f)
{
    return fputc(c, f);
}

int getc(FILE *f)
{
    return fgetc(f);
}

/* ---- stpcpy: copy string, return pointer to terminating NUL (savedir.c) ---- */
char *stpcpy(char *dst, const char *src)
{
    while ((*dst = *src) != '\0') {
        dst++;
        src++;
    }
    return dst;
}

/* ---- getpagesize ---- */
int getpagesize(void)
{
    return 4096;
}

/* ---- locale stubs (C locale only; HAVE_SETLOCALE is not defined so grep
 * does not call these, but other units may reference them) ---- */
struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
};

char *setlocale(int category, const char *locale)
{
    (void)category;
    (void)locale;
    return (char *)"C";
}

struct lconv *localeconv(void)
{
    static struct lconv lc = { (char *)".", (char *)"", (char *)"" };
    return &lc;
}

/* ---- qsort (simple insertion-sort fallback; grep's data sets are tiny) ---- */
static void mem_swap(char *a, char *b, size_t n)
{
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*cmp)(const void *, const void *))
{
    char *arr = (char *)base;
    size_t i, j;
    for (i = 1; i < nmemb; i++) {
        for (j = i; j > 0 && cmp(arr + (j - 1) * size, arr + j * size) > 0; j--)
            mem_swap(arr + (j - 1) * size, arr + j * size, size);
    }
}

/* ---- bsearch ---- */
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *))
{
    const char *arr = (const char *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = cmp(key, arr + mid * size);
        if (r == 0)
            return (void *)(arr + mid * size);
        else if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}
