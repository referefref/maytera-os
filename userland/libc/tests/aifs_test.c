// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aifs_test.c - host unit test for the files.mkdir / files.move AI tools (#711).
//
// The REAL units under test are userland/libc/aiclient.c (the ReAct executors
// exec_fs_mkdir / exec_fs_move + the aiclient_run_action gate/dispatch/audit
// path) and userland/libc/aicap.c (the capability gate + the both-ends
// aicap_path_in_scope predicate). run_aifs.sh compiles them freestanding EXACTLY
// as userland/libc/Makefile does, then symbol-prefixes them with "ad_" so they
// link next to glibc. This harness backs their file + clock syscalls with the
// host and their libc primitives with host libc. Nothing here reimplements an
// executor or the gate: the logic exercised is the shipped code.
//
// Each scenario runs as a SEPARATE PROCESS (argv[1]) so the in-memory aicap
// token cache and the on-disk /CONFIG files start clean per scenario. The
// virtual filesystem is rooted at $AIFS_ROOT (a fresh mktemp -d per run).
//
// Exit code: 0 = scenario behaved exactly as asserted, 1 = it did not,
// 2 = harness/setup error.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

// ---- unit under test (ad_-prefixed by run_aifs.sh's objcopy) ----------------
int ad_aiclient_run_action(const char *id, const char *args, char *obs, int ocap);

// ---- host root for the virtual /HOME, /WORK, /CONFIG, ... tree ---------------
static char g_root[512];
static long g_unlink_count = 0;   // every SYS_UNLINK the UUT issues is counted

static void mapped(const char *vpath, char *out, int ocap) {
    if (vpath && vpath[0] == '/') snprintf(out, ocap, "%s%s", g_root, vpath);
    else                          snprintf(out, ocap, "%s/%s", g_root, vpath ? vpath : "");
}
// mkdir -p the PARENT directories of a mapped host path.
static void host_mkparents(const char *hostpath) {
    char tmp[600]; snprintf(tmp, sizeof(tmp), "%s", hostpath);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
}

// ---- maytera syscall numbers (from syscall.h) -------------------------------
#define SYS_OPEN 10
#define SYS_CLOSE 11
#define SYS_READ 12
#define SYS_WRITE 13
#define SYS_SEEK 14
#define SYS_MKDIR 16
#define SYS_UNLINK 18
#define SYS_TIME 50
#define SYS_RENAME 70
#define M_O_CREAT 0x0040   // maytera O_CREAT == Linux O_CREAT

