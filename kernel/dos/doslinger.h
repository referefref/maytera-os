// dos/doslinger.h - C view of rustkern/doslinger.rs, the DOS post-exit linger
// policy. See that file for why the linger exists and why every duration in
// it lives THERE and not at the call site.
//
// Purity contract, because two of these are wait_event() conditions:
//   dos_linger_frame_done_rs()  atomic load + integer compare, no lock
//   dos_linger_hold_done_rs()   atomic load + integer compare, no lock
// Both are safe to evaluate from inside the wait_event*() macros.

#ifndef DOS_DOSLINGER_H
#define DOS_DOSLINGER_H

#include "../types.h"

void     dos_linger_reset_rs(void);
void     dos_linger_close_rs(void);
int      dos_linger_wanted_rs(int self_exit, uint32_t published);
void     dos_linger_arm_hold_rs(uint64_t now_ms);
int      dos_linger_frame_done_rs(uint64_t flips_since_publish);
int      dos_linger_hold_done_rs(uint64_t now_ms);
uint32_t dos_linger_frame_backstop_ms_rs(void);
uint32_t dos_linger_hold_ms_rs(void);

#endif /* DOS_DOSLINGER_H */
