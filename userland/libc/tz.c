// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// tz.c - THE timezone list / setting / local-clock implementation (#49 / #50).
//        Read tz.h first; it carries the rationale and the rules.

#include "tz.h"
#include "userconf.h"
#include "syscall.h"
#include "stdio.h"   // #148: snprintf() for tz_local_stamp()

// ---------------------------------------------------------------------------
// THE list. 35 zones, ORDERED BY ASCENDING OFFSET.
//
// Provenance: this is the wizard's post-#46 35-entry set (userland/apps/setup),
// which was the better of the two lists because it was the only one carrying
// +09:30, +10:30, +12:45, +13:00, +14:00, -03:30 and -09:30 at all. Settings'
// diverged 26-entry array supplied the city names and has been DELETED; it is
// not "also still there for compatibility", because a second list that still
// compiles is a second list that will drift again.
//
// The wizard kept its 9 new offsets APPENDED at indices 26..34 rather than
// sorted, precisely because its own index 12 was hardcoded as "UTC" in three
// places. Sorting them here is safe for the opposite reason: NOTHING persists
// an index any more (TZ.CFG stores the ID string), and the one remaining
// in-code default goes through tz_index_utc() instead of a literal. A picker
// can therefore render this array top to bottom and get a sane ordering with no
// second sorted-view table to keep in step.
//
// NO DST, by design; see tz.h.
// ---------------------------------------------------------------------------
static const tz_zone_t ZONES[] = {
    { "UTC-12:00", "UTC-12:00  Baker Island",   -720 },
    { "UTC-11:00", "UTC-11:00  Midway",         -660 },
    { "UTC-10:00", "UTC-10:00  Hawaii",         -600 },
    { "UTC-09:30", "UTC-09:30  Marquesas",      -570 },
    { "UTC-09:00", "UTC-09:00  Alaska",         -540 },
    { "UTC-08:00", "UTC-08:00  Pacific (LA)",   -480 },
    { "UTC-07:00", "UTC-07:00  Mountain",       -420 },
    { "UTC-06:00", "UTC-06:00  Central",        -360 },
    { "UTC-05:00", "UTC-05:00  Eastern (NY)",   -300 },
    { "UTC-04:00", "UTC-04:00  Atlantic",       -240 },
    { "UTC-03:30", "UTC-03:30  Newfoundland",   -210 },
    { "UTC-03:00", "UTC-03:00  Buenos Aires",   -180 },
    { "UTC-02:00", "UTC-02:00  Mid-Atlantic",   -120 },
    { "UTC-01:00", "UTC-01:00  Azores",          -60 },
    { "UTC+00:00", "UTC+00:00  London/GMT",        0 },
    { "UTC+01:00", "UTC+01:00  Paris/Berlin",      60 },
    { "UTC+02:00", "UTC+02:00  Cairo",            120 },
    { "UTC+03:00", "UTC+03:00  Moscow",           180 },
    { "UTC+03:30", "UTC+03:30  Tehran",           210 },
    { "UTC+04:00", "UTC+04:00  Dubai",            240 },
    { "UTC+05:00", "UTC+05:00  Karachi",          300 },
    { "UTC+05:30", "UTC+05:30  Mumbai/Delhi",     330 },
    { "UTC+05:45", "UTC+05:45  Kathmandu",        345 },
    { "UTC+06:00", "UTC+06:00  Dhaka",            360 },
    { "UTC+07:00", "UTC+07:00  Bangkok",          420 },
    { "UTC+08:00", "UTC+08:00  Singapore",        480 },
    { "UTC+09:00", "UTC+09:00  Tokyo",            540 },
    { "UTC+09:30", "UTC+09:30  Adelaide/Darwin",  570 },
    { "UTC+10:00", "UTC+10:00  Sydney",           600 },
    { "UTC+10:30", "UTC+10:30  Lord Howe",        630 },
    { "UTC+11:00", "UTC+11:00  Solomon Is.",      660 },
    { "UTC+12:00", "UTC+12:00  Auckland",         720 },
    { "UTC+12:45", "UTC+12:45  Chatham",          765 },
    { "UTC+13:00", "UTC+13:00  Apia",             780 },
    { "UTC+14:00", "UTC+14:00  Kiritimati",       840 },
};
#define TZ_N ((int)(sizeof(ZONES) / sizeof(ZONES[0])))

// The label array a dropdown wants, derived from ZONES at init rather than
// written out a second time by hand: a hand-copied parallel array IS a second
// list, whatever it is called.
static const char *s_labels[TZ_N];
static int         s_labels_ready = 0;

// UTC+00:00's index, found by search once. Never a literal anywhere else.
static int s_utc_idx = -1;

static void tz_tables_init(void) {
    if (s_labels_ready) return;
    for (int i = 0; i < TZ_N; i++) {
        s_labels[i] = ZONES[i].label;
        if (ZONES[i].off_min == 0) s_utc_idx = i;
    }
    if (s_utc_idx < 0) s_utc_idx = 0;   // cannot happen; never return garbage
    s_labels_ready = 1;
}

