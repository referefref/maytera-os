// msg.c - Message Passing IPC Implementation for MayteraOS
// Implements sys_msg_create_channel, sys_msg_connect, sys_msg_send, sys_msg_recv

#include "msg.h"
#include "../proc/process.h"
#include "../mm/heap.h"
#include "../serial.h"
#include "../string.h"
#include "../sync/waitq.h"   // #515: wq_ms_to_ticks() (the one ms->ticks helper)

// External timer for timestamps
extern volatile uint64_t timer_ticks;

// Global channel table
static msg_channel_t msg_channels[MSG_MAX_CHANNELS];

// Global connection ID counter (unique across all channels)
static int next_conn_id = 1;

// ============================================================================
// Internal helpers
// ============================================================================

// Find a channel by ID
static msg_channel_t *msg_get_channel(int channel_id) {
    if (channel_id < 0 || channel_id >= MSG_MAX_CHANNELS) {
        return NULL;
    }
    msg_channel_t *chan = &msg_channels[channel_id];
    if (chan->state == MSG_CHAN_FREE) {
        return NULL;
    }
    return chan;
}

// Find a connection by ID across all channels
static msg_connection_t *msg_get_connection(int conn_id) {
    for (int c = 0; c < MSG_MAX_CHANNELS; c++) {
        msg_channel_t *chan = &msg_channels[c];
        if (chan->state == MSG_CHAN_FREE) continue;

        // Check server connection
        if (chan->server_conn && chan->server_conn->id == conn_id) {
            return chan->server_conn;
        }

        // Check client connections
        for (int i = 0; i < MSG_MAX_CONNECTIONS; i++) {
            if (chan->connections[i].state != MSG_CONN_FREE &&
                chan->connections[i].id == conn_id) {
                return &chan->connections[i];
            }
        }
    }
    return NULL;
}

// Find a free connection slot in a channel
static msg_connection_t *msg_alloc_connection(msg_channel_t *chan) {
    for (int i = 0; i < MSG_MAX_CONNECTIONS; i++) {
        if (chan->connections[i].state == MSG_CONN_FREE) {
            msg_connection_t *conn = &chan->connections[i];
            conn->id = next_conn_id++;
            conn->channel_id = chan->id;
            memset(&conn->recv_queue, 0, sizeof(msg_queue_t));
            return conn;
        }
    }
    return NULL;
}

// Queue a message to a connection
static int msg_queue_message(msg_connection_t *conn, const void *data, size_t len,
                             uint32_t sender_pid) {
    msg_queue_t *q = &conn->recv_queue;

    if (q->count >= MSG_QUEUE_SIZE) {
        kprintf("[MSG] Queue full for conn %d, dropping message\n", conn->id);
        return -1;  // Queue full
    }

    msg_t *msg = &q->messages[q->tail];
    msg->header.size = (len > MSG_MAX_SIZE) ? MSG_MAX_SIZE : (uint32_t)len;
    msg->header.sender_pid = sender_pid;
    msg->header.timestamp = timer_ticks;
    memcpy(msg->data, data, msg->header.size);

    q->tail = (q->tail + 1) % MSG_QUEUE_SIZE;
    q->count++;

    return 0;
}

// Dequeue a message from a connection
static int msg_dequeue_message(msg_connection_t *conn, void *buf, size_t max_len,
                               msg_header_t *header_out) {
    msg_queue_t *q = &conn->recv_queue;

    if (q->count == 0) {
        return 0;  // No messages
    }

    msg_t *msg = &q->messages[q->head];
    size_t copy_len = (msg->header.size < max_len) ? msg->header.size : max_len;

    if (header_out) {
        *header_out = msg->header;
    }
    memcpy(buf, msg->data, copy_len);

    q->head = (q->head + 1) % MSG_QUEUE_SIZE;
    q->count--;

    return (int)copy_len;
}

// ============================================================================
// Message Passing API Implementation
// ============================================================================

