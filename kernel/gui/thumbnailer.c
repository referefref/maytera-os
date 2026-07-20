#pragma GCC diagnostic ignored "-Wunused-parameter"
// thumbnailer.c - Thumbnail generation system for MayteraOS
// Generates and caches thumbnails for the file browser
#include "thumbnailer.h"
#include "image.h"
#include "png.h"
#include "jpeg.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../cpu/isr.h"
extern volatile uint64_t timer_ticks;

// External filesystem
extern fat_fs_t g_fat_fs;

// ============================================
// Supported Format Lists
// ============================================

const char *thumb_image_formats[] = {
    "bmp", "png", "jpg", "jpeg", "gif", "ico", "tga", NULL
};
int thumb_image_format_count = 7;

const char *thumb_text_formats[] = {
    "txt", "c", "h", "cpp", "hpp", "py", "js", "json", "xml", "html",
    "css", "md", "cfg", "ini", "conf", "log", "sh", "bat", NULL
};
int thumb_text_format_count = 18;

const char *thumb_audio_formats[] = {
    "mp3", "wav", "ogg", "flac", "aac", "wma", "m4a", NULL
};
int thumb_audio_format_count = 7;

const char *thumb_video_formats[] = {
    "mp4", "avi", "mkv", "mov", "wmv", "flv", "webm", NULL
};
int thumb_video_format_count = 7;

const char *thumb_document_formats[] = {
    "pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "odt", "rtf", NULL
};
int thumb_document_format_count = 9;

// ============================================
// Global Cache State
// ============================================

static thumb_cache_t g_thumb_cache = {0};
static thumb_queue_t g_thumb_queue = {0};
static bool g_thumb_initialized = false;
static uint64_t g_cache_hits = 0;
static uint64_t g_cache_misses = 0;

// Simple hash function for path string
static uint8_t thumb_hash_path(const char *path) {
    uint8_t hash = 0;
    while (*path) {
        hash = (hash * 31) + (uint8_t)*path;
        path++;
    }
    return hash;
}

// ============================================
// Thumbnail Cache API Implementation
// ============================================

int thumb_cache_init(uint64_t max_size) {
    if (g_thumb_initialized) {
        return THUMB_SUCCESS;
    }

    memset(&g_thumb_cache, 0, sizeof(thumb_cache_t));
    memset(&g_thumb_queue, 0, sizeof(thumb_queue_t));

    g_thumb_cache.max_size = (max_size > 0) ? max_size : THUMB_CACHE_SIZE_MAX;
    g_thumb_cache.head = NULL;
    g_thumb_cache.tail = NULL;
    g_thumb_cache.count = 0;
    g_thumb_cache.total_size = 0;

    g_thumb_queue.running = true;
    g_cache_hits = 0;
    g_cache_misses = 0;

    // Create cache directory if it doesn't exist
    if (g_fat_fs.mounted) {
        if (fat_exists(&g_fat_fs, THUMB_CACHE_DIR) != 1) {
            fat_mkdir(&g_fat_fs, THUMB_CACHE_DIR);
            kprintf("[Thumbnailer] Created cache directory: %s\n", THUMB_CACHE_DIR);
        }
    }

    g_thumb_initialized = true;
    kprintf("[Thumbnailer] Cache initialized (max size: %llu bytes)\n", g_thumb_cache.max_size);
    return THUMB_SUCCESS;
}

void thumb_cache_shutdown(void) {
    if (!g_thumb_initialized) return;

    // Free all cached thumbnails
    thumb_cache_clear();

    g_thumb_queue.running = false;
    g_thumb_initialized = false;
    kprintf("[Thumbnailer] Cache shutdown\n");
}

// Move thumbnail to front of LRU list
static void thumb_cache_move_to_front(thumbnail_t *thumb) {
    if (!thumb || thumb == g_thumb_cache.head) return;

    // Remove from current position
    if (thumb->prev) thumb->prev->next = thumb->next;
    if (thumb->next) thumb->next->prev = thumb->prev;
    if (thumb == g_thumb_cache.tail) {
        g_thumb_cache.tail = thumb->prev;
    }

    // Insert at front
    thumb->prev = NULL;
    thumb->next = g_thumb_cache.head;
    if (g_thumb_cache.head) {
        g_thumb_cache.head->prev = thumb;
    }
    g_thumb_cache.head = thumb;

    if (!g_thumb_cache.tail) {
        g_thumb_cache.tail = thumb;
    }

    thumb->last_access = timer_ticks;
}

// Find thumbnail in cache
static thumbnail_t *thumb_cache_find(const char *path, thumb_size_t size) {
    uint8_t bucket = thumb_hash_path(path);
    thumbnail_t *thumb = g_thumb_cache.hash_table[bucket];

    while (thumb) {
        if (thumb->size == size && strcmp(thumb->source_path, path) == 0) {
            return thumb;
        }
        thumb = thumb->next;
    }

    return NULL;
}

