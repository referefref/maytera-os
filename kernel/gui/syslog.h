// syslog.h - System Log and Log Viewer for MayteraOS
#ifndef SYSLOG_H
#define SYSLOG_H

#include "window.h"

// Maximum log entries
#define SYSLOG_MAX_ENTRIES  256

// Maximum message length
#define SYSLOG_MSG_MAX      128

// Log levels
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_CRITICAL
} log_level_t;

// Log entry
typedef struct {
    char message[SYSLOG_MSG_MAX];
    log_level_t level;
    uint64_t timestamp;
    bool valid;
} log_entry_t;

// Log viewer window state
typedef struct {
    window_t *window;

    int scroll_offset;          // Scroll position
    int selected;               // Selected line (-1 = none)

    log_level_t filter_level;   // Minimum level to show
    bool auto_scroll;           // Auto-scroll to bottom

    uint64_t last_update;       // Last refresh time
} syslog_viewer_t;

// Global log functions (called from anywhere in kernel)

// Initialize system log
void syslog_init(void);

// Add log entry
void syslog_log(log_level_t level, const char *message);

// Convenience macros
#define LOG_DEBUG(msg)    syslog_log(LOG_DEBUG, msg)
#define LOG_INFO(msg)     syslog_log(LOG_INFO, msg)
#define LOG_WARNING(msg)  syslog_log(LOG_WARNING, msg)
#define LOG_ERROR(msg)    syslog_log(LOG_ERROR, msg)
#define LOG_CRITICAL(msg) syslog_log(LOG_CRITICAL, msg)

// Get log entries
int syslog_get_count(void);
log_entry_t *syslog_get_entry(int index);

// Clear all logs
void syslog_clear(void);

// Log viewer functions

// Create log viewer window
syslog_viewer_t *syslog_viewer_create(void);

// Destroy log viewer
void syslog_viewer_destroy(syslog_viewer_t *sv);

// Event handling
void syslog_viewer_handle_event(syslog_viewer_t *sv, gui_event_t *event);

// Drawing
void syslog_viewer_draw(syslog_viewer_t *sv);

// Launch log viewer
void syslog_viewer_launch(void);

#endif // SYSLOG_H
