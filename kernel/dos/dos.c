// dos.c - DOS Emulator Implementation
// Provides DOS-compatible syscall interface for native 64-bit code
#include "dos.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../fs/fat.h"

// External INT 21h handler
extern int int21_dispatch(uint8_t function, dos_regs_t *regs);

// Global DOS emulator state
static dos_state_t dos_state;

// Last error code
static uint16_t dos_last_error = DOS_ERR_NONE;

// External FAT filesystem (should be mounted by kernel)
extern fat_fs_t *g_dos_fat_fs;

// Weak reference - will be NULL if not defined elsewhere
fat_fs_t *g_dos_fat_fs __attribute__((weak)) = NULL;

// Console input buffer (reserved for future use)
static char console_input_buffer[256] __attribute__((unused));
static int console_input_pos __attribute__((unused)) = 0;
static int console_input_len __attribute__((unused)) = 0;

// Initialize DOS emulation layer
void dos_init(void) {
    if (dos_state.initialized) {
        return;
    }

    kprintf("DOS: Initializing DOS emulation layer...\n");

    // Clear state
    memset(&dos_state, 0, sizeof(dos_state_t));

    // Initialize standard file handles
    // Handle 0: stdin (console input)
    dos_state.handles[DOS_STDIN].in_use = 1;
    dos_state.handles[DOS_STDIN].device = 1;  // Console
    strcpy(dos_state.handles[DOS_STDIN].path, "CON");

    // Handle 1: stdout (console output)
    dos_state.handles[DOS_STDOUT].in_use = 1;
    dos_state.handles[DOS_STDOUT].device = 1;  // Console
    strcpy(dos_state.handles[DOS_STDOUT].path, "CON");

    // Handle 2: stderr (console output)
    dos_state.handles[DOS_STDERR].in_use = 1;
    dos_state.handles[DOS_STDERR].device = 1;  // Console
    strcpy(dos_state.handles[DOS_STDERR].path, "CON");

    // Handle 3: stdaux (serial)
    dos_state.handles[DOS_STDAUX].in_use = 1;
    dos_state.handles[DOS_STDAUX].device = 2;  // Serial
    strcpy(dos_state.handles[DOS_STDAUX].path, "AUX");

    // Handle 4: stdprn (printer - mapped to serial for now)
    dos_state.handles[DOS_STDPRN].in_use = 1;
    dos_state.handles[DOS_STDPRN].device = 2;  // Serial
    strcpy(dos_state.handles[DOS_STDPRN].path, "PRN");

    // Allocate DOS memory pool (64KB conventional memory simulation)
    dos_state.dos_memory_base = kmalloc(64 * 1024);
    if (dos_state.dos_memory_base) {
        dos_state.dos_memory_size = 64 * 1024;
        memset(dos_state.dos_memory_base, 0, dos_state.dos_memory_size);
    }

    // Set up environment
    dos_state.environment = (char *)kmalloc(4096);
    if (dos_state.environment) {
        dos_state.env_size = 4096;
        // Set up default environment strings
        // Format: VAR=VALUE\0VAR=VALUE\0\0
        char *env = dos_state.environment;
        strcpy(env, "PATH=C:\\");
        env += strlen(env) + 1;
        strcpy(env, "COMSPEC=C:\\COMMAND.COM");
        env += strlen(env) + 1;
        *env = '\0';  // Double null terminator
    }

    // Set default drive and directory
    dos_state.current_drive = 2;  // C:
    strcpy(dos_state.current_dir, "\\");

    // Allocate DTA (Disk Transfer Address) - 128 bytes
    dos_state.dta = kmalloc(128);
    if (dos_state.dta) {
        memset(dos_state.dta, 0, 128);
    }

    dos_state.initialized = 1;
    dos_state.terminated = 0;
    dos_state.return_code = 0;

    kprintf("DOS: Emulation layer initialized\n");
    kprintf("DOS: Memory pool: %llu KB\n", (uint64_t)(dos_state.dos_memory_size / 1024));
}

// Get DOS state
dos_state_t *dos_get_state(void) {
    return &dos_state;
}

// Get last error
uint16_t dos_get_error(void) {
    return dos_last_error;
}

// Set error code
static void dos_set_error(uint16_t error) {
    dos_last_error = error;
}

// Find free file handle
static int dos_find_free_handle(void) {
    for (int i = 5; i < DOS_MAX_FILES; i++) {
        if (!dos_state.handles[i].in_use) {
            return i;
        }
    }
    return -1;
}

// Validate file handle
static int dos_valid_handle(int handle) {
    if (handle < 0 || handle >= DOS_MAX_FILES) {
        return 0;
    }
    return dos_state.handles[handle].in_use;
}

