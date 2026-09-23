// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aidev_test.c - host unit test for the per-device AI capability manifest
// (owner req #6). The REAL userland units under test, aidev.c and aicap.c, are compiled
// freestanding and symbol-prefixed with "ad_" by run_aidev.sh, then linked next
// to this harness which backs their file and clock syscalls with the host and
// backs their libc primitives with host libc. Nothing here reimplements the
// manifest or the gate: the decision logic is the shipped code.
//
// Each scenario runs as a SEPARATE PROCESS (pick it with argv[1]) so the
// in-memory aicap token cache and the on-disk CONFIG files start clean and one
// scenario cannot mask another. run_aidev.sh drives the scenarios and asserts
// each exit code, RED (must deny) and GREEN (must allow) both.
//
// Exit code: 0 = the scenario behaved exactly as asserted, 1 = it did not,
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

// ---- the units under test (ad_-prefixed by run_aidev.sh's objcopy) ----------
enum { UT_ALLOW = 0, UT_DENY = 1 };            // mirrors AIDEV_DECISION_*
enum { UT_V_READ=0,UT_V_WRITE,UT_V_INT,UT_V_EN,UT_V_DIS,UT_V_FMT,UT_V_PART,UT_V_EJ,UT_V_CFG };
int  ad_aidev_verb_policy(const char *cls, const char *dev, int verb);
int  ad_aidev_authorize(const char *cls, const char *dev, int verb, char *reason, int rcap);
int  ad_aidev_authorize_tool(const char *tool, const char *args, char *obs, int ocap);
int  ad_aidev_verb_from_name(const char *name);
const char *ad_aidev_verb_name(int verb);
void ad_aicap_init(void);

// ---- host root for the virtual /CONFIG tree ---------------------------------
static char g_root[512];
static void mapped(const char *vpath, char *out, int ocap) {
    if (vpath[0] == '/') snprintf(out, ocap, "%s%s", g_root, vpath);
    else                 snprintf(out, ocap, "%s/%s", g_root, vpath);
}

// ---- syscall backing the ad_-prefixed code calls (numbers from syscall.h) ---
#define SYS_OPEN 10
#define SYS_CLOSE 11
#define SYS_READ 12
#define SYS_WRITE 13
#define SYS_SEEK 14
#define SYS_TIME 50
#define SYS_GET_RTC_TIME 142
#define SYS_GET_RTC_DATE 143
#define SYS_UPTIME_MS 252

long ad_syscall0(long n) {
    switch (n) {
        case SYS_TIME:         return (long)time(NULL);
        case SYS_GET_RTC_TIME: return (12L << 16) | (0L << 8) | 0L;   // 12:00:00
        case SYS_GET_RTC_DATE: return (2026L << 16) | (9L << 8) | 16L; // 2026-09-16
        case SYS_UPTIME_MS:    return 1000;
        default:               return 0;
    }
}
long ad_syscall1(long n, long a) {
    if (n == SYS_CLOSE) return close((int)a);
    return 0;
}
long ad_syscall2(long n, long a, long b) {
    if (n == SYS_OPEN) {
        char hp[600]; mapped((const char *)a, hp, sizeof(hp));
        int fd = open(hp, (int)b, 0644);        // maytera O_ flags match Linux
        return fd;
    }
    return 0;
}
long ad_syscall3(long n, long a, long b, long c) {
    switch (n) {
        case SYS_READ:  return read((int)a, (void *)b, (size_t)c);
        case SYS_WRITE: return write((int)a, (const void *)b, (size_t)c);
        case SYS_SEEK:  return lseek((int)a, (off_t)b, (int)c);
        default:        return 0;
    }
}
long ad_syscall4(long n, long a, long b, long c, long d){(void)n;(void)a;(void)b;(void)c;(void)d;return 0;}
long ad_syscall5(long n,long a,long b,long c,long d,long e){(void)n;(void)a;(void)b;(void)c;(void)d;(void)e;return 0;}
long ad_syscall6(long n,long a,long b,long c,long d,long e,long f){(void)n;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0;}

