// services.c - #95 Background services subsystem for MayteraOS
//
// See services.h for the design overview. This file owns the in-kernel
// service registry and lifecycle. Services are ordinary user-mode ELF
// programs started via proc_create_user(); after creation we tag the new
// process as a service (is_service=1) and stamp its service account uid and
// capability mask onto it. The syscall layer (proc/syscall.c) consults those
// fields to sandbox the service to its declared permissions.

#include "services.h"
#include "process.h"
#include "signal.h"
#include "../fs/fat.h"
#include "../fs/panic.h"          // #418: STAGE_SVC_SPAWN breadcrumb
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"
#include "../exec/elf.h"

extern fat_fs_t g_fat_fs;

static service_t g_services[MAX_SERVICES];
static int       g_svc_count = 0;
static int       g_svc_inited = 0;

// ---- registry helpers ------------------------------------------------------

int svc_count(void) { return g_svc_count; }

service_t *svc_at(int index) {
    if (index < 0 || index >= g_svc_count) return 0;
    return &g_services[index];
}

service_t *svc_find(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < g_svc_count; i++) {
        if (strcmp(g_services[i].name, name) == 0) return &g_services[i];
    }
    return 0;
}

// Register or update a service by name. Returns the slot, or NULL if full.
static service_t *svc_register(const char *name, const char *exec,
                               const char *account, uint32_t uid,
                               uint32_t perms, int autostart, int enabled) {
    service_t *svc = svc_find(name);
    if (!svc) {
        if (g_svc_count >= MAX_SERVICES) return 0;
        svc = &g_services[g_svc_count++];
        memset(svc, 0, sizeof(*svc));
        svc->pid = 0;
    }
    // (#785) A SERVICES.CFG line naming an existing service overrides it, and
    // that must not leave built-in hooks attached to what is now an ELF
    // service: svc_start() would then call the kernel start hook and ignore
    // exec[] entirely, so the operator's config line would silently do nothing
    // like what it says. Overriding by name means becoming a userland service.
    svc->builtin   = 0;
    svc->b_start   = 0;
    svc->b_stop    = 0;
    svc->b_running = 0;
    svc->b_persist = 0;
    strncpy(svc->name, name, SVC_NAME_MAX - 1);
    svc->name[SVC_NAME_MAX - 1] = '\0';
    strncpy(svc->exec, exec, SVC_EXEC_MAX - 1);
    svc->exec[SVC_EXEC_MAX - 1] = '\0';
    strncpy(svc->account, account ? account : "service", SVC_ACCOUNT_MAX - 1);
    svc->account[SVC_ACCOUNT_MAX - 1] = '\0';
    svc->uid       = uid;
    svc->perms     = perms;
    svc->autostart = autostart ? 1 : 0;
    svc->enabled   = enabled ? 1 : 0;
    return svc;
}

// Register an in-kernel service (#785). See services.h for why built-ins live
// in the shared registry instead of each growing a private on/off switch.
int svc_register_builtin(const char *name, const char *account, uint32_t uid,
                         uint32_t perms, int autostart, int enabled,
                         svc_start_fn start, svc_stop_fn stop,
                         svc_running_fn running, svc_persist_fn persist) {
    if (!name || !start) return -1;
    // exec[] is empty for a built-in: there is no ELF. It is left as the empty
    // string rather than something decorative like "(kernel)" so that any code
    // that tries to open it fails loudly instead of reading a stray file.
    service_t *svc = svc_register(name, "", account, uid, perms, autostart, enabled);
    if (!svc) return -2;
    svc->builtin   = 1;
    svc->pid       = 0;
    svc->b_start   = start;
    svc->b_stop    = stop;
    svc->b_running = running;
    svc->b_persist = persist;
    return 0;
}

// ---- permission string parsing/formatting ---------------------------------

