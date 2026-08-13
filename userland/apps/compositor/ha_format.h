// ha_format.h - Home Assistant entity-state display formatter (#723).
//
// Turns a raw HA (domain, device_class, state, unit) tuple into what a human
// should actually see: humanized text ("Below Horizon", not "below_horizon"),
// a graphic (an icon_id_t from the shared icon set), and a semantic bucket
// the caller maps to a theme color (success/warning/error/muted/info via
// THEME_COLOR_* - see compositor/widgets.c ha_card_draw()).
//
// This module is pure logic: it knows nothing about drawing, the framebuffer,
// or the active theme. It is keyed on DOMAIN and DEVICE_CLASS, never on
// individual entity_ids - #723 was explicit that two examples (sun.sun,
// a garage door) are a starting symptom, not the scope, and every entity in
// the instance (2033 measured, 30 domains, ~90 (domain,device_class) pairs)
// must route through the same small set of rules.
#ifndef HA_FORMAT_H
#define HA_FORMAT_H

#include "compositor.h"   // icon_id_t

typedef enum {
    HA_SEM_ACTIVE,      // a "good"/on/detected/home state -> THEME_COLOR_SUCCESS
    HA_SEM_NEUTRAL,     // an inactive/off/idle/no-signal state -> THEME_COLOR_MUTED
    HA_SEM_INFO,        // a plain reading with nothing to flag -> THEME_COLOR_INFO
    HA_SEM_WARN,        // worth a glance (update available, low battery) -> THEME_COLOR_WARNING
    HA_SEM_ALERT,       // a real problem (smoke, jammed lock, problem=on) -> THEME_COLOR_ERROR
    HA_SEM_UNAVAILABLE  // entity/device offline -> THEME_COLOR_MUTED, but the
                         // WARN icon shape still marks it as "not a real reading"
} ha_semantic_t;

typedef struct {
    char          text[48];   // humanized, unit-suffixed where numeric
    icon_id_t     icon;
    ha_semantic_t sem;
} ha_display_t;

// domain and device_class are plain C strings (device_class may be "" - most
// entities have none). raw_state/unit come straight from the /HA<n>.TXT cache
// line (see haservice/main.c). Always fills *out; never fails.
void ha_format_state(const char *domain, const char *device_class,
                      const char *raw_state, const char *unit,
                      ha_display_t *out);

#endif
