// compositor.c - Userland Compositor for MayteraOS
// This compositor runs in Ring 3, communicating with the kernel via syscalls

#include "compositor.h"
#include "window_manager.h"
#include "desktop.h"
#include "taskbar.h"
#include "../libc/syscall.h"
#include "../libc/string.h"
#include "../libc/stdio.h"
#include "../libc/stdlib.h"

// Global compositor state
static compositor_t g_compositor;

// Framebuffer and IPC wrappers are provided by ../libc/syscall.h
// (fb_map, fb_info, fb_flip, msg_create_channel, msg_send, msg_recv, ...)

// ============================================================================
// Initialization
// ============================================================================

int compositor_init(void) {
    printf("[Compositor] Initializing userland compositor...\n");
    
    // Clear state
    memset(&g_compositor, 0, sizeof(g_compositor));
    
    // Get framebuffer info
    fb_info_t fi;
    if (fb_info(&fi) < 0) {
        printf("[Compositor] ERROR: Failed to get framebuffer info\n");
        return -1;
    }

    g_compositor.screen_width  = (int)fi.width;
    g_compositor.screen_height = (int)fi.height;

    printf("[Compositor] Screen: %dx%d\n",
           g_compositor.screen_width, g_compositor.screen_height);

    // Map framebuffer
    g_compositor.framebuffer = (uint32_t *)(long)fb_map();
    if (!g_compositor.framebuffer) {
        printf("[Compositor] ERROR: Failed to map framebuffer\n");
        return -1;
    }

    printf("[Compositor] Framebuffer mapped at %p\n", g_compositor.framebuffer);

    // Enter exclusive mode: kernel WM stops drawing
    grab_input(1);
    
    // Initialize window list
    g_compositor.windows = NULL;
    g_compositor.focused = NULL;
    g_compositor.next_window_id = 1;
    g_compositor.highest_z = 0;
    
    // Initialize clients
    for (int i = 0; i < MAX_COMPOSITOR_CLIENTS; i++) {
        g_compositor.clients[i].connected = false;
    }
    g_compositor.next_client_id = 1;
    
    // Initialize input state
    g_compositor.mouse_x = g_compositor.screen_width / 2;
    g_compositor.mouse_y = g_compositor.screen_height / 2;
    g_compositor.mouse_buttons = 0;
    g_compositor.drag_window = NULL;
    g_compositor.resize_window = NULL;
    
    // Create IPC channel for clients to connect
    g_compositor.channel_id = msg_create_channel();
    if (g_compositor.channel_id < 0) {
        printf("[Compositor] ERROR: Failed to create IPC channel\n");
        return -1;
    }
    
    printf("[Compositor] IPC channel created: %d\n", g_compositor.channel_id);

    // Register with name service so clients can find us by name
    ipc_register_name("com.maytera.compositor", g_compositor.channel_id);
    
    // Initialize subsystems
    if (window_manager_init() < 0) {
        printf("[Compositor] ERROR: Failed to init window manager\n");
        return -1;
    }
    
    if (desktop_init() < 0) {
        printf("[Compositor] ERROR: Failed to init desktop\n");
        return -1;
    }
    
    if (taskbar_init() < 0) {
        printf("[Compositor] ERROR: Failed to init taskbar\n");
        return -1;
    }
    
    g_compositor.running = true;
    g_compositor.needs_redraw = true;
    g_compositor.frame_count = 0;
    g_compositor.last_frame_time = 0;
    
    printf("[Compositor] Initialization complete\n");
    return 0;
}

// ============================================================================
// Main Loop
// ============================================================================

void compositor_run(void) {
    printf("[Compositor] Entering main loop\n");
    
    while (g_compositor.running) {
        uint64_t frame_start = sys_clock();
        
        // 1. Process IPC messages from clients
        compositor_process_messages();
        
        // 2. Handle input events
        compositor_process_input();
        
        // 3. Update desktop and taskbar
        desktop_update();
        taskbar_update();
        
        // 4. Render if needed
        if (g_compositor.needs_redraw) {
            compositor_render();
            g_compositor.needs_redraw = false;
            g_compositor.frame_count++;
        }
        
        // 5. Rate limiting - aim for 60 FPS
        uint64_t frame_end = sys_clock();
        uint64_t frame_time = frame_end - frame_start;
        if (frame_time < COMPOSITOR_FRAME_MS) {
            sys_sleep(COMPOSITOR_FRAME_MS - frame_time);
        }
        
        g_compositor.last_frame_time = frame_time;
    }
    
    printf("[Compositor] Main loop ended\n");
}