// Add thumbnail to cache
static void thumb_cache_add(thumbnail_t *thumb) {
    if (!thumb) return;

    uint8_t bucket = thumb_hash_path(thumb->source_path);

    // Add to hash table bucket
    thumb->next = g_thumb_cache.hash_table[bucket];
    thumb->prev = NULL;
    if (g_thumb_cache.hash_table[bucket]) {
        g_thumb_cache.hash_table[bucket]->prev = thumb;
    }
    g_thumb_cache.hash_table[bucket] = thumb;

    // Add to LRU list (at front)
    thumb_cache_move_to_front(thumb);

    g_thumb_cache.count++;
    g_thumb_cache.total_size += thumb->cache_size;

    // Evict if needed
    if (g_thumb_cache.total_size > g_thumb_cache.max_size) {
        thumb_cache_evict(g_thumb_cache.max_size * 80 / 100);  // Evict down to 80%
    }
}

// Remove thumbnail from cache
static void thumb_cache_remove(thumbnail_t *thumb) {
    if (!thumb) return;

    uint8_t bucket = thumb_hash_path(thumb->source_path);

    // Remove from hash table
    if (thumb->prev) thumb->prev->next = thumb->next;
    if (thumb->next) thumb->next->prev = thumb->prev;
    if (g_thumb_cache.hash_table[bucket] == thumb) {
        g_thumb_cache.hash_table[bucket] = thumb->next;
    }

    // Remove from LRU list
    if (thumb == g_thumb_cache.head) {
        g_thumb_cache.head = thumb->next;
    }
    if (thumb == g_thumb_cache.tail) {
        g_thumb_cache.tail = thumb->prev;
    }

    g_thumb_cache.count--;
    g_thumb_cache.total_size -= thumb->cache_size;

    thumb_free(thumb);
    kfree(thumb);
}

thumbnail_t *thumb_cache_get(const char *path, thumb_size_t size) {
    if (!g_thumb_initialized || !path) return NULL;

    // Look in memory cache first
    thumbnail_t *cached = thumb_cache_find(path, size);
    if (cached) {
        // Validate mtime
        fat_file_t file;
        if (fat_open(&g_fat_fs, path, &file) == 0) {
            uint32_t current_mtime = 0;
            fat_close(&file);

            if (current_mtime == cached->source_mtime) {
                thumb_cache_move_to_front(cached);
                g_cache_hits++;
                return cached;
            } else {
                // File modified, invalidate cache
                thumb_cache_remove(cached);
                cached = NULL;
            }
        }
    }

    g_cache_misses++;

    // Generate new thumbnail
    thumbnail_t *thumb = (thumbnail_t *)kmalloc(sizeof(thumbnail_t));
    if (!thumb) return NULL;
    memset(thumb, 0, sizeof(thumbnail_t));

    int result = thumb_generate(path, size, thumb);
    if (result != THUMB_SUCCESS) {
        kfree(thumb);
        return NULL;
    }

    // Add to cache
    thumb_cache_add(thumb);

    return thumb;
}

bool thumb_cache_has(const char *path, thumb_size_t size) {
    if (!g_thumb_initialized || !path) return false;
    return thumb_cache_find(path, size) != NULL;
}

void thumb_cache_invalidate(const char *path) {
    if (!g_thumb_initialized || !path) return;

    // Remove all sizes for this path
    for (int i = 0; i <= THUMB_LARGE; i++) {
        thumbnail_t *thumb = thumb_cache_find(path, (thumb_size_t)i);
        if (thumb) {
            thumb_cache_remove(thumb);
        }
    }
}

void thumb_cache_clear(void) {
    if (!g_thumb_initialized) return;

    // Remove all thumbnails
    for (int i = 0; i < 256; i++) {
        while (g_thumb_cache.hash_table[i]) {
            thumbnail_t *thumb = g_thumb_cache.hash_table[i];
            g_thumb_cache.hash_table[i] = thumb->next;
            thumb_free(thumb);
            kfree(thumb);
        }
    }

    g_thumb_cache.head = NULL;
    g_thumb_cache.tail = NULL;
    g_thumb_cache.count = 0;
    g_thumb_cache.total_size = 0;

    kprintf("[Thumbnailer] Cache cleared\n");
}

void thumb_cache_stats(int *count, uint64_t *size, uint64_t *hits, uint64_t *misses) {
    if (count) *count = g_thumb_cache.count;
    if (size) *size = g_thumb_cache.total_size;
    if (hits) *hits = g_cache_hits;
    if (misses) *misses = g_cache_misses;
}

