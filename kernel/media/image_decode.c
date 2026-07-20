// image_decode.c - Unified Image Decoder for MayteraOS

#include "image_decode.h"
#define IMAGE_H  // Prevent duplicate image_t definition
#include "png.h"
#include "jpeg.h"
#include "webp.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"
#include "../video/framebuffer.h"

// Debug macro
#define IMG_DEBUG 0
#if IMG_DEBUG
#define IMG_DBG(fmt, ...) kprintf("[IMAGE] " fmt, ##__VA_ARGS__)
#else
#define IMG_DBG(fmt, ...) ((void)0)
#endif

// ============================================================================
// Format Detection
// ============================================================================

image_format_t image_detect_format(const void *data, uint32_t size) {
    if (!data || size < 4) return IMAGE_FORMAT_UNKNOWN;
    
    const uint8_t *p = (const uint8_t *)data;
    
    // BMP: "BM"
    if (size >= 2 && p[0] == 'B' && p[1] == 'M') {
        return IMAGE_FORMAT_BMP;
    }
    
    // PNG: 137 80 78 71 13 10 26 10
    if (size >= 8 && p[0] == 137 && p[1] == 80 && p[2] == 78 && p[3] == 71) {
        return IMAGE_FORMAT_PNG;
    }
    
    // JPEG: FF D8 FF
    if (size >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) {
        return IMAGE_FORMAT_JPEG;
    }
    
    // WebP: RIFF....WEBP
    if (size >= 12) {
        if (p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F' &&
            p[8] == 'W' && p[9] == 'E' && p[10] == 'B' && p[11] == 'P') {
            return IMAGE_FORMAT_WEBP;
        }
    }
    
    // GIF: GIF87a or GIF89a
    if (size >= 6 && p[0] == 'G' && p[1] == 'I' && p[2] == 'F') {
        return IMAGE_FORMAT_GIF;
    }
    
    // TIFF: II or MM
    if (size >= 4) {
        if ((p[0] == 'I' && p[1] == 'I' && p[2] == 0x2A && p[3] == 0x00) ||
            (p[0] == 'M' && p[1] == 'M' && p[2] == 0x00 && p[3] == 0x2A)) {
            return IMAGE_FORMAT_TIFF;
        }
    }
    
    // ICO: 00 00 01 00
    if (size >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] == 0) {
        return IMAGE_FORMAT_ICO;
    }
    
    return IMAGE_FORMAT_UNKNOWN;
}

const char *image_format_name(image_format_t format) {
    switch (format) {
        case IMAGE_FORMAT_BMP:  return "BMP";
        case IMAGE_FORMAT_PNG:  return "PNG";
        case IMAGE_FORMAT_JPEG: return "JPEG";
        case IMAGE_FORMAT_WEBP: return "WebP";
        case IMAGE_FORMAT_GIF:  return "GIF";
        case IMAGE_FORMAT_TIFF: return "TIFF";
        case IMAGE_FORMAT_ICO:  return "ICO";
        default:                return "Unknown";
    }
}

const char *image_format_extension(image_format_t format) {
    switch (format) {
        case IMAGE_FORMAT_BMP:  return ".bmp";
        case IMAGE_FORMAT_PNG:  return ".png";
        case IMAGE_FORMAT_JPEG: return ".jpg";
        case IMAGE_FORMAT_WEBP: return ".webp";
        case IMAGE_FORMAT_GIF:  return ".gif";
        case IMAGE_FORMAT_TIFF: return ".tiff";
        case IMAGE_FORMAT_ICO:  return ".ico";
        default:                return "";
    }
}

// ============================================================================
// BMP Decoder
// ============================================================================

#define BMP_SIGNATURE 0x4D42  // "BM" in little-endian