// ============================================================================
// Message Processing
// ============================================================================

static void compositor_process_messages(void) {
    // Accept new client connections
    int conn_id = msg_accept(g_compositor.channel_id, 0);  // Non-blocking
    if (conn_id > 0) {
        // New client connected, wait for connect message
        compositor_handle_new_client(conn_id);
    }
    
    // Process messages from existing clients
    for (int i = 0; i < MAX_COMPOSITOR_CLIENTS; i++) {
        if (!g_compositor.clients[i].connected) continue;
        
        uint8_t msg_buf[4096];
        int len = msg_recv(g_compositor.clients[i].conn_id, msg_buf, sizeof(msg_buf), 0);
        if (len > 0) {
            compositor_handle_client_message(i, msg_buf, len);
        } else if (len < 0) {
            // Client disconnected
            compositor_client_unregister(g_compositor.clients[i].id);
        }
    }
}

static void compositor_handle_new_client(int conn_id) {
    uint8_t msg_buf[4096];
    int len = msg_recv(conn_id, msg_buf, sizeof(msg_buf), 1000);  // 1 second timeout
    
    if (len < sizeof(msg_header_t)) {
        msg_close(conn_id);
        return;
    }
    
    msg_header_t *header = (msg_header_t *)msg_buf;
    if (header->type != MSG_CLIENT_CONNECT) {
        msg_close(conn_id);
        return;
    }
    
    msg_client_connect_t *connect = (msg_client_connect_t *)msg_buf;
    
    // Register the client
    uint32_t client_id = compositor_client_register(conn_id, connect->app_name);
    if (client_id == 0) {
        msg_error_t error;
        MSG_INIT_HEADER(&error, MSG_COMPOSITOR_ERROR, 0);
        error.error_code = COMP_ERR_NO_MEMORY;
        strcpy(error.message, "Too many clients");
        msg_send(conn_id, &error, sizeof(error));
        msg_close(conn_id);
        return;
    }
    
    // Send welcome message
    msg_compositor_welcome_t welcome;
    MSG_INIT_HEADER(&welcome, MSG_COMPOSITOR_WELCOME, 0);
    welcome.client_id = client_id;
    welcome.screen_width = g_compositor.screen_width;
    welcome.screen_height = g_compositor.screen_height;
    welcome.compositor_channel = g_compositor.channel_id;
    
    msg_send(conn_id, &welcome, sizeof(welcome));
    
    printf("[Compositor] Client \'%s\' connected (id=%u)\n", 
           connect->app_name, client_id);
}