int thumb_cache_evict(uint64_t target_size) {
    if (!g_thumb_initialized) return 0;

    int evicted = 0;

    // Evict from tail (LRU) until under target size
    while (g_thumb_cache.total_size > target_size && g_thumb_cache.tail) {
        thumbnail_t *victim = g_thumb_cache.tail;
        thumb_cache_remove(victim);
        evicted++;
    }

    kprintf("[Thumbnailer] Evicted %d thumbnails\n", evicted);
    return evicted;
}

// ============================================
// Thumbnail Generation API
// ============================================

int thumb_generate(const char *path, thumb_size_t size, thumbnail_t *thumb) {
    if (!path || !thumb) return THUMB_ERR_NULL_PTR;

    memset(thumb, 0, sizeof(thumbnail_t));
    strncpy(thumb->source_path, path, THUMB_MAX_PATH - 1);
    thumb->size = size;

    // Get file info
    fat_file_t file;
    if (fat_open(&g_fat_fs, path, &file) != 0) {
        return THUMB_ERR_NOT_FOUND;
    }

    thumb->source_size = file.file_size;
    thumb->source_mtime = 0;
    bool is_dir = file.is_dir;
    fat_close(&file);

    // Handle directories
    if (is_dir) {
        return thumb_generate_folder(path, size, thumb);
    }

    // Check file size limit
    if (thumb->source_size > THUMB_MAX_SOURCE_SIZE) {
        return THUMB_ERR_TOO_LARGE;
    }

    // Detect type and generate
    thumb_type_t type = thumb_detect_type(path);

    switch (type) {
        case THUMB_TYPE_IMAGE:
            return thumb_generate_image(path, NULL, 0, size, thumb);

        case THUMB_TYPE_TEXT:
            return thumb_generate_text(path, NULL, 0, size, thumb);

        case THUMB_TYPE_AUDIO:
            return thumb_generate_audio(path, size, thumb);

        case THUMB_TYPE_VIDEO:
            return thumb_generate_video(path, size, thumb);

        case THUMB_TYPE_PDF:
            return thumb_generate_pdf(path, size, thumb);

        default:
            thumb->type = THUMB_TYPE_GENERIC;
            return THUMB_SUCCESS;
    }
}

int thumb_generate_async(const char *path, thumb_size_t size,
                         void (*callback)(thumbnail_t *thumb, void *user_data),
                         void *user_data) {
    if (!g_thumb_initialized || !path) return THUMB_ERR_NULL_PTR;

    // Find a free slot in the queue
    if (g_thumb_queue.count >= THUMB_QUEUE_SIZE) {
        return THUMB_ERR_QUEUE_FULL;
    }

    int slot = g_thumb_queue.tail;
    thumb_queue_entry_t *entry = &g_thumb_queue.entries[slot];

    strncpy(entry->path, path, THUMB_MAX_PATH - 1);
    entry->size = size;
    entry->callback = callback;
    entry->user_data = user_data;
    entry->in_progress = false;
    entry->cancelled = false;

    g_thumb_queue.tail = (g_thumb_queue.tail + 1) % THUMB_QUEUE_SIZE;
    g_thumb_queue.count++;

    return THUMB_SUCCESS;
}

void thumb_cancel_async(const char *path) {
    if (!g_thumb_initialized) return;

    for (int i = 0; i < THUMB_QUEUE_SIZE; i++) {
        thumb_queue_entry_t *entry = &g_thumb_queue.entries[i];
        if (path == NULL || strcmp(entry->path, path) == 0) {
            entry->cancelled = true;
        }
    }
}

int thumb_process_async(void) {
    if (!g_thumb_initialized || !g_thumb_queue.running || g_thumb_queue.count == 0) {
        return 0;
    }

    int processed = 0;

    // Process one entry per call to avoid blocking
    thumb_queue_entry_t *entry = &g_thumb_queue.entries[g_thumb_queue.head];

    if (!entry->in_progress && !entry->cancelled) {
        entry->in_progress = true;

        thumbnail_t *thumb = (thumbnail_t *)kmalloc(sizeof(thumbnail_t));
        if (thumb) {
            int result = thumb_generate(entry->path, entry->size, thumb);

            if (result == THUMB_SUCCESS && !entry->cancelled) {
                thumb_cache_add(thumb);
                if (entry->callback) {
                    entry->callback(thumb, entry->user_data);
                }
            } else {
                thumb_free(thumb);
                kfree(thumb);
                if (entry->callback) {
                    entry->callback(NULL, entry->user_data);
                }
            }
        }

        processed++;
    }

    // Remove from queue
    g_thumb_queue.head = (g_thumb_queue.head + 1) % THUMB_QUEUE_SIZE;
    g_thumb_queue.count--;

    return processed;
}