int image_decode_bmp(const void *data, uint32_t size, image_t *img) {
    if (!data || !img) return IMAGE_ERR_NULL_PTR;
    
    img->width = 0;
    img->height = 0;
    img->pixels = NULL;
    
    if (size < sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t)) {
        return IMAGE_ERR_TOO_SMALL;
    }
    
    const uint8_t *raw = (const uint8_t *)data;
    const bmp_file_header_t *file_hdr = (const bmp_file_header_t *)raw;
    const bmp_info_header_t *info_hdr = (const bmp_info_header_t *)(raw + sizeof(bmp_file_header_t));
    
    if (file_hdr->signature != BMP_SIGNATURE) {
        return IMAGE_ERR_INVALID_SIG;
    }
    
    if (info_hdr->header_size < 40) {
        return IMAGE_ERR_UNSUPPORTED;
    }
    
    int32_t width = info_hdr->width;
    int32_t height = info_hdr->height;
    int top_down = 0;
    
    if (height < 0) {
        height = -height;
        top_down = 1;
    }
    
    if (width <= 0 || height <= 0) {
        return IMAGE_ERR_CORRUPT;
    }
    
    if (info_hdr->bpp != 24 && info_hdr->bpp != 32) {
        IMG_DBG("BMP: unsupported bpp %d\n", info_hdr->bpp);
        return IMAGE_ERR_UNSUPPORTED;
    }
    
    if (info_hdr->compression != 0 && info_hdr->compression != 3) {
        return IMAGE_ERR_UNSUPPORTED;
    }
    
    if (file_hdr->data_offset >= size) {
        return IMAGE_ERR_CORRUPT;
    }
    
    uint32_t bpp = info_hdr->bpp / 8;
    uint32_t row_size_unpadded = (uint32_t)width * bpp;
    uint32_t row_padding = (4 - (row_size_unpadded % 4)) % 4;
    uint32_t row_size = row_size_unpadded + row_padding;
    
    uint32_t pixel_data_size = row_size * (uint32_t)height;
    if (file_hdr->data_offset + pixel_data_size > size) {
        return IMAGE_ERR_CORRUPT;
    }
    
    uint32_t *pixels = kmalloc((uint32_t)width * (uint32_t)height * sizeof(uint32_t));
    if (!pixels) return IMAGE_ERR_NOMEM;
    
    const uint8_t *src_data = raw + file_hdr->data_offset;
    
    for (int32_t y = 0; y < height; y++) {
        int32_t src_y = top_down ? y : (height - 1 - y);
        const uint8_t *src_row = src_data + src_y * row_size;
        uint32_t *dst_row = pixels + y * width;
        
        for (int32_t x = 0; x < width; x++) {
            const uint8_t *src_pixel = src_row + x * bpp;
            uint8_t b = src_pixel[0];
            uint8_t g = src_pixel[1];
            uint8_t r = src_pixel[2];
            uint8_t a = (bpp == 4) ? src_pixel[3] : 255;
            
            dst_row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) | b;
        }
    }
    
    img->width = (uint32_t)width;
    img->height = (uint32_t)height;
    img->pixels = pixels;
    
    IMG_DBG("BMP: decoded %dx%d\n", width, height);
    
    return IMAGE_OK;
}

// ============================================================================
// Main Decode Functions
// ============================================================================

int image_decode_format(const void *data, uint32_t size,
                        image_format_t format, image_t *img) {
    if (!data || !img) return IMAGE_ERR_NULL_PTR;
    
    img->width = 0;
    img->height = 0;
    img->pixels = NULL;
    
    int ret;
    
    switch (format) {
        case IMAGE_FORMAT_BMP:
            ret = image_decode_bmp(data, size, img);
            break;
            
        case IMAGE_FORMAT_PNG:
            ret = png_decode(data, size, img);
            if (ret != PNG_OK) {
                ret = IMAGE_ERR_DECODE;
            }
            break;
            
        case IMAGE_FORMAT_JPEG:
            ret = jpeg_decode(data, size, img);
            if (ret != JPEG_OK) {
                ret = IMAGE_ERR_DECODE;
            }
            break;
            
        case IMAGE_FORMAT_WEBP:
            ret = webp_decode(data, size, img);
            if (ret != WEBP_OK) {
                ret = IMAGE_ERR_DECODE;
            }
            break;
            
        default:
            ret = IMAGE_ERR_UNSUPPORTED;
    }
    
    return ret;
}

