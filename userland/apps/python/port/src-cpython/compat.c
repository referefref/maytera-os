/* compat.c - #359 Phase 2 trimmed. getc/clearerr/access and the __wrap_open/
   __wrap_fcntl shims moved into / obsoleted by the shared libc (posixextra.c +
   errno-correct open() + kernel SYS_FCNTL). Only what libc still lacks remains. */
int utime(const char *path, const void *times) { (void)path; (void)times; return 0; }
int utimes(const char *path, const void *tv) { (void)path; (void)tv; return 0; }
/* libgcc popcount (freestanding build has no libgcc) */
int __popcountdi2(long long a){ unsigned long long x=(unsigned long long)a; int c=0; while(x){ x&=x-1; c++; } return c; }
int __popcountsi2(int a){ unsigned x=(unsigned)a; int c=0; while(x){ x&=x-1; c++; } return c; }
/* CPython optional native thread id */
unsigned long pthread_self(void);
unsigned long PyThread_get_thread_native_id(void){ return (unsigned long)pthread_self(); }
