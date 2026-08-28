#ifndef _KERNEL_DRIVERS_BATTERY_H
#define _KERNEL_DRIVERS_BATTERY_H
// battery.h - the C face of the control-method battery reader (#battmeter).
//
// PRESENCE reuses uiscale_is_laptop() (gui/uiscale.c), which already scans
// the DSDT/SSDTs for a declared PNP0C0A control-method battery as its laptop
// proxy - see that file's header. This module does NOT re-scan for presence;
// it asks that one probe, so there is exactly one place in the tree that
// answers "does the firmware declare a battery" (CLAUDE.md's "reuse the
// shared primitive, do not fork a private copy" rule).
//
// PERCENTAGE/STATE/TIME are new here: they require evaluating the battery's
// _BIF/_BIX and _BST methods, not just finding a name. See rustkern/battery.rs
// for the exact (deliberately narrow) subset of AML this can safely evaluate,
// and why real hardware whose _BST is backed by the Embedded Controller will
// honestly report "unknown" rather than a guessed number - this kernel has no
// EC driver, so it has nothing to guess from.
//
// TEST INJECTION: /BATTTEST.TXT on the FAT ESP (same convention as
// /TESTINPUT.TXT and /UISCALE.TXT: a marker file, absent in a shipping
// golden with a real DSDT, present only when someone wants to exercise the
// tray/Settings UI without real battery hardware - e.g. every VM this project
// tests on, none of which has one). See battery.c for its format.

#include "../types.h"

#define BATT_ST_UNKNOWN     0
#define BATT_ST_DISCHARGING 1
#define BATT_ST_CHARGING    2
#define BATT_ST_FULL        3

// Called once at boot, after ACPI tables are parsed and the FAT ESP is
// mounted (uiscale_init() must already have run, so uiscale_is_laptop() has
// a real answer rather than the pre-boot default).
void battery_init(void);

// Re-evaluate now. Internally throttled (see battery.c) so a caller that
// polls too eagerly cannot turn this into a hot loop; safe to call from a
// syscall on every invocation. Never blocks: pure memory reads over
// already-validated ACPI tables, or a small FAT file read for the test
// injector, both bounded and fast.
void battery_refresh(void);

int32_t  battery_present(void);   // 1 yes, 0 no, -1 could not ask
int32_t  battery_percent(void);   // 0-100, or -1 unknown
int32_t  battery_state(void);     // BATT_ST_*
int32_t  battery_minutes(void);   // minutes remaining, or -1 unknown
uint32_t battery_gen(void);       // bumped whenever a refresh changes anything visible

#endif // _KERNEL_DRIVERS_BATTERY_H
