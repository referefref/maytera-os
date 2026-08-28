// drivers/rtc.c - #135: MC146818 CMOS RTC. The whole chip, one file.
//
// WHAT MOVED HERE AND WHY.
//
// The read half is lifted verbatim in behaviour from gui/clock.c, including
// #115's bounded update-in-progress wait (comment preserved below, because the
// reasoning is still the reasoning). The only behavioural change on the read
// side is that the BCD/binary and 12/24-hour decode now calls
// rustkern/rtcenc.rs instead of open-coding it, which fixes a real defect: the
// old 12-hour branch had no AM case, so 12 AM decoded to 12 rather than 0.
//
// The write half is NEW here and was WRONG where it came from. proc/syscall.c's
// sys_set_rtc_time()/sys_set_rtc_date() called bin_to_bcd() unconditionally,
// with no reference to Status Register B, while all three readers consulted it.
// On a binary-mode chip that is a permanent +6*floor(v/10) error per field:
// +6h06m for a clock set at any local time of the form 1X:1Y. Every path that
// corrects the clock funnels through here - the first-run wizard's Date and
// Time page, the Settings date/time panel, and net/sntp.c's SNTP client, which
// applies its validated UTC result through exactly these two entry points - so
// a perfectly good NTP answer was being stored wrong.
//
// WAITING. The UIP wait is a BOUNDED busy wait, not wait_event, and that is
// deliberate and unchanged from #115: there is no interrupt to wake on (the
// RTC periodic IRQ is not enabled) and the wait is sub-millisecond. The thing
// that matters is the bound. proc/syscall.c's write path had NO bound at all
// (`while (inb(0x71) & 0x80) { outb(0x70, 0x0A); }`), which is the #426
// anti-pattern verbatim on a path Ring 3 can call: a chipset answering 0xFF
// would have hung the caller forever. That is fixed by construction here, since
// there is now only one wait and it is the bounded one.
//
// NOT DONE, DELIBERATELY:
//
//   * The century register (CMOS 0x32) is neither read nor written. No decoder
//     in this tree reads it (the year is `two_digit + 2000`), so writing it
//     would recreate the exact read/write asymmetry this file exists to remove.
//     Whether 0x32 even IS a century register is board-specific - it is
//     declared by the ACPI FADT century field, which this kernel does not
//     parse - so blind-writing it is a real way to corrupt firmware state on
//     an unknown machine. When the reader learns it, the writer gains the
//     matching branch, here, in one place.
//
//   * No lock is taken around the index/data port pair. That race is REAL (two
//     cores interleaving outb(0x70)/inb(0x71) read each other's register) but
//     it PRE-EXISTS on the read path, which has run unlocked for the life of
//     the tree, and putting a new shared lock on a path the taskbar clock hits
//     every frame is a contention change this task cannot verify. Recorded as
//     found-not-fixed rather than fixed-and-unproven. Local preemption IS
//     closed: the write sequence runs with interrupts off.

#include "rtc.h"
#include "../types.h"
#include "../serial.h"
#include "../cpu/mono.h"        // #115: bound the RTC update-in-progress waits
#include "../fs/bootlog.h"

// The codec, rustkern/rtcenc.rs. Both directions, one definition.
extern int rtc_encode_field_rs(int value, int is_bcd);
extern int rtc_decode_field_rs(int raw, int is_bcd);
extern int rtc_encode_hour_rs(int hour24, int is_bcd, int is_24h);
extern int rtc_decode_hour_rs(int raw, int is_bcd, int is_24h);
extern int rtc_selftest_rs(uint32_t *out_checks);

#define RTC_INDEX_PORT  0x70
#define RTC_DATA_PORT   0x71

#define RTC_SECONDS     0x00
#define RTC_MINUTES     0x02
#define RTC_HOURS       0x04
#define RTC_WEEKDAY     0x06
#define RTC_DAY         0x07
#define RTC_MONTH       0x08
#define RTC_YEAR        0x09
#define RTC_STATUS_A    0x0A
#define RTC_STATUS_B    0x0B

static inline uint8_t rtc_reg_read(uint8_t reg) {
    outb(RTC_INDEX_PORT, reg);
    return inb(RTC_DATA_PORT);
}

static inline void rtc_reg_write(uint8_t reg, uint8_t val) {
    outb(RTC_INDEX_PORT, reg);
    outb(RTC_DATA_PORT, val);
}

uint8_t rtc_status_b(void) {
    return rtc_reg_read(RTC_STATUS_B);
}

