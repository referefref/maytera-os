// https.c - HTTPS client for MayteraOS
// Combines TCP sockets with TLS for secure HTTP

#include "https.h"
#include "tcp.h"
#include "dns.h"
#include "ip.h"
#include "tls/tls.h"
#include "http2.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../cpu/mono.h"   // #499: sched_now_ms() - THE shared real-elapsed-ms clock
#include "../cpu/dlprof.h"
#include "fs/bootlog.h"   // #742: the owning header, NOT a private extern
#include "http_progress.h"   // #25: real per-fetch progress for the browser chrome

// #615: per-phase fetch profile. Every https_get/https_get_hdr/https_post
// records where its wall clock went (dns / arp / tcp connect / tls handshake /
// request / time-to-first-byte / body) and prints ONE serial line. Serial only
// (#597: file-write tracing corrupts ext2).
int g_https_prof = 1;
uint64_t g_pf_dns = 0, g_pf_arp = 0, g_pf_tcp = 0, g_pf_tls = 0;

// External declarations
extern void net_poll(void);
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;            // actual PIT frequency (250Hz)
extern void proc_sleep(uint32_t ms);   // yield the CPU during network waits
extern int g_tls_dbg;
extern void net_lock(void);   // #297: serialize NIC + TCP-table access
extern void net_unlock(void);

// HTTPS connection structure
struct https_conn {
    int tcp_socket;
    tls_context_t *tls;
    char hostname[256];
    uint16_t port;
    uint16_t pool_flags;   /* #616: bit0 = opened with the ALPN forced to http/1.1 */
    int connected;
};

// TCP send callback for TLS
static int https_tcp_send(void *user_data, const void *data, size_t length) {
    https_conn_t *conn = (https_conn_t *)user_data;

    size_t sent = 0;
    while (sent < length) {
        net_lock();
        int ret = tcp_send(conn->tcp_socket, (const uint8_t *)data + sent, length - sent);
        net_unlock();
        if (ret < 0) {
            if (ret == TCP_ERR_WOULD_BLOCK) {
                net_poll();
                tcp_timer();
                proc_sleep(1);   // yield while the TX buffer drains
                continue;
            }
            return ret;
        }
        sent += ret;
        net_poll();
        tcp_timer();
    }

    return (int)sent;
}

// TCP receive callback for TLS
static int https_tcp_recv(void *user_data, void *buffer, size_t length) {
    https_conn_t *conn = (https_conn_t *)user_data;

    // Poll for data with a real timer-based timeout (~10 seconds)
    uint64_t start_ms = sched_now_ms();          // #499: REAL elapsed ms
    uint64_t timeout_ms = 5000;                  // ~5 seconds

    while (sched_now_ms() - start_ms < timeout_ms) {
        g_dp_tls_iter++;
        net_lock();
        // #608: tcp_recv() takes uint32_t; clamp rather than let a size_t be
        // narrowed silently (a request that truncates to 0 stalls the loop).
        size_t _want = length;
        if (_want > 0xFFFFFFFFu) _want = 0xFFFFFFFFu;
        int ret = tcp_recv(conn->tcp_socket, buffer, (uint32_t)_want);
        net_unlock();
        if (ret > 0) {
            g_dp_tls_bytes += (uint64_t)ret;
            return ret;                 // deliver buffered data (incl. in CLOSE_WAIT)
        }
        if (ret == TCP_ERR_CLOSED) {
            return TLS_ERR_CLOSED;      // peer closed AND the RX buffer is drained
        }
        if (ret < 0 && ret != TCP_ERR_WOULD_BLOCK) {
            return ret;
        }

        net_poll();
        tcp_timer();
        g_dp_tls_sleep++;
        proc_sleep(2);   // yield so the OS stays responsive during the fetch

        // Do NOT bail out on CLOSE_WAIT here. A server honoring "Connection: close"
        // commonly sends its (often small) response and the FIN back-to-back, so a
        // single net_poll() can buffer the response AND move the socket to
        // CLOSE_WAIT at once. tcp_recv() still returns that buffered data while in
        // CLOSE_WAIT and only reports TCP_ERR_CLOSED once it is fully drained, so we
        // loop back and read it instead of discarding the whole response. This was
        // why small-response sites (example.com, duckduckgo) came up blank while
        // large-response sites (google) worked.
    }

    return TLS_ERR_WOULD_BLOCK;
}

// Initialize HTTPS subsystem
void https_init(void) {
    // #fix-tls-certverify: load the CA trust store so tls_set_verify(1, 0)
    // below has trust anchors to check chains against. Previously nothing
    // ever called cert_store_init()/cert_add_trusted() at all, so even if a
    // caller had asked for verification the store would have been empty.
    extern int cert_store_load_default_bundle(void);
    cert_store_load_default_bundle();
    kprintf("[HTTPS] HTTPS client initialized\n");
}

