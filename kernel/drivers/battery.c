// battery.c - boot bring-up and live refresh for the control-method battery
// reader (#battmeter). The AML evaluation and the percent/state/minutes
// arithmetic live in rustkern/battery.rs; read that file's header first for
// the safety-motivated scope line (constant-Return AML only, no EC access,
// because this kernel has no EC driver to back one). This file is the
// plumbing: where presence comes from (reused, not re-scanned), where the
// method bytes come from, the test-injection escape hatch, and the small
// change-detection cache a syscall reads from.

#include "battery.h"
#include "acpi.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "../mm/heap.h"
#include "../cpu/mono.h"

extern fat_fs_t g_fat_fs;

// Reused from gui/uiscale.c: the ONE probe for "does the firmware declare a
// PNP0C0A control-method battery". Do not re-scan for presence here.
extern int32_t uiscale_is_laptop(void);

// Mirrors rustkern::battery::BatteryReport's #[repr(C)] layout exactly - four
// i32 fields, no padding surprises on this ABI. _Static_assert below locks it.
typedef struct {
    int32_t present;
    int32_t percent;
    int32_t state;
    int32_t minutes;
} battery_report_t;
_Static_assert(sizeof(battery_report_t) == 16, "battery_report_t must match rustkern::battery::BatteryReport");

// rustkern/battery.rs
extern int32_t battery_eval_method_rs(const uint8_t *base, uint32_t len,
                                       uint8_t n0, uint8_t n1, uint8_t n2, uint8_t n3,
                                       uint64_t *out, uint32_t out_cap, uint32_t *out_n);
extern void battery_compute_rs(int32_t present,
                                int32_t have_bif, const uint64_t *bif, uint32_t bif_n,
                                int32_t have_bst, const uint64_t *bst, uint32_t bst_n,
                                battery_report_t *out);
extern uint32_t battery_selftest_rs(void);

static int g_trustworthy = 0;      // selftest passed; evaluation is used at all
static battery_report_t g_rep = { -1, -1, BATT_ST_UNKNOWN, -1 };
static uint32_t g_gen = 0;
static uint64_t g_next_refresh_us = 0;   // internal throttle floor
static int g_injection_logged = 0;       // log the injector's arrival once, not every refresh
// #battpop: log the real-ACPI evaluation OUTCOME to /BOOTLOG.TXT once, not
// every ~250ms-throttled refresh. Before this, battery_init()'s final
// "present=... pct=... state=... minutes=..." summary went to kprintf()
// (serial) ONLY - never bootlog_write() - so a machine that shipped
// "unknown status unknown" with no live serial capture (the normal case for
// an end user's own laptop) left NO durable evidence of which branch was
// taken: self-test failure, no _BIF/_BST evaluable at all, or (the expected
// case per this file's header - most real firmware backs _BST with an EC
// Field read this bounded evaluator will not attempt) _BIF evaluable but
// _BST not. /BOOTLOG.TXT is exactly the artifact this project's own "get the
// evidence from the hardware, not from a VM" rule asks for, so it needs to
// actually carry the answer.
static int g_eval_logged = 0;

#define BATT_REFRESH_MIN_US 250000ULL   // at most 4 real re-scans/sec, however hard a caller polls

// ---------------------------------------------------------------------------
// TEST INJECTION: /BATTTEST.TXT on the FAT ESP.
//
// Same convention as /TESTINPUT.TXT (drivers/testinput.c) and /UISCALE.TXT
// (gui/uiscale.c): a marker file at the root of the boot partition. Absent on
// every real machine and every test VM this project has (none of which has a
// real ACPI battery to test against), so a shipping golden with real hardware
// behind it never has this file and always goes through actual AML
// evaluation. Format, one key=value per line:
//   PRESENT=1|0|-1
//   PCT=<0-100 or -1>
//   STATE=DISCHARGING|CHARGING|FULL|UNKNOWN
//   MINUTES=<n or -1>
// A key that is missing or unparsable keeps that field's PREVIOUS value
// (defaults above on first read), so a hand-edited test file that only sets
// PCT= still works without having to restate everything.
// ---------------------------------------------------------------------------
static int parse_kv_int(const char *buf, uint32_t sz, const char *key, int defval) {
    uint32_t klen = (uint32_t)strlen(key);
    for (uint32_t i = 0; i + klen <= sz; i++) {
        if (strncmp(&buf[i], key, klen) != 0) continue;
        uint32_t j = i + klen;
        int neg = 0;
        if (j < sz && buf[j] == '-') { neg = 1; j++; }
        int v = 0, any = 0;
        while (j < sz && buf[j] >= '0' && buf[j] <= '9') { v = v * 10 + (buf[j] - '0'); j++; any = 1; }
        if (any) return neg ? -v : v;
    }
    return defval;
}

