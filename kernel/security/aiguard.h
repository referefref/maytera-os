// aiguard.h - #745: the kernel-side prompt-injection screen for LLM traffic.
//
// DECLARATIONS ONLY. The policy lives in kernel/rustkern/aiguard.rs (new kernel
// code is Rust per the standing rule); the matcher lives in security/nova.c.
// Nothing here contains logic, so there is no C to justify.
//
// WHY THIS EXISTS: nova.c has shipped a 16-rule prompt-injection matcher since
// #449 with ZERO callers. Listing the rules in a UI is not applying them, and a
// self-test proves the matcher still works while screening nothing. This header
// is the seam that finally gives it a caller in a translation unit that SHIPS
// (proc/syscall.c, on the async HTTPS POST path every LLM client uses).
#ifndef AIGUARD_H
#define AIGUARD_H

#include "../types.h"

// Verdicts. Mirrors AIGUARD_* in rustkern/aiguard.rs.
#define AIGUARD_ALLOW     0   // not an LLM request, or nothing fired
#define AIGUARD_ANNOTATE  1   // fired below the block threshold (LOW/MEDIUM)
#define AIGUARD_BLOCK     2   // fired at HIGH severity: refuse

// Result record. ALSO the SYS_AI_SCAN out-parameter, so userland has the same
// struct (userland/libc/aiguard.h) and both sides are size-locked.
//
// APPEND ONLY, and update BOTH _Static_asserts plus the userland mirror plus
// SZ_AIGUARD_VERDICT in rustkern/argtab.rs if you ever grow it. A struct that
// crosses the syscall boundary has FOUR copies of its size in this tree (#745).
typedef struct {
    int  verdict;      // AIGUARD_*
    int  severity;     // NOVA_SEV_* of the worst hit, or -1 when clean
    int  nhits;        // how many rules fired
    int  truncated;    // 1 if the scan stopped at AIGUARD_SCAN_CAP
    int  llm;          // 1 if the payload was recognised as an LLM request
    char rule[48];     // worst hit's rule name, NUL-terminated
    char category[64]; // worst hit's category
    char matched[64];  // the literal that matched (or "base64-blob")
} aiguard_verdict_t;

_Static_assert(sizeof(aiguard_verdict_t) == 196,
               "aiguard_verdict_t is size-locked against rustkern/aiguard.rs, "
               "userland/libc/aiguard.h and SZ_AIGUARD_VERDICT in argtab.rs");

// Implemented in rustkern/aiguard.rs.
//
// aiguard_screen_rs:      screen ARBITRARY text (behind SYS_AI_SCAN).
// aiguard_screen_post_rs: screen an outbound HTTPS POST body, returning
//                         AIGUARD_ALLOW immediately when the body is not an
//                         LLM request. This is the chokepoint call.
// aiguard_selftest_rs:    boot self-test; formats a multi-line report and
//                         returns the number of FAILED cases.
extern int aiguard_screen_rs(const void *text, uint64_t len, aiguard_verdict_t *out);
extern int aiguard_screen_post_rs(const void *body, uint64_t len, aiguard_verdict_t *out);
extern int aiguard_selftest_rs(char *report, uint64_t cap);

#endif // AIGUARD_H
