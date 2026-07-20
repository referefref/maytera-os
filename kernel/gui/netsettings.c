// netsettings.c - Network Settings UI for MayteraOS
#include "netsettings.h"
#include "window.h"
#include "icons.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../net/net.h"
#include "../net/ip.h"
#include "syslog.h"

// Global DHCP mode flag (we add this)
static bool g_use_dhcp = true;

// DNS server address (we add this)
static uint32_t g_dns_server = 0x08080808;  // Default: 8.8.8.8

// Colors
#define NS_BG_COLOR         0xF0F0F0
#define NS_HEADER_COLOR     0x3050A0
#define NS_TEXT_COLOR       0x202020
#define NS_TEXT_DIM_COLOR   0x808080
#define NS_FIELD_BG         0xFFFFFF
#define NS_FIELD_BORDER     0x909090
#define NS_FIELD_ACTIVE     0x3080D0
#define NS_BUTTON_BG        0x4080C0
#define NS_BUTTON_TEXT      0xFFFFFF
#define NS_STATUS_OK        0x40A040
#define NS_STATUS_ERROR     0xC04040

// UI dimensions
#define NS_PADDING          16
#define NS_FIELD_HEIGHT     28
#define NS_FIELD_WIDTH      180
#define NS_LABEL_WIDTH      100
#define NS_ROW_HEIGHT       36
#define NS_HEADER_HEIGHT    40

// Helper to convert IP field to string (for future use)
__attribute__((unused))
static void ip_to_str(ip_addr_field_t *ip, char *buf) {
    char *p = buf;
    for (int i = 0; i < 4; i++) {
        int val = ip->octets[i];
        if (val >= 100) {
            *p++ = '0' + (val / 100);
            *p++ = '0' + ((val / 10) % 10);
            *p++ = '0' + (val % 10);
        } else if (val >= 10) {
            *p++ = '0' + (val / 10);
            *p++ = '0' + (val % 10);
        } else {
            *p++ = '0' + val;
        }
        if (i < 3) *p++ = '.';
    }
    *p = '\0';
}

// Draw text helper
static void ns_draw_text(const char *text, int x, int y, uint32_t color) {
    for (int i = 0; text[i]; i++) {
        const uint8_t *glyph = font_get_glyph(text[i]);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    fb_put_pixel(x + i * 8 + col, y + row, color);
                }
            }
        }
    }
}

// Draw button
static void ns_draw_button(int x, int y, int w, int h, const char *label, bool enabled) {
    uint32_t bg = enabled ? NS_BUTTON_BG : 0x909090;
    fb_fill_rect(x, y, w, h, bg);
    fb_draw_rect(x, y, w, h, 0x404040);

    int text_len = strlen(label) * 8;
    int tx = x + (w - text_len) / 2;
    int ty = y + (h - 16) / 2;
    ns_draw_text(label, tx, ty, NS_BUTTON_TEXT);
}

// Draw checkbox
static void ns_draw_checkbox(int x, int y, bool checked, bool selected) {
    uint32_t border = selected ? NS_FIELD_ACTIVE : NS_FIELD_BORDER;
    fb_fill_rect(x, y, 18, 18, NS_FIELD_BG);
    fb_draw_rect(x, y, 18, 18, border);

    if (checked) {
        // Draw checkmark
        for (int i = 0; i < 4; i++) {
            fb_put_pixel(x + 4 + i, y + 10 - i, NS_TEXT_COLOR);
            fb_put_pixel(x + 5 + i, y + 10 - i, NS_TEXT_COLOR);
        }
        for (int i = 0; i < 8; i++) {
            fb_put_pixel(x + 8 + i, y + 6 + i, NS_TEXT_COLOR);
            fb_put_pixel(x + 9 + i, y + 6 + i, NS_TEXT_COLOR);
        }
    }
}

// Draw IP input field
static void ns_draw_ip_field(int x, int y, ip_addr_field_t *ip, bool selected, int cursor_octet, bool editing) {
    uint32_t border = selected ? NS_FIELD_ACTIVE : NS_FIELD_BORDER;
    fb_fill_rect(x, y, NS_FIELD_WIDTH, NS_FIELD_HEIGHT, NS_FIELD_BG);
    fb_draw_rect(x, y, NS_FIELD_WIDTH, NS_FIELD_HEIGHT, border);

    // Draw each octet
    int ox = x + 6;
    for (int i = 0; i < 4; i++) {
        // Highlight current octet if editing
        if (selected && editing && cursor_octet == i) {
            fb_fill_rect(ox - 2, y + 2, 38, NS_FIELD_HEIGHT - 4, 0xD0E0FF);
        }

        char octet_str[4];
        int val = ip->octets[i];
        if (val >= 100) {
            octet_str[0] = '0' + (val / 100);
            octet_str[1] = '0' + ((val / 10) % 10);
            octet_str[2] = '0' + (val % 10);
            octet_str[3] = '\0';
        } else if (val >= 10) {
            octet_str[0] = '0' + (val / 10);
            octet_str[1] = '0' + (val % 10);
            octet_str[2] = '\0';
        } else {
            octet_str[0] = '0' + val;
            octet_str[1] = '\0';
        }

        ns_draw_text(octet_str, ox, y + 6, NS_TEXT_COLOR);
        ox += 40;

        if (i < 3) {
            ns_draw_text(".", ox - 10, y + 6, NS_TEXT_DIM_COLOR);
        }
    }
}

