// updated - MayteraOS background auto-update job (#402/#403 scheduled updates).
//
// Launched periodically by the kernel cron scheduler (#265) via a
// "INTERVAL <secs> launch /APPS/UPDATED" line in /CONFIG/CRON.CFG, and once at
// boot. Each run it: reads the installed-app registry (/APPS/STORE.DB), fetches
// the repository manifest from the update server, and for every installed app
// whose available version differs from the installed one, acts per the policy
// in /APPS/STORE.CFG:
//   policy=auto   -> download + APPLY the update (replace the /APPS binary,
//                    re-register in the Start menu) and post a success toast.
//   policy=notify -> post a notification that an update is available.
//   policy=off    -> do nothing.
// This is a headless one-shot: cron owns the schedule, this owns one check.

#include "../../libc/syscall.h"
#include "../../libc/string.h"
#include "../../libc/stdlib.h"
#include "../../libc/stdio.h"
#include <stdarg.h>
#include "../../libc/fcntl.h"
#include "../../libc/unistd.h"
#include "../../libc/notify.h"
#include "arc.h"
#include "../../libc/pkgsig.h"
#include "../../libc/startmenu_reg.h"   // #<startmenu-rust> live Start-menu registration

/* Hostname, not a LAN IP: see the note in appstore/main.c. HTTP is
 * deliberate; integrity is a package-signature property, not a transport one. */
#define REPO_HOST     "updates.maytera.net"

/* #559: detached signature over manifest.json. The manifest is the TRUST
 * ANCHOR (the Debian apt model): this signature authenticates the manifest,
 * and the manifest's per-package sha256 then covers every .mpkg, so one
 * signature protects many artifacts. Verified in the kernel against a public
 * key compiled into the kernel image, so Ring-3 cannot swap the key out.
 * Fail closed at every step: there is no unverified fallback. */

// #<startmenu-rust>: SYS_DESKTOP_MENU_RELOAD used to be called here after a
// background auto-update. It resolves to a documented kernel no-op (see
// userland/apps/appstore/main.c's identical note) - the userland compositor
// has never listened for it. startmenu_register_app() below writes into the
// live config directory the compositor throttle-polls instead.

// Package-manager write to the FAT ESP (userland cannot open /APPS files for
// writing; the kernel does it via fat_write_file). Returns 0 on success.
#ifndef SYS_PKG_WRITE
#define SYS_PKG_WRITE 301
#endif
static inline int pkg_write(const char *path, const void *data, unsigned len) {
    return (int)syscall3(SYS_PKG_WRITE, (long)path, (long)data, (long)len);
}

static char    g_manifest[128 * 1024];
static uint8_t g_dl[3 * 1024 * 1024];
static uint8_t g_sig[1024];   // detached manifest signature (#559)

// ---------------------------------------------------------------------------
// #559: persistent audit log. Cron-launched processes have no stdout that
// reaches the console, so every trust decision is appended here and flushed to
// /APPS/UPD.LOG. This is the record that says WHY an update was or was not
// applied, which is exactly what a silent auto-updater otherwise hides.
// ---------------------------------------------------------------------------
static char g_log[4096];
static int  g_loglen = 0;

static void ulog(const char *fmt, ...) {
    if (g_loglen > (int)sizeof(g_log) - 256) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(g_log + g_loglen, sizeof(g_log) - g_loglen - 2, fmt, ap);
    va_end(ap);
    if (k > 0) {
        g_loglen += k;
        if (g_loglen < (int)sizeof(g_log) - 1) g_log[g_loglen++] = '\n';
        g_log[g_loglen] = 0;
    }
    // Flush every time: if this run dies mid-way, the log must still explain
    // how far it got. Cheap (a few KB) and worth it for an audit trail.
    pkg_write("/APPS/UPD.LOG", g_log, (unsigned)g_loglen);
}

