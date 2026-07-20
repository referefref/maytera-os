// mime.c - MIME type handling for MayteraOS
// Provides MIME type detection from file extensions and content (magic numbers)

#include "mime.h"
#include "../string.h"
#include "../serial.h"

// ============================================================================
// Storage
// ============================================================================

static mime_type_t g_mime_types[MIME_TYPE_COUNT];
static int g_mime_count = 0;
static bool g_mime_initialized = false;

// Default MIME type for unknown content
static const char *MIME_DEFAULT = "application/octet-stream";

// ============================================================================
// Helper Functions
// ============================================================================

// Case-insensitive string comparison
static int mime_stricmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return *a - *b;
}

// Normalize extension (add leading dot if missing, lowercase)
static void normalize_ext(const char *ext, char *out, size_t out_size) {
    if (!ext || !out || out_size < 2) return;
    
    int i = 0;
    
    // Add leading dot if missing
    if (ext[0] != '.') {
        out[i++] = '.';
    }
    
    // Copy and lowercase
    while (*ext && i < (int)out_size - 1) {
        char c = *ext++;
        out[i++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    out[i] = '\0';
}

// ============================================================================
// Built-in MIME Types Registration
// ============================================================================

// Helper macro for registering MIME types with magic bytes
#define REGISTER_MIME(type, ext, desc, cat, flags) \
    mime_register(type, ext, desc, cat, flags, NULL, 0, 0)

#define REGISTER_MIME_MAGIC(type, ext, desc, cat, flags, ...) \
    do { \
        static const uint8_t magic[] = { __VA_ARGS__ }; \
        mime_register(type, ext, desc, cat, flags, magic, sizeof(magic), 0); \
    } while(0)

static void mime_register_defaults(void) {
    // ========================================================================
    // Text types
    // ========================================================================
    REGISTER_MIME("text/plain", ".txt", "Plain Text", MIME_CAT_TEXT, MIME_FLAG_TEXT);
    REGISTER_MIME("text/html", ".html", "HTML Document", MIME_CAT_TEXT, MIME_FLAG_TEXT | MIME_FLAG_DOCUMENT);
    REGISTER_MIME("text/css", ".css", "CSS Stylesheet", MIME_CAT_TEXT, MIME_FLAG_TEXT);
    REGISTER_MIME("text/javascript", ".js", "JavaScript", MIME_CAT_TEXT, MIME_FLAG_TEXT);
    REGISTER_MIME("text/xml", ".xml", "XML Document", MIME_CAT_TEXT, MIME_FLAG_TEXT | MIME_FLAG_DOCUMENT);
    REGISTER_MIME("text/csv", ".csv", "CSV Data", MIME_CAT_TEXT, MIME_FLAG_TEXT);
    REGISTER_MIME("text/markdown", ".md", "Markdown Document", MIME_CAT_TEXT, MIME_FLAG_TEXT | MIME_FLAG_DOCUMENT);
    REGISTER_MIME("text/x-c", ".c", "C Source Code", MIME_CAT_TEXT, MIME_FLAG_TEXT);
    REGISTER_MIME("text/x-h", ".h", "C Header File", MIME_CAT_TEXT, MIME_FLAG_TEXT);
    REGISTER_MIME("text/x-asm", ".asm", "Assembly Source", MIME_CAT_TEXT, MIME_FLAG_TEXT);
    REGISTER_MIME("text/x-python", ".py", "Python Script", MIME_CAT_TEXT, MIME_FLAG_TEXT | MIME_FLAG_EXECUTABLE);
    REGISTER_MIME("text/x-shellscript", ".sh", "Shell Script", MIME_CAT_TEXT, MIME_FLAG_TEXT | MIME_FLAG_EXECUTABLE);
    
    // ========================================================================
    // Image types (with magic bytes)
    // ========================================================================
    REGISTER_MIME_MAGIC("image/bmp", ".bmp", "BMP Image", MIME_CAT_IMAGE, MIME_FLAG_BINARY, 
                        0x42, 0x4D);  // "BM"
    REGISTER_MIME_MAGIC("image/png", ".png", "PNG Image", MIME_CAT_IMAGE, MIME_FLAG_BINARY,
                        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A);
    REGISTER_MIME_MAGIC("image/jpeg", ".jpg", "JPEG Image", MIME_CAT_IMAGE, MIME_FLAG_BINARY,
                        0xFF, 0xD8, 0xFF);
    REGISTER_MIME_MAGIC("image/gif", ".gif", "GIF Image", MIME_CAT_IMAGE, MIME_FLAG_BINARY,
                        0x47, 0x49, 0x46, 0x38);  // "GIF8"
    REGISTER_MIME_MAGIC("image/webp", ".webp", "WebP Image", MIME_CAT_IMAGE, MIME_FLAG_BINARY,
                        0x52, 0x49, 0x46, 0x46);  // "RIFF" (need to also check WEBP at offset 8)
    REGISTER_MIME_MAGIC("image/x-icon", ".ico", "Icon", MIME_CAT_IMAGE, MIME_FLAG_BINARY,
                        0x00, 0x00, 0x01, 0x00);
    REGISTER_MIME_MAGIC("image/tiff", ".tiff", "TIFF Image", MIME_CAT_IMAGE, MIME_FLAG_BINARY,
                        0x49, 0x49, 0x2A, 0x00);  // Little-endian TIFF
    REGISTER_MIME("image/svg+xml", ".svg", "SVG Image", MIME_CAT_IMAGE, MIME_FLAG_TEXT);
    
    // ========================================================================
    // Audio types (with magic bytes)
    // ========================================================================
    REGISTER_MIME_MAGIC("audio/wav", ".wav", "WAV Audio", MIME_CAT_AUDIO, 
                        MIME_FLAG_BINARY | MIME_FLAG_MEDIA,
                        0x52, 0x49, 0x46, 0x46);  // "RIFF"
    REGISTER_MIME_MAGIC("audio/mpeg", ".mp3", "MP3 Audio", MIME_CAT_AUDIO,
                        MIME_FLAG_BINARY | MIME_FLAG_MEDIA | MIME_FLAG_COMPRESSED,
                        0xFF, 0xFB);  // MP3 frame sync (or 0x49, 0x44, 0x33 for ID3)
    REGISTER_MIME_MAGIC("audio/ogg", ".ogg", "OGG Audio", MIME_CAT_AUDIO,
                        MIME_FLAG_BINARY | MIME_FLAG_MEDIA | MIME_FLAG_COMPRESSED,
                        0x4F, 0x67, 0x67, 0x53);  // "OggS"
    REGISTER_MIME_MAGIC("audio/flac", ".flac", "FLAC Audio", MIME_CAT_AUDIO,
                        MIME_FLAG_BINARY | MIME_FLAG_MEDIA | MIME_FLAG_COMPRESSED,
                        0x66, 0x4C, 0x61, 0x43);  // "fLaC"
    REGISTER_MIME_MAGIC("audio/aac", ".aac", "AAC Audio", MIME_CAT_AUDIO,
                        MIME_FLAG_BINARY | MIME_FLAG_MEDIA | MIME_FLAG_COMPRESSED,
                        0xFF, 0xF1);  // ADTS frame sync
    REGISTER_MIME("audio/midi", ".mid", "MIDI Audio", MIME_CAT_AUDIO, MIME_FLAG_BINARY | MIME_FLAG_MEDIA);
    
    // ========================================================================
    // Video types (with magic bytes)
    // ========================================================================
    REGISTER_MIME_MAGIC("video/mp4", ".mp4", "MP4 Video", MIME_CAT_VIDEO,
                        MIME_FLAG_BINARY | MIME_FLAG_MEDIA | MIME_FLAG_COMPRESSED,
                        0x00, 0x00, 0x00);  // ftyp box (need to check further)
    REGISTER_MIME_MAGIC("video/x-msvideo", ".avi", "AVI Video", MIME_CAT_VIDEO,
                        MIME_FLAG_BINARY | MIME_FLAG_MEDIA,
                        0x52, 0x49, 0x46, 0x46);  // "RIFF"
    REGISTER_MIME_MAGIC("video/webm", ".webm", "WebM Video", MIME_CAT_VIDEO,
                        MIME_FLAG_BINARY | MIME_FLAG_MEDIA | MIME_FLAG_COMPRESSED,
                        0x1A, 0x45, 0xDF, 0xA3);  // EBML header
    REGISTER_MIME_MAGIC("video/x-matroska", ".mkv", "Matroska Video", MIME_CAT_VIDEO,
                        MIME_FLAG_BINARY | MIME_FLAG_MEDIA | MIME_FLAG_COMPRESSED,
                        0x1A, 0x45, 0xDF, 0xA3);  // EBML header
    REGISTER_MIME("video/quicktime", ".mov", "QuickTime Video", MIME_CAT_VIDEO,
                  MIME_FLAG_BINARY | MIME_FLAG_MEDIA);
    REGISTER_MIME("video/mpeg", ".mpg", "MPEG Video", MIME_CAT_VIDEO,
                  MIME_FLAG_BINARY | MIME_FLAG_MEDIA | MIME_FLAG_COMPRESSED);
    
    // ========================================================================
    // Application types (with magic bytes)
    // ========================================================================
    REGISTER_MIME_MAGIC("application/pdf", ".pdf", "PDF Document", MIME_CAT_APPLICATION,
                        MIME_FLAG_BINARY | MIME_FLAG_DOCUMENT,
                        0x25, 0x50, 0x44, 0x46);  // "%PDF"
    REGISTER_MIME_MAGIC("application/zip", ".zip", "ZIP Archive", MIME_CAT_APPLICATION,
                        MIME_FLAG_BINARY | MIME_FLAG_COMPRESSED | MIME_FLAG_ARCHIVE,
                        0x50, 0x4B, 0x03, 0x04);  // "PK"
    REGISTER_MIME_MAGIC("application/gzip", ".gz", "GZIP Archive", MIME_CAT_APPLICATION,
                        MIME_FLAG_BINARY | MIME_FLAG_COMPRESSED | MIME_FLAG_ARCHIVE,
                        0x1F, 0x8B);
    REGISTER_MIME_MAGIC("application/x-tar", ".tar", "TAR Archive", MIME_CAT_APPLICATION,
                        MIME_FLAG_BINARY | MIME_FLAG_ARCHIVE,
                        0x75, 0x73, 0x74, 0x61, 0x72);  // "ustar" at offset 257
    REGISTER_MIME_MAGIC("application/x-7z-compressed", ".7z", "7-Zip Archive", MIME_CAT_APPLICATION,
                        MIME_FLAG_BINARY | MIME_FLAG_COMPRESSED | MIME_FLAG_ARCHIVE,
                        0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C);  // "7z"
    REGISTER_MIME_MAGIC("application/x-rar-compressed", ".rar", "RAR Archive", MIME_CAT_APPLICATION,
                        MIME_FLAG_BINARY | MIME_FLAG_COMPRESSED | MIME_FLAG_ARCHIVE,
                        0x52, 0x61, 0x72, 0x21);  // "Rar!"
    
    // Executables
    REGISTER_MIME_MAGIC("application/x-executable", ".elf", "ELF Executable", MIME_CAT_APPLICATION,
                        MIME_FLAG_BINARY | MIME_FLAG_EXECUTABLE,
                        0x7F, 0x45, 0x4C, 0x46);  // ELF magic
    REGISTER_MIME_MAGIC("application/x-msdos-program", ".exe", "Windows Executable", MIME_CAT_APPLICATION,
                        MIME_FLAG_BINARY | MIME_FLAG_EXECUTABLE,
                        0x4D, 0x5A);  // "MZ"
    
    // Other applications
    REGISTER_MIME("application/json", ".json", "JSON Data", MIME_CAT_APPLICATION, MIME_FLAG_TEXT);
    REGISTER_MIME("application/xml", ".xml", "XML Data", MIME_CAT_APPLICATION, MIME_FLAG_TEXT | MIME_FLAG_DOCUMENT);
    REGISTER_MIME("application/x-shellscript", ".sh", "Shell Script", MIME_CAT_APPLICATION, 
                  MIME_FLAG_TEXT | MIME_FLAG_EXECUTABLE);
    
    // ========================================================================
    // Font types
    // ========================================================================
    REGISTER_MIME_MAGIC("font/ttf", ".ttf", "TrueType Font", MIME_CAT_FONT, MIME_FLAG_BINARY,
                        0x00, 0x01, 0x00, 0x00);
    REGISTER_MIME_MAGIC("font/otf", ".otf", "OpenType Font", MIME_CAT_FONT, MIME_FLAG_BINARY,
                        0x4F, 0x54, 0x54, 0x4F);  // "OTTO"
    REGISTER_MIME("font/woff", ".woff", "WOFF Font", MIME_CAT_FONT, MIME_FLAG_BINARY | MIME_FLAG_COMPRESSED);
    REGISTER_MIME("font/woff2", ".woff2", "WOFF2 Font", MIME_CAT_FONT, MIME_FLAG_BINARY | MIME_FLAG_COMPRESSED);
    
    // ========================================================================
    // Special inode types
    // ========================================================================
    REGISTER_MIME("inode/directory", "", "Directory", MIME_CAT_INODE, 0);
    REGISTER_MIME("inode/symlink", "", "Symbolic Link", MIME_CAT_INODE, 0);
    REGISTER_MIME("inode/blockdevice", "", "Block Device", MIME_CAT_INODE, 0);
    REGISTER_MIME("inode/chardevice", "", "Character Device", MIME_CAT_INODE, 0);
    
    // ========================================================================
    // Configuration files
    // ========================================================================
    REGISTER_MIME("application/x-config", ".cfg", "Configuration File", MIME_CAT_APPLICATION, MIME_FLAG_TEXT);
    REGISTER_MIME("application/x-ini", ".ini", "INI Configuration", MIME_CAT_APPLICATION, MIME_FLAG_TEXT);
    REGISTER_MIME("application/x-yaml", ".yml", "YAML Data", MIME_CAT_APPLICATION, MIME_FLAG_TEXT);
    REGISTER_MIME("application/x-yaml", ".yaml", "YAML Data", MIME_CAT_APPLICATION, MIME_FLAG_TEXT);
}

// ============================================================================
// Initialization
// ============================================================================

void mime_init(void) {
    if (g_mime_initialized) return;
    
    memset(g_mime_types, 0, sizeof(g_mime_types));
    g_mime_count = 0;
    
    mime_register_defaults();
    
    g_mime_initialized = true;
    kprintf("[MIME] Initialized with %d types\n", g_mime_count);
}

// ============================================================================
// Registration
// ============================================================================

bool mime_register(const char *type, const char *extension, const char *description,
                   mime_category_t category, uint32_t flags,
                   const uint8_t *magic, uint8_t magic_len, uint16_t magic_offset) {
    if (g_mime_count >= MIME_TYPE_COUNT) {
        return false;
    }
    
    mime_type_t *entry = &g_mime_types[g_mime_count];
    
    strncpy(entry->type, type, MIME_TYPE_MAX_LEN - 1);
    entry->type[MIME_TYPE_MAX_LEN - 1] = '\0';
    
    normalize_ext(extension, entry->extension, MIME_EXT_MAX_LEN);
    
    strncpy(entry->description, description, MIME_DESC_MAX_LEN - 1);
    entry->description[MIME_DESC_MAX_LEN - 1] = '\0';
    
    entry->category = category;
    entry->flags = flags;
    entry->magic_offset = magic_offset;
    
    if (magic && magic_len > 0 && magic_len <= MIME_MAGIC_MAX_LEN) {
        memcpy(entry->magic, magic, magic_len);
        entry->magic_len = magic_len;
    } else {
        entry->magic_len = 0;
    }
    
    entry->registered = true;
    g_mime_count++;
    
    return true;
}

// ============================================================================
// Lookup Functions
// ============================================================================

const char *mime_type_from_extension(const char *extension) {
    if (!extension) return MIME_DEFAULT;
    
    char norm_ext[MIME_EXT_MAX_LEN];
    normalize_ext(extension, norm_ext, MIME_EXT_MAX_LEN);
    
    for (int i = 0; i < g_mime_count; i++) {
        if (mime_stricmp(g_mime_types[i].extension, norm_ext) == 0) {
            return g_mime_types[i].type;
        }
    }
    
    return MIME_DEFAULT;
}

const char *mime_type_from_content(const uint8_t *data, size_t len) {
    if (!data || len == 0) return MIME_DEFAULT;
    
    // Check magic bytes for each registered type
    for (int i = 0; i < g_mime_count; i++) {
        mime_type_t *entry = &g_mime_types[i];
        
        if (entry->magic_len == 0) continue;
        
        // Check if we have enough data
        size_t need = entry->magic_offset + entry->magic_len;
        if (len < need) continue;
        
        // Compare magic bytes
        bool match = true;
        for (int j = 0; j < entry->magic_len; j++) {
            if (data[entry->magic_offset + j] != entry->magic[j]) {
                match = false;
                break;
            }
        }
        
        if (match) {
            return entry->type;
        }
    }
    
    // Heuristic: check if it looks like text
    bool looks_like_text = true;
    size_t check_len = len < 512 ? len : 512;
    for (size_t i = 0; i < check_len; i++) {
        uint8_t c = data[i];
        // Allow printable ASCII, tabs, newlines, carriage returns
        if (c < 0x09 || (c > 0x0D && c < 0x20) || c == 0x7F) {
            // Allow some non-ASCII for UTF-8 text
            if (c < 0x80 || (c >= 0xC0 && c <= 0xFD)) {
                looks_like_text = false;
                break;
            }
        }
    }
    
    if (looks_like_text) {
        return "text/plain";
    }
    
    return MIME_DEFAULT;
}

const mime_type_t *mime_get_entry_by_ext(const char *extension) {
    if (!extension) return NULL;
    
    char norm_ext[MIME_EXT_MAX_LEN];
    normalize_ext(extension, norm_ext, MIME_EXT_MAX_LEN);
    
    for (int i = 0; i < g_mime_count; i++) {
        if (mime_stricmp(g_mime_types[i].extension, norm_ext) == 0) {
            return &g_mime_types[i];
        }
    }
    
    return NULL;
}

const mime_type_t *mime_get_entry(const char *mime_type) {
    if (!mime_type) return NULL;
    
    for (int i = 0; i < g_mime_count; i++) {
        if (mime_stricmp(g_mime_types[i].type, mime_type) == 0) {
            return &g_mime_types[i];
        }
    }
    
    return NULL;
}

mime_category_t mime_get_category(const char *mime_type) {
    const mime_type_t *entry = mime_get_entry(mime_type);
    if (entry) {
        return entry->category;
    }
    
    // Parse category from MIME type string
    if (strncmp(mime_type, "text/", 5) == 0) return MIME_CAT_TEXT;
    if (strncmp(mime_type, "image/", 6) == 0) return MIME_CAT_IMAGE;
    if (strncmp(mime_type, "audio/", 6) == 0) return MIME_CAT_AUDIO;
    if (strncmp(mime_type, "video/", 6) == 0) return MIME_CAT_VIDEO;
    if (strncmp(mime_type, "application/", 12) == 0) return MIME_CAT_APPLICATION;
    if (strncmp(mime_type, "font/", 5) == 0) return MIME_CAT_FONT;
    if (strncmp(mime_type, "inode/", 6) == 0) return MIME_CAT_INODE;
    
    return MIME_CAT_UNKNOWN;
}

const char *mime_category_name(mime_category_t category) {
    switch (category) {
        case MIME_CAT_APPLICATION: return "application";
        case MIME_CAT_AUDIO:       return "audio";
        case MIME_CAT_IMAGE:       return "image";
        case MIME_CAT_TEXT:        return "text";
        case MIME_CAT_VIDEO:       return "video";
        case MIME_CAT_FONT:        return "font";
        case MIME_CAT_MODEL:       return "model";
        case MIME_CAT_MULTIPART:   return "multipart";
        case MIME_CAT_INODE:       return "inode";
        default:                   return "unknown";
    }
}

const char *mime_get_description(const char *mime_type) {
    const mime_type_t *entry = mime_get_entry(mime_type);
    if (entry && entry->description[0]) {
        return entry->description;
    }
    return mime_type;
}

bool mime_has_flag(const char *mime_type, uint32_t flag) {
    const mime_type_t *entry = mime_get_entry(mime_type);
    if (entry) {
        return (entry->flags & flag) != 0;
    }
    return false;
}

bool mime_is_text(const char *mime_type) {
    if (!mime_type) return false;
    
    // Check if it has the text flag
    if (mime_has_flag(mime_type, MIME_FLAG_TEXT)) {
        return true;
    }
    
    // Also check category
    return mime_get_category(mime_type) == MIME_CAT_TEXT;
}

bool mime_is_image(const char *mime_type) {
    if (!mime_type) return false;
    return mime_get_category(mime_type) == MIME_CAT_IMAGE;
}

bool mime_is_audio(const char *mime_type) {
    if (!mime_type) return false;
    return mime_get_category(mime_type) == MIME_CAT_AUDIO;
}

bool mime_is_video(const char *mime_type) {
    if (!mime_type) return false;
    return mime_get_category(mime_type) == MIME_CAT_VIDEO;
}

const mime_type_t *mime_get_all(int *count) {
    if (count) *count = g_mime_count;
    return g_mime_types;
}

const char *mime_get_extension(const char *mime_type) {
    const mime_type_t *entry = mime_get_entry(mime_type);
    if (entry) {
        return entry->extension;
    }
    return "";
}
