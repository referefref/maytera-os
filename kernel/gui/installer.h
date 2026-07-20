// installer.h - MayteraOS Installer GUI Application
#ifndef INSTALLER_H
#define INSTALLER_H

#include "../types.h"
#include "window.h"

// Installer constants
#define INSTALLER_WINDOW_WIDTH   650
#define INSTALLER_WINDOW_HEIGHT  520
#define INSTALLER_MAX_DISKS      16
#define INSTALLER_MAX_PARTITIONS 32
#define INSTALLER_FULLNAME_MAX   64
#define INSTALLER_USERNAME_MAX   32
#define INSTALLER_PASSWORD_MAX   64
#define INSTALLER_HOSTNAME_MAX   64

// Installer colors - Modern theme
#define INSTALLER_BG_COLOR       0x00F5F5F5  // Light grey background
#define INSTALLER_HEADER_COLOR   0x002E5A88  // Blue header
#define INSTALLER_ACCENT_COLOR   0x003498DB  // Accent blue
#define INSTALLER_TEXT_COLOR     0x00202020  // Dark text
#define INSTALLER_BTN_PRIMARY    0x003498DB  // Primary button blue
#define INSTALLER_BTN_SECONDARY  0x00808080  // Secondary button grey
#define INSTALLER_BTN_DANGER     0x00E74C3C  // Danger red
#define INSTALLER_PROGRESS_BG    0x00D0D0D0  // Progress bar background
#define INSTALLER_PROGRESS_FG    0x0027AE60  // Progress bar fill (green)
#define INSTALLER_DISK_BG        0x00FFFFFF  // Disk list item background
#define INSTALLER_DISK_SELECTED  0x00E3F2FD  // Selected disk highlight

// Installation steps
typedef enum {
    INSTALL_STEP_WELCOME = 0,
    INSTALL_STEP_DISK_SELECT,
    INSTALL_STEP_PARTITION,
    INSTALL_STEP_USER_SETUP,
    INSTALL_STEP_TIMEZONE,
    INSTALL_STEP_INSTALL,
    INSTALL_STEP_COMPLETE,
    INSTALL_STEP_COUNT
} install_step_t;

// Disk information
typedef struct {
    char name[32];           // Device name (e.g., "sda", "nvme0n1")
    char model[64];          // Disk model name
    uint64_t size_bytes;     // Total size in bytes
    uint32_t sector_size;    // Sector size (usually 512 or 4096)
    bool removable;          // Is removable media
    bool selected;           // Is currently selected
    int  drive_id;           // ATA drive id (channel*2 + unit); used by the installer engine
} installer_disk_t;

// Partition information
typedef struct {
    uint32_t disk_index;     // Parent disk index
    uint32_t part_number;    // Partition number
    uint64_t start_lba;      // Start LBA
    uint64_t size_bytes;     // Size in bytes
    uint8_t type;            // Partition type (0x00=free, 0x0B=FAT32, 0x83=Linux, etc.)
    char label[32];          // Partition label
    bool bootable;           // Is bootable
    bool selected;           // Is selected for installation
} installer_partition_t;

// User account information
typedef struct {
    char fullname[INSTALLER_FULLNAME_MAX];
    char username[INSTALLER_USERNAME_MAX];
    char password[INSTALLER_PASSWORD_MAX];
    char password_confirm[INSTALLER_PASSWORD_MAX];
    char hostname[INSTALLER_HOSTNAME_MAX];
    bool auto_login;
} installer_user_t;

// Locale settings
typedef struct {
    int timezone_index;      // Index into timezone list
    int keyboard_index;      // Index into keyboard layout list
    int language_index;      // Index into language list
} installer_locale_t;

// Installation progress
typedef struct {
    uint32_t total_files;
    uint32_t copied_files;
    uint64_t total_bytes;
    uint64_t copied_bytes;
    char current_file[128];
    int percent;
    bool complete;
    bool error;
    char error_msg[256];
} install_progress_t;

// Installer state
typedef struct {
    window_t *window;
    install_step_t current_step;

    // Disk information
    installer_disk_t disks[INSTALLER_MAX_DISKS];
    uint32_t disk_count;
    int selected_disk;

    // Partition information
    installer_partition_t partitions[INSTALLER_MAX_PARTITIONS];
    uint32_t partition_count;
    int selected_partition;
    int partition_mode;      // 0=auto, 1=manual

    // User setup
    installer_user_t user;

    // Locale settings
    installer_locale_t locale;

    // Installation progress
    install_progress_t progress;

    // UI state
    int hover_disk;
    int hover_button;
    int focused_input;       // 0=fullname, 1=username, 2=password, 3=confirm, 4=hostname
    int focused_dropdown;    // For timezone step
    bool hover_checkbox;
    bool installing;
    int  app_id;             // window-manager app registration id
} installer_t;

// ============================================================================
// Installer API
// ============================================================================

// Initialize and show the installer
void installer_init(void);

// Run the installer (main entry point)
void installer_run(void);

// ============================================================================
// Install engine (headless core, shared by the GUI and the RC "osinstall" cmd)
// ============================================================================
//
// Partition (GPT + protective MBR, single EF00 ESP at LBA 2048), then raw-clone
// the live/source FAT ESP onto the target so the target becomes a byte-identical
// bootable MayteraOS disk. target_drive_id is an ATA drive id (channel*2 + unit);
// it MUST differ from the booted/source disk. The optional progress callback is
// invoked with a percentage (0..100) and a message; percent==0 with a message
// starting "ERROR" signals failure. Returns 0 on success, negative on error.
typedef void (*installer_progress_fn)(void *ctx, int percent, const char *msg);
int installer_do_install(int target_drive_id, installer_progress_fn cb, void *ctx);

// Deferred headless auto-install (gated on /CONFIG/AUTOINST.CFG); logs to serial.
void installer_start_deferred_autoinstall(void);

// Close the installer
void installer_close(void);

// Check if installer is running
bool installer_is_running(void);

// ============================================================================
// Internal functions (called by installer)
// ============================================================================

// Step drawing functions
void installer_draw_welcome(installer_t *inst);
void installer_draw_disk_select(installer_t *inst);
void installer_draw_partition(installer_t *inst);
void installer_draw_user_setup(installer_t *inst);
void installer_draw_timezone(installer_t *inst);
void installer_draw_install(installer_t *inst);
void installer_draw_complete(installer_t *inst);

// Navigation
void installer_next_step(installer_t *inst);
void installer_prev_step(installer_t *inst);

// Disk detection
void installer_detect_disks(installer_t *inst);
void installer_detect_partitions(installer_t *inst, int disk_index);

// Partition management
bool installer_create_partition(installer_t *inst, uint64_t size_mb, uint8_t type);
bool installer_delete_partition(installer_t *inst, int part_index);
bool installer_format_partition(installer_t *inst, int part_index, const char *fs_type);

// Installation
void installer_start_install(installer_t *inst);
bool installer_copy_files(installer_t *inst);
bool installer_install_bootloader(installer_t *inst);
bool installer_configure_system(installer_t *inst);
bool installer_create_user(installer_t *inst);

// Event handlers
void installer_handle_mouse_move(installer_t *inst, int32_t x, int32_t y);
void installer_handle_mouse_down(installer_t *inst, int32_t x, int32_t y, uint32_t button);
void installer_handle_key(installer_t *inst, uint32_t keycode, char key_char);

#endif // INSTALLER_H