void msg_init(void) {
    kprintf("[MSG] Initializing message passing subsystem...\n");

    // Clear all channels
    memset(msg_channels, 0, sizeof(msg_channels));

    // Initialize channel IDs
    for (int i = 0; i < MSG_MAX_CHANNELS; i++) {
        msg_channels[i].id = i;
        msg_channels[i].state = MSG_CHAN_FREE;
    }

    kprintf("[MSG] Message passing initialized (%d channels, %d conns/channel, %d msg size)\n",
            MSG_MAX_CHANNELS, MSG_MAX_CONNECTIONS, MSG_MAX_SIZE);
}

int64_t sys_msg_create_channel(void) {
    process_t *p = proc_current();
    if (!p) {
        kprintf("[MSG] ERROR: sys_msg_create_channel - no current process\n");
        return -1;
    }

    // Find a free channel
    msg_channel_t *chan = NULL;
    for (int i = 0; i < MSG_MAX_CHANNELS; i++) {
        if (msg_channels[i].state == MSG_CHAN_FREE) {
            chan = &msg_channels[i];
            break;
        }
    }

    if (!chan) {
        kprintf("[MSG] ERROR: sys_msg_create_channel - no free channels\n");
        return -1;
    }

    // Allocate server connection
    msg_connection_t *server_conn = (msg_connection_t *)kmalloc(sizeof(msg_connection_t));
    if (!server_conn) {
        kprintf("[MSG] ERROR: sys_msg_create_channel - failed to allocate server conn\n");
        return -1;
    }

    // Initialize server connection
    memset(server_conn, 0, sizeof(msg_connection_t));
    server_conn->id = next_conn_id++;
    server_conn->state = MSG_CONN_CONNECTED;
    server_conn->channel_id = chan->id;
    server_conn->owner_pid = p->pid;
    server_conn->peer = NULL;  // Server doesn't have a single peer

    // Initialize channel
    chan->state = MSG_CHAN_ACTIVE;
    chan->creator_pid = p->pid;
    chan->server_conn = server_conn;
    chan->num_connections = 0;

    kprintf("[MSG] Created channel %d by pid %u (server conn=%d)\n",
            chan->id, p->pid, server_conn->id);

    return chan->id;
}

int64_t sys_msg_connect(int channel_id) {
    process_t *p = proc_current();
    if (!p) {
        kprintf("[MSG] ERROR: sys_msg_connect - no current process\n");
        return -1;
    }

    // Get channel
    msg_channel_t *chan = msg_get_channel(channel_id);
    if (!chan) {
        kprintf("[MSG] ERROR: sys_msg_connect - invalid channel %d\n", channel_id);
        return -1;
    }

    // Allocate client connection
    msg_connection_t *client_conn = msg_alloc_connection(chan);
    if (!client_conn) {
        kprintf("[MSG] ERROR: sys_msg_connect - no free connections in channel %d\n", channel_id);
        return -1;
    }

    // Initialize client connection
    client_conn->state = MSG_CONN_CONNECTED;
    client_conn->owner_pid = p->pid;
    client_conn->peer_pid = chan->creator_pid;
    client_conn->peer = chan->server_conn;  // Connected to server

    chan->num_connections++;

    kprintf("[MSG] Connected to channel %d: conn=%d, pid=%u -> server_pid=%u\n",
            channel_id, client_conn->id, p->pid, chan->creator_pid);

    return client_conn->id;
}

