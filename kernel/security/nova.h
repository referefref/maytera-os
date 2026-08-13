// nova.h - MayteraOS "Nova Guard": prompt-injection detection for the OS's LLM
// integrations (Kimi chat, aichat, chat-to-app, AI tool layer).
//
// ATTRIBUTION: the detection rules embedded here are derived from the Nova
// project's open ruleset:
//   Nova (nova-framework / nova-rules), (c) 2025 Thomas Roccia (@fr0gger_),
//   MIT License. https://github.com/Nova-Hunting/nova-rules
// MayteraOS ports the KEYWORD layer of Nova (.nov `keywords:` sections) to a
// small freestanding matcher. Nova's `semantics:` (embedding similarity) and
// `llm:` (LLM self-evaluation) layers require an embedding model / a judge LLM
// and are NOT evaluated natively here - so this raises the bar against the
// common, text-pattern injection/jailbreak families; it is not a guarantee.
//
// #449 (Cybersecurity app feature 2).
//
// ============================================================================
// #745: WIRED, at ONE chokepoint. READ THIS BEFORE CITING NOVA AS A CONTROL.
// ============================================================================
// Measured 2026-08-05 by grepping the whole tree for call sites:
//
//   nova_scan()           ZERO callers outside this file.
//   nova_worst_severity() ZERO callers.
//   nova_attribution()    ZERO callers.
//   nova_rule_count() / nova_rule_info() / nova_sev_name()
//                         called ONLY by gui/cybersecurity.c, which ENUMERATES
//                         the rules for display. Listing a rule is not applying
//                         it.
//   nova_selftest()       now called once at boot from security_init(), so the
//                         matcher is at least proven to still work.
//
// AS OF #745 THAT IS FIXED, and here is exactly what is and is not true now.
//
// nova_scan_n() is called by aiguard_screen_rs() (kernel/rustkern/aiguard.rs),
// which is called from kernel/proc/syscall.c at TWO places, both of which ship:
//   - sys_http_post_start()  the async HTTPS POST every LLM client in the tree
//                            uses. This is the chokepoint. An LLM-shaped request
//                            body carrying a HIGH-severity match is REFUSED with
//                            NET_ERR_AIGUARD before the job is queued, so not one
//                            byte reaches the wire.
//   - sys_ai_scan()          SYS_AI_SCAN, so a Ring-3 client can screen ONE
//                            untrusted string and tell the user WHICH rule fired.
//                            Informational; it is not the enforcement point.
//
// WHAT IS STILL HONESTLY TRUE:
//   * This is a KEYWORD layer. Nova's `semantics:` and `llm:` layers need an
//     embedding model or a judge LLM and are not evaluated on-device. A
//     paraphrased injection sharing no literal with the ruleset PASSES. Do not
//     describe this as prompt-injection detection; it is a keyword screen.
//   * The kernel POST funnel is not an absolute boundary. Ring 3 also has raw
//     sockets (SYS_SOCK_* 343-355), so an app determined to bypass this could
//     speak plaintext HTTP to an LLM proxy of its own. That is a different
//     threat (a hostile app) from the one this addresses (untrusted CONTENT
//     reaching a TRUSTED client that holds capability tokens).
//
// WHY THE KERNEL AND NOT THE AI CLIENT. The LLM clients are Ring-3 apps, and
// there are TWO of them: userland/libc/aiclient.c (used by aichat, terminal,
// msh, rss) and a completely separate one in userland/apps/paint/ai.c. A guard
// that lives only in a Ring-3 library is advice: an app can decline to link it,
// and a second client can simply not know it exists, which is exactly what
// paint/ai.c did. The kernel's async HTTPS POST syscall is the ONE funnel both
// already pass through, and it holds the fully-assembled request body in kernel
// memory before anything reaches the wire. Screening there covers both current
// clients AND any client that does not exist yet, by construction.
//
// Do not add a second call site "somewhere convenient". If a new route appears,
// it will reach sys_http_post_start() like every other one, and it is screened
// for free. That is the whole point of putting it there.

#ifndef NOVA_H
#define NOVA_H

#include "../types.h"

#define NOVA_SEV_LOW      0
#define NOVA_SEV_MEDIUM   1
#define NOVA_SEV_HIGH     2

#define NOVA_MAX_HITS     16

typedef struct {
    const char *rule;       // rule name (e.g. "DirectPromptInjection")
    const char *category;   // e.g. "prompt_manipulation/direct_injection"
    const char *matched;    // one literal pattern that matched (for display)
    int         severity;   // NOVA_SEV_*
} nova_hit_t;

// #745: locked against NovaHit in kernel/rustkern/aiguard.rs, which reads this
// struct across the FFI. A field added on one side and not the other would
// silently misread every hit rather than fail to build.
_Static_assert(sizeof(nova_hit_t) == 32, "nova_hit_t is FFI-locked at 32 bytes");

// Scan `text` (NUL-terminated) against the embedded Nova keyword ruleset.
// Fills up to NOVA_MAX_HITS entries in `hits` (may be NULL to only count).
// Returns the number of rules that fired (0 = clean). Case-insensitive.
int nova_scan(const char *text, nova_hit_t *hits, int max_hits);

// #745: the REENTRANT form, and the one every live caller uses. Scans exactly
// text[0..len), which may contain embedded NULs and need not be terminated.
// Takes no lock and touches no shared state, so two CPUs may scan at once.
int nova_scan_n(const char *text, int len, nova_hit_t *hits, int max_hits);

// Highest severity among fired rules, or -1 if clean.
int nova_worst_severity(const char *text);

// Number of rules in the embedded ruleset.
int nova_rule_count(void);

// Metadata for rule index i (for the Cybersecurity app's ruleset view).
// Returns 0 on success, -1 if i out of range.
int nova_rule_info(int i, const char **name, const char **category, int *severity);

// Human string for a severity level.
const char *nova_sev_name(int sev);

// Attribution string (shown in the UI, per the MIT license requirement).
const char *nova_attribution(void);

// Built-in self-test: runs known-malicious + known-benign prompts through
// nova_scan and checks the verdicts. Returns 0 if all pass, else the number
// of failures. `report` (optional) receives a short human summary.
int nova_selftest(char *report, int report_len);

#endif // NOVA_H