// ---- the ad_syscallN the UUT bottoms out in, backed by the host FS ----------
long ad_syscall0(long n) { if (n == SYS_TIME) return (long)time(NULL); return 0; }
long ad_syscall1(long n, long a) {
    if (n == SYS_CLOSE) return close((int)a);
    if (n == SYS_UNLINK) {
        char hp[600]; mapped((const char *)a, hp, sizeof hp);
        g_unlink_count++;                       // record EVERY delete the UUT makes
        return unlink(hp);
    }
    return 0;
}
long ad_syscall2(long n, long a, long b) {
    if (n == SYS_OPEN) {
        char hp[600]; mapped((const char *)a, hp, sizeof hp);
        if (((int)b) & M_O_CREAT) host_mkparents(hp);
        return open(hp, (int)b, 0644);          // maytera O_ flags match Linux
    }
    if (n == SYS_MKDIR) {
        char hp[600]; mapped((const char *)a, hp, sizeof hp);
        host_mkparents(hp);
        return mkdir(hp, (int)b);
    }
    if (n == SYS_RENAME) {
        char op[600], np[600];
        mapped((const char *)a, op, sizeof op);
        mapped((const char *)b, np, sizeof np);
        host_mkparents(np);
        return rename(op, np);                  // RELOCATE - no delete counted
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
size_t ad_strlcpy(char *d, const char *s, size_t n) {   // glibc lacks strlcpy
    size_t sl = strlen(s);
    if (n) { size_t c = sl < n-1 ? sl : n-1; memcpy(d,s,c); d[c]=0; }
    return sl;
}

// ---- ad_ stubs for aiclient.c symbols NOT on the tested path -----------------
// (device/guard/contract/spawn/userconf tool arms; never reached by files.*)
int  ad_aidev_authorize_tool(const char *t, const char *a, char *o, int c) { (void)t;(void)a; if(o&&c) o[0]=0; return 1; }
int  ad_aiguard_check(const char *a, int b, char *c, int d) { (void)a;(void)b;(void)c;(void)d; return 0; }
const char *ad_aiguard_sev_name(int s) { (void)s; return "none"; }
int  ad_contract_invoke(const char *a, const char *b, char *c, int d) { (void)a;(void)b;(void)c;(void)d; return -1; }
int  ad_contract_invoke_live(const char *a, const char *b, const char *c, char *d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; return -1; }
int  ad___spawn_with_env(const char *a, char *const b[], char *const c[]) { (void)a;(void)b;(void)c; return -1; }
int  ad_userconf_open_read(const char *a) { (void)a; return -1; }
const char *ad_userhome_path(void) { return "/HOME"; }
// #712: aiclient.c now dispatches photos.organize -> photorg_organize; this test
// does not exercise it, so stub it (the escrow end-to-end test exercises it live).
int  ad_photorg_organize(const char *a, const char *b, char *c, int d) { (void)a;(void)b; if(c&&d)c[0]=0; return -1; }

// ---- helpers -----------------------------------------------------------------
static void ensure_config(void) { char hp[600]; mapped("/CONFIG/x", hp, sizeof hp); host_mkparents(hp); }
static void seed_token(const char *cap, const char *allowed) {
    char hp[600]; mapped("/CONFIG/AICAPS.CFG", hp, sizeof hp); host_mkparents(hp);
    FILE *f = fopen(hp, "w");
    if (!f) { perror("seed"); exit(2); }
    // id|cap|risk|expires_at|max_uses|uses|allowed_paths|denied_commands|persist|tag
    fprintf(f, "cap_test|%s|1|0|-1|0|%s||0|hosttest\n", cap, allowed);
    fclose(f);
}
static void put_file(const char *vpath, const char *data) {
    char hp[600]; mapped(vpath, hp, sizeof hp); host_mkparents(hp);
    FILE *f = fopen(hp, "w"); if (!f) { perror("put"); exit(2); }
    fputs(data, f); fclose(f);
}
static void host_mkdir(const char *vpath) {
    char hp[600]; mapped(vpath, hp, sizeof hp); host_mkparents(hp); mkdir(hp, 0755);
}
static int is_dir(const char *vpath) {
    char hp[600]; mapped(vpath, hp, sizeof hp); struct stat st;
    return stat(hp, &st) == 0 && S_ISDIR(st.st_mode);
}
static int is_file(const char *vpath) {
    char hp[600]; mapped(vpath, hp, sizeof hp); struct stat st;
    return stat(hp, &st) == 0 && S_ISREG(st.st_mode);
}
static int read_all(const char *vpath, char *buf, int cap) {
    char hp[600]; mapped(vpath, hp, sizeof hp);
    FILE *f = fopen(hp, "r"); if (!f) return -1;
    int n = (int)fread(buf, 1, cap-1, f); buf[n>=0?n:0]=0; fclose(f); return n;
}
static int audit_nonempty(void) {
    char b[64]; return read_all("/CONFIG/AIAUDIT.LOG", b, sizeof b) > 0;
}

#define OK(msg) do{ printf("  ok: %s\n", msg); }while(0)
#define BAD(msg) do{ printf("  FAIL: %s\n", msg); return 1; }while(0)

static const char *DATA = "HELLO-PHOTOS-2026-move-me-intact\n";
static char obs[4096];

// GREEN: a granted mkdir creates the folder, no delete.
static int sc_mkdir_grant(void) {
    ensure_config(); seed_token("fs.mkdir", "/WORK");
    int az = ad_aiclient_run_action("files.mkdir", "{\"path\":\"/WORK/PHOTOS\"}", obs, sizeof obs);
    printf("  obs=%s az=%d unlink=%ld\n", obs, az, g_unlink_count);
    if (az != 0) BAD("mkdir was not ALLOWED by the granted token");
    if (!is_dir("/WORK/PHOTOS")) BAD("/WORK/PHOTOS was not created");
    if (g_unlink_count != 0) BAD("mkdir deleted something (unlink_count != 0)");
    if (!audit_nonempty()) BAD("no audit line written");
    OK("granted mkdir created the folder, zero deletes, audited"); return 0;
}
// GREEN: a granted move RELOCATES the file bytes-identical, source gone, NO delete.
static int sc_move_grant(void) {
    ensure_config(); put_file("/WORK/a.txt", DATA); host_mkdir("/WORK/PHOTOS");
    seed_token("fs.move", "/WORK");
    int az = ad_aiclient_run_action("files.move",
        "{\"src\":\"/WORK/a.txt\",\"dst\":\"/WORK/PHOTOS/a.txt\"}", obs, sizeof obs);
    printf("  obs=%s az=%d unlink=%ld\n", obs, az, g_unlink_count);
    if (az != 0) BAD("move was not ALLOWED by the granted token");
    if (!strstr(obs, "success")) BAD("move did not report success");
    if (is_file("/WORK/a.txt")) BAD("SOURCE still present - not a real move");
    if (!is_file("/WORK/PHOTOS/a.txt")) BAD("DEST missing - file lost");
    char got[256]; read_all("/WORK/PHOTOS/a.txt", got, sizeof got);
    if (strcmp(got, DATA) != 0) BAD("DEST bytes differ from source");
    if (g_unlink_count != 0) BAD("move DELETED (unlink_count != 0) - it copied+deleted, not renamed");
    OK("granted move relocated bytes-identical, source gone, ZERO deletes"); return 0;
}
// RED: move whose DESTINATION escapes the granted scope is refused, no FS change.
static int sc_move_deny_dst(void) {
    ensure_config(); put_file("/WORK/a.txt", DATA); host_mkdir("/OUTSIDE");
    seed_token("fs.move", "/WORK");
    ad_aiclient_run_action("files.move",
        "{\"src\":\"/WORK/a.txt\",\"dst\":\"/OUTSIDE/a.txt\"}", obs, sizeof obs);
    printf("  obs=%s unlink=%ld\n", obs, g_unlink_count);
    if (!strstr(obs, "CAPABILITY_DENIED")) BAD("out-of-scope DEST was not refused");
    if (!is_file("/WORK/a.txt")) BAD("SOURCE was moved despite refusal");
    if (is_file("/OUTSIDE/a.txt")) BAD("file landed OUTSIDE the granted scope");
    if (g_unlink_count != 0) BAD("a delete happened on a refused move");
    OK("out-of-scope destination refused, no filesystem change"); return 0;
}
// RED: move whose SOURCE escapes the granted scope is denied, no FS change.
static int sc_move_deny_src(void) {
    ensure_config(); put_file("/OUTSIDE/b.txt", DATA); host_mkdir("/OUTSIDE");
    seed_token("fs.move", "/WORK");
    int az = ad_aiclient_run_action("files.move",
        "{\"src\":\"/OUTSIDE/b.txt\",\"dst\":\"/WORK/b.txt\"}", obs, sizeof obs);
    printf("  obs=%s az=%d unlink=%ld\n", obs, az, g_unlink_count);
    if (az == 0) BAD("out-of-scope SOURCE was allowed");
    if (!is_file("/OUTSIDE/b.txt")) BAD("SOURCE disappeared on a denied move");
    if (is_file("/WORK/b.txt")) BAD("file was relocated on a denied move");
    if (g_unlink_count != 0) BAD("a delete happened on a denied move");
    OK("out-of-scope source denied, no filesystem change"); return 0;
}
// RED: mkdir outside the granted scope is denied, folder not created.
static int sc_mkdir_deny(void) {
    ensure_config(); seed_token("fs.mkdir", "/WORK");
    int az = ad_aiclient_run_action("files.mkdir", "{\"path\":\"/SECRET/X\"}", obs, sizeof obs);
    printf("  obs=%s az=%d unlink=%ld\n", obs, az, g_unlink_count);
    if (az == 0) BAD("out-of-scope mkdir was allowed");
    if (is_dir("/SECRET/X")) BAD("folder created outside the granted scope");
    if (g_unlink_count != 0) BAD("a delete happened on a denied mkdir");
    OK("out-of-scope mkdir denied, folder not created"); return 0;
}

int main(int argc, char **argv) {
    const char *root = getenv("AIFS_ROOT");
    if (!root || argc < 2) { fprintf(stderr, "usage: AIFS_ROOT=<dir> %s <scenario>\n", argv[0]); return 2; }
    snprintf(g_root, sizeof g_root, "%s", root);
    const char *s = argv[1];
    if (!strcmp(s, "mkdir_grant"))   return sc_mkdir_grant();
    if (!strcmp(s, "move_grant"))    return sc_move_grant();
    if (!strcmp(s, "move_deny_dst")) return sc_move_deny_dst();
    if (!strcmp(s, "move_deny_src")) return sc_move_deny_src();
    if (!strcmp(s, "mkdir_deny"))    return sc_mkdir_deny();
    fprintf(stderr, "unknown scenario %s\n", s); return 2;
}
