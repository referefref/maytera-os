// netsettings.h - Network Settings UI for MayteraOS
#ifndef NETSETTINGS_H
#define NETSETTINGS_H

#include "window.h"

// IP address field
typedef struct {
    uint8_t octets[4];
} ip_addr_field_t;

// Network configuration
typedef struct {
    bool use_dhcp;              // true = DHCP, false = static
    ip_addr_field_t ip;
    ip_addr_field_t subnet;
    ip_addr_field_t gateway;
    ip_addr_field_t dns1;
    ip_addr_field_t dns2;
} net_config_t;

// Network settings window state
typedef struct {
    window_t *window;

    net_config_t config;        // Current configuration
    net_config_t saved_config;  // Saved configuration

    int selected_field;         // Currently selected field (0-5)
    int cursor_octet;           // Which octet in IP field (0-3)
    bool editing;               // Are we editing a field?

    char status_message[64];    // Status message to display
} netsettings_t;

// Field indices
#define NETFIELD_DHCP       0
#define NETFIELD_IP         1
#define NETFIELD_SUBNET     2
#define NETFIELD_GATEWAY    3
#define NETFIELD_DNS1       4
#define NETFIELD_DNS2       5
#define NETFIELD_COUNT      6

// Create network settings window
netsettings_t *netsettings_create(void);

// Destroy network settings
void netsettings_destroy(netsettings_t *ns);

// Apply configuration to network stack
bool netsettings_apply(netsettings_t *ns);

// Event handling
void netsettings_handle_event(netsettings_t *ns, gui_event_t *event);

// Drawing
void netsettings_draw(netsettings_t *ns);

// Launch network settings
void netsettings_launch(void);

#endif // NETSETTINGS_H
