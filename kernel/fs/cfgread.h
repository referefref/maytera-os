// cfgread.h - #192: the shared policy for LOGGING a config-file read outcome.
//
// The implementation is Rust (kernel/rustkern/cfgread.rs); read the header
// comment there for why this exists. In one line: an absent /CONFIG file is the
// NORMAL state of an unconfigured setting, and it used to be reported as
// "read FAILED/empty ... giving up", three times, every two seconds, forever.
//
// HOW TO USE IT. After a config-file read, classify what happened and ask this
// whether to speak:
//
//     int act = cfgread_report_rs(path, -1, CFG_OUTCOME_ABSENT);
//     if (act == CFG_LOG_NOTE)
//         bootlog_write("[CFG] %s: not present; using defaults", path);
//
// The WORDING is yours, because you know what the file is for. The RATE and the
// LEVEL are the policy's, because that is what was wrong in three callers at
// once. Emit AT MOST the one line you are told to, and never emit one you were
// not: an unconditional print beside this call re-creates the flood.
//
// A genuine fault is NOT muted. CFG_OUTCOME_IOERR (the file IS there and would
// not read) produces CFG_LOG_WARN for the first few occurrences, then one
// CFG_LOG_SUPPRESSED line, and re-arms as soon as the file reads again.
#ifndef MAYTERA_CFGREAD_H
#define MAYTERA_CFGREAD_H

#include "../types.h"

// What happened to the read. MIRRORED IN rustkern/cfgread.rs and checked at
// boot by cfgread_abi_check_rs() (see main.c's [CFG] self-test line).
#define CFG_OUTCOME_OK      0   // usable bytes came back
#define CFG_OUTCOME_ABSENT  1   // the path is not present on any volume: NORMAL
#define CFG_OUTCOME_IOERR   2   // present, and the read still failed: a FAULT

// What to do about it.
#define CFG_LOG_NONE        0   // say nothing
#define CFG_LOG_NOTE        1   // one informational line
#define CFG_LOG_WARN        2   // one loud line
#define CFG_LOG_SUPPRESSED  3   // one line saying further warnings are withheld

// plen < 0 means "path is NUL-terminated", which is what every caller passes.
int32_t cfgread_report_rs(const char *path, int32_t plen, int32_t outcome);

// Boot-time ABI lock: hand it the macros above, get 0 if Rust agrees.
int32_t cfgread_abi_check_rs(int32_t ok, int32_t absent, int32_t ioerr,
                             int32_t none, int32_t note, int32_t warn,
                             int32_t supp);

// 0 = pass. Provably RED via `make CFGTESTFAIL=1`.
int32_t cfgread_selftest_rs(uint32_t *out_checks);

#endif // MAYTERA_CFGREAD_H
