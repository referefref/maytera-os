// procinfo.h - #487/#349 Ring-3 process-introspection surface.
//
// WHY THIS EXISTS: the Task Manager the user actually opens is the USERLAND app
// (/apps/taskmgr; gui/desktop.c launches it and only falls back to the kernel
// app if it fails to load). That app could only ever call sys_proc_list, so it
// could only ever draw a process list: the kernel knew about open handles,
// socket ownership, services and scheduled tasks, but had no way to tell Ring 3
// about any of it. These syscalls are that missing channel.
//
// Already exposed before this change, do NOT re-add:
//   SYS_PROC_LIST 238, SYS_KILL 80, SYS_SETPRIORITY 244, SYS_GET_CPU_USAGE 193,
//   SYS_GET_MEM_INFO 194, SYS_GET_CPU_PER_CORE 259, SYS_CRON_* 276-279.
//
// SECURITY NOTE (deliberate, and a departure from this file's neighbours):
// every syscall backed by this header VALIDATES its user pointer via
// validate_user_ptr() before writing a single byte. The rest of the syscall
// surface does NOT: validate_user_ptr() is defined in security/validate.c and
// has ZERO other callers in the tree, so e.g. SYS_PROC_LIST hands arg1 straight
// to proc_snapshot(), which happily writes process records into any kernel
// address a Ring-3 caller names. That is a pre-existing systemic hole affecting
// ~300 syscalls and is far outside #487's scope to fix, but new syscalls will
// not add to it. Reported for the security owner.
#ifndef PROCINFO_H
#define PROCINFO_H

#include "../types.h"

#define PI_PATH_MAX   96    // bytes of path carried per handle row (incl. NUL)
// 32 is a deliberate superset of SVC_NAME_MAX / SVC_ACCOUNT_MAX (both 24) and
// of process_t.name (32). The Rust builder bounds every copy to the DESTINATION
// field regardless, so a source field growing later truncates rather than
// overflowing; this only has to be >= what we want to display.
#define PI_NAME_MAX   32
// Rows the connection snapshot can carry. Matches TCP_MAX_CONNECTIONS (64) so
// the whole live table can be reported; also bounds the on-stack gather array.
#define TM_PI_MAX_CONNS 64

// Handle kinds, so a UI can group/iconify handles the way Process Explorer does.
#define PI_KIND_FILE    0
#define PI_KIND_DEV     1
#define PI_KIND_PIPE    2
#define PI_KIND_SOCKET  3
#define PI_KIND_UNKNOWN 4

// One row of a process's open-handle table (SYS_PROC_HANDLES).
typedef struct {
    int32_t  fd;
    int32_t  flags;              // O_* as opened
    uint32_t kind;               // PI_KIND_*
    uint32_t _pad;
    char     path[PI_PATH_MAX];  // always NUL-terminated; "" = anonymous
} handle_info_t;
_Static_assert(sizeof(handle_info_t) == 112, "handle_info_t sizeof lock (Rust HandleInfo)");

// What the C gatherer hands the Rust builder: the raw, unbounded pointers it
// pulled out of the fd table. The Rust side does every copy, bounded.
typedef struct {
    int32_t     fd;
    int32_t     flags;
    uint32_t    kind;
    uint32_t    _pad;
    const char *path;            // may be NULL or unterminated; NOT trusted
} handle_src_t;
_Static_assert(sizeof(handle_src_t) == 24, "handle_src_t sizeof lock (Rust HandleSrc)");

// One row of the service table (SYS_SVC_LIST). #95 subsystem.
typedef struct {
    uint32_t running;
    uint32_t autostart;
    uint32_t perms;
    uint32_t pid;                // 0 if not running
    char     name[PI_NAME_MAX];
    char     account[PI_NAME_MAX];
} svc_info_t;
_Static_assert(sizeof(svc_info_t) == 80, "svc_info_t sizeof lock (Rust SvcInfo)");

typedef struct {
    uint32_t    running;
    uint32_t    autostart;
    uint32_t    perms;
    uint32_t    pid;
    const char *name;            // NOT trusted to be terminated
    const char *account;
} svc_src_t;
_Static_assert(sizeof(svc_src_t) == 32, "svc_src_t sizeof lock (Rust SvcSrc)");

// Per-process detail (SYS_PROC_DETAIL): everything the Details pane needs that
// proc_info_t does not carry. Memory fields come from proc_mem_account_rs.
typedef struct {
    uint32_t pid;
    uint32_t ppid;
    uint32_t working_set_kb;
    uint32_t private_kb;
    uint32_t virt_kb;
    uint32_t heap_kb;
    uint32_t threads;
    uint32_t handles;
    uint32_t uid;
    uint32_t gid;
    uint32_t priority;
    uint32_t privilege;          // 0 kernel / 3 user
    uint32_t state;
    uint32_t vma_count;
    uint32_t mem_flags;          // PROC_MEM_F_*
    uint32_t is_service;
    uint64_t cpu_ticks;
    uint64_t cr3;
    char     name[PI_NAME_MAX];
} proc_detail_t;
_Static_assert(sizeof(proc_detail_t) == 112, "proc_detail_t sizeof lock (userland twin)");

// ---- Rust builders (rustkern.rs) + C reference twins -----------------------
// Both fill at most `cap` rows from `n` sources and return rows written, or -1
// on a bad argument. Every string copy is bounded and NUL-terminated.
int handles_build_rs(const handle_src_t *src, uint32_t n, handle_info_t *out, uint32_t cap);
int handles_build_c(const handle_src_t *src, uint32_t n, handle_info_t *out, uint32_t cap);
int svc_build_rs(const svc_src_t *src, uint32_t n, svc_info_t *out, uint32_t cap);
int svc_build_c(const svc_src_t *src, uint32_t n, svc_info_t *out, uint32_t cap);

#ifdef RUST_PROCINFO
#define handles_build(s, n, o, c) handles_build_rs((s), (n), (o), (c))
#define svc_build(s, n, o, c)     svc_build_rs((s), (n), (o), (c))
#else
#define handles_build(s, n, o, c) handles_build_c((s), (n), (o), (c))
#define svc_build(s, n, o, c)     svc_build_c((s), (n), (o), (c))
#endif

// ---- syscall backends ------------------------------------------------------
int64_t sys_proc_handles(uint32_t pid, void *ubuf, int max);
int64_t sys_net_conns(uint32_t pid, void *ubuf, int max);
int64_t sys_svc_list(void *ubuf, int max);
int64_t sys_svc_control(const char *uname, int action);
int64_t sys_proc_detail(uint32_t pid, void *uout);

#define PI_SVC_STOP   0
#define PI_SVC_START  1

// Sentinel for sys_net_conns: every connection regardless of owner. Cannot be a
// real pid (0 is a legitimate query meaning "kernel-internal / unowned").
#define PI_PID_ALL 0xFFFFFFFFu

// #404 boot-time [RUST-DIFF] differential for the procinfo builders.
void procinfo_selftest(void);

#endif // PROCINFO_H
