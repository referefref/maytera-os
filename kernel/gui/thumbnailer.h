// thumbnailer.h - Thumbnail generation system for MayteraOS
// Generates and caches thumbnails for the file browser
#ifndef THUMBNAILER_H
#define THUMBNAILER_H

#include "../types.h"
#include "image.h"

// ============================================
// Thumbnail Size Constants
// ============================================

#define THUMB_SIZE_SMALL    48      // Small icons for list view
#define THUMB_SIZE_MEDIUM   96      // Medium icons for icon view
#define THUMB_SIZE_LARGE    256     // Large icons for thumbnail view

// Default thumbnail size
#define THUMB_SIZE_DEFAULT  THUMB_SIZE_MEDIUM

// Cache configuration
#define THUMB_CACHE_DIR         "/.thumbnails/"
#define THUMB_CACHE_SIZE_MAX    (100 * 1024 * 1024)  // 100MB max cache size
#define THUMB_CACHE_ENTRIES_MAX 4096                  // Max number of cached thumbnails
#define THUMB_MAX_PATH          256
#define THUMB_HASH_SIZE         33                    // MD5 hash + null terminator

// Thumbnail generation limits
#define THUMB_MAX_SOURCE_SIZE   (50 * 1024 * 1024)   // 50MB max source file size
#define THUMB_MIN_IMAGE_DIM     16                    // Minimum image dimension
#define THUMB_MAX_IMAGE_DIM     16384                 // Maximum image dimension

// Text preview settings
#define THUMB_TEXT_MAX_LINES    8
#define THUMB_TEXT_LINE_WIDTH   32
#define THUMB_TEXT_FONT_SIZE    8   // Pixels per character

// Audio waveform settings
#define THUMB_WAVEFORM_SAMPLES  64
#define THUMB_WAVEFORM_HEIGHT   32

// Async generation queue
#define THUMB_QUEUE_SIZE        32

// ============================================
// Thumbnail Types
// ============================================

typedef enum {
    THUMB_TYPE_NONE = 0,        // No thumbnail available
    THUMB_TYPE_IMAGE,           // Scaled-down image (BMP, PNG, JPEG)
    THUMB_TYPE_TEXT,            // Text file preview (first few lines)
    THUMB_TYPE_AUDIO,           // Audio waveform or album art
    THUMB_TYPE_VIDEO,           // Video frame thumbnail
    THUMB_TYPE_PDF,             // PDF first page preview
    THUMB_TYPE_FOLDER,          // Folder composite preview
    THUMB_TYPE_GENERIC,         // Generic file type icon
    THUMB_TYPE_LOADING,         // Placeholder while generating
    THUMB_TYPE_ERROR            // Error generating thumbnail
} thumb_type_t;

// ============================================
// Thumbnail Size Enum
// ============================================

typedef enum {
    THUMB_SMALL = 0,            // 48x48
    THUMB_MEDIUM,               // 96x96
    THUMB_LARGE                 // 256x256
} thumb_size_t;

// ============================================
// Thumbnail Data Structure
// ============================================

typedef struct thumbnail {
    // Identification
    char source_path[THUMB_MAX_PATH];   // Full path to source file
    char hash[THUMB_HASH_SIZE];         // MD5 hash of path + mtime for cache key
    
    // Source file info
    uint32_t source_mtime;              // Source file modification time
    uint32_t source_size;               // Source file size
    
    // Thumbnail data
    thumb_type_t type;                  // Type of thumbnail
    thumb_size_t size;                  // Size category
    uint32_t width;                     // Actual pixel width
    uint32_t height;                    // Actual pixel height
    uint32_t *pixels;                   // BGRA pixel data
    
    // Text preview data (for text files)
    char text_lines[THUMB_TEXT_MAX_LINES][THUMB_TEXT_LINE_WIDTH + 1];
    int text_line_count;
    
    // Cache management
    uint64_t last_access;               // Timestamp of last access (for LRU)
    uint32_t cache_size;                // Size in bytes when cached
    bool cached_on_disk;                // Is this thumbnail cached to disk?
    bool is_loading;                    // Currently being generated async
    
    // Linked list for cache
    struct thumbnail *next;
    struct thumbnail *prev;
} thumbnail_t;

// ============================================
// Thumbnail Cache Structure
// ============================================

typedef struct {
    thumbnail_t *head;                  // Most recently used
    thumbnail_t *tail;                  // Least recently used
    int count;                          // Number of cached thumbnails
    uint64_t total_size;                // Total memory usage
    uint64_t max_size;                  // Maximum cache size
    
    // Hash table for O(1) lookup by path
    thumbnail_t *hash_table[256];       // Simple hash bucket array
} thumb_cache_t;

// ============================================
// Async Generation Queue
// ============================================