static void compositor_handle_client_message(int client_idx, uint8_t *data, int len) {
    comp_client_t *client = &g_compositor.clients[client_idx];
    msg_header_t *header = (msg_header_t *)data;
    
    switch (header->type) {
        case MSG_CLIENT_DISCONNECT:
            compositor_client_unregister(client->id);
            break;
            
        case MSG_WINDOW_CREATE: {
            msg_window_create_t *req = (msg_window_create_t *)data;
            comp_window_t *win = compositor_window_create(
                client->id, req->title,
                req->x, req->y, req->width, req->height,
                req->flags
            );
            
            msg_window_created_t resp;
            MSG_INIT_HEADER(&resp, MSG_WINDOW_CREATED, win ? win->id : 0);
            resp.header.seq = header->seq;
            
            if (win) {
                resp.error = COMP_ERR_NONE;
                resp.x = win->x;
                resp.y = win->y;
                resp.width = win->width;
                resp.height = win->height;
                resp.shm_id = win->shm_id;
                resp.buffer_addr = (uint64_t)win->buffer;
            } else {
                resp.error = COMP_ERR_NO_MEMORY;
            }
            
            msg_send(client->conn_id, &resp, sizeof(resp));
            g_compositor.needs_redraw = true;
            break;
        }
        
        case MSG_WINDOW_DESTROY: {
            comp_window_t *win = compositor_window_get(header->window_id);
            if (win && win->client_id == client->id) {
                compositor_window_destroy(win);
                g_compositor.needs_redraw = true;
            }
            break;
        }
        
        case MSG_WINDOW_MOVE: {
            msg_window_geometry_t *geom = (msg_window_geometry_t *)data;
            comp_window_t *win = compositor_window_get(header->window_id);
            if (win && win->client_id == client->id) {
                compositor_window_move(win, geom->x, geom->y);
                g_compositor.needs_redraw = true;
            }
            break;
        }
        
        case MSG_WINDOW_RESIZE: {
            msg_window_geometry_t *geom = (msg_window_geometry_t *)data;
            comp_window_t *win = compositor_window_get(header->window_id);
            if (win && win->client_id == client->id) {
                compositor_window_resize(win, geom->width, geom->height);
                g_compositor.needs_redraw = true;
            }
            break;
        }
        
        case MSG_WINDOW_SHOW: {
            comp_window_t *win = compositor_window_get(header->window_id);
            if (win && win->client_id == client->id) {
                win->flags |= WIN_FLAG_VISIBLE;
                g_compositor.needs_redraw = true;
            }
            break;
        }
        
        case MSG_WINDOW_HIDE: {
            comp_window_t *win = compositor_window_get(header->window_id);
            if (win && win->client_id == client->id) {
                win->flags &= ~WIN_FLAG_VISIBLE;
                g_compositor.needs_redraw = true;
            }
            break;
        }
        
        case MSG_WINDOW_FOCUS: {
            comp_window_t *win = compositor_window_get(header->window_id);
            if (win && win->client_id == client->id) {
                compositor_window_focus(win);
                g_compositor.needs_redraw = true;
            }
            break;
        }
        
        case MSG_BUFFER_COMMIT: {
            // Client has finished drawing, mark damaged
            comp_window_t *win = compositor_window_get(header->window_id);
            if (win && win->client_id == client->id) {
                g_compositor.needs_redraw = true;
            }
            break;
        }
        
        case MSG_BUFFER_DAMAGE: {
            msg_buffer_damage_t *dmg = (msg_buffer_damage_t *)data;
            compositor_damage(dmg->x, dmg->y, dmg->width, dmg->height);
            break;
        }
    }
}

// ============================================================================
// Input Processing
// ============================================================================

static void compositor_process_input(void) {
    // Get mouse state from kernel
    int32_t mx, my;
    uint32_t buttons;
    
    if (sys_get_mouse(&mx, &my, &buttons) == 0) {
        if (mx != g_compositor.mouse_x || 
            my != g_compositor.mouse_y ||
            buttons != g_compositor.mouse_buttons) {
            compositor_handle_mouse(mx, my, buttons);
        }
    }
    
    // Get keyboard events from kernel
    uint32_t keycode, scancode, mods;
    int pressed;
    
    while (sys_get_key(&keycode, &scancode, &pressed, &mods) == 0) {
        compositor_handle_key(keycode, scancode, pressed != 0, mods);
    }
}

// ============================================================================
// Window Management Implementation
// ============================================================================

comp_window_t *compositor_window_create(uint32_t client_id, const char *title,
                                         int32_t x, int32_t y,
                                         int32_t width, int32_t height,
                                         uint32_t flags) {
    // Allocate window structure
    comp_window_t *win = (comp_window_t *)malloc(sizeof(comp_window_t));
    if (!win) return NULL;
    
    memset(win, 0, sizeof(comp_window_t));
    
    win->id = g_compositor.next_window_id++;
    win->client_id = client_id;
    strncpy(win->title, title, sizeof(win->title) - 1);
    
    // Window decoration settings
    win->decorated = (flags & WIN_FLAG_DECORATED) != 0 || 
                     !(flags & WIN_FLAG_NO_TASKBAR);
    win->titlebar_height = win->decorated ? 28 : 0;
    win->border_width = win->decorated ? 1 : 0;
    
    // Auto-placement if requested
    if (x < 0) x = 100 + (g_compositor.next_window_id * 30) % 400;
    if (y < 0) y = 100 + (g_compositor.next_window_id * 20) % 300;
    
    win->x = x;
    win->y = y;
    win->width = width;
    win->height = height;
    win->flags = flags | WIN_FLAG_VISIBLE | WIN_FLAG_DECORATED;
    win->z_order = ++g_compositor.highest_z;
    
    // Create shared memory buffer for window content
    int32_t buf_size = width * height * 4;  // ARGB8888
    win->shm_id = shm_create(buf_size, 0);
    if (win->shm_id < 0) {
        free(win);
        return NULL;
    }
    
    // Map the buffer
    if (shm_map(win->shm_id, (void **)&win->buffer) < 0) {
        shm_destroy(win->shm_id);
        free(win);
        return NULL;
    }
    
    win->buf_width = width;
    win->buf_height = height;
    win->buf_stride = width * 4;
    
    // Clear buffer to white
    for (int i = 0; i < width * height; i++) {
        win->buffer[i] = 0xFFFFFFFF;
    }
    
    // Add to window list (at front = top of z-order)
    win->next = g_compositor.windows;
    win->prev = NULL;
    if (g_compositor.windows) {
        g_compositor.windows->prev = win;
    }
    g_compositor.windows = win;
    
    // Focus new window
    compositor_window_focus(win);
    
    printf("[Compositor] Created window %u: \'%s\' at (%d,%d) %dx%d\n",
           win->id, title, x, y, width, height);
    
    return win;
}

