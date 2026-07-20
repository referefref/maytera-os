// filebrowser_patch.c - Patch instructions for filebrowser.c integration
//
// This file documents the changes needed to integrate file associations
// and "Open With" functionality into filebrowser.c
//
// Changes required:
//
// 1. Add includes at the top of filebrowser.c:
//    #include "../apps/mime.h"
//    #include "../apps/associations.h"
//    #include "../apps/open_with_dialog.h"
//
// 2. Add new menu action function after menu_action_open:
//
//    static void menu_action_open_with(filebrowser_t *fb) {
//        if (fb->selected_index < 0 || fb->selected_index >= fb->entry_count) return;
//        
//        fb_entry_t *entry = &fb->entries[fb->selected_index];
//        if (entry->is_dir) return;  // No "Open With" for directories
//        
//        // Build full path
//        char filepath[FB_MAX_PATH];
//        strcpy(filepath, fb->current_path);
//        int len = strlen(filepath);
//        if (len > 0 && filepath[len - 1] != '/') {
//            strcat(filepath, "/");
//        }
//        strcat(filepath, entry->name);
//        
//        // Show "Open With" dialog
//        assoc_show_open_with_dialog(filepath, NULL);
//    }
//
// 3. Modify menu_action_open to open files with default app:
//
//    static void menu_action_open(filebrowser_t *fb) {
//        if (fb->selected_index >= 0) {
//            fb_entry_t *entry = &fb->entries[fb->selected_index];
//            if (entry->is_dir) {
//                // Navigate to directory
//                char new_path[FB_MAX_PATH];
//                if (strcmp(entry->name, "..") == 0) {
//                    fb_go_up_directory(fb);
//                } else {
//                    strcpy(new_path, fb->current_path);
//                    int len = strlen(new_path);
//                    if (len > 0 && new_path[len - 1] != '/') {
//                        strcat(new_path, "/");
//                    }
//                    strcat(new_path, entry->name);
//                    filebrowser_navigate(fb, new_path);
//                }
//            } else {
//                // Open file with default application
//                char filepath[FB_MAX_PATH];
//                strcpy(filepath, fb->current_path);
//                int len = strlen(filepath);
//                if (len > 0 && filepath[len - 1] != '/') {
//                    strcat(filepath, "/");
//                }
//                strcat(filepath, entry->name);
//                
//                // Try to open with registered default app
//                if (!assoc_open_file(filepath)) {
//                    kprintf("[FileBrowser] No app registered for: %s\n", filepath);
//                }
//            }
//        }
//    }
//
// 4. Add "Open With" menu item in fb_init_context_menu after "Open":
//
//    fb->menu_items[idx].label = "Open";
//    fb->menu_items[idx].action = menu_action_open;
//    fb->menu_items[idx].enabled = true;
//    fb->menu_items[idx].separator_after = false;  // Changed to false
//    idx++;
//
//    fb->menu_items[idx].label = "Open With...";
//    fb->menu_items[idx].action = menu_action_open_with;
//    fb->menu_items[idx].enabled = true;
//    fb->menu_items[idx].separator_after = true;
//    idx++;
//
// 5. Update fb_show_context_menu to handle "Open With" state:
//
//    // Open - enabled if something is selected
//    fb->menu_items[0].enabled = has_selection;
//    
//    // Open With - enabled if file is selected (not directory, not "..")
//    bool is_file = has_selection && !fb->entries[fb->selected_index].is_dir;
//    fb->menu_items[1].enabled = is_file && !is_parent;
//
// 6. Handle double-click to open in fb_handle_click:
//
//    // In the click handling code, when detecting double-click:
//    static uint64_t last_click_time = 0;
//    static int last_click_index = -1;
//    
//    // After calculating clicked_index:
//    uint64_t current_time = get_system_ticks();  // Or similar time function
//    bool is_double_click = (clicked_index == last_click_index && 
//                            current_time - last_click_time < 500);
//    
//    if (is_double_click && clicked_index >= 0) {
//        menu_action_open(fb);  // Open on double-click
//    }
//    
//    last_click_index = clicked_index;
//    last_click_time = current_time;

