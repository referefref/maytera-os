// compositor_client.c - Window compositor IPC client implementation
// Connects to user-mode compositor via message passing and shared memory

#include "compositor_client.h"
#include "string.h"
#include "stdlib.h"

// ============================================================================
// Global Client State
// ============================================================================

static comp_client_t g_client = {
    .conn_id = -1,
    .client_id = 0,
    .screen_width = 0,
    .screen_height = 0,
    .pixel_format = 0,
    .seq = 0,
    .connected = false
};

// Track all windows
#define MAX_CLIENT_WINDOWS 16
static comp_window_t *g_windows[MAX_CLIENT_WINDOWS] = {0};
static int g_window_count = 0;

// Event loop control
static bool g_running = false;

// ============================================================================
// Internal Helper Functions
// ============================================================================

static uint32_t next_seq(void) {
    return ++g_client.seq;
}

// Find window by ID
static comp_window_t *find_window(uint32_t window_id) {
    for (int i = 0; i < g_window_count; i++) {
        if (g_windows[i] && g_windows[i]->window_id == window_id) {
            return g_windows[i];
        }
    }
    return NULL;
}

// Add window to list
static int add_window(comp_window_t *win) {
    for (int i = 0; i < MAX_CLIENT_WINDOWS; i++) {
        if (g_windows[i] == NULL) {
            g_windows[i] = win;
            if (i >= g_window_count) {
                g_window_count = i + 1;
            }
            return 0;
        }
    }
    return -1;
}

// Remove window from list
static void remove_window(comp_window_t *win) {
    for (int i = 0; i < g_window_count; i++) {
        if (g_windows[i] == win) {
            g_windows[i] = NULL;
            break;
        }
    }
    // Compact if last window removed
    while (g_window_count > 0 && g_windows[g_window_count - 1] == NULL) {
        g_window_count--;
    }
}

// Send message and wait for response
static long send_recv(const void *msg, size_t msg_len, void *resp, size_t resp_len) {
    if (!g_client.connected || g_client.conn_id < 0) {
        return -1;
    }
    
    long sent = msg_send(g_client.conn_id, msg, msg_len);
    if (sent < 0) {
        return -1;
    }
    
    // Wait for response (1 second timeout)
    long received = msg_recv(g_client.conn_id, resp, resp_len, 1000);
    return received;
}

// Send message without waiting for response
static long send_msg(const void *msg, size_t msg_len) {
    if (!g_client.connected || g_client.conn_id < 0) {
        return -1;
    }
    return msg_send(g_client.conn_id, msg, msg_len);
}