void compositor_window_destroy(comp_window_t *win) {
    if (!win) return;
    
    // Remove from list
    if (win->prev) win->prev->next = win->next;
    else g_compositor.windows = win->next;
    if (win->next) win->next->prev = win->prev;
    
    // Clear focus if needed
    if (g_compositor.focused == win) {
        g_compositor.focused = g_compositor.windows;
    }
    
    // Clear drag/resize state
    if (g_compositor.drag_window == win) g_compositor.drag_window = NULL;
    if (g_compositor.resize_window == win) g_compositor.resize_window = NULL;
    
    // Clean up shared memory
    if (win->buffer) {
        shm_unmap(win->shm_id);
    }
    shm_destroy(win->shm_id);
    
    printf("[Compositor] Destroyed window %u\n", win->id);
    
    free(win);
}

comp_window_t *compositor_window_get(uint32_t window_id) {
    comp_window_t *win = g_compositor.windows;
    while (win) {
        if (win->id == window_id) return win;
        win = win->next;
    }
    return NULL;
}

void compositor_window_focus(comp_window_t *win) {
    if (!win || win == g_compositor.focused) return;
    
    comp_window_t *old_focus = g_compositor.focused;
    
    // Send focus out event to old window
    if (old_focus) {
        old_focus->flags &= ~WIN_FLAG_FOCUSED;
        compositor_send_focus_event(old_focus, false);
    }
    
    // Focus new window
    g_compositor.focused = win;
    win->flags |= WIN_FLAG_FOCUSED;
    
    // Raise to top
    compositor_window_raise(win);
    
    // Send focus in event
    compositor_send_focus_event(win, true);
}

void compositor_window_raise(comp_window_t *win) {
    if (!win || win == g_compositor.windows) return;
    
    // Remove from current position
    if (win->prev) win->prev->next = win->next;
    if (win->next) win->next->prev = win->prev;
    
    // Insert at front
    win->prev = NULL;
    win->next = g_compositor.windows;
    if (g_compositor.windows) {
        g_compositor.windows->prev = win;
    }
    g_compositor.windows = win;
    
    win->z_order = ++g_compositor.highest_z;
}

void compositor_window_move(comp_window_t *win, int32_t x, int32_t y) {
    if (!win) return;
    win->x = x;
    win->y = y;
}

void compositor_window_resize(comp_window_t *win, int32_t width, int32_t height) {
    if (!win) return;
    if (width < 100) width = 100;
    if (height < 50) height = 50;
    
    // Recreate buffer if size changed
    if (width != win->buf_width || height != win->buf_height) {
        // Unmap old buffer
        if (win->buffer) {
            shm_unmap(win->shm_id);
            shm_destroy(win->shm_id);
        }
        
        // Create new buffer
        int32_t buf_size = width * height * 4;
        win->shm_id = shm_create(buf_size, 0);
        if (win->shm_id >= 0) {
            shm_map(win->shm_id, (void **)&win->buffer);
            win->buf_width = width;
            win->buf_height = height;
            win->buf_stride = width * 4;
            
            // Clear to white
            for (int i = 0; i < width * height; i++) {
                win->buffer[i] = 0xFFFFFFFF;
            }
        }
        
        // Notify client of resize
        compositor_send_configure_event(win);
    }
    
    win->width = width;
    win->height = height;
}

