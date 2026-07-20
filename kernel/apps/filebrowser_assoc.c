// filebrowser_assoc.c - File browser association integration
// Helper functions for integrating file associations into the file browser

#include "associations.h"
#include "mime.h"
#include "open_with_dialog.h"
#include "../string.h"
#include "../serial.h"
#include "../gui/filebrowser.h"

// ============================================================================
// File Browser Integration Functions
// ============================================================================

/**
 * Open a file from the file browser with its default application
 * @param path Current path
 * @param filename File name
 * @return true if file was opened
 */
bool fb_open_file_default(const char *path, const char *filename) {
    // Build full path
    char filepath[FB_MAX_PATH];
    size_t len = strlen(path);
    
    if (len >= FB_MAX_PATH - 1) return false;
    
    strcpy(filepath, path);
    if (len > 0 && filepath[len - 1] != '/') {
        strcat(filepath, "/");
    }
    strncat(filepath, filename, FB_MAX_PATH - strlen(filepath) - 1);
    
    kprintf("[FileBrowser] Opening with default app: %s\n", filepath);
    
    // Try to open with registered default app
    if (assoc_open_file(filepath)) {
        return true;
    }
    
    // No handler found
    kprintf("[FileBrowser] No default app for: %s\n", filepath);
    return false;
}

/**
 * Show the "Open With" dialog for a file
 * @param path Current path
 * @param filename File name
 */
void fb_show_open_with(const char *path, const char *filename) {
    // Build full path
    char filepath[FB_MAX_PATH];
    size_t len = strlen(path);
    
    if (len >= FB_MAX_PATH - 1) return;
    
    strcpy(filepath, path);
    if (len > 0 && filepath[len - 1] != '/') {
        strcat(filepath, "/");
    }
    strncat(filepath, filename, FB_MAX_PATH - strlen(filepath) - 1);
    
    kprintf("[FileBrowser] Showing Open With dialog for: %s\n", filepath);
    
    // Show the dialog
    assoc_show_open_with_dialog(filepath, NULL);
}

/**
 * Get display name for file type
 * @param filename File name
 * @return Human readable file type name
 */
const char *fb_get_file_type_name(const char *filename) {
    // Get extension
    const char *ext = filename;
    for (const char *p = filename; *p; p++) {
        if (*p == '.') ext = p;
    }
    
    // Get MIME type
    const char *mime = mime_type_from_extension(ext);
    
    // Get description
    return mime_get_description(mime);
}

/**
 * Check if a file can be opened with any application
 * @param filename File name
 * @return true if at least one app can handle this file
 */
bool fb_can_open_file(const char *filename) {
    // Get extension
    const char *ext = filename;
    for (const char *p = filename; *p; p++) {
        if (*p == '.') ext = p;
    }
    
    // Get MIME type
    const char *mime = mime_type_from_extension(ext);
    
    // Check if we have a handler
    assoc_app_t *app = assoc_get_default(mime);
    return (app != NULL);
}

/**
 * Get the default application name for a file
 * @param filename File name
 * @return Application name or NULL if no default
 */
const char *fb_get_default_app_name(const char *filename) {
    // Get extension
    const char *ext = filename;
    for (const char *p = filename; *p; p++) {
        if (*p == '.') ext = p;
    }
    
    // Get MIME type
    const char *mime = mime_type_from_extension(ext);
    
    // Get default app
    assoc_app_t *app = assoc_get_default(mime);
    if (app) {
        return app->name;
    }
    
    return NULL;
}

/**
 * Get the number of applications that can open a file
 * @param filename File name
 * @return Number of apps
 */
int fb_get_app_count_for_file(const char *filename) {
    // Get extension
    const char *ext = filename;
    for (const char *p = filename; *p; p++) {
        if (*p == '.') ext = p;
    }
    
    // Get MIME type
    const char *mime = mime_type_from_extension(ext);
    
    // Get all apps
    assoc_app_t *apps[ASSOC_MAX_APPS];
    return assoc_get_all(mime, apps);
}
