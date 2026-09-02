// procinfo.c - #487/#349 Ring-3 process-introspection syscall backends.
//
// The C here does exactly two things: walk kernel structures that must stay C
// (the fd table needs process_t; the service registry needs service_t), and
// validate the caller's pointer. Every byte copied into the caller's buffer is
// copied by the Rust builders (rustkern.rs) through exactly-sized slices.
#include "procinfo.h"
#include "procmem.h"
#include "../sync/spinlock.h"   // #SMPGLOBALS: process_t::fd_lock
#include "process.h"
#include "services.h"
#include "../serial.h"
#include "../fs/vfs.h"
#include "../net/tcp.h"
#include "../security/validate.h"
#include "../security/uaccess_smap.h"  // #19/#645: AC bracket around the row builders

// ---------------------------------------------------------------------------
// C reference twins. Kept VERBATIM alongside the Rust for the boot differential
// and as the rollback when -DRUST_PROCINFO is dropped.
// ---------------------------------------------------------------------------
static void c_cstr_into(char *dst, uint32_t cap, const char *src) {
    if (!dst || cap == 0) return;
    for (uint32_t i = 0; i < cap; i++) dst[i] = 0;
    if (!src) return;
    uint32_t room = cap - 1;
    uint32_t i = 0;
    while (i < room && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int handles_build_c(const handle_src_t *src, uint32_t n, handle_info_t *out, uint32_t cap) {
    if (!src || !out) return -1;
    if (cap == 0 || n == 0) return 0;
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (w >= cap) break;
        out[w].fd = src[i].fd;
        out[w].flags = src[i].flags;
        out[w].kind = src[i].kind;
        out[w]._pad = 0;
        c_cstr_into(out[w].path, PI_PATH_MAX, src[i].path);
        w++;
    }
    return (int)w;
}

int svc_build_c(const svc_src_t *src, uint32_t n, svc_info_t *out, uint32_t cap) {
    if (!src || !out) return -1;
    if (cap == 0 || n == 0) return 0;
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (w >= cap) break;
        out[w].running = src[i].running;
        out[w].autostart = src[i].autostart;
        out[w].perms = src[i].perms;
        out[w].pid = src[i].pid;
        c_cstr_into(out[w].name, PI_NAME_MAX, src[i].name);
        c_cstr_into(out[w].account, PI_NAME_MAX, src[i].account);
        w++;
    }
    return (int)w;
}

// ---------------------------------------------------------------------------
// Gatherers (C: these must touch process_t / service_t / file_t).
// ---------------------------------------------------------------------------

// Classify a handle by its recorded path. The path is the only thing file_t
// carries that distinguishes a device from a regular file without reaching into
// per-fs private state, and it is exactly what Process Explorer groups on.
//
// #120: THE PREFIX MATCHING THAT USED TO BE HERE IS GONE. It was an open-coded
// p[0]=='p' && p[1]=='i' && ... chain, and SYS_FSTAT needed the same decision.
// Two copies of one classification is this codebase's most repeated defect, so
// there is now one, in rustkern/fstatkind.rs, and this asks it. Not a rewrite
// for its own sake: the shared one also recognises "socket:[...]", so
// PI_KIND_SOCKET - defined here since #487 and NEVER ONCE RETURNED, because
// sockets recorded no path - is finally reachable.
static uint32_t handle_kind_of(const file_t *f) {
    if (!f) return PI_KIND_UNKNOWN;
    kstat_kind_t k;
    fstat_kind_rs(f->path, (uint32_t)sizeof(f->path), &k);
    return k.kind;
}

#define PI_MAX_HANDLES 64   // >= MAX_FDS; bounds the on-stack gather array
// #503: the argtab (rustkern.rs CAP_HANDLES) validates min(max, this) rows, to
// match the clamp below exactly. Raising this without raising CAP_HANDLES would
// leave the extra rows' bytes unvalidated by the dispatcher.
_Static_assert(PI_MAX_HANDLES == 64, "#503 argtab: CAP_HANDLES in rustkern.rs is stale");

