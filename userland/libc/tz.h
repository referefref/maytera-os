// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// tz.h - THE timezone list, THE persisted timezone setting, and THE local
//        wall-clock helper for MayteraOS userland (#49 / #50).
//
// WHY THIS EXISTS
// ---------------
// Before this file there were TWO timezone lists (the first-run wizard's 35
// entries in userland/apps/setup/main.rs, and Settings' own diverged 26 in
// userland/apps/settings/main.c), TWO notions of "the chosen zone" (the
// wizard's /CONFIG/TZ.CFG and Settings' private 't' key in SETTINGS.CFG), and
// ZERO clocks that applied the offset. Picking Adelaide in the wizard changed
// nothing anywhere. That is the same "two implementations of one thing" shape
// that previously shipped two Task Managers and two wallpaper arrays.
//
// So: ONE list (ZONES below), ONE persisted setting (TZ.CFG, holding the zone
// ID STRING, never an index), ONE reader, and ONE local-time helper that every
// clock in the OS calls. Do not add a second copy of any of the four.
//
// THE RTC HOLDS UTC. SYS_GET_RTC_TIME/SYS_GET_RTC_DATE are UTC, because that is
// what NTP writes into them. Local time is therefore RTC + offset, computed by
// tz_local_now() and nowhere else. Anything that SETS the clock from a value a
// human typed must convert the other way first (tz_shift with a negated
// offset), or the next display would add the offset a second time.
//
// WHAT MUST NOT USE THE LOCAL HELPER: anything that needs an absolute instant
// rather than a wall-clock reading. In particular the TOTP generator
// (userland/apps/mfa) must keep reading the raw RTC, because RFC 6238 counts
// seconds since the UNIX epoch in UTC and shifting it would produce codes that
// no authenticator agrees with. Same for audit-log timestamps (libc/aicap.c)
// and for the World Time widget / World Clock app, which are UTC-referenced by
// construction and add their OWN per-city offsets.
//
// OFFSETS ARE MINUTES, NOT HOURS, AND ALWAYS WERE. India is +5:30, Adelaide is
// +9:30, Nepal is +5:45 and Chatham is +12:45. A whole-hour representation is a
// defect, not a simplification.
//
// NO DST. There is no timezone database in the tree and this file does not
// pretend to be one; a zone is a fixed offset the user picks. That is a stated
// limitation, not an oversight, and it is why the labels name a representative
// city rather than an IANA zone ID.
#ifndef LIBC_TZ_H
#define LIBC_TZ_H

// THE persisted setting. One line of text: "tz=UTC+09:30\n". The VALUE is the
// zone ID string, deliberately not an index: an index on disk breaks the moment
// the list gains an entry, which is exactly how #46 grew the wizard's list to
// 35 and left every stored index meaning a different zone.
#define TZ_CFG_NAME   "TZ.CFG"            // per-user name (#683 userconf.h)
#define TZ_CFG_LEGACY "/CONFIG/TZ.CFG"    // pre-#683 absolute path, still read

typedef struct {
    const char *id;       // "UTC+09:30" - EXACTLY the token stored in TZ.CFG
    const char *label;    // "UTC+09:30 Adelaide" - what a picker shows
    int         off_min;  // minutes east of UTC; negative is west
} tz_zone_t;

// --- THE list (static, ordered by ascending offset) ------------------------
int                 tz_count(void);
const tz_zone_t    *tz_zone(int idx);          // 0 if idx is out of range
const char         *tz_id(int idx);            // "UTC+00:00" if out of range
const char         *tz_label(int idx);
int                 tz_offset_min_at(int idx); // 0 if out of range
// The whole label array, for a picker that wants to hand it straight to a
// dropdown. Exactly tz_count() entries; never 0.
const char *const  *tz_labels(void);

// Index of UTC+00:00. Call this instead of writing 12, 14 or any other literal:
// the list has already been reordered once and every hardcoded default silently
// meant a different zone afterwards.
int tz_index_utc(void);

int tz_index_for_offset(int off_min);   // -1 if no zone has exactly that offset
int tz_index_for_id(const char *id);    // -1 if unknown; matches the ID prefix

// --- THE current setting ---------------------------------------------------
// tz_index()/tz_offset_minutes() re-read TZ.CFG on a self-throttle (see
// TZ_REFRESH_MS in tz.c) and serve a cache in between, so a compositor draw
// path may call them every frame. They never block waiting on anything.
int  tz_index(void);            // falls back to tz_index_utc() if unset
// 1 once a zone has actually been read out of TZ.CFG, 0 while the fallback is
// in force. The two are NOT the same thing: UTC is also a zone a user can
// choose, so "tz_index() == tz_index_utc()" cannot answer "has anyone set
// this?" and a migration that guessed would overwrite a real choice.
int  tz_is_set(void);
int  tz_offset_minutes(void);

// Persist a new zone AND update the cache immediately, so the calling process
// sees the change on its very next draw. Returns 0 on success, -1 if the write
// failed (in which case the on-disk setting is unchanged and the caller must
// not report success). Other processes pick it up within TZ_REFRESH_MS.
int  tz_set_index(int idx);

// Drop the cache so the next read hits the file. For a process that has just
// learned the setting changed underneath it.
void tz_invalidate(void);

// --- THE local wall clock --------------------------------------------------
typedef struct {
    int hour, min, sec;      // local
    int day, month, year;    // local; the date is rolled by the offset too
    int wday;                // 0 = Sunday
    int off_min;             // the offset that was applied
} tz_time_t;

// Reads the RTC and applies the current offset. This is the function every
// clock in the OS calls. Never fails; on a wild RTC reading it clamps to a sane
// date rather than propagating garbage into a formatter.
void tz_local_now(tz_time_t *out);

// Convenience wrappers with the same signature shape as the raw
// get_rtc_time()/get_rtc_date() they replace, so a call site converts by
// changing the function name and nothing else. Both go through tz_local_now().
void tz_local_hms(int *hour, int *min, int *sec);
void tz_local_date(int *day, int *month, int *year);

// #148: a sortable local-time filename stamp, "YYYYMMDD-HHMMSS" (15 chars +
// NUL), for any caller that names a file after the moment it was created
// (screenshots today; anything else that wants "when was this captured" in
// the name later). Goes through tz_local_now() like every other clock in the
// OS - NOT a second RTC read, NOT a second offset application. `out` must
// hold >= TZ_STAMP_LEN bytes. Zero-padded so lexical sort == chronological
// sort (the whole point of a timestamp-named file over a counter).
#define TZ_STAMP_LEN 16
void tz_local_stamp(char *out, unsigned long cap);

// Add off_min minutes to a full civil date-time, in place, with correct
// day/month/year rollover in both directions. THE one implementation: local =
// tz_shift(+offset), UTC-from-local = tz_shift(-offset).
void tz_shift(int off_min, int *hour, int *min, int *sec,
              int *day, int *month, int *year);

// Day of week for a civil date, 0 = Sunday.
int tz_wday(int day, int month, int year);

#endif // LIBC_TZ_H