static bool rtc_update_in_progress(void) {
    return (rtc_reg_read(RTC_STATUS_A) & 0x80) != 0;
}

// #115: BOUNDED wait for the RTC's update cycle to end.
//
// Both readers used to spin on `while (rtc_update_in_progress());` with no
// bound at all. That is the #426 anti-pattern verbatim: a wedged or absent RTC
// (a virtual machine that never clears the flag, a chipset that answers 0xFF on
// every CMOS read) hangs the caller forever, and the callers include the
// FILESYSTEM create path - so a dead RTC would have turned "save a file" into a
// hard freeze. It is a busy wait rather than wait_event() because there is no
// interrupt to wake on (the RTC periodic IRQ is not enabled) and the wait is
// sub-millisecond; the fix that matters is the BOUND.
//
// The update cycle takes at most ~2ms on real hardware (the UIP flag is raised
// ~244us before it starts). 20ms is an order of magnitude of headroom. On
// expiry we proceed anyway: a torn read gives a date the converter will either
// accept (off by at most one second) or reject as implausible, and both are
// better than never returning. Uses the TSC clock (cpu/mono.h), never
// timer_ticks, which is not a measure of elapsed time (#524/#525).
#define RTC_UIP_BUDGET_US 20000ull
static void rtc_wait_update_done(void) {
    if (mono_ready()) {
        uint64_t t0 = mono_us();
        while (rtc_update_in_progress()) {
            if (mono_us() - t0 >= RTC_UIP_BUDGET_US) break;
        }
        return;
    }
    // Pre-calibration (very early boot): no real clock yet, so bound by
    // iterations instead. Each pass is two port I/Os, so this is milliseconds.
    for (int i = 0; i < 200000 && rtc_update_in_progress(); i++) { }
}

// ---------------------------------------------------------------------------
// READ
// ---------------------------------------------------------------------------

void rtc_read_time(int *hour, int *minute, int *second) {
    rtc_wait_update_done();

    uint8_t sec = rtc_reg_read(RTC_SECONDS);
    uint8_t min = rtc_reg_read(RTC_MINUTES);
    uint8_t hr  = rtc_reg_read(RTC_HOURS);

    uint8_t status_b = rtc_reg_read(RTC_STATUS_B);
    int is_bcd = (status_b & RTC_REGB_DM_BINARY) ? 0 : 1;
    int is_24h = (status_b & RTC_REGB_24HOUR) ? 1 : 0;

    int s = rtc_decode_field_rs(sec, is_bcd);
    int m = rtc_decode_field_rs(min, is_bcd);
    int h = rtc_decode_hour_rs(hr, is_bcd, is_24h);

    // A refusal from the codec means the chip handed us bytes it could not
    // have produced (a torn read, or a chipset answering 0xFF). Report 0
    // rather than a laundered hex nibble; rustkern/ktime.rs's plausibility
    // window turns an impossible date into "no timestamp" downstream.
    if (second) *second = (s < 0) ? 0 : s;
    if (minute) *minute = (m < 0) ? 0 : m;
    if (hour)   *hour   = (h < 0) ? 0 : h;
}

void rtc_read_date(int *day, int *month, int *year, int *weekday) {
    rtc_wait_update_done();

    uint8_t d = rtc_reg_read(RTC_DAY);
    uint8_t m = rtc_reg_read(RTC_MONTH);
    uint8_t y = rtc_reg_read(RTC_YEAR);
    uint8_t w = rtc_reg_read(RTC_WEEKDAY);

    uint8_t status_b = rtc_reg_read(RTC_STATUS_B);
    int is_bcd = (status_b & RTC_REGB_DM_BINARY) ? 0 : 1;

    int dd = rtc_decode_field_rs(d, is_bcd);
    int mm = rtc_decode_field_rs(m, is_bcd);
    int yy = rtc_decode_field_rs(y, is_bcd);
    int ww = rtc_decode_field_rs(w, is_bcd);

    if (day)     *day     = (dd < 0) ? 1 : dd;
    if (month)   *month   = (mm < 0) ? 1 : mm;
    if (year)    *year    = (yy < 0) ? 2026 : yy + 2000;   // see the century note
    if (weekday) *weekday = (ww < 0) ? 0 : ww;
}

