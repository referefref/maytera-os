// mediaplayer.c - Media Player Application for MayteraOS
// Supports audio (WAV, MP3, FLAC, OGG) and video (MPEG, AVI) playback

#include "mediaplayer.h"
#include "window.h"
#include "desktop.h"
#include "filedialog.h"
#include "icons.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../fs/fat.h"
#include "../drivers/audio.h"
#include "../drivers/sound.h"
#include "syslog.h"

// External filesystem
extern fat_fs_t g_fat_fs;

// Timer access (defined elsewhere)
extern uint64_t timer_get_ticks(void);

// ============================================================================
// UI Constants
// ============================================================================

#define MP_WINDOW_WIDTH     500
#define MP_WINDOW_HEIGHT    350
#define MP_VIDEO_MIN_H      200

// Audio mode layout
#define MP_INFO_HEIGHT      100
#define MP_ALBUMART_SIZE    80
#define MP_CONTROLS_HEIGHT  80
#define MP_PROGRESS_HEIGHT  12
#define MP_BUTTON_SIZE      32
#define MP_BUTTON_PADDING   8
#define MP_VOLUME_WIDTH     100
#define MP_VOLUME_HEIGHT    8

// Video mode overlay
#define MP_OVERLAY_HEIGHT   60
#define MP_OVERLAY_FADE_MS  3000

// ============================================================================
// Colors
// ============================================================================

#define MP_BG_COLOR             0x1E1E1E
#define MP_BG_DARK              0x141414
#define MP_TOOLBAR_BG           0x252525
#define MP_BUTTON_COLOR         0x404040
#define MP_BUTTON_HOVER         0x505050
#define MP_BUTTON_ACTIVE        0x0078D7
#define MP_PROGRESS_BG          0x404040
#define MP_PROGRESS_FG          0x0078D7
#define MP_PROGRESS_BUFFERED    0x606060
#define MP_PROGRESS_HANDLE      0xFFFFFF
#define MP_TEXT_COLOR           0xE0E0E0
#define MP_TEXT_DIM             0x808080
#define MP_TEXT_BRIGHT          0xFFFFFF
#define MP_VOLUME_BG            0x404040
#define MP_VOLUME_FG            0x00B050
#define MP_ALBUMART_BG          0x2A2A2A
#define MP_REPEAT_ONE_COLOR     0x0078D7
#define MP_REPEAT_ALL_COLOR     0x00AA00
#define MP_SHUFFLE_COLOR        0xFFA500
#define MP_OVERLAY_BG           0x80000000  // Semi-transparent black

// ============================================================================
// Forward Declarations
// ============================================================================

static void mp_load_directory(mediaplayer_t *mp);
static void mp_draw_audio_mode(mediaplayer_t *mp, int32_t wx, int32_t wy, int32_t ww, int32_t wh);
static void mp_draw_video_mode(mediaplayer_t *mp, int32_t wx, int32_t wy, int32_t ww, int32_t wh);
static void mp_draw_controls(mediaplayer_t *mp, int32_t x, int32_t y, int32_t w, bool overlay);
static void mp_draw_progress(mediaplayer_t *mp, int32_t x, int32_t y, int32_t w);
static void mp_draw_album_art(mediaplayer_t *mp, int32_t x, int32_t y, int32_t size);
static void mp_draw_info(mediaplayer_t *mp, int32_t x, int32_t y, int32_t w, int32_t h);
static bool mp_parse_wav(mediaplayer_t *mp, const uint8_t *data, uint32_t size);
static void mp_stream_audio(mediaplayer_t *mp);
static int mp_random_index(mediaplayer_t *mp);

// ============================================================================
// Helper Functions
// ============================================================================

// Case-insensitive string compare
static int mp_strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = *s1;
        char c2 = *s2;
        // Convert to lowercase
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

// ============================================================================
// Path and String Helpers
// ============================================================================

// Build full path from directory and filename
static void mp_build_path(char *dest, const char *dir, const char *file) {
    char *p = dest;
    const char *s = dir;
    while (*s) *p++ = *s++;
    if (p > dest && *(p - 1) != '/') *p++ = '/';
    s = file;
    while (*s) *p++ = *s++;
    *p = '\0';
}

// Simple integer to string conversion
static char *mp_itoa(int val, char *buf) {
    char *p = buf;
    if (val == 0) {
        *p++ = '0';
        return p;
    }
    if (val < 0) {
        *p++ = '-';
        val = -val;
    }
    char tmp[12];
    int i = 0;
    while (val > 0) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (i > 0) *p++ = tmp[--i];
    return p;
}

// Format time as MM:SS or HH:MM:SS
void mediaplayer_format_time(uint32_t ms, char *buf, bool include_hours) {
    uint32_t total_seconds = ms / 1000;
    uint32_t hours = total_seconds / 3600;
    uint32_t minutes = (total_seconds % 3600) / 60;
    uint32_t seconds = total_seconds % 60;

    char *p = buf;
    if (include_hours || hours > 0) {
        if (hours < 10) *p++ = '0';
        p = mp_itoa(hours, p);
        *p++ = ':';
    }
    if (minutes < 10) *p++ = '0';
    p = mp_itoa(minutes, p);
    *p++ = ':';
    if (seconds < 10) *p++ = '0';
    p = mp_itoa(seconds, p);
    *p = '\0';
}

// Draw text helper
static void mp_draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    for (int i = 0; text[i]; i++) {
        const uint8_t *glyph = font_get_glyph(text[i]);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    fb_put_pixel(x + i * 8 + col, y + row, color);
                }
            }
        }
    }
}

// Draw a filled circle
static void mp_draw_circle(int32_t cx, int32_t cy, int32_t r, uint32_t color) {
    for (int32_t dy = -r; dy <= r; dy++) {
        for (int32_t dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r * r) {
                fb_put_pixel(cx + dx, cy + dy, color);
            }
        }
    }
}

// Draw play icon (triangle)
static void mp_draw_play_icon(int32_t x, int32_t y, int32_t size, uint32_t color) {
    int32_t half = size / 2;
    for (int32_t row = 0; row < size; row++) {
        int32_t width = (row < half) ? (row * 2 / 3) : ((size - row - 1) * 2 / 3);
        for (int32_t col = 0; col <= width; col++) {
            fb_put_pixel(x + col, y + row, color);
        }
    }
}

// Draw pause icon (two bars)
static void mp_draw_pause_icon(int32_t x, int32_t y, int32_t size, uint32_t color) {
    int32_t bar_w = size / 4;
    int32_t gap = size / 4;
    fb_fill_rect(x, y, bar_w, size, color);
    fb_fill_rect(x + bar_w + gap, y, bar_w, size, color);
}

// Draw stop icon (square)
static void mp_draw_stop_icon(int32_t x, int32_t y, int32_t size, uint32_t color) {
    fb_fill_rect(x, y, size, size, color);
}

// Draw previous icon
static void mp_draw_prev_icon(int32_t x, int32_t y, int32_t size, uint32_t color) {
    int32_t half = size / 2;
    fb_fill_rect(x, y, 2, size, color);
    for (int32_t row = 0; row < size; row++) {
        int32_t width = (row < half) ? (half - row) : (row - half + 1);
        for (int32_t col = 0; col < width; col++) {
            fb_put_pixel(x + 4 + size - 1 - col, y + row, color);
        }
    }
}

// Draw next icon
static void mp_draw_next_icon(int32_t x, int32_t y, int32_t size, uint32_t color) {
    int32_t half = size / 2;
    for (int32_t row = 0; row < size; row++) {
        int32_t width = (row < half) ? (row + 1) : (size - row);
        for (int32_t col = 0; col < width; col++) {
            fb_put_pixel(x + col, y + row, color);
        }
    }
    fb_fill_rect(x + size + 2, y, 2, size, color);
}

// Draw speaker/volume icon
static void mp_draw_volume_icon(int32_t x, int32_t y, int32_t size, uint32_t color, bool muted) {
    // Speaker cone
    int32_t cone_w = size / 3;
    int32_t cone_h = size / 2;
    fb_fill_rect(x, y + size / 4, cone_w, cone_h, color);

    // Speaker horn
    for (int32_t row = 0; row < size; row++) {
        int32_t start_x = x + cone_w;
        int32_t width = (row < size / 2) ? (size / 2 - row) / 2 : (row - size / 2) / 2;
        fb_fill_rect(start_x, y + row, size / 4 + width, 1, color);
    }

    // Mute X if muted
    if (muted) {
        uint32_t mute_color = 0xFF0000;
        for (int i = 0; i < size / 2; i++) {
            fb_put_pixel(x + size - i, y + i * 2, mute_color);
            fb_put_pixel(x + size - i, y + size - i * 2 - 1, mute_color);
        }
    }
}

