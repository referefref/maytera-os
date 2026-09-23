/*
 * maytera_vfs.c - a SQLite VFS for MayteraOS.
 *
 * WHAT THIS IS. SQLite compiled with SQLITE_OS_OTHER=1 ships no built-in VFS
 * and no sqlite3_os_init()/sqlite3_os_end(); the application must supply them.
 * This file is that supply: one sqlite3_vfs named "maytera", implemented over
 * the syscalls MayteraOS actually has (open/read/write/close/lseek/fstat/
 * unlink/fsync/ftruncate), and an sqlite3_os_init() that registers it as the
 * default so sqlite3_open() Just Works. It is OUR code, tracked in git beside
 * the recipe; it is NOT a patch to upstream sqlite3.c.
 *
 * WHY A CUSTOM VFS AT ALL. docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md
 * section 9 item 4 calls SQLite's pluggable VFS "exactly the escape hatch we
 * need for 2.4 and 2.5": the default unix VFS wants a working directory (2.4,
 * MayteraOS has none), fcntl(F_SETLK) advisory locking (absent) and O_EXCL
 * (2.5, honored on no local filesystem). This VFS depends on NONE of the nine
 * unimplemented syscalls in section 2.3. It uses only calls the kernel really
 * has, and it is honest about what it cannot do.
 *
 * ====================================================================
 * LOCKING CONTRACT: SINGLE-WRITER. READ THIS.
 * ====================================================================
 * xLock / xUnlock / xCheckReservedLock are NO-OPS. This VFS provides NO
 * inter-process locking, and it does not pretend to. The reason is structural,
 * not lazy: POSIX advisory locking (fcntl F_SETLK) does not exist on this OS,
 * and the usual fallback, an atomically-created lock file via O_CREAT|O_EXCL,
 * cannot work either because O_EXCL is not honored on the local filesystems
 * (assessment section 2.5: open(O_CREAT|O_EXCL) succeeds on an existing file).
 * A lock built on a non-atomic primitive locks nothing; faking it would be
 * worse than declaring the truth.
 *
 * THE TRUTH, THEREFORE: a database opened through this VFS is safe for ONE
 * writer at a time. Within a single process that serialises its own access
 * (and SQLITE_THREADSAFE=0 already means one thread), reads and writes are
 * correct and durable. TWO processes, or two threads, writing the SAME database
 * file concurrently are NOT protected and can corrupt it. This is the "honest
 * initial contract" section 9 item 4 asks for. Do not build a multi-writer
 * design on this VFS until real locking exists beneath it.
 */

#include "sqlite3.h"

#include <string.h>   /* memset, memcpy, strlen */
#include <stdlib.h>   /* open() is declared here on MayteraOS */
#include <unistd.h>   /* read/write/close/lseek/unlink/fsync/ftruncate/usleep, SEEK_* */
#include <fcntl.h>    /* O_RDONLY/O_RDWR/O_CREAT */
#include <sys/stat.h> /* struct stat, fstat, stat */
#include <time.h>     /* clock_gettime, struct timespec, CLOCK_REALTIME */
#include <errno.h>    /* errno, ENOENT */

#ifndef SQLITE_MAYTERA_MAXPATH
#define SQLITE_MAYTERA_MAXPATH 1024
#endif

/* Our sqlite3_file subclass. pMethods (inside base) MUST be the first member. */
typedef struct MayteraFile {
  sqlite3_file base;   /* Base class: holds pMethods. Must be first. */
  int fd;              /* Underlying MayteraOS file descriptor, or -1. */
} MayteraFile;

/* ---------------------------------------------------------------------------
 * sqlite3_io_methods: operations on an OPEN file.
 * ------------------------------------------------------------------------- */

static int mvfsClose(sqlite3_file *pFile){
  MayteraFile *p = (MayteraFile*)pFile;
  if (p->fd >= 0) { close(p->fd); p->fd = -1; }
  return SQLITE_OK;
}

static int mvfsRead(sqlite3_file *pFile, void *zBuf, int iAmt, sqlite3_int64 iOfst){
  MayteraFile *p = (MayteraFile*)pFile;
  char *out = (char*)zBuf;
  int got = 0;
  if (lseek(p->fd, (off_t)iOfst, SEEK_SET) < 0) return SQLITE_IOERR_READ;
  while (got < iAmt) {
    long n = read(p->fd, out + got, (size_t)(iAmt - got));
    if (n < 0) return SQLITE_IOERR_READ;
    if (n == 0) break;                 /* short read: hit EOF */
    got += (int)n;
  }
  if (got < iAmt) {
    /* SQLite requires the unread tail to be zeroed and a distinct code. */
    memset(out + got, 0, (size_t)(iAmt - got));
    return SQLITE_IOERR_SHORT_READ;
  }
  return SQLITE_OK;
}

