// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// escrow_test.c - MEASURED host end-to-end test for the AI escrow/promise
// capability (#712), same pattern as aifs_test.c: the REAL units under test
// (escrow.c, photorg.c, aiclient.c executors + the aiclient_run_action gate,
// aicap.c token layer + the #712 capability denylist) are compiled freestanding
// exactly as userland/libc/Makefile does, symbol-prefixed with "ad_" and linked
// next to glibc. This harness backs their file/dir/clock syscalls with the host
// and their libc primitives with host libc. Nothing here reimplements the escrow,
// the organiser or the gate; the logic exercised is the shipped code.
//
// It drives the OWNER'S SCENARIO: photos with varied mtimes on a scope dir are
// organised into date folders under an escrow contract, then asserts, MEASURED:
//   - the correct date folders were created,
//   - every photo moved into its folder, bytes identical, source gone,
//   - ZERO SYS_UNLINK were issued (delete_count == 0),
//   - the promise verified FULFILLED,
//   - the grant token was EARLY-CLOSED before its 3600s TTL (unusable after),
//   - an out-of-scope path is refused, and an fs.delete under the grant is
//     refused (no-delete policy),
//   - the NEGATIVE case: a promised move that did not happen -> PARTIAL and NO
//     early close (the grant is retained).
//
// Each scenario is a SEPARATE PROCESS (argv[1]) so aicap's in-memory token cache,
// the #712 denylist and the on-disk /CONFIG files start clean. The virtual FS is
// rooted at $AIFS_ROOT (a fresh mktemp -d per scenario).
//
// Exit code: 0 = behaved exactly as asserted, 1 = it did not, 2 = harness error.
// #246/#305 STAGE 6 MIGRATION NOTE. escrow.c is now a THIN CLIENT of the KERNEL
// enforcement: escrow_request() enters the kernel escrow contract
// (SYS_ESCROW_ENTER) and the kernel - not this userland layer - is the authority
// for scope + no-delete + device; escrow_close() aborts via SYS_ESCROW_ABORT on a
// failed promise. This host harness has NO kernel, so it can no longer assert the
// ENFORCEMENT properties (out-of-scope refused, fs.delete refused): those are now
// kernel-side and are proven IN A VM by /APPS/ESCU6 (see docs/
// CONTRACT_ENFORCEMENT_PLAN.md section 3g, the build host:/root/kescrow6-proof/). To run the
// organiser/promise-oracle scenarios here, the mock must stub SYS_ESCROW_ENTER/
// _EXIT/_ABORT as success; the enforcement assertions belong to ESCU6 now, not to
// this kernel-less host test. Do not read a green run here as proof of kernel
// enforcement.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <utime.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "../escrow.h"   // escrow_contract_t / escrow_verify_t / ESCROW_* (self-contained)

// ---- units under test (ad_-prefixed by run_escrow.sh's objcopy) -------------
int  ad_aiclient_run_action(const char *id, const char *args, char *obs, int ocap);
escrow_contract_t *ad_escrow_request(const char *scope, const char *note, long ttl);
int  ad_escrow_verify(escrow_contract_t *c, escrow_verify_t *out);
int  ad_escrow_close(escrow_contract_t *c);
int  ad_photorg_organize(const char *scope, const char *gb, char *summary, int scap);
int  ad_aicap_cap_denied(const char *cap);

// ---- host root for the virtual tree -----------------------------------------
static char g_root[512];
static long g_unlink_count = 0;   // every SYS_UNLINK the UUT issues is counted

static void mapped(const char *vpath, char *out, int ocap) {
    if (vpath && vpath[0] == '/') snprintf(out, ocap, "%s%s", g_root, vpath);
    else                          snprintf(out, ocap, "%s/%s", g_root, vpath ? vpath : "");
}
static void host_mkparents(const char *hostpath) {
    char tmp[700]; snprintf(tmp, sizeof(tmp), "%s", hostpath);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
}
static const char *base_name(const char *p) {
    const char *b = p; for (const char *s = p; *s; s++) if (*s == '/') b = s + 1; return b;
}

// ---- maytera syscall numbers (from syscall.h) -------------------------------
#define SYS_OPEN 10
#define SYS_CLOSE 11
#define SYS_READ 12
#define SYS_WRITE 13
#define SYS_SEEK 14
#define SYS_STAT 15
#define SYS_MKDIR 16
#define SYS_UNLINK 18
#define SYS_READDIR 19
#define SYS_TIME 50
#define SYS_RENAME 70
#define M_O_CREAT 0x0040

