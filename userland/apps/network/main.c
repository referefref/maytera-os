// network - Network Settings for MayteraOS (user-space version)
// Network interface configuration and status
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"

#define WIN_W 480
#define WIN_H 380
#define MARGIN 16
#define SECTION_H 28
#define ROW_H 24

// Toggle sizing
#define TOGGLE_WIDTH  40
#define TOGGLE_HEIGHT 20
#define TOGGLE_ON     THEME_ACCENT
#define TOGGLE_OFF    0x00606060
#define TOGGLE_THUMB  0x00FFFFFF

// Network interface
typedef struct {
    char name[16];
    char mac[20];
    char ip[20];
    char netmask[20];
    char gateway[20];
    int speed_mbps;
    int connected;
    int bytes_rx;
    int bytes_tx;
} netif_t;

#define MAX_IFACES 4

// State
static int win = -1;
static netif_t interfaces[MAX_IFACES];
static int iface_count = 0;
static int selected_iface = 0;
static int dhcp_enabled = 1;

// Initialize demo interfaces
static void init_interfaces(void) {
    // Ethernet
    interfaces[0].connected = 1;
    interfaces[0].speed_mbps = 1000;
    interfaces[0].bytes_rx = 128547823;
    interfaces[0].bytes_tx = 24785632;
    
    const char *eth_name = "eth0";
    const char *eth_mac = "52:54:00:AB:CD:EF";
    const char *eth_ip = "10.0.0.100";
    const char *eth_mask = "255.255.255.0";
    const char *eth_gw = "10.0.0.1";
    
    int i = 0;
    while (eth_name[i]) { interfaces[0].name[i] = eth_name[i]; i++; }
    interfaces[0].name[i] = '\0';
    
    i = 0;
    while (eth_mac[i]) { interfaces[0].mac[i] = eth_mac[i]; i++; }
    interfaces[0].mac[i] = '\0';
    
    i = 0;
    while (eth_ip[i]) { interfaces[0].ip[i] = eth_ip[i]; i++; }
    interfaces[0].ip[i] = '\0';
    
    i = 0;
    while (eth_mask[i]) { interfaces[0].netmask[i] = eth_mask[i]; i++; }
    interfaces[0].netmask[i] = '\0';
    
    i = 0;
    while (eth_gw[i]) { interfaces[0].gateway[i] = eth_gw[i]; i++; }
    interfaces[0].gateway[i] = '\0';
    
    // Loopback
    interfaces[1].connected = 1;
    interfaces[1].speed_mbps = 0;
    interfaces[1].bytes_rx = 4096;
    interfaces[1].bytes_tx = 4096;
    
    const char *lo_name = "lo";
    const char *lo_mac = "00:00:00:00:00:00";
    const char *lo_ip = "127.0.0.1";
    const char *lo_mask = "255.0.0.0";
    
    i = 0;
    while (lo_name[i]) { interfaces[1].name[i] = lo_name[i]; i++; }
    interfaces[1].name[i] = '\0';
    
    i = 0;
    while (lo_mac[i]) { interfaces[1].mac[i] = lo_mac[i]; i++; }
    interfaces[1].mac[i] = '\0';
    
    i = 0;
    while (lo_ip[i]) { interfaces[1].ip[i] = lo_ip[i]; i++; }
    interfaces[1].ip[i] = '\0';
    
    i = 0;
    while (lo_mask[i]) { interfaces[1].netmask[i] = lo_mask[i]; i++; }
    interfaces[1].netmask[i] = '\0';
    interfaces[1].gateway[0] = '-';
    interfaces[1].gateway[1] = '\0';
    
    iface_count = 2;
}

