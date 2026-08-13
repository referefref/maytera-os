// services.h - #95 Background services subsystem for MayteraOS
//
// A small service manager that runs user-mode ELF programs as long-lived
// background services. Each service has:
//   - a service account (a name + uid the process runs under), and
//   - a capability mask (SVC_PERM_*) that sandboxes which privileged
//     syscalls the service may use.
//
// Services are defined in /CONFIG/SERVICES.CFG (one per line); a built-in
// default is always registered so the subsystem is functional even with no
// config file present. At boot svc_autostart() starts every enabled service
// marked autostart.
//
// (#785) This comment used to say the remote-control "svc" command in
// net/remote_ctrl.c drove list/start/stop/enable/disable at runtime. That file
// was DELETED in 2c69154 (#566, RC-2323 removed entirely), so for two tickets
// this header described a runtime control surface that did not exist, and it
// was the ONLY place enable/disable was ever claimed to be reachable.
// svc_enable() in fact had ZERO callers. Runtime control now goes through
// SYS_SVC_CONTROL (kernel/proc/procinfo.c), which reaches start/stop/enable/
// disable, and is root-gated.
//
// TWO KINDS OF SERVICE (#785):
//   - a USERLAND service is an ELF at exec[], spawned under a service account
//     with a capability mask. This is the original and still the default.
//   - a BUILT-IN service is compiled into the kernel and driven by function
//     pointers (see svc_register_builtin). sshd is one: it is an in-kernel
//     listener, so there is no ELF to spawn, but an operator still needs the
//     same start/stop/enable/disable verbs for it. Adding it here rather than
//     giving sshd a private on/off mechanism is the shared-primitive rule:
//     one service subsystem, one set of verbs, one place to look.

#ifndef MAYTERA_SERVICES_H
#define MAYTERA_SERVICES_H

#include "../types.h"

// ---- Capability bits (process_t.svc_perms) ----
#define SVC_PERM_NET      (1u << 0)   // network syscalls
#define SVC_PERM_FSWRITE  (1u << 1)   // open files for writing / create
#define SVC_PERM_SPAWN    (1u << 2)   // spawn child processes
#define SVC_PERM_INPUT    (1u << 3)   // read input / window events
#define SVC_PERM_SELFUPDATE (1u << 4) // #492: install a signed kernel via SYS_KERNEL_SELFUPDATE
#define SVC_PERM_ALL      0xFFFFFFFFu

#define MAX_SERVICES        16
#define SVC_NAME_MAX        24
#define SVC_EXEC_MAX        64
#define SVC_ACCOUNT_MAX     24

// ---- built-in (in-kernel) service hooks (#785) ----
// A built-in service has no ELF and no pid. It answers these instead.
typedef int (*svc_start_fn)(void);    // 0 (or >0) on success, negative on error
typedef int (*svc_stop_fn)(void);     // 0 on success
typedef int (*svc_running_fn)(void);  // 1 if currently listening/running
// Persist the enabled flag to the service's OWN config file. Return 0 on
// success, negative on failure. A service with NO persist hook is RAM only,
// and svc_enable() says so by returning an error rather than reporting a
// success that will not survive a reboot.
typedef int (*svc_persist_fn)(int enable);

typedef struct {
    char     name[SVC_NAME_MAX];      // service identifier (e.g. "heartbeat")
    char     exec[SVC_EXEC_MAX];      // ELF path on disk (e.g. "/APPS/SVCHB")
    char     account[SVC_ACCOUNT_MAX];// service account name (informational)
    uint32_t uid;                     // uid the service process runs under
    uint32_t perms;                   // SVC_PERM_* capability mask
    uint8_t  autostart;               // start automatically at boot
    uint8_t  enabled;                 // may be started at all
    uint8_t  builtin;                 // 1 = in-kernel, driven by the hooks below
    uint8_t  _pad;
    int      pid;                     // running pid, or 0 when stopped/built-in
    svc_start_fn   b_start;           // built-in only
    svc_stop_fn    b_stop;
    svc_running_fn b_running;
    svc_persist_fn b_persist;
} service_t;

// Build the service registry: register the built-in default(s) then merge
// any services declared in /CONFIG/SERVICES.CFG. Safe to call once at boot.
void svc_init(void);

// Start every enabled service whose autostart flag is set. Call after the
// filesystem is mounted (services load their ELF from disk).
void svc_autostart(void);

// Register an in-kernel service (#785). Call from svc_init() only, after the
// registry has been reset. `running` and `persist` may be NULL; a NULL persist
// makes svc_enable() refuse rather than pretend the change is durable.
// Returns 0 on success, negative if the registry is full.
int  svc_register_builtin(const char *name, const char *account, uint32_t uid,
                          uint32_t perms, int autostart, int enabled,
                          svc_start_fn start, svc_stop_fn stop,
                          svc_running_fn running, svc_persist_fn persist);

// Lifecycle control. Each returns 0 on success, negative on error.
int  svc_start(const char *name);
int  svc_stop(const char *name);
// Enable or disable, AND persist the change. Returns 0 only when the new state
// is durable; negative when it is not (notably -7 when the service has no
// persist hook and -8 when writing its config failed). A caller that treats a
// negative as success is claiming a reboot-surviving change it did not make.
int  svc_enable(const char *name, int enable);

// Introspection for the remote-control "svc list" command.
int        svc_count(void);
service_t *svc_at(int index);
service_t *svc_find(const char *name);

// Render the bitmask as a short human string ("net,fs,spawn") into buf.
void svc_perms_str(uint32_t perms, char *buf, int buflen);

// Is the given service's process still alive? Updates ->pid to 0 if not.
int  svc_is_running(service_t *svc);

#endif // MAYTERA_SERVICES_H
