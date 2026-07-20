// dos.h - DOS Emulation Layer Structures and API
// Provides DOS-compatible functionality for 64-bit native code
#ifndef DOS_H
#define DOS_H

#include "../types.h"

// DOS INT 21h function numbers
#define DOS_CHAR_INPUT_ECHO     0x01    // Character input with echo
#define DOS_CHAR_OUTPUT         0x02    // Character output
#define DOS_PRINT_STRING        0x09    // Print $ terminated string
#define DOS_CREATE_FILE         0x3C    // Create file
#define DOS_OPEN_FILE           0x3D    // Open file
#define DOS_CLOSE_FILE          0x3E    // Close file
#define DOS_READ_FILE           0x3F    // Read from file
#define DOS_WRITE_FILE          0x40    // Write to file
#define DOS_TERMINATE           0x4C    // Terminate with return code

// DOS file access modes (for open)
#define DOS_ACCESS_READ         0x00
#define DOS_ACCESS_WRITE        0x01
#define DOS_ACCESS_READWRITE    0x02

// DOS file attributes (for create)
#define DOS_ATTR_NORMAL         0x00
#define DOS_ATTR_READONLY       0x01
#define DOS_ATTR_HIDDEN         0x02
#define DOS_ATTR_SYSTEM         0x04
#define DOS_ATTR_ARCHIVE        0x20

// DOS error codes
#define DOS_ERR_NONE            0x00    // No error
#define DOS_ERR_INVALID_FUNC    0x01    // Invalid function
#define DOS_ERR_FILE_NOT_FOUND  0x02    // File not found
#define DOS_ERR_PATH_NOT_FOUND  0x03    // Path not found
#define DOS_ERR_TOO_MANY_FILES  0x04    // Too many open files
#define DOS_ERR_ACCESS_DENIED   0x05    // Access denied
#define DOS_ERR_INVALID_HANDLE  0x06    // Invalid handle
#define DOS_ERR_NO_MEMORY       0x08    // Insufficient memory
#define DOS_ERR_INVALID_ACCESS  0x0C    // Invalid access mode
#define DOS_ERR_INVALID_DRIVE   0x0F    // Invalid drive
#define DOS_ERR_WRITE_PROTECT   0x13    // Write protected

// Maximum open files per process
#define DOS_MAX_FILES           20

// Standard file handles
#define DOS_STDIN               0
#define DOS_STDOUT              1
#define DOS_STDERR              2
#define DOS_STDAUX              3       // COM1
#define DOS_STDPRN              4       // LPT1

// DOS register structure for INT 21h calls
// Simulates the x86 registers used in DOS calls
typedef struct {
    // General purpose registers (lower 16 bits)
    uint16_t ax;
    uint16_t bx;
    uint16_t cx;
    uint16_t dx;

    // Index registers
    uint16_t si;
    uint16_t di;

    // Segment registers (for address formation)
    uint16_t ds;
    uint16_t es;

    // Flags (bit 0 = carry flag for error indication)
    uint16_t flags;
} dos_regs_t;

// Macro to access AH (function number) from AX
#define DOS_AH(regs) (((regs)->ax >> 8) & 0xFF)
#define DOS_AL(regs) ((regs)->ax & 0xFF)
#define DOS_SET_AH(regs, val) ((regs)->ax = ((regs)->ax & 0x00FF) | ((val) << 8))
#define DOS_SET_AL(regs, val) ((regs)->ax = ((regs)->ax & 0xFF00) | ((val) & 0xFF))

// BH/BL access
#define DOS_BH(regs) (((regs)->bx >> 8) & 0xFF)
#define DOS_BL(regs) ((regs)->bx & 0xFF)
#define DOS_SET_BH(regs, val) ((regs)->bx = ((regs)->bx & 0x00FF) | ((val) << 8))
#define DOS_SET_BL(regs, val) ((regs)->bx = ((regs)->bx & 0xFF00) | ((val) & 0xFF))

// CH/CL access
#define DOS_CH(regs) (((regs)->cx >> 8) & 0xFF)
#define DOS_CL(regs) ((regs)->cx & 0xFF)

// DH/DL access
#define DOS_DH(regs) (((regs)->dx >> 8) & 0xFF)
#define DOS_DL(regs) ((regs)->dx & 0xFF)
#define DOS_SET_DL(regs, val) ((regs)->dx = ((regs)->dx & 0xFF00) | ((val) & 0xFF))

// Flags
#define DOS_FLAG_CARRY          0x0001
#define DOS_SET_CARRY(regs)     ((regs)->flags |= DOS_FLAG_CARRY)
#define DOS_CLEAR_CARRY(regs)   ((regs)->flags &= ~DOS_FLAG_CARRY)
#define DOS_HAS_CARRY(regs)     ((regs)->flags & DOS_FLAG_CARRY)

