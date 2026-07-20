// mediaplayer.h - Media Player Application for MayteraOS
// Supports audio (WAV, MP3, FLAC, OGG) and video (MPEG, AVI) playback
#ifndef MEDIAPLAYER_H
#define MEDIAPLAYER_H

#include "window.h"
#include "image.h"
#include "../types.h"

// Maximum limits
#define MP_MAX_PATH         256
#define MP_MAX_FILES        512
#define MP_MAX_PLAYLIST     256
#define MP_MAX_TITLE        128
#define MP_MAX_ARTIST       128
#define MP_MAX_ALBUM        128

// Supported formats
typedef enum {
    MEDIA_FORMAT_UNKNOWN = 0,
    // Audio formats
    MEDIA_FORMAT_WAV,
    MEDIA_FORMAT_MP3,
    MEDIA_FORMAT_FLAC,
    MEDIA_FORMAT_OGG,
    // Video formats
    MEDIA_FORMAT_MPEG,
    MEDIA_FORMAT_AVI
} media_format_t;

// Media type
typedef enum {
    MEDIA_TYPE_NONE = 0,
    MEDIA_TYPE_AUDIO,
    MEDIA_TYPE_VIDEO
} media_type_t;

// Player states
typedef enum {
    MP_STATE_STOPPED = 0,
    MP_STATE_PLAYING,
    MP_STATE_PAUSED,
    MP_STATE_BUFFERING
} mp_state_t;

// Repeat modes
typedef enum {
    MP_REPEAT_NONE = 0,     // Play once
    MP_REPEAT_ONE,          // Repeat current track
    MP_REPEAT_ALL           // Repeat playlist
} mp_repeat_mode_t;

// ============================================================================
// WAV File Format Structures
// ============================================================================

#define WAV_RIFF_MAGIC      0x46464952  // "RIFF"
#define WAV_WAVE_MAGIC      0x45564157  // "WAVE"
#define WAV_FMT_MAGIC       0x20746D66  // "fmt "
#define WAV_DATA_MAGIC      0x61746164  // "data"
#define WAV_FORMAT_PCM      1

#ifndef WAV_TYPES_DEFINED
#define WAV_TYPES_DEFINED
typedef struct __attribute__((packed)) {
    uint32_t chunk_id;       // "RIFF"
    uint32_t chunk_size;     // File size - 8
    uint32_t format;         // "WAVE"
} wav_riff_header_t;

typedef struct __attribute__((packed)) {
    uint32_t subchunk_id;    // "fmt "
    uint32_t subchunk_size;  // 16 for PCM
    uint16_t audio_format;   // 1 = PCM
    uint16_t num_channels;   // 1 = mono, 2 = stereo
    uint32_t sample_rate;    // e.g., 44100
    uint32_t byte_rate;      // sample_rate * num_channels * bits_per_sample/8
    uint16_t block_align;    // num_channels * bits_per_sample/8
    uint16_t bits_per_sample;// 8 or 16
} wav_fmt_chunk_t;

typedef struct __attribute__((packed)) {
    uint32_t subchunk_id;    // "data"
    uint32_t subchunk_size;  // Number of bytes of audio data
} wav_data_chunk_t;
#endif // WAV_TYPES_DEFINED

// ============================================================================
// Media Metadata
// ============================================================================

typedef struct {
    char title[MP_MAX_TITLE];
    char artist[MP_MAX_ARTIST];
    char album[MP_MAX_ALBUM];
    uint32_t year;
    uint32_t track_number;
    uint32_t duration_ms;

    // Audio-specific
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t bitrate;           // For compressed formats (kbps)

    // Video-specific
    uint32_t width;
    uint32_t height;
    uint32_t frame_rate;        // FPS * 100 (e.g., 2997 = 29.97 fps)

    // Album art
    image_t *album_art;
    bool has_album_art;
} media_metadata_t;

// ============================================================================
// Playlist Entry
// ============================================================================