// Draw repeat icon
static void mp_draw_repeat_icon(int32_t x, int32_t y, int32_t size, uint32_t color, mp_repeat_mode_t mode) {
    // Draw circular arrows
    int32_t r = size / 2 - 2;
    int32_t cx = x + size / 2;
    int32_t cy = y + size / 2;

    // Top arc
    for (int i = -r; i <= r; i++) {
        fb_put_pixel(cx + i, cy - r + 1, color);
    }
    // Bottom arc
    for (int i = -r; i <= r; i++) {
        fb_put_pixel(cx + i, cy + r - 1, color);
    }
    // Arrows
    fb_put_pixel(cx + r - 1, cy - r, color);
    fb_put_pixel(cx + r - 2, cy - r - 1, color);
    fb_put_pixel(cx - r + 1, cy + r, color);
    fb_put_pixel(cx - r + 2, cy + r + 1, color);

    // "1" indicator for repeat one
    if (mode == MP_REPEAT_ONE) {
        mp_draw_text(x + size - 6, y + size - 10, "1", color);
    }
}

// Draw shuffle icon
static void mp_draw_shuffle_icon(int32_t x, int32_t y, int32_t size, uint32_t color) {
    // Crossing arrows
    int32_t margin = 2;
    // Line 1: bottom-left to top-right
    for (int i = 0; i < size - margin * 2; i++) {
        fb_put_pixel(x + margin + i, y + size - margin - i - 1, color);
    }
    // Line 2: top-left to bottom-right
    for (int i = 0; i < size - margin * 2; i++) {
        fb_put_pixel(x + margin + i, y + margin + i, color);
    }
    // Arrow heads
    fb_put_pixel(x + size - margin - 2, y + margin, color);
    fb_put_pixel(x + size - margin - 1, y + margin + 1, color);
    fb_put_pixel(x + size - margin - 2, y + size - margin - 1, color);
    fb_put_pixel(x + size - margin - 1, y + size - margin - 2, color);
}

// Draw fullscreen icon
static void mp_draw_fullscreen_icon(int32_t x, int32_t y, int32_t size, uint32_t color) {
    int32_t corner = size / 3;
    // Top-left corner
    fb_fill_rect(x, y, corner, 2, color);
    fb_fill_rect(x, y, 2, corner, color);
    // Top-right corner
    fb_fill_rect(x + size - corner, y, corner, 2, color);
    fb_fill_rect(x + size - 2, y, 2, corner, color);
    // Bottom-left corner
    fb_fill_rect(x, y + size - 2, corner, 2, color);
    fb_fill_rect(x, y + size - corner, 2, corner, color);
    // Bottom-right corner
    fb_fill_rect(x + size - corner, y + size - 2, corner, 2, color);
    fb_fill_rect(x + size - 2, y + size - corner, 2, corner, color);
}

// Draw playlist icon
static void mp_draw_playlist_icon(int32_t x, int32_t y, int32_t size, uint32_t color) {
    int32_t line_h = size / 5;
    for (int i = 0; i < 3; i++) {
        fb_fill_rect(x, y + i * (line_h + 2), size * 2 / 3, line_h, color);
    }
    // Note symbol on right
    int32_t note_x = x + size - 4;
    fb_fill_rect(note_x, y + 2, 2, size - 6, color);
    mp_draw_circle(note_x, y + size - 4, 3, color);
}

// ============================================================================
// Format Detection
// ============================================================================

media_format_t mediaplayer_detect_format(const uint8_t *data, uint32_t size, const char *filename) {
    if (!data || size < 12) {
        // Try by extension
        if (filename) {
            int len = strlen(filename);
            if (len >= 4) {
                const char *ext = filename + len - 4;
                if (mp_strcasecmp(ext, ".wav") == 0) return MEDIA_FORMAT_WAV;
                if (mp_strcasecmp(ext, ".mp3") == 0) return MEDIA_FORMAT_MP3;
                if (mp_strcasecmp(ext, ".ogg") == 0) return MEDIA_FORMAT_OGG;
                if (mp_strcasecmp(ext, ".avi") == 0) return MEDIA_FORMAT_AVI;
                if (mp_strcasecmp(ext, ".mpg") == 0) return MEDIA_FORMAT_MPEG;
            }
            if (len >= 5) {
                const char *ext = filename + len - 5;
                if (mp_strcasecmp(ext, ".flac") == 0) return MEDIA_FORMAT_FLAC;
                if (mp_strcasecmp(ext, ".mpeg") == 0) return MEDIA_FORMAT_MPEG;
            }
        }
        return MEDIA_FORMAT_UNKNOWN;
    }

    // Check magic bytes
    // WAV: RIFF....WAVE
    if (data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'A' && data[10] == 'V' && data[11] == 'E') {
        return MEDIA_FORMAT_WAV;
    }

    // MP3: ID3 tag or frame sync (0xFF 0xFB/0xFA/0xF3/0xF2)
    if (data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        return MEDIA_FORMAT_MP3;
    }
    if (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0) {
        return MEDIA_FORMAT_MP3;
    }

    // FLAC: fLaC
    if (data[0] == 'f' && data[1] == 'L' && data[2] == 'a' && data[3] == 'C') {
        return MEDIA_FORMAT_FLAC;
    }

    // OGG: OggS
    if (data[0] == 'O' && data[1] == 'g' && data[2] == 'g' && data[3] == 'S') {
        return MEDIA_FORMAT_OGG;
    }

    // AVI: RIFF....AVI
    if (data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'A' && data[9] == 'V' && data[10] == 'I' && data[11] == ' ') {
        return MEDIA_FORMAT_AVI;
    }

    // MPEG: 0x000001Bx (video) or 0x000001BA (pack start)
    if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01 &&
        (data[3] == 0xBA || data[3] == 0xB3)) {
        return MEDIA_FORMAT_MPEG;
    }

    return MEDIA_FORMAT_UNKNOWN;
}

const char *mediaplayer_format_name(media_format_t format) {
    switch (format) {
        case MEDIA_FORMAT_WAV:  return "WAV";
        case MEDIA_FORMAT_MP3:  return "MP3";
        case MEDIA_FORMAT_FLAC: return "FLAC";
        case MEDIA_FORMAT_OGG:  return "OGG";
        case MEDIA_FORMAT_MPEG: return "MPEG";
        case MEDIA_FORMAT_AVI:  return "AVI";
        default:                return "Unknown";
    }
}

bool mediaplayer_is_audio_file(const char *filename) {
    if (!filename) return false;
    int len = strlen(filename);
    if (len < 4) return false;

    const char *ext = filename + len - 4;
    if (mp_strcasecmp(ext, ".wav") == 0 ||
        mp_strcasecmp(ext, ".mp3") == 0 ||
        mp_strcasecmp(ext, ".ogg") == 0) {
        return true;
    }
    if (len >= 5) {
        ext = filename + len - 5;
        if (mp_strcasecmp(ext, ".flac") == 0) return true;
    }
    return false;
}

bool mediaplayer_is_video_file(const char *filename) {
    if (!filename) return false;
    int len = strlen(filename);
    if (len < 4) return false;

    const char *ext = filename + len - 4;
    if (mp_strcasecmp(ext, ".avi") == 0 ||
        mp_strcasecmp(ext, ".mpg") == 0) {
        return true;
    }
    if (len >= 5) {
        ext = filename + len - 5;
        if (mp_strcasecmp(ext, ".mpeg") == 0) return true;
    }
    return false;
}

bool mediaplayer_is_playlist_file(const char *filename) {
    if (!filename) return false;
    int len = strlen(filename);
    if (len < 4) return false;

    const char *ext = filename + len - 4;
    return (mp_strcasecmp(ext, ".m3u") == 0);
}

bool mediaplayer_is_supported(const char *filename) {
    return mediaplayer_is_audio_file(filename) ||
           mediaplayer_is_video_file(filename) ||
           mediaplayer_is_playlist_file(filename);
}

// ============================================================================
// WAV Parsing (Native Support)
// ============================================================================

