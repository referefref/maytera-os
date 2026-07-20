// syscall.h - System call interface for MayteraOS
#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

// ============================================================================
// System Call Numbers
// ============================================================================

// Process control
#define SYS_EXIT            0   // Exit process
#define SYS_FORK            1   // Fork process
#define SYS_EXEC            2   // Execute program
#define SYS_WAIT            3   // Wait for child
#define SYS_GETPID          4   // Get process ID
#define SYS_GETPPID         5   // Get parent process ID
#define SYS_YIELD           6   // Yield CPU
#define SYS_SLEEP           7   // Sleep for milliseconds

// File I/O
#define SYS_OPEN            10  // Open file
#define SYS_CLOSE           11  // Close file
#define SYS_READ            12  // Read from file
#define SYS_WRITE           13  // Write to file
#define SYS_SEEK            14  // Seek in file
#define SYS_STAT            15  // Get file status
#define SYS_MKDIR           16  // Create directory
#define SYS_RMDIR           17  // Remove directory
#define SYS_UNLINK          18  // Delete file
#define SYS_READDIR         19  // Read directory entry

// Memory management
#define SYS_BRK             20  // Change data segment size
#define SYS_MMAP            21  // Map memory
#define SYS_MUNMAP          22  // Unmap memory

// GUI syscalls (MayteraOS specific)
#define SYS_WIN_CREATE      30  // Create window
#define SYS_WIN_DESTROY     31  // Destroy window
#define SYS_WIN_DRAW_RECT   32  // Draw rectangle
#define SYS_WIN_DRAW_TEXT   33  // Draw text
#define SYS_WIN_DRAW_PIXEL  34  // Draw pixel
#define SYS_WIN_BLIT        35  // Blit bitmap
#define SYS_WIN_GET_EVENT   36  // Get window event
#define SYS_WIN_INVALIDATE  37  // Invalidate window (request redraw)

// Console I/O
#define SYS_PUTCHAR         40  // Write character
#define SYS_GETCHAR         41  // Read character

// Time
#define SYS_TIME            50  // Get current time
#define SYS_CLOCK           51  // Get system clock ticks

// Network (future)
#define SYS_SOCKET          60  // Create socket
#define SYS_CONNECT         61  // Connect socket
#define SYS_SEND            62  // Send data
#define SYS_RECV            63  // Receive data

// IPC - Shared Memory (Task #31)
#define SYS_SHM_CREATE      70  // Create shared memory region
#define SYS_SHM_MAP         71  // Map shared memory into address space
#define SYS_SHM_UNMAP       72  // Unmap shared memory
#define SYS_SHM_DESTROY     73  // Destroy shared memory (creator only)
#define SYS_SHM_INFO        74  // Get shared memory info

// IPC - Message Passing (Task #32)
#define SYS_MSG_CREATE      80  // Create message channel
#define SYS_MSG_CONNECT     81  // Connect to channel
#define SYS_MSG_SEND        82  // Send message
#define SYS_MSG_RECV        83  // Receive message
#define SYS_MSG_CLOSE       84  // Close connection
#define SYS_MSG_DESTROY     85  // Destroy channel (server only)
#define SYS_MSG_ACCEPT      86  // Accept connection (server)
#define SYS_MSG_INFO        87  // Get channel info

// Capability System (Task #37 - Security)
#define SYS_CAP_REQUEST     100 // Request capability
#define SYS_CAP_USE         101 // Use capability
#define SYS_CAP_RELEASE     102 // Release capability early
#define SYS_CAP_DELEGATE    103 // Delegate capability to another process
#define SYS_CAP_REVOKE      104 // Revoke capability
#define SYS_CAP_INFO        105 // Query capability information
#define SYS_CAP_CHECK       106 // Check if capability is valid

// Extended Attributes (xattr)
#define SYS_GETXATTR        107 // Get extended attribute
#define SYS_SETXATTR        108 // Set extended attribute
#define SYS_REMOVEXATTR     109 // Remove extended attribute
#define SYS_LISTXATTR       110 // List extended attributes

// Theme System (P14 - Theme Specialist)
#define SYS_THEME_GET_ACTIVE    120 // Get active theme index
#define SYS_THEME_GET_COLOR     121 // Get theme color by color ID
#define SYS_THEME_GET_COUNT     122 // Get number of available themes
#define SYS_THEME_SET_ACTIVE    123 // Set active theme (requires CAP_SYSTEM_APPEARANCE)
#define SYS_THEME_GET_NAME      124 // Get theme name by index
#define SYS_THEME_GET_METRIC    125 // Get theme metric by ID

// Input syscalls
#define SYS_GET_KEY         212 // Get keyboard input

// Maximum syscall number
#define SYS_MAX             130

// ============================================================================
// Syscall Register Convention (AMD64 System V ABI)
// ============================================================================
// Syscall number: RAX
// Arguments: RDI, RSI, RDX, R10, R8, R9
// Return value: RAX
// Clobbered by syscall: RCX, R11
// ============================================================================

