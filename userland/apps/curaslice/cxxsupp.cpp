// cxxsupp.cpp - minimal C++ runtime for freestanding MayteraOS userland.
//
// The app is built -fno-exceptions -fno-rtti -fno-threadsafe-statics and linked
// -nostdlib against libc.a, so NONE of libstdc++'s runtime is present. This one
// object supplies the small set of symbols the compiler and the header-only STL
// still reference: operator new/delete onto the libc heap, the C++ ABI helpers
// (__cxa_*, __dso_handle), and abort-style stubs for std::terminate and the
// std::__throw_* family that <vector>/<map> emit calls to even with exceptions
// disabled.
//
// This list is a STARTING point. Per CURAENGINE_PORT_PLAN.md section 3.3 the
// authoritative set is "whatever the first link reports as undefined"; see
// BUILD-NOTES.md for the iterate-on-undefined-symbols protocol. Add any newly
// reported symbol here rather than pulling in libstdc++.
//
// No em-dashes per repo writing-style rule.

#include <stddef.h>
#include <new>
// Host headers only (see platform.h header-strategy note); the link resolves
// these against the MayteraOS libc.a.
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Death helper. Everything unrecoverable routes here: print then exit(1).
// ---------------------------------------------------------------------------
extern "C" void curaslice_die(const char* msg) __attribute__((noreturn));
extern "C" void curaslice_die(const char* msg)
{
    if (msg)
        printf("curaslice: fatal: %s\n", msg);
    exit(1);       // MayteraOS libc exit (host header prototype, libc.a symbol)
    for (;;) { }   // exit does not return; keep the noreturn contract explicit
}

// ---------------------------------------------------------------------------
// operator new / delete onto malloc / free. Allocation failure aborts (the app
// is -fno-exceptions, so we cannot throw std::bad_alloc).
// ---------------------------------------------------------------------------
void* operator new(size_t n)
{
    if (n == 0) n = 1;
    void* p = malloc(n);
    if (!p) curaslice_die("operator new: out of memory");
    return p;
}
void* operator new[](size_t n)              { return operator new(n); }

void  operator delete(void* p)              { if (p) free(p); }
void  operator delete[](void* p)            { if (p) free(p); }

// C++14 sized-deallocation forms (gcc-12 emits these).
void  operator delete(void* p, size_t)      { if (p) free(p); }
void  operator delete[](void* p, size_t)    { if (p) free(p); }

// nothrow forms.
void* operator new(size_t n, const std::nothrow_t&) throw()   { if (n == 0) n = 1; return malloc(n); }
void* operator new[](size_t n, const std::nothrow_t&) throw() { if (n == 0) n = 1; return malloc(n); }
void  operator delete(void* p, const std::nothrow_t&) throw()   { if (p) free(p); }
void  operator delete[](void* p, const std::nothrow_t&) throw() { if (p) free(p); }

// ---------------------------------------------------------------------------
// C++ ABI helpers.
// ---------------------------------------------------------------------------
extern "C" {

// A call through a pure-virtual slot: a real bug, so die.
void __cxa_pure_virtual() { curaslice_die("pure virtual function called"); }

// Static-object destructor registration. We never run global dtors on exit
// (the process just ends), so record nothing and report success.
int __cxa_atexit(void (*/*fn*/)(void*), void* /*arg*/, void* /*dso*/) { return 0; }

// Handle for this "shared object"; the ABI wants the address, never the value.
void* __dso_handle = 0;

} // extern "C"

// ---------------------------------------------------------------------------
// std::terminate and the verbose terminate handler.
// ---------------------------------------------------------------------------
namespace std {
void terminate() { curaslice_die("std::terminate"); }
} // namespace std

namespace __gnu_cxx {
void __verbose_terminate_handler() { curaslice_die("terminate handler"); }
} // namespace __gnu_cxx

// ---------------------------------------------------------------------------
// std::__throw_* family. libstdc++ headers (vector/map/etc.) call these from
// bounds checks and internal error paths; with -fno-exceptions we cannot throw,
// so each prints a message and exits. Signatures match <bits/functexcept.h>.
// ---------------------------------------------------------------------------
namespace std {
void __throw_length_error(const char* s)        { curaslice_die(s ? s : "length_error"); }
void __throw_out_of_range(const char* s)         { curaslice_die(s ? s : "out_of_range"); }
void __throw_out_of_range_fmt(const char* s, ...) { curaslice_die(s ? s : "out_of_range"); }
void __throw_bad_alloc()                          { curaslice_die("bad_alloc"); }
void __throw_logic_error(const char* s)          { curaslice_die(s ? s : "logic_error"); }
void __throw_bad_function_call()                  { curaslice_die("bad_function_call"); }
void __throw_invalid_argument(const char* s)     { curaslice_die(s ? s : "invalid_argument"); }
void __throw_runtime_error(const char* s)        { curaslice_die(s ? s : "runtime_error"); }
void __throw_bad_cast()                           { curaslice_die("bad_cast"); }
} // namespace std