static int mvfsWrite(sqlite3_file *pFile, const void *zBuf, int iAmt, sqlite3_int64 iOfst){
  MayteraFile *p = (MayteraFile*)pFile;
  const char *in = (const char*)zBuf;
  int done = 0;
  if (lseek(p->fd, (off_t)iOfst, SEEK_SET) < 0) return SQLITE_IOERR_WRITE;
  while (done < iAmt) {
    long n = write(p->fd, in + done, (size_t)(iAmt - done));
    if (n <= 0) return SQLITE_IOERR_WRITE;
    done += (int)n;
  }
  return SQLITE_OK;
}

static int mvfsTruncate(sqlite3_file *pFile, sqlite3_int64 size){
  MayteraFile *p = (MayteraFile*)pFile;
  /* MayteraOS ftruncate is shrink-only; SQLite only ever truncates downward. */
  if (ftruncate(p->fd, (long)size) < 0) return SQLITE_IOERR_TRUNCATE;
  return SQLITE_OK;
}

static int mvfsSync(sqlite3_file *pFile, int flags){
  MayteraFile *p = (MayteraFile*)pFile;
  (void)flags;
  /* Block storage is write-through today, but call fsync so this stays correct
     on any future write-back device. A negative return is a real durability
     failure and must reach SQLite, not be swallowed. */
  if (fsync(p->fd) < 0) return SQLITE_IOERR_FSYNC;
  return SQLITE_OK;
}

static int mvfsFileSize(sqlite3_file *pFile, sqlite3_int64 *pSize){
  MayteraFile *p = (MayteraFile*)pFile;
  struct stat st;
  if (fstat(p->fd, &st) == 0) { *pSize = (sqlite3_int64)st.st_size; return SQLITE_OK; }
  /* Fallback for an older kernel whose fstat is size-only or unavailable. */
  {
    off_t end = lseek(p->fd, 0, SEEK_END);
    if (end < 0) return SQLITE_IOERR_FSTAT;
    *pSize = (sqlite3_int64)end;
  }
  return SQLITE_OK;
}

/* SINGLE-WRITER CONTRACT (see file header): no inter-process locking exists. */
static int mvfsLock(sqlite3_file *pFile, int eLock){ (void)pFile; (void)eLock; return SQLITE_OK; }
static int mvfsUnlock(sqlite3_file *pFile, int eLock){ (void)pFile; (void)eLock; return SQLITE_OK; }
static int mvfsCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  (void)pFile;
  *pResOut = 0;   /* We never hold a reserved lock, and cannot see another's. */
  return SQLITE_OK;
}

static int mvfsFileControl(sqlite3_file *pFile, int op, void *pArg){
  (void)pFile;
  if (op == SQLITE_FCNTL_VFSNAME) {
    *(char**)pArg = sqlite3_mprintf("maytera");
    return SQLITE_OK;
  }
  return SQLITE_NOTFOUND;   /* Everything else: SQLite uses its own default. */
}

static int mvfsSectorSize(sqlite3_file *pFile){ (void)pFile; return 512; }
static int mvfsDeviceCharacteristics(sqlite3_file *pFile){ (void)pFile; return 0; }

static const sqlite3_io_methods maytera_io_methods = {
  .iVersion               = 1,   /* v1: no xShm*, no xFetch/xUnfetch (WAL/mmap off) */
  .xClose                 = mvfsClose,
  .xRead                  = mvfsRead,
  .xWrite                 = mvfsWrite,
  .xTruncate              = mvfsTruncate,
  .xSync                  = mvfsSync,
  .xFileSize              = mvfsFileSize,
  .xLock                  = mvfsLock,
  .xUnlock                = mvfsUnlock,
  .xCheckReservedLock     = mvfsCheckReservedLock,
  .xFileControl           = mvfsFileControl,
  .xSectorSize            = mvfsSectorSize,
  .xDeviceCharacteristics = mvfsDeviceCharacteristics,
};

/* ---------------------------------------------------------------------------
 * sqlite3_vfs: operations that name a file by path.
 * ------------------------------------------------------------------------- */

static int mvfsOpen(sqlite3_vfs *pVfs, sqlite3_filename zName, sqlite3_file *pFile,
                    int flags, int *pOutFlags){
  MayteraFile *p = (MayteraFile*)pFile;
  int oflags;
  int fd;
  (void)pVfs;

  p->base.pMethods = 0;
  p->fd = -1;

  if (zName == 0) {
    /* An anonymous temp file. SQLITE_TEMP_STORE=3 keeps temp content in memory,
       and MayteraOS has no cwd or temp directory (2.4), so a nameless on-disk
       file cannot be created honestly. This should not be reached. */
    return SQLITE_CANTOPEN;
  }

  oflags = (flags & SQLITE_OPEN_READONLY) ? O_RDONLY : O_RDWR;
  if (flags & SQLITE_OPEN_CREATE) oflags |= O_CREAT;
  /* SQLITE_OPEN_EXCLUSIVE maps to O_EXCL, which MayteraOS does not honor (2.5).
     We deliberately do NOT request it: a lock built on a fake O_EXCL is worse
     than none. The single-writer contract stands in its place. */

  fd = open(zName, oflags, 0644);
  if (fd < 0) return SQLITE_CANTOPEN;

  p->base.pMethods = &maytera_io_methods;
  p->fd = fd;
  if (pOutFlags) *pOutFlags = flags;
  return SQLITE_OK;
}

