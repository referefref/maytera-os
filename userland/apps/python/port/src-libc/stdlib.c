// stdlib.c - Standard library implementation
#include "stdlib.h"
#include "syscall.h"
#include "string.h"
#include "errno.h"
#define errno_set(e) (errno = (e))

// ============================================================================
// Memory Allocation (simple bump allocator with free list)
// ============================================================================

#define HEAP_START      0x90000000ULL           // User heap start (below 4GB for compat)
#define HEAP_MAX_SIZE   (256 * 1024 * 1024)     // 256 MB max heap

typedef struct block_header {
    size_t size;                    // Size of data area
    int free;                       // 1 if free, 0 if allocated
    struct block_header *next;      // Next block in list
} block_header_t;

// Physical header size used for pointer arithmetic. Padded up to 16 so that,
// since HEAP_START is 16-aligned and every allocation size is rounded to a
// multiple of 16, EVERY pointer malloc() returns is 16-byte aligned. glibc and
// mlibc both guarantee max_align_t (16 on x86-64); CPython, SSE and doubles
// rely on it. (Previously data started at sizeof()==24 -> only 8-aligned.)
#define BLK_HDR 32

static block_header_t *heap_start = NULL;
static uint64_t heap_end = HEAP_START;
// First byte NOT yet backed by a mapped RW page. Always page-aligned.
// The allocator only ever maps fresh pages beyond this watermark, so it never
// re-maps (and thus never zeroes/leaks) a page that already holds live data,
// and it always maps enough pages to fully cover an allocation that straddles
// a page boundary from an unaligned heap_end.
static uint64_t heap_mapped = HEAP_START;

// Ensure every byte in [HEAP_START, up_to) is backed by a writable page.
// Maps only the page-aligned gap (heap_mapped .. roundup(up_to)).
static int ensure_mapped(uint64_t up_to) {
    if (up_to <= heap_mapped) {
        return 0;
    }
    uint64_t need_end = (up_to + 0xFFF) & ~0xFFFULL;   // page-align up
    uint64_t map_len = need_end - heap_mapped;          // multiple of 4096
    void *mem = sys_mmap((void *)heap_mapped, map_len, 3, 0);  // PROT_READ|PROT_WRITE
    if (!mem || (uint64_t)mem == (uint64_t)-1) {
        return -1;
    }
    heap_mapped = need_end;
    return 0;
}

static block_header_t *find_free_block(size_t size) {
    block_header_t *block = heap_start;
    while (block) {
        if (block->free && block->size >= size) {
            return block;
        }
        block = block->next;
    }
    return NULL;
}

static block_header_t *request_space(size_t size) {
    // Align size to 16 bytes
    size = (size + 15) & ~15;

    size_t total = BLK_HDR + size;
    
    // Check heap limit
    if (heap_end + total > HEAP_START + HEAP_MAX_SIZE) {
        return NULL;
    }
    
    // Back the bytes [heap_end, heap_end+total) with writable pages. Maps whole
    // pages and only beyond the watermark, so allocations that straddle a page
    // boundary are fully mapped and existing data is never re-zeroed.
    if (ensure_mapped(heap_end + total) != 0) {
        return NULL;
    }
    
    block_header_t *block = (block_header_t *)heap_end;
    block->size = size;
    block->free = 0;
    block->next = NULL;
    
    if (heap_start) {
        // Find last block and link
        block_header_t *last = heap_start;
        while (last->next) last = last->next;
        last->next = block;
    } else {
        heap_start = block;
    }
    
    heap_end += total;
    return block;
}

// Merge every run of adjacent free blocks in the list. Cheap O(n) sweep run on
// free() so fragmentation from a stream of alloc/free (very common in CPython)
// gets reclaimed instead of only coalescing the single following block.
static void coalesce_all(void) {
    block_header_t *b = heap_start;
    while (b) {
        if (b->free) {
            while (b->next && b->next->free) {
                b->size += BLK_HDR + b->next->size;
                b->next = b->next->next;
            }
        }
        b = b->next;
    }
}

void *malloc(size_t size) {
    if (size == 0) return NULL;

    // Align size
    size = (size + 15) & ~15;

    // Try to find a free block
    block_header_t *block = find_free_block(size);

    if (block) {
        // Split block if the remainder can hold a header plus a useful chunk.
        if (block->size >= size + BLK_HDR + 16) {
            block_header_t *new_block = (block_header_t *)((uint8_t *)block +
                                        BLK_HDR + size);
            new_block->size = block->size - size - BLK_HDR;
            new_block->free = 1;
            new_block->next = block->next;

            block->size = size;
            block->next = new_block;
        }
        block->free = 0;
    } else {
        // Request new space
        block = request_space(size);
        if (!block) return NULL;
    }

    return (void *)((uint8_t *)block + BLK_HDR);
}