int64_t sys_msg_send(int conn_id, const void *data, size_t len) {
    process_t *p = proc_current();
    if (!p) {
        kprintf("[MSG] ERROR: sys_msg_send - no current process\n");
        return -1;
    }

    // Validate length
    if (len > MSG_MAX_SIZE) {
        kprintf("[MSG] ERROR: sys_msg_send - message too large (%lu > %d)\n",
                (uint64_t)len, MSG_MAX_SIZE);
        return -1;
    }

    // Find connection
    msg_connection_t *conn = msg_get_connection(conn_id);
    if (!conn) {
        kprintf("[MSG] ERROR: sys_msg_send - invalid connection %d\n", conn_id);
        return -1;
    }

    // Verify ownership
    if (conn->owner_pid != p->pid) {
        kprintf("[MSG] ERROR: sys_msg_send - conn %d not owned by pid %u\n",
                conn_id, p->pid);
        return -1;
    }

    // Check connection state
    if (conn->state != MSG_CONN_CONNECTED) {
        kprintf("[MSG] ERROR: sys_msg_send - connection %d not connected\n", conn_id);
        return -1;
    }

    // Get channel for routing
    msg_channel_t *chan = msg_get_channel(conn->channel_id);
    if (!chan) {
        kprintf("[MSG] ERROR: sys_msg_send - channel gone for conn %d\n", conn_id);
        return -1;
    }

    // Determine destination
    // If sender is server, message goes to all clients (broadcast)
    // If sender is client, message goes to server
    int sent = 0;

    if (conn == chan->server_conn) {
        // Server sending - for now just reply to last sender
        // A proper implementation would track which client to reply to
        // For simplicity, broadcast to all connected clients
        for (int i = 0; i < MSG_MAX_CONNECTIONS; i++) {
            msg_connection_t *client = &chan->connections[i];
            if (client->state == MSG_CONN_CONNECTED) {
                if (msg_queue_message(client, data, len, p->pid) == 0) {
                    sent++;
                }
            }
        }
    } else {
        // Client sending to server
        if (chan->server_conn && chan->server_conn->state == MSG_CONN_CONNECTED) {
            if (msg_queue_message(chan->server_conn, data, len, p->pid) == 0) {
                sent = 1;
            }
        }
    }

    if (sent == 0) {
        kprintf("[MSG] WARNING: sys_msg_send - no recipients for conn %d\n", conn_id);
    }

    return (int64_t)len;  // Return bytes "sent" (queued)
}

int64_t sys_msg_recv(int conn_id, void *buf, size_t len, int timeout) {
    process_t *p = proc_current();
    if (!p) {
        kprintf("[MSG] ERROR: sys_msg_recv - no current process\n");
        return -1;
    }

    // Find connection
    msg_connection_t *conn = msg_get_connection(conn_id);
    if (!conn) {
        kprintf("[MSG] ERROR: sys_msg_recv - invalid connection %d\n", conn_id);
        return -1;
    }

    // Verify ownership
    if (conn->owner_pid != p->pid) {
        kprintf("[MSG] ERROR: sys_msg_recv - conn %d not owned by pid %u\n",
                conn_id, p->pid);
        return -1;
    }

    // Check connection state
    if (conn->state != MSG_CONN_CONNECTED) {
        kprintf("[MSG] ERROR: sys_msg_recv - connection %d not connected\n", conn_id);
        return -1;
    }

    // Try to receive immediately
    msg_header_t header;
    int bytes = msg_dequeue_message(conn, buf, len, &header);
    if (bytes > 0) {
        return bytes;
    }

    // Non-blocking mode
    if (timeout == 0) {
        return 0;  // No message available
    }

    // Blocking mode - wait for message
    //
    // #515: this proc_yield() spin is a known #426 violation and SHOULD park on
    // a conn->recv_wq woken from msg_queue_message(). It is deliberately NOT
    // converted yet: msg_cleanup_process() (on owner exit) and
    // sys_msg_destroy_channel() both kfree(chan->server_conn) unconditionally,
    // with no refcount and no wake. A wait queue inside that struct would be
    // freed under a parked waiter, whose __wait_finish() then takes a spinlock
    // on freed memory: a use-after-free WRITE, strictly worse than the
    // pre-existing UAF read of conn->state this loop already performs. Fix the
    // connection lifetime (#515) first, then convert. See the internal wait-migration plan.
    //
    // Units fixed now (independent of the above, and the #420/#512 bug family):
    // the deadline was `(uint64_t)timeout / 10`, i.e. milliseconds/10, which is
    // only ticks on a 100Hz build. At the actual g_timer_hz of 250 it expired
    // 2.5x EARLY, so a caller asking for 1000ms got ~400ms. Now converted by the
    // one sanctioned helper against the live tick rate.
    uint64_t start = timer_ticks;
    uint64_t timeout_ticks = (timeout > 0) ? wq_ms_to_ticks((uint64_t)timeout)
                                           : 0xFFFFFFFFFFFFFFFFULL;

    while (1) {
        // Check for message
        bytes = msg_dequeue_message(conn, buf, len, &header);
        if (bytes > 0) {
            return bytes;
        }

        // Check timeout
        if (timeout > 0 && (timer_ticks - start) >= timeout_ticks) {
            return 0;  // Timeout
        }

        // Yield CPU
        proc_yield();

        // Check if connection closed
        if (conn->state != MSG_CONN_CONNECTED) {
            return -1;
        }
    }
}