int64_t sys_proc_handles(uint32_t pid, void *ubuf, int max) {
    if (!ubuf || max <= 0) return -1;
    if (max > PI_MAX_HANDLES) max = PI_MAX_HANDLES;
    // Validate BEFORE any write. See procinfo.h: the rest of the syscall
    // surface does not do this; new syscalls will not add to that debt.
    if (validate_user_ptr(ubuf, (size_t)max * sizeof(handle_info_t),
                          ACCESS_RW_USER) != VALIDATE_OK) {
        return -1;
    }
    process_t *p = proc_get(pid);
    if (!p) return -1;

    handle_src_t src[PI_MAX_HANDLES];
    uint32_t n = 0;
    // #SMPGLOBALS: this is THE cross-process reader of an fd table, and the
    // only one. Every other toucher of fds[] works on proc_current(), which
    // runs on one core at a time; the Task Manager walks a table its owner is
    // concurrently mutating. Take that process's fd_lock for the whole scan,
    // so a slot cannot be read mid-swap and the census cannot straddle a
    // close. handle_kind_of() only inspects f->ops, so it is safe under the
    // lock; nothing here blocks.
    //
    // NOTE, and this is not fixed by the lock: src[].path is a pointer INTO
    // the description. The owner can still close and free it after we drop
    // the lock. See the fd_get() note in fs/vfs.c.
    { uint64_t __fl = spinlock_acquire_irqsave(&p->fd_lock);
      for (int fd = 0; fd < MAX_FDS && n < (uint32_t)max; fd++) {
        file_t *f = p->fds[fd];
        if (!f) continue;
        src[n].fd = fd;
        src[n].flags = f->flags;
        src[n].kind = handle_kind_of(f);
        src[n]._pad = 0;
        src[n].path = f->path;
        n++;
      }
      spinlock_release_irqrestore(&p->fd_lock, __fl); }
    // #19/#645: the BUILDER is this path's copy engine (see the file header:
    // every byte reaching the caller's buffer is written by it through
    // exactly-sized slices), so the AC window is around the copy engine, which
    // is exactly where mm/uaccess.asm puts its own. validate_user_ptr() above
    // already proved the destination is ACCESS_RW_USER, i.e. U/S=1, i.e. a
    // guaranteed #PF here without AC.
    uaccess_ac_t __ac = uaccess_begin();
    int64_t __r = handles_build(src, n, (handle_info_t *)ubuf, (uint32_t)max);
    uaccess_end(__ac);
    return __r;
}

int64_t sys_net_conns(uint32_t pid, void *ubuf, int max) {
    if (!ubuf || max <= 0) return -1;
    if (max > TM_PI_MAX_CONNS) max = TM_PI_MAX_CONNS;
    if (validate_user_ptr(ubuf, (size_t)max * sizeof(tcp_conn_info_t),
                          ACCESS_RW_USER) != VALIDATE_OK) {
        return -1;
    }
    tcp_conn_info_t all[TM_PI_MAX_CONNS];
    int n = tcp_conn_snapshot(all, TM_PI_MAX_CONNS);
    if (n <= 0) return 0;
    if (pid == PI_PID_ALL) {
        // Whole table: copy up to max rows straight through.
        // #19/#645: one bulk copy through the canonical primitive instead of a
        // per-row raw store into user memory.
        int w = (n < max) ? n : max;
        if (w > 0 && copy_to_user(ubuf, all, (size_t)w * sizeof(all[0])) != 0) {
            return -1;
        }
        return w;
    }
    // One process: the Rust filter bounds the destination by construction.
    // #19/#645: bracket the builder (this path's copy engine), as above.
    uaccess_ac_t __ac = uaccess_begin();
    int64_t __r = conn_filter_by_pid(all, (uint32_t)n, pid,
                                     (tcp_conn_info_t *)ubuf, (uint32_t)max);
    uaccess_end(__ac);
    return __r;
}

#define PI_MAX_SVCS 32
// #503: mirrored by CAP_SVCS in rustkern.rs. See PI_MAX_HANDLES above.
_Static_assert(PI_MAX_SVCS == 32, "#503 argtab: CAP_SVCS in rustkern.rs is stale");