// Console character input with echo
char dos_getchar(void) {
    // For now, use serial input as console
    // In a full implementation, this would check keyboard buffer
    char c = serial_read(COM1);
    dos_putchar(c);  // Echo
    return c;
}

// Console character output
void dos_putchar(char c) {
    // Output to both VGA console and serial
    serial_write(COM1, c);
    // VGA output would go here in full implementation
}

// Print $ terminated string
void dos_print_string(const char *str) {
    while (*str && *str != '$') {
        dos_putchar(*str++);
    }
}

// Create a file
int dos_create(const char *path, uint16_t attr) {
    UNUSED(attr);

    int handle = dos_find_free_handle();
    if (handle < 0) {
        dos_set_error(DOS_ERR_TOO_MANY_FILES);
        return -1;
    }

    // FAT write support not implemented - simulate success for now
    // In a full implementation, this would call fat_create()
    kprintf("DOS: Create file '%s' (write not fully implemented)\n", path);

    dos_file_handle_t *h = &dos_state.handles[handle];
    h->in_use = 1;
    h->device = 0;  // File
    h->position = 0;
    h->size = 0;
    h->access_mode = DOS_ACCESS_WRITE;
    strncpy(h->path, path, sizeof(h->path) - 1);
    h->path[sizeof(h->path) - 1] = '\0';
    h->internal = NULL;

    dos_set_error(DOS_ERR_NONE);
    return handle;
}

// Open a file
int dos_open(const char *path, uint8_t mode) {
    int handle = dos_find_free_handle();
    if (handle < 0) {
        dos_set_error(DOS_ERR_TOO_MANY_FILES);
        return -1;
    }

    dos_file_handle_t *h = &dos_state.handles[handle];

    // Check if FAT filesystem is available
    if (g_dos_fat_fs && g_dos_fat_fs->mounted) {
        // Allocate FAT file structure
        fat_file_t *fat_file = (fat_file_t *)kmalloc(sizeof(fat_file_t));
        if (!fat_file) {
            dos_set_error(DOS_ERR_NO_MEMORY);
            return -1;
        }

        // Skip drive letter if present (C:\path -> \path)
        const char *fat_path = path;
        if (path[0] && path[1] == ':') {
            fat_path = path + 2;
        }

        // Try to open the file
        if (fat_open(g_dos_fat_fs, fat_path, fat_file) != 0) {
            kfree(fat_file);
            dos_set_error(DOS_ERR_FILE_NOT_FOUND);
            return -1;
        }

        h->in_use = 1;
        h->device = 0;  // File
        h->position = 0;
        h->size = fat_file->file_size;
        h->access_mode = mode;
        strncpy(h->path, path, sizeof(h->path) - 1);
        h->path[sizeof(h->path) - 1] = '\0';
        h->internal = fat_file;

        dos_set_error(DOS_ERR_NONE);
        return handle;
    }

    // No filesystem available
    dos_set_error(DOS_ERR_FILE_NOT_FOUND);
    return -1;
}

// Close a file
int dos_close(int handle) {
    if (!dos_valid_handle(handle)) {
        dos_set_error(DOS_ERR_INVALID_HANDLE);
        return DOS_ERR_INVALID_HANDLE;
    }

    // Don't close standard handles
    if (handle < 5) {
        dos_set_error(DOS_ERR_NONE);
        return 0;
    }

    dos_file_handle_t *h = &dos_state.handles[handle];

    // Close FAT file if present
    if (h->internal && h->device == 0) {
        fat_file_t *fat_file = (fat_file_t *)h->internal;
        fat_close(fat_file);
        kfree(fat_file);
    }

    // Clear handle
    memset(h, 0, sizeof(dos_file_handle_t));

    dos_set_error(DOS_ERR_NONE);
    return 0;
}

