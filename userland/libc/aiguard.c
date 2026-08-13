// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aiguard.c - Ring-3 wrapper over SYS_AI_SCAN (#745). See aiguard.h.
#include "aiguard.h"
#include "syscall.h"
#include "string.h"

int aiguard_check(const char *text, aiguard_verdict_t *v)
{
    if (!text || !v) return -1;
    // Zero the whole record first. If the syscall is refused (a kernel that
    // predates #745 returns -1 from the dispatcher's default arm), the caller
    // must not read stack garbage and conclude a rule fired.
    memset(v, 0, sizeof(*v));
    v->severity = -1;
    long r = syscall2(SYS_AI_SCAN, (long)text, (long)v);
    if (r < 0) {
        // Fail OPEN here, deliberately, and say why: this wrapper exists to
        // produce a MESSAGE. The enforcement is in the kernel POST path and is
        // unaffected by whatever this returns, so refusing every prompt because
        // the informational call failed would break the client for no security
        // gain. If the kernel guard is present it still blocks; if it is absent
        // there is nothing to report anyway.
        return (int)r;
    }
    return (int)r;
}

const char *aiguard_sev_name(int sev)
{
    switch (sev) {
        case 2:  return "HIGH";
        case 1:  return "MEDIUM";
        case 0:  return "LOW";
        default: return "-";
    }
}