static bool mp_parse_wav(mediaplayer_t *mp, const uint8_t *data, uint32_t size) {
    if (size < sizeof(wav_riff_header_t) + sizeof(wav_fmt_chunk_t) + sizeof(wav_data_chunk_t)) {
        kprintf("[MediaPlayer] WAV file too small\n");
        return false;
    }

    wav_riff_header_t *riff = (wav_riff_header_t *)data;
    if (riff->chunk_id != WAV_RIFF_MAGIC || riff->format != WAV_WAVE_MAGIC) {
        kprintf("[MediaPlayer] Invalid WAV header\n");
        return false;
    }

    // Find chunks
    uint32_t offset = sizeof(wav_riff_header_t);
    wav_fmt_chunk_t *fmt = NULL;
    wav_data_chunk_t *data_chunk = NULL;
    uint32_t data_offset = 0;

    while (offset + 8 < size) {
        uint32_t chunk_id = *(uint32_t *)(data + offset);
        uint32_t chunk_size = *(uint32_t *)(data + offset + 4);

        if (chunk_id == WAV_FMT_MAGIC) {
            fmt = (wav_fmt_chunk_t *)(data + offset);
        } else if (chunk_id == WAV_DATA_MAGIC) {
            data_chunk = (wav_data_chunk_t *)(data + offset);
            data_offset = offset + sizeof(wav_data_chunk_t);
        }

        offset += 8 + chunk_size;
        if (chunk_size & 1) offset++;
    }

    if (!fmt || !data_chunk) {
        kprintf("[MediaPlayer] Missing WAV chunks\n");
        return false;
    }

    if (fmt->audio_format != WAV_FORMAT_PCM) {
        kprintf("[MediaPlayer] Unsupported WAV format: %u\n", fmt->audio_format);
        return false;
    }

    // Store metadata
    mp->metadata.sample_rate = fmt->sample_rate;
    mp->metadata.channels = fmt->num_channels;
    mp->metadata.bits_per_sample = fmt->bits_per_sample;
    mp->metadata.bitrate = fmt->byte_rate * 8 / 1000;

    // Calculate duration
    if (fmt->byte_rate > 0) {
        mp->metadata.duration_ms = (uint32_t)(((uint64_t)data_chunk->subchunk_size * 1000) / fmt->byte_rate);
    }

    // Set up audio buffer pointing to PCM data
    mp->audio_buffer = (int16_t *)(mp->file_data + data_offset);
    mp->audio_buffer_size = data_chunk->subchunk_size;
    mp->audio_buffer_pos = 0;

    kprintf("[MediaPlayer] WAV: %u Hz, %u-bit, %u ch, %u ms\n",
            mp->metadata.sample_rate, mp->metadata.bits_per_sample,
            mp->metadata.channels, mp->metadata.duration_ms);

    return true;
}

// ============================================================================
// Playlist Management
// ============================================================================

bool mediaplayer_load_playlist(mediaplayer_t *mp, const char *path) {
    if (!mp || !path) return false;

    kprintf("[MediaPlayer] Loading playlist: %s\n", path);

    // Read playlist file
    uint32_t file_size = 0;
    char *file_data = (char *)fat_read_file(&g_fat_fs, path, &file_size);
    if (!file_data) {
        LOG_ERROR("[MediaPlayer] Failed to read playlist file");
        return false;
    }

    // Clear existing playlist
    mediaplayer_playlist_clear(mp);

    // Extract directory from playlist path
    char playlist_dir[MP_MAX_PATH];
    strncpy(playlist_dir, path, MP_MAX_PATH - 1);
    playlist_dir[MP_MAX_PATH - 1] = '\0';
    for (int i = strlen(playlist_dir) - 1; i >= 0; i--) {
        if (playlist_dir[i] == '/') {
            playlist_dir[i] = '\0';
            break;
        }
    }

    // Parse M3U format
    char *line_start = file_data;
    char *end = file_data + file_size;

    while (line_start < end && mp->playlist_count < MP_MAX_PLAYLIST) {
        // Find end of line
        char *line_end = line_start;
        while (line_end < end && *line_end != '\n' && *line_end != '\r') {
            line_end++;
        }

        // Process line (skip empty lines and comments)
        if (line_end > line_start && *line_start != '#') {
            // Null-terminate line temporarily
            char saved = *line_end;
            *line_end = '\0';

            // Build full path if relative
            char full_path[MP_MAX_PATH];
            if (line_start[0] == '/') {
                strncpy(full_path, line_start, MP_MAX_PATH - 1);
            } else {
                mp_build_path(full_path, playlist_dir, line_start);
            }
            full_path[MP_MAX_PATH - 1] = '\0';

            // Add to playlist
            mediaplayer_playlist_add(mp, full_path);

            *line_end = saved;
        }

        // Move to next line
        line_start = line_end;
        while (line_start < end && (*line_start == '\n' || *line_start == '\r')) {
            line_start++;
        }
    }

    kfree(file_data);

    // Store playlist path
    strncpy(mp->playlist_path, path, MP_MAX_PATH - 1);
    mp->playlist_path[MP_MAX_PATH - 1] = '\0';

    kprintf("[MediaPlayer] Loaded %d playlist entries\n", mp->playlist_count);
    return mp->playlist_count > 0;
}

bool mediaplayer_save_playlist(mediaplayer_t *mp, const char *path) {
    if (!mp || !path || mp->playlist_count == 0) return false;

    // Build M3U content
    char *content = (char *)kmalloc(mp->playlist_count * MP_MAX_PATH + 256);
    if (!content) return false;

    char *p = content;

    // M3U header
    const char *header = "#EXTM3U\n";
    while (*header) *p++ = *header++;

    // Add entries
    for (int i = 0; i < mp->playlist_count; i++) {
        playlist_entry_t *entry = &mp->playlist[i];

        // Extended info line
        const char *extinf = "#EXTINF:";
        while (*extinf) *p++ = *extinf++;
        p = mp_itoa(entry->duration_ms / 1000, p);
        *p++ = ',';
        const char *title = entry->title[0] ? entry->title : entry->path;
        while (*title) *p++ = *title++;
        *p++ = '\n';

        // Path line
        const char *path_str = entry->path;
        while (*path_str) *p++ = *path_str++;
        *p++ = '\n';
    }
    *p = '\0';

    // Write file
    uint32_t content_size = p - content;
    int result = fat_write_file(&g_fat_fs, path, content, content_size);
    kfree(content);

    if (result != 0) {
        LOG_ERROR("[MediaPlayer] Failed to save playlist");
        return false;
    }

    kprintf("[MediaPlayer] Saved playlist: %s\n", path);
    return true;
}

int mediaplayer_playlist_add(mediaplayer_t *mp, const char *path) {
    if (!mp || !path || mp->playlist_count >= MP_MAX_PLAYLIST) return -1;

    playlist_entry_t *entry = &mp->playlist[mp->playlist_count];
    memset(entry, 0, sizeof(playlist_entry_t));

    strncpy(entry->path, path, MP_MAX_PATH - 1);
    entry->path[MP_MAX_PATH - 1] = '\0';

    // Extract filename as default title
    const char *filename = path;
    for (int i = strlen(path) - 1; i >= 0; i--) {
        if (path[i] == '/') {
            filename = &path[i + 1];
            break;
        }
    }
    strncpy(entry->title, filename, MP_MAX_TITLE - 1);
    entry->title[MP_MAX_TITLE - 1] = '\0';

    return mp->playlist_count++;
}

void mediaplayer_playlist_remove(mediaplayer_t *mp, int index) {
    if (!mp || index < 0 || index >= mp->playlist_count) return;

    // Shift entries down
    for (int i = index; i < mp->playlist_count - 1; i++) {
        mp->playlist[i] = mp->playlist[i + 1];
    }
    mp->playlist_count--;

    // Adjust current index if needed
    if (mp->playlist_index > index) {
        mp->playlist_index--;
    } else if (mp->playlist_index >= mp->playlist_count) {
        mp->playlist_index = mp->playlist_count - 1;
    }
}

void mediaplayer_playlist_clear(mediaplayer_t *mp) {
    if (!mp) return;
    mp->playlist_count = 0;
    mp->playlist_index = -1;
    memset(mp->playlist, 0, sizeof(mp->playlist));
}

void mediaplayer_playlist_goto(mediaplayer_t *mp, int index) {
    if (!mp || index < 0 || index >= mp->playlist_count) return;

    mp->playlist_index = index;
    mediaplayer_open(mp, mp->playlist[index].path);
}

// Simple pseudo-random for shuffle
static int mp_random_index(mediaplayer_t *mp) {
    static uint32_t seed = 12345;
    seed = seed * 1103515245 + 12345;
    return (seed >> 16) % mp->playlist_count;
}

// ============================================================================
// Directory Loading
// ============================================================================

static void mp_load_directory(mediaplayer_t *mp) {
    mp->file_count = 0;
    mp->current_index = -1;

    if (strlen(mp->directory) == 0) return;

    fat_file_t dir;
    if (fat_open(&g_fat_fs, mp->directory, &dir) != 0) return;
    if (!dir.is_dir) {
        fat_close(&dir);
        return;
    }

    fat_dir_entry_t entry;
    char name[256];

    while (fat_readdir(&dir, &entry, name) == 0 && mp->file_count < MP_MAX_FILES) {
        if (entry.attr & (FAT_ATTR_DIRECTORY | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM)) continue;

        if (mediaplayer_is_supported(name)) {
            strncpy(mp->files[mp->file_count], name, 63);
            mp->files[mp->file_count][63] = '\0';
            mp->file_count++;
        }
    }

    fat_close(&dir);

    // Find current file index
    if (strlen(mp->filename) > 0) {
        for (int i = 0; i < mp->file_count; i++) {
            if (strcmp(mp->files[i], mp->filename) == 0) {
                mp->current_index = i;
                break;
            }
        }
    }
}

// ============================================================================
// Core Functions
// ============================================================================

