// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aieject_test.c - host unit test for the FIRST real AI DEVICE executor (#708):
// device.list + device.block.eject.
//
// The REAL units under test are userland/libc/aiclient.c (exec_device_list /
// exec_device_eject + the aiclient_run_action device routing) and
// userland/libc/aidev.c (the per-device capability manifest gate) and
// userland/libc/aicap.c (the token+consent+audit gate). run_aieject.sh compiles
// them freestanding EXACTLY as userland/libc/Makefile does, then symbol-prefixes
// them with "ad_" so they link next to glibc. This harness backs their file +
// clock syscalls with the host, backs the THREE volume syscalls (SYS_VOL_LIST /
// SYS_VOL_EJECT / SYS_VOL_BUSY) with an in-memory mock removable-device table,
// and backs their libc primitives with host libc. Nothing here reimplements an
// executor or a gate: the logic exercised is the shipped code.
//
// Each scenario runs as a SEPARATE PROCESS (argv[1]) so the aicap token cache and
// the on-disk /CONFIG files start clean per scenario. The virtual filesystem is
// rooted at $AIEJ_ROOT (a fresh mktemp -d per run).
//
// Exit code: 0 = scenario behaved exactly as asserted, 1 = it did not,
// 2 = harness/setup error.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>

// ---- unit under test (ad_-prefixed by run_aieject.sh's objcopy) -------------
int ad_aiclient_run_action(const char *id, const char *args, char *obs, int ocap);

// ---- host root for the virtual /CONFIG tree ---------------------------------
static char g_root[512];
static void mapped(const char *vpath, char *out, int ocap) {
    if (vpath && vpath[0] == '/') snprintf(out, ocap, "%s%s", g_root, vpath);
    else                          snprintf(out, ocap, "%s/%s", g_root, vpath ? vpath : "");
}

// ---------------------------------------------------------------------------
// #aiaudit: aicap.c resolves its audit / metrics sinks through userconf_path()
// instead of a hardcoded "/CONFIG/..." literal, because on the real OS /CONFIG
// is root-owned and no Ring-3 session could ever write there. objcopy prefixes
// aicap.o's UNDEFINED symbols too, so the rig has to supply ad_userconf_*.
//
// The shim resolves to the SAME virtual "/CONFIG/<name>" this harness has
// always mapped, which is exactly what the real resolver yields for a root
// session (userhome_root() is "/"). Real userconf.c is deliberately not linked
// in: it would drag the passwd table and the host's own getuid() into a rig
// whose whole point is a fake root, and what is under test here is the gate,
// not the path join (which has its own coverage).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// #aiaudit: TWO SHIMS THIS RIG HAS BEEN MISSING SINCE dev 45cffc93, which is
// how long all four of these host tests have been RED. MEASURED on dev
// 7b7f0d58: "undefined reference to `ad_getuid'", every arm, "RESULT: BROKEN".
//
// 45cffc93 added getuid() to aicap.c's lost-record reporter; #469 added
// b64_encode() to aiclient.c's vision path. Both are real libc symbols that the
// SHIPPING build resolves, and neither harness supplies them, so the link died
// and the scripts reported BROKEN rather than FAIL. Nothing in the build calls
// these scripts, so nobody saw it. A test that cannot build is not a test that
// passes and is not a test that fails: it is a test nobody is running.
//
// getuid() returns 0 because this rig models a ROOT session: that is the same
// assumption ad_userconf_path() above encodes by resolving to /CONFIG/<name>,
// and the two must agree or the reporter would describe a different session
// from the one the sink belongs to.
// ---------------------------------------------------------------------------
int ad_getuid(void) { return 0; }

// ---------------------------------------------------------------------------
// #aiaudit: the rest of the rig rot, same cause as ad_getuid above. aiclient.c
// grew the #aitools tool-table emitter and the live-contract registry after
// these harnesses were written, and neither symbol was ever added here, so the
// three aiclient-linking scripts have been RED for as long as ad_getuid has.
//
// The contract registry reports NO declared contracts, which is the correct
// state for a rig that installs none: the tools under test are the built-in
// ones. aitools.c itself IS linked in by the run script (it is a pure table),
// so the tool surface exercised is the shipping one, not a stub.
// ---------------------------------------------------------------------------
int ad_contract_declared(const char *app) { (void)app; return 0; }
int ad_contract_declared_list(char *out, int ocap) {
    if (out && ocap > 0) out[0] = 0;
    return 0;
}

