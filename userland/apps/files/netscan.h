// netscan.h - Automatic LAN network-share discovery helper for the Files app.
//
// Sweeps the local IPv4 subnet (derived from the guest's own address+netmask,
// via SYS_NET_STATUS) for hosts that answer on the SMB (445/139) and/or NFS
// (2049) ports, using the EXISTING non-blocking TCP connect trio
// (tcp_socket / tcp_connect / tcp_get_state / tcp_close) so the Files UI thread
// never blocks on network I/O (#420/#549/#426). All work runs on a pthread
// worker, exactly like the Disk Usage app's scan thread; the UI reads results
// through an atomic count plus a mutex-guarded snapshot copy. The sweep is
// bounded (never more than a /24 of host addresses), cancellable and
// re-runnable.
//
// For each SMB host the shares are enumerated with the net_list_shares syscall
// (SYS_NET_LIST_SHARES). For each NFS host the exports are enumerated with the
// net_list_exports syscall (SYS_NET_LIST_EXPORTS, added by #317 nfsbrowse),
// which reaches the kernel's nfs_list_exports() over the MOUNT EXPORT
// (showmount -e) protocol. A server that denies showmount is surfaced as a bare
// NFS server. Nothing is fabricated.
#ifndef FILES_NETSCAN_H
#define FILES_NETSCAN_H

#define NS_MAX_SERVERS 64   // discovered hosts kept (subnet is capped to a /24)
#define NS_MAX_SHARES  24   // enumerated SMB shares kept per host
#define NS_NAME_MAX    40
#define NS_MAX_EXPORTS 24   // enumerated NFS exports kept per host
#define NS_EXPORT_MAX  128  // NFS exports are server-side absolute paths

typedef struct {
    unsigned int  ip;                       // host order, (a<<24)|(b<<16)|(c<<8)|d
    char          ip_str[16];               // "a.b.c.d"
    unsigned char smb;                      // 1 = 445 or 139 answered
    unsigned char nfs;                      // 1 = 2049 answered
    unsigned char enum_done;                // SMB share enumeration attempted
    int           nshares;                  // shares in shares[]
    char          shares[NS_MAX_SHARES][NS_NAME_MAX];
    unsigned char nfs_enum_done;            // NFS export enumeration attempted
    int           nexports;                 // exports in exports[]
    char          exports[NS_MAX_EXPORTS][NS_EXPORT_MAX];
} ns_server_t;

// Scan states (ns_state()).
enum { NS_IDLE = 0, NS_SCANNING = 1, NS_DONE = 2 };

// Begin (or restart) an async subnet sweep. Cancels+joins any prior sweep
// first. If networking is unavailable (no carrier / no IP) it does no work and
// leaves the scan in NS_DONE with ns_available()==0. Safe to call from the UI
// thread; returns immediately.
void ns_start(void);

// Request cancellation of a running sweep and join the worker. Idempotent.
// Frees the worker's in-flight sockets. Call before leaving the Network view
// and at app exit.
void ns_cancel(void);

// Current scan state (NS_IDLE / NS_SCANNING / NS_DONE), lock-free.
int  ns_state(void);

// 1 if the last ns_start() found usable networking (carrier + IP); else 0.
int  ns_available(void);

// Progress: writes probes-completed and probes-total (may be 0 before start).
// Returns the number of servers found so far.
int  ns_progress(int *done, int *total);

// Snapshot count of discovered servers (atomic).
int  ns_server_count(void);

// Copy discovered server #idx into *out under the results lock. Returns 0 on
// success, -1 if idx is out of range. The copy is stable for the caller.
int  ns_get_server(int idx, ns_server_t *out);

#endif // FILES_NETSCAN_H
