#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
// avi.c - AVI Container Parser Implementation for MayteraOS
// Parses AVI files and decodes uncompressed video frames

#include "avi.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"

// ============================================
// Internal Structures
// ============================================

struct avi_decoder {
    // Input data
    const uint8_t *data;
    size_t size;

    // AVI structure info
    avi_main_header_t main_header;

    // Stream information
    avi_stream_info_t streams[AVI_MAX_STREAMS];
    int num_streams;
    int video_stream;           // Index of video stream (-1 if none)
    int audio_stream;           // Index of audio stream (-1 if none)

    // Video format
    int width, height;
    int bit_depth;
    uint32_t codec;
    bool bottom_up;             // True if image is stored bottom-up

    // Audio format
    int audio_sample_rate;
    int audio_channels;
    int audio_bits;
    uint16_t audio_format;

    // Movi chunk location
    size_t movi_offset;         // Start of movi chunk data
    size_t movi_size;           // Size of movi chunk

    // Index
    avi_index_entry_t *index;
    int index_count;
    int current_frame;          // Current video frame index

    // Timing
    double frame_time;          // Seconds per frame
    double current_time;

    // State
    bool ended;
};

// ============================================
// Helper Functions
// ============================================

// Read 32-bit little-endian value
static inline uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Read 16-bit little-endian value
static inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Convert FourCC to string (for debugging)
static void fourcc_to_string(uint32_t fourcc, char *out) {
    out[0] = fourcc & 0xFF;
    out[1] = (fourcc >> 8) & 0xFF;
    out[2] = (fourcc >> 16) & 0xFF;
    out[3] = (fourcc >> 24) & 0xFF;
    out[4] = '\0';
}

// Check if a chunk ID is a video chunk (e.g., "00dc", "01dc")
static bool is_video_chunk(uint32_t chunk_id, int stream_index) {
    // Video chunks are "NNdc" or "NNdb" where NN is stream number in ASCII
    char expected_hi = '0' + (stream_index / 10);
    char expected_lo = '0' + (stream_index % 10);

    char got_hi = chunk_id & 0xFF;
    char got_lo = (chunk_id >> 8) & 0xFF;
    char type1 = (chunk_id >> 16) & 0xFF;
    char type2 = (chunk_id >> 24) & 0xFF;

    if (got_hi != expected_hi || got_lo != expected_lo) {
        return false;
    }

    // 'dc' = compressed video, 'db' = uncompressed
    return (type1 == 'd' && (type2 == 'c' || type2 == 'b'));
}

// Check if a chunk ID is an audio chunk
static bool is_audio_chunk(uint32_t chunk_id, int stream_index) {
    char expected_hi = '0' + (stream_index / 10);
    char expected_lo = '0' + (stream_index % 10);

    char got_hi = chunk_id & 0xFF;
    char got_lo = (chunk_id >> 8) & 0xFF;
    char type1 = (chunk_id >> 16) & 0xFF;
    char type2 = (chunk_id >> 24) & 0xFF;

    if (got_hi != expected_hi || got_lo != expected_lo) {
        return false;
    }

    // 'wb' = audio data
    return (type1 == 'w' && type2 == 'b');
}

// ============================================
// Chunk Parsing
// ============================================

// Find a chunk within a range
// Returns offset to chunk data (after 8-byte header), or 0 if not found
static size_t find_chunk(const uint8_t *data, size_t start, size_t end,
                         uint32_t fourcc, uint32_t *out_size) {
    size_t pos = start;

    while (pos + 8 <= end) {
        uint32_t chunk_fourcc = read_le32(data + pos);
        uint32_t chunk_size = read_le32(data + pos + 4);

        if (chunk_fourcc == fourcc) {
            if (out_size) *out_size = chunk_size;
            return pos + 8;
        }

        // Move to next chunk (size is padded to word boundary)
        pos += 8 + ((chunk_size + 1) & ~1);
    }

    return 0;
}

// Find a LIST chunk with specific type
static size_t find_list(const uint8_t *data, size_t start, size_t end,
                        uint32_t list_type, uint32_t *out_size) {
    size_t pos = start;

    while (pos + 12 <= end) {
        uint32_t chunk_fourcc = read_le32(data + pos);
        uint32_t chunk_size = read_le32(data + pos + 4);

        if (chunk_fourcc == AVI_LIST) {
            uint32_t type = read_le32(data + pos + 8);
            if (type == list_type) {
                if (out_size) *out_size = chunk_size - 4;
                return pos + 12;  // Skip LIST header and type
            }
        }

        pos += 8 + ((chunk_size + 1) & ~1);
    }

    return 0;
}