int64_t sys_svc_list(void *ubuf, int max) {
    if (!ubuf || max <= 0) return -1;
    if (max > PI_MAX_SVCS) max = PI_MAX_SVCS;
    if (validate_user_ptr(ubuf, (size_t)max * sizeof(svc_info_t),
                          ACCESS_RW_USER) != VALIDATE_OK) {
        return -1;
    }
    svc_src_t src[PI_MAX_SVCS];
    uint32_t n = 0;
    int total = svc_count();
    for (int i = 0; i < total && n < (uint32_t)max; i++) {
        service_t *s = svc_at(i);
        if (!s) continue;
        src[n].running = (uint32_t)(svc_is_running(s) ? 1 : 0);
        src[n].autostart = (uint32_t)s->autostart;
        src[n].perms = s->perms;
        src[n].pid = (uint32_t)s->pid;
        src[n].name = s->name;
        src[n].account = s->account;
        n++;
    }
    // #19/#645: bracket the builder (this path's copy engine), as above.
    uaccess_ac_t __ac = uaccess_begin();
    int64_t __r = svc_build(src, n, (svc_info_t *)ubuf, (uint32_t)max);
    uaccess_end(__ac);
    return __r;
}

int64_t sys_svc_control(const char *uname, int action) {
    if (!uname) return -1;
    // #692: starting a service creates a Ring-3 process running as that
    // service's account, and every service the golden ships is uid 0. With
    // no check here, any unprivileged process could mint a root process on
    // demand. Found by a sweep AFTER the three known launcher sites were
    // fixed, which is the whole reason the sweep was run.
    //
    // (#785) This root gate is no longer inert: the Task Manager's Start/Stop
    // buttons and the svcmgr enable/disable path both reach this syscall, so a
    // non-root desktop session is now genuinely refused here rather than merely
    // hypothetically.
    {
        process_t *cur = proc_current();
        if (cur && cur->privilege == PRIV_USER && cur->euid != 0) {
            kprintf("[SVC] DENIED svc_control by uid=%u (root only)\n", cur->euid);
            return -1;
        }
    }
    if (validate_user_string(uname, PI_NAME_MAX) != VALIDATE_OK) return -1;
    // Copy the name into kernel space before using it: the caller could
    // otherwise race the string's contents between validation and use.
    char name[PI_NAME_MAX];
    c_cstr_into(name, PI_NAME_MAX, uname);
    if (action == PI_SVC_START) return svc_start(name);
    if (action == PI_SVC_STOP)  return svc_stop(name);
    // (#785) Durable enable/disable. svc_enable() persists first and returns
    // negative if it could not, so the caller is never told a change is
    // permanent when it is only in RAM.
    if (action == PI_SVC_ENABLE)  return svc_enable(name, 1);
    if (action == PI_SVC_DISABLE) return svc_enable(name, 0);
    return -1;
}