// ============================================================================
// Client Management
// ============================================================================

uint32_t compositor_client_register(int conn_id, const char *name) {
    // Find free slot
    for (int i = 0; i < MAX_COMPOSITOR_CLIENTS; i++) {
        if (!g_compositor.clients[i].connected) {
            comp_client_t *client = &g_compositor.clients[i];
            client->id = g_compositor.next_client_id++;
            client->conn_id = conn_id;
            strncpy(client->name, name, sizeof(client->name) - 1);
            client->connected = true;
            client->num_windows = 0;
            return client->id;
        }
    }
    return 0;
}

void compositor_client_unregister(uint32_t client_id) {
    for (int i = 0; i < MAX_COMPOSITOR_CLIENTS; i++) {
        if (g_compositor.clients[i].id == client_id) {
            comp_client_t *client = &g_compositor.clients[i];
            
            // Destroy all windows owned by this client
            comp_window_t *win = g_compositor.windows;
            while (win) {
                comp_window_t *next = win->next;
                if (win->client_id == client_id) {
                    compositor_window_destroy(win);
                }
                win = next;
            }
            
            msg_close(client->conn_id);
            client->connected = false;
            
            printf("[Compositor] Client %u disconnected\n", client_id);
            break;
        }
    }
}

// ============================================================================
// Event Sending
// ============================================================================

static void compositor_send_focus_event(comp_window_t *win, bool focused) {
    comp_client_t *client = compositor_get_client(win->client_id);
    if (!client) return;
    
    msg_header_t msg;
    MSG_INIT_HEADER(&msg, focused ? MSG_WINDOW_FOCUS_IN : MSG_WINDOW_FOCUS_OUT, win->id);
    msg_send(client->conn_id, &msg, sizeof(msg));
}

static void compositor_send_configure_event(comp_window_t *win) {
    comp_client_t *client = compositor_get_client(win->client_id);
    if (!client) return;
    
    msg_window_configure_t msg;
    MSG_INIT_HEADER(&msg, MSG_WINDOW_CONFIGURE, win->id);
    msg.x = win->x;
    msg.y = win->y;
    msg.width = win->width;
    msg.height = win->height;
    msg.flags = win->flags;
    
    msg_send(client->conn_id, &msg, sizeof(msg));
}

static comp_client_t *compositor_get_client(uint32_t client_id) {
    for (int i = 0; i < MAX_COMPOSITOR_CLIENTS; i++) {
        if (g_compositor.clients[i].id == client_id && 
            g_compositor.clients[i].connected) {
            return &g_compositor.clients[i];
        }
    }
    return NULL;
}

// ============================================================================
// Rendering
// ============================================================================

// Draw window decorations
static void draw_window_decoration(comp_window_t *win) {
    if (!win->decorated) return;
    
    int32_t x = win->x - win->border_width;
    int32_t y = win->y - win->titlebar_height - win->border_width;
    int32_t w = win->width + win->border_width * 2;
    int32_t h = win->height + win->titlebar_height + win->border_width * 2;
    
    // Colors based on focus
    uint32_t title_bg = (win->flags & WIN_FLAG_FOCUSED) ? 0xFF4A90D9 : 0xFF808080;
    uint32_t title_fg = 0xFFFFFFFF;
    uint32_t border_color = (win->flags & WIN_FLAG_FOCUSED) ? 0xFF2060A0 : 0xFF606060;
    
    // Draw border
    for (int32_t px = x; px < x + w; px++) {
        if (y >= 0 && y < g_compositor.screen_height)
            g_compositor.framebuffer[y * g_compositor.screen_width + px] = border_color;
        int32_t by = y + h - 1;
        if (by >= 0 && by < g_compositor.screen_height)
            g_compositor.framebuffer[by * g_compositor.screen_width + px] = border_color;
    }
    for (int32_t py = y; py < y + h; py++) {
        if (py < 0 || py >= g_compositor.screen_height) continue;
        g_compositor.framebuffer[py * g_compositor.screen_width + x] = border_color;
        g_compositor.framebuffer[py * g_compositor.screen_width + x + w - 1] = border_color;
    }
    
    // Draw titlebar
    for (int32_t py = y + 1; py < y + win->titlebar_height; py++) {
        if (py < 0 || py >= g_compositor.screen_height) continue;
        for (int32_t px = x + 1; px < x + w - 1; px++) {
            if (px < 0 || px >= g_compositor.screen_width) continue;
            g_compositor.framebuffer[py * g_compositor.screen_width + px] = title_bg;
        }
    }
    
    // Draw close button (X) in titlebar
    int32_t btn_x = x + w - 26;
    int32_t btn_y = y + 4;
    for (int i = 0; i < 20; i++) {
        for (int j = 0; j < 20; j++) {
            int32_t px = btn_x + j;
            int32_t py = btn_y + i;
            if (px < 0 || px >= g_compositor.screen_width) continue;
            if (py < 0 || py >= g_compositor.screen_height) continue;
            // Red background
            g_compositor.framebuffer[py * g_compositor.screen_width + px] = 0xFFE04040;
        }
    }
    
    // TODO: Draw title text (needs font rendering)
}

