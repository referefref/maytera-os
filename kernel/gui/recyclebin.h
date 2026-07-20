// recyclebin.h - Recycle Bin for MayteraOS
#ifndef RECYCLEBIN_H
#define RECYCLEBIN_H

#include "window.h"

// Maximum items in recycle bin
#define RECYCLE_MAX_ITEMS 64

// Maximum path length
#define RECYCLE_PATH_MAX 256

// Recycle bin item
typedef struct {
    char original_path[RECYCLE_PATH_MAX];   // Where file came from
    char deleted_name[64];                   // Original filename
    uint32_t file_size;                      // File size in bytes
    uint64_t deleted_time;                   // When it was deleted (timer_ticks)
    bool is_directory;                       // Is this a directory?
    bool valid;                              // Is this entry valid?
} recycle_item_t;

// Recycle bin window state
typedef struct {
    window_t *window;                        // Window pointer

    recycle_item_t items[RECYCLE_MAX_ITEMS]; // Deleted items
    int item_count;                          // Number of items

    int selected;                            // Selected item index
    int scroll_offset;                       // Scroll position

    uint64_t last_update;                    // Last refresh time
} recyclebin_t;

// Create recycle bin window
recyclebin_t *recyclebin_create(void);

// Destroy recycle bin
void recyclebin_destroy(recyclebin_t *rb);

// Add item to recycle bin (called when file is deleted)
bool recyclebin_add(const char *path, bool is_dir);

// Restore selected item to original location
bool recyclebin_restore(recyclebin_t *rb);

// Permanently delete selected item
bool recyclebin_delete_permanent(recyclebin_t *rb);

// Empty the entire recycle bin
void recyclebin_empty(recyclebin_t *rb);

// Get item count
int recyclebin_get_count(void);

// Check if recycle bin is empty
bool recyclebin_is_empty(void);

// Event handling
void recyclebin_handle_event(recyclebin_t *rb, gui_event_t *event);

// Drawing
void recyclebin_draw(recyclebin_t *rb);

// Launch recycle bin window
void recyclebin_launch(void);

#endif // RECYCLEBIN_H