// #333: pre-resolve ARP for an on-link destination before connecting. Only the
// gateway MAC is cached at boot, so the very first SYN to a never-contacted
// same-subnet host is dropped (no MAC), and the TCP layer gives up after ~5 SYN
// retransmits (~1.2s) before ARP completes. Off-subnet hosts route via the
// gateway whose MAC is already cached, so we only warm the next hop when the
// destination is on our subnet. Mirrors net/smb.c (#317) and net/ipp.c (#318).
static void https_warm_arp(uint32_t host_ip) {
    extern int arp_resolve(uint32_t ip, uint8_t *mac);
    extern uint32_t ip_get_address(void);
    extern uint32_t ip_get_netmask(void);

    uint32_t our_ip = ip_get_address();
    uint32_t nm = ip_get_netmask();
    if (!our_ip || !nm) return;                       // net not configured yet
    // Only warm the destination itself when it is on-link; otherwise the next
    // hop is the gateway (already cached at boot).
    if ((host_ip & nm) != (our_ip & nm)) return;

    uint8_t mac[6];
    if (arp_resolve(host_ip, mac)) return;            // already cached
    uint64_t start_ms = sched_now_ms();              // #499: REAL elapsed ms
    while (!arp_resolve(host_ip, mac)) {
        net_poll();
        tcp_timer();
        if (sched_now_ms() - start_ms > 4000) {       // ~4s ARP window
            kprintf("[HTTPS] ARP warm-up timed out for on-link host\n");
            return;
        }
        proc_sleep(2);
    }
    kprintf("[HTTPS] ARP resolved on-link host (%02x:%02x:%02x:%02x:%02x:%02x)\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Create HTTPS connection
https_conn_t *https_connect(const char *hostname, uint16_t port) {
    // #374: never start a TLS/TCP connect when the network is down; a dead link
    // otherwise burns the full connect-retry timeout and freezes any UI that
    // waits on the fetch. Return immediately so callers get "no network" fast.
    // #549: net_wire_usable(), not net_is_up() - the NET_FAULTY policy is
    // enforced once at the fetch gate, which admits one paced re-probe. Testing
    // net_is_up() here vetoed that probe.
    extern int net_wire_usable(void);
    if (!net_wire_usable()) { kprintf("[HTTPS] no carrier/address; skipping connect to %s\n", hostname); return NULL; }
    https_conn_t *conn = kzalloc(sizeof(https_conn_t));
    if (!conn) return NULL;

    strncpy(conn->hostname, hostname, sizeof(conn->hostname) - 1);
    conn->port = port;
    conn->tcp_socket = -1;

    kprintf("[HTTPS] Connecting to %s:%d\n", hostname, port);

    net_progress_phase(HTTP_PHASE_RESOLVING);   // #25

    // Resolve hostname
    uint32_t host_ip;
    uint64_t _t_dns0 = sched_now_ms();
    int ret = dns_resolve(hostname, &host_ip);
    g_pf_dns = sched_now_ms() - _t_dns0;
    if (ret != 0) {
        kprintf("[HTTPS] DNS resolution failed: %d\n", ret);
        kfree(conn);
        return NULL;
    }

    // #333: pre-resolve ARP for on-link destinations so the first SYN is not
    // dropped (only the gateway MAC is cached at boot). Fixes same-subnet HTTP/
    // HTTPS fetches to a never-contacted LAN host (e.g. album-art on a LAN box).
    { uint64_t _t = sched_now_ms(); https_warm_arp(host_ip); g_pf_arp = sched_now_ms() - _t; }

    net_progress_phase(HTTP_PHASE_CONNECTING);   // #25

    kprintf("[HTTPS] Connecting TCP to ");
    ip_print(host_ip);
    kprintf(":%d\n", port);

    // #333: retry the connect across the ARP-resolution window. Even with the
    // warm-up above, the first SYN can race ARP on a cold cache; the TCP layer
    // gives up after ~5 retransmits (~1.2s), so try a few times before failing.
    uint64_t timeout_ms = 3000;   // #374 ~3s/attempt (was 5s); #499 REAL ms
    int connected = 0;
    uint64_t _t_tcp0 = sched_now_ms();
    for (int attempt = 0; attempt < 2 && !connected; attempt++) {  // #374 2 attempts (was 3): bound worst-case connect to ~6s
        // Create TCP socket
        conn->tcp_socket = tcp_socket();
        if (conn->tcp_socket < 0) {
            kprintf("[HTTPS] Failed to create socket\n");
            kfree(conn);
            return NULL;
        }

        ret = tcp_connect(conn->tcp_socket, host_ip, port);
        if (ret < 0 && ret != TCP_ERR_IN_PROGRESS) {
            kprintf("[HTTPS] TCP connect failed: %d (attempt %d)\n", ret, attempt + 1);
            tcp_close(conn->tcp_socket);
            conn->tcp_socket = -1;
            net_poll(); tcp_timer(); proc_sleep(200);
            continue;
        }

        // Wait for TCP connection
        uint64_t start_time_ms = sched_now_ms();
        int dead = 0;
        while (!tcp_is_connected(conn->tcp_socket)) {
            net_poll();
            tcp_timer();
            proc_sleep(2);   // yield during the connect wait

            if (sched_now_ms() - start_time_ms > timeout_ms) {
                kprintf("[HTTPS] TCP connection timeout (attempt %d)\n", attempt + 1);
                dead = 1;
                break;
            }
            tcp_state_t state = tcp_get_state(conn->tcp_socket);
            if (state == TCP_STATE_CLOSED) {
                kprintf("[HTTPS] TCP connection reset/refused (attempt %d)\n", attempt + 1);
                dead = 1;
                break;
            }
        }
        if (dead) {
            tcp_close(conn->tcp_socket);
            conn->tcp_socket = -1;
            net_poll(); tcp_timer(); proc_sleep(200);
            continue;
        }
        connected = 1;
    }
    if (!connected) {
        kprintf("[HTTPS] TCP connect failed after retries\n");
        kfree(conn);
        return NULL;
    }

    g_pf_tcp = sched_now_ms() - _t_tcp0;
    kprintf("[HTTPS] TCP connected, starting TLS handshake\n");
    net_progress_phase(HTTP_PHASE_TLS);   // #25

    // Create TLS context
    conn->tls = tls_create();
    if (!conn->tls) {
        kprintf("[HTTPS] Failed to create TLS context\n");
        tcp_close(conn->tcp_socket);
        kfree(conn);
        return NULL;
    }

    // Configure TLS
    tls_set_io(conn->tls, https_tcp_send, https_tcp_recv, conn);
    tls_set_hostname(conn->tls, hostname);
    // #fix-tls-certverify: was tls_set_verify(conn->tls, 0, 1) - "don't
    // verify for now". Now verifies the full chain to a trusted root, the
    // notBefore/notAfter validity window (real RTC clock), and the hostname
    // against the leaf's CN/SAN. allow_self_signed=0: a self-signed leaf is
    // only accepted if it is itself in the trust store (cert_verify_chain's
    // existing "self-signed and trusted" path), never unconditionally.
    tls_set_verify(conn->tls, 1, 0);

    // Perform TLS handshake
    uint64_t _t_tls0 = sched_now_ms();
    ret = tls_connect(conn->tls);
    g_pf_tls = sched_now_ms() - _t_tls0;
    if (ret < 0) {
        kprintf("[HTTPS] TLS handshake failed: %s\n", tls_strerror(ret));
        tls_free(conn->tls);
        tcp_close(conn->tcp_socket);
        kfree(conn);
        return NULL;
    }

    kprintf("[HTTPS] TLS handshake complete\n");
    conn->connected = 1;

    return conn;
}

// Send data
int https_send(https_conn_t *conn, const void *data, size_t length) {
    if (!conn || !conn->connected) return HTTPS_ERR_CLOSED;
    return tls_send(conn->tls, data, length);
}

// Receive data
int https_recv(https_conn_t *conn, void *buffer, size_t length) {
    if (!conn || !conn->connected) return HTTPS_ERR_CLOSED;
    return tls_recv(conn->tls, buffer, length);
}

// Close connection
void https_close(https_conn_t *conn) {
    if (!conn) return;

    if (conn->tls) {
        tls_close(conn->tls);
        tls_free(conn->tls);
    }

    if (conn->tcp_socket >= 0) {
        tcp_close(conn->tcp_socket);
    }

    kfree(conn);
}

// Check if connected
int https_is_connected(https_conn_t *conn) {
    return conn && conn->connected && tls_is_connected(conn->tls);
}

/* #616: tiny case-insensitive helpers for the C reuse-policy twin. */
static int https_lc(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }
static int https_ci8(const uint8_t *p, const char *lit) {
    for (int i = 0; i < 8; i++) if (https_lc(p[i]) != https_lc((unsigned char)lit[i])) return 0;
    return 1;
}
static int https_ci_eq(const uint8_t *p, uint32_t n, const char *lit) {
    uint32_t i = 0;
    for (; i < n && lit[i]; i++) if (https_lc(p[i]) != https_lc((unsigned char)lit[i])) return 0;
    return (i == n && !lit[i]);
}
static int https_has_token(const uint8_t *v, uint32_t n, const char *tok) {
    uint32_t i = 0;
    while (i < n) {
        while (i < n && (v[i] == ',' || v[i] == ' ' || v[i] == '\t')) i++;
        uint32_t st = i;
        while (i < n && v[i] != ',' && v[i] != ' ' && v[i] != '\t') i++;
        if (i > st && https_ci_eq(v + st, i - st, tok)) return 1;
    }
    return 0;
}

// ===========================================================================
// #616: TLS CONNECTION REUSE (live-connection pool)
//
// MEASURED PROBLEM (#615): every https_get/https_get_hdr opened a fresh TCP
// connection AND ran a fresh TLS handshake. On the App Store's 103 MB package
// download (396 Range GETs) the handshake was 414 ms of each 550 ms chunk =
// 75%, about 2.7 minutes of pure handshake.
//
// The state machine (slot table, LRU eviction, idle expiry) and the
// "may this connection carry another request?" response-header policy live in
// rustkern/tlspool.rs. See that file for why they are Rust. The TCP/TLS calls
// stay here in C: they are the existing net stack, not new logic.
//
// SECURITY (#232/#510): this is LIVE-CONNECTION reuse only. No session
// tickets, no session-ID resumption, no abbreviated handshake. A pooled
// connection is one whose FULL handshake already passed certificate-chain,
// validity-window and hostname verification in https_connect(); reuse inherits
// a verified peer identity because it IS the same connection. A hit requires an
// exact (host, port, alpn-mode) match, so a connection to one host can never
// serve a request for another. Nothing here weakens or bypasses verification.
// ===========================================================================
#define HTTPS_POOL_SLOTS   4
#define HTTPS_POOL_IDLE_MS 20000

typedef struct {
    uint64_t conn;            /* opaque https_conn_t*; 0 = free */
    uint64_t last_used_ms;
    uint16_t port;
    uint16_t flags;
    uint16_t hostlen;
    uint16_t pad;
    uint8_t  host[128];
} https_pool_slot_t;
_Static_assert(sizeof(https_pool_slot_t) == 152,
               "https_pool_slot_t must match rustkern/tlspool.rs TlsPoolSlot");

extern int tlspool_acquire_rs(https_pool_slot_t *, uint32_t, const uint8_t *, uint32_t,
                              uint16_t, uint16_t, uint64_t, uint64_t, uint64_t *);
extern int tlspool_reap_rs(https_pool_slot_t *, uint32_t, uint64_t, uint64_t, uint64_t *);
extern int tlspool_store_rs(https_pool_slot_t *, uint32_t, const uint8_t *, uint32_t,
                            uint16_t, uint16_t, uint64_t, uint64_t, uint64_t *);
extern int tlspool_drop_rs(https_pool_slot_t *, uint32_t, uint64_t);
extern int tlspool_count_rs(https_pool_slot_t *, uint32_t);
extern int http_reuse_ok_rs(const uint8_t *, uint32_t);

static https_pool_slot_t g_https_pool[HTTPS_POOL_SLOTS];
int g_https_reuse = 1;               /* master switch; set 0 to roll back to per-request handshakes */
uint32_t g_https_pool_hit = 0, g_https_pool_miss = 0;

static uint32_t https_strlen32(const char *s) { uint32_t n = 0; if (s) while (s[n]) n++; return n; }

/* C reference twin of the Rust response-header reuse policy (strangler
   pattern: kept as the rollback arm and as the [RUST-DIFF] reference). */
static int http_reuse_ok_c(const uint8_t *hdr, uint32_t len) {
    if (!hdr || len == 0 || len > (1u << 20)) return 0;
    uint32_t eol = 0;
    while (eol < len && hdr[eol] != '\n') eol++;
    uint32_t ll = eol;
    if (ll > 0 && hdr[ll - 1] == '\r') ll--;
    int http11;
    if (ll >= 8 && https_ci8(hdr, "HTTP/1.1")) http11 = 1;
    else if (ll >= 8 && https_ci8(hdr, "HTTP/1.0")) http11 = 0;
    else return 0;
    int persistent = http11;
    uint32_t pos = (eol < len) ? eol + 1 : len;
    while (pos < len) {
        uint32_t e = pos;
        while (e < len && hdr[e] != '\n') e++;
        uint32_t lend = e;
        if (lend > pos && hdr[lend - 1] == '\r') lend--;
        uint32_t next = (e < len) ? e + 1 : len;
        if (lend == pos) break;                       /* blank line: end of block */
        uint32_t c = pos;
        while (c < lend && hdr[c] != ':') c++;
        if (c >= lend) { pos = next; continue; }
        uint32_t nlen = c - pos;
        int is_conn = (https_ci_eq(hdr + pos, nlen, "connection") ||
                       https_ci_eq(hdr + pos, nlen, "proxy-connection"));
        if (is_conn) {
            uint32_t v = c + 1;
            while (v < lend && (hdr[v] == ' ' || hdr[v] == '\t')) v++;
            if (https_has_token(hdr + v, lend - v, "close")) persistent = 0;
            else if (https_has_token(hdr + v, lend - v, "keep-alive")) persistent = 1;
        }
        pos = next;
    }
    return persistent ? 1 : 0;
}

static int https_reuse_ok(const uint8_t *hdr, uint32_t len) {
#ifdef RUST_TLS_POOL
    return http_reuse_ok_rs(hdr, len);
#else
    return http_reuse_ok_c(hdr, len);
#endif
}

static void https_pool_reap(void) {
    uint64_t dead = 0;
    int guard = HTTPS_POOL_SLOTS + 1;
    while (guard-- > 0 &&
           tlspool_reap_rs(g_https_pool, HTTPS_POOL_SLOTS, sched_now_ms(),
                           HTTPS_POOL_IDLE_MS, &dead) == 1 && dead) {
        https_close((https_conn_t *)(unsigned long)dead);
        dead = 0;
    }
}

/* Hand back a live pooled connection, or open a fresh one. */
static https_conn_t *https_conn_acquire(const char *host, uint16_t port,
                                        int force_h1, int no_pool, int *reused) {
    uint16_t flags = (uint16_t)(force_h1 ? 1 : 0);
    if (reused) *reused = 0;

    if (g_https_reuse && !no_pool) {
        https_pool_reap();
        uint64_t c = 0;
        if (tlspool_acquire_rs(g_https_pool, HTTPS_POOL_SLOTS,
                               (const uint8_t *)host, https_strlen32(host),
                               port, flags, sched_now_ms(),
                               HTTPS_POOL_IDLE_MS, &c) == 1 && c) {
            https_conn_t *conn = (https_conn_t *)(unsigned long)c;
            /* Do not discover a peer-side FIN by writing a request into it. */
            if (https_is_connected(conn) &&
                tcp_get_state(conn->tcp_socket) == TCP_STATE_ESTABLISHED) {
                g_https_pool_hit++;
                if (reused) *reused = 1;
                g_pf_dns = 0; g_pf_arp = 0; g_pf_tcp = 0; g_pf_tls = 0;
                return conn;
            }
            https_close(conn);
        }
    }

    g_https_pool_miss++;
    https_conn_t *conn;
    if (force_h1) {
        extern int g_tls_force_h1_alpn;
        int saved = g_tls_force_h1_alpn;
        g_tls_force_h1_alpn = 1;
        conn = https_connect(host, port);
        g_tls_force_h1_alpn = saved;
    } else {
        conn = https_connect(host, port);
    }
    if (conn) conn->pool_flags = flags;
    return conn;
}

/* Give a connection back to the pool (reusable != 0) or close it. The two
   outcomes are disjoint on both sides of the FFI, so nothing double-closes. */
static void https_conn_release(https_conn_t *conn, int reusable) {
    if (!conn) return;
    if (!g_https_reuse || !reusable ||
        !https_is_connected(conn) ||
        tcp_get_state(conn->tcp_socket) != TCP_STATE_ESTABLISHED) {
        tlspool_drop_rs(g_https_pool, HTTPS_POOL_SLOTS, (uint64_t)(unsigned long)conn);
        https_close(conn);
        return;
    }
    uint64_t evicted = 0;
    int rc = tlspool_store_rs(g_https_pool, HTTPS_POOL_SLOTS,
                              (const uint8_t *)conn->hostname,
                              https_strlen32(conn->hostname),
                              conn->port, conn->pool_flags,
                              (uint64_t)(unsigned long)conn, sched_now_ms(), &evicted);
    if (evicted) https_close((https_conn_t *)(unsigned long)evicted);
    if (rc != 1) https_close(conn);   /* pool refused it: we still own it */
}

/* Drop every pooled connection (e.g. the link went down). */
void https_pool_flush(void) {
    for (int i = 0; i < HTTPS_POOL_SLOTS; i++) {
        uint64_t c = g_https_pool[i].conn;
        if (c) {
            tlspool_drop_rs(g_https_pool, HTTPS_POOL_SLOTS, c);
            https_close((https_conn_t *)(unsigned long)c);
        }
    }
}


// Get error string
const char *https_strerror(int error) {
    switch (error) {
        case HTTPS_SUCCESS:       return "Success";
        case HTTPS_ERR_NO_MEMORY: return "Out of memory";
        case HTTPS_ERR_DNS:       return "DNS resolution failed";
        case HTTPS_ERR_CONNECT:   return "Connection failed";
        case HTTPS_ERR_TLS:       return "TLS error";
        case HTTPS_ERR_TIMEOUT:   return "Timeout";
        case HTTPS_ERR_CLOSED:    return "Connection closed";
        case HTTPS_ERR_TRUNCATED: return "Truncated body (Content-Length/chunked mismatch)";
        case HTTPS_ERR_SSRF_BLOCKED: return "Redirect blocked (private/loopback host or scheme downgrade)";
        default:                  return "Unknown error";
    }
}

// Simple snprintf for building requests
static int https_snprintf(char *buf, int size, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    int written = 0;
    while (*fmt && written < size - 1) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 's') {
                const char *s = __builtin_va_arg(ap, const char *);
                while (*s && written < size - 1) {
                    buf[written++] = *s++;
                }
            } else if (*fmt == 'd') {
                int val = __builtin_va_arg(ap, int);
                char tmp[16];
                int neg = 0;
                if (val < 0) { neg = 1; val = -val; }
                int i = 0;
                do { tmp[i++] = '0' + (val % 10); val /= 10; } while (val > 0);
                if (neg && written < size - 1) buf[written++] = '-';
                while (i > 0 && written < size - 1) buf[written++] = tmp[--i];
            }
            fmt++;
        } else {
            buf[written++] = *fmt++;
        }
    }

    buf[written] = '\0';
    __builtin_va_end(ap);
    return written;
}

