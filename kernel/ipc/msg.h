// msg.h - Message Passing IPC for MayteraOS
// Implements sys_msg_create_channel, sys_msg_connect, sys_msg_send, sys_msg_recv
#ifndef IPC_MSG_H
#define IPC_MSG_H

#include "../types.h"

// Maximum number of message channels
#define MSG_MAX_CHANNELS        16

// Maximum number of connections per channel
#define MSG_MAX_CONNECTIONS     8

// Maximum message size
#define MSG_MAX_SIZE            4096

// Message queue size (number of messages per connection)
#define MSG_QUEUE_SIZE          4

// Connection states
typedef enum {
    MSG_CONN_FREE = 0,      // Connection slot is free
    MSG_CONN_PENDING,       // Connection request pending
    MSG_CONN_CONNECTED,     // Connected and active
    MSG_CONN_CLOSED,        // Connection closed
} msg_conn_state_t;

// Channel states
typedef enum {
    MSG_CHAN_FREE = 0,      // Channel slot is free
    MSG_CHAN_ACTIVE,        // Channel is active
} msg_chan_state_t;

// Message header
typedef struct {
    uint32_t size;          // Data size (not including header)
    uint32_t sender_pid;    // Sender process ID
    uint64_t timestamp;     // When message was sent (timer ticks)
} msg_header_t;

// Message in queue
typedef struct {
    msg_header_t header;
    uint8_t data[MSG_MAX_SIZE];
} msg_t;

// Message queue
typedef struct {
    msg_t messages[MSG_QUEUE_SIZE];
    uint32_t head;          // Read position
    uint32_t tail;          // Write position
    uint32_t count;         // Number of messages in queue
} msg_queue_t;

// Connection descriptor
typedef struct msg_connection {
    int id;                         // Connection ID
    msg_conn_state_t state;         // Connection state
    int channel_id;                 // Associated channel
    uint32_t owner_pid;             // Process that owns this connection
    uint32_t peer_pid;              // Process on the other end (for bidirectional)
    msg_queue_t recv_queue;         // Incoming message queue
    struct msg_connection *peer;    // Peer connection (for bidirectional channels)
    // #426 NOTE: a recv_wq belongs here, and sys_msg_recv() should park on it
    // instead of its proc_yield() spin. It is NOT safe to add yet: see #515.
    // msg_cleanup_process()/sys_msg_destroy_channel() kfree() the server
    // connection unconditionally, so a wait queue living in this struct would
    // be freed under any waiter parked on it. Fix the lifetime first.
} msg_connection_t;

// Channel descriptor
typedef struct {
    int id;                             // Channel ID
    msg_chan_state_t state;             // Channel state
    uint32_t creator_pid;               // Process that created the channel
    msg_connection_t *server_conn;      // Server-side connection
    msg_connection_t connections[MSG_MAX_CONNECTIONS];  // Client connections
    uint32_t num_connections;           // Number of active connections
} msg_channel_t;

// ============================================================================
// Message Passing API
// ============================================================================

/**
 * Initialize the message passing subsystem
 * Called once during kernel startup
 */
void msg_init(void);

/**
 * Create a new message channel (server-side)
 * The creating process becomes the "server" for this channel
 * @return          Channel ID (>= 0), or -1 on failure
 */
int64_t sys_msg_create_channel(void);

/**
 * Connect to an existing channel (client-side)
 * Creates a bidirectional connection to the channel's server
 * @param channel_id    Channel to connect to
 * @return              Connection ID (>= 0), or -1 on failure
 */
int64_t sys_msg_connect(int channel_id);

/**
 * Send a message on a connection
 * @param conn_id       Connection ID
 * @param data          Pointer to message data
 * @param len           Length of message data (max MSG_MAX_SIZE)
 * @return              Number of bytes sent, or -1 on failure
 */
int64_t sys_msg_send(int conn_id, const void *data, size_t len);

/**
 * Receive a message on a connection
 * @param conn_id       Connection ID
 * @param buf           Buffer to receive message data
 * @param len           Maximum bytes to receive
 * @param timeout       Timeout in milliseconds (-1 = infinite, 0 = non-blocking)
 * @return              Number of bytes received, 0 on timeout, -1 on error
 */
int64_t sys_msg_recv(int conn_id, void *buf, size_t len, int timeout);

/**
 * Close a connection
 * @param conn_id       Connection ID
 * @return              0 on success, -1 on failure
 */
int64_t sys_msg_close(int conn_id);

/**
 * Destroy a channel (server only)
 * All connections are closed
 * @param channel_id    Channel ID
 * @return              0 on success, -1 on failure
 */
int64_t sys_msg_destroy_channel(int channel_id);

/**
 * Accept a pending connection (server-side)
 * Returns connection ID for communicating with the client
 * @param channel_id    Channel ID
 * @param timeout       Timeout in milliseconds (-1 = infinite, 0 = non-blocking)
 * @return              Connection ID (>= 0), 0 on timeout, -1 on error
 */
int64_t sys_msg_accept(int channel_id, int timeout);

/**
 * Get channel info
 * @param channel_id    Channel ID
 * @param num_conns     Output: number of active connections (optional)
 * @return              0 on success, -1 on failure
 */
int64_t sys_msg_channel_info(int channel_id, uint32_t *num_conns);

/**
 * Clean up all message channels/connections for a process
 * Called automatically when a process terminates
 * @param pid           Process ID being cleaned up
 */
void msg_cleanup_process(uint32_t pid);


// IPC Name Service
// Register a service name (max 64 chars) bound to a channel
int64_t sys_ipc_register_name(const char *name, int channel_id);
// Lookup a registered name, returns channel_id or -1
int64_t sys_ipc_lookup_name(const char *name);

#endif // IPC_MSG_H