mediaplayer_t *mediaplayer_create(void) {
    mediaplayer_t *mp = (mediaplayer_t *)kmalloc(sizeof(mediaplayer_t));
    if (!mp) return NULL;

    memset(mp, 0, sizeof(mediaplayer_t));

    // Create window
    mp->window = window_create("Media Player", 100, 100, MP_WINDOW_WIDTH, MP_WINDOW_HEIGHT);
    if (!mp->window) {
        kfree(mp);
        return NULL;
    }

    // Initialize state
    mp->state = MP_STATE_STOPPED;
    mp->volume = 192;  // 75% default
    mp->muted = false;
    mp->repeat_mode = MP_REPEAT_NONE;
    mp->shuffle = false;
    mp->has_media = false;
    mp->fullscreen = false;
    mp->controls_visible = true;
    mp->playlist_index = -1;

    return mp;
}

void mediaplayer_destroy(mediaplayer_t *mp) {
    if (!mp) return;

    mediaplayer_stop(mp);
    mediaplayer_close(mp);

    if (mp->window) {
        window_destroy(mp->window);
    }
    kfree(mp);
}

void mediaplayer_close(mediaplayer_t *mp) {
    if (!mp) return;

    if (mp->file_data) {
        kfree(mp->file_data);
        mp->file_data = NULL;
    }

    if (mp->metadata.album_art) {
        image_free(mp->metadata.album_art);
        kfree(mp->metadata.album_art);
        mp->metadata.album_art = NULL;
    }

    if (mp->current_frame.pixels) {
        kfree(mp->current_frame.pixels);
        mp->current_frame.pixels = NULL;
    }

    mp->has_media = false;
    mp->audio_buffer = NULL;
    mp->audio_buffer_size = 0;
    memset(&mp->metadata, 0, sizeof(media_metadata_t));
}

bool mediaplayer_open(mediaplayer_t *mp, const char *path) {
    if (!mp || !path) return false;

    kprintf("[MediaPlayer] Opening: %s\n", path);

    // Close existing media
    mediaplayer_stop(mp);
    mediaplayer_close(mp);

    // Check for playlist file
    if (mediaplayer_is_playlist_file(path)) {
        if (mediaplayer_load_playlist(mp, path)) {
            if (mp->playlist_count > 0) {
                mp->playlist_index = 0;
                return mediaplayer_open(mp, mp->playlist[0].path);
            }
        }
        return false;
    }

    // Read file
    mp->file_data = (uint8_t *)fat_read_file(&g_fat_fs, path, &mp->file_size);
    if (!mp->file_data) {
        LOG_ERROR("[MediaPlayer] Failed to read file");
        kprintf("[MediaPlayer] Failed to read: %s\n", path);
        return false;
    }

    // Detect format
    mp->format = mediaplayer_detect_format(mp->file_data, mp->file_size, path);
    if (mp->format == MEDIA_FORMAT_UNKNOWN) {
        kfree(mp->file_data);
        mp->file_data = NULL;
        LOG_ERROR("[MediaPlayer] Unknown media format");
        return false;
    }

    // Determine media type
    switch (mp->format) {
        case MEDIA_FORMAT_WAV:
        case MEDIA_FORMAT_MP3:
        case MEDIA_FORMAT_FLAC:
        case MEDIA_FORMAT_OGG:
            mp->media_type = MEDIA_TYPE_AUDIO;
            break;
        case MEDIA_FORMAT_MPEG:
        case MEDIA_FORMAT_AVI:
            mp->media_type = MEDIA_TYPE_VIDEO;
            break;
        default:
            mp->media_type = MEDIA_TYPE_NONE;
            break;
    }

    // Parse format-specific data
    bool parsed = false;
    switch (mp->format) {
        case MEDIA_FORMAT_WAV:
            parsed = mp_parse_wav(mp, mp->file_data, mp->file_size);
            break;

        case MEDIA_FORMAT_MP3:
            // TODO: MP3 decoder integration
            kprintf("[MediaPlayer] MP3 format (decoder not implemented)\n");
            mp->metadata.duration_ms = 0;
            parsed = true;  // Allow opening even without decoder
            break;

        case MEDIA_FORMAT_FLAC:
            // TODO: FLAC decoder integration
            kprintf("[MediaPlayer] FLAC format (decoder not implemented)\n");
            parsed = true;
            break;

        case MEDIA_FORMAT_OGG:
            // TODO: OGG Vorbis decoder integration
            kprintf("[MediaPlayer] OGG format (decoder not implemented)\n");
            parsed = true;
            break;

        case MEDIA_FORMAT_MPEG:
        case MEDIA_FORMAT_AVI:
            // TODO: Video decoder integration
            kprintf("[MediaPlayer] Video format (decoder not implemented)\n");
            parsed = true;
            break;

        default:
            break;
    }

    if (!parsed) {
        kfree(mp->file_data);
        mp->file_data = NULL;
        return false;
    }

    // Store path info
    strncpy(mp->filepath, path, MP_MAX_PATH - 1);
    mp->filepath[MP_MAX_PATH - 1] = '\0';

    // Extract filename
    const char *filename = path;
    for (int i = strlen(path) - 1; i >= 0; i--) {
        if (path[i] == '/') {
            filename = &path[i + 1];
            break;
        }
    }
    strncpy(mp->filename, filename, 63);
    mp->filename[63] = '\0';

    // Use filename as title if no metadata
    if (mp->metadata.title[0] == '\0') {
        strncpy(mp->metadata.title, mp->filename, MP_MAX_TITLE - 1);
        // Remove extension
        int len = strlen(mp->metadata.title);
        for (int i = len - 1; i >= 0 && i >= len - 5; i--) {
            if (mp->metadata.title[i] == '.') {
                mp->metadata.title[i] = '\0';
                break;
            }
        }
    }

    // Extract directory
    strcpy(mp->directory, path);
    for (int i = strlen(mp->directory) - 1; i >= 0; i--) {
        if (mp->directory[i] == '/') {
            mp->directory[i] = '\0';
            break;
        }
    }
    if (strlen(mp->directory) == 0) {
        strcpy(mp->directory, "/");
    }

    // Load directory for navigation
    mp_load_directory(mp);

    // Update window title
    char title[128];
    snprintf(title, sizeof(title), "Media Player - %s", mp->filename);
    window_set_title(mp->window, title);

    mp->has_media = true;
    mp->position_ms = 0;

    kprintf("[MediaPlayer] Loaded: %s (%s)\n", mp->filename, mediaplayer_format_name(mp->format));
    return true;
}

// ============================================================================
// Playback Control
// ============================================================================

void mediaplayer_play(mediaplayer_t *mp) {
    if (!mp || !mp->has_media) {
        LOG_WARNING("[MediaPlayer] Cannot play: no media loaded");
        return;
    }

    if (mp->state == MP_STATE_PLAYING) return;

    // Check audio availability
    if (mp->media_type == MEDIA_TYPE_AUDIO || mp->media_type == MEDIA_TYPE_VIDEO) {
        if (!audio_is_available() && !sound_is_detected()) {
            LOG_ERROR("[MediaPlayer] No audio device available");
            kprintf("[MediaPlayer] No audio device\n");
            return;
        }
    }

    LOG_INFO("[MediaPlayer] Starting playback");

    // Set volume
    if (!mp->muted) {
        if (audio_is_available()) {
            audio_set_master_volume(mp->volume * 100 / 255);
        } else {
            sound_set_volume(mp->volume);
        }
    }

    // Start playback based on format
    if (mp->format == MEDIA_FORMAT_WAV && mp->audio_buffer) {
        mp_stream_audio(mp);
    }

    mp->state = MP_STATE_PLAYING;
    kprintf("[MediaPlayer] Playing\n");
}

static void mp_stream_audio(mediaplayer_t *mp) {
    if (!mp->audio_buffer || mp->audio_buffer_pos >= mp->audio_buffer_size) return;

    uint32_t remaining = mp->audio_buffer_size - mp->audio_buffer_pos;
    uint32_t chunk_size = remaining;
    if (chunk_size > 32768) chunk_size = 32768;

    uint8_t *play_data = (uint8_t *)mp->audio_buffer + mp->audio_buffer_pos;

    if (mp->metadata.bits_per_sample == 8) {
        sound_play_buffer(play_data, chunk_size, mp->metadata.sample_rate);
    } else if (mp->metadata.bits_per_sample == 16) {
        // Convert 16-bit to 8-bit for SB16 compatibility
        uint32_t samples = chunk_size / 2;
        uint8_t *temp = (uint8_t *)kmalloc(samples);
        if (temp) {
            int16_t *src = (int16_t *)play_data;
            for (uint32_t i = 0; i < samples; i++) {
                temp[i] = (uint8_t)((src[i] + 32768) >> 8);
            }
            sound_play_buffer(temp, samples, mp->metadata.sample_rate);
            kfree(temp);
        }
    }
}