// ---------------------------------------------------------------------------
// POSIX stubs referenced by main.cpp's Linux block (setpriority). The header
// (<sys/resource.h>) is pulled from the host toolchain; only the symbol is
// missing, so define it as a no-op priority set.
// ---------------------------------------------------------------------------
extern "C" int setpriority(int /*which*/, int /*who*/, int /*prio*/) { return 0; }

// ---------------------------------------------------------------------------
// MayteraOS port: link-iteration stubs (see BUILD-NOTES.md). Added as the
// -nostdlib link reported them undefined.
// ---------------------------------------------------------------------------
#include <cstdarg>

// glibc redirects sscanf()/vsscanf() to the C99-conformant __isoc99_* symbols;
// the MayteraOS libc exports the plain names. Bind the plain libc vsscanf via an
// asm label (bypassing the host header redirect) and forward both isoc99 entry
// points to it.
extern "C" int mos_vsscanf(const char*, const char*, va_list) __asm__("vsscanf");
extern "C" int __isoc99_vsscanf(const char* s, const char* fmt, va_list ap)
{
    return mos_vsscanf(s, fmt, ap);
}
extern "C" int __isoc99_sscanf(const char* s, const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = mos_vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

// fgetpos/fsetpos are referenced by libstdc++ headers but never exercised by
// the slicer; the MayteraOS libc has no fpos_t stream positioning, so stub
// them with the host glibc signatures (fpos_t from <stdio.h>).
extern "C" int fgetpos(FILE*, fpos_t*) { return -1; }
extern "C" int fsetpos(FILE*, const fpos_t*) { return -1; }

// C++11 operator new[] overflow check helper (emitted by new_allocator.h).
namespace std { void __throw_bad_array_new_length() { curaslice_die("bad array new length"); } }

// ---------------------------------------------------------------------------
// MayteraOS libc gap-fillers. The build container's libc.a predates several
// routines that were later added to the libc SOURCE tree, so the -nostdlib link
// reports them undefined even though the sources exist. Per the BUILD-NOTES.md
// "iterate on undefined symbols" protocol we supply them HERE (app-local) rather
// than rebuild the shared libc.a. Each is self-contained: it introduces NO new
// external reference (the stale libc.a is also missing strtoll/strtod/round/
// isupper), so porting the real, dependency-heavy libc versions is not an
// option. These are behaviourally faithful for the ranges the slicer uses.
// ---------------------------------------------------------------------------
#include <sys/time.h>   // struct timeval for gettimeofday

extern "C" {

// tolower: ASCII only (matches libc ctype.c: isupper ? c+32 : c).
int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }

// roundf: round-half-away-from-zero, self-contained (no libc round() dep).
// Values beyond 2^53 are already integral; guard the long long cast there.
float roundf(float x)
{
    if (x != x) return x;                                   // NaN passthrough
    if (x >= 9007199254740992.0f || x <= -9007199254740992.0f) return x;
    if (x >= 0.0f) return (float)(long long)(x + 0.5f);
    return (float)(long long)(x - 0.5f);
}

// gettimeofday: the slicer only uses this for elapsed-time log lines, never for
// slice geometry, so a zeroed clock is functionally harmless. Signature matches
// glibc (second arg void*). Returns 0 (success).
int gettimeofday(struct timeval *tv, void * /*tz*/)
{
    if (tv) { tv->tv_sec = 0; tv->tv_usec = 0; }
    return 0;
}

// qsort: self-contained median-of-three quicksort with an insertion-sort cutoff
// and byte-wise swap (no memcpy dependency). Matches the C89 qsort() contract
// used by infill line ordering and support-grid sorting.
static void cs_qs_swap(char *a, char *b, size_t size)
{
    while (size--) { char t = *a; *a++ = *b; *b++ = t; }
}
static void cs_qs_run(char *base, size_t n, size_t size,
                      int (*cmp)(const void *, const void *))
{
    while (n > 12) {
        char *lo = base;
        char *mid = base + (n / 2) * size;
        char *hi = base + (n - 1) * size;
        if (cmp(mid, lo) < 0) cs_qs_swap(mid, lo, size);
        if (cmp(hi, lo) < 0) cs_qs_swap(hi, lo, size);
        if (cmp(hi, mid) < 0) cs_qs_swap(hi, mid, size);
        cs_qs_swap(lo, mid, size);          // pivot -> base[0]
        size_t i = 0, j = n;
        for (;;) {
            do { i++; } while (i < n && cmp(base + i * size, lo) < 0);
            do { j--; } while (j > 0 && cmp(base + j * size, lo) > 0);
            if (i >= j) break;
            cs_qs_swap(base + i * size, base + j * size, size);
        }
        cs_qs_swap(lo, base + j * size, size);  // pivot to final slot j
        // Recurse into the smaller side, iterate on the larger (bounded stack).
        size_t left_n = j;
        size_t right_n = n - j - 1;
        if (left_n < right_n) {
            cs_qs_run(base, left_n, size, cmp);
            base = base + (j + 1) * size;
            n = right_n;
        } else {
            cs_qs_run(base + (j + 1) * size, right_n, size, cmp);
            n = left_n;
        }
    }
    // Insertion sort for the small tail.
    for (size_t a = 1; a < n; a++)
        for (size_t b = a; b > 0 &&
             cmp(base + b * size, base + (b - 1) * size) < 0; b--)
            cs_qs_swap(base + b * size, base + (b - 1) * size, size);
}
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    if (nmemb > 1 && size > 0) cs_qs_run((char *)base, nmemb, size, compar);
}

} // extern "C"