static int parse_kv_state(const char *buf, uint32_t sz, int defval) {
    for (uint32_t i = 0; i + 6 <= sz; i++) {
        if (strncmp(&buf[i], "STATE=", 6) != 0) continue;
        uint32_t j = i + 6;
        if (j + 11 <= sz && strncmp(&buf[j], "DISCHARGING", 11) == 0) return BATT_ST_DISCHARGING;
        if (j + 8  <= sz && strncmp(&buf[j], "CHARGING", 8) == 0)  return BATT_ST_CHARGING;
        if (j + 4  <= sz && strncmp(&buf[j], "FULL", 4) == 0)      return BATT_ST_FULL;
        if (j + 7  <= sz && strncmp(&buf[j], "UNKNOWN", 7) == 0)   return BATT_ST_UNKNOWN;
    }
    return defval;
}

// Returns 1 if the injector was present and applied, 0 otherwise (fall
// through to real AML evaluation).
static int battery_try_injection(battery_report_t *out) {
    if (fat_exists(&g_fat_fs, "/BATTTEST.TXT") != 1) {
        g_injection_logged = 0;
        return 0;
    }
    uint32_t sz = 0;
    char *buf = (char *)fat_read_file(&g_fat_fs, "/BATTTEST.TXT", &sz);
    if (!buf) return 0;

    out->present = parse_kv_int(buf, sz, "PRESENT=", out->present);
    out->percent = parse_kv_int(buf, sz, "PCT=", out->percent);
    out->minutes = parse_kv_int(buf, sz, "MINUTES=", out->minutes);
    out->state   = parse_kv_state(buf, sz, out->state);
    kfree(buf);

    if (!g_injection_logged) {
        kprintf("[BATTERY] TEST INJECTION ACTIVE via /BATTTEST.TXT: present=%d pct=%d "
                "state=%d minutes=%d (SYNTHETIC - delete this file for real hardware "
                "readings)\n", out->present, out->percent, out->state, out->minutes);
        g_injection_logged = 1;
    }
    return 1;
}

// _BIX carries an extra leading Revision field before the _BIF-shaped layout
// (PowerUnit, DesignCapacity, LastFullChargeCapacity, ...), so its elements
// are re-indexed here into the same shape battery_compute_rs expects. Returns
// the number of usable (shifted) elements written to `out` (capped at cap).
static uint32_t bix_to_bif_shape(const uint64_t *bix, uint32_t bix_n, uint64_t *out, uint32_t cap) {
    if (bix_n < 2) return 0;
    uint32_t n = bix_n - 1;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++) out[i] = bix[i + 1];
    return n;
}