// ============================================
// Format-Specific Generators
// ============================================

int thumb_generate_image(const char *path, const void *data, uint32_t data_size,
                         thumb_size_t size, thumbnail_t *thumb) {
    if (!path || !thumb) return THUMB_ERR_NULL_PTR;

    void *file_data = NULL;
    uint32_t file_size = data_size;
    bool need_free = false;

    // Load file data if not provided
    if (!data) {
        file_data = fat_read_file(&g_fat_fs, path, &file_size);
        if (!file_data || file_size == 0) {
            return THUMB_ERR_IO;
        }
        need_free = true;
    } else {
        file_data = (void *)data;
    }

    // Try to load the image
    image_t img;
    int result = image_load(file_data, file_size, &img);

    if (need_free) {
        kfree(file_data);
    }

    if (result != IMAGE_SUCCESS) {
        return THUMB_ERR_CORRUPT;
    }

    // Validate dimensions
    if (img.width < THUMB_MIN_IMAGE_DIM || img.height < THUMB_MIN_IMAGE_DIM ||
        img.width > THUMB_MAX_IMAGE_DIM || img.height > THUMB_MAX_IMAGE_DIM) {
        image_free(&img);
        return THUMB_ERR_CORRUPT;
    }

    // Calculate scaled size preserving aspect ratio
    uint32_t target_size = thumb_get_pixel_size(size);
    uint32_t scaled_w, scaled_h;
    thumb_calculate_aspect_size(img.width, img.height, target_size, &scaled_w, &scaled_h);

    // Allocate thumbnail pixel buffer
    thumb->pixels = (uint32_t *)kmalloc(scaled_w * scaled_h * sizeof(uint32_t));
    if (!thumb->pixels) {
        image_free(&img);
        return THUMB_ERR_NOMEM;
    }

    // Scale the image
    thumb_scale_pixels(img.pixels, img.width, img.height,
                       thumb->pixels, scaled_w, scaled_h);

    thumb->type = THUMB_TYPE_IMAGE;
    thumb->width = scaled_w;
    thumb->height = scaled_h;
    thumb->cache_size = scaled_w * scaled_h * sizeof(uint32_t);
    thumb->last_access = timer_ticks;

    image_free(&img);
    return THUMB_SUCCESS;
}

int thumb_generate_text(const char *path, const void *data, uint32_t data_size,
                        thumb_size_t size, thumbnail_t *thumb) {
    if (!path || !thumb) return THUMB_ERR_NULL_PTR;

    void *file_data = NULL;
    uint32_t file_size = data_size;
    bool need_free = false;

    // Load file data if not provided
    if (!data) {
        file_data = fat_read_file(&g_fat_fs, path, &file_size);
        if (!file_data || file_size == 0) {
            thumb->type = THUMB_TYPE_ERROR;
            return THUMB_ERR_IO;
        }
        need_free = true;
    } else {
        file_data = (void *)data;
    }

    // Extract first few lines
    thumb->text_line_count = 0;
    char *p = (char *)file_data;
    char *end = p + file_size;
    char *line_start = p;
    int col = 0;

    while (p < end && thumb->text_line_count < THUMB_TEXT_MAX_LINES) {
        if (*p == '\n' || *p == '\r') {
            // Copy line
            int line_len = (col < THUMB_TEXT_LINE_WIDTH) ? col : THUMB_TEXT_LINE_WIDTH;
            if (line_len > 0) {
                memcpy(thumb->text_lines[thumb->text_line_count], line_start, line_len);
            }
            thumb->text_lines[thumb->text_line_count][line_len] = '\0';
            thumb->text_line_count++;

            // Skip CRLF
            if (p + 1 < end && ((*p == '\r' && *(p + 1) == '\n') ||
                               (*p == '\n' && *(p + 1) == '\r'))) {
                p++;
            }
            p++;
            line_start = p;
            col = 0;
        } else {
            col++;
            p++;
        }
    }

    // Handle last line without newline
    if (col > 0 && thumb->text_line_count < THUMB_TEXT_MAX_LINES) {
        int line_len = (col < THUMB_TEXT_LINE_WIDTH) ? col : THUMB_TEXT_LINE_WIDTH;
        memcpy(thumb->text_lines[thumb->text_line_count], line_start, line_len);
        thumb->text_lines[thumb->text_line_count][line_len] = '\0';
        thumb->text_line_count++;
    }

    if (need_free) {
        kfree(file_data);
    }

    thumb->type = THUMB_TYPE_TEXT;
    thumb->width = 0;
    thumb->height = 0;
    thumb->pixels = NULL;
    thumb->cache_size = sizeof(thumb->text_lines);
    thumb->last_access = timer_ticks;

    return THUMB_SUCCESS;
}