// ---- libc primitives the ad_-prefixed code calls (backed by host libc) ------
unsigned long ad_strlcpy(char *d, const char *s, unsigned long sz) {
    unsigned long n = 0; if (sz) { while (n < sz - 1 && s[n]) { d[n] = s[n]; n++; } d[n] = 0; }
    unsigned long l = n; while (s[l]) l++; return l;
}
unsigned long ad_strlcat(char *d, const char *s, unsigned long sz) {
    unsigned long dl = 0; while (dl < sz && d[dl]) dl++;
    unsigned long i = 0; while (dl + i + 1 < sz && s[i]) { d[dl + i] = s[i]; i++; } if (dl < sz) d[dl + i] = 0;
    unsigned long sl = 0; while (s[sl]) sl++; return dl + sl;
}
void  *ad_memcpy(void *d, const void *s, unsigned long n){return memcpy(d,s,n);}
void  *ad_memmove(void *d, const void *s, unsigned long n){return memmove(d,s,n);}
void  *ad_memset(void *d, int c, unsigned long n){return memset(d,c,n);}
unsigned long ad_strlen(const char *s){return strlen(s);}
int    ad_strcmp(const char *a, const char *b){return strcmp(a,b);}
int    ad_strncmp(const char *a, const char *b, unsigned long n){return strncmp(a,b,n);}
char  *ad_strstr(const char *h, const char *n){return strstr((char*)h,n);}
char  *ad_strchr(const char *s, int c){return strchr((char*)s,c);}
int    ad_snprintf(char *b, unsigned long sz, const char *f, ...) {
    va_list ap; va_start(ap, f); int r = vsnprintf(b, sz, f, ap); va_end(ap); return r;
}
int    ad_vsnprintf(char *b, unsigned long sz, const char *f, va_list ap){return vsnprintf(b,sz,f,ap);}

// ---- helpers ----------------------------------------------------------------
static void wipe_config(void) {
    char c[600]; snprintf(c, sizeof(c), "%s/CONFIG", g_root);
    // best-effort clean of the three files we touch
    const char *fs[] = {"AICAPS.CFG","AICONSENT.CFG","AIAUDIT.LOG","AIDEVCAP.CFG",0};
    for (int i = 0; fs[i]; i++) { char p[700]; snprintf(p,sizeof(p),"%s/%s",c,fs[i]); unlink(p); }
    mkdir(c, 0755);
}
static void put_file(const char *vpath, const char *contents) {
    char hp[700]; mapped(vpath, hp, sizeof(hp));
    int fd = open(hp, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "setup: cannot write %s: %s\n", hp, strerror(errno)); exit(2); }
    if (contents && *contents) { ssize_t r = write(fd, contents, strlen(contents)); (void)r; }
    close(fd);
}
static int audit_has(const char *needle) {
    char hp[700]; mapped("/CONFIG/AIAUDIT.LOG", hp, sizeof(hp));
    FILE *f = fopen(hp, "r"); if (!f) return 0;
    static char buf[65536]; size_t n = fread(buf, 1, sizeof(buf)-1, f); fclose(f); buf[n] = 0;
    return strstr(buf, needle) != NULL;
}
static int check(const char *label, int got, int want) {
    int ok = (got == want);
    printf("  [%s] %s: got=%d want=%d\n", ok ? "PASS" : "FAIL", label, got, want);
    return ok;
}

