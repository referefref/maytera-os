// tcp.c - Transmission Control Protocol implementation
#include "tcp.h"
#include "ip.h"
#include "../serial.h"
#include "../string.h"

// #487: socket-ownership stamping needs only the current pid, not the PCB.
extern uint32_t proc_current_pid(void);

// External timer ticks (from ISR)
extern volatile uint64_t timer_ticks;
extern void net_lock(void);   // #297: global net serialization
extern void net_unlock(void);

// Per-segment TX/RX serial tracing. OFF by default: it printed 2 synchronous
// COM1 lines per packet (hundreds/sec under load), throttling throughput.
int g_tcp_dbg = 0;

// Connection table
static tcp_conn_t connections[TCP_MAX_CONNECTIONS];

// Next ephemeral port
static uint16_t next_ephemeral_port = 49152;

// Byte swap functions
static inline uint16_t htons(uint16_t h) {
    return ((h & 0xFF) << 8) | ((h >> 8) & 0xFF);
}

static inline uint16_t ntohs(uint16_t n) {
    return htons(n);
}

static inline uint32_t htonl(uint32_t h) {
    return ((h & 0xFF) << 24) | ((h & 0xFF00) << 8) |
           ((h >> 8) & 0xFF00) | ((h >> 24) & 0xFF);
}

static inline uint32_t ntohl(uint32_t n) {
    return htonl(n);
}

// Simple pseudo-random number generator for initial sequence numbers
static uint32_t tcp_rand_state = 12345;

static uint32_t tcp_rand(void) {
    tcp_rand_state = tcp_rand_state * 1103515245 + 12345;
    return tcp_rand_state;
}

// Generate initial sequence number
static uint32_t tcp_generate_isn(void) {
    // Use timer ticks + random for ISN
    return (uint32_t)timer_ticks * 250000 + tcp_rand();
}

// ---------------------------------------------------------------------------
// TCP checksum (#404 / #486 Phase D): Rust strangler.
//
// The Rust port lives in rustkern.rs. It is a drop-in for tcp_checksum_c below:
// same RFC 1071 ones-complement result over the IPv4 pseudo-header + segment,
// proven byte-for-byte identical on THIS build by tcp_checksum_rust_selftest().
// The src_ip / dest_ip args arrive in network byte order (callers pass htonl()).
extern uint16_t tcp_checksum_rs(uint32_t src_ip, uint32_t dest_ip,
                                const void *tcp_data, uint16_t tcp_length);

// Original C implementation, renamed tcp_checksum_c so BOTH the C and the Rust
// versions stay callable in the same build (trivial rollback + differential
// reference). Static: file-local, referenced by the wrapper + the self-test.
static uint16_t tcp_checksum_c(uint32_t src_ip, uint32_t dest_ip,
                               const void *tcp_data, uint16_t tcp_length) {
    uint32_t sum = 0;
    const uint16_t *ptr;

    // Add pseudo-header
    // Source IP (in network byte order)
    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;

    // Destination IP (in network byte order)
    sum += (dest_ip >> 16) & 0xFFFF;
    sum += dest_ip & 0xFFFF;

    // Protocol (6 = TCP)
    sum += htons(IP_PROTO_TCP);

    // TCP length
    sum += htons(tcp_length);

    // Add TCP header and data
    ptr = (const uint16_t *)tcp_data;
    while (tcp_length > 1) {
        sum += *ptr++;
        tcp_length -= 2;
    }

    // Add odd byte if present
    if (tcp_length > 0) {
        sum += *(const uint8_t *)ptr;
    }

    // Fold 32-bit sum to 16 bits
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

// Live callers still call tcp_checksum(). With -DRUST_TCP_CHECKSUM (set in the
// Makefile CFLAGS for build 795) the real symbol routes to the Rust port;
// otherwise it falls straight back to the C. Rollback = drop the flag, rebuild.
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dest_ip,
                             const void *tcp_data, uint16_t tcp_length) {
#ifdef RUST_TCP_CHECKSUM
    return tcp_checksum_rs(src_ip, dest_ip, tcp_data, tcp_length);
#else
    return tcp_checksum_c(src_ip, dest_ip, tcp_data, tcp_length);
#endif
}

