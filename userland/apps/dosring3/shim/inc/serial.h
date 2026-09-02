// serial.h - Ring-3 shim for the dosring3 host (#DOSRING3).
//
// This is the ONE kernel header the Ring-3 DOS host cannot take verbatim: it
// declares an interface to a UART that a Ring-3 process does not own. Every
// other kernel header the DOS sources include is copied unmodified (see
// mkgen.sh) precisely so struct layouts cannot drift.
//
// DELIBERATELY NARROW. Only the symbols the DOS/x86_16 sources actually
// reference are declared. If an upstream change starts using another serial
// facility, this MUST fail to compile rather than silently resolve, so the new
// dependency is a build error someone has to think about, not a runtime
// surprise. That is the same discipline the rest of the shim follows: no
// silent plausible answers.
#ifndef SERIAL_H
#define SERIAL_H
#include "types.h"

// The kernel echoes guest console output (INT 21h AH=02h/06h/09h/40h and INT
// 10h AH=0Eh) to COM1 as a diagnostic trace of what the guest printed. A Ring-3
// process has no UART, so the port argument is retained for source
// compatibility and the byte is routed to this process's own diagnostic
// channel. The MEANING is preserved exactly: "mirror what the guest wrote to
// the host's debug log". Only the transport differs.
#define COM1 0x3F8
void serial_write(uint16_t port, char c);

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kputs(const char *str);
void kputc(char c);
// The kernel's panic-path unlocked emitter. In Ring 3 there is no lock to
// bypass and no panic to order against, so it is the same sink as kprintf.
void kprintf_nolock(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#endif // SERIAL_H