void mediaplayer_pause(mediaplayer_t *mp) {
    if (!mp || mp->state != MP_STATE_PLAYING) return;

    if (audio_is_available()) {
        // Use audio subsystem pause if available
    }
    sound_stop();

    mp->state = MP_STATE_PAUSED;
    kprintf("[MediaPlayer] Paused\n");
}

void mediaplayer_stop(mediaplayer_t *mp) {
    if (!mp) return;

    sound_stop();
    mp->state = MP_STATE_STOPPED;
    mp->position_ms = 0;
    mp->audio_buffer_pos = 0;
    kprintf("[MediaPlayer] Stopped\n");
}

void mediaplayer_toggle_play(mediaplayer_t *mp) {
    if (!mp) return;

    if (mp->state == MP_STATE_PLAYING) {
        mediaplayer_pause(mp);
    } else {
        mediaplayer_play(mp);
    }
}

void mediaplayer_seek(mediaplayer_t *mp, int percent) {
    if (!mp || !mp->has_media) return;

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    uint32_t new_pos_ms = (mp->metadata.duration_ms * percent) / 100;
    mediaplayer_seek_ms(mp, new_pos_ms);
}

void mediaplayer_seek_ms(mediaplayer_t *mp, uint32_t position_ms) {
    if (!mp || !mp->has_media) return;

    if (position_ms > mp->metadata.duration_ms) {
        position_ms = mp->metadata.duration_ms;
    }

    mp->position_ms = position_ms;

    // Calculate byte position for WAV
    if (mp->format == MEDIA_FORMAT_WAV && mp->audio_buffer_size > 0) {
        uint32_t bytes_per_ms = (mp->metadata.sample_rate * mp->metadata.channels *
                                 (mp->metadata.bits_per_sample / 8)) / 1000;
        mp->audio_buffer_pos = position_ms * bytes_per_ms;

        // Align to sample boundary
        uint32_t bytes_per_sample = mp->metadata.channels * (mp->metadata.bits_per_sample / 8);
        mp->audio_buffer_pos = (mp->audio_buffer_pos / bytes_per_sample) * bytes_per_sample;

        if (mp->audio_buffer_pos >= mp->audio_buffer_size) {
            mp->audio_buffer_pos = mp->audio_buffer_size;
        }
    }

    kprintf("[MediaPlayer] Seek to %u ms\n", position_ms);

    // Restart playback if playing
    if (mp->state == MP_STATE_PLAYING) {
        mediaplayer_pause(mp);
        mediaplayer_play(mp);
    }
}

void mediaplayer_next(mediaplayer_t *mp) {
    if (!mp) return;

    // Check playlist first
    if (mp->playlist_count > 0) {
        if (mp->shuffle) {
            mp->playlist_index = mp_random_index(mp);
        } else {
            mp->playlist_index++;
            if (mp->playlist_index >= mp->playlist_count) {
                if (mp->repeat_mode == MP_REPEAT_ALL) {
                    mp->playlist_index = 0;
                } else {
                    mp->playlist_index = mp->playlist_count - 1;
                    return;
                }
            }
        }
        mediaplayer_open(mp, mp->playlist[mp->playlist_index].path);
        if (mp->state == MP_STATE_PLAYING) {
            mediaplayer_play(mp);
        }
        return;
    }

    // Use directory listing
    if (mp->file_count == 0) return;

    mp->current_index++;
    if (mp->current_index >= mp->file_count) {
        if (mp->repeat_mode == MP_REPEAT_ALL) {
            mp->current_index = 0;
        } else {
            mp->current_index = mp->file_count - 1;
            return;
        }
    }

    char path[MP_MAX_PATH];
    mp_build_path(path, mp->directory, mp->files[mp->current_index]);
    mediaplayer_open(mp, path);
}

void mediaplayer_prev(mediaplayer_t *mp) {
    if (!mp) return;

    // If more than 3 seconds in, restart current track
    if (mp->position_ms > 3000) {
        mediaplayer_seek_ms(mp, 0);
        return;
    }

    // Check playlist first
    if (mp->playlist_count > 0) {
        mp->playlist_index--;
        if (mp->playlist_index < 0) {
            if (mp->repeat_mode == MP_REPEAT_ALL) {
                mp->playlist_index = mp->playlist_count - 1;
            } else {
                mp->playlist_index = 0;
            }
        }
        mediaplayer_open(mp, mp->playlist[mp->playlist_index].path);
        if (mp->state == MP_STATE_PLAYING) {
            mediaplayer_play(mp);
        }
        return;
    }

    // Use directory listing
    if (mp->file_count == 0) return;

    mp->current_index--;
    if (mp->current_index < 0) {
        if (mp->repeat_mode == MP_REPEAT_ALL) {
            mp->current_index = mp->file_count - 1;
        } else {
            mp->current_index = 0;
        }
    }

    char path[MP_MAX_PATH];
    mp_build_path(path, mp->directory, mp->files[mp->current_index]);
    mediaplayer_open(mp, path);
}

// ============================================================================
// Volume Control
// ============================================================================

void mediaplayer_set_volume(mediaplayer_t *mp, uint8_t volume) {
    if (!mp) return;
    mp->volume = volume;
    if (!mp->muted) {
        if (audio_is_available()) {
            audio_set_master_volume(volume * 100 / 255);
        } else {
            sound_set_volume(volume);
        }
    }
}

uint8_t mediaplayer_get_volume(mediaplayer_t *mp) {
    return mp ? mp->volume : 0;
}

void mediaplayer_toggle_mute(mediaplayer_t *mp) {
    if (!mp) return;
    mp->muted = !mp->muted;

    if (mp->muted) {
        if (audio_is_available()) {
            audio_mute(true);
        } else {
            sound_set_volume(0);
        }
    } else {
        if (audio_is_available()) {
            audio_mute(false);
            audio_set_master_volume(mp->volume * 100 / 255);
        } else {
            sound_set_volume(mp->volume);
        }
    }
}

// ============================================================================
// Repeat/Shuffle
// ============================================================================

void mediaplayer_set_repeat(mediaplayer_t *mp, mp_repeat_mode_t mode) {
    if (mp) mp->repeat_mode = mode;
}

void mediaplayer_cycle_repeat(mediaplayer_t *mp) {
    if (!mp) return;
    mp->repeat_mode = (mp->repeat_mode + 1) % 3;
}

void mediaplayer_toggle_shuffle(mediaplayer_t *mp) {
    if (mp) mp->shuffle = !mp->shuffle;
}

// ============================================================================
// Video Features
// ============================================================================

void mediaplayer_toggle_fullscreen(mediaplayer_t *mp) {
    if (!mp) return;

    mp->fullscreen = !mp->fullscreen;

    if (mp->fullscreen) {
        // Save current bounds
        window_get_content_bounds(mp->window, &mp->saved_bounds.x, &mp->saved_bounds.y,
                                   &mp->saved_bounds.width, &mp->saved_bounds.height);
        // Maximize window
        window_maximize(mp->window);
    } else {
        // Restore bounds
        window_restore(mp->window);
    }
}

bool mediaplayer_is_video(mediaplayer_t *mp) {
    return mp && mp->media_type == MEDIA_TYPE_VIDEO;
}

void mediaplayer_show_controls(mediaplayer_t *mp) {
    if (mp) {
        mp->controls_visible = true;
        mp->controls_hide_time = timer_get_ticks() + MP_OVERLAY_FADE_MS;
    }
}

void mediaplayer_hide_controls(mediaplayer_t *mp) {
    if (mp) mp->controls_visible = false;
}

// ============================================================================
// Update
// ============================================================================

void mediaplayer_update(mediaplayer_t *mp) {
    if (!mp || mp->state != MP_STATE_PLAYING) return;

    // Check if audio finished
    if (!sound_is_playing()) {
        // Advance position
        uint32_t chunk_played = 32768;
        if (mp->metadata.bits_per_sample == 16) {
            chunk_played *= 2;
        }
        mp->audio_buffer_pos += chunk_played;

        // Update position_ms
        if (mp->audio_buffer_size > 0 && mp->metadata.duration_ms > 0) {
            mp->position_ms = (uint32_t)(((uint64_t)mp->audio_buffer_pos * mp->metadata.duration_ms) /
                                         mp->audio_buffer_size);
        }

        if (mp->audio_buffer_pos >= mp->audio_buffer_size) {
            // Track ended
            if (mp->repeat_mode == MP_REPEAT_ONE) {
                mediaplayer_seek_ms(mp, 0);
                mediaplayer_play(mp);
            } else if (mp->playlist_count > 1 || mp->file_count > 1 || mp->repeat_mode == MP_REPEAT_ALL) {
                mediaplayer_next(mp);
                mediaplayer_play(mp);
            } else {
                mediaplayer_stop(mp);
            }
        } else {
            // Continue playing
            mp_stream_audio(mp);
        }
    }

    // Auto-hide video controls
    if (mp->media_type == MEDIA_TYPE_VIDEO && mp->controls_visible) {
        if (timer_get_ticks() >= mp->controls_hide_time) {
            mp->controls_visible = false;
        }
    }
}