// Parse URL for HTTPS
static int https_parse_url(const char *url, char *host, char *path, uint16_t *port) {
    const char *p = url;

    *port = 443;  // Default HTTPS port
    path[0] = '/';
    path[1] = '\0';

    // Must start with https://
    if (strncmp(p, "https://", 8) != 0) {
        return -1;
    }
    p += 8;

    // Extract host
    int host_len = 0;
    while (*p && *p != ':' && *p != '/' && host_len < 255) {
        host[host_len++] = *p++;
    }
    host[host_len] = '\0';

    // Port
    if (*p == ':') {
        p++;
        int port_val = 0;
        while (*p >= '0' && *p <= '9') {
            port_val = port_val * 10 + (*p - '0');
            p++;
        }
        if (port_val > 0 && port_val < 65536) {
            *port = (uint16_t)port_val;
        }
    }

    // Path
    if (*p == '/') {
        int path_len = 0;
        while (*p && path_len < 1023) {
            path[path_len++] = *p++;
        }
        path[path_len] = '\0';
    }

    return 0;
}

// ============================================================================
// #404 Phase Y: HTTP response length-parse seam routed to Rust (rustkern.rs)
// under -DRUST_HTTP_PARSE (Makefile CFLAGS). The untrusted chunk-size / header-
// framing / Content-Length byte parse crosses into the bounds-checked Rust seam;
// the socket recv, body buffering, header-field extraction, and redirect/SSRF
// gating all STAY in C. Each C reference is kept verbatim as https_*_c for the
// boot-time [RUST-DIFF] differential and for a flag-off rollback. REMOVE the one
// -DRUST_HTTP_PARSE line to fall straight back to C.
//
// [RUST-SEC] MAYTERA-SEC-2026-0008: https_dechunk_c below has a REACHABLE u32
// integer-overflow -> OOB read+write (the `in + sz > len` clamp is bypassed when
// `in + sz` wraps). The C-fallback hardening of that exact bound is task #504.
// ============================================================================
extern uint32_t https_dechunk_rs(uint8_t *buf, uint32_t len);
extern int      https_chunk_complete_rs(const uint8_t *buf, uint32_t len);
extern int      http_find_header_end_rs(const uint8_t *buf, uint32_t len);
extern uint32_t https_content_length_rs(const uint8_t *hdr, uint32_t len);

// Find header end (verbatim reference)
static int https_find_header_end_c(const uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n') {
            return (int)(i + 4);
        }
    }
    return -1;
}
static int https_find_header_end(const uint8_t *buf, uint32_t len) {
#ifdef RUST_HTTP_PARSE
    return http_find_header_end_rs(buf, len);
#else
    return https_find_header_end_c(buf, len);
#endif
}

// Parse status code
static int https_parse_status(const char *response) {
    const char *p = response;
    while (*p && *p != ' ') p++;
    if (!*p) return 0;
    p++;

    int code = 0;
    while (*p >= '0' && *p <= '9') {
        code = code * 10 + (*p - '0');
        p++;
    }
    return code;
}

// HTTPS GET request
static int https_hdr_has(const char *hdr, const char *needle);

// Decode HTTP/1.1 chunked transfer-encoding in place. Returns the decoded length.
// Each chunk = <hex-size>[;ext]CRLF <data> CRLF, terminated by a 0-size chunk.
// Verbatim reference. #504: `in + sz > len` is u32 and WRAPS for a near-2^32
// hex chunk size, skipping the clamp -> the memmove over-copies (OOB). Kept
// unchanged as the ASan-provable reference; the live path routes the Rust seam.
static uint32_t https_dechunk_c(uint8_t *buf, uint32_t len) {
    uint32_t in = 0, out = 0;
    while (in < len) {
        uint32_t sz = 0; int any = 0;
        while (in < len) {
            uint8_t c = buf[in];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            sz = sz * 16u + (uint32_t)d; any = 1; in++;
        }
        while (in < len && buf[in] != '\n') in++;   // skip chunk-ext + to LF
        if (in < len) in++;                            // skip the LF
        if (!any || sz == 0) break;                    // final (0) chunk
        if (in + sz > len) sz = (in <= len) ? (len - in) : 0;
        if (sz) { memmove(buf + out, buf + in, sz); out += sz; in += sz; }
        if (in < len && buf[in] == '\r') in++;        // trailing CRLF
        if (in < len && buf[in] == '\n') in++;
    }
    return out;
}
static uint32_t https_dechunk(uint8_t *buf, uint32_t len) {
#ifdef RUST_HTTP_PARSE
    return https_dechunk_rs(buf, len);
#else
    return https_dechunk_c(buf, len);
#endif
}

// #fix-ssrf-contentlength: determine whether a chunked body currently held in
// buf[0..len) is FULLY framed, by walking real chunk framing (hex size + CRLF
// + payload + CRLF, repeated, ending at a 0-size chunk + its trailer-end
// blank line) -- not by scanning for the byte sequence "0\r\n" anywhere in
// the buffer, which false-triggers whenever a legitimate binary/text body
// happens to contain that sequence mid-payload and truncates it. Returns:
//   1  the terminating chunk (and its trailer-end blank line) have arrived
//   0  need more data (an incomplete chunk/line at the tail; keep receiving)
//  -1  malformed framing (not a recoverable "need more data" case)
static int https_chunked_is_complete_c(const uint8_t *buf, uint32_t len) {
    uint32_t p = 0;
    while (1) {
        uint32_t sz = 0; int any = 0;
        while (p < len) {
            uint8_t c = buf[p]; int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            sz = sz * 16u + (uint32_t)d; any = 1; p++;
        }
        if (!any) return (p >= len) ? 0 : -1;    // no size digits: need more, or garbage
        while (p < len && buf[p] != '\n') p++;    // chunk-ext + CRLF ending the size line
        if (p >= len) return 0;                    // size line not finished yet
        p++;                                        // past the LF

        if (sz == 0) {
            // Trailer section: zero or more header lines, ended by a blank line.
            while (1) {
                if (p >= len) return 0;
                if (buf[p] == '\n') return 1;                          // bare-LF blank line
                if (buf[p] == '\r') {
                    if (p + 1 >= len) return 0;
                    if (buf[p + 1] == '\n') return 1;                  // CRLF blank line
                }
                while (p < len && buf[p] != '\n') p++;                 // skip a trailer line
                if (p >= len) return 0;
                p++;
            }
        }

        // #504 (scope WIDENED 2026-07-16 by the #404 drift audit 2): this gate
        // had the SAME u32-wrap class as https_dechunk_c's `in + sz > len`
        // clamp, but a WORSE symptom: NON-TERMINATION (CWE-835), not an OOB.
        // With a near-2^32 chunk size, `p + sz` WRAPS to something <= len, the
        // gate passes, and `p += sz` then walks p BACKWARDS onto an earlier
        // byte. If that byte re-enters the size parser the loop spins forever,
        // burning a core inside https_get with no timeout to break it.
        //
        // #504 as filed covers ONLY the dechunk clamp, so it would have left
        // this hang live on a -DRUST_HTTP_PARSE-off build. Subtract instead of
        // add: p <= len is an invariant here (every path above returns when
        // p >= len before advancing), so len - p cannot underflow, and this
        // form cannot wrap for ANY sz.
        if (sz > len - p) return 0;               // chunk payload not fully arrived
        p += sz;                                   // safe: sz <= len - p
        if (p >= len) return 0;                    // need the trailing CRLF after the data
        if (buf[p] == '\r') p++;
        if (p >= len) return 0;
        if (buf[p] == '\n') p++; else return -1;
    }
}
static int https_chunked_is_complete(const uint8_t *buf, uint32_t len) {
#ifdef RUST_HTTP_PARSE
    return https_chunk_complete_rs(buf, len);
#else
    return https_chunked_is_complete_c(buf, len);
#endif
}

// Parse the Content-Length header value (0 = absent/unknown). Header region only.
static uint32_t https_content_length_c(const char *hdr) {
    const char *needle = "content-length:";
    for (const char *p = hdr; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b) {
            char ca = *a; if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (ca != *b) break;
            a++; b++;
        }
        if (!*b) {
            while (*a == ' ' || *a == '\t') a++;
            uint32_t v = 0; int any = 0;
            while (*a >= '0' && *a <= '9') { v = v * 10u + (uint32_t)(*a - '0'); a++; any = 1; }
            return any ? v : 0;
        }
    }
    return 0;
}
static uint32_t https_content_length(const char *hdr) {
#ifdef RUST_HTTP_PARSE
    // The header region is NUL-terminated by the caller (buffer[headers_end-1]
    // = '\0'); bound the Rust slice to that length so the scan never exceeds it.
    uint32_t n = 0; while (hdr[n]) n++;
    return https_content_length_rs((const uint8_t *)hdr, n);
#else
    return https_content_length_c(hdr);
#endif
}

