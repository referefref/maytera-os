// drivers/testinput.h - Deterministic host->guest synthetic input channel (#334)
#ifndef DRIVERS_TESTINPUT_H
#define DRIVERS_TESTINPUT_H

// Arm the DEBUG-GATED serial (COM1) input-injection channel, but ONLY if
// /TESTINPUT.TXT exists on the FAT ESP. Safe no-op otherwise (zero surface on a
// shipping golden). Call once, late in boot, after FAT mount and proc_init so a
// kernel worker thread can be created. See drivers/testinput.c for the wire
// protocol and docs/GUI_TEST_INPUT.md for the host-side procedure.
void testinput_init(void);

// #197: called from sys_get_mouse() (gui/fb_syscall.c) whenever the value
// handed to the compositor carries a LEFT-BUTTON EDGE, so a CLICK that is
// latching its injected level until the compositor actually samples it can be
// released the instant that happens instead of guessing with a fixed sleep.
// Always safe to call: a no-op until the channel arms its wait queue, and the
// channel arms it whether or not /TESTINPUT.TXT is present.
//   edge: 1 = press (0 -> 1), 2 = release (1 -> 0)
void testinput_click_edge(int edge);

#endif