// ============================================================================
// Drawing - Progress Bar
// ============================================================================

static void mp_draw_progress(mediaplayer_t *mp, int32_t x, int32_t y, int32_t w) {
    int32_t bar_h = MP_PROGRESS_HEIGHT;

    // Background
    fb_fill_rect(x, y, w, bar_h, MP_PROGRESS_BG);

    // Progress fill
    int32_t progress_w = 0;
    if (mp->has_media && mp->metadata.duration_ms > 0) {
        progress_w = (w * mp->position_ms) / mp->metadata.duration_ms;
        if (progress_w > w) progress_w = w;
    }
    if (progress_w > 0) {
        fb_fill_rect(x, y, progress_w, bar_h, MP_PROGRESS_FG);
    }

    // Handle
    int32_t handle_x = x + progress_w - 4;
    if (handle_x < x) handle_x = x;
    fb_fill_rect(handle_x, y - 2, 8, bar_h + 4, MP_PROGRESS_HANDLE);
}

// ============================================================================
// Drawing - Album Art
// ============================================================================

static void mp_draw_album_art(mediaplayer_t *mp, int32_t x, int32_t y, int32_t size) {
    // Background
    fb_fill_rect(x, y, size, size, MP_ALBUMART_BG);

    if (mp->metadata.has_album_art && mp->metadata.album_art) {
        // Draw album art scaled to fit
        image_blit_scaled(mp->metadata.album_art, x, y, size, size);
    } else {
        // Draw music note placeholder
        int32_t cx = x + size / 2;
        int32_t cy = y + size / 2;
        int32_t note_size = size / 3;

        // Note stem
        fb_fill_rect(cx + note_size / 4, cy - note_size, 3, note_size * 3 / 2, MP_TEXT_DIM);
        // Note head
        mp_draw_circle(cx, cy + note_size / 4, note_size / 3, MP_TEXT_DIM);
        // Second note
        fb_fill_rect(cx + note_size / 4 + note_size / 2, cy - note_size + 4, 3, note_size * 3 / 2 - 4, MP_TEXT_DIM);
        mp_draw_circle(cx + note_size / 2, cy + note_size / 4, note_size / 3, MP_TEXT_DIM);
        // Beam
        fb_fill_rect(cx + note_size / 4, cy - note_size, note_size / 2 + 3, 4, MP_TEXT_DIM);
    }
}

// ============================================================================
// Drawing - Info Area
// ============================================================================

static void mp_draw_info(mediaplayer_t *mp, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!mp->has_media) {
        mp_draw_text(x + 8, y + h / 2 - 8, "No file loaded - Press 'O' to open", MP_TEXT_DIM);
        return;
    }

    // Album art on left
    int32_t art_margin = 10;
    int32_t art_size = h - art_margin * 2;
    if (art_size > MP_ALBUMART_SIZE) art_size = MP_ALBUMART_SIZE;
    mp_draw_album_art(mp, x + art_margin, y + (h - art_size) / 2, art_size);

    // Text info on right
    int32_t text_x = x + art_margin + art_size + 15;
    int32_t text_y = y + 12;
    (void)w;  // Unused but kept for future text wrapping

    // Title (bright)
    mp_draw_text(text_x, text_y, mp->metadata.title[0] ? mp->metadata.title : mp->filename, MP_TEXT_BRIGHT);
    text_y += 20;

    // Artist
    if (mp->metadata.artist[0]) {
        mp_draw_text(text_x, text_y, mp->metadata.artist, MP_TEXT_COLOR);
        text_y += 18;
    }

    // Album
    if (mp->metadata.album[0]) {
        mp_draw_text(text_x, text_y, mp->metadata.album, MP_TEXT_DIM);
        text_y += 18;
    }

    // Format info
    char info[128];
    char *p = info;
    p = mp_itoa(mp->metadata.sample_rate, p);
    const char *hz = " Hz";
    while (*hz) *p++ = *hz++;
    if (mp->metadata.bitrate > 0) {
        const char *sep = ", ";
        while (*sep) *p++ = *sep++;
        p = mp_itoa(mp->metadata.bitrate, p);
        const char *kbps = " kbps";
        while (*kbps) *p++ = *kbps++;
    }
    const char *sep2 = ", ";
    while (*sep2) *p++ = *sep2++;
    const char *fmt = mediaplayer_format_name(mp->format);
    while (*fmt) *p++ = *fmt++;
    *p = '\0';
    mp_draw_text(text_x, text_y, info, MP_TEXT_DIM);

    // Track counter if playlist or multiple files
    int total = (mp->playlist_count > 0) ? mp->playlist_count : mp->file_count;
    int current = (mp->playlist_count > 0) ? mp->playlist_index + 1 : mp->current_index + 1;
    if (total > 1) {
        char counter[32];
        p = counter;
        p = mp_itoa(current, p);
        *p++ = '/';
        p = mp_itoa(total, p);
        *p = '\0';
        mp_draw_text(x + w - strlen(counter) * 8 - 10, y + 12, counter, MP_TEXT_DIM);
    }
}

// ============================================================================
// Drawing - Controls
// ============================================================================

static void mp_draw_controls(mediaplayer_t *mp, int32_t x, int32_t y, int32_t w, bool overlay) {
    (void)overlay;

    // Button positions
    int32_t center_x = x + w / 2;
    int32_t btn_y = y + 15;
    int32_t btn_spacing = MP_BUTTON_SIZE + MP_BUTTON_PADDING;

    // Previous button
    int32_t prev_x = center_x - btn_spacing * 2;
    uint32_t prev_color = mp->prev_hovered ? MP_BUTTON_HOVER : MP_BUTTON_COLOR;
    mp_draw_circle(prev_x, btn_y + MP_BUTTON_SIZE / 2, MP_BUTTON_SIZE / 2, prev_color);
    mp_draw_prev_icon(prev_x - 6, btn_y + 8, 12, MP_TEXT_COLOR);

    // Play/Pause button (larger)
    int32_t play_x = center_x;
    uint32_t play_color = mp->play_hovered ? MP_BUTTON_HOVER : MP_BUTTON_COLOR;
    if (mp->state == MP_STATE_PLAYING) {
        play_color = MP_BUTTON_ACTIVE;
    }
    mp_draw_circle(play_x, btn_y + MP_BUTTON_SIZE / 2, MP_BUTTON_SIZE / 2 + 4, play_color);
    if (mp->state == MP_STATE_PLAYING) {
        mp_draw_pause_icon(play_x - 6, btn_y + 6, 16, MP_TEXT_COLOR);
    } else {
        mp_draw_play_icon(play_x - 4, btn_y + 6, 16, MP_TEXT_COLOR);
    }

    // Stop button
    int32_t stop_x = center_x + btn_spacing;
    uint32_t stop_color = mp->stop_hovered ? MP_BUTTON_HOVER : MP_BUTTON_COLOR;
    mp_draw_circle(stop_x, btn_y + MP_BUTTON_SIZE / 2, MP_BUTTON_SIZE / 2, stop_color);
    mp_draw_stop_icon(stop_x - 5, btn_y + 10, 10, MP_TEXT_COLOR);

    // Next button
    int32_t next_x = center_x + btn_spacing * 2;
    uint32_t next_color = mp->next_hovered ? MP_BUTTON_HOVER : MP_BUTTON_COLOR;
    mp_draw_circle(next_x, btn_y + MP_BUTTON_SIZE / 2, MP_BUTTON_SIZE / 2, next_color);
    mp_draw_next_icon(next_x - 6, btn_y + 8, 12, MP_TEXT_COLOR);

    // Time display
    char time_buf[16];
    mediaplayer_format_time(mp->position_ms, time_buf, mp->metadata.duration_ms >= 3600000);
    mp_draw_text(x + 10, btn_y + 8, time_buf, MP_TEXT_COLOR);

    char dur_buf[16];
    mediaplayer_format_time(mp->metadata.duration_ms, dur_buf, mp->metadata.duration_ms >= 3600000);
    int dur_len = strlen(dur_buf);
    mp_draw_text(x + w - dur_len * 8 - 10, btn_y + 8, dur_buf, MP_TEXT_COLOR);

    // Secondary controls row
    int32_t row2_y = btn_y + MP_BUTTON_SIZE + 10;

    // Volume control on left
    int32_t vol_x = x + 10;
    int32_t vol_icon_size = 16;
    uint32_t vol_color = mp->mute_hovered ? MP_BUTTON_HOVER : MP_TEXT_DIM;
    mp_draw_volume_icon(vol_x, row2_y, vol_icon_size, vol_color, mp->muted);

    int32_t vol_bar_x = vol_x + vol_icon_size + 8;
    int32_t vol_bar_w = MP_VOLUME_WIDTH;
    fb_fill_rect(vol_bar_x, row2_y + 4, vol_bar_w, MP_VOLUME_HEIGHT, MP_VOLUME_BG);
    int32_t vol_fill = (vol_bar_w * mp->volume) / 255;
    if (vol_fill > 0) {
        fb_fill_rect(vol_bar_x, row2_y + 4, vol_fill, MP_VOLUME_HEIGHT, MP_VOLUME_FG);
    }

    // Right side buttons: Repeat, Shuffle, Fullscreen, Playlist
    int32_t right_btn_x = x + w - 10;
    int32_t small_btn = 16;

    // Playlist button
    right_btn_x -= small_btn + 8;
    uint32_t playlist_color = mp->playlist_hovered ? MP_BUTTON_HOVER : MP_TEXT_DIM;
    mp_draw_playlist_icon(right_btn_x, row2_y, small_btn, playlist_color);

    // Fullscreen button (only for video)
    if (mp->media_type == MEDIA_TYPE_VIDEO) {
        right_btn_x -= small_btn + 8;
        uint32_t fs_color = mp->fullscreen_hovered ? MP_BUTTON_HOVER : MP_TEXT_DIM;
        if (mp->fullscreen) fs_color = MP_BUTTON_ACTIVE;
        mp_draw_fullscreen_icon(right_btn_x, row2_y, small_btn, fs_color);
    }

    // Shuffle button
    right_btn_x -= small_btn + 8;
    uint32_t shuffle_color = mp->shuffle ? MP_SHUFFLE_COLOR : (mp->shuffle_hovered ? MP_BUTTON_HOVER : MP_TEXT_DIM);
    mp_draw_shuffle_icon(right_btn_x, row2_y, small_btn, shuffle_color);

    // Repeat button
    right_btn_x -= small_btn + 8;
    uint32_t repeat_color = MP_TEXT_DIM;
    if (mp->repeat_mode == MP_REPEAT_ONE) repeat_color = MP_REPEAT_ONE_COLOR;
    else if (mp->repeat_mode == MP_REPEAT_ALL) repeat_color = MP_REPEAT_ALL_COLOR;
    else if (mp->repeat_hovered) repeat_color = MP_BUTTON_HOVER;
    mp_draw_repeat_icon(right_btn_x, row2_y, small_btn, repeat_color, mp->repeat_mode);
}

