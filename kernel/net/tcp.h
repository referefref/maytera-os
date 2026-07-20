// tcp.h - Transmission Control Protocol
#ifndef TCP_H
#define TCP_H

#include "../types.h"

// TCP header flags
#define TCP_FLAG_FIN    0x01
#define TCP_FLAG_SYN    0x02
#define TCP_FLAG_RST    0x04
#define TCP_FLAG_PSH    0x08
#define TCP_FLAG_ACK    0x10
#define TCP_FLAG_URG    0x20

// TCP connection states
typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECEIVED,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_CLOSING,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT
} tcp_state_t;

// TCP header structure (20 bytes without options)
typedef struct {
    uint16_t src_port;      // Source port
    uint16_t dest_port;     // Destination port
    uint32_t seq_num;       // Sequence number
    uint32_t ack_num;       // Acknowledgment number
    uint8_t  data_offset;   // Data offset (4 bits) + reserved (4 bits)
    uint8_t  flags;         // Control flags
    uint16_t window;        // Window size
    uint16_t checksum;      // Checksum
    uint16_t urgent_ptr;    // Urgent pointer
} __attribute__((packed)) tcp_header_t;

// TCP pseudo-header for checksum calculation
typedef struct {
    uint32_t src_ip;
    uint32_t dest_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_length;
} __attribute__((packed)) tcp_pseudo_header_t;

// Maximum number of TCP connections
#define TCP_MAX_CONNECTIONS     64

// TCP buffer sizes
#define TCP_RECV_BUFFER_SIZE    32768
// #442: was 4096. That is smaller than TCP_INITIAL_WINDOW/the max possible
// receiver-advertised window (a 16-bit header field, so up to 65535), which let
// tcp_send() commit more bytes to the wire than the retransmission buffer could
// hold. Once a real peer's window let more than 4096 bytes sit unacknowledged
// (any sustained transfer: `cat` of a large file over the SSHD exec channel,
// a multi-MB VNC frame, etc.), a segment needing retransmission fell outside
// the buffer, tcp_timer() silently skipped resending it, retries still counted
// up, and after TCP_MAX_RETRIES the connection was RST and torn down -- with
// the zombie-slot bug below, repeated reconnects would then wedge the stack.
// 65536 covers the largest window this stack can ever advertise/see.
#define TCP_SEND_BUFFER_SIZE    65536

// TCP timeouts (in timer ticks, ~18.2 ticks/sec on x86)
#define TCP_RETRANSMIT_TIMEOUT  500     // 2s at 250Hz PIT (was 36 = 0.14s, stale 18.2Hz value; too short -> RST mid-transfer on slow SMB/USB-RAID reads)
#define TCP_TIME_WAIT_TIMEOUT   546     // ~30 seconds (2*MSL)
#define TCP_CONNECT_TIMEOUT     180     // ~10 seconds
#define TCP_MAX_RETRIES         5

// Initial window size
#define TCP_INITIAL_WINDOW      32768

// Maximum segment size (MTU - IP header - TCP header)
#define TCP_MSS                 1460

// TCP connection control block (TCB)
typedef struct {
    int active;                         // Is this slot in use?
    tcp_state_t state;                  // Connection state

    // Local endpoint
    uint16_t local_port;

    // Remote endpoint
    uint32_t remote_ip;
    uint16_t remote_port;

    // Sequence numbers
    uint32_t snd_una;                   // Oldest unacknowledged sequence number
    uint32_t snd_nxt;                   // Next sequence number to send
    uint32_t snd_wnd;                   // Send window size
    uint32_t rcv_nxt;                   // Next expected sequence number
    uint32_t rcv_wnd;                   // Receive window size

    // Initial sequence numbers
    uint32_t iss;                       // Initial send sequence number
    uint32_t irs;                       // Initial receive sequence number

    // Receive buffer
    uint8_t recv_buffer[TCP_RECV_BUFFER_SIZE];
    uint16_t recv_len;                  // Bytes in receive buffer

    // Send buffer (for retransmission)
    uint8_t send_buffer[TCP_SEND_BUFFER_SIZE];
    uint32_t send_len;                  // Bytes in send buffer (#442: widened from
                                         // uint16_t so it can represent the full
                                         // 65536-byte buffer without wrapping)
    uint32_t send_offset;               // Offset of first unsent byte

    // Retransmission control
    uint64_t last_send_time;            // Time of last send (for RTO)
    uint16_t retries;                   // Number of retransmission attempts

    // Listening socket (for accept)
    int listen_backlog;                 // Maximum pending connections
    int is_listener;                    // Is this a listening socket?
    int accepted;                       // #435: 1 once tcp_accept() handed this conn out (accept-once)

    // Error tracking
    int error;                          // Last error code

    // #487/#349: owning process, for per-process network attribution in Task
    // Manager / Process Explorer. Stamped at tcp_socket() from proc_current().
    // 0 = unowned/kernel-internal (the stack's own sockets, or a socket created
    // outside any process context).
    //
    // IMPORTANT: an inbound connection accepted off a listener INHERITS the
    // listener's owner_pid. It must NOT use proc_current(): that path runs from
    // the RX softirq/IRQ, where proc_current() is whatever process happened to
    // be interrupted, which would attribute the connection to a random victim.
    uint32_t owner_pid;
} tcp_conn_t;