// Syscall context (saved on kernel stack)
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;   // Clobbered by SYSCALL (contains RFLAGS)
    uint64_t r10;   // arg4
    uint64_t r9;    // arg5
    uint64_t r8;    // arg6
    uint64_t rbp;
    uint64_t rdi;   // arg1
    uint64_t rsi;   // arg2
    uint64_t rdx;   // arg3
    uint64_t rcx;   // Clobbered by SYSCALL (contains RIP)
    uint64_t rbx;
    uint64_t rax;   // syscall number / return value
} __attribute__((packed)) syscall_context_t;

// ============================================================================
// Syscall API
// ============================================================================

// Initialize syscall mechanism (set up MSRs)
// Assembly functions
extern void syscall_set_kernel_stack(uint64_t stack_ptr);

void syscall_init(void);

// Main syscall dispatcher (called from assembly)
int64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5,
                         uint64_t arg6);

// ============================================================================
// Individual syscall handlers
// ============================================================================

// Process
int64_t sys_exit(int exit_code);
int64_t sys_fork(void);
int64_t sys_getpid(void);
int64_t sys_getppid(void);
int64_t sys_yield(void);
int64_t sys_sleep(uint32_t ms);

// File I/O
int64_t sys_open(const char *path, int flags);
int64_t sys_close(int fd);
int64_t sys_read(int fd, void *buf, size_t count);
int64_t sys_write(int fd, const void *buf, size_t count);
int64_t sys_seek(int fd, int64_t offset, int whence);

// Memory
int64_t sys_brk(uint64_t addr);
int64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags);
int64_t sys_munmap(uint64_t addr, uint64_t len);

// Console
int64_t sys_putchar(int c);
int64_t sys_getchar(void);

// Time
int64_t sys_time(void);
int64_t sys_clock(void);

// Window/Graphics
int64_t sys_win_create(const char *title, int x, int y, int width, int height);
int64_t sys_win_destroy(int handle);
int64_t sys_win_draw_rect(int handle, int x, int y, int w, int h, uint32_t color);
int64_t sys_win_draw_text(int handle, int x, int y, const char *text, uint32_t color);
int64_t sys_win_draw_pixel(int handle, int x, int y, uint32_t color);
int64_t sys_win_get_event(int handle, void *event_buf, int timeout);
int64_t sys_win_invalidate(int handle);

// IPC - Shared Memory (declarations in ipc/shm.h)
// int64_t sys_shm_create(size_t size, int flags);
// int64_t sys_shm_map(int id, void **addr);
// int64_t sys_shm_unmap(int id);
// int64_t sys_shm_destroy(int id);
// int64_t sys_shm_info(int id, size_t *size, uint32_t *ref_count);

// IPC - Message Passing (declarations in ipc/msg.h)
// int64_t sys_msg_create_channel(void);
// int64_t sys_msg_connect(int channel_id);
// int64_t sys_msg_send(int conn_id, const void *data, size_t len);
// int64_t sys_msg_recv(int conn_id, void *buf, size_t len, int timeout);
// int64_t sys_msg_close(int conn_id);
// int64_t sys_msg_destroy_channel(int channel_id);
// int64_t sys_msg_accept(int channel_id, int timeout);
// int64_t sys_msg_channel_info(int channel_id, uint32_t *num_conns);

// Capability System (declarations in security/capability.h)
// int64_t sys_cap_request(const cap_request_t *req, cap_response_t *resp);
// int64_t sys_cap_use(uint64_t cap_id, int *handle, uint32_t permissions);
// int64_t sys_cap_release(uint64_t cap_id);
// int64_t sys_cap_delegate(uint64_t cap_id, uint64_t target_pid, uint64_t new_expiry, uint32_t new_perms);
// int64_t sys_cap_revoke(uint64_t cap_id);
// int64_t sys_cap_info(uint64_t cap_id, cap_info_t *info);
// int64_t sys_cap_check(uint64_t cap_id, uint32_t permissions);

// Extended Attributes (declarations in fs/xattr.h)
// int64_t sys_getxattr(const char *path, const char *name, void *value, size_t size);
// int64_t sys_setxattr(const char *path, const char *name, const void *value, size_t size, int flags);
// int64_t sys_removexattr(const char *path, const char *name);
// int64_t sys_listxattr(const char *path, char *list, size_t size);

// Theme System (declarations in gui/theme.h)
int64_t sys_theme_get_active(void);
int64_t sys_theme_get_color(uint64_t color_id);
int64_t sys_theme_get_count(void);
int64_t sys_theme_set_active(uint64_t theme_index);
int64_t sys_theme_get_name(uint64_t theme_index, char *buf, uint64_t buf_size);
int64_t sys_theme_get_metric(uint64_t metric_id);

#endif // SYSCALL_H