// Format bytes as human readable
static void format_bytes(int bytes, char *buf) {
    if (bytes >= 1000000000) {
        int gb = bytes / 1000000000;
        int frac = (bytes % 1000000000) / 100000000;
        buf[0] = '0' + (gb / 10);
        buf[1] = '0' + (gb % 10);
        buf[2] = '.';
        buf[3] = '0' + frac;
        buf[4] = ' ';
        buf[5] = 'G';
        buf[6] = 'B';
        buf[7] = '\0';
    } else if (bytes >= 1000000) {
        int mb = bytes / 1000000;
        int frac = (bytes % 1000000) / 100000;
        buf[0] = '0' + (mb / 100);
        buf[1] = '0' + ((mb / 10) % 10);
        buf[2] = '0' + (mb % 10);
        buf[3] = '.';
        buf[4] = '0' + frac;
        buf[5] = ' ';
        buf[6] = 'M';
        buf[7] = 'B';
        buf[8] = '\0';
    } else if (bytes >= 1000) {
        int kb = bytes / 1000;
        buf[0] = '0' + (kb / 100);
        buf[1] = '0' + ((kb / 10) % 10);
        buf[2] = '0' + (kb % 10);
        buf[3] = ' ';
        buf[4] = 'K';
        buf[5] = 'B';
        buf[6] = '\0';
    } else {
        gui_itoa(bytes, buf, 16);
        int len = 0;
        while (buf[len]) len++;
        buf[len++] = ' ';
        buf[len++] = 'B';
        buf[len] = '\0';
    }
}

// Draw section header
static void draw_section(int y, const char *title) {
    win_draw_rect(win, 0, y, WIN_W, SECTION_H, THEME_BG_SECONDARY);
    win_draw_text(win, MARGIN, y + 6, title, THEME_TEXT_PRIMARY);
}

// Draw interface selector
static void draw_interface_selector(int y) {
    draw_section(y, "Network Interfaces");
    
    int btn_y = y + SECTION_H + 8;
    int btn_w = (WIN_W - 2 * MARGIN - 8) / 2;
    
    for (int i = 0; i < iface_count; i++) {
        int bx = MARGIN + (i % 2) * (btn_w + 8);
        int by = btn_y + (i / 2) * 36;
        
        uint32_t bg = (i == selected_iface) ? THEME_ACCENT : THEME_BUTTON_BG;
        win_draw_rect(win, bx, by, btn_w, 32, bg);
        
        // Interface name
        win_draw_text(win, bx + 8, by + 4, interfaces[i].name, THEME_BUTTON_TEXT);
        
        // Status indicator
        uint32_t status_color = interfaces[i].connected ? THEME_SUCCESS : THEME_ERROR;
        win_draw_rect(win, bx + btn_w - 20, by + 12, 8, 8, status_color);
        
        // IP address
        win_draw_text(win, bx + 8, by + 18, interfaces[i].ip, THEME_TEXT_SECONDARY);
    }
}

// Draw IP configuration
static void draw_ip_config(int y) {
    draw_section(y, "IP Configuration");
    
    netif_t *iface = &interfaces[selected_iface];
    int row_y = y + SECTION_H + 4;
    int label_x = MARGIN;
    int value_x = 120;
    
    // DHCP toggle
    win_draw_text(win, label_x, row_y, "DHCP:", THEME_TEXT_SECONDARY);
    
    uint32_t toggle_bg = dhcp_enabled ? TOGGLE_ON : TOGGLE_OFF;
    win_draw_rect(win, value_x, row_y - 2, TOGGLE_WIDTH, TOGGLE_HEIGHT, toggle_bg);
    int thumb_x = dhcp_enabled ? value_x + TOGGLE_WIDTH - 18 : value_x + 2;
    win_draw_rect(win, thumb_x, row_y, 16, 16, TOGGLE_THUMB);
    
    row_y += ROW_H;
    
    // IP Address
    win_draw_text(win, label_x, row_y, "IP Address:", THEME_TEXT_SECONDARY);
    win_draw_text(win, value_x, row_y, iface->ip, THEME_TEXT_PRIMARY);
    row_y += ROW_H;
    
    // Netmask
    win_draw_text(win, label_x, row_y, "Netmask:", THEME_TEXT_SECONDARY);
    win_draw_text(win, value_x, row_y, iface->netmask, THEME_TEXT_PRIMARY);
    row_y += ROW_H;
    
    // Gateway
    win_draw_text(win, label_x, row_y, "Gateway:", THEME_TEXT_SECONDARY);
    win_draw_text(win, value_x, row_y, iface->gateway, THEME_TEXT_PRIMARY);
    row_y += ROW_H;
    
    // MAC
    win_draw_text(win, label_x, row_y, "MAC Address:", THEME_TEXT_SECONDARY);
    win_draw_text(win, value_x, row_y, iface->mac, THEME_TEXT_PRIMARY);
}

