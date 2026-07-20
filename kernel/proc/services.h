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
// marked autostart. The remote-control "svc" command (net/remote_ctrl.c)
// drives list/start/stop/enable/disable at runtime.

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

typedef struct {
    char     name[SVC_NAME_MAX];      // service identifier (e.g. "heartbeat")
    char     exec[SVC_EXEC_MAX];      // ELF path on disk (e.g. "/APPS/SVCHB")
    char     account[SVC_ACCOUNT_MAX];// service account name (informational)
    uint32_t uid;                     // uid the service process runs under
    uint32_t perms;                   // SVC_PERM_* capability mask
    uint8_t  autostart;               // start automatically at boot
    uint8_t  enabled;                 // may be started at all
    int      pid;                     // running pid, or 0 when stopped
} service_t;

// Build the service registry: register the built-in default(s) then merge
// any services declared in /CONFIG/SERVICES.CFG. Safe to call once at boot.
void svc_init(void);

// Start every enabled service whose autostart flag is set. Call after the
// filesystem is mounted (services load their ELF from disk).
void svc_autostart(void);

// Lifecycle control. Each returns 0 on success, negative on error.
int  svc_start(const char *name);
int  svc_stop(const char *name);
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