int thumb_generate_audio(const char *path, thumb_size_t size, thumbnail_t *thumb) {
    if (!path || !thumb) return THUMB_ERR_NULL_PTR;

    // For audio files, we generate a simple waveform placeholder
    // A full implementation would parse audio file headers and extract waveform data

    uint32_t target_size = thumb_get_pixel_size(size);
    thumb->width = target_size;
    thumb->height = THUMB_WAVEFORM_HEIGHT;

    thumb->pixels = (uint32_t *)kmalloc(thumb->width * thumb->height * sizeof(uint32_t));
    if (!thumb->pixels) {
        return THUMB_ERR_NOMEM;
    }

    // Fill with dark background
    uint32_t bg_color = 0xFF202020;  // Dark gray
    uint32_t wave_color = 0xFF40C040;  // Green
    uint32_t center_color = 0xFF808080;  // Gray center line

    for (uint32_t y = 0; y < thumb->height; y++) {
        for (uint32_t x = 0; x < thumb->width; x++) {
            thumb->pixels[y * thumb->width + x] = bg_color;
        }
    }

    // Draw center line
    for (uint32_t x = 0; x < thumb->width; x++) {
        thumb->pixels[(thumb->height / 2) * thumb->width + x] = center_color;
    }

    // Generate simple waveform pattern (placeholder)
    for (uint32_t x = 0; x < thumb->width; x++) {
        // Simple sine-like wave with some randomness
        int amplitude = (thumb->height / 2 - 2) * ((x * 17 + 13) % 100) / 100;
        int center = thumb->height / 2;

        for (int dy = -amplitude; dy <= amplitude; dy++) {
            int y = center + dy;
            if (y >= 0 && y < (int)thumb->height) {
                thumb->pixels[y * thumb->width + x] = wave_color;
            }
        }
    }

    thumb->type = THUMB_TYPE_AUDIO;
    thumb->cache_size = thumb->width * thumb->height * sizeof(uint32_t);
    thumb->last_access = timer_ticks;

    return THUMB_SUCCESS;
}

int thumb_generate_video(const char *path, thumb_size_t size, thumbnail_t *thumb) {
    if (!path || !thumb) return THUMB_ERR_NULL_PTR;

    // For video files, we generate a film strip placeholder
    // A full implementation would extract a frame from the video

    uint32_t target_size = thumb_get_pixel_size(size);
    thumb->width = target_size;
    thumb->height = target_size;

    thumb->pixels = (uint32_t *)kmalloc(thumb->width * thumb->height * sizeof(uint32_t));
    if (!thumb->pixels) {
        return THUMB_ERR_NOMEM;
    }

    // Draw film strip pattern
    uint32_t bg_color = 0xFF303030;
    uint32_t strip_color = 0xFF101010;
    uint32_t hole_color = 0xFFE0E0E0;
    uint32_t frame_color = 0xFF606060;

    // Fill background
    for (uint32_t y = 0; y < thumb->height; y++) {
        for (uint32_t x = 0; x < thumb->width; x++) {
            thumb->pixels[y * thumb->width + x] = bg_color;
        }
    }

    // Draw film strip borders
    uint32_t strip_h = thumb->height / 8;
    for (uint32_t x = 0; x < thumb->width; x++) {
        for (uint32_t y = 0; y < strip_h; y++) {
            thumb->pixels[y * thumb->width + x] = strip_color;
            thumb->pixels[(thumb->height - 1 - y) * thumb->width + x] = strip_color;
        }
    }

    // Draw sprocket holes
    uint32_t hole_w = thumb->width / 10;
    uint32_t hole_h = strip_h / 2;
    uint32_t hole_spacing = thumb->width / 4;

    for (uint32_t hx = hole_w; hx < thumb->width - hole_w; hx += hole_spacing) {
        for (uint32_t dx = 0; dx < hole_w; dx++) {
            for (uint32_t dy = 0; dy < hole_h; dy++) {
                // Top holes
                thumb->pixels[(strip_h / 4 + dy) * thumb->width + hx + dx] = hole_color;
                // Bottom holes
                thumb->pixels[(thumb->height - strip_h + strip_h / 4 + dy) * thumb->width + hx + dx] = hole_color;
            }
        }
    }

    // Draw play icon in center
    uint32_t cx = thumb->width / 2;
    uint32_t cy = thumb->height / 2;
    uint32_t tri_size = thumb->height / 4;

    for (uint32_t y = 0; y < tri_size; y++) {
        uint32_t width = y * 2 / 3;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t px = cx - tri_size / 3 + x;
            uint32_t py = cy - tri_size / 2 + y;
            if (px < thumb->width && py < thumb->height) {
                thumb->pixels[py * thumb->width + px] = frame_color;
            }
        }
    }

    thumb->type = THUMB_TYPE_VIDEO;
    thumb->cache_size = thumb->width * thumb->height * sizeof(uint32_t);
    thumb->last_access = timer_ticks;

    return THUMB_SUCCESS;
}

