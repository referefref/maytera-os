// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aiguard.h - Ring-3 view of the kernel's prompt-injection screen (#745).
//
// The RULESET and the DECISION are kernel-owned (kernel/security/nova.c +
// kernel/rustkern/aiguard.rs). This is a thin wrapper over SYS_AI_SCAN so an
// app can screen ONE untrusted string and get back the rule that fired, for a
// precise message to the user.
//
// IMPORTANT, so nobody mistakes this for the enforcement point: calling this is
// VOLUNTARY. It exists for the USER-FACING message. The control that actually
// binds is in the kernel, on the async HTTPS POST path (sys_http_post_start),
// where an LLM request carrying a HIGH-severity match is refused before a byte
// reaches the wire whether or not the app ever called this.
#ifndef LIBC_AIGUARD_H
#define LIBC_AIGUARD_H

#define AIGUARD_ALLOW     0
#define AIGUARD_ANNOTATE  1
#define AIGUARD_BLOCK     2

// MIRRORS aiguard_verdict_t in kernel/security/aiguard.h. Both are size-locked;
// if you change one, change the other, the Rust mirror, and SZ_AIGUARD_VERDICT
// in kernel/rustkern/argtab.rs.
typedef struct {
    int  verdict;
    int  severity;     // 0 LOW, 1 MEDIUM, 2 HIGH, -1 clean
    int  nhits;
    int  truncated;
    int  llm;
    char rule[48];
    char category[64];
    char matched[64];
} aiguard_verdict_t;

// Screen `text`. Returns AIGUARD_* (>= 0) or a negative errno on a bad call.
// `v` is always fully written on a non-negative return.
int aiguard_check(const char *text, aiguard_verdict_t *v);

// "LOW" / "MEDIUM" / "HIGH" / "-".
const char *aiguard_sev_name(int sev);

#endif // LIBC_AIGUARD_H