void compositor_render(void) {
    // Clear to desktop background
    uint32_t bg_color = 0xFF2C5AA0;  // Blue desktop
    for (int i = 0; i < g_compositor.screen_width * g_compositor.screen_height; i++) {
        g_compositor.framebuffer[i] = bg_color;
    }
    
    // Render desktop (icons, wallpaper)
    desktop_render(g_compositor.framebuffer, 
                   g_compositor.screen_width, g_compositor.screen_height);
    
    // Render windows (back to front = end of list to beginning)
    // First, collect windows in reverse z-order
    comp_window_t *stack[MAX_COMPOSITOR_WINDOWS];
    int stack_count = 0;
    
    comp_window_t *win = g_compositor.windows;
    while (win && stack_count < MAX_COMPOSITOR_WINDOWS) {
        stack[stack_count++] = win;
        win = win->next;
    }
    
    // Render back to front
    for (int i = stack_count - 1; i >= 0; i--) {
        win = stack[i];
        if (!(win->flags & WIN_FLAG_VISIBLE)) continue;
        if (win->flags & WIN_FLAG_MINIMIZED) continue;
        
        // Draw window decoration
        draw_window_decoration(win);
        
        // Blit window content
        if (win->buffer) {
            for (int32_t sy = 0; sy < win->height; sy++) {
                int32_t dy = win->y + sy;
                if (dy < 0 || dy >= g_compositor.screen_height) continue;
                
                for (int32_t sx = 0; sx < win->width; sx++) {
                    int32_t dx = win->x + sx;
                    if (dx < 0 || dx >= g_compositor.screen_width) continue;
                    
                    uint32_t pixel = win->buffer[sy * win->buf_width + sx];
                    // Alpha blending could be done here
                    g_compositor.framebuffer[dy * g_compositor.screen_width + dx] = pixel;
                }
            }
        }
    }
    
    // Render taskbar
    taskbar_render(g_compositor.framebuffer,
                   g_compositor.screen_width, g_compositor.screen_height);
    
    // Render cursor
    compositor_render_cursor();
    
    // Flip buffers if double buffered
    fb_flip();
}

static void compositor_render_cursor(void) {
    // Simple 12x12 cursor
    static const uint8_t cursor_data[12][12] = {
        {1,0,0,0,0,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0,0,0,0,0},
        {1,2,2,1,0,0,0,0,0,0,0,0},
        {1,2,2,2,1,0,0,0,0,0,0,0},
        {1,2,2,2,2,1,0,0,0,0,0,0},
        {1,2,2,2,2,2,1,0,0,0,0,0},
        {1,2,2,2,2,2,2,1,0,0,0,0},
        {1,2,2,2,2,1,1,1,1,0,0,0},
        {1,2,2,1,2,1,0,0,0,0,0,0},
        {1,1,1,0,1,2,1,0,0,0,0,0},
        {0,0,0,0,0,1,1,0,0,0,0,0},
    };
    
    uint32_t colors[3] = {0xFF000000, 0xFFFFFFFF, 0xFF000000};
    
    for (int cy = 0; cy < 12; cy++) {
        for (int cx = 0; cx < 12; cx++) {
            uint8_t c = cursor_data[cy][cx];
            if (c == 0) continue;
            
            int32_t px = g_compositor.mouse_x + cx;
            int32_t py = g_compositor.mouse_y + cy;
            
            if (px >= 0 && px < g_compositor.screen_width &&
                py >= 0 && py < g_compositor.screen_height) {
                g_compositor.framebuffer[py * g_compositor.screen_width + px] = colors[c];
            }
        }
    }
}