// ---- byte-for-byte replica of maytera struct stat (sys/stat.h layout) -------
// Field names are m_* (not st_*) because glibc defines st_atime/st_mtime/st_ctime
// as macros; only the byte layout has to match maytera's struct stat.
typedef struct {
    unsigned long m_dev, m_ino;
    unsigned int  m_mode, m_nlink, m_uid, m_gid;
    unsigned long m_rdev;
    long          m_size, m_blksize, m_blocks;
    unsigned long m_atime, m_mtime, m_ctime;
} mstat_t;
// maytera dirent_t replica.
typedef struct { char name[256]; unsigned int type; unsigned int size; } mdirent_t;

// ---- directory-fd table (dir reads via opendir/readdir, keyed by synthetic fd)
#define DIRFD_BASE 100000
static DIR *g_dirs[64];

// ---- the ad_syscallN the UUT bottoms out in, backed by the host FS ----------
long ad_syscall0(long n) { if (n == SYS_TIME) return (long)time(NULL); return 0; }

long ad_syscall1(long n, long a) {
    if (n == SYS_CLOSE) {
        if (a >= DIRFD_BASE) { int s = (int)(a - DIRFD_BASE);
            if (s >= 0 && s < 64 && g_dirs[s]) { closedir(g_dirs[s]); g_dirs[s] = 0; } return 0; }
        return close((int)a);
    }
    if (n == SYS_UNLINK) {
        char hp[700]; mapped((const char *)a, hp, sizeof hp);
        g_unlink_count++;                       // record EVERY delete the UUT makes
        return unlink(hp);
    }
    return 0;
}

long ad_syscall2(long n, long a, long b) {
    if (n == SYS_OPEN) {
        char hp[700]; mapped((const char *)a, hp, sizeof hp);
        struct stat st;
        if (stat(hp, &st) == 0 && S_ISDIR(st.st_mode)) {
            for (int s = 0; s < 64; s++) if (!g_dirs[s]) {
                DIR *d = opendir(hp); if (!d) return -1;
                g_dirs[s] = d; return DIRFD_BASE + s;
            }
            return -1;
        }
        if (((int)b) & M_O_CREAT) host_mkparents(hp);
        return open(hp, (int)b, 0644);
    }
    if (n == SYS_STAT) {
        char hp[700]; mapped((const char *)a, hp, sizeof hp);
        struct stat hs;
        if (stat(hp, &hs) != 0) return -1;
        mstat_t *m = (mstat_t *)b; memset(m, 0, sizeof(*m));
        m->m_mode = hs.st_mode; m->m_size = hs.st_size;
        m->m_mtime = (unsigned long)hs.st_mtime; m->m_nlink = 1;
        return 0;
    }
    if (n == SYS_MKDIR) {
        char hp[700]; mapped((const char *)a, hp, sizeof hp);
        host_mkparents(hp);
        return mkdir(hp, (int)b);
    }
    if (n == SYS_RENAME) {
        char op[700], np[700];
        mapped((const char *)a, op, sizeof op);
        mapped((const char *)b, np, sizeof np);
        // Fault injection for the negative test: a src named *FAILMOVE* cannot move.
        if (strstr(base_name((const char *)a), "FAILMOVE")) return -1;
        host_mkparents(np);
        return rename(op, np);                  // RELOCATE - no delete counted
    }
    if (n == SYS_READDIR) {
        if (a < DIRFD_BASE) return 1;
        int s = (int)(a - DIRFD_BASE);
        if (s < 0 || s >= 64 || !g_dirs[s]) return 1;
        struct dirent *de = readdir(g_dirs[s]);
        mdirent_t *m = (mdirent_t *)b;
        if (!de) { m->name[0] = 0; return 1; }
        snprintf(m->name, sizeof(m->name), "%s", de->d_name);
        int isdir = 0; unsigned sz = 0;
        struct stat es;
        if (fstatat(dirfd(g_dirs[s]), de->d_name, &es, 0) == 0) {
            isdir = S_ISDIR(es.st_mode); sz = (unsigned)es.st_size;
        } else {
            isdir = (de->d_type == DT_DIR);
        }
        m->type = isdir ? 1 : 0; m->size = sz;
        return 0;
    }
    return 0;
}

