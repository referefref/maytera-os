// netfail.c - the boot-time binding check for rustkern/netfail.rs.
// (#netfix2, 2026-09-03)
//
// WHY THIS FILE IS C, given the 2026-07-16 Rust-first rule.  Its entire job is
// to compare the C #defines in net/netfail.h against what the Rust module
// believes those numbers mean.  A Rust version could not see the C macros, so it
// could not perform the comparison; the check must live on the side it is
// checking.  That is a structural reason, not a "the surrounding code is C" one.
//
// WHY IT EXISTS AT ALL.  There is no compile-time way to bind a C #define to a
// Rust `const`.  If they drift, nothing breaks and nothing warns: the fetch path
// keeps writing durable log lines, they just name the wrong fault.  That is the
// worst possible failure for a diagnostic, because it is acted on.  A three-
// microsecond boot check removes the class.

#include "netfail.h"
#include "http_progress.h"
#include "../string.h"
#include "../fs/bootlog.h"   // NOT a private extern: see persist-extern-gate

extern int kprintf(const char *fmt, ...);

struct nf_binding {
    uint32_t    reason;
    const char *name;
    int         phase;
};

// One reason from EVERY group, plus the two that carry the most weight in a
// real report (a silent resolver and a wrong clock).  Checking one per group is
// what catches a whole block being renumbered, which is how this actually
// drifts; checking all 33 would be no stronger and would be a second copy of
// the table to keep in step.
static const struct nf_binding nf_bindings[] = {
    { NF_BAD_URL,           "BAD-URL",                                  HTTP_PHASE_IDLE       },
    { NF_NO_CARRIER,        "NO-CARRIER-cable-or-wifi-down",            HTTP_PHASE_IDLE       },
    { NF_DNS_SILENT,        "DNS-RESOLVER-ANSWERED-NOTHING",            HTTP_PHASE_RESOLVING  },
    { NF_DNS_NXDOMAIN,      "DNS-NXDOMAIN-no-such-host",                HTTP_PHASE_RESOLVING  },
    { NF_TCP_TIMEOUT,       "TCP-CONNECT-TIMEOUT",                      HTTP_PHASE_CONNECTING },
    { NF_TLS_HANDSHAKE,     "TLS-HANDSHAKE-FAILED",                     HTTP_PHASE_TLS        },
    { NF_TLS_CLOCK_WRONG,   "TLS-FAILED-BECAUSE-SYSTEM-CLOCK-IS-WRONG", HTTP_PHASE_TLS        },
    { NF_HTTP_SEND,         "HTTP-REQUEST-SEND-FAILED",                 HTTP_PHASE_SENDING    },
    { NF_HTTP_NO_STATUS,    "HTTP-NO-STATUS-LINE-333",                  HTTP_PHASE_RECEIVING  },
};

int netfail_abi_check(void) {
    int bad = 0;
    for (unsigned i = 0; i < sizeof(nf_bindings) / sizeof(nf_bindings[0]); i++) {
        const struct nf_binding *b = &nf_bindings[i];
        const char *rn = netfail_name_rs(b->reason);
        if (!rn || strcmp(rn, b->name) != 0) {
            kprintf("[NETFAIL] ABI DRIFT: C reason %u expects '%s', Rust says '%s'\n",
                    (unsigned)b->reason, b->name, rn ? rn : "(null)");
            bad++;
            continue;
        }
        if ((int)netfail_phase_rs(b->reason) != b->phase) {
            kprintf("[NETFAIL] ABI DRIFT: reason %s phase C=%d Rust=%u\n",
                    b->name, b->phase, (unsigned)netfail_phase_rs(b->reason));
            bad++;
        }
    }
    return bad;
}

// Run the Rust self-test and the binding check together, once, before net_init().
// DURABLE, not kprintf: this module's whole purpose is to be readable on a
// machine with no serial port, and an instrument that cannot report its own
// health on that machine is the exact failure it was written to fix.
void netfail_boot_selftest(void) {
    uint32_t checks = 0;
    uint32_t fail = netfail_selftest_rs(&checks);
    int abi = netfail_abi_check();
    if (fail == 0 && abi == 0) {
        bootlog_write("[NETFAIL] self-test PASS mask=0x0 checks=%u bindings=%u",
                      (unsigned)checks,
                      (unsigned)(sizeof(nf_bindings) / sizeof(nf_bindings[0])));
    } else {
        bootlog_write("[NETFAIL] self-test FAIL mask=0x%x checks=%u abi-drift=%d "
                      "- every why= field in this log is UNTRUSTWORTHY",
                      (unsigned)fail, (unsigned)checks, abi);
    }
}
