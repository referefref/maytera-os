// mime.h - MIME type handling for MayteraOS
// Provides MIME type detection from file extensions and content (magic numbers)
#ifndef MIME_H
#define MIME_H

#include "../types.h"

// Maximum lengths
#define MIME_TYPE_MAX_LEN       64
#define MIME_EXT_MAX_LEN        16
#define MIME_MAGIC_MAX_LEN      16
#define MIME_DESC_MAX_LEN       64

// Number of built-in MIME types
#define MIME_TYPE_COUNT         64

// MIME type categories (major types)
typedef enum {
    MIME_CAT_UNKNOWN = 0,
    MIME_CAT_APPLICATION,
    MIME_CAT_AUDIO,
    MIME_CAT_IMAGE,
    MIME_CAT_TEXT,
    MIME_CAT_VIDEO,
    MIME_CAT_FONT,
    MIME_CAT_MODEL,
    MIME_CAT_MULTIPART,
    MIME_CAT_INODE,         // Special filesystem types
} mime_category_t;

// Flags for MIME type properties
#define MIME_FLAG_TEXT          0x01    // File is text-based (can be viewed as text)
#define MIME_FLAG_BINARY        0x02    // File is binary data
#define MIME_FLAG_COMPRESSED    0x04    // File is compressed
#define MIME_FLAG_EXECUTABLE    0x08    // File is executable
#define MIME_FLAG_ARCHIVE       0x10    // File is an archive
#define MIME_FLAG_MEDIA         0x20    // File is audio/video media
#define MIME_FLAG_DOCUMENT      0x40    // File is a document type

// MIME type entry
typedef struct {
    char type[MIME_TYPE_MAX_LEN];       // e.g., "text/plain", "image/png"
    char extension[MIME_EXT_MAX_LEN];   // Primary extension (e.g., ".txt")
    char description[MIME_DESC_MAX_LEN]; // Human-readable description
    uint8_t magic[MIME_MAGIC_MAX_LEN];  // Magic bytes (file signature)
    uint8_t magic_len;                  // Length of magic bytes
    uint16_t magic_offset;              // Offset to check magic bytes (usually 0)
    mime_category_t category;           // Major type category
    uint32_t flags;                     // MIME_FLAG_* properties
    bool registered;                    // Is this entry active?
} mime_type_t;

// ============================================================================
// MIME Type API
// ============================================================================

/**
 * Initialize the MIME type system
 */
void mime_init(void);

/**
 * Get MIME type from file extension
 * @param extension File extension (e.g., ".txt" or "txt")
 * @return MIME type string (e.g., "text/plain") or "application/octet-stream" if unknown
 */
const char *mime_type_from_extension(const char *extension);

/**
 * Get MIME type from file content using magic number detection
 * @param data File content buffer
 * @param len Length of data buffer (at least 32 bytes recommended)
 * @return MIME type string or "application/octet-stream" if unknown
 */
const char *mime_type_from_content(const uint8_t *data, size_t len);

/**
 * Get MIME type entry from extension
 * @param extension File extension
 * @return Pointer to MIME type entry or NULL if not found
 */
const mime_type_t *mime_get_entry_by_ext(const char *extension);

/**
 * Get MIME type entry from MIME type string
 * @param mime_type MIME type string (e.g., "text/plain")
 * @return Pointer to MIME type entry or NULL if not found
 */
const mime_type_t *mime_get_entry(const char *mime_type);

/**
 * Get MIME category from MIME type string
 * @param mime_type MIME type string
 * @return MIME category enum value
 */
mime_category_t mime_get_category(const char *mime_type);

/**
 * Get MIME category name
 * @param category MIME category enum value
 * @return Category name string (e.g., "text", "image")
 */
const char *mime_category_name(mime_category_t category);

/**
 * Get human-readable description for a MIME type
 * @param mime_type MIME type string
 * @return Description string or the MIME type if no description available
 */
const char *mime_get_description(const char *mime_type);

/**
 * Check if a MIME type has a specific flag
 * @param mime_type MIME type string
 * @param flag Flag to check (MIME_FLAG_*)
 * @return true if the MIME type has the flag
 */
bool mime_has_flag(const char *mime_type, uint32_t flag);

/**
 * Check if a MIME type represents text content
 * @param mime_type MIME type string
 * @return true if the content is text-based
 */
bool mime_is_text(const char *mime_type);

/**
 * Check if a MIME type represents image content
 * @param mime_type MIME type string
 * @return true if the content is an image
 */
bool mime_is_image(const char *mime_type);

/**
 * Check if a MIME type represents audio content
 * @param mime_type MIME type string
 * @return true if the content is audio
 */
bool mime_is_audio(const char *mime_type);

/**
 * Check if a MIME type represents video content
 * @param mime_type MIME type string
 * @return true if the content is video
 */
bool mime_is_video(const char *mime_type);

/**
 * Register a new MIME type
 * @param type MIME type string
 * @param extension Primary file extension
 * @param description Human-readable description
 * @param category Category enum value
 * @param flags Flags (MIME_FLAG_*)
 * @param magic Magic bytes (can be NULL)
 * @param magic_len Length of magic bytes
 * @param magic_offset Offset to check magic bytes
 * @return true on success, false if table is full
 */
bool mime_register(const char *type, const char *extension, const char *description,
                   mime_category_t category, uint32_t flags,
                   const uint8_t *magic, uint8_t magic_len, uint16_t magic_offset);

/**
 * Get list of all registered MIME types
 * @param count Output: number of registered types
 * @return Array of MIME type entries
 */
const mime_type_t *mime_get_all(int *count);

/**
 * Get file extension from MIME type
 * @param mime_type MIME type string
 * @return Primary extension for the type or "" if unknown
 */
const char *mime_get_extension(const char *mime_type);

#endif // MIME_H