// ============================================
// Header Parsing
// ============================================

static int parse_main_header(avi_decoder_t *dec, size_t offset) {
    if (offset + sizeof(avi_main_header_t) > dec->size) {
        return VIDEO_ERR_CORRUPT;
    }

    memcpy(&dec->main_header, dec->data + offset, sizeof(avi_main_header_t));

    kprintf("[AVI] Main header: %ux%u, %u frames, %u streams\n",
            dec->main_header.width, dec->main_header.height,
            dec->main_header.total_frames, dec->main_header.streams);

    // Calculate frame time from microseconds per frame
    if (dec->main_header.microsec_per_frame > 0) {
        dec->frame_time = dec->main_header.microsec_per_frame / 1000000.0;
    } else {
        dec->frame_time = 1.0 / 30.0;  // Default to 30fps
    }

    return VIDEO_SUCCESS;
}

static int parse_stream_header(avi_decoder_t *dec, size_t strl_offset,
                               size_t strl_size, int stream_index) {
    if (stream_index >= AVI_MAX_STREAMS) {
        return VIDEO_ERR_UNSUPPORTED;
    }

    size_t strl_end = strl_offset + strl_size;

    // Find strh (stream header)
    uint32_t strh_size;
    size_t strh_offset = find_chunk(dec->data, strl_offset, strl_end,
                                    AVI_STRH, &strh_size);
    if (!strh_offset || strh_size < 48) {
        kprintf("[AVI] Stream %d: missing strh\n", stream_index);
        return VIDEO_ERR_CORRUPT;
    }

    avi_stream_header_t strh;
    memcpy(&strh, dec->data + strh_offset, sizeof(strh));

    avi_stream_info_t *info = &dec->streams[stream_index];
    info->type = strh.type;
    info->codec = strh.handler;
    info->scale = strh.scale;
    info->rate = strh.rate;
    info->length = strh.length;

    char type_str[5], codec_str[5];
    fourcc_to_string(strh.type, type_str);
    fourcc_to_string(strh.handler, codec_str);
    kprintf("[AVI] Stream %d: type=%s, codec=%s, rate=%u/%u\n",
            stream_index, type_str, codec_str, strh.rate, strh.scale);

    // Find strf (stream format)
    uint32_t strf_size;
    size_t strf_offset = find_chunk(dec->data, strl_offset, strl_end,
                                    AVI_STRF, &strf_size);
    if (!strf_offset) {
        kprintf("[AVI] Stream %d: missing strf\n", stream_index);
        return VIDEO_ERR_CORRUPT;
    }

    if (strh.type == AVI_VIDS) {
        // Video stream
        if (strf_size < sizeof(avi_bitmap_info_t)) {
            return VIDEO_ERR_CORRUPT;
        }

        avi_bitmap_info_t bmp;
        memcpy(&bmp, dec->data + strf_offset, sizeof(bmp));

        info->width = (bmp.width < 0) ? -bmp.width : bmp.width;
        info->height = (bmp.height < 0) ? -bmp.height : bmp.height;
        info->bit_depth = bmp.bit_count;
        info->codec = bmp.compression;

        kprintf("[AVI] Video: %dx%d, %d bpp, compression=0x%08x\n",
                info->width, info->height, info->bit_depth, info->codec);

        if (dec->video_stream < 0) {
            dec->video_stream = stream_index;
            dec->width = info->width;
            dec->height = info->height;
            dec->bit_depth = info->bit_depth;
            dec->codec = info->codec;
            dec->bottom_up = (bmp.height > 0);
        }

    } else if (strh.type == AVI_AUDS) {
        // Audio stream
        if (strf_size < sizeof(avi_wave_format_t)) {
            return VIDEO_ERR_CORRUPT;
        }

        avi_wave_format_t wav;
        memcpy(&wav, dec->data + strf_offset, sizeof(wav));

        info->sample_rate = wav.samples_per_sec;
        info->channels = wav.channels;
        info->bit_depth = wav.bits_per_sample;

        kprintf("[AVI] Audio: %d Hz, %d ch, %d bit, format=%d\n",
                wav.samples_per_sec, wav.channels, wav.bits_per_sample,
                wav.format_tag);

        if (dec->audio_stream < 0) {
            dec->audio_stream = stream_index;
            dec->audio_sample_rate = wav.samples_per_sec;
            dec->audio_channels = wav.channels;
            dec->audio_bits = wav.bits_per_sample;
            dec->audio_format = wav.format_tag;
        }
    }

    return VIDEO_SUCCESS;
}