void free(void *ptr) {
    if (!ptr) return;

    block_header_t *block = (block_header_t *)((uint8_t *)ptr - BLK_HDR);
    block->free = 1;
    coalesce_all();
}

void *calloc(size_t nmemb, size_t size) {
    // Guard the multiply against overflow (CVE-class bug in naive callocs).
    if (nmemb && size > (size_t)-1 / nmemb) return NULL;
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    block_header_t *block = (block_header_t *)((uint8_t *)ptr - BLK_HDR);
    size_t asize = (size + 15) & ~15;

    if (block->size >= asize) {
        return ptr;  // Already big enough
    }

    // Try to grow in place by absorbing an adjacent free block.
    if (block->next && block->next->free &&
        block->size + BLK_HDR + block->next->size >= asize) {
        block->size += BLK_HDR + block->next->size;
        block->next = block->next->next;
        return ptr;
    }

    // Allocate new block and copy the old contents.
    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, block->size);
        free(ptr);
    }
    return new_ptr;
}

// ============================================================================
// Process Control
// ============================================================================

void exit(int status) {
    sys_exit(status);
    __builtin_unreachable();
}

// _exit is defined in crt0.S - do not define it here

void abort(void) {
    sys_exit(127);
    __builtin_unreachable();
}

// ============================================================================
// String Conversion
// ============================================================================

int atoi(const char *str) {
    int result = 0;
    int sign = 1;
    
    while (*str == ' ' || *str == '\t') str++;
    
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return sign * result;
}

long atol(const char *str) {
    long result = 0;
    int sign = 1;
    
    while (*str == ' ' || *str == '\t') str++;
    
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }
    
    return sign * result;
}

// ============================================================================
// Random Numbers
// ============================================================================

static unsigned int rand_seed = 1;

int rand(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed / 65536) % (RAND_MAX + 1);
}

void srand(unsigned int seed) {
    rand_seed = seed;
}

// ============================================================================
// Absolute Value
// ============================================================================

int abs(int n) {
    return (n < 0) ? -n : n;
}

long labs(long n) {
    return (n < 0) ? -n : n;
}

// File I/O wrappers
int open(const char *path, int flags, ...) {
    /* #359 Phase 2: propagate the kernel's negative errno so callers (and
       CPython's import machinery) see ENOENT/EACCES instead of a bare -1.
       The optional POSIX mode argument is accepted for 3-arg compatibility;
       the kernel sys_open is 2-arg so it is not forwarded. */
    int r = sys_open(path, flags);
    if (r < 0) { errno = -r; return -1; }
    return r;
}

int close(int fd) {
    return sys_close(fd);
}

long read(int fd, void *buf, size_t count) {
    return sys_read(fd, buf, count);
}

long write(int fd, const void *buf, size_t count) {
    return sys_write(fd, buf, count);
}

// Time functions
long clock(void) {
    return sys_clock();
}

// ============================================================================
// Environment
// ============================================================================
// crt0 does not pass envp, so the process starts with an empty environment.
// getenv/setenv still round-trip within the process, which is what os.environ,
// tzset(), and countless ports actually depend on. environ is a NULL-terminated
// array of "NAME=VALUE" strings, grown on demand.

char **environ = NULL;
static int env_count = 0;
static int env_cap = 0;

static int env_find(const char *name, size_t nlen) {
    if (!environ) return -1;
    for (int i = 0; i < env_count; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=')
            return i;
    }
    return -1;
}

char *getenv(const char *name) {
    if (!name || !*name) return NULL;
    size_t nlen = strlen(name);
    int i = env_find(name, nlen);
    if (i < 0) return NULL;
    return environ[i] + nlen + 1;
}