// Parse a comma-separated permission list ("net,fs,spawn" / "all") into a
// SVC_PERM_* bitmask. Whitespace around tokens is tolerated.
static uint32_t svc_parse_perms(const char *s) {
    uint32_t mask = 0;
    if (!s) return 0;
    char tok[16];
    int t = 0;
    for (;; s++) {
        char c = *s;
        if (c == ',' || c == ' ' || c == '\t' || c == '\0') {
            if (t > 0) {
                tok[t] = '\0';
                if      (strcmp(tok, "all")   == 0) mask |= SVC_PERM_ALL;
                else if (strcmp(tok, "net")   == 0) mask |= SVC_PERM_NET;
                else if (strcmp(tok, "fs")    == 0) mask |= SVC_PERM_FSWRITE;
                else if (strcmp(tok, "spawn") == 0) mask |= SVC_PERM_SPAWN;
                else if (strcmp(tok, "input") == 0) mask |= SVC_PERM_INPUT;
                else if (strcmp(tok, "selfupdate") == 0) mask |= SVC_PERM_SELFUPDATE;
                t = 0;
            }
            if (c == '\0') break;
            continue;
        }
        if (t < (int)sizeof(tok) - 1) tok[t++] = c;
    }
    return mask;
}

void svc_perms_str(uint32_t perms, char *buf, int buflen) {
    if (!buf || buflen <= 0) return;
    if (perms == SVC_PERM_ALL) { strncpy(buf, "all", buflen - 1); buf[buflen-1]='\0'; return; }
    buf[0] = '\0';
    int n = 0;
    struct { uint32_t bit; const char *name; } tbl[] = {
        { SVC_PERM_NET, "net" }, { SVC_PERM_FSWRITE, "fs" },
        { SVC_PERM_SPAWN, "spawn" }, { SVC_PERM_INPUT, "input" },
        { SVC_PERM_SELFUPDATE, "selfupdate" },
    };
    for (int i = 0; i < 5; i++) {
        if (perms & tbl[i].bit) {
            int len = strlen(buf);
            if (n > 0 && len < buflen - 1) { buf[len++] = ','; buf[len] = '\0'; }
            strncpy(buf + len, tbl[i].name, buflen - 1 - len);
            buf[buflen - 1] = '\0';
            n++;
        }
    }
    if (n == 0) { strncpy(buf, "none", buflen - 1); buf[buflen-1]='\0'; }
}

// ---- config file parsing ---------------------------------------------------
//
// /CONFIG/SERVICES.CFG, one service per line, whitespace-separated fields:
//   name  exec-path  account  uid  perms  autostart  enabled
// e.g.
//   heartbeat /APPS/SVCHB svc_hb 100 fs 1 1
// Lines beginning with '#' (after optional leading spaces) are comments.

static int svc_next_field(const char **pp, char *out, int outlen) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '\n' || *p == '\r') { *pp = p; return 0; }
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        if (n < outlen - 1) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    *pp = p;
    return 1;
}