static int parse_headers(avi_decoder_t *dec) {
    // Verify RIFF header
    if (dec->size < 12) {
        return VIDEO_ERR_CORRUPT;
    }

    if (read_le32(dec->data) != AVI_RIFF) {
        kprintf("[AVI] Not a RIFF file\n");
        return VIDEO_ERR_INVALID_FMT;
    }

    if (read_le32(dec->data + 8) != AVI_AVI) {
        kprintf("[AVI] Not an AVI file\n");
        return VIDEO_ERR_INVALID_FMT;
    }

    // Find hdrl LIST
    uint32_t hdrl_size;
    size_t hdrl_offset = find_list(dec->data, 12, dec->size, AVI_HDRL, &hdrl_size);
    if (!hdrl_offset) {
        kprintf("[AVI] Missing hdrl chunk\n");
        return VIDEO_ERR_CORRUPT;
    }

    size_t hdrl_end = hdrl_offset + hdrl_size;

    // Find and parse avih (main header)
    uint32_t avih_size;
    size_t avih_offset = find_chunk(dec->data, hdrl_offset, hdrl_end,
                                    AVI_AVIH, &avih_size);
    if (!avih_offset) {
        kprintf("[AVI] Missing avih chunk\n");
        return VIDEO_ERR_CORRUPT;
    }

    int result = parse_main_header(dec, avih_offset);
    if (result != VIDEO_SUCCESS) {
        return result;
    }

    // Parse stream headers (strl LISTs)
    size_t pos = hdrl_offset;
    while (pos < hdrl_end && dec->num_streams < AVI_MAX_STREAMS) {
        uint32_t strl_size;
        size_t strl_offset = find_list(dec->data, pos, hdrl_end, AVI_STRL, &strl_size);
        if (!strl_offset) break;

        result = parse_stream_header(dec, strl_offset, strl_size, dec->num_streams);
        if (result == VIDEO_SUCCESS) {
            dec->num_streams++;
        }

        pos = strl_offset + strl_size;
    }

    // Find movi LIST
    uint32_t movi_size;
    size_t movi_offset = find_list(dec->data, 12, dec->size, AVI_MOVI, &movi_size);
    if (!movi_offset) {
        kprintf("[AVI] Missing movi chunk\n");
        return VIDEO_ERR_CORRUPT;
    }

    dec->movi_offset = movi_offset;
    dec->movi_size = movi_size;

    kprintf("[AVI] movi chunk at offset %lu, size %lu\n",
            (unsigned long)movi_offset, (unsigned long)movi_size);

    // Find idx1 (optional index)
    uint32_t idx1_size;
    size_t idx1_offset = find_chunk(dec->data, movi_offset + movi_size, dec->size,
                                    AVI_IDX1, &idx1_size);
    if (idx1_offset && idx1_size > 0) {
        dec->index_count = idx1_size / sizeof(avi_index_entry_t);
        dec->index = kmalloc(idx1_size);
        if (dec->index) {
            memcpy(dec->index, dec->data + idx1_offset, idx1_size);
            kprintf("[AVI] Loaded %d index entries\n", dec->index_count);
        }
    }

    return VIDEO_SUCCESS;
}

// ============================================
// Frame Decoding
// ============================================

// Convert BGR to BGRA (add alpha channel)
static void convert_bgr24_to_bgra(const uint8_t *src, uint32_t *dst,
                                   int width, int height, int stride,
                                   bool bottom_up) {
    for (int y = 0; y < height; y++) {
        int src_y = bottom_up ? (height - 1 - y) : y;
        const uint8_t *row = src + src_y * stride;

        for (int x = 0; x < width; x++) {
            uint8_t b = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t r = row[x * 3 + 2];
            dst[y * width + x] = (uint32_t)b | ((uint32_t)g << 8) |
                                 ((uint32_t)r << 16) | 0xFF000000;
        }
    }
}

