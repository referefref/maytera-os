// syslog.h - the kernel-wide system log ring (see syslog.c, #703).
#ifndef SYSLOG_H
#define SYSLOG_H

#include "../types.h"

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

// #703: the in-kernel log VIEWER (syslog_viewer_*) is gone; the shipping log
// viewer is the userland /APPS/SYSLOG. Only the ring survives here.

#endif // SYSLOG_H
