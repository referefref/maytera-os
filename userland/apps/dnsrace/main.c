// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// dnsrace - a verification aid, not a shipped app.
//
// WHAT IT PROVES. sys_http_fetch_start() spawns one "httpfetch" kernel worker
// per job and the table has six slots, so an app can have six blocking
// dns_resolve() calls in flight at once. This starts six fetches to six
// DIFFERENT hostnames in a tight loop, which is exactly what a browser does
// with a page whose subresources live on other hosts, and what happens
// naturally when the App Store's fetch overlaps a widget's.
//
// THE ORACLE IS THE BODY, NOT THE STATUS. Each host serves a short, distinctive
// response, and the probe checks that the body it got back is the one that
// host serves. A resolver that hands lookup A the address belonging to lookup
// B produces a connection to the WRONG SERVER carrying the RIGHT Host header,
// and what comes back is somebody else's page (or a 4xx from a server that has
// never heard of you). A status-only check can miss that; a content check
// cannot.
//
// /CONFIG/DNSRACE.CFG, if present, replaces the built-in list. One
// "url|expected-substring" per line, up to six. An EMPTY expectation means "any
// completion is a pass, a failure is a fail", which is how the #549 breaker's
// multi-host trip rule is exercised: six URLs to six distinct DEAD hosts must
// still trip the breaker, while six to ONE dead host must not.
//
// Launched via /CONFIG/AUTORUN.CFG on a throwaway VM.
#include "stdio.h"
#include "unistd.h"
#include "string.h"
#include "fcntl.h"
#include "syscall.h"

#define NJOB 6

static char g_urlbuf[NJOB][160];
static char g_wantbuf[NJOB][64];

static const char *g_urls[NJOB] = {
    "http://example.com/",
    "http://info.cern.ch/",
    "http://icanhazip.com/",
    "http://detectportal.firefox.com/success.txt",
    "http://captive.apple.com/",
    "http://www.msftconnecttest.com/connecttest.txt",
};

// A substring only THAT host's response contains.
static const char *g_want[NJOB] = {
    "Example Domain",
    "info.cern.ch",
    ".",                       // icanhazip returns a bare IPv4 literal
    "success",
    "<TITLE>Success</TITLE>",
    "Microsoft Connect Test",
};

static char g_buf[8192];
static char g_cfg[1024];

static int contains(const char *hay, int n, const char *needle) {
    int m = 0; while (needle[m]) m++;
    if (m == 0) return 1;              // no expectation: any 200 body passes
    if (n < m) return 0;
    for (int i = 0; i + m <= n; i++) {
        int j = 0;
        while (j < m && hay[i + j] == needle[j]) j++;
        if (j == m) return 1;
    }
    return 0;
}

// Returns the number of URLs loaded from /CONFIG/DNSRACE.CFG (0 = use built-ins).
static int load_cfg(void) {
    int fd = open("/CONFIG/DNSRACE.CFG", O_RDONLY);
    if (fd < 0) return 0;
    long n = read(fd, g_cfg, sizeof(g_cfg) - 1);
    close(fd);
    if (n <= 0) return 0;
    g_cfg[n] = 0;
    int used = 0, i = 0;
    while (g_cfg[i] && used < NJOB) {
        int u = 0, w = 0, bar = 0;
        while (g_cfg[i] && g_cfg[i] != '\n' && g_cfg[i] != '\r') {
            char c = g_cfg[i++];
            if (c == '|' && !bar) { bar = 1; continue; }
            if (!bar) { if (u < (int)sizeof(g_urlbuf[0]) - 1) g_urlbuf[used][u++] = c; }
            else      { if (w < (int)sizeof(g_wantbuf[0]) - 1) g_wantbuf[used][w++] = c; }
        }
        while (g_cfg[i] == '\n' || g_cfg[i] == '\r') i++;
        g_urlbuf[used][u] = 0;
        g_wantbuf[used][w] = 0;
        if (u > 0 && g_urlbuf[used][0] != '#') {
            g_urls[used] = g_urlbuf[used];
            g_want[used] = g_wantbuf[used];
            used++;
        }
    }
    return used;
}

int main(void) {
    int n_from_cfg = load_cfg();
    int njob = n_from_cfg ? n_from_cfg : NJOB;
    printf("[DNSRACE-APP] start: %d concurrent fetches to %d distinct hosts (cfg=%d)\n",
           njob, njob, n_from_cfg);

    int id[NJOB];
    for (int i = 0; i < njob; i++) {
        id[i] = http_fetch_start(g_urls[i]);
        printf("[DNSRACE-APP] start[%d] %s -> job %d\n", i, g_urls[i], id[i]);
    }

    int verdict[NJOB];          // 0 = still running, 1 = right body, 2 = wrong/failed
    for (int i = 0; i < NJOB; i++) verdict[i] = 0;

    for (int spin = 0; spin < 400; spin++) {
        int left = 0;
        for (int i = 0; i < njob; i++) {
            if (verdict[i] || id[i] < 0) continue;
            int status = 0; unsigned int len = 0;
            int st = http_fetch_poll(id[i], &status, &len);
            if (st == 1 || st == 2) {
                int nb = http_fetch_read(id[i], g_buf, sizeof(g_buf) - 1);
                if (nb < 0) nb = 0;
                g_buf[nb] = 0;
                int right = (st == 1 && status == 200 && contains(g_buf, nb, g_want[i]));
                printf("[DNSRACE-APP] done[%d] %s state=%d status=%d len=%u body=%s\n",
                       i, g_urls[i], st, status, len, right ? "CORRECT" : "WRONG-OR-EMPTY");
                verdict[i] = right ? 1 : 2;
            } else {
                left++;
            }
        }
        if (!left) break;
        sys_sleep(200);
    }

    int ok = 0, bad = 0, hung = 0;
    for (int i = 0; i < njob; i++) {
        if (verdict[i] == 1) ok++;
        else if (verdict[i] == 2) bad++;
        else hung++;
    }
    printf("[DNSRACE-APP] SUMMARY correct=%d wrong=%d never-finished=%d of %d\n",
           ok, bad, hung, njob);
    printf("[DNSRACE-APP] END\n");
    return 0;
}