// Boot-time differential self-test (#404 / #486 Phase D). Asserts
// tcp_checksum_rs == tcp_checksum_c over a bounded corpus (every payload length
// 0..1500 once + a few thousand random-length/random-content/random-IP vectors
// + real SYN header samples) on THIS build, logs ONE [RUST-DIFF] line to serial
// and /BOOTLOG. Bounded, runs once, no busy-wait (#426). It references
// tcp_checksum_rs unconditionally so the Rust archive member is always linked.
static inline uint32_t tcp_rustdiff_rng(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

void tcp_checksum_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    static uint8_t buf[1500];

    uint32_t seed = 0x7c9e2b15;
    uint32_t vectors = 0, mismatches = 0;
    int first_bad_len = -1;
    uint16_t first_bad_rs = 0, first_bad_c = 0;

    // Pass 1: every length 0..1500 once, random src/dst IP + random content
    // (covers odd/even trailing-byte + carry folding at every length boundary).
    for (int len = 0; len <= 1500; len++) {
        for (int i = 0; i < len; i++) {
            buf[i] = (uint8_t)(tcp_rustdiff_rng(&seed) & 0xFF);
        }
        uint32_t s = tcp_rustdiff_rng(&seed);
        uint32_t d = tcp_rustdiff_rng(&seed);
        uint16_t rs = tcp_checksum_rs(s, d, buf, (uint16_t)len);
        uint16_t c = tcp_checksum_c(s, d, buf, (uint16_t)len);
        vectors++;
        if (rs != c) {
            mismatches++;
            if (first_bad_len < 0) { first_bad_len = len; first_bad_rs = rs; first_bad_c = c; }
        }
    }

    // Pass 2: random length in [0,1500] with fresh random content + IPs.
    for (int n = 0; n < 4096; n++) {
        int len = (int)(tcp_rustdiff_rng(&seed) % 1501);
        for (int i = 0; i < len; i++) {
            buf[i] = (uint8_t)(tcp_rustdiff_rng(&seed) & 0xFF);
        }
        uint32_t s = tcp_rustdiff_rng(&seed);
        uint32_t d = tcp_rustdiff_rng(&seed);
        uint16_t rs = tcp_checksum_rs(s, d, buf, (uint16_t)len);
        uint16_t c = tcp_checksum_c(s, d, buf, (uint16_t)len);
        vectors++;
        if (rs != c) {
            mismatches++;
            if (first_bad_len < 0) { first_bad_len = len; first_bad_rs = rs; first_bad_c = c; }
        }
    }

    // Pass 3: real 20-byte TCP SYN header samples at a range of payload sizes.
    {
        static const uint8_t syn[20] = {
            0xC0, 0x1F, 0x00, 0x50, 0x12, 0x34, 0x56, 0x78,
            0x00, 0x00, 0x00, 0x00, 0x50, 0x02, 0x72, 0x10,
            0x00, 0x00, 0x00, 0x00
        };
        for (int i = 0; i < 20; i++) buf[i] = syn[i];
        int sizes[6] = { 20, 21, 40, 100, 513, 1460 };
        uint32_t s = htonl(0xC0A8010A); // a private-range test address
        uint32_t d = htonl(0xC0A80102); // a private-range test address
        for (int k = 0; k < 6; k++) {
            int len = sizes[k];
            for (int i = 20; i < len; i++) buf[i] = (uint8_t)(0x40 + (i & 0x3F));
            uint16_t rs = tcp_checksum_rs(s, d, buf, (uint16_t)len);
            uint16_t c = tcp_checksum_c(s, d, buf, (uint16_t)len);
            vectors++;
            if (rs != c) {
                mismatches++;
                if (first_bad_len < 0) { first_bad_len = len; first_bad_rs = rs; first_bad_c = c; }
            }
        }
    }

    const char *verdict = (mismatches == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] tcp_checksum: %u vectors, %u mismatches -> %s\n",
            vectors, mismatches, verdict);
    bootlog_write("[RUST-DIFF] tcp_checksum: %u vectors, %u mismatches -> %s",
                  vectors, mismatches, verdict);
    if (mismatches != 0) {
        kprintf("[RUST-DIFF] tcp FIRST MISMATCH len=%d rs=0x%04x c=0x%04x\n",
                first_bad_len, first_bad_rs, first_bad_c);
        bootlog_write("[RUST-DIFF] tcp FIRST MISMATCH len=%d rs=0x%04x c=0x%04x",
                      first_bad_len, first_bad_rs, first_bad_c);
    }
}

// Find connection by local port, remote IP, and remote port
static tcp_conn_t* tcp_find_connection(uint16_t local_port, uint32_t remote_ip,
                                        uint16_t remote_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *conn = &connections[i];
        if (conn->active &&
            conn->local_port == local_port &&
            conn->remote_ip == remote_ip &&
            conn->remote_port == remote_port) {
            return conn;
        }
    }
    return NULL;
}

// Find a listening socket for a port
static tcp_conn_t* tcp_find_listener(uint16_t local_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *conn = &connections[i];
        if (conn->active &&
            conn->is_listener &&
            conn->local_port == local_port &&
            conn->state == TCP_STATE_LISTEN) {
            return conn;
        }
    }
    return NULL;
}

// Get connection by socket descriptor
static tcp_conn_t* tcp_get_conn(int sock) {
    if (sock < 0 || sock >= TCP_MAX_CONNECTIONS) {
        return NULL;
    }
    tcp_conn_t *conn = &connections[sock];
    if (!conn->active) {
        return NULL;
    }
    return conn;
}

// Allocate a new connection slot
static int tcp_alloc_conn(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) {
            memset(&connections[i], 0, sizeof(tcp_conn_t));
            connections[i].active = 1;
            connections[i].state = TCP_STATE_CLOSED;
            connections[i].rcv_wnd = TCP_INITIAL_WINDOW;
            connections[i].snd_wnd = TCP_INITIAL_WINDOW;
            return i;
        }
    }
    // Pool full: reclaim a connection that is effectively dead (lingering close
    // states or one whose owner process died without closing it cleanly).
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        int st = connections[i].state;
        if (st == TCP_STATE_TIME_WAIT || st == TCP_STATE_FIN_WAIT_2 ||
            st == TCP_STATE_CLOSING   || st == TCP_STATE_CLOSED) {
            kprintf("[TCP] pool full: reclaiming dead socket %d (state %d)\n", i, st);
            memset(&connections[i], 0, sizeof(tcp_conn_t));
            connections[i].active = 1;
            connections[i].state = TCP_STATE_CLOSED;
            connections[i].rcv_wnd = TCP_INITIAL_WINDOW;
            connections[i].snd_wnd = TCP_INITIAL_WINDOW;
            return i;
        }
    }
    return -1;
}

// Allocate an ephemeral port
static uint16_t tcp_alloc_port(void) {
    uint16_t start = next_ephemeral_port;
    do {
        uint16_t port = next_ephemeral_port++;
        if (next_ephemeral_port >= 65535) {
            next_ephemeral_port = 49152;
        }

        // Check if port is in use
        int in_use = 0;
        for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
            if (connections[i].active && connections[i].local_port == port) {
                in_use = 1;
                break;
            }
        }

        if (!in_use) {
            return port;
        }
    } while (next_ephemeral_port != start);

    return 0; // No ports available
}

