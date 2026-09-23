// netclock.c - is this machine's clock plausible, and does the log say so?
// (#netfix2, 2026-09-03)
//
// A wrong real-time clock is a NETWORK fault from the user's chair: every HTTPS
// page fails, every app that fetches fails, and `ping 1.1.1.1` stays perfect,
// because ICMP has no certificates and no notion of "now".  That is, character
// for character, the report we have been getting.  It is a completely different
// fix from a resolver fault and until now it was indistinguishable from one in
// the only artifact we ever get: /BOOTLOG.TXT off a USB stick.
//
// MEASURED, on the owner's iMac14,4, from the build host:/root/imac-logs-20260902/:
//   [RTC] raw ... decoded=2026-08-30 13:14:01
// on an image whose /BUILDINFO.TXT says it was built 2026-09-02.  Three days
// slow.  Harmless in itself - no certificate has a three-day window - so the
// clock is NOT the owner's current fault.  But that machine is a 2013 iMac, the
// classic failure of a machine that age is a flat CMOS battery, and the day
// that battery goes the RTC reads something like 2000-01-01, EVERY certificate
// on the internet becomes "not yet valid", and every HTTPS fetch in the OS fails
// with a message about certificates that sends the reader to look at the site.
//
// WHY THIS FILE IS C, given the 2026-07-16 Rust-first rule.  The comparison
// itself is Rust (netfail_clock_check_rs in rustkern/netfail.rs).  What is left
// here is (a) reading __DATE__, a C preprocessor construct Rust cannot see, and
// (b) reading the CMOS RTC through the existing rtc_read_* C API.  Both are
// bindings to things that only exist on the C side.

#include "netfail.h"
#include "../version.h"

#include "../fs/bootlog.h"   // NOT a private extern: see persist-extern-gate

extern void rtc_read_time(int *hour, int *minute, int *second);
extern void rtc_read_date(int *day, int *month, int *year, int *weekday);

// __DATE__ is "Mmm DD YYYY" with a SPACE-padded day ("Sep  3 2026"), which is
// the detail that a naive parse gets wrong and then silently reports the wrong
// build date forever.
static const char nc_months[12][4] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void netclock_parse_build(uint32_t *y, uint32_t *m, uint32_t *d) {
    const char *s = MAYTERA_BUILD_DATE;
    *y = 0; *m = 0; *d = 0;
    if (!s) return;
    // Month
    for (int i = 0; i < 12; i++) {
        if (s[0] == nc_months[i][0] && s[1] == nc_months[i][1] && s[2] == nc_months[i][2]) {
            *m = (uint32_t)(i + 1);
            break;
        }
    }
    if (*m == 0) return;
    // Day: characters 4-5, the first of which may be a space.
    uint32_t day = 0;
    for (int i = 4; i <= 5; i++) {
        if (s[i] >= '0' && s[i] <= '9') day = day * 10 + (uint32_t)(s[i] - '0');
        else if (s[i] != ' ') return;
    }
    if (day == 0 || day > 31) return;
    *d = day;
    // Year: characters 7-10.
    uint32_t year = 0;
    for (int i = 7; i <= 10; i++) {
        if (s[i] < '0' || s[i] > '9') return;
        year = year * 10 + (uint32_t)(s[i] - '0');
    }
    if (year < 2000 || year > 2200) return;
    *y = year;
}

static int nc_verdict_cached = -1;

// Non-zero when the RTC cannot possibly be telling the truth.  Deliberately
// cheap: it re-reads the RTC each call, but the RTC read is a handful of port
// I/Os and this runs only on a certificate rejection, never in a hot path.
int cert_clock_is_implausible(void) {
    int day, month, year, weekday;
    rtc_read_date(&day, &month, &year, &weekday);
    uint32_t v = netfail_clock_check_rs((uint32_t)year, (uint32_t)month, (uint32_t)day);
    return (v == NF_CLOCK_BEFORE_BUILD || v == NF_CLOCK_ABSURDLY_AHEAD);
}

// Called once from net_init().  DURABLE, because the machine this matters on has
// no serial port; that is the entire lesson blame.md records four times.
void netclock_boot_report(void) {
    uint32_t by = 0, bm = 0, bd = 0;
    netclock_parse_build(&by, &bm, &bd);
    netfail_clock_set_build_rs(by, bm, bd);

    int day, month, year, weekday, hour, minute, second;
    rtc_read_date(&day, &month, &year, &weekday);
    rtc_read_time(&hour, &minute, &second);
    uint32_t v = netfail_clock_check_rs((uint32_t)year, (uint32_t)month, (uint32_t)day);
    nc_verdict_cached = (int)v;

    if (v == NF_CLOCK_BEFORE_BUILD || v == NF_CLOCK_ABSURDLY_AHEAD) {
        // Say what is wrong, why we are sure, and what it will break, in the
        // line itself.  A log line that needs a second document to interpret is
        // how a diagnostic gets read as a curiosity (see blame.md, the
        // line-length audit that reported its own data loss for weeks).
        bootlog_write("[CLOCK] SYSTEM CLOCK IS WRONG: RTC reads %04d-%02d-%02d %02d:%02d:%02d "
                      "but this kernel was built %04u-%02u-%02u, and it cannot be running "
                      // #dnsfallback: THIS SAID "EVERY HTTPS FETCH WILL FAIL"
                      // AND THAT IS MEASURABLY FALSE. On the ICS bench with the
                      // RTC set to the owner's exact wrong value (2026-08-30
                      // against a 2026-09-03 build, four days slow), the fetch
                      // of https://example.com returned rc=0 status=200 while
                      // this very line was in the log. A slow clock rejects only
                      // certificates whose validity window STARTS AFTER the
                      // wrong date, so four days slow breaks exactly the sites
                      // whose certificate was issued in those four days - which
                      // is why the owner's coingecko fetch died on the clock and
                      // other fetches did not.
                      //
                      // AN OVERSTATED WARNING IS A WARNING THAT GETS DISMISSED.
                      // A user who reads "EVERY HTTPS FETCH WILL FAIL" and then
                      // watches a page load correctly concludes the message is
                      // wrong and stops believing it, which is worse than not
                      // having warned at all.
                      "before it was built. HTTPS WILL FAIL FOR SOME SITES AND NOT "
                      "OTHERS (a certificate is rejected when this clock falls outside "
                      "its validity window, so the sites that break are the ones whose "
                      "certificate was issued most recently), WHILE PING KEEPS WORKING "
                      "THROUGHOUT. That mixture is the symptom, not evidence the clock "
                      "is fine. Fix the date in Settings > Date and Time; on an older "
                      "machine a flat CMOS battery is the usual cause, and this log's "
                      "[RTC] line reports vrt=0 when the hardware confirms it.",
                      year, month, day, hour, minute, second, by, bm, bd);
    } else {
        bootlog_write("[CLOCK] RTC %04d-%02d-%02d %02d:%02d:%02d, built %04u-%02u-%02u, "
                      "verdict=%s (TLS certificate windows are checked against this clock)",
                      year, month, day, hour, minute, second, by, bm, bd,
                      v == NF_CLOCK_OK ? "plausible" : "build-date-unknown");
    }
}