void compositor_damage(int32_t x, int32_t y, int32_t width, int32_t height) {
    // For now, just mark full redraw needed
    // TODO: Implement proper damage tracking
    g_compositor.needs_redraw = true;
}

void compositor_damage_all(void) {
    g_compositor.needs_redraw = true;
}

// ============================================================================
// Input Handling
// ============================================================================

void compositor_handle_mouse(int32_t x, int32_t y, uint32_t buttons) {
    int32_t dx = x - g_compositor.mouse_x;
    int32_t dy = y - g_compositor.mouse_y;
    
    g_compositor.mouse_x = x;
    g_compositor.mouse_y = y;
    
    bool left_pressed = (buttons & MOUSE_BTN_LEFT) && 
                        !(g_compositor.mouse_buttons & MOUSE_BTN_LEFT);
    bool left_released = !(buttons & MOUSE_BTN_LEFT) && 
                         (g_compositor.mouse_buttons & MOUSE_BTN_LEFT);
    
    g_compositor.mouse_buttons = buttons;
    g_compositor.needs_redraw = true;
    
    // Handle window dragging
    if (g_compositor.drag_window) {
        if (left_released) {
            g_compositor.drag_window = NULL;
        } else {
            compositor_window_move(g_compositor.drag_window,
                g_compositor.drag_window->x + dx,
                g_compositor.drag_window->y + dy);
        }
        return;
    }
    
    // Handle window resizing
    if (g_compositor.resize_window) {
        if (left_released) {
            g_compositor.resize_window = NULL;
        } else {
            int32_t new_w = g_compositor.resize_orig_w + 
                           (x - g_compositor.resize_orig_x);
            int32_t new_h = g_compositor.resize_orig_h + 
                           (y - g_compositor.resize_orig_y);
            compositor_window_resize(g_compositor.resize_window, new_w, new_h);
        }
        return;
    }
    
    // Check taskbar first
    if (taskbar_handle_mouse(x, y, buttons)) {
        return;
    }
    
    // Check desktop icons
    if (desktop_handle_mouse(x, y, buttons)) {
        return;
    }
    
    // Find window under cursor
    comp_window_t *win = compositor_window_at(x, y);
    if (!win) return;
    
    if (left_pressed) {
        // Focus window
        compositor_window_focus(win);
        
        // Check for titlebar drag
        if (compositor_in_titlebar(win, x, y)) {
            // Check close button
            int32_t close_x = win->x + win->width - 26;
            if (x >= close_x && x < close_x + 20 &&
                y >= win->y - win->titlebar_height + 4 &&
                y < win->y - win->titlebar_height + 24) {
                // Close button clicked
                compositor_send_close_request(win);
                return;
            }
            
            // Start dragging
            g_compositor.drag_window = win;
            g_compositor.drag_offset_x = x - win->x;
            g_compositor.drag_offset_y = y - win->y;
            return;
        }
        
        // Check for resize edge
        int edge = compositor_resize_edge_at(win, x, y);
        if (edge) {
            g_compositor.resize_window = win;
            g_compositor.resize_edge = edge;
            g_compositor.resize_orig_x = x;
            g_compositor.resize_orig_y = y;
            g_compositor.resize_orig_w = win->width;
            g_compositor.resize_orig_h = win->height;
            return;
        }
    }
    
    // Forward mouse event to window
    comp_client_t *client = compositor_get_client(win->client_id);
    if (client) {
        msg_mouse_move_t msg;
        MSG_INIT_HEADER(&msg, MSG_INPUT_MOUSE_MOVE, win->id);
        msg.x = x - win->x;
        msg.y = y - win->y;
        msg.dx = dx;
        msg.dy = dy;
        msg.buttons = buttons;
        msg_send(client->conn_id, &msg, sizeof(msg));
        
        if (left_pressed || left_released) {
            msg_mouse_button_t btn;
            MSG_INIT_HEADER(&btn, MSG_INPUT_MOUSE_BUTTON, win->id);
            btn.x = x - win->x;
            btn.y = y - win->y;
            btn.button = MOUSE_BTN_LEFT;
            btn.state = left_pressed ? 1 : 0;
            btn.modifiers = 0;
            msg_send(client->conn_id, &btn, sizeof(btn));
        }
    }
}