typedef struct {
    char path[MP_MAX_PATH];
    char title[MP_MAX_TITLE];
    uint32_t duration_ms;
    bool loaded;                // Metadata loaded?
} playlist_entry_t;

// ============================================================================
// Video Frame Buffer
// ============================================================================

typedef struct {
    uint32_t *pixels;           // BGRA pixel data
    uint32_t width;
    uint32_t height;
    uint64_t pts;               // Presentation timestamp (ms)
} video_frame_t;

// ============================================================================
// Decoder Context (Abstract Interface)
// ============================================================================

typedef struct decoder_ctx decoder_ctx_t;

// Decoder interface functions
typedef int (*decoder_open_fn)(decoder_ctx_t *ctx, const uint8_t *data, uint32_t size);
typedef void (*decoder_close_fn)(decoder_ctx_t *ctx);
typedef int (*decoder_read_audio_fn)(decoder_ctx_t *ctx, int16_t *samples, uint32_t max_samples);
typedef int (*decoder_read_video_fn)(decoder_ctx_t *ctx, video_frame_t *frame);
typedef int (*decoder_seek_fn)(decoder_ctx_t *ctx, uint32_t position_ms);

struct decoder_ctx {
    void *private_data;
    media_format_t format;
    media_type_t type;
    media_metadata_t metadata;

    // Position tracking
    uint32_t position_ms;
    uint64_t bytes_decoded;

    // Interface functions
    decoder_open_fn open;
    decoder_close_fn close;
    decoder_read_audio_fn read_audio;
    decoder_read_video_fn read_video;
    decoder_seek_fn seek;
};

// ============================================================================
// Media Player State
// ============================================================================

typedef struct {
    window_t *window;           // Application window

    // Current media
    char filepath[MP_MAX_PATH];
    char filename[64];
    bool has_media;
    media_type_t media_type;
    media_format_t format;

    // Decoder
    decoder_ctx_t *decoder;
    uint8_t *file_data;
    uint32_t file_size;

    // Metadata
    media_metadata_t metadata;

    // Playback state
    mp_state_t state;
    uint32_t position_ms;       // Current position in milliseconds
    uint8_t volume;             // 0-255
    bool muted;
    mp_repeat_mode_t repeat_mode;
    bool shuffle;

    // Playlist
    playlist_entry_t playlist[MP_MAX_PLAYLIST];
    int playlist_count;
    int playlist_index;         // Current playing index
    char playlist_path[MP_MAX_PATH];  // Current playlist file path

    // Directory browsing
    char directory[MP_MAX_PATH];
    char files[MP_MAX_FILES][64];
    int file_count;
    int current_index;

    // Audio buffer (for streaming)
    int16_t *audio_buffer;
    uint32_t audio_buffer_size;
    uint32_t audio_buffer_pos;

    // Video state
    video_frame_t current_frame;
    bool fullscreen;
    rect_t saved_bounds;
    uint32_t last_frame_time;
    bool controls_visible;
    uint32_t controls_hide_time;

    // UI state
    bool play_hovered;
    bool stop_hovered;
    bool prev_hovered;
    bool next_hovered;
    bool mute_hovered;
    bool fullscreen_hovered;
    bool repeat_hovered;
    bool shuffle_hovered;
    bool playlist_hovered;
    bool seek_dragging;
    bool vol_dragging;
    int drag_start_x;

    // Update timing
    uint32_t last_update_tick;

} mediaplayer_t;

// ============================================================================
// Public API - Core Functions
// ============================================================================

// Create media player application
mediaplayer_t *mediaplayer_create(void);

// Destroy media player
void mediaplayer_destroy(mediaplayer_t *mp);

// Open media file
bool mediaplayer_open(mediaplayer_t *mp, const char *path);

// Close current media
void mediaplayer_close(mediaplayer_t *mp);

// ============================================================================
// Public API - Playback Control
// ============================================================================