// vsscanf: DEFINE the plain libc "vsscanf" symbol by giving mos_vsscanf (declared
// with __asm__("vsscanf") above) its body. Defining a function literally named
// vsscanf is impossible: host <stdio.h> asm-redirects that name to
// __isoc99_vsscanf, which the forwarders above already provide, so it would
// double-define. The stale libc.a lacks vsscanf AND its strtoll/strtoull/strtod
// helpers, so a full port would only add MORE undefined symbols. The slicer's
// ONLY sscanf user is main.cpp's optional "-m 3x3 matrix" argument, never passed
// by the self-test preset or the 3D Print app. We therefore provide a compact,
// dependency-free scanner covering the specifiers that path uses (whitespace,
// %d/%u/%x integer, %f/%e/%g/%lf float, %c, %s, with optional width and '*'
// suppression). Returns the count of successful assignments.
extern "C" int mos_vsscanf(const char *str, const char *fmt, va_list ap)
{
    const char *s = str;
    int assigned = 0;
    while (*fmt) {
        unsigned char fc = (unsigned char)*fmt;
        if (fc == ' ' || (fc >= '\t' && fc <= '\r')) {
            while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
            fmt++;
            continue;
        }
        if (fc != '%') {
            if (*s != *fmt) return assigned;
            s++; fmt++;
            continue;
        }
        fmt++;                                   // consume '%'
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }
        int width = 0, have_width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); have_width = 1; }
        int is_long = 0;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'L' || *fmt == 'z' ||
               *fmt == 'j' || *fmt == 't') { if (*fmt == 'l' || *fmt == 'L') is_long = 1; fmt++; }
        char conv = *fmt++;
        if (conv == '%') { if (*s == '%') { s++; continue; } return assigned; }
        if (conv != 'c') while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;
        if (*s == '\0') return assigned ? assigned : -1;

        if (conv == 'd' || conv == 'u' || conv == 'i' || conv == 'x' || conv == 'X' || conv == 'o') {
            int base = (conv == 'x' || conv == 'X') ? 16 : (conv == 'o') ? 8 : 10;
            int neg = 0; const char *p = s;
            if (*p == '+' || *p == '-') { neg = (*p == '-'); p++; }
            long long v = 0; int digits = 0;
            while (*p && (!have_width || (p - s) < width)) {
                int d; unsigned char c = (unsigned char)*p;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                if (d >= base) break;
                v = v * base + d; digits++; p++;
            }
            if (!digits) return assigned;
            if (neg) v = -v;
            if (!suppress) {
                if (is_long) *__builtin_va_arg(ap, long *) = (long)v;
                else *__builtin_va_arg(ap, int *) = (int)v;
                assigned++;
            }
            s = p;
        } else if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') {
            const char *p = s; double sign = 1.0;
            if (*p == '+' || *p == '-') { if (*p == '-') sign = -1.0; p++; }
            double val = 0.0; int digits = 0;
            while (*p >= '0' && *p <= '9') { val = val * 10.0 + (*p - '0'); p++; digits++; }
            if (*p == '.') {
                p++; double f = 0.1;
                while (*p >= '0' && *p <= '9') { val += (*p - '0') * f; f *= 0.1; p++; digits++; }
            }
            if (!digits) return assigned;
            if (*p == 'e' || *p == 'E') {
                const char *ep = p + 1; int es = 1, ev = 0, ed = 0;
                if (*ep == '+' || *ep == '-') { if (*ep == '-') es = -1; ep++; }
                while (*ep >= '0' && *ep <= '9') { ev = ev * 10 + (*ep - '0'); ep++; ed++; }
                if (ed) { double m = 1.0; for (int k = 0; k < ev; k++) m *= 10.0;
                          if (es < 0) val /= m; else val *= m; p = ep; }
            }
            val *= sign;
            if (!suppress) {
                if (is_long) *__builtin_va_arg(ap, double *) = val;
                else *__builtin_va_arg(ap, float *) = (float)val;
                assigned++;
            }
            s = p;
        } else if (conv == 'c') {
            int cnt = have_width ? width : 1;
            char *out = suppress ? 0 : __builtin_va_arg(ap, char *);
            int i = 0;
            for (; i < cnt && *s; i++) { if (out) out[i] = *s; s++; }
            if (i == 0) return assigned;
            if (!suppress) assigned++;
        } else if (conv == 's') {
            char *out = suppress ? 0 : __builtin_va_arg(ap, char *);
            int n = 0;
            while (*s && !(*s == ' ' || (*s >= '\t' && *s <= '\r')) &&
                   (!have_width || n < width)) { if (out) out[n] = *s; s++; n++; }
            if (!n) return assigned;
            if (out) out[n] = '\0';
            if (!suppress) assigned++;
        } else {
            return assigned;                     // unsupported specifier
        }
    }
    return assigned;
}