int64_t sys_msg_close(int conn_id) {
    process_t *p = proc_current();
    if (!p) {
        kprintf("[MSG] ERROR: sys_msg_close - no current process\n");
        return -1;
    }

    // Find connection
    msg_connection_t *conn = msg_get_connection(conn_id);
    if (!conn) {
        kprintf("[MSG] ERROR: sys_msg_close - invalid connection %d\n", conn_id);
        return -1;
    }

    // Verify ownership
    if (conn->owner_pid != p->pid) {
        kprintf("[MSG] ERROR: sys_msg_close - conn %d not owned by pid %u\n",
                conn_id, p->pid);
        return -1;
    }

    msg_channel_t *chan = msg_get_channel(conn->channel_id);

    // If this is a server connection, close the whole channel
    if (chan && conn == chan->server_conn) {
        kprintf("[MSG] Closing server connection %d closes channel %d\n",
                conn_id, chan->id);
        return sys_msg_destroy_channel(chan->id);
    }

    // Close client connection
    conn->state = MSG_CONN_CLOSED;

    if (chan) {
        chan->num_connections--;
    }

    kprintf("[MSG] Closed connection %d\n", conn_id);

    return 0;
}

int64_t sys_msg_destroy_channel(int channel_id) {
    process_t *p = proc_current();
    if (!p) {
        kprintf("[MSG] ERROR: sys_msg_destroy_channel - no current process\n");
        return -1;
    }

    msg_channel_t *chan = msg_get_channel(channel_id);
    if (!chan) {
        kprintf("[MSG] ERROR: sys_msg_destroy_channel - invalid channel %d\n", channel_id);
        return -1;
    }

    // Only creator can destroy
    if (chan->creator_pid != p->pid) {
        kprintf("[MSG] ERROR: sys_msg_destroy_channel - pid %u not creator of channel %d\n",
                p->pid, channel_id);
        return -1;
    }

    // Close all client connections
    for (int i = 0; i < MSG_MAX_CONNECTIONS; i++) {
        if (chan->connections[i].state != MSG_CONN_FREE) {
            chan->connections[i].state = MSG_CONN_CLOSED;
        }
    }

    // Free server connection
    if (chan->server_conn) {
        kfree(chan->server_conn);
        chan->server_conn = NULL;
    }

    // Mark channel as free
    chan->state = MSG_CHAN_FREE;
    chan->creator_pid = 0;
    chan->num_connections = 0;

    kprintf("[MSG] Destroyed channel %d\n", channel_id);

    return 0;
}

int64_t sys_msg_accept(int channel_id, int timeout) {
    (void)timeout;  // TODO: Implement proper connection accept with timeout

    process_t *p = proc_current();
    if (!p) {
        kprintf("[MSG] ERROR: sys_msg_accept - no current process\n");
        return -1;
    }

    msg_channel_t *chan = msg_get_channel(channel_id);
    if (!chan) {
        kprintf("[MSG] ERROR: sys_msg_accept - invalid channel %d\n", channel_id);
        return -1;
    }

    // Only server can accept
    if (chan->creator_pid != p->pid) {
        kprintf("[MSG] ERROR: sys_msg_accept - pid %u not server for channel %d\n",
                p->pid, channel_id);
        return -1;
    }

    // In this simple implementation, connections are auto-accepted in sys_msg_connect
    // sys_msg_accept just returns the server's connection ID to receive messages
    // A more complete implementation would have pending connections

    if (chan->server_conn) {
        return chan->server_conn->id;
    }

    return -1;
}