// Playback controls
void mediaplayer_play(mediaplayer_t *mp);
void mediaplayer_pause(mediaplayer_t *mp);
void mediaplayer_stop(mediaplayer_t *mp);
void mediaplayer_toggle_play(mediaplayer_t *mp);

// Seek to position (0-100 percent)
void mediaplayer_seek(mediaplayer_t *mp, int percent);

// Seek to absolute position in milliseconds
void mediaplayer_seek_ms(mediaplayer_t *mp, uint32_t position_ms);

// Navigation
void mediaplayer_next(mediaplayer_t *mp);
void mediaplayer_prev(mediaplayer_t *mp);

// ============================================================================
// Public API - Volume Control
// ============================================================================

// Set volume (0-255)
void mediaplayer_set_volume(mediaplayer_t *mp, uint8_t volume);

// Get current volume
uint8_t mediaplayer_get_volume(mediaplayer_t *mp);

// Toggle mute
void mediaplayer_toggle_mute(mediaplayer_t *mp);

// ============================================================================
// Public API - Playlist Management
// ============================================================================

// Load playlist from M3U file
bool mediaplayer_load_playlist(mediaplayer_t *mp, const char *path);

// Save current playlist to M3U file
bool mediaplayer_save_playlist(mediaplayer_t *mp, const char *path);

// Add file to playlist
int mediaplayer_playlist_add(mediaplayer_t *mp, const char *path);

// Remove item from playlist
void mediaplayer_playlist_remove(mediaplayer_t *mp, int index);

// Clear playlist
void mediaplayer_playlist_clear(mediaplayer_t *mp);

// Jump to playlist index
void mediaplayer_playlist_goto(mediaplayer_t *mp, int index);

// ============================================================================
// Public API - Repeat/Shuffle
// ============================================================================

// Set repeat mode
void mediaplayer_set_repeat(mediaplayer_t *mp, mp_repeat_mode_t mode);

// Cycle repeat mode
void mediaplayer_cycle_repeat(mediaplayer_t *mp);

// Toggle shuffle
void mediaplayer_toggle_shuffle(mediaplayer_t *mp);

// ============================================================================
// Public API - Video Features
// ============================================================================

// Toggle fullscreen mode
void mediaplayer_toggle_fullscreen(mediaplayer_t *mp);

// Check if current media is video
bool mediaplayer_is_video(mediaplayer_t *mp);

// Show/hide video controls
void mediaplayer_show_controls(mediaplayer_t *mp);
void mediaplayer_hide_controls(mediaplayer_t *mp);

// ============================================================================
// Public API - Events and Drawing
// ============================================================================

// Event handling
void mediaplayer_handle_event(mediaplayer_t *mp, gui_event_t *event);

// Update playback state (call periodically)
void mediaplayer_update(mediaplayer_t *mp);

// Drawing
void mediaplayer_draw(mediaplayer_t *mp);

// ============================================================================
// Public API - Launch Functions
// ============================================================================

// Launch media player with optional file
void mediaplayer_launch(const char *filepath);

// Simplified launch with no arguments (for menu integration)
extern void mediaplayer_launch_simple(void);

// ============================================================================
// Helper Functions
// ============================================================================

// Detect media format from file data
media_format_t mediaplayer_detect_format(const uint8_t *data, uint32_t size, const char *filename);

// Get format name string
const char *mediaplayer_format_name(media_format_t format);

// Check if file is supported media
bool mediaplayer_is_supported(const char *filename);

// Check if file is audio
bool mediaplayer_is_audio_file(const char *filename);

// Check if file is video
bool mediaplayer_is_video_file(const char *filename);

// Check if file is playlist
bool mediaplayer_is_playlist_file(const char *filename);

// Format time as HH:MM:SS or MM:SS
void mediaplayer_format_time(uint32_t ms, char *buf, bool include_hours);

#endif // MEDIAPLAYER_H
