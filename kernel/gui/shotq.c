// gui/shotq.c - Stage 1 screen.capture: the kernel-mediated screenshot request
// queue that REPLACES the ambient /SCREENSHOT.REQ file drop
// (docs/SYSTEM_CAPABILITY_API.md sections 1.3 and 4.2).
//
// THE AMBIENT PATH THIS REMOVES. screenshot_poll() in the compositor used to
// open("/SCREENSHOT.REQ"), read an ATTACKER-SUPPLIED absolute output path from
// the body, and capture the whole composited backbuffer - every window on
// screen - to that path, with no uid check, no capability, no consent and no
// audit record. Any Ring-3 app that could write a file the compositor reads
// could drive it. That file is deleted; the compositor now polls THIS queue,
// and the only Ring-3 way into it is SYS_SCREENSHOT_REQUEST, which the
// dispatcher capability chokepoint gates on screen.capture and which this
// handler additionally binds to the exact granted path.
//
// The queue is a tiny ring. Every producer and the single consumer run in a
// syscall body under the Big Kernel Lock, so accesses are already serialised
// and no extra lock is needed (adding one would be a second place to be wrong).
#include "../proc/caps.h"
#include "../proc/process.h"
#include "../fs/perms.h"
#include "../security/validate.h"   // strncpy_from_user
#include "../string.h"
#include "../serial.h"
#include "../security/seclog.h"   // seclog_report_capability

extern int fb_owner_is(uint32_t pid);
extern uint64_t sched_now_ms(void);

#define SHOTQ_MAX      4
#define SHOTQ_PATHLEN  128

static char shotq[SHOTQ_MAX][SHOTQ_PATHLEN];
static int  shotq_head;   // next to dequeue
static int  shotq_tail;   // next to enqueue

static int shotq_full(void)  { return ((shotq_tail + 1) % SHOTQ_MAX) == shotq_head; }
static int shotq_empty(void) { return shotq_head == shotq_tail; }

// Kernel-trusted enqueue: for the serial/debug capture path (a Ring-0 caller,
// e.g. a testinput SHOT verb on a marker-gated build). No capability check
// because a Ring-0 caller is not the threat this gate exists for; the ambient
// Ring-3 file drop was. Returns 0 on success, -1 if the ring is full.
int screenshot_enqueue(const char *path)
{
    if (!path || !path[0]) return -1;
    if (shotq_full()) return -1;
    strncpy(shotq[shotq_tail], path, SHOTQ_PATHLEN - 1);
    shotq[shotq_tail][SHOTQ_PATHLEN - 1] = 0;
    shotq_tail = (shotq_tail + 1) % SHOTQ_MAX;
    return 0;
}

// SYS_SCREENSHOT_REQUEST (Ring 3). The dispatcher chokepoint has already proven
// the caller holds a live screen.capture grant of SOME scope. Here we bind that
// to the EXACT requested path, confirm the caller could already write it
// (perms_check, the noun's own precondition), consume one use of the grant, and
// enqueue. Order matters: validate before consuming, so a rejected request does
// not burn a use.
int64_t sys_screenshot_request(const char *u_path)
{
    process_t *p = proc_current();
    if (!p) return -1;

    char path[SHOTQ_PATHLEN];
    if (strncpy_from_user(path, u_path, sizeof(path)) < 0) return CAP_EARG;
    path[sizeof(path) - 1] = 0;

    // The grant must cover THIS path (exact scope), without consuming yet.
    uint64_t now = sched_now_ms();
    int idx = cap_covers_path(p->cap_grants, CAP_MAX_GRANTS, CAP_SCREEN_CAPTURE, path, now);
    if (idx < 0) {
        cap_ledger_note(CAP_LEDGER_REFUSED);
        char d[160];
        snprintf(d, sizeof(d), "REFUSED screen.capture: grant does not cover %s", path);
        seclog_report_capability((unsigned)p->pid, d);
        return CAP_EDENIED;
    }

    // The noun is a path the caller could already write (design 4.2). This is
    // the same perms_check() the whole filesystem uses; the grant does not
    // widen it.
    if (perms_check(path, p->euid, p->egid, W_OK) != 0)
        return -13;   // EACCES

    if (shotq_full()) return -11;   // EAGAIN: transient, not a permission answer

    if (screenshot_enqueue(path) != 0) return -11;

    (void)cap_consume_use(p->cap_grants, CAP_MAX_GRANTS, (uint32_t)idx);
    cap_ledger_note(CAP_LEDGER_ALLOWED);
    return 0;
}

// SYS_SCREENSHOT_POLL (compositor only). Dequeue the next requested path into
// the caller's buffer. Returns the path length (>0) or 0 if none pending. The
// compositor then captures its backbuffer to that path exactly as before; the
// ONLY change is where the path came from - a kernel-mediated, cap-gated queue
// instead of a world-writable file.
int64_t sys_screenshot_poll(char *u_out, int cap)
{
    process_t *p = proc_current();
    if (!p) return -1;
    if (!fb_owner_is(p->pid)) return -1;   // compositor principal only
    if (shotq_empty()) return 0;
    if (!u_out || cap <= 0) return -1;

    char path[SHOTQ_PATHLEN];
    strncpy(path, shotq[shotq_head], sizeof(path) - 1);
    path[sizeof(path) - 1] = 0;
    shotq_head = (shotq_head + 1) % SHOTQ_MAX;

    int n = (int)strlen(path);
    if (n > cap - 1) n = cap - 1;
    if (copy_to_user(u_out, path, (unsigned long)(n + 1)) != 0) return -1;
    return n;
}