int tz_count(void) { return TZ_N; }

const tz_zone_t *tz_zone(int idx) {
    if (idx < 0 || idx >= TZ_N) return 0;
    return &ZONES[idx];
}

int tz_index_utc(void) { tz_tables_init(); return s_utc_idx; }

const char *tz_id(int idx) {
    if (idx < 0 || idx >= TZ_N) return ZONES[tz_index_utc()].id;
    return ZONES[idx].id;
}

const char *tz_label(int idx) {
    if (idx < 0 || idx >= TZ_N) return ZONES[tz_index_utc()].label;
    return ZONES[idx].label;
}

int tz_offset_min_at(int idx) {
    if (idx < 0 || idx >= TZ_N) return 0;
    return ZONES[idx].off_min;
}

const char *const *tz_labels(void) { tz_tables_init(); return s_labels; }

int tz_index_for_offset(int off_min) {
    for (int i = 0; i < TZ_N; i++) if (ZONES[i].off_min == off_min) return i;
    return -1;
}

// Matches on the leading "UTC+HH:MM" token only, so a stored value that also
// carries a city ("UTC+09:30 Adelaide", which an older Settings build could
// have written) still resolves instead of silently falling back to UTC.
int tz_index_for_id(const char *id) {
    if (!id) return -1;
    for (int i = 0; i < TZ_N; i++) {
        const char *a = ZONES[i].id;
        int k = 0;
        while (a[k] && a[k] == id[k]) k++;
        if (a[k] == 0) return i;        // whole ID matched a prefix of `id`
    }
    return -1;
}

// ---------------------------------------------------------------------------
// THE persisted setting.
//
// #426: this is reached from the compositor DRAW path, which must never block.
// The read is one open/read/close of a file under 32 bytes, and it happens at
// most once every TZ_REFRESH_MS; every other call is served from the cache with
// no syscall but the monotonic clock. There is no wait, no poll loop and no
// retry, so there is nothing here that can stall a frame. A failed read leaves
// the previous cached value in place and simply re-arms the throttle.
// ---------------------------------------------------------------------------
#define TZ_REFRESH_MS 2000UL

static int           s_cur_idx   = -1;    // -1 = never loaded
static int           s_from_file = 0;    // the value above came out of TZ.CFG
static unsigned long s_last_ms   = 0;
static int           s_loaded    = 0;

void tz_invalidate(void) { s_loaded = 0; }

// Parse "tz=UTC+09:30" out of the file body. Tolerates leading whitespace,
// other key=value lines, CRLF, and a missing trailing newline. Returns the
// canonical index, or -1.
static int tz_parse(const char *buf, int n) {
    for (int i = 0; i + 2 < n; i++) {
        if (buf[i] != 't' || buf[i+1] != 'z' || buf[i+2] != '=') continue;
        if (i > 0 && buf[i-1] != '\n' && buf[i-1] != '\r' && buf[i-1] != ' ') continue;
        int j = i + 3;
        while (j < n && (buf[j] == ' ' || buf[j] == '\t')) j++;
        char v[16];
        int k = 0;
        while (j < n && k < (int)sizeof(v) - 1 &&
               buf[j] != '\n' && buf[j] != '\r') v[k++] = buf[j++];
        while (k > 0 && (v[k-1] == ' ' || v[k-1] == '\t')) k--;
        v[k] = 0;
        return tz_index_for_id(v);
    }
    return -1;
}

static void tz_reload(void) {
    int fd = userconf_open_read(TZ_CFG_NAME, TZ_CFG_LEGACY);
    if (fd < 0) {
        // No setting yet (a machine that never ran the wizard). UTC is the
        // honest answer, and it is what every clock showed before this work.
        if (s_cur_idx < 0) s_cur_idx = tz_index_utc();
        return;
    }
    char rb[64];
    long n = sys_read(fd, rb, sizeof(rb) - 1);
    sys_close(fd);
    if (n <= 0) { if (s_cur_idx < 0) s_cur_idx = tz_index_utc(); return; }
    rb[n] = 0;
    int idx = tz_parse(rb, (int)n);
    if (idx >= 0) { s_cur_idx = idx; s_from_file = 1; }
    else if (s_cur_idx < 0) s_cur_idx = tz_index_utc();
}

int tz_index(void) {
    tz_tables_init();
    unsigned long now = uptime_ms();
    if (!s_loaded || (unsigned long)(now - s_last_ms) >= TZ_REFRESH_MS) {
        s_last_ms = now;
        s_loaded  = 1;
        tz_reload();
    }
    return (s_cur_idx >= 0) ? s_cur_idx : tz_index_utc();
}

int tz_is_set(void) { (void)tz_index(); return s_from_file; }

int tz_offset_minutes(void) { return ZONES[tz_index()].off_min; }

