// panel_network.h - Network Panel for MayteraOS Settings
// Part of Task #59 - Unified Settings App
#ifndef PANEL_NETWORK_H
#define PANEL_NETWORK_H

#include "../../types.h"
#include "../window.h"

// Network sub-panel tabs
#define NET_TAB_STATUS      0
#define NET_TAB_CONFIG      1
#define NET_TAB_DIAG        2
#define NET_TAB_COUNT       3

// Network field indices (for keyboard navigation)
#define NET_FIELD_DHCP          0
#define NET_FIELD_IP            1
#define NET_FIELD_SUBNET        2
#define NET_FIELD_GATEWAY       3
#define NET_FIELD_DNS           4
#define NET_FIELD_PING_TARGET   5
#define NET_FIELD_DNS_HOST      6

// Network configuration structure
typedef struct {
    bool use_dhcp;          // Use DHCP or static IP
    uint32_t static_ip;     // Static IP address
    uint32_t subnet_mask;   // Subnet mask
    uint32_t gateway;       // Default gateway
    uint32_t dns_server;    // DNS server
} net_config_t;

// Network panel state
typedef struct {
    int current_tab;            // Current sub-tab (Status, Configure, Diagnostics)
    int selected_field;         // Currently selected input field (-1 = none)

    net_config_t config;        // Current configuration

    // Status message
    char status_message[64];
    bool status_is_error;

    // Diagnostics state
    uint32_t ping_target;       // IP to ping
    bool ping_in_progress;
    bool ping_success;
    char ping_result[32];

    char dns_hostname[64];      // Hostname to lookup
    bool dns_in_progress;
    bool dns_success;
    char dns_result[32];

    // Button positions (for click detection)
    int apply_button_x, apply_button_y;
    int dhcp_button_x, dhcp_button_y;
    int ping_button_x, ping_button_y;
    int dns_button_x, dns_button_y;
} network_panel_t;

// Create network panel
network_panel_t *network_panel_create(void);

// Destroy network panel
void network_panel_destroy(network_panel_t *panel);

// Draw network panel content
// x, y, w, h: bounds of the panel area within the settings window
void network_panel_draw(network_panel_t *panel, int x, int y, int w, int h);

// Handle events for network panel
void network_panel_handle_event(network_panel_t *panel, gui_event_t *event,
                                 int x, int y, int w, int h);

// Get current sub-tab
int network_panel_get_tab(network_panel_t *panel);

// Set current sub-tab
void network_panel_set_tab(network_panel_t *panel, int tab);

// Refresh network status (call periodically to update status display)
void network_panel_refresh(network_panel_t *panel);

#endif // PANEL_NETWORK_H