int64_t sys_proc_detail(uint32_t pid, void *uout) {
    if (!uout) return -1;
    if (validate_user_ptr(uout, sizeof(proc_detail_t), ACCESS_RW_USER) != VALIDATE_OK) {
        return -1;
    }
    process_t *p = proc_get(pid);
    if (!p) return -1;

    proc_detail_t d;
    for (uint32_t i = 0; i < sizeof(d); i++) ((uint8_t *)&d)[i] = 0;
    d.pid = p->pid;
    d.ppid = p->ppid;
    d.uid = p->uid;
    d.gid = p->gid;
    d.priority = (uint32_t)p->priority;
    d.privilege = p->privilege;
    d.state = (uint32_t)p->state;
    d.cpu_ticks = p->total_time;
    // #503 / MAYTERA-SEC-2026-0016: cr3 is DELIBERATELY NOT reported.
    //
    // This used to be `d.cr3 = p->cr3;` for ANY pid, with no ownership check, so
    // any unprivileged Ring-3 process could read the physical page-table base of
    // every process on the system. A page-table base is kernel memory layout: it
    // is exactly the primitive that turns a blind write into an aimed one, and
    // handing it out defeats ASLR for free.
    //
    // It is zeroed rather than gated on euid==0 because there is no consumer to
    // preserve: the field has ZERO readers in the whole tree (the userland twin
    // in libc/syscall.h declares it and never touches it), and root in Ring 3 is
    // still Ring 3, so "root may read the kernel's page-table base" is not a
    // privilege anyone here needs. The field stays for ABI (proc_detail_t is
    // size-locked at 112 and the twin must match); it now reads 0 always. If a
    // real need ever appears, add it back as a PRIV_KERNEL-only field on purpose.
    d.cr3 = 0;
    d.threads = proc_thread_count(p->pid);
    d.is_service = p->is_service;
    c_cstr_into(d.name, PI_NAME_MAX, p->name);

    uint32_t nh = 0;
    // #SMPGLOBALS: same reason as the handle scan above - a count taken while
    // the owner is closing fds could otherwise straddle a swap.
    { uint64_t __fl = spinlock_acquire_irqsave(&p->fd_lock);
      for (int fd = 0; fd < MAX_FDS; fd++) if (p->fds[fd]) nh++;
      spinlock_release_irqrestore(&p->fd_lock, __fl); }
    d.handles = nh;

    // #421 phase 5: this is a THIRD caller of proc_mem_fill_in()+
    // proc_mem_account() (process.c's proc_snapshot() and procmem.c's
    // proc_mem_info() are the other two), reachable directly from userland
    // via SYS_PROC_DETAIL (sys_proc_detail, syscall.c). It was missed on the
    // first pass of the g_proc_mm_lock fix (see process.h for the full race
    // writeup) because it lives in a separate file from procmem.c/process.c;
    // grep for every proc_mem_fill_in/proc_mem_account call site before
    // trusting this lock is complete again if a fourth ever appears. Found
    // by reproducing the real crash: with only the first two call sites
    // locked, AssaultCube's phase-4/5 crash-recovery scenario still
    // panicked the kernel at the exact same proc_mem_account_rs address,
    // because sys_proc_detail (polled by Task Manager / procinfo consumers)
    // walked the same being-freed vma_list through this unlocked path.
    proc_mem_in_t mi;
    proc_mem_out_t mo;
    proc_mm_lock();
    proc_mem_fill_in(p, &mi);
    int mem_ok = (proc_mem_account(&mi, &mo) == 1);
    proc_mm_unlock();
    if (mem_ok) {
        d.working_set_kb = mo.working_set_kb;
        d.private_kb = mo.private_kb;
        d.virt_kb = mo.virt_kb;
        d.heap_kb = mo.heap_kb;
        d.vma_count = mo.vma_walked;
        d.mem_flags = mo.flags;
    }
    // #19/#645: one fixed-size struct to user memory, through the primitive.
    if (copy_to_user(uout, &d, sizeof(d)) != 0) return -1;
    return 1;
}

// ---------------------------------------------------------------------------
// #404 boot-time [RUST-DIFF] differential.
//
// Corpus reaches the states a naive builder gets wrong, each counted:
//   - more sources than cap      (the CWE-787 over-write)
//   - cap == 0 / n == 0
//   - a NULL source string       (must yield "", not a fault)
//   - an UNTERMINATED source     (must truncate, not over-read)
//   - a source longer than the field (truncation + terminator)
//   - a source exactly field-1   (exact fit)
// A canary row past `cap` proves neither implementation writes past it.
// ---------------------------------------------------------------------------
static uint32_t pi_rng = 0xA5A51234u;
static uint32_t pi_rand(void) {
    pi_rng ^= pi_rng << 13; pi_rng ^= pi_rng >> 17; pi_rng ^= pi_rng << 5;
    return pi_rng;
}

