// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aisvc.c - see aisvc.h for why this is one reader and not two.
//
// The body below is LIFTED from libc/aiclient.c's load_aisvc(), which was the
// correct parser of the two. aiclient.c now calls this instead of carrying it,
// so this is a MOVE, not a third copy.

#include "aisvc.h"
#include "userconf.h"
#include "string.h"
#include "syscall.h"

#define AISVC_NAME "AISVC.CFG"

int aisvc_load(aisvc_cfg_t *cfg) {
    if (!cfg) return 0;
    int fd = userconf_open_read(AISVC_NAME, 0);   // #684: no legacy fallback (secret)
    if (fd < 0) return 0;
    // One buffer sized for the whole file. The provisioner writes one line and
    // Settings writes five; 1024 is what aiclient.c has always used and the
    // Settings writer's own buffer is 512, so the file cannot outgrow this.
    static char buf[1024];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;

    int i = 0;
    while (buf[i]) {
        char line[512]; int ll = 0;
        while (buf[i] && buf[i] != '\n') { if (ll < (int)sizeof(line) - 1) line[ll++] = buf[i]; i++; }
        if (buf[i] == '\n') i++;
        line[ll] = 0;
        int e = ll - 1;
        while (e >= 0 && (line[e] == '\r' || line[e] == ' ' || line[e] == '\t')) line[e--] = 0;
        if (line[0] == '#' || line[0] == 0) continue;
        char *eq = 0;
        for (char *p = line; *p; p++) if (*p == '=') { eq = p; break; }
        if (!eq) continue;
        *eq = 0;
        const char *key = line; char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        if      (!strcmp(key, "endpoint"))  { if (val[0]) strlcpy(cfg->endpoint, val, sizeof(cfg->endpoint)); }
        else if (!strcmp(key, "model"))     { if (val[0]) strlcpy(cfg->model, val, sizeof(cfg->model)); }
        else if (!strcmp(key, "api_style")) { cfg->api_style = !strcmp(val, "anthropic")
                                                               ? AISVC_STYLE_ANTHROPIC : AISVC_STYLE_BEARER; }
        else if (!strcmp(key, "api_key"))   { if (val[0]) { strlcpy(cfg->api_key, val, sizeof(cfg->api_key));
                                                            cfg->have_key = 1; } }
        else if (!strcmp(key, "tool_calls")) {
            if      (!strcmp(val, "on"))  cfg->tool_calls = AISVC_TC_ON;
            else if (!strcmp(val, "off")) cfg->tool_calls = AISVC_TC_OFF;
            else                          cfg->tool_calls = AISVC_TC_AUTO;
        }
        // An unknown key is IGNORED, not an error: Settings and the first-boot
        // wizard may add fields a given client does not consume, and a reader
        // that refused them would make adding one a breaking change.
    }
    return 1;
}