static int env_grow(void) {
    if (env_count + 1 < env_cap) return 0;
    int ncap = env_cap ? env_cap * 2 : 8;
    char **na = (char **)malloc((size_t)ncap * sizeof(char *));
    if (!na) return -1;
    for (int i = 0; i < env_count; i++) na[i] = environ[i];
    na[env_count] = NULL;
    // old array is intentionally leaked (small, avoids freeing putenv strings)
    environ = na;
    env_cap = ncap;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=')) { errno_set(EINVAL); return -1; }
    if (!value) value = "";
    size_t nlen = strlen(name);
    int i = env_find(name, nlen);
    if (i >= 0 && !overwrite) return 0;
    size_t vlen = strlen(value);
    char *entry = (char *)malloc(nlen + vlen + 2);
    if (!entry) { errno_set(ENOMEM); return -1; }
    memcpy(entry, name, nlen);
    entry[nlen] = '=';
    memcpy(entry + nlen + 1, value, vlen);
    entry[nlen + 1 + vlen] = '\0';
    if (i >= 0) {
        environ[i] = entry;         // old entry leaked (may be a putenv string)
    } else {
        if (env_grow() < 0) { free(entry); errno_set(ENOMEM); return -1; }
        environ[env_count++] = entry;
        environ[env_count] = NULL;
    }
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) { errno_set(EINVAL); return -1; }
    size_t nlen = strlen(name);
    int i = env_find(name, nlen);
    if (i < 0) return 0;
    for (int j = i; j < env_count; j++) environ[j] = environ[j + 1];
    env_count--;
    return 0;
}

int putenv(char *string) {
    // string must contain '='; the pointer becomes part of the environment.
    if (!string) { errno_set(EINVAL); return -1; }
    char *eq = strchr(string, '=');
    if (!eq) return unsetenv(string);
    size_t nlen = (size_t)(eq - string);
    int i = env_find(string, nlen);
    if (i >= 0) { environ[i] = string; return 0; }
    if (env_grow() < 0) { errno_set(ENOMEM); return -1; }
    environ[env_count++] = string;
    environ[env_count] = NULL;
    return 0;
}

// ============================================================================
// Sorting
// ============================================================================
// Median-of-three quicksort with an insertion-sort cutoff, iterative fallback
// unnecessary at these sizes. Matches C89 qsort() contract.

static void qs_swap(char *a, char *b, size_t size) {
    while (size--) { char t = *a; *a++ = *b; *b++ = t; }
}

static void qsort_r_impl(char *base, size_t n, size_t size,
                         int (*cmp)(const void *, const void *)) {
    while (n > 12) {
        char *lo = base;
        char *hi = base + (n - 1) * size;
        char *mid = base + (n / 2) * size;
        // median-of-three -> mid
        if (cmp(mid, lo) < 0) qs_swap(mid, lo, size);
        if (cmp(hi, lo) < 0) qs_swap(hi, lo, size);
        if (cmp(hi, mid) < 0) qs_swap(hi, mid, size);
        // partition around pivot value at mid
        qs_swap(mid, hi - size, size);     // stash pivot at hi-1
        char *pivot = hi - size;
        char *i = lo;
        char *j = hi - size;
        for (;;) {
            do { i += size; } while (cmp(i, pivot) < 0);
            do { j -= size; } while (cmp(pivot, j) < 0);
            if (i >= j) break;
            qs_swap(i, j, size);
        }
        qs_swap(i, hi - size, size);       // restore pivot
        // recurse into the smaller side, loop on the larger (bounded stack)
        size_t left_n = (size_t)(i - lo) / size;
        size_t right_n = n - left_n - 1;
        if (left_n < right_n) {
            qsort_r_impl(lo, left_n, size, cmp);
            base = i + size;
            n = right_n;
        } else {
            qsort_r_impl(i + size, right_n, size, cmp);
            n = left_n;
        }
    }
    // insertion sort for the small tail
    for (size_t a = 1; a < n; a++) {
        for (char *p = base + a * size; p > base && cmp(p - size, p) > 0; p -= size)
            qs_swap(p - size, p, size);
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb < 2 || size == 0) return;
    qsort_r_impl((char *)base, nmemb, size, compar);
}

// ============================================================================
// Wide integer conversion / absolute value
// ============================================================================

long long llabs(long long n) { return (n < 0) ? -n : n; }