void procinfo_selftest(void) {
    enum { MAXN = 24 };
    int mism = 0, vecs = 0, canary = 0;
    int cov_overflow = 0, cov_cap0 = 0, cov_null = 0, cov_unterm = 0,
        cov_trunc = 0, cov_exact = 0;

    // A pool of source strings, including a deliberately UNTERMINATED one.
    static char longstr[PI_PATH_MAX * 2];
    for (uint32_t i = 0; i < sizeof(longstr); i++) longstr[i] = 'L';  // no NUL at all
    static char exact[PI_PATH_MAX];
    for (uint32_t i = 0; i < PI_PATH_MAX - 1; i++) exact[i] = 'E';
    exact[PI_PATH_MAX - 1] = 0;

    for (int iter = 0; iter < 400; iter++) {
        handle_src_t src[MAXN];
        uint32_t n = pi_rand() % (MAXN + 1);
        for (uint32_t i = 0; i < n; i++) {
            src[i].fd = (int32_t)i;
            src[i].flags = (int32_t)(pi_rand() % 4);
            src[i].kind = pi_rand() % 5;
            src[i]._pad = 0;
            uint32_t pick = pi_rand() % 4;
            if (pick == 0) { src[i].path = 0; cov_null++; }
            else if (pick == 1) { src[i].path = longstr; cov_unterm++; cov_trunc++; }
            else if (pick == 2) { src[i].path = exact; cov_exact++; }
            else { src[i].path = "/APPS/TASKMGR"; }
        }
        uint32_t cap = pi_rand() % (MAXN + 1);
        if (iter % 9 == 0) cap = 0;
        if (cap == 0) cov_cap0++;
        if (n > cap) cov_overflow++;

        static handle_info_t oc[MAXN + 2], orr[MAXN + 2];
        for (uint32_t i = 0; i < MAXN + 2; i++) { oc[i].fd = 0x7EEE; orr[i].fd = 0x7EEE; }
        int rc = handles_build_c(src, n, oc, cap);
        int rr = handles_build_rs(src, n, orr, cap);
        vecs++;
        if (rc != rr) mism++;
        else {
            for (int i = 0; i < rc; i++) {
                if (oc[i].fd != orr[i].fd || oc[i].flags != orr[i].flags ||
                    oc[i].kind != orr[i].kind) { mism++; break; }
                int bad = 0;
                for (int k = 0; k < PI_PATH_MAX; k++)
                    if (oc[i].path[k] != orr[i].path[k]) { bad = 1; break; }
                if (bad) { mism++; break; }
            }
        }
        if (oc[cap].fd != 0x7EEE || orr[cap].fd != 0x7EEE) canary++;
    }

    // Service builder, same shape.
    for (int iter = 0; iter < 200; iter++) {
        svc_src_t src[MAXN];
        uint32_t n = pi_rand() % (MAXN + 1);
        for (uint32_t i = 0; i < n; i++) {
            src[i].running = pi_rand() % 2;
            src[i].autostart = pi_rand() % 2;
            src[i].perms = pi_rand();
            src[i].pid = pi_rand() % 64;
            src[i].name = (pi_rand() % 3 == 0) ? 0 : longstr;   // NULL / unterminated
            src[i].account = "system";
        }
        uint32_t cap = pi_rand() % (MAXN + 1);
        static svc_info_t oc[MAXN + 2], orr[MAXN + 2];
        for (uint32_t i = 0; i < MAXN + 2; i++) { oc[i].pid = 0xBEEF; orr[i].pid = 0xBEEF; }
        int rc = svc_build_c(src, n, oc, cap);
        int rr = svc_build_rs(src, n, orr, cap);
        vecs++;
        if (rc != rr) mism++;
        else {
            for (int i = 0; i < rc; i++) {
                if (oc[i].running != orr[i].running || oc[i].pid != orr[i].pid) { mism++; break; }
                int bad = 0;
                for (int k = 0; k < PI_NAME_MAX; k++)
                    if (oc[i].name[k] != orr[i].name[k] ||
                        oc[i].account[k] != orr[i].account[k]) { bad = 1; break; }
                if (bad) { mism++; break; }
            }
        }
        if (oc[cap].pid != 0xBEEF || orr[cap].pid != 0xBEEF) canary++;
    }

    // Contract edges.
    { handle_info_t o[2];
      if (handles_build_c(0, 1, o, 2) != -1 || handles_build_rs(0, 1, o, 2) != -1) mism++;
      vecs++; }
    { handle_src_t s[2];
      if (handles_build_c(s, 1, 0, 2) != -1 || handles_build_rs(s, 1, 0, 2) != -1) mism++;
      vecs++; }

    kprintf("[RUST-DIFF] procinfo: %d vecs mism=%d canary=%d %s (LIVE=%s)\n",
            vecs, mism, canary, (mism || canary) ? "MISMATCH" : "MATCH",
#ifdef RUST_PROCINFO
            "rust"
#else
            "c"
#endif
    );
    kprintf("[RUST-DIFF] procinfo coverage: cap_overflow=%d cap0=%d null_str=%d "
            "unterminated=%d trunc=%d exact_fit=%d\n",
            cov_overflow, cov_cap0, cov_null, cov_unterm, cov_trunc, cov_exact);
}