// ---------------------------------------------------------------------------
// WRITE
//
// Sequence follows the MC146818 datasheet's documented update-freeze, which is
// also what Linux's mc146818_set_time() does: raise Status Register B bit 7
// (SET), write the fields, restore the saved register. While SET is held the
// chip does not advance, so the six field writes land as one instant rather
// than straddling a rollover.
//
// Interrupts are off for the freeze window (a handful of port I/Os, single-
// digit microseconds) so this core cannot be preempted with the clock halted.
// The bounded UIP wait happens BEFORE the freeze, with interrupts on, so the
// 20ms budget is never spent with the RTC stopped. Nothing inside the window
// can block, so wq_assert_may_block() has nothing to complain about.
// ---------------------------------------------------------------------------

static inline uint64_t rtc_irq_save_cli(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void rtc_irq_restore(uint64_t flags) {
    __asm__ volatile("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
}

// Encode every field FIRST. If any one of them is not representable we write
// nothing at all: a partial write would leave the chip holding a time that is
// neither the old one nor the requested one.
int rtc_set_time(int hour, int minute, int second) {
    uint8_t status_b = rtc_reg_read(RTC_STATUS_B);
    int is_bcd = (status_b & RTC_REGB_DM_BINARY) ? 0 : 1;
    int is_24h = (status_b & RTC_REGB_24HOUR) ? 1 : 0;

    int es = rtc_encode_field_rs(second, is_bcd);
    int em = rtc_encode_field_rs(minute, is_bcd);
    int eh = rtc_encode_hour_rs(hour, is_bcd, is_24h);
    if (es < 0 || em < 0 || eh < 0 || second > 59 || minute > 59) {
        kprintf("[RTC] set_time REFUSED %02d:%02d:%02d (regB=0x%02x)\n",
                hour, minute, second, status_b);
        return -1;
    }

    rtc_wait_update_done();

    uint64_t flags = rtc_irq_save_cli();
    uint8_t saved = rtc_reg_read(RTC_STATUS_B);
    rtc_reg_write(RTC_STATUS_B, (uint8_t)(saved | RTC_REGB_SET));
    rtc_reg_write(RTC_SECONDS, (uint8_t)es);
    rtc_reg_write(RTC_MINUTES, (uint8_t)em);
    rtc_reg_write(RTC_HOURS,   (uint8_t)eh);
    rtc_reg_write(RTC_STATUS_B, (uint8_t)(saved & (uint8_t)~RTC_REGB_SET));
    rtc_irq_restore(flags);

    return 0;
}

int rtc_set_date(int day, int month, int year) {
    uint8_t status_b = rtc_reg_read(RTC_STATUS_B);
    int is_bcd = (status_b & RTC_REGB_DM_BINARY) ? 0 : 1;

    // The chip holds a two-digit year and no decoder in this tree reads a
    // century register, so a year outside 2000..2099 cannot be represented
    // here and must be refused rather than silently folded.
    if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || day > 31) {
        kprintf("[RTC] set_date REFUSED %04d-%02d-%02d (regB=0x%02x)\n",
                year, month, day, status_b);
        return -1;
    }

    int ed = rtc_encode_field_rs(day, is_bcd);
    int emo = rtc_encode_field_rs(month, is_bcd);
    int ey = rtc_encode_field_rs(year % 100, is_bcd);
    if (ed < 0 || emo < 0 || ey < 0) return -1;

    rtc_wait_update_done();

    uint64_t flags = rtc_irq_save_cli();
    uint8_t saved = rtc_reg_read(RTC_STATUS_B);
    rtc_reg_write(RTC_STATUS_B, (uint8_t)(saved | RTC_REGB_SET));
    rtc_reg_write(RTC_DAY,   (uint8_t)ed);
    rtc_reg_write(RTC_MONTH, (uint8_t)emo);
    rtc_reg_write(RTC_YEAR,  (uint8_t)ey);
    rtc_reg_write(RTC_STATUS_B, (uint8_t)(saved & (uint8_t)~RTC_REGB_SET));
    rtc_irq_restore(flags);

    return 0;
}

// ---------------------------------------------------------------------------
// BOOT REPORT
//
// The iMac has no serial port, so kprintf is invisible on the machine this bug
// was reported from. Everything needed to confirm or refute the diagnosis goes
// to /BOOTLOG.TXT, which the owner can read back:
//
//   regB       the mode byte, the one fact the whole bug turns on
//   raw        the register bytes exactly as the chip returned them, BEFORE
//              any decode, so the encoding is checkable independently of our
//              decoder being right
//   decoded    what this driver made of them
//   selftest   proof the codec in THIS build is the one that was tested
//
// A raw/decoded pair that disagree in the 6*floor(v/10) pattern IS the bug,
// visible without a debugger, a serial cable, or a rebuild.
// ---------------------------------------------------------------------------
void rtc_mode_report(void) {
    uint32_t checks = 0;
    int rc = rtc_selftest_rs(&checks);

    uint8_t b = rtc_reg_read(RTC_STATUS_B);
    uint8_t a = rtc_reg_read(RTC_STATUS_A);
    uint8_t rs = rtc_reg_read(RTC_SECONDS);
    uint8_t rm = rtc_reg_read(RTC_MINUTES);
    uint8_t rh = rtc_reg_read(RTC_HOURS);
    uint8_t rd = rtc_reg_read(RTC_DAY);
    uint8_t rmo = rtc_reg_read(RTC_MONTH);
    uint8_t ry = rtc_reg_read(RTC_YEAR);

    int h = 0, mi = 0, s = 0, d = 1, mo = 1, y = 2026, wd = 0;
    rtc_read_time(&h, &mi, &s);
    rtc_read_date(&d, &mo, &y, &wd);

    const char *mode = (b & RTC_REGB_DM_BINARY) ? "BINARY" : "BCD";
    const char *hfmt = (b & RTC_REGB_24HOUR) ? "24h" : "12h";

    kprintf("[RTC] codec selftest %s (%u checks); regA=0x%02x regB=0x%02x "
            "mode=%s/%s%s\n",
            rc == 0 ? "PASS" : "FAIL", checks, a, b, mode, hfmt,
            (b & RTC_REGB_DSE) ? "/DSE" : "");
    kprintf("[RTC] raw s=%02x m=%02x h=%02x d=%02x mo=%02x y=%02x -> "
            "%04d-%02d-%02d %02d:%02d:%02d\n",
            rs, rm, rh, rd, rmo, ry, y, mo, d, h, mi, s);

    bootlog_write("[RTC] selftest=%s checks=%u regA=0x%02x regB=0x%02x mode=%s/%s%s",
                  rc == 0 ? "PASS" : "FAIL", checks, a, b, mode, hfmt,
                  (b & RTC_REGB_DSE) ? "/DSE" : "");
    bootlog_write("[RTC] raw s=%02x m=%02x h=%02x d=%02x mo=%02x y=%02x decoded=%04d-%02d-%02d %02d:%02d:%02d",
                  rs, rm, rh, rd, rmo, ry, y, mo, d, h, mi, s);
}

// ---------------------------------------------------------------------------
// LIVE HARDWARE DIFFERENTIAL - `make RTCBINTEST=1`. OFF in the golden.
//
// WHY A BUILD FLAG AND NOT ALWAYS ON: this test WRITES Status Register B to put
// the chip into the other data mode, and writes the time. Doing that on a
// user's machine unprompted is exactly the kind of thing that leaves a real
// iMac with a halted or reinterpreted RTC. It restores everything it touched,
// but "it restores it" is not a good enough reason to run it on someone's
// hardware, so it is compiled out unless asked for. Same shape as NOBLOCKTEST.
//
// WHY IT EXISTS ANYWAY: a host-side unit test of the codec proves the
// arithmetic, not the DRIVER. This runs the real rtc_set_time() against a real
// MC146818 through real port I/O, in Ring 0, in the shipping kernel, in both
// data modes, and prints the answer the chip gives back. It is the WRONG-then-
// RIGHT witness: the OLD encoder's exact arithmetic is reproduced here (clearly
// labelled) so both arms hit the same chip in the same boot.
//
// QEMU's mc146818 honours the DM bit, so a plain VM reaches the binary-mode
// arm that real BCD-mode hardware never would.
// ---------------------------------------------------------------------------
#ifdef RTC_LIVE_TEST

// THE BUG, verbatim, as it shipped in proc/syscall.c. Not a paraphrase:
//     static uint8_t bin_to_bcd(uint8_t v) { return ((v/10)<<4)|(v%10); }
// applied with no reference to Status Register B.
static uint8_t rtc_old_bin_to_bcd(uint8_t v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static void rtc_force_mode(uint8_t regb_saved, int binary) {
    uint8_t b = regb_saved;
    if (binary) b |= RTC_REGB_DM_BINARY; else b &= (uint8_t)~RTC_REGB_DM_BINARY;
    b |= RTC_REGB_24HOUR;                 // pin 24h so the arms are comparable
    rtc_reg_write(RTC_STATUS_B, (uint8_t)(b | RTC_REGB_SET));
    rtc_reg_write(RTC_STATUS_B, b);
}

// Write 13:15:00 the OLD way (unconditional BCD), then read it back through the
// normal driver. Returns minutes-of-day as the chip reports them.
static int rtc_probe_old(int h, int m) {
    uint64_t fl = rtc_irq_save_cli();
    uint8_t saved = rtc_reg_read(RTC_STATUS_B);
    rtc_reg_write(RTC_STATUS_B, (uint8_t)(saved | RTC_REGB_SET));
    rtc_reg_write(RTC_SECONDS, rtc_old_bin_to_bcd(0));
    rtc_reg_write(RTC_MINUTES, rtc_old_bin_to_bcd((uint8_t)m));
    rtc_reg_write(RTC_HOURS,   rtc_old_bin_to_bcd((uint8_t)h));
    rtc_reg_write(RTC_STATUS_B, saved);
    rtc_irq_restore(fl);
    int rh = 0, rm = 0, rs = 0;
    rtc_read_time(&rh, &rm, &rs);
    return rh * 60 + rm;
}

static int rtc_probe_new(int h, int m) {
    if (rtc_set_time(h, m, 0) != 0) return -1;
    int rh = 0, rm = 0, rs = 0;
    rtc_read_time(&rh, &rm, &rs);
    return rh * 60 + rm;
}

void rtc_live_test(void) {
    const int SET_H = 13, SET_M = 15;
    const int want = SET_H * 60 + SET_M;

    // Save EVERYTHING we are about to disturb, raw.
    uint8_t regb0 = rtc_reg_read(RTC_STATUS_B);
    uint8_t s0 = rtc_reg_read(RTC_SECONDS);
    uint8_t m0 = rtc_reg_read(RTC_MINUTES);
    uint8_t h0 = rtc_reg_read(RTC_HOURS);

    kprintf("[RTCTEST] chip as found: regB=0x%02x (%s/%s)\n", regb0,
            (regb0 & RTC_REGB_DM_BINARY) ? "BINARY" : "BCD",
            (regb0 & RTC_REGB_24HOUR) ? "24h" : "12h");

    int fails = 0;
    for (int binary = 0; binary <= 1; binary++) {
        rtc_force_mode(regb0, binary);
        const char *mode = binary ? "BINARY" : "BCD   ";

        int got_old = rtc_probe_old(SET_H, SET_M);
        int got_new = rtc_probe_new(SET_H, SET_M);
        int err_old = got_old - want;
        int err_new = got_new - want;

        kprintf("[RTCTEST] %s: set %02d:%02d -> OLD reads %02d:%02d (err %+dh%02dm)"
                "  NEW reads %02d:%02d (err %+dh%02dm)\n",
                mode, SET_H, SET_M,
                got_old / 60, got_old % 60, err_old / 60,
                (err_old < 0 ? -err_old : err_old) % 60,
                got_new / 60, got_new % 60, err_new / 60,
                (err_new < 0 ? -err_new : err_new) % 60);

        bootlog_write("[RTCTEST] %s set=%02d:%02d old=%02d:%02d(err%+d) new=%02d:%02d(err%+d)",
                      mode, SET_H, SET_M, got_old / 60, got_old % 60, err_old,
                      got_new / 60, got_new % 60, err_new);

        if (err_new != 0) fails++;
        if (binary && err_old != 6 * 60 + 6) {
            kprintf("[RTCTEST] NOTE: binary-mode OLD error was %+d min, expected +366\n",
                    err_old);
        }
    }

    // Restore mode first, then the raw bytes, so they are reinterpreted the way
    // they were written.
    rtc_force_mode(regb0, (regb0 & RTC_REGB_DM_BINARY) ? 1 : 0);
    uint64_t fl = rtc_irq_save_cli();
    rtc_reg_write(RTC_STATUS_B, (uint8_t)(regb0 | RTC_REGB_SET));
    rtc_reg_write(RTC_SECONDS, s0);
    rtc_reg_write(RTC_MINUTES, m0);
    rtc_reg_write(RTC_HOURS,   h0);
    rtc_reg_write(RTC_STATUS_B, regb0);
    rtc_irq_restore(fl);

    kprintf("[RTCTEST] %s (regB restored to 0x%02x)\n",
            fails == 0 ? "PASS - new encoder exact in BOTH data modes"
                       : "FAIL - new encoder was wrong in a mode",
            rtc_reg_read(RTC_STATUS_B));
    bootlog_write("[RTCTEST] %s", fails == 0 ? "PASS" : "FAIL");
}
#endif // RTC_LIVE_TEST