// Convert raw 32-bit to BGRA
static void convert_bgrx32_to_bgra(const uint8_t *src, uint32_t *dst,
                                    int width, int height, int stride,
                                    bool bottom_up) {
    for (int y = 0; y < height; y++) {
        int src_y = bottom_up ? (height - 1 - y) : y;
        const uint32_t *row = (const uint32_t *)(src + src_y * stride);

        for (int x = 0; x < width; x++) {
            // Set alpha to 0xFF
            dst[y * width + x] = row[x] | 0xFF000000;
        }
    }
}

// Decode uncompressed video frame
static int decode_raw_frame(avi_decoder_t *dec, const uint8_t *frame_data,
                           uint32_t frame_size, uint32_t *rgb_buffer) {
    int width = dec->width;
    int height = dec->height;

    // Calculate expected stride (rows padded to 4-byte boundary)
    int bytes_per_pixel = dec->bit_depth / 8;
    int stride = ((width * bytes_per_pixel + 3) & ~3);

    // Verify frame size
    uint32_t expected_size = stride * height;
    if (frame_size < expected_size) {
        kprintf("[AVI] Frame too small: got %u, expected %u\n",
                frame_size, expected_size);
        return VIDEO_ERR_CORRUPT;
    }

    // Convert based on bit depth
    switch (dec->bit_depth) {
        case 24:
            convert_bgr24_to_bgra(frame_data, rgb_buffer, width, height,
                                   stride, dec->bottom_up);
            break;

        case 32:
            convert_bgrx32_to_bgra(frame_data, rgb_buffer, width, height,
                                    stride, dec->bottom_up);
            break;

        default:
            kprintf("[AVI] Unsupported bit depth: %d\n", dec->bit_depth);
            return VIDEO_ERR_UNSUPPORTED;
    }

    return VIDEO_SUCCESS;
}

// Find next video frame in movi chunk (without index)
static int find_next_video_frame(avi_decoder_t *dec, size_t *frame_offset,
                                  uint32_t *frame_size) {
    static size_t scan_pos = 0;
    if (dec->current_frame == 0) {
        scan_pos = dec->movi_offset;
    }

    size_t movi_end = dec->movi_offset + dec->movi_size;

    while (scan_pos + 8 <= movi_end) {
        uint32_t chunk_id = read_le32(dec->data + scan_pos);
        uint32_t chunk_size = read_le32(dec->data + scan_pos + 4);

        if (is_video_chunk(chunk_id, dec->video_stream)) {
            *frame_offset = scan_pos + 8;
            *frame_size = chunk_size;
            scan_pos += 8 + ((chunk_size + 1) & ~1);
            return VIDEO_SUCCESS;
        }

        scan_pos += 8 + ((chunk_size + 1) & ~1);
    }

    return VIDEO_ERR_EOF;
}

// ============================================
// Public API Implementation
// ============================================

avi_decoder_t *avi_decoder_open(const void *data, size_t size) {
    if (!data || size < 12) {
        kprintf("[AVI] Invalid data\n");
        return NULL;
    }

    avi_decoder_t *dec = kzalloc(sizeof(avi_decoder_t));
    if (!dec) {
        kprintf("[AVI] Failed to allocate decoder\n");
        return NULL;
    }

    dec->data = (const uint8_t *)data;
    dec->size = size;
    dec->video_stream = -1;
    dec->audio_stream = -1;
    dec->current_frame = 0;
    dec->current_time = 0.0;
    dec->ended = false;

    int result = parse_headers(dec);
    if (result != VIDEO_SUCCESS) {
        kprintf("[AVI] Failed to parse headers: %s\n", video_error_string(result));
        avi_decoder_close(dec);
        return NULL;
    }

    if (dec->video_stream < 0) {
        kprintf("[AVI] No video stream found\n");
        avi_decoder_close(dec);
        return NULL;
    }

    // Check if codec is supported
    if (dec->codec != AVI_RAW && dec->codec != AVI_DIB && dec->codec != AVI_RGB) {
        char codec_str[5];
        fourcc_to_string(dec->codec, codec_str);
        kprintf("[AVI] Unsupported codec: %s (0x%08x)\n", codec_str, dec->codec);
        kprintf("[AVI] Only uncompressed RGB is currently supported\n");
        // Don't fail - try anyway, might work for some formats
    }

    kprintf("[AVI] Decoder opened: %dx%d, %d bpp\n",
            dec->width, dec->height, dec->bit_depth);

    return dec;
}