// Copy string with length limit
static void safe_strcpy(char *dst, const char *src, size_t max_len) {
    size_t i;
    for (i = 0; i < max_len - 1 && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

// ============================================================================
// Client Initialization
// ============================================================================

int comp_init(void) {
    if (g_client.connected) {
        return 0;  // Already connected
    }
    
    // Connect to compositor channel
    int conn = msg_connect(COMPOSITOR_CHANNEL_ID);
    if (conn < 0) {
        return -1;
    }
    
    g_client.conn_id = conn;
    
    // Send connect message
    comp_msg_connect_t connect_msg = {
        .header = {
            .type = MSG_CONNECT,
            .window_id = 0,
            .seq = next_seq(),
            .size = sizeof(comp_msg_connect_t)
        },
        .protocol_version = 1
    };
    safe_strcpy(connect_msg.client_name, "MayteraApp", sizeof(connect_msg.client_name));
    
    comp_msg_connect_response_t response;
    long result = send_recv(&connect_msg, sizeof(connect_msg), 
                           &response, sizeof(response));
    
    if (result < 0 || response.header.type != MSG_CONNECT) {
        msg_close(conn);
        g_client.conn_id = -1;
        return -1;
    }
    
    g_client.client_id = response.client_id;
    g_client.screen_width = response.screen_width;
    g_client.screen_height = response.screen_height;
    g_client.pixel_format = response.pixel_format;
    g_client.connected = true;
    
    return 0;
}

void comp_shutdown(void) {
    if (!g_client.connected) {
        return;
    }
    
    // Destroy all windows
    for (int i = 0; i < g_window_count; i++) {
        if (g_windows[i]) {
            comp_window_destroy(g_windows[i]);
        }
    }
    
    // Send disconnect message
    comp_msg_header_t disconnect_msg = {
        .type = MSG_DISCONNECT,
        .window_id = 0,
        .seq = next_seq(),
        .size = sizeof(comp_msg_header_t)
    };
    send_msg(&disconnect_msg, sizeof(disconnect_msg));
    
    // Close connection
    msg_close(g_client.conn_id);
    
    g_client.conn_id = -1;
    g_client.connected = false;
}

bool comp_is_connected(void) {
    return g_client.connected;
}

void comp_get_screen_size(uint32_t *width, uint32_t *height) {
    if (width) *width = g_client.screen_width;
    if (height) *height = g_client.screen_height;
}

// ============================================================================
// Window Management
// ============================================================================

comp_window_t *comp_window_create(const char *title, int x, int y,
                                   uint32_t width, uint32_t height,
                                   uint32_t flags) {
    if (!g_client.connected) {
        if (comp_init() != 0) {
            return NULL;
        }
    }
    
    // Allocate window structure
    comp_window_t *win = (comp_window_t *)malloc(sizeof(comp_window_t));
    if (!win) {
        return NULL;
    }
    
    memset(win, 0, sizeof(comp_window_t));
    
    // Send create window message
    comp_msg_create_window_t create_msg = {
        .header = {
            .type = MSG_CREATE_WINDOW,
            .window_id = 0,
            .seq = next_seq(),
            .size = sizeof(comp_msg_create_window_t)
        },
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .flags = flags
    };
    if (title) {
        safe_strcpy(create_msg.title, title, COMP_MAX_TITLE);
    }
    
    comp_msg_create_window_response_t response;
    long result = send_recv(&create_msg, sizeof(create_msg),
                           &response, sizeof(response));
    
    if (result < 0 || response.result < 0) {
        free(win);
        return NULL;
    }
    
    // Initialize window state
    win->window_id = response.window_id;
    win->x = (x < 0) ? 0 : x;  // Compositor may adjust position
    win->y = (y < 0) ? 0 : y;
    win->width = width;
    win->height = height;
    win->flags = flags;
    if (title) {
        safe_strcpy(win->title, title, COMP_MAX_TITLE);
    }
    
    // Create double buffers
    win->buffer_count = COMP_DEFAULT_BUFFERS;
    win->front_buffer = 0;
    win->back_buffer = 1;
    
    size_t buffer_size = width * height * 4;  // ARGB32
    
    for (int i = 0; i < win->buffer_count; i++) {
        comp_buffer_t *buf = &win->buffers[i];
        
        // Create shared memory for buffer
        buf->shm_id = shm_create(buffer_size, SHM_FLAG_NONE);
        if (buf->shm_id < 0) {
            // Cleanup on failure
            for (int j = 0; j < i; j++) {
                if (win->buffers[j].pixels) {
                    shm_unmap(win->buffers[j].shm_id);
                }
                shm_destroy(win->buffers[j].shm_id);
            }
            free(win);
            return NULL;
        }
        
        // Map shared memory
        void *addr = NULL;
        if (shm_map(buf->shm_id, &addr) != 0) {
            shm_destroy(buf->shm_id);
            for (int j = 0; j < i; j++) {
                if (win->buffers[j].pixels) {
                    shm_unmap(win->buffers[j].shm_id);
                }
                shm_destroy(win->buffers[j].shm_id);
            }
            free(win);
            return NULL;
        }
        
        buf->pixels = (uint32_t *)addr;
        buf->width = width;
        buf->height = height;
        buf->size = buffer_size;
        buf->in_use = false;
        
        // Register buffer with compositor
        comp_msg_create_buffer_t buf_msg = {
            .header = {
                .type = MSG_CREATE_BUFFER,
                .window_id = win->window_id,
                .seq = next_seq(),
                .size = sizeof(comp_msg_create_buffer_t)
            },
            .width = width,
            .height = height,
            .format = 0,  // ARGB32
            .shm_id = buf->shm_id
        };
        
        comp_msg_create_buffer_response_t buf_response;
        result = send_recv(&buf_msg, sizeof(buf_msg),
                          &buf_response, sizeof(buf_response));
        
        if (result >= 0 && buf_response.result >= 0) {
            buf->buffer_id = buf_response.buffer_id;
        }
    }
    
    // Add to window list
    if (add_window(win) != 0) {
        comp_window_destroy(win);
        return NULL;
    }
    
    return win;
}

void comp_window_destroy(comp_window_t *win) {
    if (!win) return;
    
    // Send destroy message
    if (g_client.connected) {
        comp_msg_header_t destroy_msg = {
            .type = MSG_DESTROY_WINDOW,
            .window_id = win->window_id,
            .seq = next_seq(),
            .size = sizeof(comp_msg_header_t)
        };
        send_msg(&destroy_msg, sizeof(destroy_msg));
    }
    
    // Free buffers
    for (int i = 0; i < win->buffer_count; i++) {
        comp_buffer_t *buf = &win->buffers[i];
        if (buf->pixels) {
            shm_unmap(buf->shm_id);
        }
        if (buf->shm_id >= 0) {
            shm_destroy(buf->shm_id);
        }
    }
    
    // Remove from window list
    remove_window(win);
    
    free(win);
}

int comp_window_set_title(comp_window_t *win, const char *title) {
    if (!win || !title) return -1;
    
    comp_msg_set_title_t msg = {
        .header = {
            .type = MSG_SET_TITLE,
            .window_id = win->window_id,
            .seq = next_seq(),
            .size = sizeof(comp_msg_set_title_t)
        }
    };
    safe_strcpy(msg.title, title, COMP_MAX_TITLE);
    safe_strcpy(win->title, title, COMP_MAX_TITLE);
    
    return (send_msg(&msg, sizeof(msg)) >= 0) ? 0 : -1;
}

int comp_window_move(comp_window_t *win, int x, int y) {
    if (!win) return -1;
    
    comp_msg_move_t msg = {
        .header = {
            .type = MSG_MOVE,
            .window_id = win->window_id,
            .seq = next_seq(),
            .size = sizeof(comp_msg_move_t)
        },
        .x = x,
        .y = y
    };
    
    if (send_msg(&msg, sizeof(msg)) >= 0) {
        win->x = x;
        win->y = y;
        return 0;
    }
    return -1;
}

int comp_window_resize(comp_window_t *win, uint32_t width, uint32_t height) {
    if (!win || width == 0 || height == 0) return -1;
    
    // Send resize message
    comp_msg_resize_t msg = {
        .header = {
            .type = MSG_RESIZE,
            .window_id = win->window_id,
            .seq = next_seq(),
            .size = sizeof(comp_msg_resize_t)
        },
        .width = width,
        .height = height
    };
    
    if (send_msg(&msg, sizeof(msg)) < 0) {
        return -1;
    }
    
    // Reallocate buffers
    size_t new_size = width * height * 4;
    
    for (int i = 0; i < win->buffer_count; i++) {
        comp_buffer_t *buf = &win->buffers[i];
        
        // Unmap old buffer
        if (buf->pixels) {
            shm_unmap(buf->shm_id);
            buf->pixels = NULL;
        }
        
        // Destroy old shared memory
        if (buf->shm_id >= 0) {
            shm_destroy(buf->shm_id);
        }
        
        // Create new shared memory
        buf->shm_id = shm_create(new_size, SHM_FLAG_NONE);
        if (buf->shm_id < 0) {
            return -1;
        }
        
        // Map new buffer
        void *addr = NULL;
        if (shm_map(buf->shm_id, &addr) != 0) {
            shm_destroy(buf->shm_id);
            buf->shm_id = -1;
            return -1;
        }
        
        buf->pixels = (uint32_t *)addr;
        buf->width = width;
        buf->height = height;
        buf->size = new_size;
        
        // Re-register buffer with compositor
        comp_msg_create_buffer_t buf_msg = {
            .header = {
                .type = MSG_CREATE_BUFFER,
                .window_id = win->window_id,
                .seq = next_seq(),
                .size = sizeof(comp_msg_create_buffer_t)
            },
            .width = width,
            .height = height,
            .format = 0,
            .shm_id = buf->shm_id
        };
        
        comp_msg_create_buffer_response_t buf_response;
        send_recv(&buf_msg, sizeof(buf_msg), &buf_response, sizeof(buf_response));
        if (buf_response.result >= 0) {
            buf->buffer_id = buf_response.buffer_id;
        }
    }
    
    win->width = width;
    win->height = height;
    
    return 0;
}

int comp_window_show(comp_window_t *win) {
    if (!win) return -1;
    
    comp_msg_header_t msg = {
        .type = MSG_SHOW,
        .window_id = win->window_id,
        .seq = next_seq(),
        .size = sizeof(comp_msg_header_t)
    };
    
    if (send_msg(&msg, sizeof(msg)) >= 0) {
        win->flags |= COMP_FLAG_VISIBLE;
        return 0;
    }
    return -1;
}

int comp_window_hide(comp_window_t *win) {
    if (!win) return -1;
    
    comp_msg_header_t msg = {
        .type = MSG_HIDE,
        .window_id = win->window_id,
        .seq = next_seq(),
        .size = sizeof(comp_msg_header_t)
    };
    
    if (send_msg(&msg, sizeof(msg)) >= 0) {
        win->flags &= ~COMP_FLAG_VISIBLE;
        return 0;
    }
    return -1;
}

int comp_window_minimize(comp_window_t *win) {
    if (!win) return -1;
    
    comp_msg_header_t msg = {
        .type = MSG_MINIMIZE,
        .window_id = win->window_id,
        .seq = next_seq(),
        .size = sizeof(comp_msg_header_t)
    };
    
    return (send_msg(&msg, sizeof(msg)) >= 0) ? 0 : -1;
}

int comp_window_maximize(comp_window_t *win) {
    if (!win) return -1;
    
    comp_msg_header_t msg = {
        .type = MSG_MAXIMIZE,
        .window_id = win->window_id,
        .seq = next_seq(),
        .size = sizeof(comp_msg_header_t)
    };
    
    return (send_msg(&msg, sizeof(msg)) >= 0) ? 0 : -1;
}

int comp_window_restore(comp_window_t *win) {
    if (!win) return -1;
    
    comp_msg_header_t msg = {
        .type = MSG_RESTORE,
        .window_id = win->window_id,
        .seq = next_seq(),
        .size = sizeof(comp_msg_header_t)
    };
    
    return (send_msg(&msg, sizeof(msg)) >= 0) ? 0 : -1;
}

int comp_window_request_focus(comp_window_t *win) {
    if (!win) return -1;
    
    comp_msg_header_t msg = {
        .type = MSG_REQUEST_FOCUS,
        .window_id = win->window_id,
        .seq = next_seq(),
        .size = sizeof(comp_msg_header_t)
    };
    
    return (send_msg(&msg, sizeof(msg)) >= 0) ? 0 : -1;
}

int comp_window_set_flags(comp_window_t *win, uint32_t flags) {
    if (!win) return -1;
    
    comp_msg_set_flags_t msg = {
        .header = {
            .type = MSG_SET_FLAGS,
            .window_id = win->window_id,
            .seq = next_seq(),
            .size = sizeof(comp_msg_set_flags_t)
        },
        .flags = flags
    };
    
    if (send_msg(&msg, sizeof(msg)) >= 0) {
        win->flags = flags;
        return 0;
    }
    return -1;
}

// ============================================================================
// Buffer Management
// ============================================================================

uint32_t *comp_window_get_buffer(comp_window_t *win) {
    if (!win || win->back_buffer < 0 || win->back_buffer >= win->buffer_count) {
        return NULL;
    }
    return win->buffers[win->back_buffer].pixels;
}

void comp_window_get_buffer_size(comp_window_t *win, uint32_t *width, uint32_t *height) {
    if (!win) {
        if (width) *width = 0;
        if (height) *height = 0;
        return;
    }
    
    comp_buffer_t *buf = &win->buffers[win->back_buffer];
    if (width) *width = buf->width;
    if (height) *height = buf->height;
}

int comp_window_present(comp_window_t *win) {
    return comp_window_present_rect(win, 0, 0, win->width, win->height);
}

int comp_window_present_rect(comp_window_t *win, int x, int y,
                              uint32_t width, uint32_t height) {
    if (!win) return -1;
    
    comp_buffer_t *buf = &win->buffers[win->back_buffer];
    
    comp_msg_present_buffer_t msg = {
        .header = {
            .type = MSG_PRESENT_BUFFER,
            .window_id = win->window_id,
            .seq = next_seq(),
            .size = sizeof(comp_msg_present_buffer_t)
        },
        .buffer_id = buf->buffer_id,
        .x = x,
        .y = y,
        .width = width,
        .height = height
    };
    
    if (send_msg(&msg, sizeof(msg)) < 0) {
        return -1;
    }
    
    // Swap buffers
    buf->in_use = true;
    int old_front = win->front_buffer;
    win->front_buffer = win->back_buffer;
    win->back_buffer = old_front;
    win->buffers[win->back_buffer].in_use = false;
    
    return 0;
}

int comp_window_damage(comp_window_t *win, int x, int y,
                        uint32_t width, uint32_t height) {
    if (!win) return -1;
    
    comp_msg_damage_rect_t msg = {
        .header = {
            .type = MSG_DAMAGE_RECT,
            .window_id = win->window_id,
            .seq = next_seq(),
            .size = sizeof(comp_msg_damage_rect_t)
        },
        .x = x,
        .y = y,
        .width = width,
        .height = height
    };
    
    return (send_msg(&msg, sizeof(msg)) >= 0) ? 0 : -1;
}

// ============================================================================
// Event Handling
// ============================================================================

void comp_window_set_handler(comp_window_t *win,
                              void (*handler)(comp_window_t *win, comp_event_t *event),
                              void *user_data) {
    if (!win) return;
    win->event_handler = handler;
    win->user_data = user_data;
}

int comp_poll_event(comp_event_t *event) {
    if (!g_client.connected || !event) {
        return COMP_EVENT_NONE;
    }
    
    long result = msg_recv(g_client.conn_id, event, sizeof(comp_event_t), 0);
    if (result <= 0) {
        return COMP_EVENT_NONE;
    }
    
    return event->header.type;
}

int comp_wait_event(comp_event_t *event, int timeout) {
    if (!g_client.connected || !event) {
        return COMP_EVENT_NONE;
    }
    
    long result = msg_recv(g_client.conn_id, event, sizeof(comp_event_t), timeout);
    if (result <= 0) {
        return COMP_EVENT_NONE;
    }
    
    return event->header.type;
}

void comp_dispatch_event(comp_event_t *event) {
    if (!event) return;
    
    comp_window_t *win = find_window(event->header.window_id);
    if (win && win->event_handler) {
        win->event_handler(win, event);
    }
}

void comp_main_loop(void) {
    g_running = true;
    comp_event_t event;
    
    while (g_running && g_window_count > 0) {
        int type = comp_wait_event(&event, 100);
        
        if (type != COMP_EVENT_NONE) {
            comp_dispatch_event(&event);
            
            // Handle close events
            if (type == COMP_EVENT_CLOSE) {
                comp_window_t *win = find_window(event.header.window_id);
                if (win) {
                    comp_window_destroy(win);
                }
            }
        }
    }
    
    g_running = false;
}

void comp_exit_loop(void) {
    g_running = false;
}

// ============================================================================
// Drawing Helpers
// ============================================================================

void comp_window_clear(comp_window_t *win, uint32_t color) {
    if (!win) return;
    
    uint32_t *pixels = comp_window_get_buffer(win);
    if (!pixels) return;
    
    uint32_t count = win->width * win->height;
    for (uint32_t i = 0; i < count; i++) {
        pixels[i] = color;
    }
}

void comp_window_fill_rect(comp_window_t *win, int x, int y,
                            uint32_t width, uint32_t height, uint32_t color) {
    if (!win) return;
    
    uint32_t *pixels = comp_window_get_buffer(win);
    if (!pixels) return;
    
    // Clip to window bounds
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > win->width) width = win->width - x;
    if (y + height > win->height) height = win->height - y;
    
    if (width <= 0 || height <= 0) return;
    
    for (uint32_t row = 0; row < height; row++) {
        uint32_t *dst = pixels + (y + row) * win->width + x;
        for (uint32_t col = 0; col < width; col++) {
            dst[col] = color;
        }
    }
}

void comp_window_draw_rect(comp_window_t *win, int x, int y,
                            uint32_t width, uint32_t height, uint32_t color) {
    if (!win || width == 0 || height == 0) return;
    
    // Top
    comp_window_fill_rect(win, x, y, width, 1, color);
    // Bottom
    comp_window_fill_rect(win, x, y + height - 1, width, 1, color);
    // Left
    comp_window_fill_rect(win, x, y, 1, height, color);
    // Right
    comp_window_fill_rect(win, x + width - 1, y, 1, height, color);
}

void comp_window_draw_pixel(comp_window_t *win, int x, int y, uint32_t color) {
    if (!win) return;
    
    uint32_t *pixels = comp_window_get_buffer(win);
    if (!pixels) return;
    
    if (x >= 0 && x < (int)win->width && y >= 0 && y < (int)win->height) {
        pixels[y * win->width + x] = color;
    }
}

void comp_window_draw_hline(comp_window_t *win, int x, int y,
                             uint32_t width, uint32_t color) {
    comp_window_fill_rect(win, x, y, width, 1, color);
}

void comp_window_draw_vline(comp_window_t *win, int x, int y,
                             uint32_t height, uint32_t color) {
    comp_window_fill_rect(win, x, y, 1, height, color);
}

void comp_window_blit(comp_window_t *win, int x, int y,
                       uint32_t width, uint32_t height,
                       const uint32_t *src_pixels, uint32_t stride) {
    if (!win || !src_pixels) return;
    
    uint32_t *pixels = comp_window_get_buffer(win);
    if (!pixels) return;
    
    // Clip to window bounds
    int src_x = 0, src_y = 0;
    if (x < 0) { src_x = -x; width += x; x = 0; }
    if (y < 0) { src_y = -y; height += y; y = 0; }
    if (x + width > win->width) width = win->width - x;
    if (y + height > win->height) height = win->height - y;
    
    if (width <= 0 || height <= 0) return;
    
    for (uint32_t row = 0; row < height; row++) {
        uint32_t *dst = pixels + (y + row) * win->width + x;
        const uint32_t *src = src_pixels + (src_y + row) * stride + src_x;
        for (uint32_t col = 0; col < width; col++) {
            dst[col] = src[col];
        }
    }
}
