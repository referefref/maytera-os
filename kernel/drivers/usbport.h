// drivers/usbport.h - #307/#433 follow-up: when to STOP retrying a root port.
//
// The implementation is Rust (kernel/rustkern/usbport.rs); read that file's
// header comment for the fault, the measurement and the reasoning. This header
// is the ABI, and its constants are locked against the Rust module's at boot by
// usbport_abi_check_rs() (see main.c's [USBPORT] self-test line).
//
// THE ONE THING TO KNOW BEFORE CHANGING ANY OF THIS: the owner BOOTS FROM A USB
// STICK. Every rule in here is BEHAVIOURAL (this port failed to enumerate) and
// never POSITIONAL (this port is on the left). Nothing in this kernel knows
// which physical socket a root-port number is wired to, so a positional rule is
// a guess, and the cost of guessing wrong is an unbootable machine.

#ifndef MAYTERA_USBPORT_H
#define MAYTERA_USBPORT_H

#include "../types.h"    // NOT <stdint.h>: see the note below

// WHY "../types.h" AND NOT <stdint.h>, WHICH IS WHAT YOU WOULD WRITE.
//
// The kernel builds -nostdinc, so <stdint.h> "cannot" resolve. It resolves
// anyway: the vendored decoders ship their own compat copies (media/faad2/
// compat/stdint.h, media/opus/compat/stdint.h) and those directories are on the
// include path. Their uint64_t is `unsigned long long` in one arm and
// `unsigned long` in another; kernel/types.h:36 is `unsigned long long`.
//
// The failure is not in this file and does not name it. Writing <stdint.h> here
// broke the build in main.c, a thousand lines from anything to do with USB,
// with "conflicting types for boot_info_get_total_memory ... uint64_t {aka long
// unsigned int} vs uint64_t {aka long long unsigned int}". Every kernel header
// in this tree includes "../types.h"; follow that and not C convention.


// Table geometry. MIRRORED IN rustkern/usbport.rs.
#define USBPORT_MAX_CTRL   4     // == MAX_XHCI_CONTROLLERS in drivers/xhci.c
#define USBPORT_MAX_PORT   256   // == the g_port_enumerated[][256] geometry

// Full retry budgets a connected port may burn before it is retired.
#define USBPORT_GIVEUP_BUDGETS 6

// How many times a port may be retired and then revived by a connect-status
// change before the revival itself is refused. The retry budget above bounds
// ONE arming; without this the CCS edge resets that budget without limit, which
// is not a bound. MEASURED on the owner's 2026-08-28 capture: his faulty port
// asserted a genuine CCS edge with nothing plugged in and burned a second full
// budget in the same boot. Worst case is now 3 * 6 * 4 = 72 attempts per boot.
// Nothing here is persisted, so a reboot re-arms a hard-retired port.
#define USBPORT_MAX_RETIRES 3

// Port states (usbport_state_rs).
#define USBPORT_ST_ACTIVE    0   // enumeration may be attempted
#define USBPORT_ST_TERMINAL  1   // retired; only a CCS edge re-arms it
#define USBPORT_ST_CFGOFF    2   // listed in /USBPORT.CFG; a replug does NOT re-arm
#define USBPORT_ST_HARD      3   // retired MAX_RETIRES times; a CCS edge no longer re-arms

// usbport_budget_failed_rs return codes.
#define USBPORT_GIVEUP_NO       0  // still under budget
#define USBPORT_GIVEUP_NOW      1  // just retired: caller logs, ONCE, durably
#define USBPORT_GIVEUP_ALREADY  2  // was already retired: caller stays quiet
#define USBPORT_GIVEUP_FINAL    3  // retired for the LAST time: caller logs a louder line, once

// 1 = the caller may attempt to enumerate this port, 0 = it must not.
// An out-of-range port answers 1: this governor only ever REMOVES work, so a
// geometry mistake must never be able to suppress the boot device's port.
int32_t usbport_should_enumerate_rs(int32_t ctrl, int32_t port);

// Record an exhausted retry budget. still_connected is a freshly-read CCS bit;
// a budget that ended because the device was unplugged is not counted.
int32_t usbport_budget_failed_rs(int32_t ctrl, int32_t port, int32_t still_connected);

// A device on this port enumerated. Clears the budget count.
void usbport_enum_ok_rs(int32_t ctrl, int32_t port);

// The hardware reported a real connect-status change. Returns 1 if that revived
// a retired port (so the caller can log it). Does not revive a /USBPORT.CFG one.
int32_t usbport_connect_changed_rs(int32_t ctrl, int32_t port);

// Apply a /USBPORT.CFG entry. Returns 1 if the state changed.
int32_t usbport_config_disable_rs(int32_t ctrl, int32_t port);

// Current state / burned-budget count, for logging.
int32_t usbport_state_rs(int32_t ctrl, int32_t port);
int32_t usbport_budgets_rs(int32_t ctrl, int32_t port);
int32_t usbport_retires_rs(int32_t ctrl, int32_t port);

// Parse /USBPORT.CFG. Writes up to max (ctrl, port) pairs; ctrl == -1 means
// "every controller". Returns the count written, or -1 on a bad argument.
// port values are 0-based on return (the FILE is 1-based; see the Rust header).
int32_t usbport_parse_cfg_rs(const uint8_t *buf, int32_t len,
                             int32_t *out_ctrl, int32_t *out_port, int32_t max);

// Locks this header's constants against the Rust module's. 1 = match.
int32_t usbport_abi_check_rs(int32_t max_ctrl, int32_t max_port, int32_t budgets,
                             int32_t st_active, int32_t st_terminal,
                             int32_t st_cfgoff, int32_t max_retires,
                             int32_t st_hard);

// Boot self-test. 1 = all checks passed; *out_checks receives the count.
int32_t usbport_selftest_rs(uint32_t *out_checks);

#endif  // MAYTERA_USBPORT_H