// DOS file handle structure
typedef struct {
    int      in_use;            // Handle is allocated
    int      device;            // 0=file, 1=console, 2=serial
    uint32_t position;          // Current file position
    uint32_t size;              // File size
    uint8_t  access_mode;       // Read/write/both
    char     path[256];         // File path
    void    *internal;          // Internal file handle (fat_file_t*)
} dos_file_handle_t;

// DOS Program Segment Prefix (PSP) - simplified
typedef struct {
    uint16_t int20_terminate;   // INT 20h instruction (CD 20)
    uint16_t mem_top_segment;   // Top of memory
    uint8_t  reserved1;
    uint8_t  dos_far_call[5];   // Far call to DOS
    uint32_t terminate_addr;    // Terminate address
    uint32_t ctrl_break_addr;   // Ctrl-Break handler
    uint32_t crit_error_addr;   // Critical error handler
    uint16_t parent_psp;        // Parent PSP segment
    uint8_t  file_table[20];    // Job File Table
    uint16_t environment_seg;   // Environment segment
    uint32_t ss_sp;             // SS:SP on entry
    uint16_t max_open_files;    // Max open files
    uint32_t file_table_ptr;    // Pointer to file table
    uint32_t prev_psp;          // Previous PSP
    uint8_t  reserved2[20];
    uint8_t  dos_dispatch[3];   // INT 21h, RETF
    uint8_t  reserved3[9];
    uint8_t  fcb1[16];          // First FCB
    uint8_t  fcb2[20];          // Second FCB
    uint8_t  cmd_len;           // Command line length
    char     cmd_line[127];     // Command line
} __attribute__((packed)) dos_psp_t;

// DOS Memory Control Block (MCB)
typedef struct {
    uint8_t  signature;         // 'M' or 'Z' (last block)
    uint16_t owner_psp;         // Owning PSP segment (0=free)
    uint16_t size_paragraphs;   // Size in 16-byte paragraphs
    uint8_t  reserved[3];
    uint8_t  name[8];           // Program name (DOS 4.0+)
} __attribute__((packed)) dos_mcb_t;

// DOS emulator state
typedef struct {
    int      initialized;
    uint8_t  return_code;       // Program return code
    int      terminated;        // Program has terminated

    // File handles
    dos_file_handle_t handles[DOS_MAX_FILES];

    // Current PSP
    dos_psp_t *current_psp;

    // Memory management
    void    *dos_memory_base;   // Base of DOS memory pool
    size_t   dos_memory_size;   // Size of DOS memory pool

    // Environment
    char    *environment;       // Environment strings
    size_t   env_size;

    // Current drive and directory
    uint8_t  current_drive;     // 0=A, 1=B, 2=C, etc.
    char     current_dir[256];  // Current directory

    // DTA (Disk Transfer Address)
    void    *dta;
} dos_state_t;

// Initialize DOS emulation layer
void dos_init(void);

// Main DOS call interface - simulate INT 21h
// Returns 0 on success, error code on failure
int dos_call(uint8_t function, dos_regs_t *regs);

// Alternative interface with direct function call
// These can be called directly without simulating registers

// Console I/O
char dos_getchar(void);         // Read character with echo
void dos_putchar(char c);       // Output character
void dos_print_string(const char *str);  // Print $ terminated string

// File I/O
int dos_create(const char *path, uint16_t attr);  // Returns handle or -1
int dos_open(const char *path, uint8_t mode);     // Returns handle or -1
int dos_close(int handle);                        // Returns 0 or error
int dos_read(int handle, void *buf, uint16_t len, uint16_t *bytes_read);
int dos_write(int handle, const void *buf, uint16_t len, uint16_t *bytes_written);

// Memory management (DOS style)
void *dos_alloc(uint16_t paragraphs);   // Allocate memory (16-byte units)
int dos_free(void *block);              // Free memory block
int dos_resize(void *block, uint16_t new_paragraphs);  // Resize block

// Program termination
void dos_terminate(uint8_t return_code);

// Get DOS state (for debugging)
dos_state_t *dos_get_state(void);

// Get last error code
uint16_t dos_get_error(void);

// Helper to form segment:offset address from pointer
static inline uint32_t dos_ptr_to_segofs(void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    // Simple mapping: segment = high 16 bits, offset = low 16 bits of lower 20 bits
    // This is a simplified mapping since we're in 64-bit mode
    uint16_t segment = (uint16_t)((addr >> 4) & 0xFFFF);
    uint16_t offset = (uint16_t)(addr & 0x000F);
    return ((uint32_t)segment << 16) | offset;
}

// Helper to form pointer from segment:offset
static inline void *dos_segofs_to_ptr(uint16_t segment, uint16_t offset) {
    // In our emulation, we use a flat memory model
    // segment:offset = segment * 16 + offset
    return (void *)(uintptr_t)((segment << 4) + offset);
}

#endif // DOS_H