// ============================================================================
// Drawing - Audio Mode
// ============================================================================

static void mp_draw_audio_mode(mediaplayer_t *mp, int32_t wx, int32_t wy, int32_t ww, int32_t wh) {
    // Background
    fb_fill_rect(wx, wy, ww, wh, MP_BG_COLOR);

    // Info area (top)
    fb_fill_rect(wx, wy, ww, MP_INFO_HEIGHT, MP_BG_DARK);
    mp_draw_info(mp, wx, wy, ww, MP_INFO_HEIGHT);

    // Progress bar
    int32_t prog_y = wy + MP_INFO_HEIGHT + 10;
    int32_t prog_margin = 60;
    mp_draw_progress(mp, wx + prog_margin, prog_y, ww - prog_margin * 2);

    // Controls
    int32_t ctrl_y = prog_y + MP_PROGRESS_HEIGHT + 10;
    mp_draw_controls(mp, wx, ctrl_y, ww, false);
}

// ============================================================================
// Drawing - Video Mode
// ============================================================================

static void mp_draw_video_mode(mediaplayer_t *mp, int32_t wx, int32_t wy, int32_t ww, int32_t wh) {
    // Video frame area
    fb_fill_rect(wx, wy, ww, wh, 0x000000);

    // Draw video frame if available
    if (mp->current_frame.pixels) {
        // Scale to fit while maintaining aspect ratio
        int32_t vid_w = mp->current_frame.width;
        int32_t vid_h = mp->current_frame.height;

        // Calculate scaled size
        int32_t scale_w = ww;
        int32_t scale_h = (vid_h * ww) / vid_w;
        if (scale_h > wh) {
            scale_h = wh;
            scale_w = (vid_w * wh) / vid_h;
        }

        int32_t vid_x = wx + (ww - scale_w) / 2;
        int32_t vid_y = wy + (wh - scale_h) / 2;

        // Draw scaled frame
        image_t frame_img = {
            .width = mp->current_frame.width,
            .height = mp->current_frame.height,
            .pixels = mp->current_frame.pixels
        };
        image_blit_scaled(&frame_img, vid_x, vid_y, scale_w, scale_h);
    } else {
        // No video frame - show placeholder
        const char *msg = "Video playback not available";
        int msg_len = strlen(msg);
        mp_draw_text(wx + (ww - msg_len * 8) / 2, wy + wh / 2 - 8, msg, MP_TEXT_DIM);
    }

    // Overlay controls (shown on mouse move)
    if (mp->controls_visible) {
        int32_t overlay_y = wy + wh - MP_OVERLAY_HEIGHT;

        // Semi-transparent background (draw darkened)
        for (int32_t row = 0; row < MP_OVERLAY_HEIGHT; row++) {
            for (int32_t col = 0; col < ww; col++) {
                fb_put_pixel(wx + col, overlay_y + row, MP_TOOLBAR_BG);
            }
        }

        // Progress bar
        mp_draw_progress(mp, wx + 60, overlay_y + 5, ww - 120);

        // Controls below progress
        mp_draw_controls(mp, wx, overlay_y + 20, ww, true);
    }
}

// ============================================================================
// Drawing - Main
// ============================================================================