// Draw statistics
static void draw_stats(int y) {
    draw_section(y, "Statistics");
    
    netif_t *iface = &interfaces[selected_iface];
    int row_y = y + SECTION_H + 4;
    int label_x = MARGIN;
    int value_x = 120;
    
    // Status
    win_draw_text(win, label_x, row_y, "Status:", THEME_TEXT_SECONDARY);
    const char *status = iface->connected ? "Connected" : "Disconnected";
    uint32_t status_color = iface->connected ? THEME_SUCCESS : THEME_ERROR;
    win_draw_text(win, value_x, row_y, status, status_color);
    row_y += ROW_H;
    
    // Speed
    win_draw_text(win, label_x, row_y, "Speed:", THEME_TEXT_SECONDARY);
    if (iface->speed_mbps > 0) {
        char speed_str[16];
        gui_itoa(iface->speed_mbps, speed_str, 8);
        int len = 0;
        while (speed_str[len]) len++;
        speed_str[len++] = ' ';
        speed_str[len++] = 'M';
        speed_str[len++] = 'b';
        speed_str[len++] = 'p';
        speed_str[len++] = 's';
        speed_str[len] = '\0';
        win_draw_text(win, value_x, row_y, speed_str, THEME_TEXT_PRIMARY);
    } else {
        win_draw_text(win, value_x, row_y, "N/A", THEME_TEXT_SECONDARY);
    }
    row_y += ROW_H;
    
    // RX bytes
    win_draw_text(win, label_x, row_y, "Received:", THEME_TEXT_SECONDARY);
    char rx_str[20];
    format_bytes(iface->bytes_rx, rx_str);
    win_draw_text(win, value_x, row_y, rx_str, THEME_TEXT_PRIMARY);
    row_y += ROW_H;
    
    // TX bytes
    win_draw_text(win, label_x, row_y, "Sent:", THEME_TEXT_SECONDARY);
    char tx_str[20];
    format_bytes(iface->bytes_tx, tx_str);
    win_draw_text(win, value_x, row_y, tx_str, THEME_TEXT_PRIMARY);
}

// Full redraw
static void draw_all(void) {
    // Background
    win_draw_rect(win, 0, 0, WIN_W, WIN_H, THEME_BG_PRIMARY);
    
    // Sections
    draw_interface_selector(0);
    draw_ip_config(92);
    draw_stats(244);
    
    win_invalidate(win);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    // Create window
    win = win_create("Network Settings", 100, 70, WIN_W, WIN_H);
    if (win < 0) {
        printf("Failed to create window\n");
        return 1;
    }
    
    printf("Network Settings window created (handle=%d)\n", win);
    
    // Initialize demo data
    init_interfaces();
    
    // Initial draw
    draw_all();
    
    // Event loop
    gui_event_t event;
    int running = 1;
    int win_x = 100, win_y = 70;
    
    while (running) {
        int event_type = win_get_event(win, &event, 100);
        if (event_type == 0) continue;
        
        switch (event.type) {
            case EVENT_REDRAW:
                draw_all();
                break;
                
            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;
                
            case EVENT_KEY_DOWN:
                if (event.key_char == 27) {
                    running = 0;
                } else if (event.key_char == '1' && iface_count > 0) {
                    selected_iface = 0;
                    draw_all();
                } else if (event.key_char == '2' && iface_count > 1) {
                    selected_iface = 1;
                    draw_all();
                }
                break;
                
            case EVENT_MOUSE_DOWN:
                if (event.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    int lx = event.mouse_x;
                    int ly = event.mouse_y;
                    
                    // Check interface buttons
                    int btn_y = SECTION_H + 8;
                    int btn_w = (WIN_W - 2 * MARGIN - 8) / 2;
                    
                    for (int i = 0; i < iface_count; i++) {
                        int bx = MARGIN + (i % 2) * (btn_w + 8);
                        int by = btn_y + (i / 2) * 36;
                        
                        if (lx >= bx && lx < bx + btn_w && ly >= by && ly < by + 32) {
                            selected_iface = i;
                            draw_all();
                            break;
                        }
                    }
                    
                    // Check DHCP toggle
                    int toggle_y = 92 + SECTION_H + 4 - 2;
                    if (lx >= 120 && lx < 120 + TOGGLE_WIDTH &&
                        ly >= toggle_y && ly < toggle_y + TOGGLE_HEIGHT) {
                        dhcp_enabled = !dhcp_enabled;
                        draw_all();
                    }
                }
                break;
                
            default:
                break;
        }
    }
    
    win_destroy(win);
    printf("Network Settings closed\n");
    
    return 0;
}