static int https_is_redirect(int code) {
    return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

// #fix-ssrf-contentlength: is this IPv4 address (host byte order -- high byte
// is the first octet, matching dns_resolve()/ip_print()'s convention here)
// private, loopback, or link-local? A public-origin redirect must never be
// allowed to steer the kernel HTTP client at one of these (classic SSRF via
// a 3xx Location header), including cloud-metadata addresses which live in
// the 169.254.0.0/16 link-local block.
static int https_ip_is_private(uint32_t ip) {
    uint8_t a = (uint8_t)(ip >> 24);
    uint8_t b = (uint8_t)(ip >> 16);
    if (ip == 0) return 1;                          // 0.0.0.0/8
    if (a == 10) return 1;                          // 10.0.0.0/8
    if (a == 127) return 1;                         // 127.0.0.0/8 (loopback)
    if (a == 169 && b == 254) return 1;              // 169.254.0.0/16 (link-local/metadata)
    if (a == 172 && b >= 16 && b <= 31) return 1;    // 172.16.0.0/12
    if (a == 192 && b == 168) return 1;              // 192.0.2.1/16
    return 0;
}

// Resolve `host` (hostname or literal IP) and report whether it lands on a
// private/loopback/link-local address. Resolving (rather than only checking
// literal dotted-quad text) also closes DNS-rebinding: a hostname Location
// that resolves to 127.0.0.1 is blocked the same as a literal IP would be.
static int https_host_is_private(const char *host) {
    uint32_t ip = 0;
    if (dns_resolve(host, &ip) != 0) return 0;   // unresolvable: let the normal
                                                  // connect path fail it on its own
    return https_ip_is_private(ip);
}
// Find the Location header value in a (NUL-terminated) header block. Case-insensitive.
static int https_find_location(const char *h, char *out, int cap) {
    const char *p = h;
    while (*p) {
        if (p == h || p[-1] == '\n') {
            const char *k = "location:"; int i = 0;
            while (k[i]) {
                char c = p[i]; if (c >= 'A' && c <= 'Z') c += 32;
                if (c != k[i]) break;
                i++;
            }
            if (!k[i]) {
                const char *v = p + i;
                while (*v == ' ' || *v == '\t') v++;
                int n = 0;
                while (*v && *v != '\r' && *v != '\n' && n < cap - 1) out[n++] = *v++;
                out[n] = 0;
                return n > 0;
            }
        }
        p++;
    }
    return 0;
}
// Resolve a redirect target against the current absolute URL.
static void https_resolve_url(const char *base, const char *loc, char *out, int cap) {
    int n = 0;
    if (!loc) { out[0] = 0; return; }
    int blen = 0; while (base[blen]) blen++;
    if (loc[0]=='h'&&loc[1]=='t'&&loc[2]=='t'&&loc[3]=='p') {
        for (int i = 0; loc[i] && n < cap-1; i++) out[n++] = loc[i];
    } else if (loc[0]=='/' && loc[1]=='/') {
        const char *sc = "https:"; for (int i=0; sc[i] && n<cap-1; i++) out[n++]=sc[i];
        for (int i=0; loc[i] && n<cap-1; i++) out[n++]=loc[i];
    } else {
        int he=0, sl=0;
        for (int i=0; base[i]; i++) { if (base[i]=='/') { sl++; if (sl==3){he=i;break;} } }
        if (he==0) he=blen;
        if (loc[0]=='/') {
            for (int i=0; i<he && n<cap-1; i++) out[n++]=base[i];
            for (int i=0; loc[i] && n<cap-1; i++) out[n++]=loc[i];
        } else {
            int de=0, s2=0;
            for (int i=0; base[i]; i++) { if (base[i]=='/') { s2++; if (s2>=3) de=i+1; } }
            int upto = de>he?de:he;
            for (int i=0; i<upto && n<cap-1; i++) out[n++]=base[i];
            if (n==0 || out[n-1] != '/') { if (n<cap-1) out[n++]='/'; }
            for (int i=0; loc[i] && n<cap-1; i++) out[n++]=loc[i];
        }
    }
    out[n]=0;
}

// #576: GET-over-TLS core shared by the plain browser GET (https_get) and the
// header/Range GET (https_get_hdr). extra_headers, when non-NULL/non-empty, is
// a block of CRLF-terminated request-header lines (e.g. "Range: bytes=0-255\r\n"),
// same contract as https_post()'s `headers`. It reuses the existing TLS
// transport + HTTP response parser; it does NOT add a second TLS or HTTP parser.
static int https_get_ex(const char *url, const char *extra_headers,
                        uint8_t **body_out, uint32_t *body_len_out, int *status_out) {
    char host[256];
    char path[1024];
    uint16_t port;
    int have_hdr = (extra_headers && extra_headers[0]);

    if (body_out) *body_out = NULL;
    if (body_len_out) *body_len_out = 0;
    if (status_out) *status_out = 0;

    char cur_url[1100];
    { int i = 0; for (; url[i] && i < (int)sizeof(cur_url)-1; i++) cur_url[i]=url[i]; cur_url[i]=0; }
    int redir_hops = 0;
    int force_fresh = 0;    /* #616: bypass the pool on the one retry */
    int reuse_retry = 0;    /* #616: only ever retry a stale pooled connection once */

    // #fix-ssrf-contentlength: capture whether the ORIGINAL request already
    // targets a private/loopback/link-local host (e.g. a LAN device or the
    // local Home Assistant box, #414). If so, the caller has already opted
    // into an internal destination and redirects among private hosts are
    // fine; otherwise a public-origin request must never be redirected onto
    // an internal address (classic SSRF via a 3xx Location header).
    int orig_is_private = 0;
    {
        char oh[256]; char op[1024]; uint16_t oport;
        if (https_parse_url(cur_url, oh, op, &oport) == 0) {
            orig_is_private = https_host_is_private(oh);
        }
    }
redo_fetch: ;

    // Parse URL
    if (https_parse_url(cur_url, host, path, &port) != 0) {
        kprintf("[HTTPS] Invalid URL\n");
        return HTTPS_ERR_CONNECT;
    }

    // Connect. #576: a custom-header/Range GET must use HTTP/1.1 (the h2 client
    // has no Range/custom-header support), so force the ALPN to http/1.1 for it,
    // the same way https_post() does for POST.
    uint64_t _t0 = sched_now_ms();
    uint64_t _t_conn = 0, _t_req = 0, _t_first = 0, _t_last = 0;
    https_conn_t *conn;
    int was_reused = 0;
    /* #616: a pooled, already-verified connection to this exact host:port when
       one is live, otherwise a fresh TCP + full verified TLS handshake. */
    conn = https_conn_acquire(host, port, have_hdr, force_fresh, &was_reused);
    force_fresh = 0;
    if (!conn) {
        return HTTPS_ERR_CONNECT;
    }
    _t_conn = sched_now_ms();
    net_progress_phase(HTTP_PHASE_SENDING);   // #25: covers both a fresh connect and a pooled reuse

    // HTTP/2 dispatch: if ALPN negotiated "h2", use the HTTP/2 client. Modern
    // sites (Wikipedia, Cloudflare) prefer h2 and stall an HTTP/1.1 request.
    if (!have_hdr && tls_alpn_is_h2(conn->tls)) {
        kprintf("[HTTPS] ALPN=h2 -> HTTP/2 path\n");
        char loc[1100]; loc[0]=0; int h2st = 0;
        net_progress_phase(HTTP_PHASE_RECEIVING);   // #25: h2_get is a black box, no byte-granular progress
        int h2rc = http2_get(conn->tls, host, path, body_out, body_len_out, &h2st, loc, sizeof(loc));
        https_close(conn);
        // #333: a transient net-stack stall (RX contention when many fetches run
        // at once) can make http2_get return with NO status (rc<0, h2st==0) even
        // though the request/response framing is correct. Reconnect and retry a
        // couple of times; DNS is positively cached by now so the reconnect is
        // cheap and does not hit the DNS negative-cache. This makes h2 fetches to
        // musicbrainz.org / archive.org (album-art path) reliable under load.
        for (int h2try = 0; h2rc != 0 && h2st == 0 && h2try < 3; h2try++) {
            kprintf("[HTTPS] h2 no-status; reconnect+retry %d\n", h2try + 1);
            net_poll(); tcp_timer(); proc_sleep(400);
            conn = https_connect(host, port);
            if (!conn) break;
            loc[0] = 0; h2st = 0;
            h2rc = http2_get(conn->tls, host, path, body_out, body_len_out, &h2st, loc, sizeof(loc));
            https_close(conn);
        }
        if (status_out) *status_out = h2st;
        if (h2rc == 0 && https_is_redirect(h2st) && loc[0] && redir_hops < 6) {
            char nxt[1100]; https_resolve_url(cur_url, loc, nxt, sizeof(nxt));
            // #fix-ssrf-contentlength: only follow if the resolved target is
            // still https:// (https_parse_url rejects anything else, blocking
            // a downgrade to plain http) AND is not a private/loopback/
            // link-local host unless the original request already was one.
            char rhost[256]; char rpath[1024]; uint16_t rport;
            if (https_parse_url(nxt, rhost, rpath, &rport) == 0 &&
                (orig_is_private || !https_host_is_private(rhost))) {
                if (body_out && *body_out) { kfree(*body_out); *body_out = NULL; }
                if (body_len_out) *body_len_out = 0;
                { int i=0; for(; nxt[i] && i<(int)sizeof(cur_url)-1; i++) cur_url[i]=nxt[i]; cur_url[i]=0; }
                redir_hops++;
                goto redo_fetch;
            }
            kprintf("[HTTPS] Redirect blocked (SSRF/scheme downgrade): %s\n", nxt);
        }
        return (h2rc == 0) ? HTTPS_SUCCESS : HTTPS_ERR_TLS;
    }

    // Build request. #576: inject the caller's extra headers (e.g. a Range:
    // line) when present, and size the buffer to fit them (a Range line or a
    // JWT auth header can exceed the old fixed 2048).
    uint32_t req_cap = 2048 + (have_hdr ? (uint32_t)strlen(extra_headers) : 0);
    char *request = (char *)kmalloc(req_cap);
    if (!request) { https_conn_release(conn, 0); return HTTPS_ERR_NO_MEMORY; }
    /* #190: send a modern-Chrome request line + headers. Fingerprinting CDNs
       (Cloudflare WAF, Akamai) score the User-Agent and header set alongside
       the TLS JA3/JA4; a non-browser UA can trigger an immediate post-handshake
       drop or managed challenge even when the TLS fingerprint passes. */
    int req_len = https_snprintf(request, (int)req_cap,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Accept-Encoding: identity\r\n"
        "%s"
        "%s"
        "\r\n",
        path, host, have_hdr ? extra_headers : "",
        g_https_reuse ? "Connection: keep-alive\r\n" : "Connection: close\r\n");

    // Send request
    int ret = https_send(conn, request, req_len);
    _t_req = sched_now_ms();
    kfree(request);
    if (ret < 0) {
        kprintf("[HTTPS] Failed to send request: %d\n", ret);
        https_conn_release(conn, 0);
        /* #616: a pooled connection the peer had already torn down fails HERE.
           Retry ONCE on a genuinely fresh handshake before reporting failure. */
        if (was_reused && !reuse_retry) { reuse_retry = 1; force_fresh = 1; goto redo_fetch; }
        return ret;
    }

    // Receive response (64KB buffer for larger pages)
    uint32_t buffer_size = 1048576;   // 1MB: real web pages exceed 64KB (#245)
    uint8_t *buffer = kmalloc(buffer_size);
    if (!buffer) {
        https_conn_release(conn, 0);
        return HTTPS_ERR_NO_MEMORY;
    }

    uint32_t buffer_len = 0;
    int headers_end = -1;
    int status_code = 0;

    int would_block_count = 0;
    int is_chunked = 0;
    int has_cl = 0;                 /* #616: a Content-Length header was present */
    int no_body = 0;                /* #616: 1xx/204/304 carry no body at all */
    int chunked_done = 0;           /* set once real chunk framing confirms the terminator arrived */
    uint32_t content_length = 0;   /* 0 = unknown */
    /* #499: 30s of REAL time. timer_ticks is not a wall clock, so under a KVM
       tick burst this whole-fetch deadline used to expire before the first
       header byte was even parsed. */
    uint64_t recv_deadline_ms = sched_now_ms() + 30000;
    while (buffer_len < buffer_size) {
        g_dp_hb_iter++;
        if ((int64_t)(sched_now_ms() - recv_deadline_ms) >= 0) break;  /* overall fetch deadline */
        ret = https_recv(conn, buffer + buffer_len, buffer_size - buffer_len);
        if (ret < 0) {
            if (ret == TLS_ERR_CLOSED || ret == HTTPS_ERR_CLOSED) break;
            if (ret == TLS_ERR_TIMEOUT) break;  /* #277: keep partial body on stall */
            if (ret == TLS_ERR_WOULD_BLOCK) {
                // #420: do NOT treat a stall as "end of data" just because
                // headers already arrived. https_tcp_recv() already waited
                // up to ~5s for a byte before returning WOULD_BLOCK here, and
                // a gap of a few seconds between TLS records is normal for a
                // large body under load (this is what truncated big HA/API
                // and browser responses to a partial body instead of erroring
                // outright). The real "done" signals are the content-length/
                // chunked-terminator checks below and TLS_ERR_CLOSED above;
                // WOULD_BLOCK just means "keep waiting" until the overall
                // recv_deadline (~30s) above gives up. would_block_count is
                // now only a defensive fallback in case the deadline check
                // is ever bypassed.
                if (++would_block_count > 12) break;
                // Poll network to process any pending packets, then yield so
                // the OS stays responsive instead of spinning during the fetch.
                net_poll();
                tcp_timer();
                g_dp_hb_sleep++;
                proc_sleep(2);
                continue;
            }
            kfree(buffer);
            https_conn_release(conn, 0);
            /* #616: same stale-pooled-connection retry as the send path. */
            if (was_reused && !reuse_retry && buffer_len == 0) {
                reuse_retry = 1; force_fresh = 1; goto redo_fetch;
            }
            return ret;
        }
        if (ret == 0) break;
        would_block_count = 0;
        // #25: gate BOTH actions on one check of the PRE-assignment state -
        // testing _t_first again after the assignment below is always false
        // (a live boot-log capture caught this: the RECEIVING transition
        // never fired because this used to be two separate `if (!_t_first)`
        // checks, and the first one already makes _t_first non-zero).
        if (!_t_first) { _t_first = sched_now_ms(); net_progress_phase(HTTP_PHASE_RECEIVING); }

        buffer_len += ret;

        // Look for headers end
        if (headers_end < 0) {
            headers_end = https_find_header_end(buffer, buffer_len);
            if (headers_end > 0) {
                buffer[headers_end - 1] = '\0';
                status_code = https_parse_status((const char *)buffer);
                kprintf("[HTTPS] Status: %d\n", status_code);
                is_chunked = https_hdr_has((const char *)buffer,
                                           "transfer-encoding: chunked");
                content_length = https_content_length((const char *)buffer);
                net_progress_content_len(content_length);   // #25
                /* #616: with Connection: keep-alive the peer no longer closes to
                   signal the end, so a bodyless (1xx/204/304) or Content-Length: 0
                   response must be recognised explicitly or the loop would sit on
                   the 30s deadline. */
                has_cl = https_hdr_has((const char *)buffer, "content-length:");
                no_body = (status_code == 204 || status_code == 304 ||
                           (status_code >= 100 && status_code < 200));
                // #608: same 206 Content-Range gate as the plain-HTTP client
                // (net/wget.c). Rust, fail-open, rejects only a provable
                // wrong-start / overshoot mismatch.
                if (status_code == 206 && have_hdr) {
                    extern int http_range_check_rs(const uint8_t *, uint32_t,
                                                   const uint8_t *, uint32_t);
                    uint32_t _hl = 0; while (extra_headers[_hl]) _hl++;
                    if (!http_range_check_rs((const uint8_t *)extra_headers, _hl,
                                             buffer, (uint32_t)headers_end)) {
                        kprintf("[HTTPS] 206 Content-Range does not satisfy the "
                                "requested Range; refusing\n");
                        kfree(buffer);
                        https_conn_release(conn, 0);
                        return HTTPS_ERR_TRUNCATED;
                    }
                }
            }
        }

        // Deterministic completion: stop once the full body has arrived rather
        // than waiting for the peer to close. Large HTTP/1.1 keep-alive servers
        // (e.g. Wikipedia) otherwise never close, so the old close-driven loop
        // stalled until the per-recv timeout/buffer cap and hung the browser.
        if (headers_end > 0) {
            uint32_t body_len = buffer_len - (uint32_t)headers_end;
            net_progress_bytes(body_len);   // #25
            if (is_chunked) {
                // #fix-ssrf-contentlength: real chunk-framing walk instead of
                // scanning the tail for the raw bytes "0\r\n\r\n" -- that
                // substring can legitimately appear inside a chunk's own
                // payload (binary data, or text that happens to contain
                // "0\r\n\r\n"), which falsely signalled "done" and truncated
                // the body while still returning HTTPS_SUCCESS. Bounded by
                // buffer_size (1MB) and the number of network reads, not
                // per-byte, so this stays well clear of the old O(n^2)
                // per-byte scan that pinned the CPU.
                int cc = https_chunked_is_complete(buffer + headers_end, body_len);
                if (cc == 1) { chunked_done = 1; goto recv_done; }
                if (cc < 0) {
                    kprintf("[HTTPS] Malformed chunked framing; aborting\n");
                    kfree(buffer);
                    https_conn_release(conn, 0);
                    return HTTPS_ERR_TRUNCATED;
                }
                // cc == 0: terminating chunk not seen yet, keep receiving.
            } else if (no_body || (has_cl && body_len >= content_length)) {
                goto recv_done;
            }
        }
    }
recv_done:
    _t_last = sched_now_ms();

    /* #616: a pooled connection that produced NOTHING is a stale socket the
       peer closed while it sat idle. Retry once on a fresh handshake. */
    if (was_reused && !reuse_retry && buffer_len == 0) {
        kfree(buffer);
        https_conn_release(conn, 0);
        reuse_retry = 1; force_fresh = 1;
        goto redo_fetch;
    }
    /* Keep the connection ONLY when this response was framed unambiguously
       (so no unread bytes can bleed into the next request) and the peer did
       not ask to close. Anything else: close, exactly as before. */
    {
        uint32_t _blen = (headers_end > 0) ? (buffer_len - (uint32_t)headers_end) : 0;
        int _framed = (headers_end > 0) && !is_chunked &&
                      (no_body ? (_blen == 0) : (has_cl && _blen == content_length));
        https_conn_release(conn, _framed && https_reuse_ok(buffer, (uint32_t)headers_end) == 1);
    }

    if (g_https_prof) {
        kprintf("[HTTPPROF] GET tot=%u conn=%u(dns=%u arp=%u tcp=%u tls=%u) req=%u ttfb=%u body=%u len=%u st=%d reuse=%d pool=%u/%u\n",
                (uint32_t)(_t_last - _t0), (uint32_t)(_t_conn - _t0),
                (uint32_t)g_pf_dns, (uint32_t)g_pf_arp, (uint32_t)g_pf_tcp, (uint32_t)g_pf_tls,
                (uint32_t)(_t_req - _t_conn),
                (uint32_t)(_t_first ? (_t_first - _t_req) : 0),
                (uint32_t)(_t_first ? (_t_last - _t_first) : 0),
                buffer_len, status_code, was_reused,
                g_https_pool_hit, g_https_pool_miss);
    }

    if (status_out) *status_out = status_code;

    // #fix-ssrf-contentlength: the loop above can also exit here via a plain
    // `break` (peer close, TLS timeout, WOULD_BLOCK cap, or the overall ~30s
    // deadline) with the body short of the advertised Content-Length. Treat
    // that as an error instead of silently returning HTTPS_SUCCESS with a
    // truncated body -- this is what corrupted JSON callers (HA /api/states,
    // pip metadata) that only check the status code.
    if (headers_end > 0 && is_chunked && !chunked_done) {
        kprintf("[HTTPS] Truncated chunked body: connection ended before the terminating chunk\n");
        kfree(buffer);
        return HTTPS_ERR_TRUNCATED;
    }
    if (headers_end > 0 && !is_chunked && content_length > 0) {
        uint32_t body_len = buffer_len - (uint32_t)headers_end;
        if (body_len < content_length) {
            kprintf("[HTTPS] Truncated body: got %u of %u advertised bytes\n",
                    body_len, content_length);
            kfree(buffer);
            return HTTPS_ERR_TRUNCATED;
        }
    }

    if (https_is_redirect(status_code) && headers_end > 0 && redir_hops < 6) {
        char loc[1100];
        if (https_find_location((const char *)buffer, loc, sizeof(loc)) && loc[0]) {
            char nxt[1100]; https_resolve_url(cur_url, loc, nxt, sizeof(nxt));
            // #fix-ssrf-contentlength: same SSRF gate as the h2 redirect path.
            char rhost[256]; char rpath[1024]; uint16_t rport;
            if (https_parse_url(nxt, rhost, rpath, &rport) == 0 &&
                (orig_is_private || !https_host_is_private(rhost))) {
                kfree(buffer);
                { int i=0; for(; nxt[i] && i<(int)sizeof(cur_url)-1; i++) cur_url[i]=nxt[i]; cur_url[i]=0; }
                redir_hops++;
                goto redo_fetch;
            }
            kprintf("[HTTPS] Redirect blocked (SSRF/scheme downgrade): %s\n", nxt);
        }
    }

    // Return body
    if (headers_end > 0 && body_out) {
        uint32_t body_len = buffer_len - headers_end;
        if (body_len > 0) {
            uint8_t *body = kmalloc(body_len + 1);
            if (body) {
                memcpy(body, buffer + headers_end, body_len);
                if (https_hdr_has((const char *) buffer,
                                  "transfer-encoding: chunked")) {
                    body_len = https_dechunk(body, body_len);
                }
                body[body_len] = '\0';
                *body_out = body;
                if (body_len_out) *body_len_out = body_len;
            }
        }
    }

    kfree(buffer);
    return HTTPS_SUCCESS;
}

// #576: public wrappers around the shared GET-over-TLS core.
int https_get(const char *url, uint8_t **body_out, uint32_t *body_len_out, int *status_out) {
    return https_get_ex(url, (const char *)0, body_out, body_len_out, status_out);
}

// #576: GET with caller-supplied extra request headers (e.g. a Range: line for
// the App Store's chunked partial-content package download) over the SAME TLS
// transport + HTTP response parser as the plain browser GET. Handles 206
// Partial Content and a server that ignores Range (200 full body) through the
// shared recv/parse loop (Content-Length drives completion for both).
int https_get_hdr(const char *url, const char *extra_headers,
                  uint8_t **body_out, uint32_t *body_len_out, int *status_out) {
    return https_get_ex(url, extra_headers, body_out, body_len_out, status_out);
}

// Case-insensitive substring search for a header presence (header region only).
static int https_hdr_has(const char *hdr, const char *needle) {
    for (const char *p = hdr; *p; p++) {
        const char *a = p, *b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

// HTTPS POST: send `body` with the caller's extra `headers` (e.g.
// "Authorization: Bearer ...\r\nContent-Type: application/json\r\n") and return
// the response body (chunked transfer-encoding decoded). Mirrors https_get but
// for POST + a request body. Used by sys_http_post (LLM/REST clients).
int https_post(const char *url, const char *headers, const char *body,
               uint8_t **body_out, uint32_t *body_len_out, int *status_out) {
    char host[256];
    char path[1024];
    uint16_t port;
    extern ssize_t http_decode_chunked(uint8_t *buf, size_t len);

    if (body_out) *body_out = NULL;
    if (body_len_out) *body_len_out = 0;
    if (status_out) *status_out = 0;

    if (https_parse_url(url, host, path, &port) != 0) {
        kprintf("[HTTPS] POST invalid URL\n");
        return HTTPS_ERR_CONNECT;
    }

    extern int g_tls_force_h1_alpn;
    uint64_t _pt0 = sched_now_ms(), _pt_conn = 0, _pt_req = 0, _pt_first = 0;
    int saved_h1_alpn = g_tls_force_h1_alpn;
    g_tls_force_h1_alpn = 1;   // #185: POST over HTTP/1.1 (h2 client has no POST yet)
    https_conn_t *conn = https_connect(host, port);
    g_tls_force_h1_alpn = saved_h1_alpn;   // #333: restore prior default (now 1 = http/1.1-only), do not force-enable h2
    if (!conn) { kprintf("[HTTPPROF] POST connect FAILED host=%s\n", host); return HTTPS_ERR_CONNECT; }
    _pt_conn = sched_now_ms();

    uint32_t body_len = body ? (uint32_t)strlen(body) : 0;

    // Request line + headers can be large with extra headers; build dynamically.
    uint32_t req_cap = 2048 + body_len + (headers ? (uint32_t)strlen(headers) : 0);
    char *request = kmalloc(req_cap);
    if (!request) { https_close(conn); return HTTPS_ERR_NO_MEMORY; }

    int req_len = https_snprintf(request, (int)req_cap,
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36\r\n"
        "Accept: */*\r\n"
        "Accept-Encoding: identity\r\n"
        "%s"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, headers ? headers : "", (int)body_len);
    // Append the body (https_snprintf may not handle very large %s safely).
    if (body_len) {
        if ((uint32_t)req_len + body_len < req_cap) {
            memcpy(request + req_len, body, body_len);
            req_len += (int)body_len;
        }
    }

    if (g_tls_dbg) kprintf("[POSTDBG] sending request req_len=%d host=%s path=%s\n", req_len, host, path);
    int ret = https_send(conn, request, req_len);
    if (g_tls_dbg) kprintf("[POSTDBG] https_send ret=%d (req_len=%d)\n", ret, req_len);
    _pt_req = sched_now_ms();
    kfree(request);
    if (ret < 0) { kprintf("[HTTPPROF] POST send FAILED ret=%d\n", ret); https_close(conn); return ret; }

    uint32_t buffer_size = 131072;   // 128KB for chat responses
    uint8_t *buffer = kmalloc(buffer_size);
    if (!buffer) { https_close(conn); return HTTPS_ERR_NO_MEMORY; }

    uint32_t buffer_len = 0;
    int headers_end = -1;
    int status_code = 0;
    int is_chunked = 0;
    uint32_t content_length = 0;   /* 0 = unknown, #fix-ssrf-contentlength */
    int would_block_count = 0;

    int dbg_calls = 0;
    while (buffer_len < buffer_size) {
        ret = https_recv(conn, buffer + buffer_len, buffer_size - buffer_len);
        if (g_tls_dbg && dbg_calls++ < 40) kprintf("[POSTDBG] recv ret=%d buflen=%u wb=%d\n", ret, (unsigned)buffer_len, would_block_count);
        if (ret < 0) {
            if (ret == TLS_ERR_CLOSED || ret == HTTPS_ERR_CLOSED) break;
            if (ret == TLS_ERR_TIMEOUT) break;  /* #277: keep partial body on stall */
            if (ret == TLS_ERR_WOULD_BLOCK) {
                if (++would_block_count > 4000) break;  // #264 ~20s cap (was 90s, starved the single POST worker)
                net_poll();
                tcp_timer();
                proc_sleep(5);   // yield: keep the desktop responsive during the LLM call
                continue;
            }
            kfree(buffer);
            https_close(conn);
            return ret;
        }
        if (ret == 0) {
            // Transient "no record yet" (e.g. right after skipping NewSessionTicket
            // records, before the HTTP response segment arrives). With Connection:
            // close, real EOF arrives as TLS_ERR_CLOSED. So treat 0 like would-block:
            // poll + yield + retry rather than ending the read prematurely.
            if (++would_block_count > 4000) break;  // #264 ~20s cap (was 90s, starved the single POST worker)
            net_poll();
            tcp_timer();
            proc_sleep(5);
            continue;
        }
        would_block_count = 0;
        if (!_pt_first) _pt_first = sched_now_ms();
        buffer_len += ret;

        if (headers_end < 0) {
            headers_end = https_find_header_end(buffer, buffer_len);
            if (headers_end > 0) {
                char save = (char)buffer[headers_end - 1];
                buffer[headers_end - 1] = '\0';
                status_code = https_parse_status((const char *)buffer);
                is_chunked = https_hdr_has((const char *)buffer, "transfer-encoding: chunked");
                content_length = https_content_length((const char *)buffer);
                buffer[headers_end - 1] = (uint8_t)save;
                kprintf("[HTTPS] POST status %d chunked=%d\n", status_code, is_chunked);
            }
        }
    }

    https_close(conn);
    if (g_https_prof) {
        kprintf("[HTTPPROF] POST tot=%u conn=%u(dns=%u tcp=%u tls=%u) ttfb=%u len=%u st=%d hdrend=%d\n",
                (uint32_t)(sched_now_ms() - _pt0), (uint32_t)(_pt_conn - _pt0),
                (uint32_t)g_pf_dns, (uint32_t)g_pf_tcp, (uint32_t)g_pf_tls,
                (uint32_t)(_pt_first ? (_pt_first - _pt_req) : 0),
                buffer_len, status_code, headers_end);
    }
    if (status_out) *status_out = status_code;

    // #fix-ssrf-contentlength: a short/truncated body (peer stall, the ~20s
    // would-block cap, or the connection closing early) must not be reported
    // as success -- that is what silently corrupted HA/LLM JSON responses.
    if (headers_end > 0 && !is_chunked && content_length > 0) {
        uint32_t blen = buffer_len - (uint32_t)headers_end;
        if (blen < content_length) {
            kprintf("[HTTPS] POST: truncated body: got %u of %u advertised bytes\n",
                    blen, content_length);
            kfree(buffer);
            return HTTPS_ERR_TRUNCATED;
        }
    }

    if (headers_end > 0 && body_out) {
        uint32_t blen = buffer_len - headers_end;
        if (blen > 0) {
            uint8_t *bcopy = kmalloc(blen + 1);
            if (bcopy) {
                memcpy(bcopy, buffer + headers_end, blen);
                if (is_chunked) {
                    ssize_t dec = http_decode_chunked(bcopy, blen);
                    if (dec < 0) {
                        // #fix-ssrf-contentlength: incomplete/malformed chunk
                        // framing used to fall through and hand back the RAW
                        // (still chunk-encoded) bytes as a "successful" body.
                        kprintf("[HTTPS] POST: incomplete/malformed chunked body\n");
                        kfree(bcopy);
                        kfree(buffer);
                        return HTTPS_ERR_TRUNCATED;
                    }
                    blen = (uint32_t)dec;
                }
                bcopy[blen] = '\0';
                *body_out = bcopy;
                if (body_len_out) *body_len_out = blen;
            }
        }
    }

    kfree(buffer);
    return HTTPS_SUCCESS;
}

// ===========================================================================
// #333: network self-test (gated on /CONFIG/NETTEST.CFG).
//   - HTTP/2: GET musicbrainz.org + archive.org -> assert 200 + non-empty body
//     (regression repro for the "[HTTP2] failed: no status" bug).
//   - ARP:    GET an optional fresh same-subnet LAN host (lan=<url> in the CFG)
//     -> assert it connects (repro for the same-subnet first-SYN-drop bug).
// Compares against a known-working h2 host (coingecko) first.
// ===========================================================================
#include "../fs/fat.h"
extern int g_http2_dbg;
extern fat_fs_t g_fat_fs;
extern void kprintf_set_dual_output(int on);

static void nettest_one_n(const char *tag, const char *url, int expect_body, int tries) {
    kprintf("[NETTEST] --- %s: GET %s ---\n", tag, url);
    for (int attempt = 1; attempt <= tries; attempt++) {
        uint8_t *body = NULL; uint32_t blen = 0; int st = 0;
        // #497 fault 4: dispatch on the URL's scheme exactly the way the real
        // consumer (async_fetch_worker, proc/syscall.c) does, so an http:// URL
        // in the CFG genuinely exercises the plain client and its http->https
        // redirect hand-off rather than being forced down the TLS client.
        extern int wget_fetch(const char *, uint8_t **, uint32_t *, int *);
        int is_https = (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p'&&url[4]=='s');
        int rc = is_https ? https_get(url, &body, &blen, &st)
                          : wget_fetch(url, &body, &blen, &st);
        if (rc == 0 && st == 200 && (!expect_body || blen > 0)) {
            kprintf("[NETTEST] %s: PASS (attempt %d) status=%d len=%u first=", tag, attempt, st, blen);
            for (uint32_t i = 0; i < blen && i < 48; i++) {
                char c = (char)body[i];
                kprintf("%c", (c >= 32 && c < 127) ? c : '.');
            }
            kprintf("\n");
            if (body) kfree(body);
            return;
        }
        kprintf("[NETTEST] %s: attempt %d FAIL rc=%d status=%d len=%u\n", tag, attempt, rc, st, blen);
        if (body) kfree(body);
        proc_sleep(11000);  // outlast the 30s DNS neg-cache across 4 attempts (0/11/22/33s)
    }
    kprintf("[NETTEST] %s: FAIL (all attempts)\n", tag);
}

static void nettest_one(const char *tag, const char *url, int expect_body) {
    nettest_one_n(tag, url, expect_body, 3);
}

// Read a "key=" value from the CFG text into out (stops at CR/LF). Returns 1/0.
static int nettest_cfg_get(const char *cfg, const char *key, char *out, int cap) {
    int klen = 0; while (key[klen]) klen++;
    for (const char *p = cfg; *p; p++) {
        if ((p == cfg || p[-1] == '\n')) {
            int i = 0;
            while (i < klen && p[i] && p[i] == key[i]) i++;
            if (i == klen && p[i] == '=') {
                const char *v = p + klen + 1;
                int n = 0;
                while (*v && *v != '\r' && *v != '\n' && n < cap - 1) out[n++] = *v++;
                out[n] = 0;
                return n > 0;
            }
        }
    }
    return 0;
}

// Read a small unsigned decimal "key=" value; returns the value or `dflt`.
static int nettest_cfg_int(const char *cfg, const char *key, int dflt) {
    char v[32];
    if (!nettest_cfg_get(cfg, key, v, sizeof(v)) || !v[0]) return dflt;
    int n = 0;
    for (int i = 0; v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + (v[i] - '0');
    return n;
}

#define NETTEST_CFG_SETTLE_MS 8000

// #576: cfg-gated Range/partial-content probe for the new https_get_hdr() TLS
// path. `range=<https-url>` issues two Range GETs over TLS and, for a small
// resource, verifies the ranged bytes equal the same slice of a full GET. This
// is the regression proof that the header/Range fetch primitive now speaks TLS
// (returns 206 Partial Content), not just plaintext http. Never runs in
// production: gated on /CONFIG/NETTEST.CFG like the rest of the self-test.
static void nettest_range(const char *url) {
    extern int https_get_hdr(const char *, const char *, uint8_t **, uint32_t *, int *);
    kprintf("[NETTEST] --- range: %s (Range over https) ---\n", url);
    uint8_t *b1 = NULL; uint32_t l1 = 0; int s1 = 0;
    int r1 = https_get_hdr(url, "Range: bytes=0-63\r\n", &b1, &l1, &s1);
    kprintf("[NETTEST] range[0-63]:   rc=%d status=%d len=%u first=", r1, s1, l1);
    for (uint32_t i = 0; i < l1 && i < 32; i++) { char c=(char)b1[i]; kprintf("%c",(c>=32&&c<127)?c:'.'); }
    kprintf("\n");
    uint8_t *b2 = NULL; uint32_t l2 = 0; int s2 = 0;
    int r2 = https_get_hdr(url, "Range: bytes=100-163\r\n", &b2, &l2, &s2);
    kprintf("[NETTEST] range[100-163]: rc=%d status=%d len=%u\n", r2, s2, l2);
    uint8_t *bf = NULL; uint32_t lf = 0; int sf = 0;
    int rf = https_get(url, &bf, &lf, &sf);
    kprintf("[NETTEST] full GET:       rc=%d status=%d len=%u\n", rf, sf, lf);
    int pass206 = (r1==0 && s1==206 && l1>0 && r2==0 && s2==206 && l2>0);
    int match = 0;
    if (rf==0 && bf && lf >= 164 && b2 && l2 >= 64) {
        match = 1;
        for (int i = 0; i < 64; i++) if (bf[100+i] != b2[i]) { match = 0; break; }
    }
    kprintf("[NETTEST] range: %s (206=%d bytematch=%d)\n",
            (pass206 && match) ? "PASS" : "PARTIAL/FAIL", pass206, match);
    if (b1) kfree(b1);
    if (b2) kfree(b2);
    if (bf) kfree(bf);
}

// #615: throughput bench. `dlbench=<url>` + `dlchunks=<n>` issue N sequential
// 256KB Range GETs against `url` and report per-chunk and aggregate wall clock,
// so the per-request overhead is MEASURED, not inferred. Each chunk already
// prints its own [HTTPPROF] phase line.
static void nettest_dlbench(const char *url, int nchunks) {
    extern int https_get_hdr(const char *, const char *, uint8_t **, uint32_t *, int *);
    extern int wget_fetch_hdr(const char *, const char *, uint8_t **, uint32_t *, int *);
    int is_https = (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p'&&url[4]=='s');
    kprintf("[NETTEST] --- dlbench: %d x 256KB Range GET %s ---\n", nchunks, url);
    uint64_t t0 = sched_now_ms();
    uint64_t total = 0;
    int okc = 0;
    for (int i = 0; i < nchunks; i++) {
        char hdr[64];
        uint32_t lo = (uint32_t)i * 262144u, hi = lo + 262143u;
        int n = 0;
        const char *pre = "Range: bytes=";
        for (int k = 0; pre[k]; k++) hdr[n++] = pre[k];
        char tmp[16]; int tn;
        uint32_t v = lo; tn = 0; if (!v) tmp[tn++]='0'; while (v) { tmp[tn++] = (char)('0'+(v%10)); v/=10; }
        while (tn) hdr[n++] = tmp[--tn];
        hdr[n++] = '-';
        v = hi; tn = 0; if (!v) tmp[tn++]='0'; while (v) { tmp[tn++] = (char)('0'+(v%10)); v/=10; }
        while (tn) hdr[n++] = tmp[--tn];
        hdr[n++]='\r'; hdr[n++]='\n'; hdr[n]=0;
        uint8_t *b = NULL; uint32_t bl = 0; int st = 0;
        uint64_t c0 = sched_now_ms();
        int rc = is_https ? https_get_hdr(url, hdr, &b, &bl, &st)
                          : wget_fetch_hdr(url, hdr, &b, &bl, &st);
        uint64_t cms = sched_now_ms() - c0;
        kprintf("[DLBENCH] chunk %d rc=%d st=%d len=%u ms=%u\n", i, rc, st, bl, (uint32_t)cms);
        if (rc == 0 && bl) { total += bl; okc++; }
        if (b) kfree(b);
    }
    uint64_t tms = sched_now_ms() - t0;
    kprintf("[DLBENCH] TOTAL chunks=%d/%d bytes=%u ms=%u => %u KB/s (per-chunk %u ms)\n",
            okc, nchunks, (uint32_t)total, (uint32_t)tms,
            (uint32_t)(tms ? (total / (tms ? tms : 1)) : 0),
            (uint32_t)(okc ? (tms / (uint32_t)okc) : 0));
}

// #615: guest-side https POST probe. `post=<url>` POSTs "{}" with a JSON
// content-type, exactly like the App Store's bump_download(), and reports
// rc/status/body so a silent POST failure is visible without a full install.
static void nettest_post(const char *url) {
    extern int https_post(const char *, const char *, const char *,
                          uint8_t **, uint32_t *, int *);
    kprintf("[NETTEST] --- post: %s ---\n", url);
    uint8_t *b = NULL; uint32_t bl = 0; int st = 0;
    int rc = https_post(url, "Content-Type: application/json\r\n", "{}", &b, &bl, &st);
    kprintf("[NETTEST] post: rc=%d status=%d len=%u body=", rc, st, bl);
    for (uint32_t i = 0; i < bl && i < 160; i++) { char c=(char)b[i]; kprintf("%c",(c>=32&&c<127)?c:'.'); }
    kprintf("\n");
    if (b) kfree(b);
}

static void nettest_worker(void *arg) {
    (void)arg;
    // Settle before READING the gate file: this worker is started from the tail
    // of boot, and /CONFIG lives on the boot volume, which is not necessarily
    // mounted at the instant we start. Reading the CFG first and sleeping after
    // would make the whole self-test silently no-op on a cold boot (the read
    // returns NULL and the worker returns) - which is indistinguishable from
    // "the gate file is absent". Sleep first, then read.
    proc_sleep(NETTEST_CFG_SETTLE_MS);
    uint32_t sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/NETTEST.CFG", &sz);
    if (!cfg) return;    // gate: no-op unless present

    // Defer past the other boot self-tests (SMB/IPP/decoders) so we measure the
    // h2 path under a representative (desktop-widgets-only) load, not the boot
    // pile-on that transiently starves the single NIC. `delay=<ms>` overrides
    // it so an interactive diagnosis run does not wait 50s every iteration.
    // Default total stays the historical ~50s.
    {
        int delay = nettest_cfg_int(cfg, "delay", 50000);
        if (delay > NETTEST_CFG_SETTLE_MS)
            proc_sleep((uint32_t)(delay - NETTEST_CFG_SETTLE_MS));
    }

    kprintf_set_dual_output(1);
    kprintf("\n========== NET SELFTEST (#333) START ==========\n");

    // #497/#333 diagnosis harness: `u1=`..`u6=` run arbitrary URLs with the h2
    // + TLS frame traces on, so a real public host (feeds.bbci.co.uk, xkcd) can
    // be retargeted from the CFG on the boot volume WITHOUT a kernel rebuild.
    // `only=1` skips the fixed built-in host list; `tries=` bounds the retries
    // (1 keeps a trace readable). Both default to the historical behavior.
    int only = nettest_cfg_int(cfg, "only", 0);
    int tries = nettest_cfg_int(cfg, "tries", 3);
    if (nettest_cfg_int(cfg, "tlsdbg", 0)) g_tls_dbg = 1;
    {
        char rng[512];
        if (nettest_cfg_get(cfg, "range", rng, sizeof(rng)) && rng[0])
            nettest_range(rng);   // #576
    }
    {   // #615 throughput bench + POST probe
        char u[512];
        if (nettest_cfg_get(cfg, "dlbench", u, sizeof(u)) && u[0])
            nettest_dlbench(u, nettest_cfg_int(cfg, "dlchunks", 6));
        if (nettest_cfg_get(cfg, "dlbench2", u, sizeof(u)) && u[0])
            nettest_dlbench(u, nettest_cfg_int(cfg, "dlchunks", 6));
        if (nettest_cfg_get(cfg, "post", u, sizeof(u)) && u[0])
            nettest_post(u);
    }
    if (only) {
        g_http2_dbg = 1;
        static const char *keys[] = { "u1", "u2", "u3", "u4", "u5", "u6" };
        for (int i = 0; i < 6; i++) {
            char u[512];
            if (!nettest_cfg_get(cfg, keys[i], u, sizeof(u)) || !u[0]) continue;
            nettest_one_n(keys[i], u, 1, tries);
        }
        g_http2_dbg = 0; g_tls_dbg = 0;
        kprintf("========== NET SELFTEST (#333) END ==========\n");
        kprintf_set_dual_output(0);
        kfree(cfg);
        return;
    }

    // Bug-2 FIRST: fresh same-subnet LAN host (never contacted -> exercises the
    // ARP warm-up + connect retry). Run before the internet tests so it is not
    // starved by their retries.
    char lan[256];
    if (nettest_cfg_get(cfg, "lan", lan, sizeof(lan)) && lan[0]) {
        nettest_one("lan-arp", lan, 0);
    } else {
        kprintf("[NETTEST] lan-arp: skipped (no lan= line in CFG)\n");
    }

    g_http2_dbg = 1;
    // Known-good h2 host for comparison.
    nettest_one("coingecko-h2",
                "https://api.coingecko.com/api/v3/ping", 1);
    // Bug-1 repro hosts: musicbrainz + archive.org over HTTP/2.
    nettest_one("musicbrainz-h2",
                "https://musicbrainz.org/ws/2/artist/?query=radiohead&fmt=json&limit=1", 1);
    nettest_one("archiveorg-h2",
                "https://archive.org/metadata/principleofrelativ00eins", 1);
    g_http2_dbg = 0;

    // #502: TLS 1.2 live coverage. Both hosts are 1.2-MAX (forcing 1.3 on them
    // returns a fatal protocol_version alert), and each exercises a different
    // half of the 1.2 code:
    //   xkcd.com  -> 0xc02f, AES-128-GCM, SHA-256 PRF, x25519 ECDHE
    //   hnrss.org -> 0xc030, AES-256-GCM, SHA-384 PRF, secp384r1 ECDHE
    // Both sign ServerKeyExchange with RSA-PSS and both use RFC 7627 extended
    // master secret, so between them they cover every branch that matters. They
    // are here so a 1.2 regression is caught by the self-test rather than by a
    // user noticing the RSS feeds are empty.
    nettest_one("xkcd-tls12",  "https://xkcd.com/rss.xml", 0);
    nettest_one("hnrss-tls12", "https://hnrss.org/frontpage", 0);

    kprintf("========== NET SELFTEST (#333) END ==========\n");
    kprintf_set_dual_output(0);
    kfree(cfg);
}

void nettest_start_deferred_selftest(void) {
    extern int proc_create_ex(const char *name, void (*entry)(void *), void *arg,
                              int priority, uint32_t stack_size);
    proc_create_ex("nettest", nettest_worker, 0, 1, 256 * 1024);
}

// ============================================================================
// #404 Phase Y: HTTP response length-parse Rust seam boot-time self-test.
// Proves the routed Rust seams (https_dechunk_rs, https_chunk_complete_rs,
// http_decode_chunked_rs, http_find_header_end_rs, https_content_length_rs,
// http_parse_uint_rs) == their verbatim C references on well-formed HTTP
// responses ([RUST-DIFF]), witnesses the REACHABLE https_dechunk OOB confinement
// ([RUST-SEC] MAYTERA-SEC-2026-0008), and micro-benchmarks the decoder
// ([RUST-PERF]). Bounded, runs once at boot (#426, no busy-wait).
// ============================================================================
extern long   http_decode_chunked_rs(uint8_t *buf, unsigned long len);
extern unsigned long http_parse_uint_rs(const uint8_t *s, unsigned long len);

static inline uint64_t hparse_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// Verbatim size_t reference of net/wget.c http_decode_chunked (for the diff).
static long hdc_ref(uint8_t *buf, unsigned long len) {
    uint8_t *read_ptr = buf, *write_ptr = buf, *end = buf + len;
    unsigned long total = 0;
    while (read_ptr < end) {
        unsigned long cs = 0; int any = 0; uint8_t *p = read_ptr;
        while (p < end) { uint8_t c = *p; int d;
            if (c>='0'&&c<='9') d=c-'0'; else if (c>='a'&&c<='f') d=c-'a'+10;
            else if (c>='A'&&c<='F') d=c-'A'+10; else break;
            cs = cs*16 + (unsigned long)d; any = 1; p++; }
        if (!any) return -1;
        while (p < end && *p != '\n') p++;
        if (p >= end) return -1;
        p++; read_ptr = p;
        if (cs == 0) return (long)total;
        if ((unsigned long)(end - read_ptr) < cs) return -1;
        if (write_ptr != read_ptr) memmove(write_ptr, read_ptr, cs);
        write_ptr += cs; read_ptr += cs; total += cs;
        if (read_ptr < end && *read_ptr == '\r') read_ptr++;
        if (read_ptr < end && *read_ptr == '\n') read_ptr++;
    }
    return -1;
}
// Verbatim size_t reference of net/wget.c parse_uint.
static unsigned long pu_ref(const uint8_t *s, unsigned long len) {
    unsigned long v = 0, i = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') { v = v*10 + (s[i]-'0'); i++; }
    return v;
}

void http_parse_rust_selftest(void) {
    uint32_t vectors = 0, mism = 0; int first_bad = -1;

    // ---- [RUST-DIFF]: well-formed corpus (chunked bodies, CL values, headers) ----
    // A: chunked decode diff over a set of well-formed chunked bodies (hex sizes,
    // multi-chunk, extensions, upper/lower hex, exact terminators).
    static const char *chunks[] = {
        "4\r\nWiki\r\n5\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\n\r\n",
        "1a\r\nabcdefghijklmnopqrstuvwxyz\r\n0\r\n\r\n",
        "0\r\n\r\n",
        "3\r\nabc\r\n0\r\n\r\n",
        "A\r\n0123456789\r\n7;ext=1\r\n1234567\r\n0\r\n\r\n",
        "ff\r\n%0255c%0255c\r\n0\r\n\r\n",  // 0xff filled below
    };
    uint8_t rb[512], cb[512];
    for (unsigned t = 0; t < sizeof(chunks)/sizeof(chunks[0]); t++) {
        unsigned long L = 0; while (chunks[t][L]) L++;
        // build a well-formed 255-byte chunk for the last case deterministically
        if (t == 5) {
            unsigned long n = 0; const char *hdr = "ff\r\n"; while (hdr[n]) { cb[n]=hdr[n]; n++; }
            for (int k = 0; k < 255; k++) cb[n++] = (uint8_t)('A' + (k % 26));
            const char *tl = "\r\n0\r\n\r\n"; unsigned long m=0; while (tl[m]) cb[n++]=tl[m++];
            L = n; for (unsigned long j=0;j<L;j++) rb[j]=cb[j];
        } else {
            for (unsigned long j=0;j<L;j++){ rb[j]=(uint8_t)chunks[t][j]; cb[j]=(uint8_t)chunks[t][j]; }
        }
        long rr = http_decode_chunked_rs(rb, L);
        long cc = hdc_ref(cb, L);
        vectors++;
        int bad = (rr != cc);
        if (!bad && rr >= 0) { for (long j=0;j<rr;j++) if (rb[j]!=cb[j]) { bad=1; break; } }
        // also exercise the https_dechunk (uint32) pair on the same bytes
        for (unsigned long j=0;j<L;j++){ rb[j]=(t==5)?cb[j]:(uint8_t)chunks[t][j]; cb[j]=rb[j]; }
        uint32_t r2 = https_dechunk_rs(rb, (uint32_t)L);
        uint32_t c2 = https_dechunk_c(cb, (uint32_t)L);
        if (r2 != c2) bad = 1;
        else for (uint32_t j=0;j<r2;j++) if (rb[j]!=cb[j]) { bad=1; break; }
        // and the completeness gate
        for (unsigned long j=0;j<L;j++) rb[j]=(t==5)?cb[j]:(uint8_t)chunks[t][j];
        if (https_chunk_complete_rs(rb,(uint32_t)L) != https_chunked_is_complete_c(rb,(uint32_t)L)) bad=1;
        if (bad) { mism++; if (first_bad<0) first_bad=(int)vectors-1; }
    }
    // B: partial/incremental chunked frames (gate must agree at each prefix).
    {
        const char *full = "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
        unsigned long FL = 0; while (full[FL]) FL++;
        for (unsigned long pre = 0; pre <= FL; pre++) {
            uint8_t tb[64]; for (unsigned long j=0;j<pre;j++) tb[j]=(uint8_t)full[j];
            int r = https_chunk_complete_rs(tb,(uint32_t)pre);
            int c = https_chunked_is_complete_c(tb,(uint32_t)pre);
            vectors++; if (r!=c) { mism++; if (first_bad<0) first_bad=(int)vectors-1; }
        }
    }
    // C: header-block framing + Content-Length + parse_uint diff.
    {
        static const char *heads[] = {
            "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\nContent-Type: text/html\r\n\r\nBODY",
            "HTTP/1.1 204 No Content\r\n\r\n",
            "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
            "no header end here at all",
            "HTTP/1.1 200 OK\r\nContent-Length:   4294967000  \r\n\r\n",
        };
        for (unsigned t=0;t<sizeof(heads)/sizeof(heads[0]);t++) {
            unsigned long L=0; while (heads[t][L]) L++;
            uint8_t hb[256]; for (unsigned long j=0;j<L;j++) hb[j]=(uint8_t)heads[t][j]; hb[L]=0;
            vectors++;
            if (http_find_header_end_rs(hb,(uint32_t)L) != https_find_header_end_c(hb,(uint32_t)L)) { mism++; if(first_bad<0)first_bad=(int)vectors-1; continue; }
            if (https_content_length_rs(hb,(uint32_t)L) != https_content_length_c((const char*)hb)) { mism++; if(first_bad<0)first_bad=(int)vectors-1; continue; }
            // parse_uint on a synthetic digit run
            const char *digs = "4294967000rest";
            if (http_parse_uint_rs((const uint8_t*)digs, 14) != pu_ref((const uint8_t*)digs,14)) { mism++; if(first_bad<0)first_bad=(int)vectors-1; }
        }
    }
    const char *verdict = (mism==0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] http_parse: %u vectors (chunked+gate+hdr+CL), %u mismatches -> %s\n", vectors, mism, verdict);
    bootlog_write("[RUST-DIFF] http_parse: %u vectors, %u mismatches -> %s", vectors, mism, verdict);
    if (mism) { kprintf("[RUST-DIFF] http_parse FIRST MISMATCH vector=%d\n", first_bad);
                bootlog_write("[RUST-DIFF] http_parse FIRST MISMATCH vector=%d", first_bad); }

    // ---- [RUST-SEC]: the crafted near-2^32 chunk-size OOB, confined by Rust ----
    // "FFFFFFFE\r\n0\r\n\r\n": https_chunked_is_complete_c returns 1 (its p+sz
    // wraps to a clean terminator) so the C https_dechunk_c would run and its
    // `in+sz>len` wraps -> memmove(~4GiB) OOB (ASan-proven OFFLINE). The Rust
    // gate rejects it (NEED_MORE) and the Rust decoder clamps: both memory-safe.
    {
        const char *evil = "FFFFFFFE\r\n0\r\n\r\n";
        unsigned long L=0; while (evil[L]) L++;
        uint8_t eb[32]; for (unsigned long j=0;j<L;j++) eb[j]=(uint8_t)evil[j];
        int c_gate = https_chunked_is_complete_c(eb,(uint32_t)L);   // == 1 (reaches decoder)
        int r_gate = https_chunk_complete_rs(eb,(uint32_t)L);       // confined: != 1
        // Run the Rust decoder on the evil buffer: must stay in-bounds (out<=len).
        uint8_t db[32]; for (unsigned long j=0;j<L;j++) db[j]=(uint8_t)evil[j];
        uint32_t r_out = https_dechunk_rs(db,(uint32_t)L);
        int confined = (r_gate != 1) && (r_out <= (uint32_t)L);
        kprintf("[RUST-SEC] http_parse: https_dechunk u32 overflow OOB (MAYTERA-SEC-2026-0008) "
                "C-gate=%d reaches vulnerable memmove; Rust gate=%d out=%u -> confined=%s; #504\n",
                c_gate, r_gate, r_out, confined ? "OK" : "FAIL");
        bootlog_write("[RUST-SEC] http_parse: https_dechunk u32-overflow OOB reachable in C "
                      "(MAYTERA-SEC-2026-0008); Rust confines (gate=%d out=%u) -> %s; #504",
                      r_gate, r_out, confined ? "OK" : "FAIL");
    }

    // ---- [RUST-PERF]: RDTSC over the chunked decoder (2000 iters, warm) ----
    {
        const int iters = 2000;
        uint8_t src[300]; unsigned long n=0; const char *h="ff\r\n"; while(h[n]){src[n]=h[n];n++;}
        for (int k=0;k<255;k++) src[n++]=(uint8_t)('A'+(k%26));
        const char *tl="\r\n0\r\n\r\n"; unsigned long m=0; while(tl[m]) src[n++]=tl[m++];
        unsigned long SL=n; uint8_t work[300];
        for (int i=0;i<200;i++){
            for(unsigned long j=0;j<SL;j++) work[j]=src[j];
            https_dechunk_c(work,(uint32_t)SL);
            for(unsigned long j=0;j<SL;j++) work[j]=src[j];
            https_dechunk_rs(work,(uint32_t)SL);
        }
        uint64_t t0=hparse_tsc();
        for (int i=0;i<iters;i++){ for(unsigned long j=0;j<SL;j++)work[j]=src[j]; https_dechunk_c(work,(uint32_t)SL); }
        uint64_t t1=hparse_tsc();
        for (int i=0;i<iters;i++){ for(unsigned long j=0;j<SL;j++)work[j]=src[j]; https_dechunk_rs(work,(uint32_t)SL); }
        uint64_t t2=hparse_tsc();
        uint64_t c_cyc=(t1-t0)/iters, r_cyc=(t2-t1)/iters;
        uint64_t ratio100 = c_cyc ? (r_cyc*100ULL/c_cyc) : 0;
        kprintf("[RUST-PERF] http_parse (dechunk 255B): C=%llu cyc RS=%llu cyc ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
        bootlog_write("[RUST-PERF] http_parse: C=%llu RS=%llu cyc/decode ratio=%llu.%02llu",
                (unsigned long long)c_cyc,(unsigned long long)r_cyc,
                (unsigned long long)(ratio100/100),(unsigned long long)(ratio100%100));
    }
}


// ===========================================================================
// #616 boot self-test for the TLS connection pool (rustkern/tlspool.rs).
//
// TWO parts, deliberately different in kind:
//   [RUST-DIFF] tlspool  - the response-header reuse POLICY has a C reference
//                          twin (http_reuse_ok_c), so it gets a real
//                          differential over well-formed AND hostile blocks
//                          plus in-kernel byte mutations.
//   [RUST-POOL] tlspool  - the pool state machine is NEW logic with no C twin,
//                          so a differential would prove nothing (both arms
//                          would share any design bug). It is checked against
//                          its INVARIANTS instead: exclusive checkout,
//                          exact-match lookup, LRU eviction, idle expiry,
//                          oversize refusal, no-double-store.
// ===========================================================================
void tlspool_rust_selftest(void) {
    static const char *hdrs[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n",
        "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 200 OK\r\nconnection: CLOSE\r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\n",
        "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\n",
        "HTTP/1.1 200 OK\r\nConnection: keep-alive, close\r\n\r\n",
        "HTTP/1.1 200 OK\r\nConnection: Keep-Alive\r\n\r\n",
        "HTTP/1.1 200 OK\r\nProxy-Connection: close\r\n\r\n",
        "HTTP/1.1 200 OK\r\nX-Closer: close\r\n\r\n",
        "HTTP/1.1 200 OK\r\nConnection\r\n\r\n",
        "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-262143/108000000\r\nContent-Length: 262144\r\n\r\n",
        "HTTP/2 200\r\n\r\n",
        "HTTP/1.1 200 OK\n\n",
        "HTTP/1.1\r\n",
        "garbage with no version at all\r\n\r\n",
        "HTTP/1.1 200 OK\r\nConnection:\r\n\r\n",
        "HTTP/1.1 200 OK\r\nConnection: ,,, close ,,,\r\n\r\n",
        "HTTP/1.1 200 OK\r\nConnection: closer\r\n\r\n",
        "",
    };
    uint32_t vec = 0, mism = 0; int firstbad = -1;
    uint8_t mb[320];
    for (unsigned t = 0; t < sizeof(hdrs)/sizeof(hdrs[0]); t++) {
        uint32_t L = 0; while (hdrs[t][L] && L < sizeof(mb)) { mb[L] = (uint8_t)hdrs[t][L]; L++; }
        vec++;
        if (http_reuse_ok_rs(mb, L) != http_reuse_ok_c(mb, L)) { mism++; if (firstbad<0) firstbad=(int)vec-1; }
        for (uint32_t p = 0; p <= L; p++) {
            vec++;
            if (http_reuse_ok_rs(mb, p) != http_reuse_ok_c(mb, p)) { mism++; if (firstbad<0) firstbad=(int)vec-1; }
        }
        for (uint32_t k = 0; k < L; k++) {
            uint8_t save = mb[k];
            static const uint8_t muts[5] = { 0x00, 0x0d, 0x0a, 0x3a, 0xff };
            for (int m = 0; m < 5; m++) {
                mb[k] = muts[m];
                vec++;
                if (http_reuse_ok_rs(mb, L) != http_reuse_ok_c(mb, L)) { mism++; if (firstbad<0) firstbad=(int)vec-1; }
            }
            mb[k] = save;
        }
    }
    kprintf("[RUST-DIFF] tlspool reuse-policy %u vectors %u mism %s (firstbad=%d)\n",
            vec, mism, mism ? "FAIL" : "PASS", firstbad);

    https_pool_slot_t sp[3];
    for (int i = 0; i < 3; i++) {
        sp[i].conn = 0; sp[i].last_used_ms = 0; sp[i].port = 0;
        sp[i].flags = 0; sp[i].hostlen = 0; sp[i].pad = 0;
        for (int j = 0; j < 128; j++) sp[i].host[j] = 0;
    }
    int inv = 0, invfail = 0;
#define POOLCHK(cond) do { inv++; if (!(cond)) { invfail++; \
    kprintf("[RUST-POOL] INVARIANT FAILED at line %d\n", __LINE__); } } while (0)

    uint64_t ev = 0, got = 0;
    const char *ha = "a.example.com", *hb = "b.example.com", *hc = "c.example.com";
    POOLCHK(tlspool_store_rs(sp, 3, (const uint8_t*)ha, 13, 443, 0, 0x1111, 1000, &ev) == 1 && ev == 0);
    POOLCHK(tlspool_store_rs(sp, 3, (const uint8_t*)hb, 13, 443, 0, 0x2222, 1100, &ev) == 1 && ev == 0);
    POOLCHK(tlspool_store_rs(sp, 3, (const uint8_t*)hc, 13, 443, 0, 0x3333, 1200, &ev) == 1 && ev == 0);
    POOLCHK(tlspool_count_rs(sp, 3) == 3);
    POOLCHK(tlspool_acquire_rs(sp, 3, (const uint8_t*)hb, 13, 443, 0, 1300, 20000, &got) == 1 && got == 0x2222);
    POOLCHK(tlspool_count_rs(sp, 3) == 2);
    POOLCHK(tlspool_acquire_rs(sp, 3, (const uint8_t*)hb, 13, 443, 0, 1300, 20000, &got) == 0 && got == 0);
    POOLCHK(tlspool_store_rs(sp, 3, (const uint8_t*)hb, 13, 443, 0, 0x2222, 1300, &ev) == 1);
    POOLCHK(tlspool_acquire_rs(sp, 3, (const uint8_t*)hb, 12, 443, 0, 1300, 20000, &got) == 0);
    POOLCHK(tlspool_acquire_rs(sp, 3, (const uint8_t*)"b.example.comX", 14, 443, 0, 1300, 20000, &got) == 0);
    POOLCHK(tlspool_acquire_rs(sp, 3, (const uint8_t*)hb, 13, 8443, 0, 1300, 20000, &got) == 0);
    POOLCHK(tlspool_acquire_rs(sp, 3, (const uint8_t*)hb, 13, 443, 1, 1300, 20000, &got) == 0);
    POOLCHK(tlspool_count_rs(sp, 3) == 3);
    ev = 0;
    POOLCHK(tlspool_store_rs(sp, 3, (const uint8_t*)"d.example.com", 13, 443, 0, 0x4444, 1400, &ev) == 1
            && ev == 0x1111);
    POOLCHK(tlspool_count_rs(sp, 3) == 3);
    { uint8_t big[200]; for (int i = 0; i < 200; i++) big[i] = 'x';
      ev = 0;
      POOLCHK(tlspool_store_rs(sp, 3, big, 200, 443, 0, 0x5555, 1500, &ev) == 0 && ev == 0); }
    POOLCHK(tlspool_store_rs(sp, 3, (const uint8_t*)hc, 13, 443, 0, 0x3333, 1600, &ev) == 1);
    { int seen = 0; for (int i = 0; i < 3; i++) if (sp[i].conn == 0x3333) seen++;
      POOLCHK(seen == 1); }
    POOLCHK(tlspool_acquire_rs(sp, 3, (const uint8_t*)hc, 13, 443, 0, 1600 + 20001, 20000, &got) == 0);
    { int reaped = 0; uint64_t d = 0;
      while (tlspool_reap_rs(sp, 3, 1600 + 20001, 20000, &d) == 1 && d) { reaped++; d = 0; }
      POOLCHK(reaped >= 1);
      POOLCHK(tlspool_count_rs(sp, 3) == 0); }
    POOLCHK(tlspool_store_rs(sp, 3, (const uint8_t*)ha, 13, 443, 0, 0x6666, 2000, &ev) == 1);
    POOLCHK(tlspool_drop_rs(sp, 3, 0x6666) == 1);
    POOLCHK(tlspool_drop_rs(sp, 3, 0x6666) == 0);
    POOLCHK(tlspool_count_rs(sp, 3) == 0);
    POOLCHK(tlspool_acquire_rs((https_pool_slot_t*)0, 3, (const uint8_t*)ha, 13, 443, 0, 1, 1, &got) == 0);
    POOLCHK(tlspool_store_rs(sp, 0, (const uint8_t*)ha, 13, 443, 0, 0x7777, 1, &ev) == 0);
    POOLCHK(tlspool_store_rs(sp, 3, (const uint8_t*)ha, 0, 443, 0, 0x7777, 1, &ev) == 0);
    POOLCHK(tlspool_store_rs(sp, 3, (const uint8_t*)ha, 13, 443, 0, 0, 1, &ev) == 0);
#undef POOLCHK

    kprintf("[RUST-POOL] tlspool invariants %d checks %d fail %s\n",
            inv, invfail, invfail ? "FAIL" : "PASS");
}