void mediaplayer_draw(mediaplayer_t *mp) {
    if (!mp || !mp->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(mp->window, &wx, &wy, &ww, &wh);

    if (mp->media_type == MEDIA_TYPE_VIDEO) {
        mp_draw_video_mode(mp, wx, wy, ww, wh);
    } else {
        mp_draw_audio_mode(mp, wx, wy, ww, wh);
    }
}

// ============================================================================
// Event Handling
// ============================================================================

void mediaplayer_handle_event(mediaplayer_t *mp, gui_event_t *event) {
    if (!mp || !event) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(mp->window, &wx, &wy, &ww, &wh);

    switch (event->type) {
        case EVENT_KEY_DOWN:
            switch (event->keycode) {
                case ' ':
                    mediaplayer_toggle_play(mp);
                    break;
                case 's':
                case 'S':
                    mediaplayer_stop(mp);
                    break;
                case 'n':
                case 'N':
                case 0x4D:  // Right arrow
                    mediaplayer_next(mp);
                    break;
                case 'p':
                case 'P':
                case 0x4B:  // Left arrow
                    mediaplayer_prev(mp);
                    break;
                case '+':
                case '=':
                    if (mp->volume < 240) mp->volume += 16;
                    else mp->volume = 255;
                    mediaplayer_set_volume(mp, mp->volume);
                    break;
                case '-':
                case '_':
                    if (mp->volume > 16) mp->volume -= 16;
                    else mp->volume = 0;
                    mediaplayer_set_volume(mp, mp->volume);
                    break;
                case 'm':
                    mediaplayer_toggle_mute(mp);
                    break;
                case 'r':
                case 'R':
                    mediaplayer_cycle_repeat(mp);
                    break;
                case 'h':
                case 'H':
                    mediaplayer_toggle_shuffle(mp);
                    break;
                case 'f':
                case 'F':
                case 0x57:  // F11
                    if (mp->media_type == MEDIA_TYPE_VIDEO) {
                        mediaplayer_toggle_fullscreen(mp);
                    }
                    break;
                case 'o':
                case 'O': {
                    char filepath[MP_MAX_PATH];
                    if (filedialog_open("Open Media File", "/", "*.*", "Media Files", filepath)) {
                        mediaplayer_open(mp, filepath);
                    }
                    break;
                }
                case 'l':
                case 'L': {
                    char filepath[MP_MAX_PATH];
                    if (filedialog_open("Open Playlist", "/", "*.m3u", "M3U Playlists", filepath)) {
                        mediaplayer_load_playlist(mp, filepath);
                    }
                    break;
                }
            }
            break;

        case EVENT_MOUSE_DOWN: {
            int32_t mx = event->mouse_x;
            int32_t my = event->mouse_y;

            // Show controls in video mode
            if (mp->media_type == MEDIA_TYPE_VIDEO) {
                mediaplayer_show_controls(mp);
            }

            // Determine layout based on mode
            int32_t ctrl_y, prog_y, prog_margin;
            if (mp->media_type == MEDIA_TYPE_VIDEO && mp->controls_visible) {
                int32_t overlay_y = wy + wh - MP_OVERLAY_HEIGHT;
                prog_y = overlay_y + 5;
                ctrl_y = overlay_y + 20 + 15;
                prog_margin = 60;
            } else {
                prog_y = wy + MP_INFO_HEIGHT + 10;
                ctrl_y = prog_y + MP_PROGRESS_HEIGHT + 10 + 15;
                prog_margin = 60;
            }

            // Button hit tests
            int32_t center_x = wx + ww / 2;
            int32_t btn_spacing = MP_BUTTON_SIZE + MP_BUTTON_PADDING;
            int32_t btn_y = ctrl_y;

            // Play button
            int32_t play_x = center_x;
            int32_t dx = mx - play_x;
            int32_t dy = my - (btn_y + MP_BUTTON_SIZE / 2);
            if (dx * dx + dy * dy <= (MP_BUTTON_SIZE / 2 + 4) * (MP_BUTTON_SIZE / 2 + 4)) {
                mediaplayer_toggle_play(mp);
                break;
            }

            // Stop button
            int32_t stop_x = center_x + btn_spacing;
            dx = mx - stop_x;
            if (dx * dx + dy * dy <= (MP_BUTTON_SIZE / 2) * (MP_BUTTON_SIZE / 2)) {
                mediaplayer_stop(mp);
                break;
            }

            // Prev button
            int32_t prev_x = center_x - btn_spacing * 2;
            dx = mx - prev_x;
            if (dx * dx + dy * dy <= (MP_BUTTON_SIZE / 2) * (MP_BUTTON_SIZE / 2)) {
                mediaplayer_prev(mp);
                break;
            }

            // Next button
            int32_t next_x = center_x + btn_spacing * 2;
            dx = mx - next_x;
            if (dx * dx + dy * dy <= (MP_BUTTON_SIZE / 2) * (MP_BUTTON_SIZE / 2)) {
                mediaplayer_next(mp);
                break;
            }

            // Progress bar click
            int32_t bar_x = wx + prog_margin;
            int32_t bar_w = ww - prog_margin * 2;
            if (mx >= bar_x && mx <= bar_x + bar_w &&
                my >= prog_y - 4 && my <= prog_y + MP_PROGRESS_HEIGHT + 4) {
                mp->seek_dragging = true;
                mp->drag_start_x = mx;
                int percent = ((mx - bar_x) * 100) / bar_w;
                mediaplayer_seek(mp, percent);
                break;
            }

            // Volume bar click
            int32_t row2_y = btn_y + MP_BUTTON_SIZE + 10;
            int32_t vol_icon_size = 16;
            int32_t vol_x = wx + 10;
            int32_t vol_bar_x = vol_x + vol_icon_size + 8;

            // Mute button click
            if (mx >= vol_x && mx <= vol_x + vol_icon_size &&
                my >= row2_y && my <= row2_y + vol_icon_size) {
                mediaplayer_toggle_mute(mp);
                break;
            }

            // Volume slider
            if (mx >= vol_bar_x && mx <= vol_bar_x + MP_VOLUME_WIDTH &&
                my >= row2_y && my <= row2_y + MP_VOLUME_HEIGHT + 8) {
                mp->vol_dragging = true;
                int vol_percent = ((mx - vol_bar_x) * 255) / MP_VOLUME_WIDTH;
                if (vol_percent < 0) vol_percent = 0;
                if (vol_percent > 255) vol_percent = 255;
                mediaplayer_set_volume(mp, vol_percent);
                break;
            }

            // Right side buttons
            int32_t right_btn_x = wx + ww - 10;
            int32_t small_btn = 16;

            // Playlist button
            right_btn_x -= small_btn + 8;
            if (mx >= right_btn_x && mx <= right_btn_x + small_btn &&
                my >= row2_y && my <= row2_y + small_btn) {
                // TODO: Show playlist panel
                break;
            }

            // Fullscreen button
            if (mp->media_type == MEDIA_TYPE_VIDEO) {
                right_btn_x -= small_btn + 8;
                if (mx >= right_btn_x && mx <= right_btn_x + small_btn &&
                    my >= row2_y && my <= row2_y + small_btn) {
                    mediaplayer_toggle_fullscreen(mp);
                    break;
                }
            }

            // Shuffle button
            right_btn_x -= small_btn + 8;
            if (mx >= right_btn_x && mx <= right_btn_x + small_btn &&
                my >= row2_y && my <= row2_y + small_btn) {
                mediaplayer_toggle_shuffle(mp);
                break;
            }

            // Repeat button
            right_btn_x -= small_btn + 8;
            if (mx >= right_btn_x && mx <= right_btn_x + small_btn &&
                my >= row2_y && my <= row2_y + small_btn) {
                mediaplayer_cycle_repeat(mp);
                break;
            }

            // Double-click for fullscreen (video only)
            if (mp->media_type == MEDIA_TYPE_VIDEO && event->mouse_buttons == MOUSE_BUTTON_LEFT) {
                static uint32_t last_click_time = 0;
                uint32_t current_time = timer_get_ticks();
                if (current_time - last_click_time < 500) {
                    mediaplayer_toggle_fullscreen(mp);
                }
                last_click_time = current_time;
            }
            break;
        }

        case EVENT_MOUSE_UP:
            mp->seek_dragging = false;
            mp->vol_dragging = false;
            break;

        case EVENT_MOUSE_MOVE: {
            int32_t mx = event->mouse_x;
            int32_t my = event->mouse_y;

            // Show controls in video mode on mouse move
            if (mp->media_type == MEDIA_TYPE_VIDEO) {
                mediaplayer_show_controls(mp);
            }

            // Update hover states
            int32_t ctrl_y, prog_margin;
            if (mp->media_type == MEDIA_TYPE_VIDEO && mp->controls_visible) {
                int32_t overlay_y = wy + wh - MP_OVERLAY_HEIGHT;
                ctrl_y = overlay_y + 20 + 15;
                prog_margin = 60;
            } else {
                int32_t prog_y = wy + MP_INFO_HEIGHT + 10;
                ctrl_y = prog_y + MP_PROGRESS_HEIGHT + 10 + 15;
                prog_margin = 60;
            }

            int32_t center_x = wx + ww / 2;
            int32_t btn_spacing = MP_BUTTON_SIZE + MP_BUTTON_PADDING;
            int32_t btn_y = ctrl_y;
            int32_t dy = my - (btn_y + MP_BUTTON_SIZE / 2);

            // Play button hover
            int32_t play_x = center_x;
            int32_t dx = mx - play_x;
            mp->play_hovered = (dx * dx + dy * dy <= (MP_BUTTON_SIZE / 2 + 4) * (MP_BUTTON_SIZE / 2 + 4));

            // Stop button hover
            int32_t stop_x = center_x + btn_spacing;
            dx = mx - stop_x;
            mp->stop_hovered = (dx * dx + dy * dy <= (MP_BUTTON_SIZE / 2) * (MP_BUTTON_SIZE / 2));

            // Prev button hover
            int32_t prev_x = center_x - btn_spacing * 2;
            dx = mx - prev_x;
            mp->prev_hovered = (dx * dx + dy * dy <= (MP_BUTTON_SIZE / 2) * (MP_BUTTON_SIZE / 2));

            // Next button hover
            int32_t next_x = center_x + btn_spacing * 2;
            dx = mx - next_x;
            mp->next_hovered = (dx * dx + dy * dy <= (MP_BUTTON_SIZE / 2) * (MP_BUTTON_SIZE / 2));

            // Handle dragging
            if (mp->seek_dragging) {
                int32_t bar_x = wx + prog_margin;
                int32_t bar_w = ww - prog_margin * 2;
                int percent = ((mx - bar_x) * 100) / bar_w;
                mediaplayer_seek(mp, percent);
            }
            if (mp->vol_dragging) {
                int32_t vol_icon_size = 16;
                int32_t vol_x = wx + 10;
                int32_t vol_bar_x = vol_x + vol_icon_size + 8;
                int vol_percent = ((mx - vol_bar_x) * 255) / MP_VOLUME_WIDTH;
                if (vol_percent < 0) vol_percent = 0;
                if (vol_percent > 255) vol_percent = 255;
                mediaplayer_set_volume(mp, vol_percent);
            }
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// Launch Functions
// ============================================================================

void mediaplayer_launch(const char *filepath) {
    LOG_INFO("[MediaPlayer] Application launched");
    mediaplayer_t *mp = mediaplayer_create();
    if (!mp) {
        LOG_ERROR("[MediaPlayer] Failed to create player window");
        kprintf("[MediaPlayer] Failed to create player\n");
        return;
    }

    if (filepath && strlen(filepath) > 0) {
        char log_msg[128];
        strncpy(log_msg, "[MediaPlayer] Opening: ", 24);
        strncat(log_msg, filepath, 80);
        LOG_INFO(log_msg);
        mediaplayer_open(mp, filepath);
    }

    // Register with window manager
    wm_register_app(mp->window, mp,
                    (app_event_handler_t)mediaplayer_handle_event,
                    (app_draw_handler_t)mediaplayer_draw,
                    (app_destroy_handler_t)mediaplayer_destroy);

    kprintf("[MediaPlayer] Launched\n");
}

void mediaplayer_launch_simple(void) {
    mediaplayer_launch(NULL);
}