int thumb_generate_pdf(const char *path, thumb_size_t size, thumbnail_t *thumb) {
    if (!path || !thumb) return THUMB_ERR_NULL_PTR;

    // For PDF files, we generate a document icon placeholder
    // A full implementation would render the first page

    uint32_t target_size = thumb_get_pixel_size(size);
    thumb->width = target_size;
    thumb->height = target_size;

    thumb->pixels = (uint32_t *)kmalloc(thumb->width * thumb->height * sizeof(uint32_t));
    if (!thumb->pixels) {
        return THUMB_ERR_NOMEM;
    }

    // Draw document shape
    uint32_t bg_color = 0xFFFFFFFF;   // White
    uint32_t border_color = 0xFFD02020;  // Red (PDF color)
    uint32_t text_color = 0xFF808080;    // Gray for text lines

    // Fill background
    for (uint32_t y = 0; y < thumb->height; y++) {
        for (uint32_t x = 0; x < thumb->width; x++) {
            thumb->pixels[y * thumb->width + x] = bg_color;
        }
    }

    // Draw border
    uint32_t border = 2;
    for (uint32_t x = 0; x < thumb->width; x++) {
        for (uint32_t b = 0; b < border; b++) {
            thumb->pixels[b * thumb->width + x] = border_color;
            thumb->pixels[(thumb->height - 1 - b) * thumb->width + x] = border_color;
        }
    }
    for (uint32_t y = 0; y < thumb->height; y++) {
        for (uint32_t b = 0; b < border; b++) {
            thumb->pixels[y * thumb->width + b] = border_color;
            thumb->pixels[y * thumb->width + (thumb->width - 1 - b)] = border_color;
        }
    }

    // Draw text lines
    uint32_t margin = thumb->width / 8;
    uint32_t line_height = thumb->height / 12;
    uint32_t line_spacing = line_height * 2;

    for (uint32_t ly = margin + line_height; ly < thumb->height - margin; ly += line_spacing) {
        uint32_t line_width = thumb->width - (2 * margin);
        // Vary line lengths
        line_width = line_width * (80 + (ly % 20)) / 100;

        for (uint32_t lx = margin; lx < margin + line_width && lx < thumb->width - margin; lx++) {
            for (uint32_t lh = 0; lh < line_height / 2; lh++) {
                if (ly + lh < thumb->height) {
                    thumb->pixels[(ly + lh) * thumb->width + lx] = text_color;
                }
            }
        }
    }

    thumb->type = THUMB_TYPE_PDF;
    thumb->cache_size = thumb->width * thumb->height * sizeof(uint32_t);
    thumb->last_access = timer_ticks;

    return THUMB_SUCCESS;
}

int thumb_generate_folder(const char *path, thumb_size_t size, thumbnail_t *thumb) {
    if (!path || !thumb) return THUMB_ERR_NULL_PTR;

    // For folders, we could create a composite thumbnail of contained images
    // For now, just mark as folder type (will use icon)

    thumb->type = THUMB_TYPE_FOLDER;
    thumb->width = 0;
    thumb->height = 0;
    thumb->pixels = NULL;
    thumb->cache_size = 0;
    thumb->last_access = timer_ticks;

    return THUMB_SUCCESS;
}

// ============================================
// Utility Functions
// ============================================

int thumb_get_pixel_size(thumb_size_t size) {
    switch (size) {
        case THUMB_SMALL:  return THUMB_SIZE_SMALL;
        case THUMB_MEDIUM: return THUMB_SIZE_MEDIUM;
        case THUMB_LARGE:  return THUMB_SIZE_LARGE;
        default:           return THUMB_SIZE_DEFAULT;
    }
}