// Create network settings
netsettings_t *netsettings_create(void) {
    netsettings_t *ns = (netsettings_t *)kmalloc(sizeof(netsettings_t));
    if (!ns) return NULL;

    memset(ns, 0, sizeof(netsettings_t));

    ns->window = window_create("Network Settings", 120, 70, 380, 340);
    if (!ns->window) {
        kfree(ns);
        return NULL;
    }

    // Disable resize for this window
    ns->window->flags &= ~WINDOW_FLAG_RESIZABLE;

    // Load current network configuration from IP module
    ns->config.use_dhcp = g_use_dhcp;

    // IP address - stored as host byte order
    uint32_t ip = ip_get_address();
    ns->config.ip.octets[0] = (ip >> 24) & 0xFF;
    ns->config.ip.octets[1] = (ip >> 16) & 0xFF;
    ns->config.ip.octets[2] = (ip >> 8) & 0xFF;
    ns->config.ip.octets[3] = ip & 0xFF;

    // Subnet mask
    uint32_t subnet = ip_get_netmask();
    ns->config.subnet.octets[0] = (subnet >> 24) & 0xFF;
    ns->config.subnet.octets[1] = (subnet >> 16) & 0xFF;
    ns->config.subnet.octets[2] = (subnet >> 8) & 0xFF;
    ns->config.subnet.octets[3] = subnet & 0xFF;

    // Gateway
    uint32_t gateway = ip_get_gateway();
    ns->config.gateway.octets[0] = (gateway >> 24) & 0xFF;
    ns->config.gateway.octets[1] = (gateway >> 16) & 0xFF;
    ns->config.gateway.octets[2] = (gateway >> 8) & 0xFF;
    ns->config.gateway.octets[3] = gateway & 0xFF;

    // DNS server
    ns->config.dns1.octets[0] = (g_dns_server >> 24) & 0xFF;
    ns->config.dns1.octets[1] = (g_dns_server >> 16) & 0xFF;
    ns->config.dns1.octets[2] = (g_dns_server >> 8) & 0xFF;
    ns->config.dns1.octets[3] = g_dns_server & 0xFF;

    // Default secondary DNS
    ns->config.dns2.octets[0] = 8;
    ns->config.dns2.octets[1] = 8;
    ns->config.dns2.octets[2] = 4;
    ns->config.dns2.octets[3] = 4;

    ns->saved_config = ns->config;

    ns->selected_field = 0;
    ns->cursor_octet = 0;
    ns->editing = false;

    strncpy(ns->status_message, "Ready", 63);

    return ns;
}

// Destroy
void netsettings_destroy(netsettings_t *ns) {
    if (!ns) return;
    if (ns->window) {
        window_destroy(ns->window);
    }
    kfree(ns);
}

// Get IP field by index
static ip_addr_field_t *get_ip_field(netsettings_t *ns, int field) {
    switch (field) {
        case NETFIELD_IP:       return &ns->config.ip;
        case NETFIELD_SUBNET:   return &ns->config.subnet;
        case NETFIELD_GATEWAY:  return &ns->config.gateway;
        case NETFIELD_DNS1:     return &ns->config.dns1;
        case NETFIELD_DNS2:     return &ns->config.dns2;
        default:                return NULL;
    }
}

