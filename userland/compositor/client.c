// client.c - Client Library Implementation for MayteraOS Compositor
#include "client.h"
#include "../libc/syscall.h"
#include "../libc/string.h"
#include "../libc/stdio.h"
#include "../libc/stdlib.h"

// Well-known compositor channel
#define COMPOSITOR_CHANNEL_ID   1

// ============================================================================
// Connection Management
// ============================================================================

compositor_connection_t *compositor_connect(const char *app_name) {
    compositor_connection_t *conn = (compositor_connection_t *)malloc(sizeof(compositor_connection_t));
    if (!conn) return NULL;
    
    memset(conn, 0, sizeof(compositor_connection_t));
    
    // Connect to compositor channel
    conn->conn_id = msg_connect(COMPOSITOR_CHANNEL_ID);
    if (conn->conn_id < 0) {
        printf("[Client] Failed to connect to compositor\n");
        free(conn);
        return NULL;
    }
    
    // Send connection request
    msg_client_connect_t connect_msg;
    MSG_INIT_HEADER(&connect_msg, MSG_CLIENT_CONNECT, 0);
    connect_msg.protocol_version = COMPOSITOR_PROTOCOL_VERSION;
    strncpy(connect_msg.app_name, app_name, sizeof(connect_msg.app_name) - 1);
    
    if (msg_send(conn->conn_id, &connect_msg, sizeof(connect_msg)) < 0) {
        printf("[Client] Failed to send connect message\n");
        msg_close(conn->conn_id);
        free(conn);
        return NULL;
    }
    
    // Wait for welcome response
    msg_compositor_welcome_t welcome;
    int len = msg_recv(conn->conn_id, &welcome, sizeof(welcome), 5000);
    if (len < sizeof(msg_header_t) || welcome.header.type != MSG_COMPOSITOR_WELCOME) {
        printf("[Client] Did not receive welcome from compositor\n");
        msg_close(conn->conn_id);
        free(conn);
        return NULL;
    }
    
    conn->client_id = welcome.client_id;
    conn->screen_width = welcome.screen_width;
    conn->screen_height = welcome.screen_height;
    conn->connected = true;
    conn->num_windows = 0;
    
    printf("[Client] Connected to compositor (client=%u, screen=%dx%d)\n",
           conn->client_id, conn->screen_width, conn->screen_height);
    
    return conn;
}

void compositor_disconnect(compositor_connection_t *conn) {
    if (!conn) return;
    
    // Destroy all windows
    while (conn->num_windows > 0) {
        window_destroy(conn->windows[conn->num_windows - 1]);
    }
    
    // Send disconnect message
    if (conn->connected) {
        msg_header_t disconnect;
        MSG_INIT_HEADER(&disconnect, MSG_CLIENT_DISCONNECT, 0);
        msg_send(conn->conn_id, &disconnect, sizeof(disconnect));
        msg_close(conn->conn_id);
    }
    
    free(conn);
}

// ============================================================================
// Window Management
// ============================================================================

static compositor_connection_t *g_conn = NULL;  // For finding connection from window

client_window_t *window_create(compositor_connection_t *conn,
                               const char *title,
                               int32_t x, int32_t y,
                               int32_t width, int32_t height,
                               uint32_t flags) {
    if (!conn || !conn->connected) return NULL;
    if (conn->num_windows >= 64) return NULL;
    
    // Send create window request
    msg_window_create_t create;
    MSG_INIT_HEADER(&create, MSG_WINDOW_CREATE, 0);
    create.x = x;
    create.y = y;
    create.width = width;
    create.height = height;
    create.flags = flags | WIN_FLAG_DECORATED | WIN_FLAG_RESIZABLE;
    strncpy(create.title, title, sizeof(create.title) - 1);
    
    if (msg_send(conn->conn_id, &create, sizeof(create)) < 0) {
        printf("[Client] Failed to send window create\n");
        return NULL;
    }
    
    // Wait for response
    msg_window_created_t response;
    int len = msg_recv(conn->conn_id, &response, sizeof(response), 5000);
    if (len < sizeof(msg_header_t) || response.header.type != MSG_WINDOW_CREATED) {
        printf("[Client] Did not receive window created response\n");
        return NULL;
    }
    
    if (response.error != COMP_ERR_NONE) {
        printf("[Client] Window create failed with error %u\n", response.error);
        return NULL;
    }
    
    // Allocate window structure
    client_window_t *win = (client_window_t *)malloc(sizeof(client_window_t));
    if (!win) return NULL;
    
    memset(win, 0, sizeof(client_window_t));
    
    win->id = response.header.window_id;
    win->x = response.x;
    win->y = response.y;
    win->width = response.width;
    win->height = response.height;
    win->flags = flags;
    win->shm_id = response.shm_id;
    
    // Map the shared memory buffer
    if (shm_map(win->shm_id, (void **)&win->buffer) < 0) {
        printf("[Client] Failed to map window buffer\n");
        free(win);
        return NULL;
    }
    
    win->buf_stride = win->width * 4;
    
    // Clear to white
    for (int i = 0; i < win->width * win->height; i++) {
        win->buffer[i] = 0xFFFFFFFF;
    }
    
    // Add to connection's window list
    conn->windows[conn->num_windows++] = win;
    g_conn = conn;  // Remember connection for later
    
    printf("[Client] Created window %u at (%d,%d) %dx%d\n",
           win->id, win->x, win->y, win->width, win->height);
    
    return win;
}