int avi_decoder_info(avi_decoder_t *dec, video_info_t *info) {
    if (!dec || !info) {
        return VIDEO_ERR_NULL_PTR;
    }

    info->width = dec->width;
    info->height = dec->height;
    info->fps_num = (dec->frame_time > 0) ? (int)(1.0 / dec->frame_time * 1000) : 30000;
    info->fps_den = 1000;
    info->has_audio = (dec->audio_stream >= 0) ? 1 : 0;
    info->audio_sample_rate = dec->audio_sample_rate;
    info->audio_channels = dec->audio_channels;
    info->audio_bits = dec->audio_bits;
    info->duration = dec->main_header.total_frames * dec->frame_time;
    info->format = VIDEO_FORMAT_AVI;

    return VIDEO_SUCCESS;
}

int avi_decoder_frame(avi_decoder_t *dec, uint32_t *rgb_buffer) {
    if (!dec || !rgb_buffer) {
        return VIDEO_ERR_NULL_PTR;
    }

    if (dec->ended) {
        return VIDEO_ERR_EOF;
    }

    size_t frame_offset;
    uint32_t frame_size;

    // Use index if available
    if (dec->index && dec->index_count > 0) {
        // Find next video frame in index
        while (dec->current_frame < dec->index_count) {
            avi_index_entry_t *entry = &dec->index[dec->current_frame];

            if (is_video_chunk(entry->chunk_id, dec->video_stream)) {
                // Index offset is relative to movi start (before 'movi' type)
                // Actually it can be relative to file start or movi, we try both
                frame_offset = dec->movi_offset + entry->offset;
                frame_size = entry->size;

                // Sanity check
                if (frame_offset + frame_size <= dec->size) {
                    dec->current_frame++;
                    break;
                }

                // Try absolute offset
                frame_offset = entry->offset + 8;  // Skip chunk header
                if (frame_offset + frame_size <= dec->size) {
                    dec->current_frame++;
                    break;
                }
            }

            dec->current_frame++;
        }

        if (dec->current_frame >= dec->index_count) {
            dec->ended = true;
            return VIDEO_ERR_EOF;
        }
    } else {
        // Scan movi chunk for frames
        int result = find_next_video_frame(dec, &frame_offset, &frame_size);
        if (result != VIDEO_SUCCESS) {
            dec->ended = true;
            return result;
        }
        dec->current_frame++;
    }

    // Decode the frame
    int result = decode_raw_frame(dec, dec->data + frame_offset, frame_size, rgb_buffer);
    if (result == VIDEO_SUCCESS) {
        dec->current_time += dec->frame_time;
    }

    return result;
}

int avi_decoder_audio(avi_decoder_t *dec, int16_t *samples, int max_samples) {
    if (!dec || !samples) {
        return VIDEO_ERR_NULL_PTR;
    }

    if (dec->audio_stream < 0) {
        return VIDEO_ERR_NO_AUDIO;
    }

    // Audio decoding not yet fully implemented
    // Would need to scan movi for audio chunks and decode based on format
    return 0;
}

void avi_decoder_seek(avi_decoder_t *dec, double seconds) {
    if (!dec) return;

    if (seconds <= 0.0) {
        dec->current_frame = 0;
        dec->current_time = 0.0;
        dec->ended = false;
        return;
    }

    // Calculate target frame
    int target_frame = (int)(seconds / dec->frame_time);

    if (dec->index && dec->index_count > 0) {
        // Count video frames in index up to target
        int video_frame_count = 0;
        for (int i = 0; i < dec->index_count && video_frame_count < target_frame; i++) {
            if (is_video_chunk(dec->index[i].chunk_id, dec->video_stream)) {
                video_frame_count++;
                dec->current_frame = i + 1;
            }
        }
        dec->current_time = video_frame_count * dec->frame_time;
    } else {
        // Without index, we'd need to scan through the file
        // For now, just rewind
        dec->current_frame = 0;
        dec->current_time = 0.0;
    }

    dec->ended = false;
}

int avi_decoder_get_time_ms(avi_decoder_t *dec) {
    return dec ? (int)(dec->current_time * 1000) : 0;
}

int avi_decoder_has_ended(avi_decoder_t *dec) {
    return dec ? (dec->ended ? 1 : 0) : 1;
}

void avi_decoder_close(avi_decoder_t *dec) {
    if (!dec) return;

    kfree(dec->index);
    kfree(dec);
}
