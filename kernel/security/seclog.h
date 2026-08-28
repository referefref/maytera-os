// seclog.h - security event sink: persistent log + desktop notifications (#653)
#ifndef SECLOG_H
#define SECLOG_H

// Start the PRIO_LOW worker that drains the security audit ring to
// /CONFIG/SECURITY.LOG and raises desktop notifications for CRITICAL/WARNING.
// Call AFTER the root filesystem is mounted and the scheduler is running.
void seclog_init(void);

// Wake the worker. Called from security_audit(). Safe from ANY context,
// including interrupts-off and spinlock-held paths: it does no I/O and no
// allocation, only a wait-queue wake. This is the whole reason the worker
// exists - see the header comment in seclog.c.
void seclog_kick(void);

// #653: narrow producer wrapper for proc/syscall.c.
//
// WHY THIS EXISTS INSTEAD OF syscall.c INCLUDING security.h DIRECTLY:
//
// #646 CORRECTION: the original reason recorded here is NO LONGER TRUE, so it
// is corrected rather than left to mislead the next reader. It said security.h
// transitively includes security/overflow.h, whose validate_rect() helpers
// collide with gui/window.h rect_t in the syscall.c translation unit. That was
// accurate at the time; overflow.{c,h} have since been DELETED as an entirely
// unused module (#646), so the header collision is gone and syscall.c could now
// include security.h.
//
// The wrapper is kept anyway, on its own merits: it is a one-function producer
// surface with no type dependencies, so it can be pulled into any translation
// unit, and it keeps the audit_event_t enum in exactly one place (security.h)
// rather than duplicating an ordinal at the call site.
void seclog_report_bad_user_ptr(unsigned int pid, const char *detail);

// #745: the same narrow-producer pattern for privilege elevation, so
// proc/elevate.c needs no security.h include and the audit ordinal stays in
// exactly one place. Every raise, cancel, wrong password and grant goes through
// here, so /CONFIG/SECURITY.LOG carries the actor and the outcome of every
// system-wide install that was ever asked for, not only the ones that
// succeeded.
void seclog_report_elevation(unsigned int pid, const char *detail);

// #745: the same narrow-producer pattern for the AI prompt-injection screen, so
// proc/syscall.c needs no security.h include (it already includes this header
// for seclog_report_bad_user_ptr) and the audit ordinal stays in exactly one
// place. Every refused and every flagged LLM request goes through here, so
// /CONFIG/SECURITY.LOG carries the ACTOR pid and the rule for each one.
void seclog_report_ai_injection(unsigned int pid, const char *detail);

// #fdguard: the same narrow-producer pattern for cross-process I/O boundary
// refusals, so proc/fdlayer.c and drivers/pty.c need no security.h include
// (they already include this header) and the audit ordinal stays in one
// place. Every refused cross-process fd op and every refused /dev/pts attach
// goes through here, so /CONFIG/SECURITY.LOG carries the actor pid, the
// target and the reason for each one.
void seclog_report_io_boundary(unsigned int pid, const char *detail);

#endif // SECLOG_H