void window_destroy(client_window_t *win) {
    if (!win || !g_conn) return;
    
    // Send destroy message
    msg_header_t destroy;
    MSG_INIT_HEADER(&destroy, MSG_WINDOW_DESTROY, win->id);
    msg_send(g_conn->conn_id, &destroy, sizeof(destroy));
    
    // Unmap buffer
    if (win->buffer) {
        shm_unmap(win->shm_id);
    }
    
    // Remove from connection's list
    for (int i = 0; i < g_conn->num_windows; i++) {
        if (g_conn->windows[i] == win) {
            for (int j = i; j < g_conn->num_windows - 1; j++) {
                g_conn->windows[j] = g_conn->windows[j + 1];
            }
            g_conn->num_windows--;
            break;
        }
    }
    
    free(win);
}

void window_show(client_window_t *win) {
    if (!win || !g_conn) return;
    msg_header_t msg;
    MSG_INIT_HEADER(&msg, MSG_WINDOW_SHOW, win->id);
    msg_send(g_conn->conn_id, &msg, sizeof(msg));
    win->flags |= WIN_FLAG_VISIBLE;
}

void window_hide(client_window_t *win) {
    if (!win || !g_conn) return;
    msg_header_t msg;
    MSG_INIT_HEADER(&msg, MSG_WINDOW_HIDE, win->id);
    msg_send(g_conn->conn_id, &msg, sizeof(msg));
    win->flags &= ~WIN_FLAG_VISIBLE;
}

void window_move(client_window_t *win, int32_t x, int32_t y) {
    if (!win || !g_conn) return;
    msg_window_geometry_t msg;
    MSG_INIT_HEADER(&msg, MSG_WINDOW_MOVE, win->id);
    msg.x = x;
    msg.y = y;
    msg.width = win->width;
    msg.height = win->height;
    msg_send(g_conn->conn_id, &msg, sizeof(msg));
    win->x = x;
    win->y = y;
}

void window_resize(client_window_t *win, int32_t width, int32_t height) {
    if (!win || !g_conn) return;
    
    // For resize, we need to reallocate buffer
    // This is handled by compositor - we just request it
    msg_window_geometry_t msg;
    MSG_INIT_HEADER(&msg, MSG_WINDOW_RESIZE, win->id);
    msg.x = win->x;
    msg.y = win->y;
    msg.width = width;
    msg.height = height;
    msg_send(g_conn->conn_id, &msg, sizeof(msg));
}

void window_set_title(client_window_t *win, const char *title) {
    if (!win || !g_conn) return;
    msg_window_create_t msg;  // Reuse struct for title
    MSG_INIT_HEADER(&msg, MSG_WINDOW_SET_TITLE, win->id);
    strncpy(msg.title, title, sizeof(msg.title) - 1);
    msg_send(g_conn->conn_id, &msg, sizeof(msg));
}

void window_focus(client_window_t *win) {
    if (!win || !g_conn) return;
    msg_header_t msg;
    MSG_INIT_HEADER(&msg, MSG_WINDOW_FOCUS, win->id);
    msg_send(g_conn->conn_id, &msg, sizeof(msg));
}

void window_commit(client_window_t *win) {
    if (!win || !g_conn) return;
    msg_header_t msg;
    MSG_INIT_HEADER(&msg, MSG_BUFFER_COMMIT, win->id);
    msg_send(g_conn->conn_id, &msg, sizeof(msg));
}

void window_damage(client_window_t *win, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!win || !g_conn) return;
    msg_buffer_damage_t msg;
    MSG_INIT_HEADER(&msg, MSG_BUFFER_DAMAGE, win->id);
    msg.x = x;
    msg.y = y;
    msg.width = w;
    msg.height = h;
    msg_send(g_conn->conn_id, &msg, sizeof(msg));
}

// ============================================================================
// Drawing Helpers
// ============================================================================

