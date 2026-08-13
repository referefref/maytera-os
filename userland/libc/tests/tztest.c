// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// Host-side self-test for the PURE parts of userland/libc/tz.c (#49/#50).
// Links against the real freestanding tz.o, so it exercises the SHIPPING code,
// not a copy. Only functions that touch no syscall are called here.
#include <stdio.h>
#include <string.h>

typedef struct { const char *id; const char *label; int off_min; } tz_zone_t;
int  tz_count(void);
const tz_zone_t *tz_zone(int idx);
const char *tz_id(int idx);
const char *tz_label(int idx);
int  tz_offset_min_at(int idx);
const char *const *tz_labels(void);
int  tz_index_utc(void);
int  tz_index_for_offset(int off_min);
int  tz_index_for_id(const char *id);
void tz_shift(int off_min, int *h, int *mi, int *s, int *d, int *mo, int *y);
int  tz_wday(int d, int m, int y);

// userconf stubs: never reached by the pure functions, present only so the
// freestanding object links into a hosted test binary.
int userconf_open_read(const char *n, const char *l) { (void)n; (void)l; return -1; }
int userconf_open_write(const char *n) { (void)n; return -1; }
int userconf_finish_write(int fd, const void *b, unsigned long l) { (void)fd; (void)b; (void)l; return -1; }

static int fails = 0;
#define CK(cond, fmt, ...) do { if (!(cond)) { printf("FAIL " fmt "\n", ##__VA_ARGS__); fails++; } } while (0)

static void shift_case(const char *what, int off,
                       int h0,int m0,int d0,int mo0,int y0,
                       int h1,int m1,int d1,int mo1,int y1) {
    int h=h0,m=m0,s=0,d=d0,mo=mo0,y=y0;
    tz_shift(off, &h,&m,&s,&d,&mo,&y);
    CK(h==h1&&m==m1&&d==d1&&mo==mo1&&y==y1,
       "%s: %04d-%02d-%02d %02d:%02d %+d -> got %04d-%02d-%02d %02d:%02d want %04d-%02d-%02d %02d:%02d",
       what, y0,mo0,d0,h0,m0, off, y,mo,d,h,m, y1,mo1,d1,h1,m1);
}