// Plain re-exports of host libc, matching the ad_str* shims already here.
char *ad_strchr(const char *s, int c) { return strchr((char *)s, c); }

long ad_b64_encode(const unsigned char *in, long inlen, char *out, long outcap) {
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!in || !out || inlen < 0) return -1;
    long need = ((inlen + 2) / 3) * 4;
    if (outcap < need + 1) return -1;
    long o = 0;
    for (long i = 0; i < inlen; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < inlen) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < inlen) v |= (unsigned)in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < inlen) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < inlen) ? T[v & 63]        : '=';
    }
    out[o] = 0;
    return o;
}

int ad_userconf_path(const char *name, char *out, unsigned long cap) {
    int n = snprintf(out, (size_t)cap, "/CONFIG/%s", name ? name : "");
    return (n > 0 && (unsigned long)n < cap) ? 0 : -1;
}
void ad_userconf_ensure_parent_dir(const char *path) {
    char hp[700]; mapped(path, hp, (int)sizeof(hp));
    char *last = 0;
    for (char *s = hp; *s; s++) if (*s == '/') last = s;
    if (last && last != hp) { *last = 0; mkdir(hp, 0755); }
}
static void host_mkparents(const char *hostpath) {
    char tmp[600]; snprintf(tmp, sizeof(tmp), "%s", hostpath);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
}

// #deadtests: the ENVIRONMENT stubs these units need, in ONE place.
// See ai_env_stubs.h for why four tests could not link before this.
#define AIES_NO_PRINTF 1   // this harness already defines ad_printf
#include "ai_env_stubs.h"

// ---- maytera syscall numbers (from syscall.h) -------------------------------
#define SYS_OPEN 10
#define SYS_CLOSE 11
#define SYS_READ 12
#define SYS_WRITE 13
#define SYS_SEEK 14
#define SYS_TIME 50
#define SYS_VOL_LIST  283
#define SYS_VOL_EJECT 284
#define SYS_VOL_BUSY  285
#define M_O_CREAT 0x0040

// ---- sc_volume_t: MUST match userland/libc/syscall.h + kernel/proc/syscall.h -
typedef struct {
    int32_t  index;
    uint32_t flags;
    uint32_t fs_type;
    uint32_t pad;
    uint64_t total_bytes;
    uint64_t free_bytes;
    char     name[64];
    char     mount[32];
    char     fsname[8];
} sc_volume_t;
#define MOSVOL_MOUNTED   0x01
#define MOSVOL_REMOVABLE 0x02

// ---- mock removable-device table --------------------------------------------
typedef struct { sc_volume_t v; int busy; int ejected; } mockvol_t;
static mockvol_t g_mock[8];
static int g_nmock = 0;
static long g_eject_calls = 0;   // every SYS_VOL_EJECT the UUT issues is counted

static void mock_add(int index, uint32_t flags, const char *name,
                     const char *mount, const char *fs, int busy) {
    mockvol_t *m = &g_mock[g_nmock++];
    memset(m, 0, sizeof(*m));
    m->v.index = index;
    m->v.flags = flags;
    m->v.total_bytes = 64u * 1024 * 1024;
    snprintf(m->v.name, sizeof(m->v.name), "%s", name);
    snprintf(m->v.mount, sizeof(m->v.mount), "%s", mount);
    snprintf(m->v.fsname, sizeof(m->v.fsname), "%s", fs);
    m->busy = busy;
    m->ejected = 0;
}
static int mock_is_ejected(const char *mount) {
    for (int i = 0; i < g_nmock; i++)
        if (!strcmp(g_mock[i].v.mount, mount)) return g_mock[i].ejected;
    return -1;
}