typedef struct {
    char path[THUMB_MAX_PATH];          // File path to generate thumbnail for
    thumb_size_t size;                  // Requested size
    void (*callback)(thumbnail_t *thumb, void *user_data);  // Completion callback
    void *user_data;                    // User data for callback
    bool in_progress;                   // Currently being generated
    bool cancelled;                     // Generation cancelled
} thumb_queue_entry_t;

typedef struct {
    thumb_queue_entry_t entries[THUMB_QUEUE_SIZE];
    int head;                           // Next entry to process
    int tail;                           // Next free slot
    int count;                          // Number of pending requests
    bool running;                       // Is the generator running
} thumb_queue_t;

// ============================================
// Supported Format List
// ============================================

// Image formats
extern const char *thumb_image_formats[];
extern int thumb_image_format_count;

// Text formats
extern const char *thumb_text_formats[];
extern int thumb_text_format_count;

// Audio formats
extern const char *thumb_audio_formats[];
extern int thumb_audio_format_count;

// Video formats
extern const char *thumb_video_formats[];
extern int thumb_video_format_count;

// Document formats
extern const char *thumb_document_formats[];
extern int thumb_document_format_count;

// ============================================
// Thumbnail Cache API
// ============================================

/**
 * Initialize the thumbnail cache system
 * @param max_size Maximum cache size in bytes (0 = use default)
 * @return 0 on success, negative error code on failure
 */
int thumb_cache_init(uint64_t max_size);

/**
 * Shutdown the thumbnail cache, freeing all resources
 */
void thumb_cache_shutdown(void);

/**
 * Get a thumbnail from cache or generate one
 * @param path Full path to the file
 * @param size Desired thumbnail size
 * @return Thumbnail pointer (do not free), or NULL if not available
 */
thumbnail_t *thumb_cache_get(const char *path, thumb_size_t size);

/**
 * Check if a thumbnail is cached (without generating)
 * @param path Full path to the file
 * @param size Desired thumbnail size
 * @return true if cached, false otherwise
 */
bool thumb_cache_has(const char *path, thumb_size_t size);

/**
 * Invalidate a cached thumbnail (force regeneration)
 * @param path Full path to the file
 */
void thumb_cache_invalidate(const char *path);

/**
 * Clear all cached thumbnails
 */
void thumb_cache_clear(void);

/**
 * Get cache statistics
 * @param count Output: number of cached thumbnails
 * @param size Output: total cache size in bytes
 * @param hits Output: cache hit count
 * @param misses Output: cache miss count
 */
void thumb_cache_stats(int *count, uint64_t *size, uint64_t *hits, uint64_t *misses);

/**
 * Evict thumbnails using LRU policy until cache is under max size
 * @param target_size Target size to reach
 * @return Number of thumbnails evicted
 */
int thumb_cache_evict(uint64_t target_size);

// ============================================
// Thumbnail Generation API
// ============================================

/**
 * Generate a thumbnail for a file (synchronous)
 * @param path Full path to the file
 * @param size Desired thumbnail size
 * @param thumb Output thumbnail structure (allocated by caller)
 * @return 0 on success, negative error code on failure
 */
int thumb_generate(const char *path, thumb_size_t size, thumbnail_t *thumb);

/**
 * Generate a thumbnail asynchronously
 * @param path Full path to the file
 * @param size Desired thumbnail size
 * @param callback Function to call when complete
 * @param user_data User data passed to callback
 * @return 0 on success, negative error code on failure
 */
int thumb_generate_async(const char *path, thumb_size_t size,
                         void (*callback)(thumbnail_t *thumb, void *user_data),
                         void *user_data);

/**
 * Cancel pending async thumbnail generation
 * @param path Full path to the file (NULL = cancel all)
 */
void thumb_cancel_async(const char *path);

/**
 * Process pending async thumbnail generations
 * Call this periodically from main loop
 * @return Number of thumbnails generated this call
 */
int thumb_process_async(void);

// ============================================
// Format-Specific Generators
// ============================================

/**
 * Generate thumbnail for an image file
 * @param path Full path to image
 * @param data File data (or NULL to load from disk)
 * @param data_size Size of data
 * @param size Target thumbnail size
 * @param thumb Output thumbnail
 * @return 0 on success, negative error code on failure
 */
int thumb_generate_image(const char *path, const void *data, uint32_t data_size,
                         thumb_size_t size, thumbnail_t *thumb);

/**
 * Generate thumbnail for a text file
 * @param path Full path to text file
 * @param data File data (or NULL to load from disk)
 * @param data_size Size of data
 * @param size Target thumbnail size
 * @param thumb Output thumbnail
 * @return 0 on success, negative error code on failure
 */
int thumb_generate_text(const char *path, const void *data, uint32_t data_size,
                        thumb_size_t size, thumbnail_t *thumb);

