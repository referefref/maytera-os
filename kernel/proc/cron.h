// cron.h - #265 cron-like timer/scheduler subsystem for MayteraOS.
//
// A small in-kernel scheduler that fires jobs on a schedule. It complements
// the #95 background-services subsystem: a service handles a long-lived
// process, while cron handles *time-driven* work (run something every N
// seconds, once at a future moment, or daily/weekly at a wall-clock time).
//
// Two clocks are used:
//   - INTERVAL / ONESHOT jobs are driven by the monotonic 250 Hz tick counter
//     (timer_ticks), so they are immune to RTC drift / clock changes.
//   - DAILY / WEEKLY jobs are driven by the RTC wall clock (HH:MM, weekday).
//
// A cheap hook (cron_tick) runs off the scheduler tick to flag due tick-based
// jobs; the actual firing happens in a dedicated kernel worker process
// (cron_start_worker) so actions may safely block (filesystem I/O,
// proc_create launches) without stalling the timer interrupt.
//
// Jobs persist in /CONFIG/CRON.CFG (one per line) and are reloaded at boot, so
// scheduled work survives a reboot. The on-disk text format is:
//
//   <TYPE> <WHEN> <ACTION> <TARGET> [label...]
//
//   TYPE   = INTERVAL | ONESHOT | DAILY | WEEKLY
//   WHEN   = INTERVAL: seconds        (e.g. 60)
//            ONESHOT : seconds-from-load (e.g. 30)
//            DAILY   : HH:MM          (e.g. 03:00)
//            WEEKLY  : DOW:HH:MM      (DOW 0=Sun..6=Sat, e.g. 1:09:30)
//   ACTION = callback | launch | event
//   TARGET = callback name | program path | event name
//
// e.g.
//   INTERVAL 60 launch /APPS/SOMETHING
//   DAILY 03:00 callback backup            daily backup
//
// The cron_job_t struct below is the wire format shared with libc/userland
// (SYS_CRON_ADD / SYS_CRON_LIST / SYS_CRON_REMOVE / SYS_CRON_ENABLE), so keep
// the layout stable.

#ifndef MAYTERA_CRON_H
#define MAYTERA_CRON_H

#include "../types.h"

#define CRON_MAX_JOBS    32
#define CRON_LABEL_MAX   32
#define CRON_TARGET_MAX  64

// Schedule types.
#define CRON_TYPE_ONESHOT   0   // fire once at next_fire_tick, then disable
#define CRON_TYPE_INTERVAL  1   // fire every interval_ms (tick clock)
#define CRON_TYPE_DAILY     2   // fire daily at hour:minute (wall clock)
#define CRON_TYPE_WEEKLY    3   // fire weekly on weekday at hour:minute

// Action types.
#define CRON_ACT_CALLBACK   0   // invoke a registered kernel callback (target=name)
#define CRON_ACT_LAUNCH     1   // launch an ELF program (target=path)
#define CRON_ACT_EVENT      2   // emit a named event to serial/syslog (target=name)

// Userland-visible job record (wire format for the SYS_CRON_* syscalls).
typedef struct {
    uint32_t id;             // unique job id (assigned by the kernel)
    uint8_t  type;           // CRON_TYPE_*
    uint8_t  action;         // CRON_ACT_*
    uint8_t  enabled;        // 1 = active
    uint8_t  weekday;        // 0=Sun..6=Sat (WEEKLY only)
    uint8_t  hour;           // 0-23 (DAILY/WEEKLY)
    uint8_t  minute;         // 0-59 (DAILY/WEEKLY)
    uint8_t  reserved[2];
    uint32_t interval_ms;    // INTERVAL/ONESHOT delay, milliseconds
    uint32_t run_count;      // number of times the job has fired
    uint64_t next_fire_tick; // monotonic tick of next fire (tick-based jobs)
    char     target[CRON_TARGET_MAX]; // callback name / program path / event name
    char     label[CRON_LABEL_MAX];   // human label
} cron_job_t;

// Kernel callback signature; id is the firing job's id.
typedef void (*cron_callback_fn)(uint32_t id, void *ctx);

// Build the registry (built-in callbacks) and load /CONFIG/CRON.CFG. Safe to
// call once at boot, after the filesystem is mounted.
void cron_init(void);

// Spawn the cron worker kernel process. Call once after the scheduler is up.
void cron_start_worker(void);

// Cheap hook off sched_tick(): flags tick-based jobs that have come due so the
// worker wakes promptly. Does no I/O and fires nothing itself.
void cron_tick(void);

// Public API. cron_add returns the new job id (>0) or <0 on error; the other
// three return 0 on success or <0. All four persist the registry to disk.
int  cron_add(const cron_job_t *job);
int  cron_remove(uint32_t id);
int  cron_enable(uint32_t id, int enable);

// Copy a snapshot of all jobs into out[0..max-1]; returns the count copied.
int  cron_list(cron_job_t *out, int max);

// Register a named kernel callback that a CRON_ACT_CALLBACK job can fire.
// Returns 0 on success, <0 if the table is full. Safe to call at boot.
int  cron_register_callback(const char *name, cron_callback_fn fn, void *ctx);

// Add a job WITHOUT persisting it to disk (used for built-in / self-test jobs).
// Returns the new id (>0) or <0.
int  cron_add_volatile(const cron_job_t *job);

// Gated serial self-test (#265). No-op unless /CONFIG/CRONTEST.CFG exists.
void cron_start_deferred_selftest(void);

#endif // MAYTERA_CRON_H
