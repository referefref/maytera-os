/* opus_compat.c - the few libc helpers the vendored fixed-point libopus may
 * reference that the MayteraOS kernel does not already export. abs/labs/memchr
 * come from media/tremor/tremor_compat.c and snprintf from string.c (shared);
 * only the printf/fprintf/abort/stderr stubs are unique here. They are reached
 * solely from disabled assert/debug paths. No FPU, no real libc. #331 Opus. */

typedef unsigned long long o_size;

typedef struct _MAYTERA_OPUS_FILE FILE;
FILE *stderr = 0;
int  printf(const char *fmt, ...)            { (void)fmt; return 0; }
int  fprintf(FILE *f, const char *fmt, ...)  { (void)f; (void)fmt; return 0; }
void abort(void)                             { for (;;) {} }