// ---- the ad_syscallN the UUT bottoms out in ---------------------------------
long ad_syscall0(long n) { if (n == SYS_TIME) return (long)time(NULL); return 0; }
long ad_syscall1(long n, long a) {
    if (n == SYS_CLOSE) return close((int)a);
    if (n == SYS_VOL_EJECT) {
        int idx = (int)a;
        for (int i = 0; i < g_nmock; i++)
            if (g_mock[i].v.index == idx && !g_mock[i].ejected) {
                g_mock[i].ejected = 1; g_eject_calls++; return 0;
            }
        return -1;
    }
    if (n == SYS_VOL_BUSY) {
        int idx = (int)a;
        for (int i = 0; i < g_nmock; i++)
            if (g_mock[i].v.index == idx) return g_mock[i].busy;
        return 0;
    }
    return 0;
}
long ad_syscall2(long n, long a, long b) {
    if (n == SYS_OPEN) {
        char hp[600]; mapped((const char *)a, hp, sizeof hp);
        if (((int)b) & M_O_CREAT) host_mkparents(hp);
        return open(hp, (int)b, 0644);
    }
    if (n == SYS_VOL_LIST) {
        sc_volume_t *dst = (sc_volume_t *)a;
        int max = (int)b, w = 0;
        for (int i = 0; i < g_nmock && w < max; i++)
            if (!g_mock[i].ejected) dst[w++] = g_mock[i].v;
        return w;
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
int   ad_strcasecmp(const char *a, const char *b) { return strcasecmp(a,b); }
size_t ad_strlen(const char *s) { return strlen(s); }
char *ad_strstr(const char *h, const char *n) { return strstr((char*)h,n); }
char *ad_strrchr(const char *s, int c) { return strrchr((char*)s,c); }
size_t ad_strlcpy(char *d, const char *s, size_t n) {
    size_t sl = strlen(s);
    if (n) { size_t cc = sl < n-1 ? sl : n-1; memcpy(d,s,cc); d[cc]=0; }
    return sl;
}

// ---- ad_ stubs for aiclient.c symbols NOT on the device path -----------------
// (aidev_authorize_tool is REAL here - linked from aidev.c - so it is NOT stubbed.)
int  ad_contract_invoke(const char *a, const char *b, char *c, int d) { (void)a;(void)b;(void)c;(void)d; return -1; }
int  ad_contract_invoke_live(const char *a, const char *b, const char *c, char *d, int e) { (void)a;(void)b;(void)c;(void)d;(void)e; return -1; }
int  ad___spawn_with_env(const char *a, char *const b[], char *const c[]) { (void)a;(void)b;(void)c; return -1; }
int  ad_userconf_open_read(const char *a) { (void)a; return -1; }
const char *ad_userhome_path(void) { return "/HOME"; }
int  ad_photorg_organize(const char *a, const char *b, char *c, int d) { (void)a;(void)b; if(c&&d)c[0]=0; return -1; }

// ---- helpers -----------------------------------------------------------------
static void ensure_config(void) { char hp[600]; mapped("/CONFIG/x", hp, sizeof hp); host_mkparents(hp); }
static void put_file(const char *vpath, const char *data) {
    char hp[600]; mapped(vpath, hp, sizeof hp); host_mkparents(hp);
    FILE *f = fopen(hp, "w"); if (!f) { perror("put"); exit(2); }
    fputs(data, f); fclose(f);
}
// eject=allow, format+partition=forbid, everything else class default char.
static void devcap_eject_allow(void) {
    // class|dev_id|read|write|interrupt|enable|disable|format|partition|eject|config
    put_file("/CONFIG/AIDEVCAP.CFG", "block|*|a|c|f|c|c|f|f|a|c\n");
}
static void preseed_consent(const char *line) { put_file("/CONFIG/AICONSENT.CFG", line); }
static int read_all(const char *vpath, char *buf, int cap) {
    char hp[600]; mapped(vpath, hp, sizeof hp);
    FILE *f = fopen(hp, "r"); if (!f) return -1;
    int n = (int)fread(buf, 1, cap-1, f); buf[n>=0?n:0]=0; fclose(f); return n;
}
static int audit_nonempty(void) { char b[64]; return read_all("/CONFIG/AIAUDIT.LOG", b, sizeof b) > 0; }

#define OK(msg) do{ printf("  ok: %s\n", msg); }while(0)
#define BAD(msg) do{ printf("  FAIL: %s\n", msg); return 1; }while(0)

static char obs[4096];

// A removable stick /USB0 and a NON-removable system volume /SYS0 (which vol_list
// would never actually surface, but we plant it to prove the executor's own
// MOSVOL_REMOVABLE guard refuses it independently).
static void seed_two_volumes(int usb_busy) {
    mock_add(0, MOSVOL_MOUNTED | MOSVOL_REMOVABLE, "SanDisk Cruzer Blade", "/USB0", "FAT16", usb_busy);
    mock_add(9, MOSVOL_MOUNTED,                    "Internal System Disk", "/SYS0", "ext2",  0);
}

// 1. ALLOW (manifest override eject=a): eject flushes+unmounts+stops the stick.
static int sc_eject_allow(void) {
    ensure_config(); devcap_eject_allow(); seed_two_volumes(0);
    int az = ad_aiclient_run_action("device.block.eject", "{\"mount\":\"/USB0\"}", obs, sizeof obs);
    printf("  obs=%s az=%d eject_calls=%ld\n", obs, az, g_eject_calls);
    if (az != 0) BAD("eject was not ALLOWED by the manifest");
    if (!strstr(obs, "\"status\":\"ejected\"")) BAD("obs did not report ejected");
    if (mock_is_ejected("/USB0") != 1) BAD("the stick was not actually ejected");
    if (g_eject_calls != 1) BAD("SYS_VOL_EJECT was not called exactly once");
    if (!audit_nonempty()) BAD("no audit line written");
    OK("manifest-ALLOW eject flushed+unmounted+stopped the removable stick, audited"); return 0;
}
// 2. CONSENT -> executor: default eject=CONSENT, preseed grants once, eject runs.
static int sc_eject_consent(void) {
    ensure_config(); seed_two_volumes(0); preseed_consent("device.block.eject=once\n");
    int az = ad_aiclient_run_action("device.block.eject", "{\"mount\":\"/USB0\"}", obs, sizeof obs);
    printf("  obs=%s az=%d eject_calls=%ld\n", obs, az, g_eject_calls);
    if (az != 0) BAD("eject was not ALLOWED after a consent grant");
    if (!strstr(obs, "\"status\":\"ejected\"")) BAD("obs did not report ejected after consent");
    if (mock_is_ejected("/USB0") != 1) BAD("the stick was not ejected after consent");
    OK("consent grant flows through to the safe-eject executor"); return 0;
}
// 3. CONSENT default, NO grant: refused cleanly, nothing ejected.
static int sc_eject_consent_deny(void) {
    ensure_config(); seed_two_volumes(0);   // no override, no preseed => deny
    int az = ad_aiclient_run_action("device.block.eject", "{\"mount\":\"/USB0\"}", obs, sizeof obs);
    printf("  obs=%s az=%d eject_calls=%ld\n", obs, az, g_eject_calls);
    if (az == 0) BAD("eject was allowed with no consent and no token");
    if (mock_is_ejected("/USB0") == 1) BAD("stick was ejected despite denied consent");
    if (g_eject_calls != 0) BAD("SYS_VOL_EJECT was called on a denied eject");
    OK("no consent => eject refused, device untouched"); return 0;
}
// 4. FORBIDden verb (format) refused by the manifest, no executor, nothing ejected.
static int sc_format_forbid(void) {
    ensure_config(); devcap_eject_allow(); seed_two_volumes(0);   // format=f in the override
    int az = ad_aiclient_run_action("device.block.format", "{\"mount\":\"/USB0\"}", obs, sizeof obs);
    printf("  obs=%s az=%d eject_calls=%ld\n", obs, az, g_eject_calls);
    if (az == 0) BAD("format was NOT refused by the manifest");
    if (!strstr(obs, "device-denied")) BAD("obs did not report a device denial for format");
    if (g_eject_calls != 0) BAD("a device action ran for a forbidden verb");
    if (!audit_nonempty()) BAD("forbidden verb was not audited");
    OK("forbidden verb (format) refused by the manifest, no action, audited"); return 0;
}
// 5. BUSY device refuses cleanly (does NOT force), nothing ejected.
static int sc_eject_busy(void) {
    ensure_config(); devcap_eject_allow(); seed_two_volumes(2 /* 2 open handles on /USB0 */);
    int az = ad_aiclient_run_action("device.block.eject", "{\"mount\":\"/USB0\"}", obs, sizeof obs);
    printf("  obs=%s az=%d eject_calls=%ld\n", obs, az, g_eject_calls);
    if (!strstr(obs, "device-busy")) BAD("busy device was not refused as busy");
    if (!strstr(obs, "\"open_handles\":2")) BAD("busy refusal did not report the open handle count");
    if (mock_is_ejected("/USB0") == 1) BAD("a BUSY device was ejected (forced) - unsafe");
    if (g_eject_calls != 0) BAD("SYS_VOL_EJECT was called on a busy device");
    OK("busy device refused cleanly, not forced"); return 0;
}
// 6. Not found: eject of a name/path not present refuses cleanly, no-op.
static int sc_eject_notfound(void) {
    ensure_config(); devcap_eject_allow(); seed_two_volumes(0);
    int az = ad_aiclient_run_action("device.block.eject", "{\"mount\":\"/NOPE\"}", obs, sizeof obs);
    printf("  obs=%s az=%d eject_calls=%ld\n", obs, az, g_eject_calls);
    (void)az;
    if (!strstr(obs, "device-not-found")) BAD("missing device was not reported not-found");
    if (g_eject_calls != 0) BAD("SYS_VOL_EJECT was called for a missing device");
    OK("unknown device refused cleanly, no-op"); return 0;
}
// 7. System/boot disk NEVER a target: eject of a non-removable volume is refused.
static int sc_eject_sysdisk(void) {
    ensure_config(); devcap_eject_allow(); seed_two_volumes(0);
    int az = ad_aiclient_run_action("device.block.eject", "{\"mount\":\"/SYS0\"}", obs, sizeof obs);
    printf("  obs=%s az=%d eject_calls=%ld\n", obs, az, g_eject_calls);
    (void)az;
    if (!strstr(obs, "device-not-found")) BAD("non-removable disk was not refused");
    if (mock_is_ejected("/SYS0") == 1) BAD("a NON-REMOVABLE system disk was ejected - unsafe");
    if (g_eject_calls != 0) BAD("SYS_VOL_EJECT was called on a non-removable disk");
    OK("non-removable system disk is never a target"); return 0;
}
// 8. device.list only enumerates REMOVABLE volumes (no system disk), read-only.
static int sc_list(void) {
    ensure_config(); seed_two_volumes(3);
    int az = ad_aiclient_run_action("device.list", "{}", obs, sizeof obs);
    printf("  obs=%s az=%d\n", obs, az);
    if (az != 0) BAD("device.list was not permitted (should be read-only ungated)");
    if (!strstr(obs, "/USB0")) BAD("device.list did not list the removable stick");
    if (strstr(obs, "/SYS0")) BAD("device.list listed a NON-removable disk");
    if (!strstr(obs, "\"busy\":3")) BAD("device.list did not report the busy count");
    if (mock_is_ejected("/USB0") == 1) BAD("device.list ejected something - not read-only");
    OK("device.list enumerates only removable volumes, read-only"); return 0;
}

int main(int argc, char **argv) {
    const char *root = getenv("AIEJ_ROOT");
    if (!root || argc < 2) { fprintf(stderr, "usage: AIEJ_ROOT=<dir> %s <scenario>\n", argv[0]); return 2; }
    snprintf(g_root, sizeof g_root, "%s", root);
    const char *s = argv[1];
    if (!strcmp(s, "eject_allow"))        return sc_eject_allow();
    if (!strcmp(s, "eject_consent"))      return sc_eject_consent();
    if (!strcmp(s, "eject_consent_deny")) return sc_eject_consent_deny();
    if (!strcmp(s, "format_forbid"))      return sc_format_forbid();
    if (!strcmp(s, "eject_busy"))         return sc_eject_busy();
    if (!strcmp(s, "eject_notfound"))     return sc_eject_notfound();
    if (!strcmp(s, "eject_sysdisk"))      return sc_eject_sysdisk();
    if (!strcmp(s, "list"))               return sc_list();
    fprintf(stderr, "unknown scenario %s\n", s); return 2;
}