long ad_syscall3(long n, long a, long b, long c) {
    if (n == SYS_READ)  return read((int)a, (void *)b, (size_t)c);
    if (n == SYS_WRITE) return write((int)a, (const void *)b, (size_t)c);
    if (n == SYS_SEEK)  return lseek((int)a, b, (int)c);
    return 0;
}
long ad_syscall4(long n, long a, long b, long c, long d) { (void)n;(void)a;(void)b;(void)c;(void)d; return 0; }
long ad_syscall5(long n, long a, long b, long c, long d, long e) { (void)n;(void)a;(void)b;(void)c;(void)d;(void)e; return 0; }
long ad_syscall6(long n, long a, long b, long c, long d, long e, long f) { (void)n;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return 0; }

// ---- libc primitives the UUT calls, backed by host libc (ad_-namespaced) ----
int   ad_snprintf(char *s, size_t n, const char *f, ...) { va_list ap; va_start(ap,f); int r=vsnprintf(s,n,f,ap); va_end(ap); return r; }
int   ad_printf(const char *f, ...) { va_list ap; va_start(ap,f); int r=vprintf(f,ap); va_end(ap); return r; }
void *ad_malloc(size_t n) { return malloc(n); }
void  ad_free(void *p) { free(p); }
void *ad_memcpy(void *d, const void *s, size_t n) { return memcpy(d,s,n); }
void *ad_memmove(void *d, const void *s, size_t n) { return memmove(d,s,n); }
void *ad_memset(void *d, int c, size_t n) { return memset(d,c,n); }
int   ad_strcmp(const char *a, const char *b) { return strcmp(a,b); }
int   ad_strncmp(const char *a, const char *b, size_t n) { return strncmp(a,b,n); }
size_t ad_strlen(const char *s) { return strlen(s); }
char *ad_strstr(const char *h, const char *n) { return strstr((char*)h,n); }
size_t ad_strlcpy(char *d, const char *s, size_t n) {
    size_t sl = strlen(s);
    if (n) { size_t c = sl < n-1 ? sl : n-1; memcpy(d,s,c); d[c]=0; }
    return sl;
}

// ---- ad_ stubs for aiclient.c symbols NOT on the tested path -----------------
int  ad_aidev_authorize_tool(const char *t, const char *a, char *o, int c) { (void)t;(void)a; if(o&&c) o[0]=0; return 1; }
int  ad_aiguard_check(const char *a, int b, char *c, int d) { (void)a;(void)b;(void)c;(void)d; return 0; }
const char *ad_aiguard_sev_name(int s) { (void)s; return "none"; }
int  ad_contract_invoke(const char *a, const char *b, char *c, int d) { (void)a;(void)b;(void)c;(void)d; return -1; }
int  ad_contract_invoke_live(const char *a, const char *b, const char *c, char *d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; return -1; }
int  ad___spawn_with_env(const char *a, char *const b[], char *const c[]) { (void)a;(void)b;(void)c; return -1; }
int  ad_userconf_open_read(const char *a) { (void)a; return -1; }
const char *ad_userhome_path(void) { return "/HOME"; }

