// drivers/rtc.h - #135: the MC146818 CMOS real-time clock, ONE owner.
//
// Before #135 this hardware had no owner. Its port I/O lived in
// gui/clock.c (a desktop widget), a private duplicate lived in gui/desktop.c,
// and the WRITE half lived in proc/syscall.c with a third, incompatible idea
// of how a register is encoded. That asymmetry is what shipped the 6h06m
// clock error; see rustkern/rtcenc.rs for the arithmetic.
//
// Everything that touches the chip now goes through here, and both directions
// share rustkern/rtcenc.rs, so a decoder and an encoder that disagree is no
// longer expressible.

#ifndef DRIVERS_RTC_H
#define DRIVERS_RTC_H

#include "../types.h"

// Status Register B mode bits, as the chip defines them.
#define RTC_REGB_DM_BINARY  0x04   // set: registers are BINARY, clear: packed BCD
#define RTC_REGB_24HOUR     0x02   // set: 24-hour, clear: 12-hour with a PM bit
#define RTC_REGB_DSE        0x01   // set: chip does its own daylight-saving jumps
#define RTC_REGB_SET        0x80   // set: updates frozen (write window)

// Raw Status Register B. Exposed because "what mode is this chip in" is the
// single fact that decides whether a clock write is correct, and on the
// owner's iMac it is not observable any other way (no serial port).
uint8_t rtc_status_b(void);

// READ. Signatures are unchanged from the versions that used to live in
// gui/clock.c, so every existing caller (syscalls 142/143, rustkern/ktime.rs's
// wall clock, gui/login.c, and all of userland via libc/tz.c) keeps working.
// Values are whatever the chip holds; this layer does no timezone arithmetic.
void rtc_read_time(int *hour, int *minute, int *second);
void rtc_read_date(int *day, int *month, int *year, int *weekday);

// WRITE. Returns 0 on success, -1 if the caller's values are not representable
// (in which case NOTHING is written and the previous time survives, the same
// refusal discipline rustkern/ktime.rs applies to implausible dates).
//
// `hour` is a 24-hour clock hour 0..23 regardless of what mode the chip is in;
// the 12-hour conversion, if the chip needs one, happens here.
int rtc_set_time(int hour, int minute, int second);
int rtc_set_date(int day, int month, int year);

// One line to serial AND to /BOOTLOG.TXT describing the chip's mode and the
// raw bytes it currently holds. Called once at boot.
//
// This exists because the iMac has NO SERIAL PORT, so kprintf is invisible
// there, and the whole diagnosis of #135 turns on a single bit (Status
// Register B bit 2) that nobody can read off that machine. The persistent log
// is the only channel back.
void rtc_mode_report(void);

#endif // DRIVERS_RTC_H