// Error codes
#define TCP_ERR_NONE            0
#define TCP_ERR_NO_MEMORY       -1
#define TCP_ERR_NO_PORTS        -2
#define TCP_ERR_TIMEOUT         -3
#define TCP_ERR_REFUSED         -4
#define TCP_ERR_RESET           -5
#define TCP_ERR_CLOSED          -6
#define TCP_ERR_INVALID         -7
#define TCP_ERR_IN_PROGRESS     -8
#define TCP_ERR_WOULD_BLOCK     -9

// Initialize TCP layer
void tcp_init(void);

// Socket-like API

// Create a TCP socket (returns socket descriptor or negative error)
int tcp_socket(void);

// Bind a socket to a local port
int tcp_bind(int sock, uint16_t port);

// Listen for incoming connections
int tcp_listen(int sock, int backlog);

// Accept an incoming connection (returns new socket or negative error)
// Non-blocking: returns TCP_ERR_WOULD_BLOCK if no pending connections
int tcp_accept(int sock);

// Connect to a remote host (non-blocking, check state for completion)
int tcp_connect(int sock, uint32_t remote_ip, uint16_t remote_port);

// Send data (returns bytes sent or negative error)
int tcp_send(int sock, const void *data, uint16_t length);

// Receive data (returns bytes received or negative error)
// Non-blocking: returns 0 if no data available
int tcp_recv(int sock, void *buffer, uint16_t length);

// Close a connection
int tcp_close(int sock);

// Get connection state
tcp_state_t tcp_get_state(int sock);

// Get last error for a socket
int tcp_get_error(int sock);

// Check if connection is established
int tcp_is_connected(int sock);

// Handle incoming TCP packet (called by IP layer)
void tcp_handle(uint32_t src_ip, const void *data, uint16_t length);

// Process TCP timers (call regularly from main loop)
void tcp_timer(void);

// Get state name for debugging
const char* tcp_state_name(tcp_state_t state);
int tcp_diag_snapshot(int *out_active, int *out_counts, int ncounts);

// #404/#349 Task Manager: per-connection snapshot row (read-only view of the
// active TCP table, netstat-style). #487: rows now carry owner_pid, so this is
// no longer a system-wide-only view; conn_filter_by_pid() selects one process's
// connections. The host has a single IP (ip_get_address()); local addr is that.
typedef struct {
    uint8_t  state;        // tcp_state_t
    uint8_t  is_listener;  // 1 if a listening socket
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t remote_ip;
    uint16_t recv_len;     // bytes queued in the receive buffer
    uint32_t send_len;     // bytes queued in the send buffer
    uint32_t owner_pid;    // #487: owning process, 0 = unowned/kernel-internal
} tcp_conn_info_t;

// Mirrored by TcpConnInfo in rustkern.rs, which the live filter indexes as a
// slice. Locked on both sides so the layouts can never drift apart silently.
_Static_assert(sizeof(tcp_conn_info_t) == 24, "tcp_conn_info_t layout changed: update TcpConnInfo in rustkern.rs");

// Fill `out` (up to `max`) with the active connections. Returns count written.
int tcp_conn_snapshot(tcp_conn_info_t *out, int max);

// #487/#349: select the rows of `in` owned by `pid` into `out`, writing at most
// `out_cap` rows. Returns rows written, or -1 on a bad argument. Rust port +
// C reference twin; routed under -DRUST_CONN_OWNER.
int conn_filter_by_pid_rs(const tcp_conn_info_t *in, uint32_t n, uint32_t pid,
                          tcp_conn_info_t *out, uint32_t out_cap);
int conn_filter_by_pid_c(const tcp_conn_info_t *in, uint32_t n, uint32_t pid,
                         tcp_conn_info_t *out, uint32_t out_cap);

#ifdef RUST_CONN_OWNER
#define conn_filter_by_pid(i, n, p, o, c) conn_filter_by_pid_rs((i), (n), (p), (o), (c))
#else
#define conn_filter_by_pid(i, n, p, o, c) conn_filter_by_pid_c((i), (n), (p), (o), (c))
#endif

// #404 boot-time [RUST-DIFF] differential for the conn_owner seam.
void conn_owner_selftest(void);

#endif // TCP_H