// Case-insensitive string comparison
static int strcasecmp_simple(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = *s1, c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

// Get file extension
static const char *get_extension(const char *path) {
    const char *ext = NULL;
    while (*path) {
        if (*path == '.') ext = path + 1;
        else if (*path == '/') ext = NULL;  // Reset on directory separator
        path++;
    }
    return ext;
}

thumb_type_t thumb_detect_type(const char *path) {
    if (!path) return THUMB_TYPE_NONE;

    const char *ext = get_extension(path);
    if (!ext) return THUMB_TYPE_GENERIC;

    // Check image formats
    for (int i = 0; thumb_image_formats[i]; i++) {
        if (strcasecmp_simple(ext, thumb_image_formats[i]) == 0) {
            return THUMB_TYPE_IMAGE;
        }
    }

    // Check text formats
    for (int i = 0; thumb_text_formats[i]; i++) {
        if (strcasecmp_simple(ext, thumb_text_formats[i]) == 0) {
            return THUMB_TYPE_TEXT;
        }
    }

    // Check audio formats
    for (int i = 0; thumb_audio_formats[i]; i++) {
        if (strcasecmp_simple(ext, thumb_audio_formats[i]) == 0) {
            return THUMB_TYPE_AUDIO;
        }
    }

    // Check video formats
    for (int i = 0; thumb_video_formats[i]; i++) {
        if (strcasecmp_simple(ext, thumb_video_formats[i]) == 0) {
            return THUMB_TYPE_VIDEO;
        }
    }

    // Check document formats (PDF)
    for (int i = 0; thumb_document_formats[i]; i++) {
        if (strcasecmp_simple(ext, thumb_document_formats[i]) == 0) {
            if (strcasecmp_simple(ext, "pdf") == 0) {
                return THUMB_TYPE_PDF;
            }
            return THUMB_TYPE_GENERIC;  // Other document types
        }
    }

    return THUMB_TYPE_GENERIC;
}

bool thumb_is_supported(const char *extension) {
    if (!extension) return false;

    // Check all format lists
    for (int i = 0; thumb_image_formats[i]; i++) {
        if (strcasecmp_simple(extension, thumb_image_formats[i]) == 0) return true;
    }
    for (int i = 0; thumb_text_formats[i]; i++) {
        if (strcasecmp_simple(extension, thumb_text_formats[i]) == 0) return true;
    }
    for (int i = 0; thumb_audio_formats[i]; i++) {
        if (strcasecmp_simple(extension, thumb_audio_formats[i]) == 0) return true;
    }
    for (int i = 0; thumb_video_formats[i]; i++) {
        if (strcasecmp_simple(extension, thumb_video_formats[i]) == 0) return true;
    }
    for (int i = 0; thumb_document_formats[i]; i++) {
        if (strcasecmp_simple(extension, thumb_document_formats[i]) == 0) return true;
    }

    return false;
}

void thumb_scale_pixels(const uint32_t *src, uint32_t src_w, uint32_t src_h,
                        uint32_t *dst, uint32_t dst_w, uint32_t dst_h) {
    if (!src || !dst || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return;

    // Nearest neighbor scaling with fixed-point arithmetic
    uint32_t x_ratio = ((src_w << 16) / dst_w);
    uint32_t y_ratio = ((src_h << 16) / dst_h);

    for (uint32_t y = 0; y < dst_h; y++) {
        uint32_t src_y = (y * y_ratio) >> 16;
        if (src_y >= src_h) src_y = src_h - 1;

        const uint32_t *src_row = src + src_y * src_w;
        uint32_t *dst_row = dst + y * dst_w;

        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t src_x = (x * x_ratio) >> 16;
            if (src_x >= src_w) src_x = src_w - 1;

            dst_row[x] = src_row[src_x];
        }
    }
}

void thumb_calculate_aspect_size(uint32_t src_w, uint32_t src_h, uint32_t max_size,
                                  uint32_t *out_w, uint32_t *out_h) {
    if (!out_w || !out_h) return;

    // If source fits within max_size, use original dimensions
    if (src_w <= max_size && src_h <= max_size) {
        *out_w = src_w;
        *out_h = src_h;
        return;
    }

    // Scale down preserving aspect ratio
    if (src_w > src_h) {
        // Landscape
        *out_w = max_size;
        *out_h = (src_h * max_size) / src_w;
    } else {
        // Portrait or square
        *out_h = max_size;
        *out_w = (src_w * max_size) / src_h;
    }

    // Ensure minimum size
    if (*out_w == 0) *out_w = 1;
    if (*out_h == 0) *out_h = 1;
}

void thumb_free(thumbnail_t *thumb) {
    if (!thumb) return;

    if (thumb->pixels) {
        kfree(thumb->pixels);
        thumb->pixels = NULL;
    }

    thumb->type = THUMB_TYPE_NONE;
    thumb->width = 0;
    thumb->height = 0;
    thumb->cache_size = 0;
}

void thumb_compute_hash(const char *path, uint32_t mtime, char *out_hash) {
    if (!path || !out_hash) return;

    // Simple hash computation (not cryptographically secure, just for cache keys)
    uint32_t h1 = 5381;
    uint32_t h2 = mtime;

    const char *p = path;
    while (*p) {
        h1 = ((h1 << 5) + h1) ^ (uint8_t)*p;
        h2 = ((h2 << 5) + h2) + (uint8_t)*p;
        p++;
    }

    // Convert to hex string
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out_hash[i] = hex[(h1 >> (28 - i * 4)) & 0xF];
        out_hash[8 + i] = hex[(h2 >> (28 - i * 4)) & 0xF];
    }
    out_hash[16] = '\0';
}

