/* tremor_compat.c - libc helpers needed by vendored Tremor/libogg that the
 * MayteraOS kernel does not already provide (kernel supplies memcpy/memmove/
 * memset/memcmp/strlen/strcpy/strncpy/strcmp/strncmp/strchr/strcat).
 * No FPU, no real libc. Part of the #236 OGG Vorbis (Tremor) port. */

typedef unsigned long long t_size;

int errno = 0;

void *memchr(const void *s, int c, t_size n){
    const unsigned char *p = (const unsigned char *)s;
    while(n--){ if(*p == (unsigned char)c) return (void *)p; p++; }
    return 0;
}

int strncasecmp(const char *a, const char *b, t_size n){
    while(n--){
        int ca = *a++, cb = *b++;
        if(ca>='A'&&ca<='Z') ca += 32;
        if(cb>='A'&&cb<='Z') cb += 32;
        if(ca != cb) return ca - cb;
        if(ca == 0) return 0;
    }
    return 0;
}

int abs(int x){ return x < 0 ? -x : x; }
long labs(long x){ return x < 0 ? -x : x; }

/* Simple, allocation-free insertion-then-shell sort (qsort replacement).
 * Tremor uses qsort only to sort small codebook tables at header-parse time;
 * a shell sort is O(n^1.3), stable enough, and needs no recursion/stack. */
void qsort(void *base, t_size nmemb, t_size size,
           int (*cmp)(const void *, const void *)){
    unsigned char *a = (unsigned char *)base;
    unsigned char tmp[256];
    if(size > sizeof(tmp) || nmemb < 2) return;
    for(t_size gap = nmemb/2; gap > 0; gap /= 2){
        for(t_size i = gap; i < nmemb; i++){
            for(long j = (long)i - (long)gap; j >= 0; j -= gap){
                unsigned char *pj  = a + (t_size)j * size;
                unsigned char *pjg = a + ((t_size)j + gap) * size;
                if(cmp(pj, pjg) <= 0) break;
                for(t_size k = 0; k < size; k++){
                    tmp[k] = pj[k]; pj[k] = pjg[k]; pjg[k] = tmp[k];
                }
            }
        }
    }
}

/* FILE-based ov_open/ov_fopen path is never called; stubs satisfy the linker. */
typedef struct _MAYTERA_FILE FILE;
FILE  *fopen(const char *p, const char *m){ (void)p;(void)m; return 0; }
t_size fread(void *b, t_size s, t_size n, FILE *f){ (void)b;(void)s;(void)n;(void)f; return 0; }
int    fseek(FILE *f, long o, int w){ (void)f;(void)o;(void)w; return -1; }
long   ftell(FILE *f){ (void)f; return -1; }
int    fclose(FILE *f){ (void)f; return 0; }