// Read from file
int dos_read(int handle, void *buf, uint16_t len, uint16_t *bytes_read) {
    if (!dos_valid_handle(handle)) {
        dos_set_error(DOS_ERR_INVALID_HANDLE);
        return DOS_ERR_INVALID_HANDLE;
    }

    dos_file_handle_t *h = &dos_state.handles[handle];
    *bytes_read = 0;

    // Handle console input (stdin)
    if (h->device == 1 && handle == DOS_STDIN) {
        // Read from serial as console
        uint16_t count = 0;
        char *cbuf = (char *)buf;
        while (count < len) {
            if (serial_received(COM1)) {
                char c = serial_read(COM1);
                cbuf[count++] = c;
                dos_putchar(c);  // Echo
                if (c == '\r' || c == '\n') {
                    break;
                }
            }
        }
        *bytes_read = count;
        dos_set_error(DOS_ERR_NONE);
        return 0;
    }

    // Handle serial (stdaux)
    if (h->device == 2) {
        uint16_t count = 0;
        char *cbuf = (char *)buf;
        while (count < len && serial_received(COM1)) {
            cbuf[count++] = serial_read(COM1);
        }
        *bytes_read = count;
        dos_set_error(DOS_ERR_NONE);
        return 0;
    }

    // Handle file
    if (h->device == 0 && h->internal) {
        fat_file_t *fat_file = (fat_file_t *)h->internal;

        // Seek to current position if needed
        if (fat_file->position != h->position) {
            fat_seek(fat_file, h->position);
        }

        int result = fat_read(fat_file, buf, len);
        if (result >= 0) {
            *bytes_read = (uint16_t)result;
            h->position += result;
            dos_set_error(DOS_ERR_NONE);
            return 0;
        }

        dos_set_error(DOS_ERR_ACCESS_DENIED);
        return DOS_ERR_ACCESS_DENIED;
    }

    dos_set_error(DOS_ERR_INVALID_HANDLE);
    return DOS_ERR_INVALID_HANDLE;
}

// Write to file
int dos_write(int handle, const void *buf, uint16_t len, uint16_t *bytes_written) {
    if (!dos_valid_handle(handle)) {
        dos_set_error(DOS_ERR_INVALID_HANDLE);
        return DOS_ERR_INVALID_HANDLE;
    }

    dos_file_handle_t *h = &dos_state.handles[handle];
    *bytes_written = 0;

    // Handle console output (stdout/stderr)
    if (h->device == 1 && (handle == DOS_STDOUT || handle == DOS_STDERR)) {
        const char *cbuf = (const char *)buf;
        for (uint16_t i = 0; i < len; i++) {
            dos_putchar(cbuf[i]);
        }
        *bytes_written = len;
        dos_set_error(DOS_ERR_NONE);
        return 0;
    }

    // Handle serial (stdaux/stdprn)
    if (h->device == 2) {
        const char *cbuf = (const char *)buf;
        for (uint16_t i = 0; i < len; i++) {
            serial_write(COM1, cbuf[i]);
        }
        *bytes_written = len;
        dos_set_error(DOS_ERR_NONE);
        return 0;
    }

    // Handle file write - not fully implemented
    if (h->device == 0) {
        // FAT write not implemented - just track position
        kprintf("DOS: Write to file '%s' not implemented (len=%u)\n", h->path, len);
        *bytes_written = len;  // Pretend success
        h->position += len;
        if (h->position > h->size) {
            h->size = h->position;
        }
        dos_set_error(DOS_ERR_NONE);
        return 0;
    }

    dos_set_error(DOS_ERR_INVALID_HANDLE);
    return DOS_ERR_INVALID_HANDLE;
}

// DOS memory allocation (in 16-byte paragraphs)
void *dos_alloc(uint16_t paragraphs) {
    size_t size = (size_t)paragraphs * 16;

    void *ptr = kmalloc(size);
    if (!ptr) {
        dos_set_error(DOS_ERR_NO_MEMORY);
        return NULL;
    }

    memset(ptr, 0, size);
    dos_set_error(DOS_ERR_NONE);
    return ptr;
}

// DOS memory free
int dos_free(void *block) {
    if (!block) {
        dos_set_error(DOS_ERR_INVALID_ACCESS);
        return DOS_ERR_INVALID_ACCESS;
    }

    kfree(block);
    dos_set_error(DOS_ERR_NONE);
    return 0;
}

// DOS memory resize
int dos_resize(void *block, uint16_t new_paragraphs) {
    if (!block) {
        dos_set_error(DOS_ERR_INVALID_ACCESS);
        return DOS_ERR_INVALID_ACCESS;
    }

    size_t new_size = (size_t)new_paragraphs * 16;
    void *new_ptr = krealloc(block, new_size);

    if (!new_ptr) {
        dos_set_error(DOS_ERR_NO_MEMORY);
        return DOS_ERR_NO_MEMORY;
    }

    dos_set_error(DOS_ERR_NONE);
    return 0;
}

// Program termination
void dos_terminate(uint8_t return_code) {
    kprintf("DOS: Program terminated with code %u\n", return_code);

    dos_state.return_code = return_code;
    dos_state.terminated = 1;

    // Close all file handles except standard ones
    for (int i = 5; i < DOS_MAX_FILES; i++) {
        if (dos_state.handles[i].in_use) {
            dos_close(i);
        }
    }
}

// Main DOS call interface - simulate INT 21h
int dos_call(uint8_t function, dos_regs_t *regs) {
    if (!dos_state.initialized) {
        dos_init();
    }

    // Clear carry flag (no error)
    DOS_CLEAR_CARRY(regs);

    // Dispatch to INT 21h handler
    return int21_dispatch(function, regs);
}