static uint32_t svc_atou(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

static void svc_load_config(void) {
    if (!g_fat_fs.mounted) return;
    uint32_t sz = 0;
    char *data = (char *)fat_read_file(&g_fat_fs, "/CONFIG/SERVICES.CFG", &sz);
    if (!data || sz == 0) { if (data) kfree(data); return; }

    const char *p = data;
    const char *end = data + sz;
    while (p < end) {
        // Extract one line.
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        const char *le = p;
        if (p < end) p++;  // skip newline

        // Trim leading whitespace; skip blanks and comments.
        while (ls < le && (*ls == ' ' || *ls == '\t' || *ls == '\r')) ls++;
        if (ls >= le || *ls == '#') continue;

        // Parse the seven fields off this line. Use a local NUL-bounded
        // cursor so svc_next_field stops at the line end.
        char line[256];
        int ll = 0;
        for (const char *q = ls; q < le && ll < (int)sizeof(line) - 1; q++) line[ll++] = *q;
        line[ll] = '\0';

        const char *cur = line;
        char name[SVC_NAME_MAX], exec[SVC_EXEC_MAX], account[SVC_ACCOUNT_MAX];
        char uidf[16], permsf[64], autof[8], enf[8];
        if (!svc_next_field(&cur, name, sizeof(name)))   continue;
        if (!svc_next_field(&cur, exec, sizeof(exec)))   continue;
        if (!svc_next_field(&cur, account, sizeof(account))) strncpy(account, "service", sizeof(account) - 1);
        if (!svc_next_field(&cur, uidf, sizeof(uidf)))   strncpy(uidf, "0", sizeof(uidf) - 1);
        if (!svc_next_field(&cur, permsf, sizeof(permsf))) permsf[0] = '\0';
        if (!svc_next_field(&cur, autof, sizeof(autof))) strncpy(autof, "0", sizeof(autof) - 1);
        if (!svc_next_field(&cur, enf, sizeof(enf)))     strncpy(enf, "1", sizeof(enf) - 1);

        svc_register(name, exec, account, svc_atou(uidf),
                     svc_parse_perms(permsf),
                     svc_atou(autof) != 0, svc_atou(enf) != 0);
    }
    kfree(data);
}

// ---- public API ------------------------------------------------------------

void svc_init(void) {
    if (g_svc_inited) return;
    g_svc_inited = 1;
    g_svc_count = 0;

    // Built-in default service so the subsystem is always functional even
    // without a config file. The heartbeat writes a tick counter to
    // /SVCLOG.TXT, demonstrating a sandboxed background service: it is granted
    // only the fs-write capability (SVC_PERM_FSWRITE), so the kernel denies it
    // any other privileged syscall regardless of its uid. It runs as the
    // "svc_hb" system service account (uid 0, like syslogd/cron) so it can
    // write its system log; a non-root service uid is fully supported by the
    // registry but is then subject to the normal filesystem permission layer.
    svc_register("heartbeat", "/APPS/SVCHB", "svc_hb", 0,
                 SVC_PERM_FSWRITE, /*autostart*/1, /*enabled*/1);

#ifdef MAYTERA_SSHD
    // (#785) The in-kernel SSH server registers as a BUILT-IN service so an
    // operator gets the same start/stop/enable/disable verbs for it as for any
    // other service. Compiled out entirely unless the kernel was built with
    // MAYTERA_SSHD=1, so on a default build there is no sshd to register, no
    // listener to enable, and no code on the image to regress.
    { extern void sshd_service_register(void); sshd_service_register(); }
#endif

    // Merge/override with declarations from /CONFIG/SERVICES.CFG if present.
    svc_load_config();

    kprintf("[SVC] service manager: %d service(s) registered\n", g_svc_count);
}

int svc_is_running(service_t *svc) {
    // (#785) A built-in has no pid, so the pid test below would report it
    // permanently stopped and svc_start() would happily start it a second time.
    if (svc && svc->builtin) return svc->b_running ? svc->b_running() : 0;
    if (!svc || svc->pid <= 0) { if (svc) svc->pid = 0; return 0; }
    process_t *p = proc_get((uint32_t)svc->pid);
    if (!p || p->state == PROC_STATE_ZOMBIE || p->state == PROC_STATE_UNUSED) {
        svc->pid = 0;
        return 0;
    }
    return 1;
}

int svc_start(const char *name) {
    service_t *svc = svc_find(name);
    if (!svc) return -1;
    if (!svc->enabled) return -2;
    if (svc_is_running(svc)) return svc->pid;  // already running

    if (svc->builtin) {
        int r = svc->b_start();
        kprintf("[SVC] built-in '%s' start -> %d (running=%d)\n",
                svc->name, r, svc_is_running(svc));
        // Report what actually happened, not what was requested. A built-in
        // that refuses to start (sshd with enable=0 in its own config, or no
        // host key) must not be reported as started.
        if (r < 0) return r;
        return svc_is_running(svc) ? 0 : -9;
    }

    if (!g_fat_fs.mounted) return -3;
    uint32_t sz = 0;
    void *data = fat_read_file(&g_fat_fs, svc->exec, &sz);
    if (!data || sz == 0) { if (data) kfree(data); return -4; }
    if (elf_validate(data, sz) != 0) { kfree(data); return -5; }

    // #418: breadcrumb the spawn BEFORE elf_load_user/proc_create_user run -
    // main.c documents this exact spot as a historical fault site (physical
    // pages "not yet safely writable through the kernel identity map" right
    // as services start), which is also when the iMac crash was observed.
    stage_set(STAGE_SVC_SPAWN, svc->name);

    // #692: a service runs as its own service account, named here. It used
    // to be spawned with an inherited identity and then have uid and euid
    // stamped on afterwards while gid and egid were left inherited, which
    // produced the incoherent pair uid 0 / gid 1000. proc_as_uid() resolves
    // the gid from /CONFIG/PASSWD, so both now move together.
    int pid = proc_create_user_as(svc->name, data, sz, 0, 0, proc_as_uid(svc->uid));
    kfree(data);
    if (pid <= 0) return -6;

    // Tag the new process as a sandboxed service under its service account.
    process_t *p = proc_get((uint32_t)pid);
    if (p) {
        p->is_service = 1;
        p->svc_perms  = svc->perms;
        // uid/gid/euid/egid were set coherently at spawn (#692); do not
        // re-stamp them here, that half-stamp was the bug.
    }
    svc->pid = pid;
    kprintf("[SVC] started '%s' pid %d uid %u gid %u\n", svc->name, pid,
            p ? p->uid : svc->uid, p ? p->gid : 0);
    return pid;
}

int svc_stop(const char *name) {
    service_t *svc = svc_find(name);
    if (!svc) return -1;
    if (svc->builtin) {
        if (!svc->b_stop) return -7;
        int r = svc->b_stop();
        kprintf("[SVC] built-in '%s' stop -> %d (running=%d)\n",
                svc->name, r, svc_is_running(svc));
        return r;
    }
    if (!svc_is_running(svc)) { svc->pid = 0; return 0; }

    process_t *p = proc_get((uint32_t)svc->pid);
    if (p) sig_raise(p, SIGKILL);  // delivered at the service's next syscall return
    kprintf("[SVC] stopping '%s' pid %d\n", svc->name, svc->pid);
    svc->pid = 0;
    return 0;
}

// (#785) THIS FUNCTION USED TO LIE BY OMISSION. It set svc->enabled in RAM and
// returned 0, and nothing anywhere wrote /CONFIG/SERVICES.CFG back, so the
// change vanished at the next boot while the caller was told it succeeded. It
// was never noticed because it had ZERO callers: the only surface that was ever
// documented to reach it, the remote-control "svc" command, had been deleted
// two tickets earlier. A function with no callers is not a working feature.
//
// It now persists through the service's own config file, and when it cannot it
// says so instead of returning 0. "Enable/disable as needed" is only true if it
// survives the reboot, which is the whole point of the verb.
int svc_enable(const char *name, int enable) {
    service_t *svc = svc_find(name);
    if (!svc) return -1;

    // Persist FIRST. If the durable state cannot be written there is no point
    // changing the RAM state: that would leave the running system and the disk
    // disagreeing, which is worse than refusing, and the operator would only
    // find out at the next boot.
    if (!svc->b_persist) return -7;   // no durable backing: refuse, do not pretend
    int pr = svc->b_persist(enable ? 1 : 0);
    if (pr != 0) {
        kprintf("[SVC] '%s' %s NOT persisted (%d) - refusing\n",
                svc->name, enable ? "enable" : "disable", pr);
        return -8;
    }

    svc->enabled = enable ? 1 : 0;
    if (!enable && svc_is_running(svc)) svc_stop(name);
    kprintf("[SVC] '%s' %s (persisted)\n", svc->name, enable ? "enabled" : "disabled");
    return 0;
}

void svc_autostart(void) {
    for (int i = 0; i < g_svc_count; i++) {
        service_t *svc = &g_services[i];
        if (svc->enabled && svc->autostart) {
            int r = svc_start(svc->name);
            if (r <= 0)
                kprintf("[SVC] autostart '%s' failed (%d)\n", svc->name, r);
        }
    }
}