static int mvfsDelete(sqlite3_vfs *pVfs, const char *zName, int syncDir){
  (void)pVfs; (void)syncDir;   /* Write-through storage: no directory fsync. */
  if (unlink(zName) == 0) return SQLITE_OK;
  if (errno == ENOENT) return SQLITE_OK;   /* Already gone is success for SQLite. */
  return SQLITE_IOERR_DELETE;
}

static int mvfsAccess(sqlite3_vfs *pVfs, const char *zName, int flags, int *pResOut){
  struct stat st;
  (void)pVfs;
  if (stat(zName, &st) != 0) { *pResOut = 0; return SQLITE_OK; }
  /* Existence is the only distinction MayteraOS's single-user model makes
     meaningful here: a file that exists is readable and writable by its owner.
     EXISTS / READ / READWRITE therefore all answer "yes it is there". */
  (void)flags;
  *pResOut = 1;
  return SQLITE_OK;
}

static int mvfsFullPathname(sqlite3_vfs *pVfs, const char *zPath, int nOut, char *zOut){
  size_t n = strlen(zPath);
  (void)pVfs;
  if (zPath[0] == '/') {
    if ((int)n >= nOut) return SQLITE_CANTOPEN;
    memcpy(zOut, zPath, n + 1);
  } else {
    /* No working directory exists on MayteraOS (assessment 2.4): a relative
       path is resolved against the filesystem root, matching the kernel's own
       fdlayer behavior. Callers SHOULD pass absolute paths. */
    if ((int)n + 1 >= nOut) return SQLITE_CANTOPEN;
    zOut[0] = '/';
    memcpy(zOut + 1, zPath, n + 1);
  }
  return SQLITE_OK;
}

static int mvfsRandomness(sqlite3_vfs *pVfs, int nByte, char *zOut){
  /* Seeded once from the real-time clock, then a xorshift64* stream. This is
     used to salt rollback-journal headers and sqlite3_randomness(); it is not a
     cryptographic source and SQLite does not require one. */
  static unsigned long long s = 0;
  int i;
  (void)pVfs;
  if (s == 0) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) { ts.tv_sec = 1; ts.tv_nsec = 1; }
    s = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    if (s == 0) s = 0x9e3779b97f4a7c15ull;
  }
  for (i = 0; i < nByte; i++) {
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    zOut[i] = (char)((s * 0x2545F4914F6CDD1Dull) >> 33);
  }
  return nByte;
}

static int mvfsSleep(sqlite3_vfs *pVfs, int microseconds){
  (void)pVfs;
  if (microseconds > 0) usleep((unsigned long)microseconds);
  return microseconds;
}

static int mvfsCurrentTime(sqlite3_vfs *pVfs, double *pTime){
  struct timespec ts;
  (void)pVfs;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) { ts.tv_sec = 0; ts.tv_nsec = 0; }
  /* Julian Day number. The Unix epoch (1970-01-01T00:00:00Z) is JD 2440587.5. */
  *pTime = 2440587.5 + ((double)ts.tv_sec + (double)ts.tv_nsec / 1e9) / 86400.0;
  return SQLITE_OK;
}

static int mvfsGetLastError(sqlite3_vfs *pVfs, int nBuf, char *zBuf){
  (void)pVfs;
  if (nBuf > 0 && zBuf) zBuf[0] = 0;
  return errno;
}

static sqlite3_vfs maytera_vfs = {
  .iVersion      = 1,                      /* v1: no xCurrentTimeInt64, no xSystemCall */
  .szOsFile      = sizeof(MayteraFile),
  .mxPathname    = SQLITE_MAYTERA_MAXPATH,
  .pNext         = 0,
  .zName         = "maytera",
  .pAppData      = 0,
  .xOpen         = mvfsOpen,
  .xDelete       = mvfsDelete,
  .xAccess       = mvfsAccess,
  .xFullPathname = mvfsFullPathname,
  /* xDlOpen/xDlError/xDlSym/xDlClose left NULL: SQLITE_OMIT_LOAD_EXTENSION. */
  .xRandomness   = mvfsRandomness,
  .xSleep        = mvfsSleep,
  .xCurrentTime  = mvfsCurrentTime,
  .xGetLastError = mvfsGetLastError,
};

/* Register the MayteraOS VFS. Exposed so an app can register explicitly if it
   ever runs before sqlite3_initialize(); normally sqlite3_os_init() below does
   it. makeDefault != 0 makes it the VFS sqlite3_open() uses. */
int maytera_vfs_register(int makeDefault){
  return sqlite3_vfs_register(&maytera_vfs, makeDefault);
}

/* SQLITE_OS_OTHER=1 means the amalgamation supplies NO sqlite3_os_init/os_end.
   sqlite3_initialize() (called automatically on the first sqlite3_open unless
   SQLITE_OMIT_AUTOINIT) calls sqlite3_os_init(), which is where the default VFS
   is registered. */
int sqlite3_os_init(void){
  return maytera_vfs_register(1);
}

int sqlite3_os_end(void){
  return SQLITE_OK;
}