/**
 * Generate thumbnail for an audio file (waveform or album art)
 * @param path Full path to audio file
 * @param size Target thumbnail size
 * @param thumb Output thumbnail
 * @return 0 on success, negative error code on failure
 */
int thumb_generate_audio(const char *path, thumb_size_t size, thumbnail_t *thumb);

/**
 * Generate thumbnail for a video file (extract frame)
 * @param path Full path to video file
 * @param size Target thumbnail size
 * @param thumb Output thumbnail
 * @return 0 on success, negative error code on failure
 */
int thumb_generate_video(const char *path, thumb_size_t size, thumbnail_t *thumb);

/**
 * Generate thumbnail for a PDF file (render first page)
 * @param path Full path to PDF file
 * @param size Target thumbnail size
 * @param thumb Output thumbnail
 * @return 0 on success, negative error code on failure
 */
int thumb_generate_pdf(const char *path, thumb_size_t size, thumbnail_t *thumb);

/**
 * Generate composite thumbnail for a folder
 * @param path Full path to folder
 * @param size Target thumbnail size
 * @param thumb Output thumbnail
 * @return 0 on success, negative error code on failure
 */
int thumb_generate_folder(const char *path, thumb_size_t size, thumbnail_t *thumb);

// ============================================
// Utility Functions
// ============================================

/**
 * Get the appropriate thumbnail size in pixels
 * @param size Size enum
 * @return Size in pixels
 */
int thumb_get_pixel_size(thumb_size_t size);

/**
 * Detect the file type for thumbnail generation
 * @param path Full path to file
 * @return Thumbnail type that should be generated
 */
thumb_type_t thumb_detect_type(const char *path);

/**
 * Check if a file extension is supported for thumbnailing
 * @param extension File extension (without dot)
 * @return true if supported, false otherwise
 */
bool thumb_is_supported(const char *extension);

/**
 * Scale image pixels to target size (nearest neighbor)
 * @param src Source pixel data
 * @param src_w Source width
 * @param src_h Source height
 * @param dst Destination pixel buffer (allocated by caller)
 * @param dst_w Destination width
 * @param dst_h Destination height
 */
void thumb_scale_pixels(const uint32_t *src, uint32_t src_w, uint32_t src_h,
                        uint32_t *dst, uint32_t dst_w, uint32_t dst_h);

/**
 * Scale image preserving aspect ratio
 * @param src_w Source width
 * @param src_h Source height
 * @param max_size Maximum dimension (width or height)
 * @param out_w Output width
 * @param out_h Output height
 */
void thumb_calculate_aspect_size(uint32_t src_w, uint32_t src_h, uint32_t max_size,
                                  uint32_t *out_w, uint32_t *out_h);

/**
 * Free thumbnail resources (pixels, etc)
 * @param thumb Thumbnail to free
 */
void thumb_free(thumbnail_t *thumb);

/**
 * Compute hash for cache key
 * @param path File path
 * @param mtime Modification time
 * @param out_hash Output hash string (THUMB_HASH_SIZE bytes)
 */
void thumb_compute_hash(const char *path, uint32_t mtime, char *out_hash);

// ============================================
// Disk Cache Functions
// ============================================

/**
 * Load a thumbnail from disk cache
 * @param hash Cache key hash
 * @param thumb Output thumbnail
 * @return 0 on success, negative error code on failure
 */
int thumb_disk_cache_load(const char *hash, thumbnail_t *thumb);

/**
 * Save a thumbnail to disk cache
 * @param thumb Thumbnail to save
 * @return 0 on success, negative error code on failure
 */
int thumb_disk_cache_save(thumbnail_t *thumb);

/**
 * Delete a thumbnail from disk cache
 * @param hash Cache key hash
 * @return 0 on success, negative error code on failure
 */
int thumb_disk_cache_delete(const char *hash);

/**
 * Clean up disk cache (delete old/invalid entries)
 * @param max_age Maximum age in seconds (0 = delete all)
 * @return Number of entries deleted
 */
int thumb_disk_cache_cleanup(uint32_t max_age);

/**
 * Get disk cache size
 * @return Total size in bytes
 */
uint64_t thumb_disk_cache_size(void);

// ============================================
// Error Codes
// ============================================

#define THUMB_SUCCESS           0
#define THUMB_ERR_NULL_PTR      -1
#define THUMB_ERR_NOT_FOUND     -2
#define THUMB_ERR_UNSUPPORTED   -3
#define THUMB_ERR_NOMEM         -4
#define THUMB_ERR_IO            -5
#define THUMB_ERR_CORRUPT       -6
#define THUMB_ERR_TOO_LARGE     -7
#define THUMB_ERR_QUEUE_FULL    -8
#define THUMB_ERR_CANCELLED     -9

/**
 * Get error string for error code
 */
const char *thumb_error_string(int err);

#endif // THUMBNAILER_H