// ---------------------------------------------------------------------------
// #559: repository source. Defaults to the public repo, overridable by a single
// line in /APPS/STORE.SRC (e.g. "http://192.0.2.1:8559"). This is apt's
// sources.list idea: WHERE packages come from is configuration, WHETHER they
// are trusted is decided by the signature over the manifest. A redirected
// client still refuses anything the signing key did not sign, so making the
// source configurable adds no trust surface, and it lets the shipping binary
// be verified against a controlled test repo.
// ---------------------------------------------------------------------------
#define REPO_DEFAULT "http://updates.maytera.net"
static char g_repo[160] = REPO_DEFAULT;

static void repo_load(void) {
    int fd = sys_open("/APPS/STORE.SRC", O_RDONLY);
    if (fd < 0) return;
    char b[192];
    int n = sys_read(fd, b, (int)sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    int i = 0;
    while (b[i] && b[i] != '\n' && b[i] != '\r' && b[i] != ' ') i++;
    b[i] = 0;
    // Only an http:// base is accepted, and it must fit; anything else leaves
    // the compiled-in default in place rather than half-applying.
    if (strncmp(b, "http://", 7) == 0 && i > 7 && i < (int)sizeof(g_repo))
        strcpy(g_repo, b);
}

// Join the repo base and a relative path into `out`, with exactly one slash.
static void repo_url(const char *rel, char *out, int cap) {
    int o = 0;
    for (const char *p = g_repo; *p && o < cap - 1; p++) out[o++] = *p;
    if (o > 0 && out[o - 1] == '/') o--;
    if (o < cap - 1) out[o++] = '/';
    while (*rel == '/') rel++;
    while (*rel && o < cap - 1) out[o++] = *rel++;
    out[o] = 0;
}

enum { POL_OFF = 0, POL_NOTIFY = 1, POL_AUTO = 2 };

static int read_policy(void) {
    int fd = sys_open("/APPS/STORE.CFG", O_RDONLY);
    if (fd < 0) return POL_NOTIFY;           // sensible default
    char buf[256]; int n = sys_read(fd, buf, sizeof(buf) - 1); sys_close(fd);
    if (n <= 0) return POL_NOTIFY; buf[n] = 0;
    char *p = strstr(buf, "policy=");
    if (!p) return POL_NOTIFY; p += 7;
    if (strncmp(p, "auto", 4) == 0) return POL_AUTO;
    if (strncmp(p, "off", 3) == 0) return POL_OFF;
    return POL_NOTIFY;
}

// ---- HTTP GET via the async fetch worker ----------------------------------
static int http_get(const char *url, uint8_t *buf, int cap) {
    int job = http_fetch_start(url);
    if (job < 0) return -1;
    for (int i = 0; i < 600; i++) {
        int status = 0; unsigned int len = 0;
        int st = http_fetch_poll(job, &status, &len);
        if (st < 0) { http_fetch_cancel(job); return -1; }
        if (st == 1) return http_fetch_read(job, (char *)buf, (unsigned)cap);
        if (st == 2) { http_fetch_read(job, (char *)buf, (unsigned)cap); return -1; }
        usleep(50000);
    }
    http_fetch_cancel(job);
    return -1;
}

// ---- tiny JSON helpers (same tolerant scanner as the store) ---------------
static const char *find_key(const char *o, const char *e, const char *k) {
    int kl = strlen(k);
    for (const char *p = o; p + kl + 2 < e; p++)
        if (p[0] == '"' && strncmp(p + 1, k, kl) == 0 && p[1 + kl] == '"') {
            const char *q = p + 1 + kl + 1;
            while (q < e && (*q == ' ' || *q == ':')) q++;
            return q;
        }
    return 0;
}
static void jstr(const char *o, const char *e, const char *k, char *out, int cap) {
    out[0] = 0; const char *v = find_key(o, e, k);
    if (!v || v >= e || *v != '"') return; v++;
    int i = 0; while (v < e && *v != '"' && i < cap - 1) out[i++] = *v++;
    out[i] = 0;
}

// ---- installed registry (/APPS/STORE.DB) ----------------------------------
#define MAXREG 64
static char g_rid[MAXREG][32];
static char g_rver[MAXREG][16];
static int  g_nreg = 0;

static void load_reg(void) {
    g_nreg = 0;
    int fd = sys_open("/APPS/STORE.DB", O_RDONLY);
    if (fd < 0) return;
    static char buf[8192];
    int n = sys_read(fd, buf, sizeof(buf) - 1); sys_close(fd);
    if (n <= 0) return; buf[n] = 0;
    char *p = buf;
    while (*p && g_nreg < MAXREG) {
        while (*p == ' ' || *p == '\n' || *p == '\r') p++;
        int i = 0; while (*p && *p != ' ' && *p != '\n' && i < 31) g_rid[g_nreg][i++] = *p++;
        g_rid[g_nreg][i] = 0;
        while (*p == ' ') p++;
        int j = 0; while (*p && *p != ' ' && *p != '\n' && *p != '\r' && j < 15) g_rver[g_nreg][j++] = *p++;
        g_rver[g_nreg][j] = 0;
        while (*p && *p != '\n') p++;
        if (g_rid[g_nreg][0]) g_nreg++;
    }
}

static void reg_set(const char *id, const char *ver) {
    static char buf[8192]; int len = 0;
    int fd = sys_open("/APPS/STORE.DB", O_RDONLY);
    if (fd >= 0) { len = sys_read(fd, buf, sizeof(buf) - 1); sys_close(fd); if (len < 0) len = 0; }
    buf[len] = 0;
    static char out[8192]; int o = 0; char *p = buf; int idl = strlen(id);
    while (*p) {
        char *ls = p; while (*p && *p != '\n') p++; int ll = p - ls; if (*p == '\n') p++;
        if (!(ll > idl && strncmp(ls, id, idl) == 0 && ls[idl] == ' ')) {
            for (int i = 0; i < ll && o < (int)sizeof(out) - 2; i++) out[o++] = ls[i];
            if (o < (int)sizeof(out) - 2) out[o++] = '\n';
        }
    }
    for (const char *a = id; *a && o < (int)sizeof(out) - 2; a++) out[o++] = *a;
    if (o < (int)sizeof(out) - 2) out[o++] = ' ';
    for (const char *a = ver; *a && o < (int)sizeof(out) - 2; a++) out[o++] = *a;
    if (o < (int)sizeof(out) - 2) out[o++] = '\n';
    pkg_write("/APPS/STORE.DB", out, o);
}

// regini_register() (the /APPS/REGINI.CFG writer) is GONE: it was a
// byte-for-byte duplicate of appstore/main.c's copy of the same dead code
// (see the #<startmenu-rust> note above). Both now share
// startmenu_register_app() (userland/libc/startmenu_reg.c).

static void mkparents(const char *dest) {
    char tmp[128]; int n = strlen(dest); if (n > 127) n = 127;
    for (int i = 1; i < n; i++) if (dest[i] == '/') {
        int j; for (j = 0; j < i; j++) tmp[j] = dest[j]; tmp[j] = 0; sys_mkdir(tmp, 0755);
    }
}
static int find_entry(arc_entry *e, int n, const char *id, const char *rel) {
    char want[160]; int w = 0;
    for (const char *a = id; *a && w < 158; a++) want[w++] = *a; want[w++] = '/';
    for (const char *a = rel; *a && w < 159; a++) want[w++] = *a; want[w] = 0;
    for (int i = 0; i < n; i++) if (strcmp(e[i].name, want) == 0) return i;
    return -1;
}

// Download + install package "id" version "ver" at manifest path "path".
static int apply_update(const char *id, const char *name, const char *ver, const char *path,
                        const char *sha256hex) {
    static char url[224];
    repo_url(path, url, (int)sizeof(url));
    int n = http_get(url, g_dl, sizeof(g_dl));
    printf("[upd] download %s -> %d bytes\n", url, n);
    ulog("download %s -> %d bytes", url, n);
    if (n <= 0) { ulog("ABORT: download failed"); return -1; }

    // #559: the downloaded package must match the sha256 in the signed manifest
    // before it is unpacked. Headless path: refuse and log, never apply.
    {
        int vrc = pkgsig_verify_package(g_dl, (size_t)n, sha256hex);
        if (vrc != PKGSIG_OK) {
            printf("[upd] REFUSED %s: %s\n", id, pkgsig_strerror(vrc));
            ulog("REFUSED package %s: %s (rc=%d, not unpacked, fail closed)",
                 id, pkgsig_strerror(vrc), vrc);
            notify_post("Update refused", name, NOTIFY_ERROR);
            return -1;
        }
        ulog("package %s sha256 VERIFIED against signed manifest (%d bytes)", id, n);
    }

    int count = 0;
    arc_entry *ents = arc_targz_extract(g_dl, (size_t)n, &count);
    printf("[upd] targz_extract -> count=%d ents=%p\n", count, (void*)ents);
    if (!ents || count <= 0) return -1;
    for (int z = 0; z < count && z < 6; z++) printf("[upd]   entry: %s (%u)\n", ents[z].name, (unsigned)ents[z].size);
    int ii = find_entry(ents, count, id, "INSTALL");
    printf("[upd] INSTALL entry idx=%d\n", ii);
    if (ii < 0) { arc_free_entries(ents, count); return -1; }
    char *man = (char *)ents[ii].data; size_t mlen = ents[ii].size;
    char launch[96]; launch[0] = 0; int wrote = 0;
    size_t p = 0;
    while (p < mlen) {
        char line[256]; int ll = 0;
        while (p < mlen && man[p] != '\n' && ll < 255) line[ll++] = man[p++];
        if (p < mlen) p++; line[ll] = 0;
        if (line[0] == '#' || !line[0]) continue;
        char *ar = strstr(line, "->"); if (!ar) continue;
        char src[128], dst[128]; int si = 0; char *s = line;
        while (s < ar && *s == ' ') s++;
        while (s < ar && *s != ' ' && si < 127) src[si++] = *s++; src[si] = 0;
        char *d = ar + 2; while (*d == ' ') d++;
        int di = 0; while (*d && *d != ' ' && *d != '\r' && di < 127) dst[di++] = *d++; dst[di] = 0;
        if (!src[0] || !dst[0]) continue;
        int fe = find_entry(ents, count, id, src);
        if (fe < 0) continue;
        int rc = pkg_write(dst, ents[fe].data, ents[fe].size);
        printf("[upd]   pkg_write %s (%u) -> rc=%d\n", dst, (unsigned)ents[fe].size, rc);
        if (rc >= 0) wrote++;
        if (!launch[0] && strncmp(dst, "/APPS/", 6) == 0) {
            int lower = 1; for (const char *c = dst + 6; *c; c++) if (*c >= 'A' && *c <= 'Z') { lower = 0; break; }
            if (lower) strcpy(launch, dst);
        }
    }
    arc_free_entries(ents, count);
    if (!wrote) return -1;
    reg_set(id, ver);
    if (launch[0]) startmenu_register_app(id, name, launch, 0);  /* #745 (#77): the updater never parses a category; NULL keeps its historical "Installed" group */
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    repo_load();                 // #559: repo source before any fetch
    int policy = read_policy();
    ulog("=== updated run start ===");
    ulog("repo=%s policy=%d", g_repo, policy);
    printf("[upd] policy=%d\n", policy);
    if (policy == POL_OFF) return 0;

    // Wait for the network, then fetch the manifest.
    for (int w = 0; w < 20 && !sys_net_is_up(); w++) usleep(500000);
    ulog("net_up=%d", sys_net_is_up());
    int n = -1;
    for (int a = 0; a < 4 && n <= 0; a++) { if (a) usleep(1000000); char murl[224]; repo_url("manifest.json", murl, (int)sizeof(murl));
        n = http_get(murl, (uint8_t *)g_manifest, sizeof(g_manifest) - 1); }
    ulog("manifest fetch -> %d bytes", n);
    if (n <= 0) { ulog("ABORT: manifest fetch failed"); return 1; }
    g_manifest[n] = 0;

    // #559: authenticate the manifest before trusting any version or hash in it.
    {
        int sn = -1;
        for (int a = 0; a < 3 && sn <= 0; a++) { if (a) usleep(500000);
            char surl[224]; repo_url("manifest.json.sig", surl, (int)sizeof(surl));
            sn = http_get(surl, g_sig, (int)sizeof(g_sig)); }
        ulog("signature fetch -> %d bytes", sn);
        if (sn <= 0) { printf("[upd] REFUSED: no repo signature\n");
            ulog("REFUSED: no repo signature (fail closed)"); return 1; }
        int vrc = pkgsig_verify_manifest(g_manifest, (size_t)n, g_sig, (size_t)sn);
        if (vrc != PKGSIG_OK) {
            printf("[upd] REFUSED: manifest signature: %s\n", pkgsig_strerror(vrc));
            ulog("REFUSED: manifest signature: %s (rc=%d, fail closed)", pkgsig_strerror(vrc), vrc);
            return 1;
        }
    }

    ulog("manifest signature VERIFIED against baked-in kernel pubkey");
    load_reg();
    ulog("installed apps in registry: %d", g_nreg);
    if (g_nreg == 0) return 0;

    char *pk = strstr(g_manifest, "\"packages\"");
    if (!pk) return 1;
    char *arr = strchr(pk, '['); if (!arr) return 1;
    char *p = arr + 1, *end = g_manifest + n;
    int applied = 0, notified = 0;

    while (p < end) {
        while (p < end && *p != '{' && *p != ']') p++;
        if (p >= end || *p == ']') break;
        char *obj = p; int depth = 0; char *q = p;
        while (q < end) { if (*q == '{') depth++; else if (*q == '}') { depth--; if (!depth) { q++; break; } } q++; }
        char *oe = q;
        char id[32], name[48], ver[16], path[96], shahex[65];
        jstr(obj, oe, "id", id, sizeof(id));
        jstr(obj, oe, "name", name, sizeof(name));
        jstr(obj, oe, "version", ver, sizeof(ver));
        jstr(obj, oe, "path", path, sizeof(path));
        jstr(obj, oe, "sha256", shahex, sizeof(shahex));
        p = oe;
        if (!id[0]) continue;
        // installed?
        for (int r = 0; r < g_nreg; r++) {
            if (strcmp(g_rid[r], id) == 0) {
                printf("[upd] installed %s: have=%s avail=%s\n", id, g_rver[r], ver);
                ulog("installed %s: have=%s avail=%s", id, g_rver[r], ver);
                if (strcmp(g_rver[r], ver) != 0 && ver[0]) {
                    // newer version available
                    if (policy == POL_AUTO) {
                        if (apply_update(id, name, ver, path, shahex) == 0) {
                            char body[96]; strcpy(body, name);
                            strncat(body, " updated to v", sizeof(body)-strlen(body)-1);
                            strncat(body, ver, sizeof(body)-strlen(body)-1);
                            notify_post("App updated", body, NOTIFY_SUCCESS);
                            applied++;
                        }
                    } else { // POL_NOTIFY
                        char body[96]; strcpy(body, name);
                        strncat(body, " v", sizeof(body)-strlen(body)-1);
                        strncat(body, ver, sizeof(body)-strlen(body)-1);
                        strncat(body, " is available", sizeof(body)-strlen(body)-1);
                        notify_post("Update available", body, NOTIFY_INFO);
                        notified++;
                    }
                }
                break;
            }
        }
    }
    (void)applied; (void)notified;
    return 0;
}