static int battery_eval_from_acpi(battery_report_t *out) {
    int32_t present = uiscale_is_laptop();
    out->present = present;
    out->percent = -1;
    out->state = BATT_ST_UNKNOWN;
    out->minutes = -1;

    if (present != 1) return 1; // answered: absent or unknown, nothing more to evaluate

    if (!g_trustworthy) {
        // The AML evaluator's own self-test failed; do not trust anything it
        // would report. Presence still stands (that came from uiscale, whose
        // OWN self-test gates it independently), but percent/state/minutes
        // stay unknown rather than risk a mis-parsed number.
        return 1;
    }

    uint64_t bif_raw[16]; uint32_t bif_raw_n = 0;
    uint64_t bif[16]; uint32_t bif_n = 0;
    int have_bif = 0;
    uint64_t bst[16]; uint32_t bst_n = 0;
    int have_bst = 0;

    uint64_t base = 0; uint32_t len = 0;
    // Try the DSDT, then every SSDT (bounded at 32, matching
    // uiscale_probe_laptop()'s own bound for the same reason: real firmware
    // splits device declarations across several SSDTs).
    for (int i = -1; i < 32 && !(have_bif && have_bst); i++) {
        if (i == -1) {
            if (!acpi_get_dsdt(&base, &len)) continue;
        } else {
            if (!acpi_get_ssdt(i, &base, &len)) break;
        }
        const uint8_t *tbl = (const uint8_t *)(uintptr_t)base;
        if (!have_bif) {
            if (battery_eval_method_rs(tbl, len, '_','B','I','X', bif_raw, 16, &bif_raw_n) == 1) {
                bif_n = bix_to_bif_shape(bif_raw, bif_raw_n, bif, 16);
                have_bif = (bif_n >= 3);
            } else if (battery_eval_method_rs(tbl, len, '_','B','I','F', bif, 16, &bif_n) == 1) {
                have_bif = (bif_n >= 3);
            }
        }
        if (!have_bst) {
            if (battery_eval_method_rs(tbl, len, '_','B','S','T', bst, 16, &bst_n) == 1) {
                have_bst = (bst_n >= 3);
            }
        }
    }

    // #battpop: see g_eval_logged's declaration above for why this needs to
    // reach /BOOTLOG.TXT and not just serial. Logged once, on the FIRST real
    // (non-injected) evaluation only - present/have_bif/have_bst do not
    // change without a firmware reload, so nothing is lost by not repeating
    // this on every throttled refresh.
    if (!g_eval_logged) {
        g_eval_logged = 1;
        bootlog_write("[BATTERY] ACPI eval: have_bif=%d have_bst=%d%s\n",
                       have_bif, have_bst,
                       (!have_bst) ? " (no usable _BST - percent/state/minutes "
                                     "report unknown; likely an EC-backed _BST "
                                     "this bounded evaluator will not attempt, "
                                     "see rustkern/battery.rs)" : "");
    }

    battery_compute_rs(present, have_bif, bif, bif_n, have_bst, bst, bst_n, out);
    return 1;
}

void battery_init(void) {
    uint32_t st = battery_selftest_rs();
    g_trustworthy = (st == 0);
    if (!g_trustworthy) {
        kprintf("[BATTERY] AML evaluator SELF-TEST FAILED mask=0x%x - percentage/state "
                "will report unknown even if a battery is present. See "
                "rustkern/battery.rs for what each bit means.\n", st);
        bootlog_write("[BATTERY] self-test FAILED: percent/state disabled\n");
    } else {
        kprintf("[BATTERY] AML evaluator self-test PASS\n");
    }
    battery_refresh();
    kprintf("[BATTERY] present=%s pct=%d state=%d minutes=%d\n",
            g_rep.present == 1 ? "yes" : (g_rep.present == 0 ? "no" : "unknown"),
            g_rep.percent, g_rep.state, g_rep.minutes);
}

void battery_refresh(void) {
    uint64_t now = mono_ready() ? mono_us() : 0;
    if (now != 0 && g_next_refresh_us != 0 && now < g_next_refresh_us) return;
    if (now != 0) g_next_refresh_us = now + BATT_REFRESH_MIN_US;

    battery_report_t fresh = g_rep;
    if (!battery_try_injection(&fresh)) {
        battery_eval_from_acpi(&fresh);
    }

    if (fresh.present != g_rep.present || fresh.percent != g_rep.percent ||
        fresh.state != g_rep.state || fresh.minutes != g_rep.minutes) {
        g_gen++;
    }
    g_rep = fresh;
}

int32_t  battery_present(void) { return g_rep.present; }
int32_t  battery_percent(void) { return g_rep.percent; }
int32_t  battery_state(void)   { return g_rep.state; }
int32_t  battery_minutes(void) { return g_rep.minutes; }
uint32_t battery_gen(void)     { return g_gen; }
