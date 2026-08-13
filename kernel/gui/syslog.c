// syslog.c - the kernel-wide system log ring.
//
// #703: this file used to be TWO things: the log ring that every subsystem
// writes to through the LOG_INFO/LOG_ERROR macros (syslog_log has ~30 callers
// across mm/, proc/, drivers/, net/ and gui/), and a GUI log-VIEWER window that
// was reachable only from gui/registry.c, which had no caller. The viewer has
// been deleted; the userland /APPS/SYSLOG is the shipping log viewer. The ring
// stays, unchanged, because deleting this file would have broken the whole
// kernel. Do not re-merge a GUI into it.
#include "syslog.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"

// External timer
extern volatile uint64_t timer_ticks;

// Global log storage
static log_entry_t g_log_entries[SYSLOG_MAX_ENTRIES];
static int g_log_count = 0;
static int g_log_head = 0;  // Circular buffer head
static bool g_log_initialized = false;

// Initialize system log
void syslog_init(void) {
    if (g_log_initialized) return;

    memset(g_log_entries, 0, sizeof(g_log_entries));
    g_log_count = 0;
    g_log_head = 0;
    g_log_initialized = true;
}

// Level prefixes for serial output
static const char *level_prefix[] = {
    "[DBG]", "[INF]", "[WRN]", "[ERR]", "[CRT]"
};

// Add log entry
void syslog_log(log_level_t level, const char *message) {
    if (!g_log_initialized) {
        syslog_init();
    }

    if (!message) return;

    // Always output to serial for persistence
    uint32_t secs = (uint32_t)(timer_ticks / 100);
    int mins = secs / 60;
    secs = secs % 60;
    kprintf("%02d:%02d %s %s\n", mins, secs, level_prefix[level], message);

    // Find insertion point (circular buffer)
    int idx;
    if (g_log_count < SYSLOG_MAX_ENTRIES) {
        idx = g_log_count;
        g_log_count++;
    } else {
        // Overwrite oldest entry
        idx = g_log_head;
        g_log_head = (g_log_head + 1) % SYSLOG_MAX_ENTRIES;
    }

    log_entry_t *entry = &g_log_entries[idx];
    strncpy(entry->message, message, SYSLOG_MSG_MAX - 1);
    entry->message[SYSLOG_MSG_MAX - 1] = '\0';
    entry->level = level;
    entry->timestamp = timer_ticks;
    entry->valid = true;
}

// Get log count
int syslog_get_count(void) {
    return g_log_count;
}

// Get log entry (0 = oldest)
log_entry_t *syslog_get_entry(int index) {
    if (index < 0 || index >= g_log_count) {
        return NULL;
    }

    // Convert to circular buffer index
    int real_idx = (g_log_head + index) % SYSLOG_MAX_ENTRIES;
    return &g_log_entries[real_idx];
}

// Clear all logs
void syslog_clear(void) {
    for (int i = 0; i < SYSLOG_MAX_ENTRIES; i++) {
        g_log_entries[i].valid = false;
    }
    g_log_count = 0;
    g_log_head = 0;
}