int tz_set_index(int idx) {
    tz_tables_init();
    if (idx < 0 || idx >= TZ_N) return -1;

    char buf[32];
    int  n = 0;
    buf[n++] = 't'; buf[n++] = 'z'; buf[n++] = '=';
    for (const char *c = ZONES[idx].id; *c && n < (int)sizeof(buf) - 2; c++) buf[n++] = *c;
    buf[n++] = '\n';

    int fd = userconf_open_write(TZ_CFG_NAME);
    if (fd < 0) return -1;
    if (userconf_finish_write(fd, buf, (unsigned long)n) != 0) return -1;

    // Only after the bytes are known to be on disk: a cache that ran ahead of a
    // failed write would show the user a zone their next boot would not have.
    s_cur_idx   = idx;
    s_from_file = 1;
    s_loaded    = 1;
    s_last_ms   = uptime_ms();
    return 0;
}

// ---------------------------------------------------------------------------
// THE local wall clock.
// ---------------------------------------------------------------------------
static int tz_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

static int tz_mdays(int m, int y) {
    static const int d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12) return 30;
    if (m == 2 && tz_leap(y)) return 29;
    return d[m - 1];
}

int tz_wday(int day, int month, int year) {
    // Zeller's congruence, shifted so 0 = Sunday.
    int m = month, y = year;
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100, J = y / 100;
    int h = (day + 13 * (m + 1) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;  // 0 = Sat
    h = (h + 6) % 7;
    return (h < 0) ? h + 7 : h;
}

void tz_shift(int off_min, int *hour, int *min, int *sec,
              int *day, int *month, int *year) {
    if (!hour || !min) return;

    int h = *hour, mi = *min;
    int d  = day   ? *day   : 1;
    int mo = month ? *month : 1;
    int y  = year  ? *year  : 2026;
    (void)sec;   // seconds are unaffected by a whole-minute offset

    // Clamp a wild RTC reading rather than propagating it into arithmetic that
    // would then walk the date by months.
    if (h  < 0 || h  > 23)   h  = 0;
    if (mi < 0 || mi > 59)   mi = 0;
    if (mo < 1 || mo > 12)   mo = 1;
    if (y  < 1970 || y > 3000) y = 2026;
    if (d  < 1) d = 1;
    if (d > tz_mdays(mo, y)) d = tz_mdays(mo, y);

    long total = (long)h * 60 + mi + off_min;
    long dayadj = 0;
    while (total < 0)          { total += 24 * 60; dayadj--; }
    while (total >= 24 * 60)   { total -= 24 * 60; dayadj++; }

    while (dayadj > 0) {
        int dim = tz_mdays(mo, y);
        if (d < dim) d++;
        else { d = 1; mo++; if (mo > 12) { mo = 1; y++; } }
        dayadj--;
    }
    while (dayadj < 0) {
        if (d > 1) d--;
        else { mo--; if (mo < 1) { mo = 12; y--; } d = tz_mdays(mo, y); }
        dayadj++;
    }

    *hour = (int)(total / 60);
    *min  = (int)(total % 60);
    if (day)   *day   = d;
    if (month) *month = mo;
    if (year)  *year  = y;
}

void tz_local_now(tz_time_t *out) {
    if (!out) return;

    // The RTC time and date are two separate syscalls, so a midnight rollover
    // between them would pair tomorrow's 00:00 with yesterday's date. Read the
    // date on both sides and re-read the time if it moved; a second rollover
    // inside that window is not physically reachable.
    int d1 = 1, m1 = 1, y1 = 2026;
    int d2 = 1, m2 = 1, y2 = 2026;
    int h = 0, mi = 0, s = 0;
    get_rtc_date(&d1, &m1, &y1);
    get_rtc_time(&h, &mi, &s);
    get_rtc_date(&d2, &m2, &y2);
    if (d1 != d2 || m1 != m2 || y1 != y2) {
        get_rtc_time(&h, &mi, &s);
        d1 = d2; m1 = m2; y1 = y2;
    }
    if (s < 0 || s > 60) s = 0;

    int off = tz_offset_minutes();
    tz_shift(off, &h, &mi, &s, &d1, &m1, &y1);

    out->hour = h; out->min = mi; out->sec = s;
    out->day = d1; out->month = m1; out->year = y1;
    out->wday = tz_wday(d1, m1, y1);
    out->off_min = off;
}

void tz_local_hms(int *hour, int *min, int *sec) {
    tz_time_t t;
    tz_local_now(&t);
    if (hour) *hour = t.hour;
    if (min)  *min  = t.min;
    if (sec)  *sec  = t.sec;
}

void tz_local_date(int *day, int *month, int *year) {
    tz_time_t t;
    tz_local_now(&t);
    if (day)   *day   = t.day;
    if (month) *month = t.month;
    if (year)  *year  = t.year;
}

// #148: see tz.h. Zero-padded 4/2/2/2/2/2 so string sort == time sort.
void tz_local_stamp(char *out, unsigned long cap) {
    if (!out || cap == 0) return;
    tz_time_t t;
    tz_local_now(&t);
    snprintf(out, cap, "%04d%02d%02d-%02d%02d%02d",
             t.year, t.month, t.day, t.hour, t.min, t.sec);
}