void compositor_handle_key(uint32_t keycode, uint32_t scancode, bool pressed, uint32_t modifiers) {
    // Forward to focused window
    if (!g_compositor.focused) return;
    
    comp_client_t *client = compositor_get_client(g_compositor.focused->client_id);
    if (!client) return;
    
    msg_key_event_t msg;
    MSG_INIT_HEADER(&msg, pressed ? MSG_INPUT_KEY_DOWN : MSG_INPUT_KEY_UP, 
                    g_compositor.focused->id);
    msg.keycode = keycode;
    msg.scancode = scancode;
    msg.modifiers = modifiers;
    msg.utf8[0] = 0;  // TODO: UTF-8 conversion
    
    msg_send(client->conn_id, &msg, sizeof(msg));
}

static void compositor_send_close_request(comp_window_t *win) {
    comp_client_t *client = compositor_get_client(win->client_id);
    if (!client) return;
    
    msg_header_t msg;
    MSG_INIT_HEADER(&msg, MSG_WINDOW_CLOSE_REQUEST, win->id);
    msg_send(client->conn_id, &msg, sizeof(msg));
}

// ============================================================================
// Helper Functions
// ============================================================================

comp_window_t *compositor_window_at(int32_t x, int32_t y) {
    // Check windows front to back
    comp_window_t *win = g_compositor.windows;
    while (win) {
        if (!(win->flags & WIN_FLAG_VISIBLE)) {
            win = win->next;
            continue;
        }
        if (win->flags & WIN_FLAG_MINIMIZED) {
            win = win->next;
            continue;
        }
        
        // Check window bounds including titlebar
        int32_t top = win->y - win->titlebar_height - win->border_width;
        int32_t left = win->x - win->border_width;
        int32_t right = win->x + win->width + win->border_width;
        int32_t bottom = win->y + win->height + win->border_width;
        
        if (x >= left && x < right && y >= top && y < bottom) {
            return win;
        }
        
        win = win->next;
    }
    return NULL;
}

bool compositor_in_titlebar(comp_window_t *win, int32_t x, int32_t y) {
    if (!win->decorated) return false;
    
    int32_t title_top = win->y - win->titlebar_height;
    int32_t title_bottom = win->y;
    
    return (y >= title_top && y < title_bottom &&
            x >= win->x && x < win->x + win->width);
}

int compositor_resize_edge_at(comp_window_t *win, int32_t x, int32_t y) {
    if (!(win->flags & WIN_FLAG_RESIZABLE)) return 0;
    
    int edge = 0;
    int border = 5;  // Resize border width
    
    // Check right edge
    if (x >= win->x + win->width - border && x < win->x + win->width + win->border_width) {
        edge |= 1;
    }
    
    // Check bottom edge
    if (y >= win->y + win->height - border && y < win->y + win->height + win->border_width) {
        edge |= 2;
    }
    
    return edge;
}

// ============================================================================
// Shutdown
// ============================================================================

void compositor_shutdown(void) {
    printf("[Compositor] Shutting down...\n");
    
    g_compositor.running = false;
    
    // Destroy all windows
    while (g_compositor.windows) {
        compositor_window_destroy(g_compositor.windows);
    }
    
    // Disconnect all clients
    for (int i = 0; i < MAX_COMPOSITOR_CLIENTS; i++) {
        if (g_compositor.clients[i].connected) {
            msg_close(g_compositor.clients[i].conn_id);
        }
    }
    
    // Destroy channel
    msg_destroy_channel(g_compositor.channel_id);
    
    // Cleanup subsystems
    taskbar_shutdown();
    desktop_shutdown();
    window_manager_shutdown();
    
    printf("[Compositor] Shutdown complete\n");
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char **argv) {
    printf("MayteraOS Userland Compositor v1.0\n");
    
    if (compositor_init() < 0) {
        printf("[Compositor] FATAL: Initialization failed\n");
        return 1;
    }
    
    compositor_run();
    compositor_shutdown();
    
    return 0;
}