// Send a TCP segment
static int tcp_send_segment(tcp_conn_t *conn, uint8_t flags,
                            const void *data, uint16_t data_len) {
    static uint8_t packet[TCP_MSS + 60];  // Room for TCP header + options
    tcp_header_t *header = (tcp_header_t *)packet;

    uint16_t header_len = 20;  // No options for now
    uint16_t total_len = header_len + data_len;

    if (total_len > sizeof(packet)) {
        return -1;
    }

    // Build TCP header
    header->src_port = htons(conn->local_port);
    header->dest_port = htons(conn->remote_port);
    header->seq_num = htonl(conn->snd_nxt);
    header->ack_num = htonl(conn->rcv_nxt);
    header->data_offset = (header_len / 4) << 4;  // Upper 4 bits
    header->flags = flags;
    header->window = htons((uint16_t)conn->rcv_wnd);
    header->checksum = 0;
    header->urgent_ptr = 0;

    // Copy data if present
    if (data && data_len > 0) {
        memcpy(packet + header_len, data, data_len);
    }

    // Calculate checksum
    uint32_t our_ip = ip_get_address();
    header->checksum = tcp_checksum(htonl(our_ip), htonl(conn->remote_ip),
                                    packet, total_len);

    // Update sequence number for data sent
    if (data_len > 0) {
        conn->snd_nxt += data_len;
    }
    // SYN and FIN consume one sequence number
    if (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) {
        conn->snd_nxt++;
    }

    // Record send time for retransmission
    conn->last_send_time = timer_ticks;

    // Send via IP
    if (g_tcp_dbg) {
        kprintf("[TCP] Sending segment: flags=0x%02x seq=%u ack=%u len=%d to ",
                flags, ntohl(header->seq_num), ntohl(header->ack_num), data_len);
        ip_print(conn->remote_ip);
        kprintf(":%d\n", conn->remote_port);
    }

    return ip_send(conn->remote_ip, IP_PROTO_TCP, packet, total_len);
}

// Send RST segment (stateless)
static void tcp_send_rst(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
                         uint32_t seq_num, uint32_t ack_num) {
    static uint8_t packet[20];
    tcp_header_t *header = (tcp_header_t *)packet;

    header->src_port = htons(src_port);
    header->dest_port = htons(dest_port);
    header->seq_num = htonl(seq_num);
    header->ack_num = htonl(ack_num);
    header->data_offset = 0x50;  // 5 32-bit words
    header->flags = TCP_FLAG_RST | TCP_FLAG_ACK;
    header->window = 0;
    header->checksum = 0;
    header->urgent_ptr = 0;

    uint32_t our_ip = ip_get_address();
    header->checksum = tcp_checksum(htonl(our_ip), htonl(dest_ip), packet, 20);

    ip_send(dest_ip, IP_PROTO_TCP, packet, 20);
}

// Initialize TCP layer
void tcp_init(void) {
    memset(connections, 0, sizeof(connections));
    next_ephemeral_port = 49152;

    // Seed random generator with timer ticks
    tcp_rand_state = (uint32_t)timer_ticks ^ 0xDEADBEEF;

    // Register with IP layer
    ip_register_handler(IP_PROTO_TCP, tcp_handle);

    kprintf("[TCP] TCP initialized\n");
}

// Create a TCP socket
int tcp_socket(void) {
    int sock = tcp_alloc_conn();
    if (sock < 0) {
        return TCP_ERR_NO_MEMORY;
    }

    // #487/#349: stamp the creating process so Task Manager / Process Explorer
    // can attribute this connection. This is the ONE path where proc_current()
    // is meaningful: tcp_socket() is only reached from SYS_SOCKET, i.e. in the
    // caller's own syscall context. The inbound-SYN path in tcp_handle_packet()
    // must inherit from the listener instead (it runs from the RX IRQ, where
    // proc_current() is an unrelated victim process).
    // proc_current_pid() is a narrow accessor (proc/process.h) so the net layer
    // does not have to pull in the whole PCB definition; it returns 0 when
    // there is no current process, which reads as "unowned".
    connections[sock].owner_pid = proc_current_pid();

    kprintf("[TCP] Created socket %d\n", sock);
    return sock;
}

// Bind a socket to a local port
int tcp_bind(int sock, uint16_t port) {
    tcp_conn_t *conn = tcp_get_conn(sock);
    if (!conn) {
        return TCP_ERR_INVALID;
    }

    if (conn->state != TCP_STATE_CLOSED) {
        return TCP_ERR_INVALID;
    }

    // Check if port is already in use
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (connections[i].active && connections[i].local_port == port) {
            return TCP_ERR_INVALID;
        }
    }

    conn->local_port = port;
    kprintf("[TCP] Socket %d bound to port %d\n", sock, port);
    return 0;
}

// Listen for incoming connections
int tcp_listen(int sock, int backlog) {
    tcp_conn_t *conn = tcp_get_conn(sock);
    if (!conn) {
        return TCP_ERR_INVALID;
    }

    if (conn->local_port == 0) {
        return TCP_ERR_INVALID;  // Must bind first
    }

    conn->state = TCP_STATE_LISTEN;
    conn->is_listener = 1;
    conn->listen_backlog = backlog;

    kprintf("[TCP] Socket %d listening on port %d\n", sock, conn->local_port);
    return 0;
}

// Accept an incoming connection
int tcp_accept(int sock) {
    tcp_conn_t *listener = tcp_get_conn(sock);
    if (!listener) {
        return TCP_ERR_INVALID;
    }

    if (!listener->is_listener || listener->state != TCP_STATE_LISTEN) {
        return TCP_ERR_INVALID;
    }

    // Look for an established connection on this port
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *conn = &connections[i];
        if (conn->active &&
            !conn->is_listener &&
            !conn->accepted &&
            conn->local_port == listener->local_port &&
            conn->state == TCP_STATE_ESTABLISHED) {
            // #435: hand each established connection out ONCE. Without the
            // accepted flag, a concurrent (threaded) accept loop re-accepts the
            // same still-ESTABLISHED socket every poll and spawns multiple
            // handlers on one connection, corrupting the SSH stream.
            conn->accepted = 1;
            // Found an established connection, return it
            kprintf("[TCP] Accept: returning socket %d (established)\n", i);
            return i;
        }
    }

    // No pending connections
    return TCP_ERR_WOULD_BLOCK;
}