// Apply configuration
bool netsettings_apply(netsettings_t *ns) {
    if (!ns) return false;

    // Convert IP fields to 32-bit values
    uint32_t ip = (ns->config.ip.octets[0] << 24) |
                  (ns->config.ip.octets[1] << 16) |
                  (ns->config.ip.octets[2] << 8) |
                  ns->config.ip.octets[3];

    uint32_t subnet = (ns->config.subnet.octets[0] << 24) |
                      (ns->config.subnet.octets[1] << 16) |
                      (ns->config.subnet.octets[2] << 8) |
                      ns->config.subnet.octets[3];

    uint32_t gateway = (ns->config.gateway.octets[0] << 24) |
                       (ns->config.gateway.octets[1] << 16) |
                       (ns->config.gateway.octets[2] << 8) |
                       ns->config.gateway.octets[3];

    uint32_t dns = (ns->config.dns1.octets[0] << 24) |
                   (ns->config.dns1.octets[1] << 16) |
                   (ns->config.dns1.octets[2] << 8) |
                   ns->config.dns1.octets[3];

    // Update global DHCP flag
    g_use_dhcp = ns->config.use_dhcp;
    g_dns_server = dns;

    // Apply to IP module (only if not using DHCP)
    if (!ns->config.use_dhcp) {
        ip_set_address(ip);
        ip_set_netmask(subnet);
        ip_set_gateway(gateway);
    }

    ns->saved_config = ns->config;

    kprintf("[NetSettings] Applied: IP=%d.%d.%d.%d, DHCP=%s\n",
            ns->config.ip.octets[0], ns->config.ip.octets[1],
            ns->config.ip.octets[2], ns->config.ip.octets[3],
            ns->config.use_dhcp ? "yes" : "no");

    strncpy(ns->status_message, "Settings applied", 63);
    return true;
}

// Event handling
void netsettings_handle_event(netsettings_t *ns, gui_event_t *event) {
    if (!ns || !event) return;

    switch (event->type) {
        case EVENT_KEY_DOWN:
            if (ns->editing && ns->selected_field > NETFIELD_DHCP) {
                // Editing IP field
                ip_addr_field_t *field = get_ip_field(ns, ns->selected_field);
                if (!field) break;

                if (event->keycode >= '0' && event->keycode <= '9') {
                    // Add digit to current octet
                    int digit = event->keycode - '0';
                    int current = field->octets[ns->cursor_octet];
                    int new_val = current * 10 + digit;
                    if (new_val <= 255) {
                        field->octets[ns->cursor_octet] = new_val;
                    }
                } else if (event->keycode == '.' || event->keycode == 0x4D) {  // Period or right arrow
                    // Move to next octet
                    if (ns->cursor_octet < 3) ns->cursor_octet++;
                } else if (event->keycode == 0x08) {  // Backspace
                    field->octets[ns->cursor_octet] /= 10;
                } else if (event->keycode == 0x4B) {  // Left arrow
                    if (ns->cursor_octet > 0) ns->cursor_octet--;
                } else if (event->keycode == 0x0D || event->keycode == 0x1B) {  // Enter or Escape
                    ns->editing = false;
                }
            } else {
                // Not editing
                switch (event->keycode) {
                    case 0x48:  // Up arrow
                        if (ns->selected_field > 0) ns->selected_field--;
                        ns->cursor_octet = 0;
                        break;
                    case 0x50:  // Down arrow
                        if (ns->selected_field < NETFIELD_COUNT - 1) ns->selected_field++;
                        ns->cursor_octet = 0;
                        break;
                    case 0x0D:  // Enter
                    case ' ':   // Space
                        if (ns->selected_field == NETFIELD_DHCP) {
                            ns->config.use_dhcp = !ns->config.use_dhcp;
                        } else {
                            ns->editing = true;
                            ns->cursor_octet = 0;
                        }
                        break;
                    case 'a':
                    case 'A':
                        netsettings_apply(ns);
                        break;
                }
            }
            break;

        case EVENT_MOUSE_DOWN:
            {
                int32_t wx, wy, ww, wh;
                window_get_content_bounds(ns->window, &wx, &wy, &ww, &wh);

                int mx = event->mouse_x;
                int my = event->mouse_y;

                // Check field clicks
                int row_y = wy + NS_HEADER_HEIGHT + NS_PADDING;

                for (int i = 0; i < NETFIELD_COUNT; i++) {
                    int field_x = wx + NS_LABEL_WIDTH + NS_PADDING;

                    if (my >= row_y && my < row_y + NS_FIELD_HEIGHT) {
                        ns->selected_field = i;
                        ns->cursor_octet = 0;

                        if (i == NETFIELD_DHCP) {
                            // Toggle checkbox if clicked on it
                            if (mx >= field_x && mx < field_x + 18) {
                                ns->config.use_dhcp = !ns->config.use_dhcp;
                            }
                        } else if (!ns->config.use_dhcp || i >= NETFIELD_DNS1) {
                            ns->editing = true;
                            // Determine which octet was clicked
                            int octet_x = field_x + 6;
                            for (int o = 0; o < 4; o++) {
                                if (mx >= octet_x && mx < octet_x + 36) {
                                    ns->cursor_octet = o;
                                    break;
                                }
                                octet_x += 40;
                            }
                        }
                        break;
                    }
                    row_y += NS_ROW_HEIGHT;
                }

                // Check Apply button
                int btn_y = wy + wh - NS_PADDING - 30;
                int btn_x = wx + ww - NS_PADDING - 80;
                if (mx >= btn_x && mx < btn_x + 80 &&
                    my >= btn_y && my < btn_y + 30) {
                    netsettings_apply(ns);
                }
            }
            break;

        default:
            break;
    }
}

