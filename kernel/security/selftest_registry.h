// security/selftest_registry.h - #PERMSKIP: a self-test that declines to run
// must say so through here.
//
// THE RULE, and it is enforced by kernel/tools/skipgate at build time:
//
//   Inside a function whose name ends in _selftest, a printed line that says
//   SKIP (or "skipped", or "not run") must be produced by selftest_notrun().
//   Printing one yourself does not compile past the gate.
//
// WHY. fs/perms.c's directory-traversal vectors were armed by the literal path
// "/HOME/ADMIN". The first-boot wizard lets the owner name the account, so on
// a machine whose owner is called anything else the kernel printed
//
//     [PERMS-SELFTEST] SKIP traversal vectors (/HOME/ADMIN not 1000:0750)
//
// on EVERY boot, FOREVER, in a line that reads like ordinary noise about an
// unprovisioned image. The traversal half of the permission model has probably
// never been exercised on a real user's machine. A self-test that stops
// testing has to be louder than one that passes, not quieter, and it has to be
// louder in the log that survives a power cycle: the two machines whose
// evidence matters (the ASUS laptop, the iMac14,4) have no serial port, so
// kprintf() produces literally nothing there.
//
// So selftest_notrun() writes through bootlog_write(), and
// selftest_notrun_report() prints the per-boot summary through the same sink.
// A reader of a recovered /BOOTLOG.TXT can answer "did anything decline to
// verify itself on this machine" with one grep.

#ifndef SELFTEST_REGISTRY_H
#define SELFTEST_REGISTRY_H

// Record that `group` could not run, and why. Prints ONE loud durable line.
// `group` is a short stable identifier ("perms/traversal"); `reason` is a
// sentence a person who does not know the code can act on. Neither is copied
// by reference: both are snapshotted into the register.
void selftest_notrun(const char *group, const char *reason);

// Record that a self-test group DID run. Only so the summary can say
// "13 ran, 1 did not"; a bare "1 did not" gives a reader nothing to scale it
// against.
void selftest_ran(const char *group);

// One durable summary line per boot, plus one line per not-run group. Called
// from kernel/main.c after the login-time self-tests, which is the last point
// at which a group can still declare itself.
void selftest_notrun_report(void);

#endif // SELFTEST_REGISTRY_H