int64_t sys_msg_channel_info(int channel_id, uint32_t *num_conns) {
    msg_channel_t *chan = msg_get_channel(channel_id);
    if (!chan) {
        return -1;
    }

    if (num_conns) {
        *num_conns = chan->num_connections;
    }

    return 0;
}

void msg_cleanup_process(uint32_t pid) {
    kprintf("[MSG] Cleaning up message channels for pid %u\n", pid);

    for (int c = 0; c < MSG_MAX_CHANNELS; c++) {
        msg_channel_t *chan = &msg_channels[c];
        if (chan->state == MSG_CHAN_FREE) continue;

        // If this process owns the channel, destroy it
        if (chan->creator_pid == pid) {
            // Close all connections
            for (int i = 0; i < MSG_MAX_CONNECTIONS; i++) {
                chan->connections[i].state = MSG_CONN_FREE;
            }

            if (chan->server_conn) {
                kfree(chan->server_conn);
                chan->server_conn = NULL;
            }

            chan->state = MSG_CHAN_FREE;
            chan->creator_pid = 0;
            chan->num_connections = 0;

            kprintf("[MSG] Auto-destroyed channel %d (owner exited)\n", c);
            continue;
        }

        // Close any connections owned by this process
        for (int i = 0; i < MSG_MAX_CONNECTIONS; i++) {
            msg_connection_t *conn = &chan->connections[i];
            if (conn->state != MSG_CONN_FREE && conn->owner_pid == pid) {
                conn->state = MSG_CONN_FREE;
                chan->num_connections--;
                kprintf("[MSG] Closed orphan connection %d in channel %d\n", conn->id, c);
            }
        }
    }
}


// ============================================================================
// IPC Name Service
// ============================================================================

#define IPC_NAME_MAX_LEN     64
#define IPC_NAME_MAX_ENTRIES 32

typedef struct {
    char     name[IPC_NAME_MAX_LEN];
    int      channel_id;
    uint32_t owner_pid;
    int      active;
} ipc_name_entry_t;

static ipc_name_entry_t g_name_table[IPC_NAME_MAX_ENTRIES];
static int g_name_table_inited = 0;

int64_t sys_ipc_register_name(const char *name, int channel_id) {
    if (!name || channel_id < 0) return -1;
    if (!g_name_table_inited) {
        for (int i = 0; i < IPC_NAME_MAX_ENTRIES; i++) g_name_table[i].active = 0;
        g_name_table_inited = 1;
    }
    process_t *p = proc_current();
    uint32_t pid = p ? p->pid : 0;

    // Check if name already registered
    for (int i = 0; i < IPC_NAME_MAX_ENTRIES; i++) {
        if (g_name_table[i].active &&
            strncmp(g_name_table[i].name, name, IPC_NAME_MAX_LEN) == 0) {
            if (g_name_table[i].owner_pid == pid) {
                g_name_table[i].channel_id = channel_id;
                return 0;
            }
            return -1;
        }
    }
    for (int i = 0; i < IPC_NAME_MAX_ENTRIES; i++) {
        if (!g_name_table[i].active) {
            strncpy(g_name_table[i].name, name, IPC_NAME_MAX_LEN - 1);
            g_name_table[i].name[IPC_NAME_MAX_LEN - 1] = '\0';
            g_name_table[i].channel_id = channel_id;
            g_name_table[i].owner_pid = pid;
            g_name_table[i].active = 1;
            return 0;
        }
    }
    return -1;
}

int64_t sys_ipc_lookup_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < IPC_NAME_MAX_ENTRIES; i++) {
        if (g_name_table[i].active &&
            strncmp(g_name_table[i].name, name, IPC_NAME_MAX_LEN) == 0) {
            return g_name_table[i].channel_id;
        }
    }
    return -1;
}
