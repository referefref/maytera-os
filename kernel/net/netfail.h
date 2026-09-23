// netfail.h - C face of rustkern/netfail.rs.  (#netfix2, 2026-09-03)
//
// Read the header comment in kernel/rustkern/netfail.rs first; it explains WHY
// this exists.  In one line: the owner's machines have no serial port, so the
// only thing that can ever answer "why did the browser fail" is a durable line
// in /BOOTLOG.TXT, and "it failed" is not an answer.  Each layer records a
// reason as it returns its error; the fetch chokepoint prints it as why=.
//
// USAGE, and the ONE rule that matters:
//
//   NETFAIL(NF_DNS_SILENT, 0);        // at the site that KNOWS.  Overwrites.
//   NETFAIL_WEAK(NF_TCP_FAILED, rc);  // at an OUTER layer.  Fills only a gap.
//
// The layers nest (https_get -> https_connect -> dns_resolve).  The inner layer
// knows the resolver went silent; by the time that has propagated out, the outer
// layer only knows "connect returned -1".  If outer layers used the strong form
// they would relabel every DNS fault in the OS as a TCP fault, which does not
// merely lose detail, it sends the reader to the wrong subsystem.  So: strong at
// the bottom, weak on the way out.
//
// SAFE FROM ANY CONTEXT.  Atomics over static storage; no allocation, no lock,
// no wait, no print.  That is not incidental: dns_send() is reachable from the
// #549 no-block TX path (interrupts off, net_lock held), where a print or a
// wait would deadlock the machine we are trying to diagnose.

#ifndef NETFAIL_H
#define NETFAIL_H

#include "../types.h"

// These MUST match the constants in rustkern/netfail.rs.  There is no
// compile-time way to bind a C #define to a Rust const, so the binding is
// checked AT BOOT by netfail_abi_check() below, which compares names and phases
// through the Rust accessors for a reason in every group.  A silent drift here
// would make every log line confidently wrong, which is worse than no line.
#define NF_NONE                     0u

#define NF_BAD_URL                  1u
#define NF_NO_CARRIER               2u
#define NF_NO_ADDRESS               3u
#define NF_IF_FAULTY                4u
#define NF_OOM                      5u

#define NF_DNS_NO_SERVER            10u
#define NF_DNS_LINK_DOWN            11u
#define NF_DNS_SILENT               12u
#define NF_DNS_NXDOMAIN             13u
#define NF_DNS_SERVFAIL             14u
#define NF_DNS_REFUSED              15u
#define NF_DNS_NO_A                 16u
#define NF_DNS_BAD_NAME             17u
#define NF_DNS_NEG_CACHED           18u

#define NF_ARP_UNRESOLVED           20u
#define NF_SOCKET                   21u
#define NF_TCP_REFUSED              22u
#define NF_TCP_TIMEOUT              23u
#define NF_TCP_FAILED               24u

#define NF_TLS_CTX                  30u
#define NF_TLS_HANDSHAKE            31u
#define NF_TLS_CERT_EXPIRED         32u
#define NF_TLS_CERT_NOT_YET_VALID   33u
#define NF_TLS_CLOCK_WRONG          34u
#define NF_TLS_CERT_UNTRUSTED       35u
#define NF_TLS_CERT_SIGNATURE       36u
#define NF_TLS_CERT_NAME            37u
#define NF_TLS_CERT_BAD             38u

#define NF_HTTP_SEND                40u
#define NF_HTTP_NO_STATUS           41u
#define NF_HTTP_STATUS              42u
#define NF_HTTP_TRUNCATED           43u
#define NF_HTTP_CHUNKED             44u
#define NF_HTTP_REDIRECT_BLOCKED    45u
#define NF_HTTP_RECV_TIMEOUT        46u
#define NF_HTTP_TOO_BIG             47u

// rustkern/netfail.rs
void        netfail_note_rs(uint32_t pid, uint32_t reason, int detail);
void        netfail_note_weak_rs(uint32_t pid, uint32_t reason, int detail);
uint32_t    netfail_take_rs(uint32_t pid, int *detail_out);
void        netfail_clear_rs(uint32_t pid);
const char *netfail_name_rs(uint32_t reason);
uint32_t    netfail_phase_rs(uint32_t reason);
void        netfail_counters_rs(uint32_t *set, uint32_t *dropped);
uint32_t    netfail_selftest_rs(uint32_t *checks_out);

uint32_t proc_current_pid(void);

// Clock plausibility (see rustkern/netfail.rs).  0 ok, 1 before the build date,
// 2 absurdly far ahead, 3 not known yet.
#define NF_CLOCK_OK              0u
#define NF_CLOCK_BEFORE_BUILD    1u
#define NF_CLOCK_ABSURDLY_AHEAD  2u
#define NF_CLOCK_UNKNOWN         3u
void     netfail_clock_set_build_rs(uint32_t y, uint32_t m, uint32_t d);
uint32_t netfail_clock_check_rs(uint32_t y, uint32_t m, uint32_t d);

// Sticky per-owner outcome, so the process that has to DRAW the message can ask
// for it after the worker thread that discovered it has exited.
void     netfail_publish_rs(uint32_t owner, uint32_t reason, int detail);
uint32_t netfail_published_rs(uint32_t owner, int *detail_out);

// Reads the RTC and returns non-zero when this machine's clock cannot possibly
// be right.  Defined in net/netclock.c.
int  cert_clock_is_implausible(void);
// Emits the durable [CLOCK] line once at boot and seeds the build date.
void netclock_boot_report(void);

// The reason is keyed by the calling thread, because several fetches can be in
// flight at once (the browser's worker, the App Store's, a widget's sync GET)
// and a single global slot would attribute one thread's TLS failure to another
// thread's DNS failure.  A confidently wrong log line is worse than a missing
// field, because it gets acted on.
static inline void netfail_note(uint32_t reason, int detail) {
    netfail_note_rs(proc_current_pid(), reason, detail);
}
static inline void netfail_note_weak(uint32_t reason, int detail) {
    netfail_note_weak_rs(proc_current_pid(), reason, detail);
}
static inline uint32_t netfail_take(int *detail_out) {
    return netfail_take_rs(proc_current_pid(), detail_out);
}
static inline void netfail_clear(void) {
    netfail_clear_rs(proc_current_pid());
}

#define NETFAIL(r, d)       netfail_note((r), (d))
#define NETFAIL_WEAK(r, d)  netfail_note_weak((r), (d))

// Boot-time check that the C constants above still name the same things the
// Rust module thinks they do.  Returns 0 on agreement.  Cheap, and it runs on
// the golden: an ABI drift here is exactly the kind of fault that produces a
// plausible-looking log that points at the wrong subsystem forever.
int netfail_abi_check(void);

#endif // NETFAIL_H
