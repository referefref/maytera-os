// panel_hardware.h - Hardware Panel for MayteraOS Settings
// Part of Task #59 - Unified Settings App
#ifndef PANEL_HARDWARE_H
#define PANEL_HARDWARE_H

#include "../../types.h"
#include "../window.h"

// Hardware sub-panel tabs
#define HW_TAB_OVERVIEW     0
#define HW_TAB_STORAGE      1
#define HW_TAB_PCI          2
#define HW_TAB_USB          3
#define HW_TAB_COUNT        4

// Hardware panel state
typedef struct {
    int current_tab;        // Current sub-tab (Overview, Storage, PCI, USB)
    int scroll_offset;      // Scroll position for lists
    int max_scroll;         // Maximum scroll offset
} hardware_panel_t;

// Create hardware panel
hardware_panel_t *hardware_panel_create(void);

// Destroy hardware panel
void hardware_panel_destroy(hardware_panel_t *panel);

// Draw hardware panel content
// x, y, w, h: bounds of the panel area within the settings window
void hardware_panel_draw(hardware_panel_t *panel, int x, int y, int w, int h);

// Handle events for hardware panel
void hardware_panel_handle_event(hardware_panel_t *panel, gui_event_t *event,
                                  int x, int y, int w, int h);

// Get current sub-tab
int hardware_panel_get_tab(hardware_panel_t *panel);

// Set current sub-tab
void hardware_panel_set_tab(hardware_panel_t *panel, int tab);

#endif // PANEL_HARDWARE_H
