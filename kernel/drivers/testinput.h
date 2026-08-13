// drivers/testinput.h - Deterministic host->guest synthetic input channel (#334)
#ifndef DRIVERS_TESTINPUT_H
#define DRIVERS_TESTINPUT_H

// Arm the DEBUG-GATED serial (COM1) input-injection channel, but ONLY if
// /TESTINPUT.TXT exists on the FAT ESP. Safe no-op otherwise (zero surface on a
// shipping golden). Call once, late in boot, after FAT mount and proc_init so a
// kernel worker thread can be created. See drivers/testinput.c for the wire
// protocol and docs/GUI_TEST_INPUT.md for the host-side procedure.
void testinput_init(void);

#endif