int image_decode(const void *data, uint32_t size, image_t *img) {
    if (!data || !img) return IMAGE_ERR_NULL_PTR;
    
    image_format_t format = image_detect_format(data, size);
    if (format == IMAGE_FORMAT_UNKNOWN) {
        return IMAGE_ERR_UNSUPPORTED;
    }
    
    return image_decode_format(data, size, format, img);
}

int image_get_info(const void *data, uint32_t size,
                   uint32_t *width, uint32_t *height,
                   image_format_t *format) {
    if (!data) return IMAGE_ERR_NULL_PTR;
    
    image_format_t fmt = image_detect_format(data, size);
    if (format) *format = fmt;
    
    uint32_t w = 0, h = 0;
    int ret = IMAGE_OK;
    
    switch (fmt) {
        case IMAGE_FORMAT_BMP:
            if (size >= sizeof(bmp_file_header_t) + sizeof(bmp_info_header_t)) {
                const bmp_info_header_t *info = (const bmp_info_header_t *)
                    ((const uint8_t *)data + sizeof(bmp_file_header_t));
                w = info->width > 0 ? info->width : -info->width;
                h = info->height > 0 ? info->height : -info->height;
            }
            break;
            
        case IMAGE_FORMAT_PNG:
            ret = png_get_info(data, size, &w, &h, NULL, NULL);
            break;
            
        case IMAGE_FORMAT_JPEG:
            ret = jpeg_get_info(data, size, &w, &h, NULL);
            break;
            
        case IMAGE_FORMAT_WEBP:
            ret = webp_get_info(data, size, &w, &h, NULL, NULL);
            break;
            
        default:
            ret = IMAGE_ERR_UNSUPPORTED;
    }
    
    if (width) *width = w;
    if (height) *height = h;
    
    return ret;
}

void image_free(image_t *img) {
    if (!img) return;
    
    if (img->pixels) {
        kfree(img->pixels);
        img->pixels = NULL;
    }
    
    img->width = 0;
    img->height = 0;
}

const char *image_error_string(int err) {
    switch (err) {
        case IMAGE_OK:             return "Success";
        case IMAGE_ERR_NULL_PTR:   return "Null pointer";
        case IMAGE_ERR_INVALID_SIG: return "Invalid signature";
        case IMAGE_ERR_UNSUPPORTED: return "Unsupported format";
        case IMAGE_ERR_NOMEM:      return "Out of memory";
        case IMAGE_ERR_CORRUPT:    return "Corrupt data";
        case IMAGE_ERR_TOO_SMALL:  return "Data too small";
        case IMAGE_ERR_DECODE:     return "Decode error";
        default:                   return "Unknown error";
    }
}

// ============================================================================
// Image Manipulation
// ============================================================================

int image_scale(const image_t *src, image_t *dst,
                uint32_t width, uint32_t height) {
    if (!src || !dst || !src->pixels) return IMAGE_ERR_NULL_PTR;
    if (width == 0 || height == 0) return IMAGE_ERR_CORRUPT;
    
    dst->pixels = kmalloc(width * height * sizeof(uint32_t));
    if (!dst->pixels) return IMAGE_ERR_NOMEM;
    
    dst->width = width;
    dst->height = height;
    
    // Nearest neighbor scaling
    uint32_t x_ratio = ((src->width << 16) / width);
    uint32_t y_ratio = ((src->height << 16) / height);
    
    for (uint32_t y = 0; y < height; y++) {
        uint32_t src_y = (y * y_ratio) >> 16;
        if (src_y >= src->height) src_y = src->height - 1;
        
        const uint32_t *src_row = src->pixels + src_y * src->width;
        uint32_t *dst_row = dst->pixels + y * width;
        
        for (uint32_t x = 0; x < width; x++) {
            uint32_t src_x = (x * x_ratio) >> 16;
            if (src_x >= src->width) src_x = src->width - 1;
            dst_row[x] = src_row[src_x];
        }
    }
    
    return IMAGE_OK;
}