// ---- helpers -----------------------------------------------------------------
static void ensure_config(void) { char hp[700]; mapped("/CONFIG/x", hp, sizeof hp); host_mkparents(hp); }
static void put_file(const char *vpath, const char *data) {
    char hp[700]; mapped(vpath, hp, sizeof hp); host_mkparents(hp);
    FILE *f = fopen(hp, "w"); if (!f) { perror("put"); exit(2); }
    fputs(data, f); fclose(f);
}
static void put_photo(const char *vpath, const char *data, long mtime) {
    put_file(vpath, data);
    char hp[700]; mapped(vpath, hp, sizeof hp);
    struct utimbuf ub; ub.actime = mtime; ub.modtime = mtime;
    if (utime(hp, &ub) != 0) perror("utime");
}
// ---- #713 EXIF fixtures: raw-bytes photo writer + synthetic EXIF JPEG --------
static void put_photo_bin(const char *vpath, const unsigned char *data, int len,
                          long mtime) {
    char hp[700]; mapped(vpath, hp, sizeof hp); host_mkparents(hp);
    FILE *f = fopen(hp, "wb"); if (!f) { perror("putbin"); exit(2); }
    if (fwrite(data, 1, (size_t)len, f) != (size_t)len) { perror("fwrite"); exit(2); }
    fclose(f);
    struct utimbuf ub; ub.actime = mtime; ub.modtime = mtime;
    if (utime(hp, &ub) != 0) perror("utime");
}
static void ex_put16(unsigned char *p, unsigned int v, int be) {
    if (be) { p[0]=(v>>8)&0xFF; p[1]=v&0xFF; } else { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
}
static void ex_put32(unsigned char *p, unsigned long v, int be) {
    if (be) { p[0]=(v>>24)&0xFF; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF; }
    else    { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
}
// Build a minimal JPEG carrying EXIF DateTimeOriginal = y-m-d, TIFF byte order
// `be` (0=II little-endian, 1=MM big-endian). Returns bytes written to out.
static int build_exif_jpeg(unsigned char *out, int be, int y, int m, int d) {
    unsigned char *o = out; *o++ = 0xFF; *o++ = 0xD8;    // SOI
    unsigned char tiff[64]; memset(tiff, 0, sizeof tiff);
    tiff[0] = be ? 0x4D : 0x49; tiff[1] = be ? 0x4D : 0x49;
    ex_put16(tiff+2, 42, be); ex_put32(tiff+4, 8, be);
    ex_put16(tiff+8, 1, be);
    ex_put16(tiff+10, 0x8769, be); ex_put16(tiff+12, 4, be);
    ex_put32(tiff+14, 1, be);      ex_put32(tiff+18, 26, be);
    ex_put32(tiff+22, 0, be);
    ex_put16(tiff+26, 1, be);
    ex_put16(tiff+28, 0x9003, be); ex_put16(tiff+30, 2, be);
    ex_put32(tiff+32, 20, be);     ex_put32(tiff+36, 44, be);
    ex_put32(tiff+40, 0, be);
    char ds[24]; snprintf(ds, sizeof ds, "%04d:%02d:%02d 13:00:00", y, m, d);
    memcpy(tiff+44, ds, 20);
    int payload = 6 + 64;
    *o++ = 0xFF; *o++ = 0xE1; ex_put16(o, (unsigned)(payload+2), 1); o += 2;
    memcpy(o, "Exif\0\0", 6); o += 6; memcpy(o, tiff, 64); o += 64;
    *o++ = 0xFF; *o++ = 0xD9;                              // EOI
    return (int)(o - out);
}

static void seed_consent(const char *line) {
    char hp[700]; mapped("/CONFIG/AICONSENT.CFG", hp, sizeof hp); host_mkparents(hp);
    FILE *f = fopen(hp, "w"); if (!f) { perror("consent"); exit(2); }
    fprintf(f, "%s\n", line); fclose(f);
}
static int is_dir(const char *vpath) {
    char hp[700]; mapped(vpath, hp, sizeof hp); struct stat st;
    return stat(hp, &st) == 0 && S_ISDIR(st.st_mode);
}
static int is_file(const char *vpath) {
    char hp[700]; mapped(vpath, hp, sizeof hp); struct stat st;
    return stat(hp, &st) == 0 && S_ISREG(st.st_mode);
}
static int read_all(const char *vpath, char *buf, int cap) {
    char hp[700]; mapped(vpath, hp, sizeof hp);
    FILE *f = fopen(hp, "r"); if (!f) return -1;
    int n = (int)fread(buf, 1, cap-1, f); buf[n>=0?n:0]=0; fclose(f); return n;
}
static int audit_has(const char *needle) {
    char b[32768]; if (read_all("/CONFIG/AIAUDIT.LOG", b, sizeof b) <= 0) return 0;
    return strstr(b, needle) != 0;
}
// find a file with matching content under vdir (one level); 1 if found.
static int dir_has_content(const char *vdir, const char *want) {
    char hp[700]; mapped(vdir, hp, sizeof hp);
    DIR *d = opendir(hp); if (!d) return 0;
    struct dirent *de; int found = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char vp[800]; snprintf(vp, sizeof vp, "%s/%s", vdir, de->d_name);
        char got[512]; if (read_all(vp, got, sizeof got) < 0) continue;
        if (!strcmp(got, want)) { found = 1; break; }
    }
    closedir(d);
    return found;
}

#define OK(msg)  do{ printf("  ok: %s\n", msg); }while(0)
#define BAD(msg) do{ printf("  FAIL: %s\n", msg); return 1; }while(0)

// Known epochs: 2026-01-15 00:00:00 UTC and 2026-03-10 00:00:00 UTC.
#define MT_JAN 1768435200L
#define MT_MAR 1773100800L
#define D_JAN1 "JAN1-photo-bytes-keep-intact\n"
#define D_JAN2 "JAN2-different-content\n"
#define D_MAR1 "MAR1-content-png\n"
static char obs[8192];
static char summary[4096];

// ---------------------------------------------------------------------------
// GREEN: full owner scenario through the organiser + escrow contract.
// ---------------------------------------------------------------------------
static int sc_organize(void) {
    ensure_config();
    put_photo("/MEDIA/USB/JAN1.JPG", D_JAN1, MT_JAN);
    put_photo("/MEDIA/USB/JAN2.JPG", D_JAN2, MT_JAN + 86400);
    put_photo("/MEDIA/USB/MAR1.PNG", D_MAR1, MT_MAR);
    put_file ("/MEDIA/USB/README.TXT", "not an image, must stay\n");
    // Pre-existing collision: a DIFFERENT file already at the Jan destination.
    put_file ("/MEDIA/USB/2026-01/JAN1.JPG", "OLD-PREEXISTING-DO-NOT-LOSE\n");

    int verdict = ad_photorg_organize("/MEDIA/USB", "YYYY-MM", summary, sizeof summary);
    printf("  verdict=%d unlink=%ld\n  summary:\n%s\n", verdict, g_unlink_count, summary);

    if (g_unlink_count != 0) BAD("a delete happened during the organise (unlink != 0)");
    if (verdict != ESCROW_FULFILLED) BAD("promise did not verify FULFILLED");
    if (!is_dir("/MEDIA/USB/2026-01")) BAD("2026-01 date folder not created");
    if (!is_dir("/MEDIA/USB/2026-03")) BAD("2026-03 date folder not created");
    if (is_file("/MEDIA/USB/JAN1.JPG")) BAD("root JAN1.JPG still present (not moved)");
    if (is_file("/MEDIA/USB/JAN2.JPG")) BAD("root JAN2.JPG still present (not moved)");
    if (is_file("/MEDIA/USB/MAR1.PNG")) BAD("root MAR1.PNG still present (not moved)");
    if (!is_file("/MEDIA/USB/README.TXT")) BAD("non-image README.TXT was touched");
    // pre-existing collision target untouched
    { char got[128]; read_all("/MEDIA/USB/2026-01/JAN1.JPG", got, sizeof got);
      if (strcmp(got, "OLD-PREEXISTING-DO-NOT-LOSE\n") != 0) BAD("pre-existing file overwritten/lost"); }
    // moved photos present bytes-identical under their date folders (collision-safe name)
    if (!dir_has_content("/MEDIA/USB/2026-01", D_JAN1)) BAD("JAN1 bytes not found in 2026-01 (collision-safe move failed)");
    if (!dir_has_content("/MEDIA/USB/2026-01", D_JAN2)) BAD("JAN2 bytes not found in 2026-01");
    if (!dir_has_content("/MEDIA/USB/2026-03", D_MAR1)) BAD("MAR1 bytes not found in 2026-03");
    // EARLY CLOSE: after fulfilment the grant token is revoked -> a move is refused.
    ad_aiclient_run_action("files.move",
        "{\"src\":\"/MEDIA/USB/2026-03/MAR1.PNG\",\"dst\":\"/MEDIA/USB/2026-01/MAR1.PNG\"}",
        obs, sizeof obs);
    if (!strstr(obs, "CAPABILITY_DENIED")) BAD("token was NOT early-closed (move still allowed after close)");
    if (ad_aicap_cap_denied("fs.delete")) BAD("no-delete policy not lifted after fulfilled close");
    if (!audit_has("FULFILLED")) BAD("audit trail missing the FULFILLED verdict");
    if (!audit_has("escrow.grant")) BAD("audit trail missing the grant record");
    OK("organised bytes-intact, zero deletes, promise FULFILLED, token early-closed");
    return 0;
}

// ---------------------------------------------------------------------------
// GREEN: the chatbot tool path (photos.organize) through the REAL gate.
// ---------------------------------------------------------------------------
static int sc_tool(void) {
    ensure_config();
    seed_consent("photos.organize=session");     // headless consent for the outer tool
    put_photo("/MEDIA/USB/A.JPG", "aa\n", MT_JAN);
    put_photo("/MEDIA/USB/B.PNG", "bb\n", MT_MAR);
    put_file ("/MEDIA/USB/DOC.TXT", "keep\n");
    int az = ad_aiclient_run_action("photos.organize",
        "{\"path\":\"/MEDIA/USB\",\"group_by\":\"YYYY-MM\"}", obs, sizeof obs);
    printf("  az=%d unlink=%ld obs=%.300s\n", az, g_unlink_count, obs);
    if (az != 0) BAD("photos.organize was not authorized through the gate");
    if (g_unlink_count != 0) BAD("a delete happened via the tool path");
    if (!strstr(obs, "FULFILLED")) BAD("tool did not report FULFILLED");
    if (!strstr(obs, "advisory")) BAD("tool observation not labelled advisory");
    if (!is_dir("/MEDIA/USB/2026-01") || !is_dir("/MEDIA/USB/2026-03")) BAD("tool did not create date folders");
    if (is_file("/MEDIA/USB/A.JPG") || is_file("/MEDIA/USB/B.PNG")) BAD("tool did not move the photos");
    if (!is_file("/MEDIA/USB/DOC.TXT")) BAD("tool touched a non-image file");
    OK("chatbot photos.organize tool ran the contract end-to-end through the gate");
    return 0;
}

// ---------------------------------------------------------------------------
// GATE: mid-contract, prove the scoped grant + no-delete + out-of-scope refusal.
// ---------------------------------------------------------------------------
static int sc_guards(void) {
    ensure_config();
    put_file("/MEDIA/USB/junk.txt", "j\n");
    put_file("/MEDIA/USB/P.JPG", "p\n");
    escrow_contract_t *c = ad_escrow_request("/MEDIA/USB", "guard test", 3600);
    if (!c) BAD("escrow_request returned no contract");

    // in-scope mkdir + move are ALLOWED by the scoped grant
    if (ad_aiclient_run_action("files.mkdir", "{\"path\":\"/MEDIA/USB/D1\"}", obs, sizeof obs) != 0)
        BAD("in-scope mkdir refused under the grant");
    if (!is_dir("/MEDIA/USB/D1")) BAD("in-scope folder not created");
    if (ad_aiclient_run_action("files.move",
        "{\"src\":\"/MEDIA/USB/junk.txt\",\"dst\":\"/MEDIA/USB/D1/junk.txt\"}", obs, sizeof obs) != 0)
        BAD("in-scope move refused under the grant");

    // fs.delete is REFUSED (no-delete policy) - the file survives, zero unlinks
    ad_aiclient_run_action("files.delete", "{\"path\":\"/MEDIA/USB/P.JPG\"}", obs, sizeof obs);
    if (!strstr(obs, "CAPABILITY_DENIED")) BAD("fs.delete was NOT refused under the no-delete grant");
    if (!is_file("/MEDIA/USB/P.JPG")) BAD("P.JPG was deleted despite the no-delete policy");
    if (!ad_aicap_cap_denied("fs.delete")) BAD("fs.delete not on the denylist while active");

    // out-of-scope mkdir + out-of-scope move ends are REFUSED
    ad_aiclient_run_action("files.mkdir", "{\"path\":\"/HOME/EVIL\"}", obs, sizeof obs);
    if (!strstr(obs, "CAPABILITY_DENIED")) BAD("out-of-scope mkdir was not refused");
    if (is_dir("/HOME/EVIL")) BAD("folder created outside the scoped grant");
    ad_aiclient_run_action("files.move",
        "{\"src\":\"/MEDIA/USB/D1/junk.txt\",\"dst\":\"/HOME/junk.txt\"}", obs, sizeof obs);
    if (!strstr(obs, "CAPABILITY_DENIED")) BAD("out-of-scope destination move was not refused");
    if (is_file("/HOME/junk.txt")) BAD("file relocated outside the scoped grant");
    ad_aiclient_run_action("files.move",
        "{\"src\":\"/OUTSIDE/x.txt\",\"dst\":\"/MEDIA/USB/x.txt\"}", obs, sizeof obs);
    if (!strstr(obs, "CAPABILITY_DENIED")) BAD("out-of-scope source move was not refused");

    if (g_unlink_count != 0) BAD("a delete happened during the guard checks");
    ad_escrow_close(c);
    OK("scoped grant enforced: in-scope allowed, delete + out-of-scope refused, zero deletes");
    return 0;
}

// ---------------------------------------------------------------------------
// NEGATIVE: a promised move that does not happen -> PARTIAL, NO early close.
// ---------------------------------------------------------------------------
static int sc_partial(void) {
    ensure_config();
    put_photo("/MEDIA/USB/FAILMOVE_A.JPG", "fa\n", MT_JAN);  // rename() forced to fail
    put_photo("/MEDIA/USB/OK_B.JPG", "ob\n", MT_JAN);
    put_file ("/MEDIA/USB/probe.txt", "pr\n");               // for the post-close probe

    int verdict = ad_photorg_organize("/MEDIA/USB", "YYYY-MM", summary, sizeof summary);
    printf("  verdict=%d unlink=%ld\n  summary:\n%s\n", verdict, g_unlink_count, summary);

    if (verdict != ESCROW_PARTIAL) BAD("an unfulfilled promise did not verify PARTIAL");
    if (g_unlink_count != 0) BAD("a delete happened during a partial organise");
    if (!is_file("/MEDIA/USB/FAILMOVE_A.JPG")) BAD("the un-movable photo was lost");
    if (is_file("/MEDIA/USB/OK_B.JPG")) BAD("the movable photo was not moved");
    if (!dir_has_content("/MEDIA/USB/2026-01", "ob\n")) BAD("OK_B bytes not in the date folder");

    // NOT early-closed: the grant is retained, so an in-scope move still works.
    int az = ad_aiclient_run_action("files.move",
        "{\"src\":\"/MEDIA/USB/probe.txt\",\"dst\":\"/MEDIA/USB/2026-01/probe.txt\"}", obs, sizeof obs);
    if (az != 0) BAD("grant was revoked on a PARTIAL close (should stay open until TTL)");
    // and delete is STILL forbidden (policy retained on a partial close)
    ad_aiclient_run_action("files.delete", "{\"path\":\"/MEDIA/USB/FAILMOVE_A.JPG\"}", obs, sizeof obs);
    if (!strstr(obs, "CAPABILITY_DENIED")) BAD("delete allowed after a partial close");
    if (!ad_aicap_cap_denied("fs.delete")) BAD("no-delete policy not retained after a partial close");
    if (g_unlink_count != 0) BAD("a delete slipped through on the partial path");
    OK("PARTIAL verdict, grant retained (no early close), delete still forbidden, zero deletes");
    return 0;
}

// ---------------------------------------------------------------------------
// DENY CONTROL: even a consent that WOULD allow delete is overridden by the
// escrow no-delete policy. With the #712 denylist neutered this LEAKS (the
// negative control in run_escrow.sh relies on that).
// ---------------------------------------------------------------------------
static int sc_deny_ctrl(void) {
    ensure_config();
    put_file("/MEDIA/USB/P.JPG", "p\n");
    seed_consent("fs.delete=session");          // a consent that WOULD allow delete
    escrow_contract_t *c = ad_escrow_request("/MEDIA/USB", "deny ctrl", 3600);
    if (!c) BAD("escrow_request returned no contract");
    ad_aiclient_run_action("files.delete", "{\"path\":\"/MEDIA/USB/P.JPG\"}", obs, sizeof obs);
    printf("  obs=%.200s unlink=%ld\n", obs, g_unlink_count);
    if (!strstr(obs, "CAPABILITY_DENIED")) BAD("fs.delete allowed despite the no-delete policy overriding consent");
    if (!is_file("/MEDIA/USB/P.JPG")) BAD("P.JPG was deleted");
    if (g_unlink_count != 0) BAD("a delete happened");
    ad_escrow_close(c);
    OK("no-delete escrow policy OVERRIDES a standing delete consent");
    return 0;
}

// ---------------------------------------------------------------------------
// GREEN: #713 EXIF capture-date grouping through the REAL organiser.
// A JPEG whose EXIF DateTimeOriginal (2019-07) DISAGREES with its mtime
// (2026-03) must group by the EXIF date; a JPEG with no EXIF must fall back to
// its mtime (2026-01); a truncated JPEG must fall back to mtime WITHOUT crashing
// (2026-03); a non-image is skipped as before.
// ---------------------------------------------------------------------------
static int sc_exif(void) {
    ensure_config();
    unsigned char jbuf[256];

    // EXIF says July 2019; mtime says March 2026. EXIF must win.
    int n1 = build_exif_jpeg(jbuf, 0 /*II*/, 2019, 7, 4);
    put_photo_bin("/MEDIA/USB/EXIFII.JPG", jbuf, n1, MT_MAR);
    // Same, big-endian EXIF (Dec 2023); mtime March 2026.
    int n2 = build_exif_jpeg(jbuf, 1 /*MM*/, 2023, 12, 25);
    put_photo_bin("/MEDIA/USB/EXIFMM.JPG", jbuf, n2, MT_MAR);
    // No EXIF: a JPEG that is really just text -> fall back to mtime (Jan 2026).
    put_photo("/MEDIA/USB/NOEXIF.JPG", "not really a jpeg, no exif here\n", MT_JAN);
    // Truncated EXIF JPEG: valid SOI+APP1 header then cut mid-TIFF. Must not
    // crash and must fall back to mtime (March 2026).
    int nf = build_exif_jpeg(jbuf, 1, 2019, 7, 4);
    put_photo_bin("/MEDIA/USB/BADEXIF.JPG", jbuf, nf > 20 ? 20 : nf, MT_MAR);
    // Non-image, must be left alone.
    put_file("/MEDIA/USB/NOTES.TXT", "leave me\n");

    int verdict = ad_photorg_organize("/MEDIA/USB", "YYYY-MM", summary, sizeof summary);
    printf("  verdict=%d unlink=%ld\n", verdict, g_unlink_count);

    if (g_unlink_count != 0) BAD("a delete happened during EXIF organise");
    if (verdict != ESCROW_FULFILLED) BAD("EXIF organise did not verify FULFILLED");
    // EXIF date wins over mtime for both byte orders.
    if (!is_file("/MEDIA/USB/2019-07/EXIFII.JPG"))
        BAD("II EXIF photo not grouped by capture date 2019-07");
    if (!is_file("/MEDIA/USB/2023-12/EXIFMM.JPG"))
        BAD("MM EXIF photo not grouped by capture date 2023-12");
    // No EXIF -> mtime fallback (Jan 2026), NOT any EXIF-derived folder.
    if (!is_file("/MEDIA/USB/2026-01/NOEXIF.JPG"))
        BAD("no-EXIF photo did not fall back to mtime 2026-01");
    // Truncated EXIF -> mtime fallback (Mar 2026), no crash.
    if (!is_file("/MEDIA/USB/2026-03/BADEXIF.JPG"))
        BAD("truncated-EXIF photo did not fall back to mtime 2026-03");
    // Prove EXIF really overrode mtime: the EXIF photos are NOT in the mtime folder.
    if (is_file("/MEDIA/USB/2026-03/EXIFII.JPG") || is_file("/MEDIA/USB/2026-03/EXIFMM.JPG"))
        BAD("EXIF photo landed in the mtime folder (EXIF was ignored)");
    // Non-image untouched.
    if (!is_file("/MEDIA/USB/NOTES.TXT")) BAD("non-image NOTES.TXT was touched");
    OK("grouped by EXIF date (II+MM), mtime fallback for no-EXIF and truncated, non-image skipped");
    return 0;
}

int main(int argc, char **argv) {
    const char *root = getenv("AIFS_ROOT");
    if (!root || argc < 2) { fprintf(stderr, "usage: AIFS_ROOT=<dir> %s <scenario>\n", argv[0]); return 2; }
    snprintf(g_root, sizeof g_root, "%s", root);
    const char *s = argv[1];
    if (!strcmp(s, "organize")) return sc_organize();
    if (!strcmp(s, "tool"))     return sc_tool();
    if (!strcmp(s, "guards"))   return sc_guards();
    if (!strcmp(s, "partial"))  return sc_partial();
    if (!strcmp(s, "deny_ctrl")) return sc_deny_ctrl();
    if (!strcmp(s, "exif"))     return sc_exif();
    fprintf(stderr, "unknown scenario %s\n", s); return 2;
}