void window_fill_rect(client_window_t *win, int32_t x, int32_t y,
                      int32_t width, int32_t height, uint32_t color) {
    if (!win || !win->buffer) return;
    
    // Clip to window bounds
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > win->width) width = win->width - x;
    if (y + height > win->height) height = win->height - y;
    if (width <= 0 || height <= 0) return;
    
    for (int32_t py = y; py < y + height; py++) {
        for (int32_t px = x; px < x + width; px++) {
            win->buffer[py * win->width + px] = color;
        }
    }
}

void window_clear(client_window_t *win, uint32_t color) {
    if (!win || !win->buffer) return;
    for (int i = 0; i < win->width * win->height; i++) {
        win->buffer[i] = color;
    }
}

// ============================================================================
// Event Handling
// ============================================================================

static client_window_t *find_window(compositor_connection_t *conn, uint32_t window_id) {
    for (int i = 0; i < conn->num_windows; i++) {
        if (conn->windows[i]->id == window_id) {
            return conn->windows[i];
        }
    }
    return NULL;
}

static void handle_event(compositor_connection_t *conn, msg_header_t *header) {
    client_window_t *win = find_window(conn, header->window_id);
    
    switch (header->type) {
        case MSG_WINDOW_CLOSE_REQUEST:
            if (win && win->on_close) {
                win->on_close(win);
            } else if (win) {
                // Default: destroy window
                window_destroy(win);
            }
            break;
            
        case MSG_WINDOW_CONFIGURE: {
            msg_window_configure_t *cfg = (msg_window_configure_t *)header;
            if (win) {
                // Buffer might need reallocation
                if (cfg->width != win->width || cfg->height != win->height) {
                    // Remap buffer
                    if (win->buffer) {
                        shm_unmap(win->shm_id);
                    }
                    shm_map(win->shm_id, (void **)&win->buffer);
                    
                    win->x = cfg->x;
                    win->y = cfg->y;
                    win->width = cfg->width;
                    win->height = cfg->height;
                    win->buf_stride = win->width * 4;
                    
                    if (win->on_resize) {
                        win->on_resize(win, cfg->width, cfg->height);
                    }
                }
            }
            break;
        }
        
        case MSG_WINDOW_FOCUS_IN:
            if (win) {
                win->flags |= WIN_FLAG_FOCUSED;
                if (win->on_focus) win->on_focus(win, true);
            }
            break;
            
        case MSG_WINDOW_FOCUS_OUT:
            if (win) {
                win->flags &= ~WIN_FLAG_FOCUSED;
                if (win->on_focus) win->on_focus(win, false);
            }
            break;
            
        case MSG_INPUT_MOUSE_MOVE: {
            msg_mouse_move_t *mouse = (msg_mouse_move_t *)header;
            if (win && win->on_mouse_move) {
                win->on_mouse_move(win, mouse->x, mouse->y, mouse->buttons);
            }
            break;
        }
        
        case MSG_INPUT_MOUSE_BUTTON: {
            msg_mouse_button_t *btn = (msg_mouse_button_t *)header;
            if (win && win->on_mouse_button) {
                win->on_mouse_button(win, btn->x, btn->y, btn->button, btn->state != 0);
            }
            break;
        }
        
        case MSG_INPUT_KEY_DOWN:
        case MSG_INPUT_KEY_UP: {
            msg_key_event_t *key = (msg_key_event_t *)header;
            if (win && win->on_key) {
                win->on_key(win, key->keycode, header->type == MSG_INPUT_KEY_DOWN, 
                           key->modifiers);
            }
            break;
        }
    }
}

int compositor_process_events(compositor_connection_t *conn) {
    if (!conn || !conn->connected) return -1;
    
    int count = 0;
    uint8_t buf[4096];
    
    // Non-blocking receive
    int len;
    while ((len = msg_recv(conn->conn_id, buf, sizeof(buf), 0)) > 0) {
        msg_header_t *header = (msg_header_t *)buf;
        handle_event(conn, header);
        count++;
    }
    
    return count;
}

int compositor_wait_events(compositor_connection_t *conn, int timeout) {
    if (!conn || !conn->connected) return -1;
    
    uint8_t buf[4096];
    int len = msg_recv(conn->conn_id, buf, sizeof(buf), timeout);
    
    if (len > 0) {
        msg_header_t *header = (msg_header_t *)buf;
        handle_event(conn, header);
        
        // Process any additional pending events
        return 1 + compositor_process_events(conn);
    }
    
    return 0;
}

void compositor_main_loop(compositor_connection_t *conn) {
    if (!conn) return;
    
    while (conn->connected && conn->num_windows > 0) {
        compositor_wait_events(conn, 100);  // 100ms timeout
    }
}