// Drawing
void netsettings_draw(netsettings_t *ns) {
    if (!ns || !ns->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(ns->window, &wx, &wy, &ww, &wh);

    // Background
    fb_fill_rect(wx, wy, ww, wh, NS_BG_COLOR);

    // Header
    fb_fill_rect(wx, wy, ww, NS_HEADER_HEIGHT, NS_HEADER_COLOR);
    icon_draw(ICON_NETWORK, wx + NS_PADDING, wy + 8, 0xFFFFFF);
    ns_draw_text("Network Configuration", wx + NS_PADDING + 28, wy + 12, 0xFFFFFF);

    // Network status
    bool link_up = nic_link_up();
    const char *status = link_up ? "Connected" : "Disconnected";
    uint32_t status_color = link_up ? NS_STATUS_OK : NS_STATUS_ERROR;
    ns_draw_text(status, wx + ww - NS_PADDING - strlen(status) * 8, wy + 12, status_color);

    // Fields
    int row_y = wy + NS_HEADER_HEIGHT + NS_PADDING;
    int label_x = wx + NS_PADDING;
    int field_x = wx + NS_LABEL_WIDTH + NS_PADDING;

    // DHCP toggle
    ns_draw_text("Use DHCP:", label_x, row_y + 1, NS_TEXT_COLOR);
    ns_draw_checkbox(field_x, row_y, ns->config.use_dhcp, ns->selected_field == NETFIELD_DHCP);
    row_y += NS_ROW_HEIGHT;

    // IP fields (disabled if DHCP)
    bool static_enabled = !ns->config.use_dhcp;

    // IP Address
    uint32_t label_color = static_enabled ? NS_TEXT_COLOR : NS_TEXT_DIM_COLOR;
    ns_draw_text("IP Address:", label_x, row_y + 6, label_color);
    ns_draw_ip_field(field_x, row_y, &ns->config.ip, ns->selected_field == NETFIELD_IP && static_enabled,
                     ns->cursor_octet, ns->editing);
    row_y += NS_ROW_HEIGHT;

    // Subnet Mask
    ns_draw_text("Subnet:", label_x, row_y + 6, label_color);
    ns_draw_ip_field(field_x, row_y, &ns->config.subnet, ns->selected_field == NETFIELD_SUBNET && static_enabled,
                     ns->cursor_octet, ns->editing);
    row_y += NS_ROW_HEIGHT;

    // Gateway
    ns_draw_text("Gateway:", label_x, row_y + 6, label_color);
    ns_draw_ip_field(field_x, row_y, &ns->config.gateway, ns->selected_field == NETFIELD_GATEWAY && static_enabled,
                     ns->cursor_octet, ns->editing);
    row_y += NS_ROW_HEIGHT;

    // DNS 1 (always editable)
    ns_draw_text("DNS 1:", label_x, row_y + 6, NS_TEXT_COLOR);
    ns_draw_ip_field(field_x, row_y, &ns->config.dns1, ns->selected_field == NETFIELD_DNS1,
                     ns->cursor_octet, ns->editing);
    row_y += NS_ROW_HEIGHT;

    // DNS 2
    ns_draw_text("DNS 2:", label_x, row_y + 6, NS_TEXT_COLOR);
    ns_draw_ip_field(field_x, row_y, &ns->config.dns2, ns->selected_field == NETFIELD_DNS2,
                     ns->cursor_octet, ns->editing);
    row_y += NS_ROW_HEIGHT;

    // Status message
    ns_draw_text(ns->status_message, label_x, row_y + 6, NS_TEXT_DIM_COLOR);

    // Apply button
    int btn_y = wy + wh - NS_PADDING - 30;
    int btn_x = wx + ww - NS_PADDING - 80;
    ns_draw_button(btn_x, btn_y, 80, 30, "Apply", true);

    // Help text
    ns_draw_text("Up/Down: Select  Enter: Edit  A: Apply", label_x, btn_y + 7, NS_TEXT_DIM_COLOR);
}

// Launch
void netsettings_launch(void) {
    LOG_INFO("[NetSettings] Application launched");
    netsettings_t *ns = netsettings_create();
    if (!ns) {
        LOG_ERROR("[NetSettings] Failed to create window");
        kprintf("[NetSettings] Failed to create window\n");
        return;
    }

    wm_register_app(ns->window, ns,
                    (app_event_handler_t)netsettings_handle_event,
                    (app_draw_handler_t)netsettings_draw,
                    (app_destroy_handler_t)netsettings_destroy);

    kprintf("[NetSettings] Launched\n");
}
