// ai_env_stubs.h - #deadtests: the ENVIRONMENT half of the AI host harnesses.
//
// WHY THIS FILE EXISTS. userland/libc/tests/{aieject,aifs,escrow}_test.c each
// compile the SHIPPING ../aiclient.c, ../aicap.c, ../aitools.c (and friends)
// freestanding, objcopy-prefix every symbol with ad_, and link the result next
// to glibc. Anything those translation units call that belongs to the OS
// ENVIRONMENT rather than to the logic under test has to be supplied by the
// harness. Each harness already mocks the file and clock syscalls; these are
// the per-user-config, capability-consent, app-registry and AI-service-config
// helpers that the same units acquired later.
//
// THEY WERE NOT SUPPLIED, AND THAT IS WHY THE FOUR TESTS DID NOT RUN. Measured
// 2026-10-02 on dev @b387c216: run_aidev.sh, run_aieject.sh, run_aifs.sh and
// run_escrow.sh all exited 1 with "RESULT: BROKEN - positive arm did not build"
// on fourteen undefined ad_* references. Nothing in this repo invoked any of
// the four, so a link that had been broken by ordinary growth of aiclient.c sat
// there unnoticed. The fault was in the HARNESS, not in the tree: no shipping
// code was wrong. That is the #deadtests pattern exactly - "nobody wired it"
// and "it could not be wired" are indistinguishable from outside.
//
// ONE COPY, NOT THREE. The three aiclient-based harnesses need an identical set,
// so the set lives here. Two hardcoded lists in this tree have already drifted
// apart (host-sync.sh / host-pin-check.sh), so a second copy is not an option.
//
// INCLUDE IT AFTER the harness's own mapped(), because the file-backed stubs
// route through that virtual-to-host path mapping, so a write these units make
// lands in the same sandbox the test later reads. mapped() is the ONLY thing
// this header needs from its includer; everything else it brings itself, so one
// include line is the whole wiring.
//
// HONESTY ABOUT ad_cap_acquire. The real cap_acquire() raises the TRUSTED
// COMPOSITOR's consent modal and waits for a human. A host harness has neither,
// so this stub returns 0 = GRANTED. That is deliberate and it is the SAFE
// direction for these tests: it means the stub is never the thing that refuses,
// so every refusal a scenario observes came from the gate actually under test
// (the aidev manifest, or aicap's token/consent/scope logic). A stub that
// denied would make a passing "refused" assertion prove nothing.
#ifndef MAYTERA_AI_ENV_STUBS_H
#define MAYTERA_AI_ENV_STUBS_H

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

// ---- per-user config / home (userland/libc/userconf.c) ----------------------
// Routed through the harness's mapped() so these writes land in the test's
// sandbox and are visible to its own read_all()/put_file() helpers.

void ad_uc_report_write_refusal(const char *what, const char *path, int rc) {
    fprintf(stderr, "      [stub uc_report_write_refusal] %s %s rc=%d\n",
            what ? what : "(null)", path ? path : "(null)", rc);
}

int ad_userconf_finish_write(int fd, const void *buf, unsigned long len) {
    if (fd < 0) return -1;
    long w = (len && buf) ? (long)write(fd, buf, (size_t)len) : 0;
    close(fd);
    return (w == (long)len) ? 0 : -1;
}

// Own parent-directory maker, so this header depends on mapped() and nothing
// else. (The harnesses each have a host_mkparents(); aidev_test.c does not, and
// a header that works in three of four places is the kind of near-miss that put
// these tests in this state.)
static inline void aies_mkparents(const char *hostpath) {
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s", hostpath);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; (void)mkdir(tmp, 0755); *p = '/'; }
}

static inline int aies_open_mapped(const char *vpath, int wr) {
    char hp[700];
    mapped(vpath, hp, (int)sizeof(hp));
    if (wr) { aies_mkparents(hp); return open(hp, O_WRONLY | O_CREAT | O_TRUNC, 0600); }
    return open(hp, O_RDONLY);
}

int ad_userhome_open_read(const char *sub, const char *name, const char *legacy) {
    char v[600];
    if (sub && *sub) snprintf(v, sizeof v, "/HOME/%s/%s", sub, name ? name : "");
    else            snprintf(v, sizeof v, "/HOME/%s", name ? name : "");
    int fd = aies_open_mapped(v, 0);
    if (fd < 0 && legacy && *legacy) fd = aies_open_mapped(legacy, 0);
    return fd;
}

int ad_userhome_open_write(const char *sub, const char *name) {
    char v[600];
    if (sub && *sub) snprintf(v, sizeof v, "/HOME/%s/%s", sub, name ? name : "");
    else            snprintf(v, sizeof v, "/HOME/%s", name ? name : "");
    return aies_open_mapped(v, 1);
}

int ad_userconf_open_write_path(const char *path) {
    if (!path || !*path) return -1;
    return aies_open_mapped(path, 1);
}

int ad_userscratch_path(const char *name, char *out, unsigned long cap) {
    if (!out || !cap) return -1;
    int n = snprintf(out, (size_t)cap, "/HOME/.SCRATCH/%s", name ? name : "");
    return (n > 0 && (unsigned long)n < cap) ? 0 : -1;
}

// ---- compositor consent broker (userland/libc/capreq.c) ---------------------
// See the HONESTY note at the top: GRANTED, so the stub never refuses.
long ad_cap_acquire(unsigned int cap, unsigned int scope_kind,
                    const char *scope, const char *reason) {
    (void)cap; (void)scope_kind; (void)scope; (void)reason;
    return 0;
}
const char *ad_cap_err_str(long e) { (void)e; return "host-harness stub (no consent broker)"; }
int ad_cap_is_denial(long e) { return e < 0; }

// ---- launchable-app registry (userland/libc/applaunch.c) --------------------
// An EMPTY registry, which is the honest state for a host harness: there is no
// /APPS manifest and no kernel to spawn into. Every app.launch tool call
// therefore reports "unknown app" rather than a fabricated success.
int         ad_applaunch_load(void)  { return 0; }
int         ad_applaunch_count(void) { return 0; }
const void *ad_applaunch_at(int i)   { (void)i; return 0; }
int         ad_applaunch_run(const char *id, const char *rom_query,
                             const char *arg, void *res) {
    (void)id; (void)rom_query; (void)arg; (void)res;
    return -1;   // APPLAUNCH_ERR_UNKNOWN
}

// ---- libc printf, as the prefixed units see it ------------------------------
// aidev.c/aicap.c call printf(); objcopy renames it to ad_printf. Three of the
// four harnesses already defined this one by hand; aidev_test.c did not, which
// is one of the three references that broke its link.
#ifndef AIES_NO_PRINTF
#include <stdarg.h>
int ad_printf(const char *f, ...) {
    va_list ap; va_start(ap, f); int r = vprintf(f, ap); va_end(ap); return r;
}
#endif

// ---- per-user AI service config (userland/libc/aisvc.c) ---------------------
// 0 means "no per-user AISVC.CFG", which aisvc_load() documents as NOT an error
// and which leaves the caller's seeded defaults exactly as they were.
int ad_aisvc_load(void *cfg) { (void)cfg; return 0; }

#endif // MAYTERA_AI_ENV_STUBS_H