int main(void) {
    int n = tz_count();
    printf("tz_count = %d\n", n);
    CK(n == 35, "expected 35 zones, got %d", n);

    // 1. Ascending, strictly, with no duplicate offsets.
    for (int i = 1; i < n; i++)
        CK(tz_offset_min_at(i) > tz_offset_min_at(i-1),
           "list not strictly ascending at %d (%d after %d)", i,
           tz_offset_min_at(i), tz_offset_min_at(i-1));

    // 2. Every non-hourly zone the brief names is present with the right value.
    struct { const char *id; int off; } want[] = {
        {"UTC+05:30", 330}, {"UTC+09:30", 570}, {"UTC+05:45", 345},
        {"UTC+12:45", 765}, {"UTC+03:30", 210}, {"UTC-03:30", -210},
        {"UTC-09:30", -570}, {"UTC+10:30", 630}, {"UTC+13:00", 780},
        {"UTC+14:00", 840}, {"UTC-12:00", -720}, {"UTC+00:00", 0},
    };
    for (unsigned k = 0; k < sizeof(want)/sizeof(want[0]); k++) {
        int i = tz_index_for_id(want[k].id);
        CK(i >= 0, "%s missing from the list", want[k].id);
        if (i >= 0) CK(tz_offset_min_at(i) == want[k].off,
                       "%s has offset %d, want %d", want[k].id, tz_offset_min_at(i), want[k].off);
    }

    // 3. The ID must be the exact prefix of the label, so a picker's row and the
    //    string written to TZ.CFG can never disagree.
    for (int i = 0; i < n; i++) {
        CK(strncmp(tz_id(i), tz_label(i), strlen(tz_id(i))) == 0,
           "label %d (%s) does not start with its id (%s)", i, tz_label(i), tz_id(i));
        CK(tz_labels()[i] == tz_label(i), "tz_labels()[%d] disagrees with tz_label()", i);
    }

    // 4. tz_index_utc() really is UTC, and is not a hardcoded 12 or 14.
    CK(tz_offset_min_at(tz_index_utc()) == 0, "tz_index_utc() is not the zero-offset zone");
    printf("tz_index_utc = %d (%s)\n", tz_index_utc(), tz_id(tz_index_utc()));

    // 5. Round trip: offset -> index -> offset.
    for (int i = 0; i < n; i++)
        CK(tz_index_for_offset(tz_offset_min_at(i)) == i, "offset round trip failed at %d", i);
    CK(tz_index_for_offset(1) == -1, "a bogus offset should not resolve");
    CK(tz_index_for_id("UTC+09:31") == -1, "a bogus id should not resolve");
    // A stored value that carries a city suffix must still resolve (the parser
    // matches the ID prefix), because an older build could have written one.
    CK(tz_index_for_id("UTC+09:30 Adelaide") == tz_index_for_id("UTC+09:30"),
       "id-with-suffix did not resolve to the same zone");

    // 6. tz_shift: the whole point of the ticket.
    shift_case("Adelaide +9:30 same day",   570, 8,15, 12,6,2026, 17,45, 12,6,2026);
    shift_case("Auckland +12:00 next day",  720, 20,0, 30,6,2026,  8, 0,  1,7,2026);
    shift_case("Chatham +12:45 next day",   765, 23,30,31,12,2026, 12,15, 1,1,2027);
    shift_case("India +5:30 minute carry",  330, 22,45, 1,3,2026,  4,15,  2,3,2026);
    shift_case("Nepal +5:45",               345,  0, 0, 1,1,2026,  5,45,  1,1,2026);
    shift_case("LA -8:00 previous day",    -480,  3, 0, 1,1,2026, 19, 0, 31,12,2025);
    shift_case("Newfoundland -3:30",       -210,  1,15, 1,3,2026, 21,45, 28,2,2026);
    shift_case("Marquesas -9:30 leap Feb", -570,  5, 0, 1,3,2024, 19,30, 29,2,2024);
    shift_case("Kiritimati +14:00",         840, 11, 0, 28,2,2026,  1, 0,  1,3,2026);
    shift_case("zero offset is identity",     0, 13,37, 15,8,2026, 13,37, 15,8,2026);

    // 7. local -> UTC -> local must be the identity for every zone (this is the
    //    property the manual "set the clock" paths rely on).
    for (int i = 0; i < n; i++) {
        int off = tz_offset_min_at(i);
        int h=18,m=20,s=5,d=1,mo=1,y=2026;
        tz_shift(-off, &h,&m,&s,&d,&mo,&y);
        tz_shift(off,  &h,&m,&s,&d,&mo,&y);
        CK(h==18&&m==20&&d==1&&mo==1&&y==2026,
           "round trip broken for %s: got %04d-%02d-%02d %02d:%02d", tz_id(i), y,mo,d,h,m);
    }

    // 8. tz_wday against known dates.
    CK(tz_wday(12, 8, 2026) == 3, "2026-08-12 should be Wednesday, got %d", tz_wday(12,8,2026));
    CK(tz_wday(1, 1, 2000) == 6,  "2000-01-01 should be Saturday, got %d", tz_wday(1,1,2000));
    CK(tz_wday(29, 2, 2024) == 4, "2024-02-29 should be Thursday, got %d", tz_wday(29,2,2024));

    // 9. A wild RTC reading must be clamped, not propagated.
    { int h=99,m=99,s=0,d=99,mo=99,y=1; tz_shift(0,&h,&m,&s,&d,&mo,&y);
      CK(h>=0&&h<24&&m>=0&&m<60&&d>=1&&d<=31&&mo>=1&&mo<=12&&y>=1970,
         "garbage input not clamped: %04d-%02d-%02d %02d:%02d", y,mo,d,h,m); }

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails ? 1 : 0;
}

// Freestanding-object link stubs. tz.o is built with the shipping CFLAGS
// (-fstack-protector-strong -mstack-protector-guard=global) and calls the libc
// syscall trampolines; none of the functions exercised above reaches them.
unsigned long __stack_chk_guard = 0xdeadbeefcafef00dUL;
long syscall0(long n) { (void)n; return 0; }
long syscall1(long n, long a) { (void)n; (void)a; return 0; }
long syscall3(long n, long a, long b, long c) { (void)n;(void)a;(void)b;(void)c; return 0; }