// Connect to a remote host
int tcp_connect(int sock, uint32_t remote_ip, uint16_t remote_port) {
    tcp_conn_t *conn = tcp_get_conn(sock);
    if (!conn) {
        return TCP_ERR_INVALID;
    }

    if (conn->state != TCP_STATE_CLOSED) {
        return TCP_ERR_INVALID;
    }

    // Allocate local port if not bound
    if (conn->local_port == 0) {
        conn->local_port = tcp_alloc_port();
        if (conn->local_port == 0) {
            return TCP_ERR_NO_PORTS;
        }
    }

    // Set remote endpoint
    conn->remote_ip = remote_ip;
    conn->remote_port = remote_port;

    // Generate initial sequence number
    conn->iss = tcp_generate_isn();
    conn->snd_una = conn->iss;
    conn->snd_nxt = conn->iss;

    kprintf("[TCP] Connecting socket %d to ", sock);
    ip_print(remote_ip);
    kprintf(":%d (ISS=%u)\n", remote_port, conn->iss);

    // Send SYN
    conn->state = TCP_STATE_SYN_SENT;
    conn->retries = 0;

    if (tcp_send_segment(conn, TCP_FLAG_SYN, NULL, 0) < 0) {
        // SYN send may fail if ARP is not yet resolved for the destination.
        // Stay in SYN_SENT state so TCP retransmission can retry after ARP resolves.
        kprintf("[TCP] Initial SYN failed (ARP pending), will retry via retransmit\n");
    }

    return TCP_ERR_IN_PROGRESS;
}

// Send data on a connection
int tcp_send(int sock, const void *data, uint16_t length) {
    tcp_conn_t *conn = tcp_get_conn(sock);
    if (!conn) {
        return TCP_ERR_INVALID;
    }

    if (conn->state != TCP_STATE_ESTABLISHED &&
        conn->state != TCP_STATE_CLOSE_WAIT) {
        return TCP_ERR_INVALID;
    }

    if (length == 0) {
        return 0;
    }

    // Limit to MSS
    if (length > TCP_MSS) {
        length = TCP_MSS;
    }

    // Check window
    uint32_t in_flight = conn->snd_nxt - conn->snd_una;
    if (in_flight >= conn->snd_wnd) {
        return TCP_ERR_WOULD_BLOCK;
    }

    // #442: the retransmission buffer must hold EVERY unacknowledged byte we
    // put on the wire, or a later retransmit silently has nothing to resend
    // (see tcp_timer()). Previously this only capped how much got copied into
    // send_buffer but still transmitted the full segment regardless, so once
    // more than TCP_SEND_BUFFER_SIZE bytes were in flight, any segment beyond
    // that could never be retransmitted -- eventually exhausting
    // TCP_MAX_RETRIES and RST-closing the connection (the SSHD exec-channel
    // truncation / sustained-stream RST-burst bug). Gate on capacity instead:
    // block (like the window check above) until earlier data is ACKed and
    // send_len shrinks, so we never send more than we can retain.
    if ((uint32_t)conn->send_len + (uint32_t)length > TCP_SEND_BUFFER_SIZE) {
        return TCP_ERR_WOULD_BLOCK;
    }

    // Copy to send buffer for potential retransmission
    memcpy(conn->send_buffer + conn->send_len, data, length);
    conn->send_len += length;

    // Send the data
    if (tcp_send_segment(conn, TCP_FLAG_ACK | TCP_FLAG_PSH, data, length) < 0) {
        return TCP_ERR_NO_MEMORY;
    }

    return length;
}

// Receive data from a connection
int tcp_recv(int sock, void *buffer, uint16_t length) {
    tcp_conn_t *conn = tcp_get_conn(sock);
    if (!conn) {
        return TCP_ERR_INVALID;
    }

    // Check for connection closed
    if (conn->state == TCP_STATE_CLOSED ||
        conn->state == TCP_STATE_TIME_WAIT) {
        if (conn->recv_len > 0) {
            // Return remaining data
        } else {
            return TCP_ERR_CLOSED;
        }
    }

    if (conn->recv_len == 0) {
        // Check if remote has closed
        if (conn->state == TCP_STATE_CLOSE_WAIT ||
            conn->state == TCP_STATE_LAST_ACK) {
            return TCP_ERR_CLOSED;
        }
        return 0;  // No data available
    }

    // Copy data from receive buffer
    uint16_t copy_len = length;
    if (copy_len > conn->recv_len) {
        copy_len = conn->recv_len;
    }

    memcpy(buffer, conn->recv_buffer, copy_len);

    // Shift remaining data
    if (copy_len < conn->recv_len) {
        memmove(conn->recv_buffer, conn->recv_buffer + copy_len,
                conn->recv_len - copy_len);
    }
    uint32_t old_wnd = conn->rcv_wnd;
    conn->recv_len -= copy_len;

    // Update receive window
    conn->rcv_wnd = TCP_RECV_BUFFER_SIZE - conn->recv_len;

    // Window-update ACK: if our advertised receive window was (near) zero and we
    // just freed a meaningful amount of buffer, proactively tell the peer the
    // window has reopened. Without this, a sender that filled our 16KB buffer
    // (rcv_wnd advertised 0) stalls forever waiting for an update, deadlocking
    // large HTTP/2 responses (Google, Wikipedia). See #245.
    if (old_wnd < (TCP_RECV_BUFFER_SIZE / 2) &&
        conn->rcv_wnd >= (TCP_RECV_BUFFER_SIZE / 2) &&
        (conn->state == TCP_STATE_ESTABLISHED ||
         conn->state == TCP_STATE_CLOSE_WAIT)) {
        tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
    }

    return copy_len;
}