int image_thumbnail(const image_t *src, image_t *thumb, uint32_t max_size) {
    if (!src || !thumb) return IMAGE_ERR_NULL_PTR;
    
    uint32_t new_w, new_h;
    
    if (src->width > src->height) {
        new_w = max_size;
        new_h = (src->height * max_size) / src->width;
        if (new_h == 0) new_h = 1;
    } else {
        new_h = max_size;
        new_w = (src->width * max_size) / src->height;
        if (new_w == 0) new_w = 1;
    }
    
    return image_scale(src, thumb, new_w, new_h);
}

int image_copy(const image_t *src, image_t *dst) {
    if (!src || !dst || !src->pixels) return IMAGE_ERR_NULL_PTR;
    
    size_t size = src->width * src->height * sizeof(uint32_t);
    dst->pixels = kmalloc(size);
    if (!dst->pixels) return IMAGE_ERR_NOMEM;
    
    memcpy(dst->pixels, src->pixels, size);
    dst->width = src->width;
    dst->height = src->height;
    
    return IMAGE_OK;
}

void image_flip_vertical(image_t *img) {
    if (!img || !img->pixels) return;
    
    uint32_t *row = kmalloc(img->width * sizeof(uint32_t));
    if (!row) return;
    
    for (uint32_t y = 0; y < img->height / 2; y++) {
        uint32_t *top = img->pixels + y * img->width;
        uint32_t *bot = img->pixels + (img->height - 1 - y) * img->width;
        
        memcpy(row, top, img->width * sizeof(uint32_t));
        memcpy(top, bot, img->width * sizeof(uint32_t));
        memcpy(bot, row, img->width * sizeof(uint32_t));
    }
    
    kfree(row);
}

void image_flip_horizontal(image_t *img) {
    if (!img || !img->pixels) return;
    
    for (uint32_t y = 0; y < img->height; y++) {
        uint32_t *row = img->pixels + y * img->width;
        for (uint32_t x = 0; x < img->width / 2; x++) {
            uint32_t tmp = row[x];
            row[x] = row[img->width - 1 - x];
            row[img->width - 1 - x] = tmp;
        }
    }
}

// ============================================================================
// Image Display
// ============================================================================

void image_blit(const image_t *img, int x, int y) {
    if (!img || !img->pixels || img->width == 0 || img->height == 0) {
        return;
    }
    
    // Handle negative coordinates
    if (x < 0 || y < 0) {
        uint32_t sx = (x < 0) ? (uint32_t)(-x) : 0;
        uint32_t sy = (y < 0) ? (uint32_t)(-y) : 0;
        int dx = (x < 0) ? 0 : x;
        int dy = (y < 0) ? 0 : y;
        
        if (sx >= img->width || sy >= img->height) {
            return;
        }
        
        image_blit_region(img, dx, dy, sx, sy,
                          img->width - sx, img->height - sy);
    } else {
        fb_blit((uint32_t)x, (uint32_t)y, img->width, img->height, img->pixels);
    }
}

void image_blit_region(const image_t *img, int dx, int dy,
                       uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh) {
    if (!img || !img->pixels) return;
    
    if (sx >= img->width || sy >= img->height) return;
    
    if (sx + sw > img->width) sw = img->width - sx;
    if (sy + sh > img->height) sh = img->height - sy;
    
    if (sw == 0 || sh == 0) return;
    
    if (dx < 0) {
        uint32_t skip = (uint32_t)(-dx);
        if (skip >= sw) return;
        sx += skip;
        sw -= skip;
        dx = 0;
    }
    if (dy < 0) {
        uint32_t skip = (uint32_t)(-dy);
        if (skip >= sh) return;
        sy += skip;
        sh -= skip;
        dy = 0;
    }
    
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();
    
    if ((uint32_t)dx >= fb_w || (uint32_t)dy >= fb_h) return;
    
    if ((uint32_t)dx + sw > fb_w) sw = fb_w - (uint32_t)dx;
    if ((uint32_t)dy + sh > fb_h) sh = fb_h - (uint32_t)dy;
    
    for (uint32_t y = 0; y < sh; y++) {
        const uint32_t *src_row = img->pixels + (sy + y) * img->width + sx;
        for (uint32_t x = 0; x < sw; x++) {
            fb_put_pixel((uint32_t)dx + x, (uint32_t)dy + y, src_row[x]);
        }
    }
}

