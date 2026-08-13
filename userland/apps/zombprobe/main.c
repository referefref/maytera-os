// zombprobe - #745 task 37: does an async HTTP fetch leak a process-table slot?
//
// WHY THIS EXISTS. The App Store's "Couldn't reach the App Store repository"
// is reached when http_get_live() returns <= 0 four times, and http_get_live()
// returns -1 IMMEDIATELY when http_fetch_start() returns a negative number,
// before a single packet is sent. So the interesting question is not "can the
// box reach the repo", it is "can a fetch even START". This probe answers that
// by doing N complete fetch/poll/read cycles and printing, for every one of
// them, the start return code AND a census of the kernel process table taken
// through SYS_PROC_LIST. The census is what turns "it stopped working" into
// "there are 58 httpfetch zombies and MAX_PROCESSES is 64".
//
// It is the regression check for the fix, in both directions: on a kernel with
// the leak, ZP:START rc goes negative somewhere around iteration 55-60 and
// never recovers; on a fixed kernel every iteration succeeds and the zombie
// count stays flat.
//
// Output discipline (blame.md, 2026-08-12 "Three deployment facts"): a
// userland printf() on the no-PTY path emits ONE SERIAL RECORD PER CHARACTER,
// so every line here goes out as a single SYS_WRITE.

#include "../../libc/maytera.h"
#include "../../libc/syscall.h"
#include "../../libc/unistd.h"

#ifndef ZP_ITERS
#define ZP_ITERS 90
#endif

// The LAN repo mirror. Deliberately plain http on the local server: the leak
// this probe measures happens in sys_http_fetch_start() BEFORE any network
// I/O, so the URL only has to be fast and reliable. The real store URL is
// exercised separately at the end (see zp_store_probe).
static const char *ZP_URL   = "http://192.0.2.1/manifest.json";
static const char *ZP_STORE = "https://updates.maytera.net/manifest.json";

static void outf(const char *fmt, ...) {
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    syscall3(SYS_WRITE, 1, (long)(uintptr_t)buf, (long)n);
}

// Small "i=<n>" tag builder; the libc's gui_itoa lives in the GUI header this
// headless probe does not want to pull in.
static int vsnprintf_tag(char *out, int cap, int v) {
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0 && n < (int)sizeof(t)) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    int i = 0;
    if (cap > 2) { out[i++] = 'i'; out[i++] = '='; }
    while (n > 0 && i < cap - 1) out[i++] = t[--n];
    out[i] = 0;
    return i;
}

// PROC_STATE_ZOMBIE from kernel/proc/process.h. The enum is not exported to
// userland, so it is spelled out here with the value it has there; if that
// ever moves, this probe reports nonsense rather than failing, which is why
// the census also prints the raw total.
#define ZP_STATE_ZOMBIE 5u
#define ZP_MAX_PROC     64

static proc_info_t g_pi[ZP_MAX_PROC];

// Census: total slots in use, zombies, and how many of those zombies are the
// async-fetch worker (kernel/proc/syscall.c names it "httpfetch").
static void census(const char *tag) {
    int n = sys_proc_list(g_pi, ZP_MAX_PROC);
    int zomb = 0, zfetch = 0, live_fetch = 0;
    for (int i = 0; i < n; i++) {
        int is_fetch = (strcmp(g_pi[i].name, "httpfetch") == 0);
        if (g_pi[i].state == ZP_STATE_ZOMBIE) {
            zomb++;
            if (is_fetch) zfetch++;
        } else if (is_fetch) {
            live_fetch++;
        }
    }
    outf("ZP:CENSUS %s used=%d/%d zombies=%d httpfetch_zombies=%d httpfetch_live=%d\n",
         tag, n, ZP_MAX_PROC, zomb, zfetch, live_fetch);
}

// One complete fetch. Returns the http_fetch_start() code (>= 0 = slot id),
// and drains the job to completion so the JOB slot (six of them, task #36) is
// never the thing that runs out - only the process table is under test here.
static char g_body[4096];

static int one_fetch(int iter, const char *url) {
    int job = http_fetch_start(url);
    if (job < 0) {
        outf("ZP:START iter=%d rc=%d %s\n", iter, job,
             job == NET_ERR_FAULTY ? "(NET_ERR_FAULTY: breaker latched)"
                                   : "(-1: no slot / worker could not spawn)");
        return job;
    }
    int status = 0; unsigned int len = 0; int st = 0;
    for (int t = 0; t < 600; t++) {          // ~30s budget, same as the app
        st = http_fetch_poll(job, &status, &len);
        if (st != 0) break;
        usleep(50000);
    }
    if (st == 0) {
        outf("ZP:TIMEOUT iter=%d job=%d\n", iter, job);
        http_fetch_cancel(job);
        return -100;
    }
    int rd = http_fetch_read(job, g_body, (unsigned)sizeof(g_body));
    outf("ZP:OK iter=%d job=%d state=%d http=%d len=%u read=%d\n",
         iter, job, st, status, len, rd);
    return job;
}

// Replay the App Store's own load_manifest() retry shape against the REAL
// store URL, and say which of its branches it would take. This is what makes
// the probe's verdict about the user's symptom rather than about a lab URL.
static void zp_store_probe(void) {
    int n = -1;
    for (int attempt = 0; attempt < 4 && n <= 0; attempt++) {
        if (attempt) usleep(1000000);
        int job = http_fetch_start(ZP_STORE);
        if (job < 0) {
            outf("ZP:STORE attempt=%d http_fetch_start rc=%d -> http_get_live returns -1\n",
                 attempt, job);
            n = -1;
            continue;
        }
        int status = 0; unsigned int len = 0; int st = 0;
        for (int t = 0; t < 600; t++) {
            st = http_fetch_poll(job, &status, &len);
            if (st != 0) break;
            usleep(50000);
        }
        if (st == 1) { n = http_fetch_read(job, g_body, (unsigned)sizeof(g_body)); }
        else         { http_fetch_read(job, g_body, (unsigned)sizeof(g_body)); n = -1; }
        outf("ZP:STORE attempt=%d job=%d state=%d http=%d len=%u n=%d\n",
             attempt, job, st, status, len, n);
    }
    if (n <= 0)
        outf("ZP:STORE VERDICT app would show: \"Couldn't reach the App Store repository\"\n");
    else
        outf("ZP:STORE VERDICT app would LOAD the catalog (n=%d)\n", n);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    outf("ZP:BEGIN zombprobe iters=%d url=%s\n", ZP_ITERS, ZP_URL);

    // Wait for the stack to come up rather than burning iterations on it.
    for (int w = 0; w < 60 && !sys_net_is_up(); w++) usleep(500000);
    outf("ZP:NET up=%d\n", sys_net_is_up());

    census("boot");

    int first_fail = -1, fails = 0;
    for (int i = 1; i <= ZP_ITERS; i++) {
        int rc = one_fetch(i, ZP_URL);
        if (rc < 0) {
            fails++;
            if (first_fail < 0) first_fail = i;
        }
        if (i % 10 == 0 || (rc < 0 && fails <= 3)) {
            char tag[16];
            (void)vsnprintf_tag(tag, sizeof(tag), i);
            census(tag);
        }
    }

    census("end");
    outf("ZP:SUMMARY iters=%d failures=%d first_failure_at=%d\n",
         ZP_ITERS, fails, first_fail);
    zp_store_probe();
    census("after-store");
    outf("ZP:DONE\n");
    return 0;
}