// ============================================
// Disk Cache Functions
// ============================================

int thumb_disk_cache_load(const char *hash, thumbnail_t *thumb) {
    if (!hash || !thumb || !g_fat_fs.mounted) return THUMB_ERR_NULL_PTR;

    char cache_path[THUMB_MAX_PATH];
    strcpy(cache_path, THUMB_CACHE_DIR);
    strcat(cache_path, hash);
    strcat(cache_path, ".thm");

    uint32_t size;
    void *data = fat_read_file(&g_fat_fs, cache_path, &size);
    if (!data || size < sizeof(uint32_t) * 4) {
        return THUMB_ERR_NOT_FOUND;
    }

    // Parse cached thumbnail
    uint32_t *header = (uint32_t *)data;
    thumb->type = (thumb_type_t)header[0];
    thumb->width = header[1];
    thumb->height = header[2];
    uint32_t pixel_size = header[3];

    if (pixel_size > 0 && size >= sizeof(uint32_t) * 4 + pixel_size) {
        thumb->pixels = (uint32_t *)kmalloc(pixel_size);
        if (thumb->pixels) {
            memcpy(thumb->pixels, (uint8_t *)data + sizeof(uint32_t) * 4, pixel_size);
            thumb->cache_size = pixel_size;
        }
    }

    kfree(data);
    thumb->cached_on_disk = true;
    return THUMB_SUCCESS;
}

int thumb_disk_cache_save(thumbnail_t *thumb) {
    if (!thumb || !g_fat_fs.mounted) return THUMB_ERR_NULL_PTR;

    char hash[THUMB_HASH_SIZE];
    thumb_compute_hash(thumb->source_path, thumb->source_mtime, hash);

    char cache_path[THUMB_MAX_PATH];
    strcpy(cache_path, THUMB_CACHE_DIR);
    strcat(cache_path, hash);
    strcat(cache_path, ".thm");

    // Build cache file data
    uint32_t pixel_size = (thumb->pixels) ? (thumb->width * thumb->height * sizeof(uint32_t)) : 0;
    uint32_t total_size = sizeof(uint32_t) * 4 + pixel_size;

    uint8_t *data = (uint8_t *)kmalloc(total_size);
    if (!data) return THUMB_ERR_NOMEM;

    uint32_t *header = (uint32_t *)data;
    header[0] = (uint32_t)thumb->type;
    header[1] = thumb->width;
    header[2] = thumb->height;
    header[3] = pixel_size;

    if (thumb->pixels && pixel_size > 0) {
        memcpy(data + sizeof(uint32_t) * 4, thumb->pixels, pixel_size);
    }

    int result = fat_write_file(&g_fat_fs, cache_path, data, total_size);
    kfree(data);

    if (result == 0) {
        thumb->cached_on_disk = true;
        return THUMB_SUCCESS;
    }

    return THUMB_ERR_IO;
}

int thumb_disk_cache_delete(const char *hash) {
    if (!hash || !g_fat_fs.mounted) return THUMB_ERR_NULL_PTR;

    char cache_path[THUMB_MAX_PATH];
    strcpy(cache_path, THUMB_CACHE_DIR);
    strcat(cache_path, hash);
    strcat(cache_path, ".thm");

    return (fat_delete(&g_fat_fs, cache_path) == 0) ? THUMB_SUCCESS : THUMB_ERR_NOT_FOUND;
}

int thumb_disk_cache_cleanup(uint32_t max_age) {
    // TODO: Implement disk cache cleanup
    // Would need to iterate through cache directory and delete old files
    (void)max_age;
    return 0;
}

uint64_t thumb_disk_cache_size(void) {
    // TODO: Implement disk cache size calculation
    return 0;
}

// ============================================
// Error Strings
// ============================================

const char *thumb_error_string(int err) {
    switch (err) {
        case THUMB_SUCCESS:        return "Success";
        case THUMB_ERR_NULL_PTR:   return "Null pointer";
        case THUMB_ERR_NOT_FOUND:  return "File not found";
        case THUMB_ERR_UNSUPPORTED: return "Unsupported format";
        case THUMB_ERR_NOMEM:      return "Out of memory";
        case THUMB_ERR_IO:         return "I/O error";
        case THUMB_ERR_CORRUPT:    return "Corrupted file";
        case THUMB_ERR_TOO_LARGE:  return "File too large";
        case THUMB_ERR_QUEUE_FULL: return "Queue full";
        case THUMB_ERR_CANCELLED:  return "Cancelled";
        default:                   return "Unknown error";
    }
}
