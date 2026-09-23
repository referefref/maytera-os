// netscan.c - Automatic LAN network-share discovery for the Files app.
// See netscan.h for the contract. Everything here runs on a pthread worker
// off the UI thread; the UI reads results through atomics + a mutex snapshot.
#include "netscan.h"
#include "../../libc/syscall.h"
#include "../../libc/pthread.h"
#include "../../libc/string.h"
#include "../../libc/stdio.h"

// ---- tuning ---------------------------------------------------------------
// The kernel TCP table is TCP_MAX_CONNECTIONS==64 sockets, shared process-wide,
// so keep a conservative in-flight window and leave headroom for the rest of
// the system. Live LAN hosts answer in well under a millisecond; only dark
// addresses run to the timeout, so the timeout dominates worst-case sweep time
// (~ hosts*ports/window * timeout). A /24 x 3 ports / 24 in-flight x 350 ms
// is ~11 s worst case for an all-dark subnet, and far less in practice, and it
// is cancellable throughout.
#define NS_INFLIGHT        24     // concurrent connect probes (of 64 total)
#define NS_PROBE_TIMEOUT   350    // ms a single connect may take before we give up
#define NS_POLL_MS         20     // sleep between poll passes (timed, never a spin)
#define NS_CLOSED_GRACE    80     // ms before a CLOSED readback counts as "refused"
#define NS_MAX_HOSTS       254    // a /24 host range; the sweep never exceeds this

// Ports probed per host. 445 and 139 = SMB; 2049 = NFS.
static const int NS_PORTS[] = { 445, 139, 2049 };
#define NS_NPORTS ((int)(sizeof(NS_PORTS)/sizeof(NS_PORTS[0])))

// ---- shared state ---------------------------------------------------------
static volatile int      g_state     = NS_IDLE;   // NS_* (atomic load/store)
static volatile int      g_cancel    = 0;
static volatile int      g_available = 0;
static volatile int      g_probed    = 0;         // probes completed (atomic)
static volatile int      g_total     = 0;         // probes queued in total
static pthread_t         g_thread    = 0;
static int               g_thread_live = 0;

static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;
static ns_server_t       g_servers[NS_MAX_SERVERS];
static volatile int      g_nservers = 0;          // atomic; only grows during a scan

