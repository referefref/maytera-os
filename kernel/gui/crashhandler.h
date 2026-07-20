// crashhandler.h - Crash Handler Service for MayteraOS
#ifndef CRASHHANDLER_H
#define CRASHHANDLER_H

#include "../types.h"
#include "window.h"

// Crash types
typedef enum {
    CRASH_PAGE_FAULT,
    CRASH_GENERAL_PROTECTION,
    CRASH_INVALID_OPCODE,
    CRASH_DIVIDE_BY_ZERO,
    CRASH_DOUBLE_FAULT,
    CRASH_STACK_FAULT,
    CRASH_ASSERTION_FAILED,
    CRASH_NULL_POINTER,
    CRASH_UNKNOWN
} crash_type_t;

// CPU register state at crash
typedef struct {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
    uint64_t cs, ss;
    uint64_t cr2;  // Page fault address
    uint64_t error_code;
} crash_regs_t;

// Memory snapshot (stack and nearby memory)
#define CRASH_STACK_SNAPSHOT_SIZE   256  // bytes
#define CRASH_CODE_SNAPSHOT_SIZE    64   // bytes around RIP

typedef struct {
    uint8_t stack[CRASH_STACK_SNAPSHOT_SIZE];
    uint8_t code[CRASH_CODE_SNAPSHOT_SIZE];
    uint64_t stack_base;
    uint64_t code_base;
} crash_memory_t;

// Crash info structure
typedef struct {
    crash_type_t type;
    const char *type_name;
    const char *description;
    crash_regs_t regs;
    crash_memory_t memory;
    
    // App info if applicable
    int app_id;
    const char *app_name;
    window_t *app_window;
    
    // Timestamp
    uint64_t timestamp;
    
    // Stack trace entries (populated from user stack by exception handler)
    uint64_t stack_entries[8];
    int stack_entry_count;
    
    // Valid flag
    bool valid;
} crash_info_t;

// Crash log (circular buffer of recent crashes)
#define CRASH_LOG_SIZE  8

// ============================================
// Public API
// ============================================

/**
 * Initialize the crash handler service
 */
void crashhandler_init(void);

/**
 * Report a crash (called from exception handlers)
 * @param type      Type of crash
 * @param regs      CPU registers at time of crash
 * @param app_id    App ID if known (-1 for kernel)
 */
void crashhandler_report(crash_type_t type, crash_regs_t *regs, int app_id);

/**
 * Show the crash dialog
 * This is called after a crash to display info and let user take action
 */
void crashhandler_show_dialog(void);

/**
 * Get the most recent crash info
 * @return Pointer to crash info, or NULL if no crashes
 */
crash_info_t *crashhandler_get_last(void);

/**
 * Get crash at index in log (0 = most recent)
 * @param index     Index in crash log
 * @return Pointer to crash info, or NULL if invalid index
 */
crash_info_t *crashhandler_get(int index);

/**
 * Get count of crashes in log
 */
int crashhandler_get_count(void);

/**
 * Kill the crashed application
 * @param crash     Crash info
 * @return true if app was killed
 */
bool crashhandler_kill_app(crash_info_t *crash);

/**
 * Try to recover from crash (if possible)
 * @param crash     Crash info
 * @return true if recovery was attempted
 */
bool crashhandler_try_recover(crash_info_t *crash);

/**
 * Dump crash info to serial console
 * @param crash     Crash info to dump
 */
void crashhandler_dump_serial(crash_info_t *crash);

/**
 * Clear crash log
 */
void crashhandler_clear_log(void);

/**
 * Get crash type name string
 */
const char *crashhandler_type_name(crash_type_t type);

#endif // CRASHHANDLER_H
