// i_maytera.c - MayteraOS platform implementation for DOOM
// Provides file I/O, system functions, and launch support

#include "i_maytera.h"
#include "d_main.h"
#include "m_argv.h"

// File handle table
doom_file_t doom_file_table[DOOM_MAX_FILES];
static int doom_file_init_done = 0;

// Initialize file table
static void doom_file_init(void) {
    if (doom_file_init_done) return;
    memset(doom_file_table, 0, sizeof(doom_file_table));
    doom_file_init_done = 1;
}

// Open a file (read-only, loads entire file into memory)
int doom_open(const char *path, int flags) {
    (void)flags;  // We only support read-only

    doom_file_init();

    // Find free slot
    int fd = -1;
    for (int i = 0; i < DOOM_MAX_FILES; i++) {
        if (!doom_file_table[i].valid) {
            fd = i;
            break;
        }
    }

    if (fd < 0) {
        kprintf("[DOOM] doom_open: no free file slots\n");
        return -1;
    }

    doom_file_t *df = &doom_file_table[fd];

    // Convert path to uppercase for FAT
    char fat_path[256];
    int j = 0;
    for (int i = 0; path[i] && j < 255; i++) {
        char c = path[i];
        // Convert forward slash to path format
        if (c == '\\') c = '/';
        // Uppercase for FAT
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        fat_path[j++] = c;
    }
    fat_path[j] = '\0';

    // Prepend / if not present
    char final_path[260];
    if (fat_path[0] != '/') {
        final_path[0] = '/';
        strncpy(final_path + 1, fat_path, 258);
        final_path[259] = '\0';
    } else {
        strncpy(final_path, fat_path, 259);
        final_path[259] = '\0';
    }

    kprintf("[DOOM] Opening file: %s\n", final_path);

    // Read entire file using FAT
    uint32_t size = 0;
    df->buffer = (uint8_t *)fat_read_file(&g_fat_fs, final_path, &size);

    if (!df->buffer || size == 0) {
        kprintf("[DOOM] doom_open: failed to read %s\n", final_path);
        return -1;
    }

    df->size = size;
    df->pos = 0;
    df->valid = 1;

    kprintf("[DOOM] Opened file: %s (%u bytes, fd=%d)\n", final_path, size, fd);
    return fd;
}

// Close a file
int doom_close(int fd) {
    if (fd < 0 || fd >= DOOM_MAX_FILES) return -1;

    doom_file_t *df = &doom_file_table[fd];
    if (!df->valid) return -1;

    if (df->buffer) {
        kfree(df->buffer);
        df->buffer = NULL;
    }

    df->valid = 0;
    df->size = 0;
    df->pos = 0;

    return 0;
}

// Read from a file
int doom_read(int fd, void *buf, int count) {
    if (fd < 0 || fd >= DOOM_MAX_FILES) return -1;

    doom_file_t *df = &doom_file_table[fd];
    if (!df->valid || !df->buffer) return -1;

    // Clamp to available data
    int available = df->size - df->pos;
    if (count > available) count = available;
    if (count <= 0) return 0;

    memcpy(buf, df->buffer + df->pos, count);
    df->pos += count;

    return count;
}

// Seek in a file
int doom_lseek(int fd, int offset, int whence) {
    if (fd < 0 || fd >= DOOM_MAX_FILES) return -1;

    doom_file_t *df = &doom_file_table[fd];
    if (!df->valid) return -1;

    int new_pos;
    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = df->pos + offset;
            break;
        case SEEK_END:
            new_pos = df->size + offset;
            break;
        default:
            return -1;
    }

    if (new_pos < 0) new_pos = 0;
    if (new_pos > (int)df->size) new_pos = df->size;

    df->pos = new_pos;
    return new_pos;
}

// Get file length
int doom_filelength(int fd) {
    if (fd < 0 || fd >= DOOM_MAX_FILES) return -1;

    doom_file_t *df = &doom_file_table[fd];
    if (!df->valid) return -1;

    return df->size;
}

// ============================================================================
// DOOM Launch Function
// ============================================================================

// Forward declarations
extern void D_DoomMain(void);
extern window_t *doom_window;
extern int doom_running;

// Event/draw handlers for window manager
static void doom_handle_event(void *app_data, gui_event_t *event);
static void doom_draw(void *app_data, int x, int y, int w, int h);
static void doom_destroy(void *app_data);

// Global state
static int doom_dock_index = -1;

void doom_launch(void) {
    if (doom_running) {
        kprintf("[DOOM] Already running, ignoring launch\n");
        return;
    }
    kprintf("\n\n========== DOOM LAUNCH ==========\n");
    kprintf("[DOOM] doom_launch() called\n");
    LOG_INFO("[DOOM] Launching DOOM");
    kprintf("[DOOM] Starting DOOM...\n");

    // Set up command line arguments (empty for now)
    kprintf("[DOOM] Setting up argv...\n");
    static char *doom_argv[] = { "doom", NULL };
    myargc = 1;
    myargv = doom_argv;
    kprintf("[DOOM] argv set: myargc=%d\n", myargc);

    // Mark as running
    doom_running = 1;
    kprintf("[DOOM] doom_running set to 1\n");

    // Initialize MayteraOS-specific stuff
    kprintf("[DOOM] Calling I_MayteraInit()...\n");
    I_MayteraInit();
    kprintf("[DOOM] I_MayteraInit() returned\n");

    // Start DOOM main
    kprintf("[DOOM] About to call D_DoomMain()...\n");
    D_DoomMain();
    kprintf("[DOOM] D_DoomMain() returned\n");

    // When D_DoomMain returns (or doom_running becomes 0), cleanup
    kprintf("[DOOM] DOOM has exited\n");
    LOG_INFO("[DOOM] DOOM exited");
}

// ============================================================================
// MayteraOS Platform Functions
// ============================================================================

void I_MayteraInit(void) {
    kprintf("[DOOM] I_MayteraInit\n");
    doom_file_init();
}

void I_MayteraShutdown(void) {
    kprintf("[DOOM] I_MayteraShutdown\n");

    // Close any open files
    for (int i = 0; i < DOOM_MAX_FILES; i++) {
        if (doom_file_table[i].valid) {
            doom_close(i);
        }
    }

    doom_running = 0;
}

void I_MayteraPumpEvents(void) {
    // Event pumping handled by I_StartTic in i_video_maytera.c
}
