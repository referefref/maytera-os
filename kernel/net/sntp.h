// net/sntp.h - #797 SNTP (RFC 4330) one-shot time client
//
// Scope, stated plainly: this is SNTP, not NTP. It performs ONE unicast
// request/response exchange and sets the RTC from the answer. It does NOT
// discipline the clock: no offset/delay filtering, no frequency correction, no
// poll interval, no clock slewing, no peer selection. A one-shot step is what
// the first-boot wizard and the Settings date/time panel need, and pretending
// otherwise would be a much larger subsystem.
//
// The validation that decides whether a reply is allowed to move the clock
// lives in rustkern/sntp.rs (new kernel code, so Rust per the 2026-07-16 rule).
// This file is the transport: resolve, bind, send, wait, and apply.

#ifndef NET_SNTP_H
#define NET_SNTP_H

#include "../types.h"

#define SNTP_PORT            123
#define SNTP_PKT_LEN         48

// Default server. pool.ntp.org is the vendor-neutral NTP Pool Project round
// robin, chosen deliberately over any single vendor's pool (time.google.com,
// time.apple.com, time.windows.com) so the default does not hand every
// MayteraOS machine's boot time to one company. It is still a default, not a
// policy: every caller may pass its own server, and the first-boot wizard's
// "NTP server" field is exactly that caller.
#define SNTP_DEFAULT_SERVER  "pool.ntp.org"

// The default is a short ORDERED FALLBACK LIST, not a single name, and the
// reason is measured rather than defensive. Our DNS resolver returns ONE A
// record, so "pool.ntp.org" in practice means "whichever single address the
// resolver happened to return". On 2026-08-09 that was 44.32.200.249 and then
// 44.32.200.122, and neither answers NTP from that network at all (confirmed
// from a second host, so it was the pool member, not MayteraOS). A one-shot
// client that gives up there is a client that does not work.
//
// The list is tried IN ORDER and stops at the first server whose reply passes
// validation. Each entry gets an equal share of the caller's budget, so the
// total wait is still the budget the caller asked for.
//
// This applies ONLY when the caller did not name a server. An EXPLICIT server
// is queried alone: if the user typed a host into the wizard's NTP field,
// quietly asking someone else instead would be wrong.
#define SNTP_FALLBACK_1      "time.cloudflare.com"
#define SNTP_FALLBACK_2      "time.nist.gov"
#define SNTP_DEFAULT_COUNT   3

// Default budget for the whole exchange, including DNS. Chosen so a user
// staring at the wizard gets an answer rather than a stall; see sntp_sync().
#define SNTP_DEFAULT_TIMEOUT_MS  5000u

// ---------------------------------------------------------------------------
// Status codes.
//
// -1..-9 are produced by the Rust validator (rustkern/sntp.rs) and MUST stay in
// step with the SNTP_E_* constants there. -20 and below are transport failures
// raised by this file. They are distinct on purpose: "no answer" and "an answer
// that failed validation" are different problems with different fixes, and a
// wizard that shows one message for both teaches the user nothing.
// ---------------------------------------------------------------------------
#define SNTP_OK               0
#define SNTP_E_ARG           (-1)   // null pointer / impossible length
#define SNTP_E_SHORT         (-2)   // reply shorter than 48 bytes
#define SNTP_E_LI            (-3)   // leap indicator 3: server not synchronised
#define SNTP_E_VERSION       (-4)   // version outside 1..=4
#define SNTP_E_MODE          (-5)   // mode != 4 (server)
#define SNTP_E_STRATUM       (-6)   // stratum 0 (kiss-o'-death) or > 15
#define SNTP_E_NONCE         (-7)   // originate timestamp != the nonce we sent
#define SNTP_E_ZEROTS        (-8)   // transmit timestamp is zero
#define SNTP_E_RANGE         (-9)   // decoded date outside the sanity window

#define SNTP_E_NOLINK       (-20)   // no carrier: INSTANT no-op, never a stall (#381)
#define SNTP_E_NONET        (-21)   // carrier up but no IP configured yet
#define SNTP_E_RESOLVE      (-22)   // could not resolve the server name
#define SNTP_E_BIND         (-23)   // could not bind a local UDP port
#define SNTP_E_SEND         (-24)   // udp_send() failed on every attempt
#define SNTP_E_TIMEOUT      (-25)   // no reply within the budget
#define SNTP_E_BUSY         (-26)   // another sync is already in flight
#define SNTP_E_SELFTEST     (-27)   // the Rust validator failed its own vectors

// Result of a successful sync. #[repr(C)] twin is SntpResult in
// rustkern/sntp.rs; the _Static_assert in net/sntp.c locks the size.
typedef struct {
    int32_t  status;
    int32_t  year;
    int32_t  month;    // 1..12
    int32_t  day;      // 1..31
    int32_t  hour;     // 0..23
    int32_t  minute;
    int32_t  second;
    int32_t  era;      // 0 or 1: which NTP era was decoded (2036 rollover)
    uint64_t unix_ts;
} sntp_result_t;

// One-shot sync. `server` may be a hostname or a dotted-quad; NULL or "" means
// SNTP_DEFAULT_SERVER. `timeout_ms` of 0 means SNTP_DEFAULT_TIMEOUT_MS. On
// SNTP_OK the RTC has been set and *out (if non-NULL) holds the applied time.
// On any error the RTC is UNTOUCHED and the return value says why.
//
// Blocking, and callable ONLY from a normal process context with the scheduler
// live: it sleeps on a wait queue. Never call it from an IRQ handler, from the
// compositor draw path, or from anything holding net_lock.
int sntp_sync(const char *server, uint32_t timeout_ms, sntp_result_t *out);

// Human-readable form of a status code, for logs and for the wizard.
const char *sntp_strerror(int status);

#endif // NET_SNTP_H