long long strtoll(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    while (*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\f'||*s=='\v') s++;
    int neg = 0;
    if (*s=='+') s++; else if (*s=='-') { neg=1; s++; }
    if ((base==0||base==16) && s[0]=='0' && (s[1]=='x'||s[1]=='X')) { s+=2; base=16; }
    else if (base==0 && s[0]=='0') base=8;
    else if (base==0) base=10;
    long long acc=0; int any=0;
    for (;;) {
        int c=(unsigned char)*s, d;
        if (c>='0'&&c<='9') d=c-'0';
        else if (c>='a'&&c<='z') d=c-'a'+10;
        else if (c>='A'&&c<='Z') d=c-'A'+10;
        else break;
        if (d>=base) break;
        acc = acc*base + d; any=1; s++;
    }
    if (endptr) *endptr=(char*)(any?s:nptr);
    return neg ? -acc : acc;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    while (*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\f'||*s=='\v') s++;
    int neg = 0;
    if (*s=='+') s++; else if (*s=='-') { neg=1; s++; }
    if ((base==0||base==16) && s[0]=='0' && (s[1]=='x'||s[1]=='X')) { s+=2; base=16; }
    else if (base==0 && s[0]=='0') base=8;
    else if (base==0) base=10;
    unsigned long long acc=0; int any=0;
    for (;;) {
        int c=(unsigned char)*s, d;
        if (c>='0'&&c<='9') d=c-'0';
        else if (c>='a'&&c<='z') d=c-'a'+10;
        else if (c>='A'&&c<='Z') d=c-'A'+10;
        else break;
        if (d>=base) break;
        acc = acc*(unsigned long long)base + (unsigned long long)d; any=1; s++;
    }
    if (endptr) *endptr=(char*)(any?s:nptr);
    return neg ? (unsigned long long)(-(long long)acc) : acc;
}

// ============================================================================
// Floating point parsing (strtod / strtof / atof)
// ============================================================================
// Decimal and hex-float parsing with correct end-pointer semantics. Uses
// power-of-ten scaling: accurate to ~1 ULP for typical inputs, which is all a
// C-side parser needs (CPython parses its own float literals via _Py_dg_strtod).

// 10^0 .. 10^22 are ALL exactly representable as IEEE doubles.
static const double p10_exact[23] = {
    1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11,
    1e12,1e13,1e14,1e15,1e16,1e17,1e18,1e19,1e20,1e21,1e22
};

// Scale mant by 10^e. When the mantissa was accumulated exactly (<= 15 decimal
// digits, so it fits in the 53-bit significand) and |e| <= 22, a single
// multiply/divide by an exact power of ten yields the correctly-rounded result
// (Clinger's fast path). Larger exponents chunk through 10^22 (a few ULP);
// truly extreme/subnormal magnitudes fall back to repeated squaring. This
// matches glibc for typical inputs; CPython parses its own literals via dtoa.
static double pow10_scale_digits(double x, int e, int ndig) {
    if (ndig <= 15) {
        while (e > 22)  { x *= 1e22; e -= 22; }
        while (e < -22) { x /= 1e22; e += 22; }
        if (e >= 0) return x * p10_exact[e];
        return x / p10_exact[-e];
    }
    double base = (e < 0) ? 0.1 : 10.0;
    int n = (e < 0) ? -e : e;
    while (n) {
        if (n & 1) x *= base;
        base *= base;
        n >>= 1;
    }
    return x;
}

double strtod(const char *nptr, char **endptr) {
    const char *s = nptr;
    while (*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\f'||*s=='\v') s++;

    int neg = 0;
    if (*s=='+') s++; else if (*s=='-') { neg=1; s++; }

    // inf / nan
    if ((s[0]=='i'||s[0]=='I') && (s[1]=='n'||s[1]=='N') && (s[2]=='f'||s[2]=='F')) {
        s += 3;
        if ((s[0]=='i'||s[0]=='I')&&(s[1]=='n'||s[1]=='N')&&(s[2]=='i'||s[2]=='I')&&
            (s[3]=='t'||s[3]=='T')&&(s[4]=='y'||s[4]=='Y')) s += 5;
        if (endptr) *endptr=(char*)s;
        double inf = 1e308*10.0;
        return neg ? -inf : inf;
    }
    if ((s[0]=='n'||s[0]=='N') && (s[1]=='a'||s[1]=='A') && (s[2]=='n'||s[2]=='N')) {
        s += 3;
        if (endptr) *endptr=(char*)s;
        double z = 0.0;
        return z/z; // NaN
    }

    // hex float 0x...
    if (s[0]=='0' && (s[1]=='x'||s[1]=='X')) {
        const char *hs = s + 2;
        double val = 0.0; int anydig = 0; int bexp = 0;
        for (; ; hs++) {
            int c=(unsigned char)*hs, d;
            if (c>='0'&&c<='9') d=c-'0';
            else if (c>='a'&&c<='f') d=c-'a'+10;
            else if (c>='A'&&c<='F') d=c-'A'+10;
            else break;
            val = val*16.0 + d; anydig=1;
        }
        if (*hs=='.') {
            hs++;
            for (; ; hs++) {
                int c=(unsigned char)*hs, d;
                if (c>='0'&&c<='9') d=c-'0';
                else if (c>='a'&&c<='f') d=c-'a'+10;
                else if (c>='A'&&c<='F') d=c-'A'+10;
                else break;
                val = val*16.0 + d; bexp -= 4; anydig=1;
            }
        }
        if (!anydig) { if (endptr) *endptr=(char*)nptr; return 0.0; }
        if (*hs=='p'||*hs=='P') {
            const char *es=hs+1; int esign=0;
            if (*es=='+') es++; else if (*es=='-'){esign=1;es++;}
            int e=0, ed=0;
            while (*es>='0'&&*es<='9'){ e=e*10+(*es-'0'); es++; ed=1; }
            if (ed){ bexp += esign?-e:e; hs=es; }
        }
        // scale by 2^bexp
        double p2 = 1.0; int be = bexp<0?-bexp:bexp; double b=2.0;
        while (be){ if(be&1)p2*=b; b*=b; be>>=1; }
        val = bexp<0 ? val/p2 : val*p2;
        if (endptr) *endptr=(char*)hs;
        return neg?-val:val;
    }

    // decimal
    double mant = 0.0;
    int anydig = 0;
    int exp10 = 0;
    int ndig = 0;   // digits accumulated into mant (exactness budget)
    while (*s>='0'&&*s<='9') { mant = mant*10.0 + (*s-'0'); s++; anydig=1; ndig++; }
    if (*s=='.') {
        s++;
        while (*s>='0'&&*s<='9') { mant = mant*10.0 + (*s-'0'); exp10--; s++; anydig=1; ndig++; }
    }
    if (!anydig) { if (endptr) *endptr=(char*)nptr; return 0.0; }
    if (*s=='e'||*s=='E') {
        const char *es=s+1; int esign=0;
        if (*es=='+') es++; else if (*es=='-'){esign=1;es++;}
        int e=0, ed=0;
        while (*es>='0'&&*es<='9'){ e=e*10+(*es-'0'); es++; ed=1; }
        if (ed){ exp10 += esign?-e:e; s=es; }
    }
    double val = pow10_scale_digits(mant, exp10, ndig);
    if (endptr) *endptr=(char*)s;
    return neg?-val:val;
}

float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}

double atof(const char *str) {
    return strtod(str, (char **)0);
}


// ===========================================================================
// strtol / strtoul / bsearch  (added for the NetSurf browser engine port #245)
// ===========================================================================
unsigned long strtoul(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    while (*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\f'||*s=='\v') s++;
    int neg = 0;
    if (*s=='+') s++; else if (*s=='-') { neg=1; s++; }
    if ((base==0||base==16) && s[0]=='0' && (s[1]=='x'||s[1]=='X')) { s+=2; base=16; }
    else if (base==0 && s[0]=='0') base=8;
    else if (base==0) base=10;
    unsigned long acc=0; int any=0;
    for (;;) {
        int c=(unsigned char)*s, d;
        if (c>='0'&&c<='9') d=c-'0';
        else if (c>='a'&&c<='z') d=c-'a'+10;
        else if (c>='A'&&c<='Z') d=c-'A'+10;
        else break;
        if (d>=base) break;
        acc = acc*(unsigned long)base + (unsigned long)d; any=1; s++;
    }
    if (endptr) *endptr=(char*)(any?s:nptr);
    return neg ? (unsigned long)(-(long)acc) : acc;
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    while (*s==' '||*s=='\t'||*s=='\n'||*s=='\r'||*s=='\f'||*s=='\v') s++;
    int neg = 0;
    if (*s=='+') s++; else if (*s=='-') { neg=1; s++; }
    if ((base==0||base==16) && s[0]=='0' && (s[1]=='x'||s[1]=='X')) { s+=2; base=16; }
    else if (base==0 && s[0]=='0') base=8;
    else if (base==0) base=10;
    long acc=0; int any=0;
    for (;;) {
        int c=(unsigned char)*s, d;
        if (c>='0'&&c<='9') d=c-'0';
        else if (c>='a'&&c<='z') d=c-'a'+10;
        else if (c>='A'&&c<='Z') d=c-'A'+10;
        else break;
        if (d>=base) break;
        acc = acc*base + d; any=1; s++;
    }
    if (endptr) *endptr=(char*)(any?s:nptr);
    return neg ? -acc : acc;
}

void *bsearch(const void *key, const void *base, unsigned long nmemb,
              unsigned long size, int (*compar)(const void *, const void *)) {
    unsigned long lo=0, hi=nmemb;
    const char *b=(const char*)base;
    while (lo<hi) {
        unsigned long mid = lo + (hi-lo)/2;
        const void *p = b + mid*size;
        int r = compar(key, p);
        if (r<0) hi=mid;
        else if (r>0) lo=mid+1;
        else return (void*)p;
    }
    return (void*)0;
}