// ---- helpers --------------------------------------------------------------
// Self-contained string copy (main.c's str_copy is file-static and not shared).
static void ns_strcpy(char *d, const char *s, int max) {
    int i = 0; while (s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0;
}

static void ip_to_str(unsigned int ip, char *out) {
    // host order a.b.c.d
    snprintf(out, 16, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

// Find-or-create the server record for `ip`. Caller holds g_lock. Returns NULL
// only if the (capped) table is full.
static ns_server_t *server_for(unsigned int ip) {
    int n = g_nservers;
    for (int i = 0; i < n; i++)
        if (g_servers[i].ip == ip) return &g_servers[i];
    if (n >= NS_MAX_SERVERS) return NULL;
    ns_server_t *s = &g_servers[n];
    memset(s, 0, sizeof(*s));
    s->ip = ip;
    ip_to_str(ip, s->ip_str);
    __atomic_store_n(&g_nservers, n + 1, __ATOMIC_RELEASE);
    return s;
}

// Enumerate SMB shares for a host already known to answer on 445/139. Runs on
// the worker thread; net_list_shares() self-pumps the network stack. Skips the
// IPC$ control pipe (not a browsable file share); keeps everything else.
static void enumerate_smb(ns_server_t *s) {
    static char buf[2048];   // worker-thread-only; the worker is single-threaded
    char ipstr[16];
    // Snapshot the ip string under the lock, then release it: net_list_shares
    // can take a moment and must not hold the UI's snapshot lock.
    pthread_mutex_lock(&g_lock);
    ns_strcpy(ipstr, s->ip_str, sizeof(ipstr));
    pthread_mutex_unlock(&g_lock);

    buf[0] = 0;
    int rc = net_list_shares(ipstr, buf, sizeof(buf));
    // rc < 0 = could not enumerate (guest denied / no srvsvc); leave nshares 0.

    pthread_mutex_lock(&g_lock);
    s->enum_done = 1;
    if (rc > 0) {
        int i = 0, cnt = 0;
        while (buf[i] && cnt < NS_MAX_SHARES) {
            char name[NS_NAME_MAX]; int o = 0;
            while (buf[i] && buf[i] != '\n' && o < NS_NAME_MAX - 1) name[o++] = buf[i++];
            name[o] = 0;
            if (buf[i] == '\n') i++;
            if (name[0] == 0) continue;
            // Skip the IPC$ control pipe; keep C$/ADMIN$ (mountable with creds).
            if (strcasecmp(name, "IPC$") == 0) continue;
            ns_strcpy(s->shares[cnt], name, NS_NAME_MAX);
            cnt++;
        }
        s->nshares = cnt;
    }
    pthread_mutex_unlock(&g_lock);
}

// Enumerate NFS exports for a host answering on 2049 (MOUNT EXPORT / showmount
// -e), via net_list_exports (SYS_NET_LIST_EXPORTS). Runs on the worker thread;
// the syscall self-pumps the net stack through the RPC layer and has a bounded
// timeout, so it cannot hang the worker. rc < 0 = could not enumerate (server
// denied showmount / no rpcbind): leave nexports 0 and the UI shows the host as
// a bare NFS server.
static void enumerate_nfs(ns_server_t *s) {
    static char buf[4096];   // worker-thread-only; the worker is single-threaded
    char ipstr[16];
    pthread_mutex_lock(&g_lock);
    ns_strcpy(ipstr, s->ip_str, sizeof(ipstr));
    pthread_mutex_unlock(&g_lock);

    buf[0] = 0;
    int rc = net_list_exports(ipstr, buf, sizeof(buf));

    pthread_mutex_lock(&g_lock);
    s->nfs_enum_done = 1;
    if (rc > 0) {
        int i = 0, cnt = 0;
        while (buf[i] && cnt < NS_MAX_EXPORTS) {
            char name[NS_EXPORT_MAX]; int o = 0;
            while (buf[i] && buf[i] != '\n' && o < NS_EXPORT_MAX - 1) name[o++] = buf[i++];
            name[o] = 0;
            if (buf[i] == '\n') i++;
            if (name[0] == 0) continue;
            ns_strcpy(s->exports[cnt], name, NS_EXPORT_MAX);
            cnt++;
        }
        s->nexports = cnt;
    }
    pthread_mutex_unlock(&g_lock);
}

// ---- probe pipeline -------------------------------------------------------
typedef struct {
    int          sock;    // -1 = free slot
    unsigned int ip;
    int          port;
    long         t0;      // uptime_ms when the connect was fired
    int          used;    // 0 = free
} probe_t;

static long now_ms(void) { return (long)uptime_ms(); }

// Record a positive result for (ip,port). Caller must NOT hold g_lock.
static void note_open(unsigned int ip, int port) {
    ns_server_t *found_smb = NULL;
    ns_server_t *found_nfs = NULL;
    pthread_mutex_lock(&g_lock);
    ns_server_t *s = server_for(ip);
    if (s) {
        if (port == 445 || port == 139) s->smb = 1;
        if (port == 2049)               s->nfs = 1;
        if (s->smb && !s->enum_done)     found_smb = s;   // enumerate outside the lock
        if (s->nfs && !s->nfs_enum_done) found_nfs = s;
    }
    pthread_mutex_unlock(&g_lock);
    if (found_smb && !g_cancel) enumerate_smb(found_smb);
    if (found_nfs && !g_cancel) enumerate_nfs(found_nfs);
}

static void *scan_thread(void *arg) {
    (void)arg;

    // 1) Decide the subnet to sweep from our own address + netmask.
    net_status_t nst;
    if (sys_net_status(&nst) != 0 || !nst.link_up || nst.ip == 0) {
        g_available = 0;
        __atomic_store_n(&g_state, NS_DONE, __ATOMIC_RELEASE);
        return NULL;
    }
    g_available = 1;

    unsigned int self = nst.ip;
    unsigned int mask = nst.netmask ? nst.netmask : 0xFFFFFF00u;
    // Force at least /24 granularity so the host range never exceeds 254 and a
    // large (e.g. /16) subnet does not blow up into a 65k-host sweep. Smaller
    // subnets (/25.../30) keep their real, smaller range.
    unsigned int emask = mask | 0xFFFFFF00u;
    unsigned int net   = self & emask;
    unsigned int bcast = net | (~emask);

    // 2) Build the work list of (ip,port) probes.
    // Count queued probes first so progress has a denominator.
    int hostcount = 0;
    for (unsigned int a = net + 1; a < bcast && hostcount < NS_MAX_HOSTS; a++) {
        if (a == self) continue;
        hostcount++;
    }
    __atomic_store_n(&g_total, hostcount * NS_NPORTS, __ATOMIC_RELEASE);
    __atomic_store_n(&g_probed, 0, __ATOMIC_RELEASE);

    // Iterator state over (host, port).
    unsigned int next_ip   = net + 1;
    int          next_port = 0;

    probe_t inflight[NS_INFLIGHT];
    for (int i = 0; i < NS_INFLIGHT; i++) { inflight[i].used = 0; inflight[i].sock = -1; }

    for (;;) {
        if (g_cancel) break;

        // Fill free slots from the work iterator.
        for (int i = 0; i < NS_INFLIGHT; i++) {
            if (inflight[i].used) continue;
            // Advance to the next valid (ip,port), skipping self and broadcast.
            while (next_ip < bcast) {
                if (next_ip == self) { next_ip++; next_port = 0; continue; }
                if ((int)(next_ip - (net + 1)) >= NS_MAX_HOSTS) { next_ip = bcast; break; }
                break;
            }
            if (next_ip >= bcast) break;   // no more work to enqueue

            unsigned int ip = next_ip;
            int port = NS_PORTS[next_port];
            // advance iterator
            next_port++;
            if (next_port >= NS_NPORTS) { next_port = 0; next_ip++; }

            int sk = tcp_socket();
            if (sk < 0) break;   // table momentarily full: try again next pass
            // tcp_connect fires the SYN (or the on-demand ARP) and returns; the
            // handshake is driven by polling tcp_get_state below (#549 async).
            tcp_connect(sk, ip, port);
            inflight[i].sock = sk;
            inflight[i].ip   = ip;
            inflight[i].port = port;
            inflight[i].t0   = now_ms();
            inflight[i].used = 1;
        }

        // Are we finished? No work left to enqueue AND nothing in flight.
        int any_active = 0;
        for (int i = 0; i < NS_INFLIGHT; i++) if (inflight[i].used) { any_active = 1; break; }
        int more_work = (next_ip < bcast);
        if (!any_active && !more_work) break;

        // Poll each in-flight probe.
        long now = now_ms();
        for (int i = 0; i < NS_INFLIGHT; i++) {
            if (!inflight[i].used) continue;
            int st = tcp_get_state(inflight[i].sock);
            int done = 0, open = 0;
            if (st == TCP_STATE_ESTABLISHED) { open = 1; done = 1; }
            else if (st == TCP_STATE_CLOSED && (now - inflight[i].t0) >= NS_CLOSED_GRACE) {
                // RST/refused (or the local side gave up): port is not open.
                done = 1;
            }
            else if ((now - inflight[i].t0) >= NS_PROBE_TIMEOUT) {
                done = 1;   // no answer: dark address or filtered port
            }
            if (done) {
                tcp_close(inflight[i].sock);
                unsigned int ip = inflight[i].ip; int port = inflight[i].port;
                inflight[i].used = 0; inflight[i].sock = -1;
                __atomic_fetch_add(&g_probed, 1, __ATOMIC_RELAXED);
                if (open) note_open(ip, port);
            }
        }

        if (g_cancel) break;
        sys_sleep(NS_POLL_MS);   // timed wait on the kernel timer, not a spin
    }

    // Close anything still open (cancel path).
    for (int i = 0; i < NS_INFLIGHT; i++)
        if (inflight[i].used && inflight[i].sock >= 0) tcp_close(inflight[i].sock);

    __atomic_store_n(&g_state, NS_DONE, __ATOMIC_RELEASE);
    return NULL;
}

// ---- public API -----------------------------------------------------------
void ns_cancel(void) {
    if (g_thread_live) {
        g_cancel = 1;
        pthread_join(g_thread, NULL);
        g_thread_live = 0;
    }
    g_cancel = 0;
    g_state = NS_IDLE;
}

void ns_start(void) {
    ns_cancel();
    pthread_mutex_lock(&g_lock);
    memset(g_servers, 0, sizeof(g_servers));
    __atomic_store_n(&g_nservers, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_lock);
    g_cancel = 0;
    g_available = 0;
    __atomic_store_n(&g_probed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_total,  0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_state, NS_SCANNING, __ATOMIC_RELEASE);

    pthread_attr_t at;
    pthread_attr_init(&at);
    at.stack_size = 64 * 1024;   // flat worker, no deep recursion
    int rc = pthread_create(&g_thread, &at, scan_thread, NULL);
    pthread_attr_destroy(&at);
    if (rc != 0) { g_state = NS_DONE; return; }
    g_thread_live = 1;
}

int ns_state(void)     { return __atomic_load_n(&g_state, __ATOMIC_ACQUIRE); }
int ns_available(void) { return g_available; }

int ns_progress(int *done, int *total) {
    if (done)  *done  = __atomic_load_n(&g_probed, __ATOMIC_ACQUIRE);
    if (total) *total = __atomic_load_n(&g_total,  __ATOMIC_ACQUIRE);
    return __atomic_load_n(&g_nservers, __ATOMIC_ACQUIRE);
}

int ns_server_count(void) { return __atomic_load_n(&g_nservers, __ATOMIC_ACQUIRE); }

int ns_get_server(int idx, ns_server_t *out) {
    if (!out) return -1;
    int rc = -1;
    pthread_mutex_lock(&g_lock);
    if (idx >= 0 && idx < g_nservers) { *out = g_servers[idx]; rc = 0; }
    pthread_mutex_unlock(&g_lock);
    return rc;
}