void image_blit_scaled(const image_t *img, int dx, int dy,
                       uint32_t dw, uint32_t dh) {
    if (!img || !img->pixels || img->width == 0 || img->height == 0) {
        return;
    }
    if (dw == 0 || dh == 0) return;
    
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();
    
    if (dx >= (int)fb_w || dy >= (int)fb_h) return;
    
    uint32_t x_ratio = ((img->width << 16) / dw);
    uint32_t y_ratio = ((img->height << 16) / dh);
    
    uint32_t start_x = 0, start_y = 0;
    uint32_t dest_x_start = (dx < 0) ? 0 : (uint32_t)dx;
    uint32_t dest_y_start = (dy < 0) ? 0 : (uint32_t)dy;
    
    if (dx < 0) start_x = (uint32_t)(-dx);
    if (dy < 0) start_y = (uint32_t)(-dy);
    
    uint32_t visible_w = dw - start_x;
    uint32_t visible_h = dh - start_y;
    
    if (dest_x_start + visible_w > fb_w) visible_w = fb_w - dest_x_start;
    if (dest_y_start + visible_h > fb_h) visible_h = fb_h - dest_y_start;
    
    for (uint32_t y = 0; y < visible_h; y++) {
        uint32_t src_y = ((start_y + y) * y_ratio) >> 16;
        if (src_y >= img->height) src_y = img->height - 1;
        
        const uint32_t *src_row = img->pixels + src_y * img->width;
        
        for (uint32_t x = 0; x < visible_w; x++) {
            uint32_t src_x = ((start_x + x) * x_ratio) >> 16;
            if (src_x >= img->width) src_x = img->width - 1;
            
            fb_put_pixel(dest_x_start + x, dest_y_start + y, src_row[src_x]);
        }
    }
}

void image_blit_alpha(const image_t *img, int x, int y) {
    if (!img || !img->pixels) return;
    
    uint32_t fb_w = fb_get_width();
    uint32_t fb_h = fb_get_height();
    
    for (uint32_t iy = 0; iy < img->height; iy++) {
        int dy = y + iy;
        if (dy < 0) continue;
        if ((uint32_t)dy >= fb_h) break;
        
        for (uint32_t ix = 0; ix < img->width; ix++) {
            int dx = x + ix;
            if (dx < 0) continue;
            if ((uint32_t)dx >= fb_w) break;
            
            uint32_t src = img->pixels[iy * img->width + ix];
            uint8_t alpha = (src >> 24) & 0xFF;
            
            if (alpha == 0) continue;
            
            if (alpha == 255) {
                fb_put_pixel((uint32_t)dx, (uint32_t)dy, src);
            } else {
                // Alpha blend
                uint32_t dst = fb_get_pixel((uint32_t)dx, (uint32_t)dy);
                
                uint8_t sr = (src >> 16) & 0xFF;
                uint8_t sg = (src >> 8) & 0xFF;
                uint8_t sb = src & 0xFF;
                
                uint8_t dr = (dst >> 16) & 0xFF;
                uint8_t dg = (dst >> 8) & 0xFF;
                uint8_t db = dst & 0xFF;
                
                uint8_t r = (sr * alpha + dr * (255 - alpha)) / 255;
                uint8_t g = (sg * alpha + dg * (255 - alpha)) / 255;
                uint8_t b = (sb * alpha + db * (255 - alpha)) / 255;
                
                fb_put_pixel((uint32_t)dx, (uint32_t)dy,
                            0xFF000000 | (r << 16) | (g << 8) | b);
            }
        }
    }
}