int main(int argc, char **argv) {
    const char *root = getenv("AIDEV_ROOT");
    if (!root) { fprintf(stderr, "AIDEV_ROOT unset\n"); return 2; }
    snprintf(g_root, sizeof(g_root), "%s", root);
    int sc = (argc >= 2) ? atoi(argv[1]) : 0;
    wipe_config();

    char reason[256], obs[512];
    int all = 1;

    switch (sc) {
    case 1: // block/read is ALLOW: permitted without consent, audited manifest-allow.
        printf("Scenario 1: block/read (class default ALLOW)\n");
        all &= check("policy", ad_aidev_verb_policy("block","*",UT_V_READ), 2 /*ALLOW*/);
        all &= check("authorize", ad_aidev_authorize("block","disk0",UT_V_READ,reason,sizeof(reason)), UT_ALLOW);
        all &= check("audited-allow", audit_has("manifest-allow"), 1);
        break;

    case 2: // block/format is CONSENT; consent DENY -> denied.
        printf("Scenario 2: block/format CONSENT, user DENIES\n");
        put_file("/CONFIG/AICONSENT.CFG", "device.block.format=deny\n");
        all &= check("policy", ad_aidev_verb_policy("block","*",UT_V_FMT), 1 /*CONSENT*/);
        all &= check("authorize-denied", ad_aidev_authorize("block","disk0",UT_V_FMT,reason,sizeof(reason)), UT_DENY);
        all &= check("audited-denied", audit_has("denied"), 1);
        break;

    case 3: // block/format is CONSENT; consent ONCE -> allowed.
        printf("Scenario 3: block/format CONSENT, user allows ONCE\n");
        put_file("/CONFIG/AICONSENT.CFG", "device.block.format=once\n");
        all &= check("authorize-allowed", ad_aidev_authorize("block","disk0",UT_V_FMT,reason,sizeof(reason)), UT_ALLOW);
        all &= check("audited-ok", audit_has("|ok|"), 1);
        break;

    case 4: // input/write is FORBID; DENY even with a permissive preseed present.
            // Proves the manifest is consulted BEFORE consent: a "once" grant for
            // the verb does not matter because the verb never reaches consent.
        printf("Scenario 4: input/write FORBID beats a permissive consent preseed\n");
        put_file("/CONFIG/AICONSENT.CFG", "device.input.write=once\n");
        all &= check("policy", ad_aidev_verb_policy("input","*",UT_V_WRITE), 0 /*FORBID*/);
        all &= check("authorize-denied", ad_aidev_authorize("input","kbd0",UT_V_WRITE,reason,sizeof(reason)), UT_DENY);
        all &= check("audited-manifest-forbid", audit_has("manifest-forbid"), 1);
        // the preseed line must be UNCONSUMED (consent was never reached)
        { char hp[700]; mapped("/CONFIG/AICONSENT.CFG", hp, sizeof(hp));
          FILE *f=fopen(hp,"r"); char b[256]={0}; if(f){ size_t n=fread(b,1,sizeof(b)-1,f); fclose(f); b[n]=0; }
          all &= check("preseed-untouched", strstr(b,"device.input.write")!=NULL, 1); }
        break;

    case 5: // Per-device override TIGHTENS: forbid format class-wide via the file.
            // format was CONSENT by default; the override makes it FORBID, and
            // that beats a "once" preseed.
        printf("Scenario 5: AIDEVCAP.CFG override forbids block/format (tightens)\n");
        put_file("/CONFIG/AIDEVCAP.CFG", "block|*|-|-|-|-|-|f|-|-|-\n");
        put_file("/CONFIG/AICONSENT.CFG", "device.block.format=once\n");
        all &= check("policy", ad_aidev_verb_policy("block","*",UT_V_FMT), 0 /*FORBID*/);
        all &= check("authorize-denied", ad_aidev_authorize("block","disk0",UT_V_FMT,reason,sizeof(reason)), UT_DENY);
        all &= check("audited-manifest-forbid", audit_has("manifest-forbid"), 1);
        break;

    case 6: // Per-device override RELAXES: allow format for one specific device,
            // and the exact-device record beats the class-wide record.
        printf("Scenario 6: AIDEVCAP.CFG per-device override relaxes format to ALLOW\n");
        put_file("/CONFIG/AIDEVCAP.CFG",
                 "block|*|-|-|-|-|-|c|-|-|-\n"
                 "block|USB:0781:5583|-|-|-|-|-|a|-|-|-\n");
        all &= check("class-wide-still-consent", ad_aidev_verb_policy("block","other",UT_V_FMT), 1 /*CONSENT*/);
        all &= check("device-relaxed-allow", ad_aidev_verb_policy("block","USB:0781:5583",UT_V_FMT), 2 /*ALLOW*/);
        all &= check("authorize-allowed-no-consent",
                     ad_aidev_authorize("block","USB:0781:5583",UT_V_FMT,reason,sizeof(reason)), UT_ALLOW);
        break;

    case 7: // Tool-id adoption path (what aiclient.c calls). RED and GREEN.
        printf("Scenario 7: device.* tool-id path (aiclient adoption)\n");
        put_file("/CONFIG/AICONSENT.CFG", "device.block.partition=deny\n");
        // forbidden verb on a forbidden class: denied, obs says device-denied
        all &= check("input-format-denied",
                     ad_aidev_authorize_tool("device.input.format", "{}", obs, sizeof(obs)), 1 /*AICAP_DENIED*/);
        all &= check("obs-device-denied", strstr(obs,"device-denied")!=NULL, 1);
        // consent verb, user denies via preseed: denied
        all &= check("block-partition-denied",
                     ad_aidev_authorize_tool("device.block.partition", "{\"device\":\"USB:0781:5583\"}", obs, sizeof(obs)), 1);
        // unknown class fails closed (all FORBID)
        all &= check("unknown-class-denied",
                     ad_aidev_authorize_tool("device.wombat.read", "{}", obs, sizeof(obs)), 1);
        // allowed verb: block/read is ALLOW -> AICAP_ALLOW(0)
        all &= check("block-read-allowed",
                     ad_aidev_authorize_tool("device.block.read", "{\"device\":\"disk0\"}", obs, sizeof(obs)), 0 /*AICAP_ALLOW*/);
        break;

    case 8: // Verb name table round-trips and rejects garbage.
        printf("Scenario 8: verb name mapping\n");
        all &= check("format-idx", ad_aidev_verb_from_name("format"), UT_V_FMT);
        all &= check("partition-idx", ad_aidev_verb_from_name("partition"), UT_V_PART);
        all &= check("garbage-rejected", ad_aidev_verb_from_name("frobnicate"), -1);
        all &= check("name-of-format", strcmp(ad_aidev_verb_name(UT_V_FMT),"format")==0, 1);
        break;

    default:
        fprintf(stderr, "unknown scenario %d\n", sc);
        return 2;
    }

    printf("Scenario %d: %s\n", sc, all ? "OK" : "FAILED");
    return all ? 0 : 1;
}