// Close a connection
int tcp_close(int sock) {
    tcp_conn_t *conn = tcp_get_conn(sock);
    if (!conn) {
        return TCP_ERR_INVALID;
    }

    kprintf("[TCP] Closing socket %d (state=%s)\n", sock, tcp_state_name(conn->state));

    switch (conn->state) {
        case TCP_STATE_CLOSED:
            // Already closed, just free
            conn->active = 0;
            break;

        case TCP_STATE_LISTEN:
        case TCP_STATE_SYN_SENT:
            // No connection, just close
            conn->active = 0;
            conn->state = TCP_STATE_CLOSED;
            break;

        case TCP_STATE_SYN_RECEIVED:
        case TCP_STATE_ESTABLISHED:
            // Send FIN
            conn->state = TCP_STATE_FIN_WAIT_1;
            tcp_send_segment(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            break;

        case TCP_STATE_CLOSE_WAIT:
            // Send FIN
            conn->state = TCP_STATE_LAST_ACK;
            tcp_send_segment(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            break;

        case TCP_STATE_FIN_WAIT_1:
        case TCP_STATE_FIN_WAIT_2:
        case TCP_STATE_CLOSING:
        case TCP_STATE_LAST_ACK:
        case TCP_STATE_TIME_WAIT:
            // Already closing
            break;
    }

    return 0;
}

// Get connection state
tcp_state_t tcp_get_state(int sock) {
    tcp_conn_t *conn = tcp_get_conn(sock);
    if (!conn) {
        return TCP_STATE_CLOSED;
    }
    return conn->state;
}

// Get last error
int tcp_get_error(int sock) {
    tcp_conn_t *conn = tcp_get_conn(sock);
    if (!conn) {
        return TCP_ERR_INVALID;
    }
    return conn->error;
}

// Check if connected
int tcp_is_connected(int sock) {
    tcp_conn_t *conn = tcp_get_conn(sock);
    if (!conn) {
        return 0;
    }
    return conn->state == TCP_STATE_ESTABLISHED;
}

// Handle incoming TCP packet
void tcp_handle(uint32_t src_ip_raw, const void *data, uint16_t length) {
    if (length < 20) {
        return;  // Too short for TCP header
    }

    const tcp_header_t *header = (const tcp_header_t *)data;

    // Convert src_ip from network to host byte order
    uint32_t src_ip = ntohl(src_ip_raw);

    uint16_t src_port = ntohs(header->src_port);
    uint16_t dest_port = ntohs(header->dest_port);
    uint32_t seq_num = ntohl(header->seq_num);
    uint32_t ack_num = ntohl(header->ack_num);
    uint8_t flags = header->flags;
    uint16_t window = ntohs(header->window);

    // Calculate data offset
    uint8_t data_offset = (header->data_offset >> 4) * 4;
    if (data_offset < 20 || data_offset > length) {
        return;  // Invalid header length
    }

    const uint8_t *payload = (const uint8_t *)data + data_offset;
    uint16_t payload_len = length - data_offset;

    if (g_tcp_dbg) {
        kprintf("[TCP] Received: src=");
        ip_print(src_ip);
        kprintf(":%d dst=port %d flags=0x%02x seq=%u ack=%u len=%d\n",
                src_port, dest_port, flags, seq_num, ack_num, payload_len);
    }

    // Verify checksum
    uint32_t our_ip = ip_get_address();
    uint16_t computed = tcp_checksum(htonl(src_ip), htonl(our_ip), data, length);
    if (computed != 0) {
        kprintf("[TCP] Bad checksum, dropping\n");
        return;
    }

    // Find existing connection
    tcp_conn_t *conn = tcp_find_connection(dest_port, src_ip, src_port);

    // If no connection and it's a SYN, check for listener
    if (!conn && (flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) {
        tcp_conn_t *listener = tcp_find_listener(dest_port);
        if (listener) {
            // Create new connection for this SYN
            int new_sock = tcp_alloc_conn();
            if (new_sock >= 0) {
                conn = &connections[new_sock];
                conn->local_port = dest_port;
                conn->remote_ip = src_ip;
                conn->remote_port = src_port;
                conn->irs = seq_num;
                conn->rcv_nxt = seq_num + 1;
                conn->iss = tcp_generate_isn();
                conn->snd_una = conn->iss;
                conn->snd_nxt = conn->iss;
                conn->snd_wnd = window;
                conn->state = TCP_STATE_SYN_RECEIVED;
                // #487/#349: INHERIT the listener's owner. Deliberately NOT
                // proc_current(): this runs from the RX path, so the "current"
                // process is whatever the NIC interrupt happened to preempt,
                // and using it would attribute inbound connections to random
                // innocent processes.
                conn->owner_pid = listener->owner_pid;

                kprintf("[TCP] New connection from ");
                ip_print(src_ip);
                kprintf(":%d (socket %d)\n", src_port, new_sock);

                // Send SYN+ACK
                tcp_send_segment(conn, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
                return;
            }
        }
    }

    // No connection found
    if (!conn) {
        // Send RST if not a RST
        if (!(flags & TCP_FLAG_RST)) {
            if (flags & TCP_FLAG_ACK) {
                tcp_send_rst(src_ip, dest_port, src_port, ack_num, 0);
            } else {
                tcp_send_rst(src_ip, dest_port, src_port, 0, seq_num + payload_len + 1);
            }
        }
        return;
    }

    // Handle RST
    if (flags & TCP_FLAG_RST) {
        kprintf("[TCP] Connection reset\n");
        conn->state = TCP_STATE_CLOSED;
        conn->error = TCP_ERR_RESET;
        return;
    }

    // State machine processing
    switch (conn->state) {
        case TCP_STATE_SYN_SENT:
            // Expecting SYN+ACK
            if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                // Check ACK
                if (ack_num == conn->snd_nxt) {
                    conn->snd_una = ack_num;
                    conn->irs = seq_num;
                    conn->rcv_nxt = seq_num + 1;
                    conn->snd_wnd = window;
                    conn->state = TCP_STATE_ESTABLISHED;

                    kprintf("[TCP] Connection established\n");

                    // Send ACK
                    tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
                } else {
                    // Bad ACK
                    tcp_send_rst(src_ip, dest_port, src_port, ack_num, 0);
                }
            } else if (flags & TCP_FLAG_SYN) {
                // Simultaneous open
                conn->irs = seq_num;
                conn->rcv_nxt = seq_num + 1;
                conn->state = TCP_STATE_SYN_RECEIVED;
                tcp_send_segment(conn, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_STATE_SYN_RECEIVED:
            // Expecting ACK
            if (flags & TCP_FLAG_ACK) {
                if (ack_num == conn->snd_nxt) {
                    conn->snd_una = ack_num;
                    conn->snd_wnd = window;
                    conn->state = TCP_STATE_ESTABLISHED;
                    kprintf("[TCP] Connection established (passive)\n");
                }
            }
            break;

        case TCP_STATE_ESTABLISHED:
            // Process ACK
            if (flags & TCP_FLAG_ACK) {
                // Advance send window
                if (ack_num > conn->snd_una && ack_num <= conn->snd_nxt) {
                    uint32_t acked = ack_num - conn->snd_una;
                    conn->snd_una = ack_num;
                    conn->snd_wnd = window;

                    // Remove acked data from send buffer
                    if (acked <= conn->send_len) {
                        memmove(conn->send_buffer, conn->send_buffer + acked,
                                conn->send_len - acked);
                        conn->send_len -= acked;
                    } else {
                        conn->send_len = 0;
                    }
                    conn->retries = 0;
                }
            }

            // Process incoming data
            if (payload_len > 0) {
                // Check sequence number
                if (seq_num == conn->rcv_nxt) {
                    // In-order data
                    uint16_t space = TCP_RECV_BUFFER_SIZE - conn->recv_len;
                    uint16_t copy_len = payload_len;
                    if (copy_len > space) {
                        copy_len = space;
                    }

                    if (copy_len > 0) {
                        memcpy(conn->recv_buffer + conn->recv_len, payload, copy_len);
                        conn->recv_len += copy_len;
                        conn->rcv_nxt += copy_len;
                        conn->rcv_wnd = TCP_RECV_BUFFER_SIZE - conn->recv_len;
                    }

                    // Send ACK
                    tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
                } else {
                    // Out of order - send duplicate ACK
                    tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
                }
            }

            // Handle FIN
            if (flags & TCP_FLAG_FIN) {
                conn->rcv_nxt++;
                conn->state = TCP_STATE_CLOSE_WAIT;
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
                kprintf("[TCP] Remote closed, entering CLOSE_WAIT\n");
            }
            break;

        case TCP_STATE_FIN_WAIT_1:
            if (flags & TCP_FLAG_ACK) {
                if (ack_num == conn->snd_nxt) {
                    conn->snd_una = ack_num;
                    if (flags & TCP_FLAG_FIN) {
                        conn->rcv_nxt++;
                        conn->state = TCP_STATE_TIME_WAIT;
                        conn->last_send_time = timer_ticks;
                        tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
                    } else {
                        conn->state = TCP_STATE_FIN_WAIT_2;
                    }
                }
            } else if (flags & TCP_FLAG_FIN) {
                conn->rcv_nxt++;
                conn->state = TCP_STATE_CLOSING;
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_STATE_FIN_WAIT_2:
            if (flags & TCP_FLAG_FIN) {
                conn->rcv_nxt++;
                conn->state = TCP_STATE_TIME_WAIT;
                conn->last_send_time = timer_ticks;
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            }
            break;

        case TCP_STATE_CLOSE_WAIT:
            // Waiting for application to close
            break;

        case TCP_STATE_CLOSING:
            if (flags & TCP_FLAG_ACK) {
                if (ack_num == conn->snd_nxt) {
                    conn->state = TCP_STATE_TIME_WAIT;
                    conn->last_send_time = timer_ticks;
                }
            }
            break;

        case TCP_STATE_LAST_ACK:
            if (flags & TCP_FLAG_ACK) {
                if (ack_num == conn->snd_nxt) {
                    conn->state = TCP_STATE_CLOSED;
                    conn->active = 0;
                    kprintf("[TCP] Connection closed\n");
                }
            }
            break;

        case TCP_STATE_TIME_WAIT:
            // Handle retransmitted FIN
            if (flags & TCP_FLAG_FIN) {
                tcp_send_segment(conn, TCP_FLAG_ACK, NULL, 0);
                conn->last_send_time = timer_ticks;
            }
            break;

        default:
            break;
    }
}

// Process TCP timers
void tcp_timer(void) {
    uint64_t now = timer_ticks;

    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_conn_t *conn = &connections[i];
        if (!conn->active) {
            continue;
        }

        switch (conn->state) {
            case TCP_STATE_SYN_SENT:
            case TCP_STATE_SYN_RECEIVED:
                // Retransmit SYN
                if (now - conn->last_send_time > TCP_RETRANSMIT_TIMEOUT) {
                    if (conn->retries >= TCP_MAX_RETRIES) {
                        kprintf("[TCP] Connection timeout\n");
                        conn->state = TCP_STATE_CLOSED;
                        conn->error = TCP_ERR_TIMEOUT;
                        conn->active = 0;
                    } else {
                        conn->retries++;
                        conn->snd_nxt = conn->iss;  // Reset sequence number
                        if (conn->state == TCP_STATE_SYN_SENT) {
                            tcp_send_segment(conn, TCP_FLAG_SYN, NULL, 0);
                        } else {
                            tcp_send_segment(conn, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
                        }
                    }
                }
                break;

            case TCP_STATE_ESTABLISHED:
                // Retransmit unacknowledged data
                if (conn->snd_una < conn->snd_nxt &&
                    now - conn->last_send_time > TCP_RETRANSMIT_TIMEOUT) {
                    if (conn->retries >= TCP_MAX_RETRIES) {
                        kprintf("[TCP] Retransmit timeout, closing\n");
                        conn->state = TCP_STATE_CLOSED;
                        conn->error = TCP_ERR_TIMEOUT;
                        tcp_send_rst(conn->remote_ip, conn->local_port,
                                     conn->remote_port, conn->snd_nxt, conn->rcv_nxt);
                        // #442: this branch set state=CLOSED but never freed the
                        // slot (unlike the identical SYN_SENT/SYN_RECEIVED timeout
                        // case just above, which does `conn->active = 0`). The
                        // connection stayed "active" forever in a state tcp_handle()
                        // has no case for, so tcp_find_connection() kept matching
                        // it for the same 4-tuple: a client's reconnect on that
                        // tuple got silently swallowed by this zombie slot instead
                        // of allocating a fresh connection -- the "reconnect wedges
                        // the kernel TCP state until reboot" bug. Free it here too.
                        conn->active = 0;
                    } else {
                        conn->retries++;
                        // Retransmit from send buffer. #442: this used to hand the
                        // WHOLE unacked backlog to tcp_send_segment() as one
                        // segment; that function's wire packet buffer is sized for
                        // one MSS, so any retransmit needed once more than one MSS
                        // was outstanding (routine once the send buffer could hold
                        // more than a few KB) silently failed -- tcp_send_segment
                        // returned -1, unchecked here, so nothing went out, retries
                        // still counted up, and the connection got RST-closed after
                        // TCP_MAX_RETRIES for no visible reason. Resend in MSS-sized
                        // chunks so the full outstanding range actually goes back
                        // out; tcp_send_segment() advances snd_nxt by each chunk's
                        // length, so it ends up matching the original snd_nxt again.
                        uint32_t unsent = conn->snd_nxt - conn->snd_una;
                        if (unsent <= conn->send_len) {
                            conn->snd_nxt = conn->snd_una;
                            uint32_t off = 0;
                            while (off < unsent) {
                                uint32_t rem = unsent - off;
                                uint16_t seg = (rem > TCP_MSS) ? TCP_MSS : (uint16_t)rem;
                                if (tcp_send_segment(conn, TCP_FLAG_ACK | TCP_FLAG_PSH,
                                                     conn->send_buffer + off, seg) < 0) {
                                    break;
                                }
                                off += seg;
                            }
                        }
                    }
                }
                break;

            case TCP_STATE_FIN_WAIT_1:
            case TCP_STATE_CLOSING:
            case TCP_STATE_LAST_ACK:
                // Retransmit FIN
                if (now - conn->last_send_time > TCP_RETRANSMIT_TIMEOUT) {
                    if (conn->retries >= TCP_MAX_RETRIES) {
                        conn->state = TCP_STATE_CLOSED;
                        conn->active = 0;
                    } else {
                        conn->retries++;
                        conn->snd_nxt--;  // Back up for FIN
                        tcp_send_segment(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
                    }
                }
                break;

            case TCP_STATE_FIN_WAIT_2:
                // #297: we sent FIN and it was ACKed; we are waiting for the
                // peer's FIN. Some servers (and lost-FIN cases) never send it,
                // leaving the slot active forever. Reap after a bounded wait so
                // repeated HTTPS POSTs cannot slowly leak connection slots.
                if (now - conn->last_send_time > TCP_TIME_WAIT_TIMEOUT) {
                    kprintf("[TCP] FIN_WAIT_2 reaped (no peer FIN)\n");
                    conn->state = TCP_STATE_CLOSED;
                    conn->active = 0;
                }
                break;

            case TCP_STATE_CLOSE_WAIT:
                // #297: peer closed; we owe a FIN once the app calls close().
                // If the owning task died without closing, this lingers forever.
                // Reap stale CLOSE_WAIT slots after a bounded wait.
                if (now - conn->last_send_time > TCP_TIME_WAIT_TIMEOUT * 2) {
                    kprintf("[TCP] CLOSE_WAIT reaped (app never closed)\n");
                    conn->state = TCP_STATE_CLOSED;
                    conn->active = 0;
                }
                break;

            case TCP_STATE_TIME_WAIT:
                // Timeout after 2*MSL
                if (now - conn->last_send_time > TCP_TIME_WAIT_TIMEOUT) {
                    kprintf("[TCP] TIME_WAIT expired, closing\n");
                    conn->state = TCP_STATE_CLOSED;
                    conn->active = 0;
                }
                break;

            default:
                break;
        }
    }
}

// Get state name for debugging
const char* tcp_state_name(tcp_state_t state) {
    switch (state) {
        case TCP_STATE_CLOSED:       return "CLOSED";
        case TCP_STATE_LISTEN:       return "LISTEN";
        case TCP_STATE_SYN_SENT:     return "SYN_SENT";
        case TCP_STATE_SYN_RECEIVED: return "SYN_RECEIVED";
        case TCP_STATE_ESTABLISHED:  return "ESTABLISHED";
        case TCP_STATE_FIN_WAIT_1:   return "FIN_WAIT_1";
        case TCP_STATE_FIN_WAIT_2:   return "FIN_WAIT_2";
        case TCP_STATE_CLOSE_WAIT:   return "CLOSE_WAIT";
        case TCP_STATE_CLOSING:      return "CLOSING";
        case TCP_STATE_LAST_ACK:     return "LAST_ACK";
        case TCP_STATE_TIME_WAIT:    return "TIME_WAIT";
        default:                     return "UNKNOWN";
    }
}

// --- #297 diagnostics: snapshot of the connection table ----------------------
// Counts active slots and per-state totals. Used by the RC "tcpstat" command to
// watch for socket/PCB exhaustion or lingering close states across repeated
// HTTPS POSTs. out_counts must hold at least 11 ints (one per tcp_state_t).
int tcp_diag_snapshot(int *out_active, int *out_counts, int ncounts) {
    int active = 0;
    for (int i = 0; i < 11 && i < ncounts; i++) out_counts[i] = 0;
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].active) continue;
        active++;
        int st = (int)connections[i].state;
        if (st >= 0 && st < 11 && st < ncounts) out_counts[st]++;
    }
    if (out_active) *out_active = active;
    return TCP_MAX_CONNECTIONS;
}

// #404/#349 Task Manager: read-only per-connection snapshot (netstat-style).
// See tcp_conn_info_t in tcp.h. System-wide (no pid ownership on tcp_conn_t).
int tcp_conn_snapshot(tcp_conn_info_t *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int i = 0; i < TCP_MAX_CONNECTIONS && n < max; i++) {
        if (!connections[i].active) continue;
        out[n].state       = (uint8_t)connections[i].state;
        out[n].is_listener = (uint8_t)connections[i].is_listener;
        out[n].local_port  = connections[i].local_port;
        out[n].remote_port = connections[i].remote_port;
        out[n].remote_ip   = connections[i].remote_ip;
        out[n].recv_len    = connections[i].recv_len;
        out[n].send_len    = connections[i].send_len;
        out[n].owner_pid   = connections[i].owner_pid;   // #487
        n++;
    }
    return n;
}

// #487/#349: C reference twin of conn_filter_by_pid_rs. Kept for the boot
// differential and as the rollback when -DRUST_CONN_OWNER is dropped.
int conn_filter_by_pid_c(const tcp_conn_info_t *in, uint32_t n, uint32_t pid,
                         tcp_conn_info_t *out, uint32_t out_cap) {
    if (!in || !out) return -1;
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (in[i].owner_pid != pid) continue;
        if (w >= out_cap) break;      // never write past the caller's array
        out[w++] = in[i];
    }
    return (int)w;
}

// ---------------------------------------------------------------------------
// #487/#349 boot-time [RUST-DIFF] differential for the conn_owner seam.
//
// Corpus reaches the states a naive filter gets wrong, each counted:
//   - more matches than out_cap  (the CWE-787 over-write: must stop at cap)
//   - out_cap == 0               (must write nothing, return 0)
//   - zero matches
//   - pid 0 (unowned rows)
//   - every row matching
// A canary row past `out_cap` proves neither implementation writes past it.
// ---------------------------------------------------------------------------
static uint32_t co_rng = 0x5EED1234u;
static uint32_t co_rand(void) {
    co_rng ^= co_rng << 13; co_rng ^= co_rng >> 17; co_rng ^= co_rng << 5;
    return co_rng;
}

void conn_owner_selftest(void) {
    enum { MAXN = 32 };
    int mism = 0, vecs = 0, canary = 0;
    int cov_overflow = 0, cov_cap0 = 0, cov_nomatch = 0, cov_allmatch = 0, cov_pid0 = 0;

    for (int iter = 0; iter < 500; iter++) {
        tcp_conn_info_t in[MAXN];
        uint32_t n = co_rand() % (MAXN + 1);
        // 0..4, NOT 1..4: pid 0 means "unowned/kernel-internal" and is a real
        // query (it lists the stack's own sockets). An earlier version of this
        // generator used `1 + (rand % 4)`, so the pid0 coverage counter could
        // never fire: a control that cannot fire proves nothing (blame.md).
        uint32_t pid = co_rand() % 5;
        uint32_t matches = 0;
        for (uint32_t i = 0; i < n; i++) {
            in[i].state = (uint8_t)(co_rand() % 11);
            in[i].is_listener = (uint8_t)(co_rand() % 2);
            in[i].local_port = (uint16_t)(co_rand() % 65536);
            in[i].remote_port = (uint16_t)(co_rand() % 65536);
            in[i].remote_ip = co_rand();
            in[i].recv_len = (uint16_t)(co_rand() % 4096);
            in[i].send_len = co_rand() % 4096;
            // Bias hard toward the queried pid so out_cap overflow is common.
            in[i].owner_pid = (co_rand() % 3 == 0) ? (co_rand() % 5) : pid;
            if (in[i].owner_pid == pid) matches++;
        }
        uint32_t cap = co_rand() % (MAXN + 1);
        if (iter % 11 == 0) cap = 0;

        if (matches > cap) cov_overflow++;
        if (cap == 0) cov_cap0++;
        if (matches == 0) cov_nomatch++;
        if (n > 0 && matches == n) cov_allmatch++;
        if (pid == 0) cov_pid0++;

        tcp_conn_info_t oc[MAXN + 2], orr[MAXN + 2];
        // Canary rows immediately past cap.
        for (uint32_t i = 0; i < MAXN + 2; i++) {
            oc[i].owner_pid = 0xDEADBEEFu; orr[i].owner_pid = 0xDEADBEEFu;
        }
        int rc = conn_filter_by_pid_c(in, n, pid, oc, cap);
        int rr = conn_filter_by_pid_rs(in, n, pid, orr, cap);
        vecs++;
        if (rc != rr) mism++;
        else {
            for (int i = 0; i < rc; i++) {
                if (oc[i].owner_pid != orr[i].owner_pid ||
                    oc[i].local_port != orr[i].local_port ||
                    oc[i].remote_port != orr[i].remote_port ||
                    oc[i].remote_ip != orr[i].remote_ip ||
                    oc[i].state != orr[i].state ||
                    oc[i].is_listener != orr[i].is_listener ||
                    oc[i].recv_len != orr[i].recv_len ||
                    oc[i].send_len != orr[i].send_len) { mism++; break; }
            }
        }
        if (oc[cap].owner_pid != 0xDEADBEEFu || orr[cap].owner_pid != 0xDEADBEEFu) canary++;
    }

    // Contract edges: NULL in / NULL out must be rejected by both.
    {
        tcp_conn_info_t o[2];
        if (conn_filter_by_pid_c(0, 1, 1, o, 2) != -1 ||
            conn_filter_by_pid_rs(0, 1, 1, o, 2) != -1) mism++;
        vecs++;
        tcp_conn_info_t i2[2];
        if (conn_filter_by_pid_c(i2, 1, 1, 0, 2) != -1 ||
            conn_filter_by_pid_rs(i2, 1, 1, 0, 2) != -1) mism++;
        vecs++;
    }

    kprintf("[RUST-DIFF] conn_owner: %d vecs mism=%d canary=%d %s (LIVE=%s)\n",
            vecs, mism, canary, (mism || canary) ? "MISMATCH" : "MATCH",
#ifdef RUST_CONN_OWNER
            "rust"
#else
            "c"
#endif
    );
    kprintf("[RUST-DIFF] conn_owner coverage: cap_overflow=%d cap0=%d no_match=%d "
            "all_match=%d pid0=%d\n",
            cov_overflow, cov_cap0, cov_nomatch, cov_allmatch, cov_pid0);
}
