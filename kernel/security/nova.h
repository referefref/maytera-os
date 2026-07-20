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

// Scan `text` (NUL-terminated) against the embedded Nova keyword ruleset.
// Fills up to NOVA_MAX_HITS entries in `hits` (may be NULL to only count).
// Returns the number of rules that fired (0 = clean). Case-insensitive.
int nova_scan(const char *text, nova_hit_t *hits, int max_hits);

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
