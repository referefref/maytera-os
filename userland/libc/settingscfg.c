// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// settingscfg.c - see settingscfg.h for the rationale. Same shape as tz.c's
// tz_reload()/tz_index() throttle: read SETTINGS.CFG at most once every
// SETTINGSCFG_REFRESH_MS, serve a cache in between. A failed read leaves the
// previous cached value in place and simply re-arms the throttle - there is
// no wait, no poll loop and no retry, so nothing here can stall a frame.

#include "settingscfg.h"
#include "userconf.h"
#include "syscall.h"

#define SETTINGSCFG_REFRESH_MS 2000UL

static int           s_use24h    = 1;     // historical hardcoded default
static int           s_dblclick_raw = 50;  // SETTINGS.CFG default (settings/main.c)
static unsigned long s_last_ms   = 0;
static int           s_loaded    = 0;

// Parse "h=<0|1>" and "k=<0-100>" out of SETTINGS.CFG's "key=value\n" lines
// (settings/main.c's sv_putint() format). Tolerant of any other keys/order;
// only 'h' and 'k' are read here, matching settingscfg.h's getters.
static void settingscfg_parse(const char *buf, long n) {
    long i = 0;
    while (i < n) {
        char key = buf[i];
        if (i + 1 < n && buf[i+1] == '=') {
            long j = i + 2;
            int neg = 0;
            if (j < n && buf[j] == '-') { neg = 1; j++; }
            long val = 0; int any = 0;
            while (j < n && buf[j] >= '0' && buf[j] <= '9') {
                val = val * 10 + (buf[j] - '0'); j++; any = 1;
            }
            if (any) {
                if (neg) val = -val;
                if (key == 'h') s_use24h = val ? 1 : 0;
                else if (key == 'k') s_dblclick_raw = (int)val;
            }
        }
        while (i < n && buf[i] != '\n') i++;
        if (i < n) i++;   // skip the '\n'
    }
}

static void settingscfg_reload(void) {
    int fd = userconf_open_read("SETTINGS.CFG", "SETTINGS.CFG");
    if (fd < 0) return;   // keep whatever is cached
    char b[512];
    long n = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    settingscfg_parse(b, n);
}

static void settingscfg_poll(void) {
    unsigned long now = uptime_ms();
    if (!s_loaded || (unsigned long)(now - s_last_ms) >= SETTINGSCFG_REFRESH_MS) {
        s_last_ms = now;
        s_loaded  = 1;
        settingscfg_reload();
    }
}

int settingscfg_use24h(void) {
    settingscfg_poll();
    return s_use24h;
}

int settingscfg_dblclick_ms(void) {
    settingscfg_poll();
    int ms = 900 - (s_dblclick_raw * 7);
    if (ms < 150) ms = 150;
    if (ms > 900) ms = 900;
    return ms;
}

void settingscfg_invalidate(void) { s_loaded = 0; }
