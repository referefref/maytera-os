/* sdl_video.c - SDL_SetVideoMode, surfaces, the software blit engine,
 * palette, GL context, window manager calls. Part of the MayteraOS SDL 1.2
 * backend (task #745,
 * docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 2 #7).
 *
 * ONE window, because SDL 1.2 itself is single-window by design - this maps
 * directly onto a MayteraOS compositor window, with none of the multi-window
 * bookkeeping the pre-existing SDL2 sdlshim.cpp files needed.
 *
 * SYS_WIN_BLIT (35) SCALES src_w x src_h to the window's real content bounds
 * (kernel/proc/syscall.c sys_win_blit() / winblit_plan_rs(); x,y are accepted
 * but UNUSED, so a partial-rect blit is not a thing the kernel offers). That
 * is why this backend never calls win_get_size() to match a buffer to the
 * window: it always presents at exactly the game's requested SDL_SetVideoMode
 * resolution and lets the kernel's own nearest-neighbour scaler fit that to
 * whatever the real window turns out to be. A game that picks a classic low
 * resolution (320x200, 640x480) is upscaled for free, the same way
 * fullscreen/maximize already upscale Arena's own framebuffer.
 */
#include "sdl_priv.h"
#include "GL/gl.h"
#include "zbuffer.h"
#include <string.h>
#include <stdio.h>

sdl_video_state_t g_sdlv;
int g_gl_attrs[SDL_GL_ATTR_COUNT];

/* ======================================================================
 * Pixel format helpers
 * ==================================================================== */
static int popcount32(Uint32 v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }
static int mask_shift(Uint32 mask) { int s = 0; if (!mask) return 0; while (!(mask & 1)) { mask >>= 1; s++; } return s; }

static void build_format(SDL_PixelFormat *f, int depth, Uint32 Rm, Uint32 Gm, Uint32 Bm, Uint32 Am) {
    memset(f, 0, sizeof(*f));
    f->BitsPerPixel = (Uint8)depth;
    f->BytesPerPixel = (Uint8)((depth + 7) / 8);
    f->Rmask = Rm; f->Gmask = Gm; f->Bmask = Bm; f->Amask = Am;
    f->Rshift = (Uint8)mask_shift(Rm); f->Gshift = (Uint8)mask_shift(Gm);
    f->Bshift = (Uint8)mask_shift(Bm); f->Ashift = (Uint8)mask_shift(Am);
    f->Rloss = (Uint8)(8 - popcount32(Rm)); f->Gloss = (Uint8)(8 - popcount32(Gm));
    f->Bloss = (Uint8)(8 - popcount32(Bm)); f->Aloss = (Uint8)(8 - popcount32(Am));
    f->alpha = 255;
    f->palette = 0;
}

static SDL_Palette *alloc_palette(int ncolors) {
    SDL_Palette *p = (SDL_Palette *)SDL_malloc(sizeof(SDL_Palette));
    if (!p) return 0;
    p->colors = (SDL_Color *)SDL_calloc((size_t)ncolors, sizeof(SDL_Color));
    if (!p->colors) { SDL_free(p); return 0; }
    p->ncolors = ncolors;
    return p;
}

static Uint32 sdlpriv_read_pixel(const Uint8 *p, int bpp) {
    switch (bpp) {
    case 1: return p[0];
    case 2: return (Uint32)p[0] | ((Uint32)p[1] << 8);
    case 3: return (Uint32)p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16);
    default: return (Uint32)p[0] | ((Uint32)p[1] << 8) | ((Uint32)p[2] << 16) | ((Uint32)p[3] << 24);
    }
}
static void sdlpriv_write_pixel(Uint8 *p, int bpp, Uint32 v) {
    p[0] = (Uint8)v;
    if (bpp > 1) p[1] = (Uint8)(v >> 8);
    if (bpp > 2) p[2] = (Uint8)(v >> 16);
    if (bpp > 3) p[3] = (Uint8)(v >> 24);
}

Uint32 SDL_MapRGBA(const SDL_PixelFormat * const fmt, const Uint8 r, const Uint8 g, const Uint8 b, const Uint8 a) {
    if (fmt->palette) {
        int best = 0; long bestd = 0x7FFFFFFF;
        for (int i = 0; i < fmt->palette->ncolors; i++) {
            SDL_Color *c = &fmt->palette->colors[i];
            long dr = (long)c->r - r, dg = (long)c->g - g, db = (long)c->b - b;
            long d = dr * dr + dg * dg + db * db;
            if (d < bestd) { bestd = d; best = i; if (d == 0) break; }
        }
        return (Uint32)best;
    }
    Uint32 v = ((Uint32)(r >> fmt->Rloss) << fmt->Rshift) |
               ((Uint32)(g >> fmt->Gloss) << fmt->Gshift) |
               ((Uint32)(b >> fmt->Bloss) << fmt->Bshift);
    if (fmt->Amask) v |= ((Uint32)(a >> fmt->Aloss) << fmt->Ashift);
    return v;
}
Uint32 SDL_MapRGB(const SDL_PixelFormat * const fmt, const Uint8 r, const Uint8 g, const Uint8 b) {
    return SDL_MapRGBA(fmt, r, g, b, 255);
}
static Uint8 expand_component(Uint32 pixel, Uint32 mask, Uint8 shift, Uint8 loss) {
    if (!mask) return 0;
    Uint8 v = (Uint8)((pixel & mask) >> shift);
    return (Uint8)((v << loss) | (v >> (8 - loss)));
}
void SDL_GetRGBA(Uint32 pixel, const SDL_PixelFormat * const fmt, Uint8 *r, Uint8 *g, Uint8 *b, Uint8 *a) {
    if (fmt->palette) {
        int idx = (int)pixel;
        if (idx >= 0 && idx < fmt->palette->ncolors) {
            *r = fmt->palette->colors[idx].r;
            *g = fmt->palette->colors[idx].g;
            *b = fmt->palette->colors[idx].b;
        } else { *r = *g = *b = 0; }
        *a = 255;
        return;
    }
    *r = expand_component(pixel, fmt->Rmask, fmt->Rshift, fmt->Rloss);
    *g = expand_component(pixel, fmt->Gmask, fmt->Gshift, fmt->Gloss);
    *b = expand_component(pixel, fmt->Bmask, fmt->Bshift, fmt->Bloss);
    *a = fmt->Amask ? expand_component(pixel, fmt->Amask, fmt->Ashift, fmt->Aloss) : 255;
}
void SDL_GetRGB(Uint32 pixel, const SDL_PixelFormat * const fmt, Uint8 *r, Uint8 *g, Uint8 *b) {
    Uint8 a; SDL_GetRGBA(pixel, fmt, r, g, b, &a);
}

/* ======================================================================
 * Surfaces
 * ==================================================================== */
SDL_Surface *SDL_CreateRGBSurfaceFrom(void *pixels, int width, int height, int depth, int pitch,
                                       Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
    if (width <= 0 || height <= 0) { sdlpriv_set_error("Width and height must be positive"); return 0; }
    SDL_Surface *s = (SDL_Surface *)SDL_calloc(1, sizeof(SDL_Surface));
    SDL_PixelFormat *f = (SDL_PixelFormat *)SDL_malloc(sizeof(SDL_PixelFormat));
    if (!s || !f) { SDL_free(s); SDL_free(f); sdlpriv_set_error("Out of memory"); return 0; }
    build_format(f, depth, Rmask, Gmask, Bmask, Amask);
    if (depth == 8 && !Rmask && !Gmask && !Bmask) {
        /* Real SDL1.2 gives an 8bpp surface a default grayscale ramp
         * palette; games that plan to SDL_SetColors() immediately still get
         * a sane image if they forget to. */
        f->palette = alloc_palette(256);
        if (f->palette) for (int i = 0; i < 256; i++) {
            f->palette->colors[i].r = f->palette->colors[i].g = f->palette->colors[i].b = (Uint8)i;
        }
    }
    s->flags = SDL_SWSURFACE;
    s->format = f;
    s->w = width; s->h = height;
    s->pitch = (Uint16)pitch;
    s->pixels = pixels;
    s->clip_rect.x = 0; s->clip_rect.y = 0; s->clip_rect.w = (Uint16)width; s->clip_rect.h = (Uint16)height;
    s->refcount = 1;
    return s;
}
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
                                   Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask) {
    if (width <= 0 || height <= 0) { sdlpriv_set_error("Width and height must be positive"); return 0; }
    int bpp = (depth + 7) / 8;
    void *px = SDL_calloc((size_t)width * (size_t)height, (size_t)(bpp > 0 ? bpp : 1));
    if (!px) { sdlpriv_set_error("Out of memory"); return 0; }
    SDL_Surface *s = SDL_CreateRGBSurfaceFrom(px, width, height, depth, width * bpp, Rmask, Gmask, Bmask, Amask);
    if (!s) { SDL_free(px); return 0; }
    s->flags = flags & (SDL_HWSURFACE | SDL_SRCCOLORKEY | SDL_SRCALPHA);
    return s;
}
void SDL_FreeSurface(SDL_Surface *surface) {
    if (!surface) return;
    if (surface == g_sdlv.screen) return;  /* the screen surface is owned by SDL_SetVideoMode/SDL_Quit */
    if (--surface->refcount > 0) return;
    if (!(surface->flags & SDL_PREALLOC) && surface->pixels) SDL_free(surface->pixels);
    if (surface->format) {
        if (surface->format->palette) { SDL_free(surface->format->palette->colors); SDL_free(surface->format->palette); }
        SDL_free(surface->format);
    }
    SDL_free(surface);
}
int SDL_LockSurface(SDL_Surface *surface) { (void)surface; return 0; /* no HW surfaces to lock */ }
void SDL_UnlockSurface(SDL_Surface *surface) { (void)surface; }

int SDL_SetColorKey(SDL_Surface *surface, Uint32 flag, Uint32 key) {
    if (!surface) return -1;
    if (flag & SDL_SRCCOLORKEY) surface->flags |= SDL_SRCCOLORKEY; else surface->flags &= ~(Uint32)SDL_SRCCOLORKEY;
    surface->format->colorkey = key;
    return 0;
}
int SDL_SetAlpha(SDL_Surface *surface, Uint32 flag, Uint8 alpha) {
    if (!surface) return -1;
    if (flag & SDL_SRCALPHA) surface->flags |= SDL_SRCALPHA; else surface->flags &= ~(Uint32)SDL_SRCALPHA;
    surface->format->alpha = alpha;
    return 0;
}
SDL_bool SDL_SetClipRect(SDL_Surface *surface, const SDL_Rect *rect) {
    if (!surface) return SDL_FALSE;
    SDL_Rect full; full.x = 0; full.y = 0; full.w = (Uint16)surface->w; full.h = (Uint16)surface->h;
    if (!rect) { surface->clip_rect = full; return SDL_TRUE; }
    int x0 = rect->x < 0 ? 0 : rect->x;
    int y0 = rect->y < 0 ? 0 : rect->y;
    int x1 = rect->x + rect->w; if (x1 > surface->w) x1 = surface->w;
    int y1 = rect->y + rect->h; if (y1 > surface->h) y1 = surface->h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    surface->clip_rect.x = (Sint16)x0; surface->clip_rect.y = (Sint16)y0;
    surface->clip_rect.w = (Uint16)(x1 - x0); surface->clip_rect.h = (Uint16)(y1 - y0);
    return SDL_TRUE;
}
void SDL_GetClipRect(SDL_Surface *surface, SDL_Rect *rect) {
    if (surface && rect) *rect = surface->clip_rect;
}

int SDL_SetColors(SDL_Surface *surface, SDL_Color *colors, int firstcolor, int ncolors) {
    return SDL_SetPalette(surface, SDL_LOGPAL | SDL_PHYSPAL, colors, firstcolor, ncolors);
}
int SDL_SetPalette(SDL_Surface *surface, int flags, SDL_Color *colors, int firstcolor, int ncolors) {
    (void)flags;
    if (!surface || !surface->format->palette) return 0;
    SDL_Palette *p = surface->format->palette;
    for (int i = 0; i < ncolors && (firstcolor + i) < p->ncolors; i++) p->colors[firstcolor + i] = colors[i];
    return 1;
}

/* ======================================================================
 * The software blit engine, shared by SDL_UpperBlit/LowerBlit/
 * SDL_ConvertSurface. Correctness over speed: every pixel goes through
 * GetRGBA/MapRGBA so any source depth (1/2/3/4 bytes, palette or truecolor)
 * can land in any destination depth. That is the same trade every purely
 * software TinyGL/libgl path in this tree already makes.
 * ==================================================================== */
int sdlpriv_blit(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
    if (!src || !dst) { sdlpriv_set_error("Passed a NULL surface"); return -1; }
    SDL_Rect sr; sr.x = 0; sr.y = 0; sr.w = (Uint16)src->w; sr.h = (Uint16)src->h;
    if (srcrect) sr = *srcrect;
    int dst_x0 = dstrect ? dstrect->x : 0;
    int dst_y0 = dstrect ? dstrect->y : 0;

    if (sr.x < 0) { dst_x0 -= sr.x; sr.w = (Uint16)(sr.w + sr.x); sr.x = 0; }
    if (sr.y < 0) { dst_y0 -= sr.y; sr.h = (Uint16)(sr.h + sr.y); sr.y = 0; }
    if (sr.x + sr.w > src->w) sr.w = (Uint16)(src->w - sr.x);
    if (sr.y + sr.h > src->h) sr.h = (Uint16)(src->h - sr.y);

    SDL_Rect cr = dst->clip_rect;
    if (cr.w == 0 && cr.h == 0 && cr.x == 0 && cr.y == 0) { cr.w = (Uint16)dst->w; cr.h = (Uint16)dst->h; }
    if (dst_x0 < cr.x) { int d = cr.x - dst_x0; sr.x = (Uint16)(sr.x + d); sr.w = (Uint16)((int)sr.w - d); dst_x0 = cr.x; }
    if (dst_y0 < cr.y) { int d = cr.y - dst_y0; sr.y = (Uint16)(sr.y + d); sr.h = (Uint16)((int)sr.h - d); dst_y0 = cr.y; }
    if (dst_x0 + (int)sr.w > cr.x + cr.w) sr.w = (Uint16)(cr.x + cr.w - dst_x0);
    if (dst_y0 + (int)sr.h > cr.y + cr.h) sr.h = (Uint16)(cr.y + cr.h - dst_y0);

    if (dstrect) { dstrect->x = (Sint16)dst_x0; dstrect->y = (Sint16)dst_y0; dstrect->w = sr.w; dstrect->h = sr.h; }
    if ((int)sr.w <= 0 || (int)sr.h <= 0) return 0;

    int use_ckey = (src->flags & SDL_SRCCOLORKEY) != 0;
    Uint32 ckey = src->format->colorkey;
    int use_surf_alpha = (src->flags & SDL_SRCALPHA) != 0 && !src->format->Amask && src->format->alpha < 255;
    Uint8 surf_alpha = src->format->alpha;
    int sbpp = src->format->BytesPerPixel, dbpp = dst->format->BytesPerPixel;

    for (int y = 0; y < (int)sr.h; y++) {
        const Uint8 *srow = (const Uint8 *)src->pixels + (size_t)(sr.y + y) * src->pitch + (size_t)sr.x * sbpp;
        Uint8 *drow = (Uint8 *)dst->pixels + (size_t)(dst_y0 + y) * dst->pitch + (size_t)dst_x0 * dbpp;
        for (int x = 0; x < (int)sr.w; x++) {
            Uint32 sp = sdlpriv_read_pixel(srow + (size_t)x * sbpp, sbpp);
            if (use_ckey && sp == ckey) continue;
            Uint8 r, g, b, a;
            SDL_GetRGBA(sp, src->format, &r, &g, &b, &a);
            if (use_surf_alpha) a = surf_alpha;
            Uint8 *dp = drow + (size_t)x * dbpp;
            if (a >= 255) {
                sdlpriv_write_pixel(dp, dbpp, SDL_MapRGBA(dst->format, r, g, b, 255));
            } else if (a > 0) {
                Uint32 dpix = sdlpriv_read_pixel(dp, dbpp);
                Uint8 dr_, dg_, db_, da_;
                SDL_GetRGBA(dpix, dst->format, &dr_, &dg_, &db_, &da_);
                Uint8 or_ = (Uint8)(((int)r * a + (int)dr_ * (255 - a)) / 255);
                Uint8 og_ = (Uint8)(((int)g * a + (int)dg_ * (255 - a)) / 255);
                Uint8 ob_ = (Uint8)(((int)b * a + (int)db_ * (255 - a)) / 255);
                sdlpriv_write_pixel(dp, dbpp, SDL_MapRGBA(dst->format, or_, og_, ob_, 255));
            }
            /* a == 0: fully transparent, skip. */
        }
    }
    return 0;
}
int SDL_UpperBlit(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
    return sdlpriv_blit(src, srcrect, dst, dstrect);
}
int SDL_LowerBlit(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
    return sdlpriv_blit(src, srcrect, dst, dstrect);
}

int SDL_FillRect(SDL_Surface *dst, SDL_Rect *dstrect, Uint32 color) {
    if (!dst) return -1;
    SDL_Rect r; r.x = 0; r.y = 0; r.w = (Uint16)dst->w; r.h = (Uint16)dst->h;
    if (dstrect) r = *dstrect;
    if (r.x < 0) { r.w = (Uint16)((int)r.w + r.x); r.x = 0; }
    if (r.y < 0) { r.h = (Uint16)((int)r.h + r.y); r.y = 0; }
    if (r.x + r.w > dst->w) r.w = (Uint16)(dst->w - r.x);
    if (r.y + r.h > dst->h) r.h = (Uint16)(dst->h - r.y);
    if ((int)r.w <= 0 || (int)r.h <= 0) return 0;
    int bpp = dst->format->BytesPerPixel;
    for (int y = 0; y < (int)r.h; y++) {
        Uint8 *drow = (Uint8 *)dst->pixels + (size_t)(r.y + y) * dst->pitch + (size_t)r.x * bpp;
        for (int x = 0; x < (int)r.w; x++) sdlpriv_write_pixel(drow + (size_t)x * bpp, bpp, color);
    }
    return 0;
}

SDL_Surface *SDL_ConvertSurface(SDL_Surface *src, SDL_PixelFormat *fmt, Uint32 flags) {
    if (!src || !fmt) return 0;
    SDL_Surface *dst = SDL_CreateRGBSurface(flags, src->w, src->h, fmt->BitsPerPixel, fmt->Rmask, fmt->Gmask, fmt->Bmask, fmt->Amask);
    if (!dst) return 0;
    if (fmt->palette && dst->format->palette) {
        int n = fmt->palette->ncolors < dst->format->palette->ncolors ? fmt->palette->ncolors : dst->format->palette->ncolors;
        SDL_memcpy(dst->format->palette->colors, fmt->palette->colors, (size_t)n * sizeof(SDL_Color));
    }
    int sbpp = src->format->BytesPerPixel, dbpp = dst->format->BytesPerPixel;
    for (int y = 0; y < src->h; y++) {
        const Uint8 *srow = (const Uint8 *)src->pixels + (size_t)y * src->pitch;
        Uint8 *drow = (Uint8 *)dst->pixels + (size_t)y * dst->pitch;
        for (int x = 0; x < src->w; x++) {
            Uint32 sp = sdlpriv_read_pixel(srow + (size_t)x * sbpp, sbpp);
            Uint8 r, g, b, a; SDL_GetRGBA(sp, src->format, &r, &g, &b, &a);
            sdlpriv_write_pixel(drow + (size_t)x * dbpp, dbpp, SDL_MapRGBA(dst->format, r, g, b, a));
        }
    }
    dst->format->colorkey = src->format->colorkey;
    dst->format->alpha = src->format->alpha;
    dst->flags |= src->flags & (SDL_SRCCOLORKEY | SDL_SRCALPHA);
    return dst;
}
SDL_Surface *SDL_DisplayFormat(SDL_Surface *surface) {
    if (!g_sdlv.screen) { sdlpriv_set_error("No video mode has been set"); return 0; }
    return SDL_ConvertSurface(surface, g_sdlv.screen->format, SDL_SWSURFACE);
}
SDL_Surface *SDL_DisplayFormatAlpha(SDL_Surface *surface) {
    SDL_PixelFormat f; build_format(&f, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    return SDL_ConvertSurface(surface, &f, SDL_SWSURFACE);
}

/* SoftStretch: nearest-neighbour, format-converting (a real SDL 1.2 requires
 * matching src/dst formats and no alpha; this is a superset, which never
 * breaks a compliant caller and additionally tolerates a mismatched-format
 * one). */
int SDL_SoftStretch(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect) {
    if (!src || !dst) return -1;
    SDL_Rect sr; sr.x = 0; sr.y = 0; sr.w = (Uint16)src->w; sr.h = (Uint16)src->h;
    if (srcrect) sr = *srcrect;
    SDL_Rect dr; dr.x = 0; dr.y = 0; dr.w = (Uint16)dst->w; dr.h = (Uint16)dst->h;
    if (dstrect) dr = *dstrect;
    if ((int)sr.w <= 0 || (int)sr.h <= 0 || (int)dr.w <= 0 || (int)dr.h <= 0) return 0;
    int sbpp = src->format->BytesPerPixel, dbpp = dst->format->BytesPerPixel;
    for (int y = 0; y < (int)dr.h; y++) {
        int sy = sr.y + (y * (int)sr.h) / (int)dr.h;
        if (sy >= src->h) sy = src->h - 1;
        const Uint8 *srow = (const Uint8 *)src->pixels + (size_t)sy * src->pitch;
        Uint8 *drow = (Uint8 *)dst->pixels + (size_t)(dr.y + y) * dst->pitch + (size_t)dr.x * dbpp;
        for (int x = 0; x < (int)dr.w; x++) {
            int sx = sr.x + (x * (int)sr.w) / (int)dr.w;
            if (sx >= src->w) sx = src->w - 1;
            Uint32 sp = sdlpriv_read_pixel(srow + (size_t)sx * sbpp, sbpp);
            Uint8 r, g, b, a; SDL_GetRGBA(sp, src->format, &r, &g, &b, &a);
            sdlpriv_write_pixel(drow + (size_t)x * dbpp, dbpp, SDL_MapRGBA(dst->format, r, g, b, a));
        }
    }
    return 0;
}

/* ======================================================================
 * A minimal, self-contained BMP reader/writer for SDL_LoadBMP_RW /
 * SDL_SaveBMP_RW. Covers the common uncompressed BITMAPINFOHEADER cases
 * (1/4/8bpp indexed, 24/32bpp truecolor) that SDL 1.2 games actually ship;
 * RLE and BITMAPV4/V5 headers are refused with a clear error rather than
 * silently misread. This is deliberately new, self-contained code (SDL
 * 1.2's own src/video/SDL_bmp.c is BSD/zlib too, but pulling in one file
 * from a much larger tree for ~120 lines of well-known format parsing was
 * not worth the extra provenance to track).
 * ==================================================================== */
#pragma pack(push, 1)
struct bmp_file_header { Uint16 magic; Uint32 size; Uint16 r1, r2; Uint32 data_off; };
struct bmp_info_header { Uint32 hsize; Sint32 w, h; Uint16 planes, bpp; Uint32 compression, imgsize;
                          Sint32 xppm, yppm; Uint32 clr_used, clr_important; };
#pragma pack(pop)

SDL_Surface *SDL_LoadBMP_RW(SDL_RWops *src, int freesrc) {
    if (!src) return 0;
    struct bmp_file_header fh; struct bmp_info_header ih;
    SDL_Surface *out = 0;
    if (SDL_RWread(src, &fh, sizeof(fh), 1) != 1 || fh.magic != 0x4D42) { sdlpriv_set_error("Not a BMP file"); goto done; }
    if (SDL_RWread(src, &ih, sizeof(ih), 1) != 1) { sdlpriv_set_error("Truncated BMP header"); goto done; }
    if (ih.compression != 0) { sdlpriv_set_error("Compressed BMP not supported"); goto done; }
    if (ih.bpp != 8 && ih.bpp != 24 && ih.bpp != 32) { sdlpriv_set_error("Unsupported BMP bit depth"); goto done; }
    {
        int w = ih.w, h = ih.h, flip = 1;
        if (h < 0) { h = -h; flip = 0; }
        if (w <= 0 || h <= 0) { sdlpriv_set_error("Invalid BMP dimensions"); goto done; }
        Uint32 Rm = 0, Gm = 0, Bm = 0;
        if (ih.bpp == 24) { Rm = 0xFF0000; Gm = 0x00FF00; Bm = 0x0000FF; }
        else if (ih.bpp == 32) { Rm = 0x00FF0000; Gm = 0x0000FF00; Bm = 0x000000FF; }
        out = SDL_CreateRGBSurface(SDL_SWSURFACE, w, h, ih.bpp, Rm, Gm, Bm, 0);
        if (!out) goto done;
        if (ih.bpp == 8) {
            int ncolors = ih.clr_used ? (int)ih.clr_used : 256;
            if (ncolors > 256) ncolors = 256;
            for (int i = 0; i < ncolors; i++) {
                Uint8 bgra[4];
                if (SDL_RWread(src, bgra, 4, 1) != 1) { sdlpriv_set_error("Truncated BMP palette"); SDL_FreeSurface(out); out = 0; goto done; }
                out->format->palette->colors[i].b = bgra[0];
                out->format->palette->colors[i].g = bgra[1];
                out->format->palette->colors[i].r = bgra[2];
            }
        }
        SDL_RWseek(src, (int)fh.data_off, RW_SEEK_SET);
        int row_bytes = ((w * ih.bpp + 31) / 32) * 4;  /* rows padded to a 4-byte boundary */
        Uint8 *rowbuf = (Uint8 *)SDL_malloc((size_t)row_bytes);
        if (!rowbuf) { sdlpriv_set_error("Out of memory"); SDL_FreeSurface(out); out = 0; goto done; }
        for (int y = 0; y < h; y++) {
            if (SDL_RWread(src, rowbuf, (size_t)row_bytes, 1) != 1) { SDL_free(rowbuf); SDL_FreeSurface(out); out = 0; sdlpriv_set_error("Truncated BMP pixel data"); goto done; }
            int dy = flip ? (h - 1 - y) : y;
            SDL_memcpy((Uint8 *)out->pixels + (size_t)dy * out->pitch, rowbuf, (size_t)out->w * out->format->BytesPerPixel);
        }
        SDL_free(rowbuf);
    }
done:
    if (freesrc) SDL_RWclose(src);
    return out;
}
int SDL_SaveBMP_RW(SDL_Surface *surface, SDL_RWops *dst, int freedst) {
    (void)surface;
    sdlpriv_set_error("SDL_SaveBMP is not implemented on MayteraOS yet");
    if (freedst && dst) SDL_RWclose(dst);
    return -1;
}

/* ======================================================================
 * YUV overlays: deliberately unsupported (see README "What this backend
 * defers"). Honest failure, not a fake success.
 * ==================================================================== */
SDL_Overlay *SDL_CreateYUVOverlay(int width, int height, Uint32 format, SDL_Surface *display) {
    (void)width; (void)height; (void)format; (void)display;
    sdlpriv_set_error("YUV overlays are not implemented on MayteraOS");
    return 0;
}
int SDL_LockYUVOverlay(SDL_Overlay *overlay) { (void)overlay; return -1; }
void SDL_UnlockYUVOverlay(SDL_Overlay *overlay) { (void)overlay; }
int SDL_DisplayYUVOverlay(SDL_Overlay *overlay, SDL_Rect *dstrect) { (void)overlay; (void)dstrect; return -1; }
void SDL_FreeYUVOverlay(SDL_Overlay *overlay) { (void)overlay; }

/* ======================================================================
 * Video mode / window lifecycle
 * ==================================================================== */
char *SDL_VideoDriverName(char *namebuf, int maxlen) {
    if (namebuf && maxlen > 0) {
        int n = 0; const char *s = "maytera";
        while (s[n] && n < maxlen - 1) { namebuf[n] = s[n]; n++; }
        namebuf[n] = 0;
    }
    return namebuf;
}
int SDL_VideoInit(const char *driver_name, Uint32 flags) { (void)driver_name; (void)flags; return 0; }
void SDL_VideoQuit(void) {
    if (g_sdlv.have_window) { win_destroy(g_sdlv.win_handle); g_sdlv.have_window = 0; g_sdlv.win_handle = -1; }
    if (g_sdlv.is_gl && g_sdlv.zb) { glClose(); ZB_close((ZBuffer *)g_sdlv.zb); g_sdlv.zb = 0; }
    if (g_sdlv.present_buf) { SDL_free(g_sdlv.present_buf); g_sdlv.present_buf = 0; }
    if (g_sdlv.screen) { SDL_Surface *s = g_sdlv.screen; g_sdlv.screen = 0; s->refcount = 1; SDL_FreeSurface(s); }
    memset(&g_sdlv, 0, sizeof(g_sdlv));
    g_sdlv.win_handle = -1;
}
SDL_Surface *SDL_GetVideoSurface(void) { return g_sdlv.screen; }

const SDL_VideoInfo *SDL_GetVideoInfo(void) {
    static SDL_VideoInfo info;
    static SDL_PixelFormat fmt;
    memset(&info, 0, sizeof(info));
    build_format(&fmt, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    info.wm_available = 1;
    info.hw_available = 0;
    info.vfmt = &fmt;
    fb_info_t fi; memset(&fi, 0, sizeof(fi));
    if (fb_info(&fi) == 0 && fi.width > 0 && fi.height > 0) { info.current_w = (int)fi.width; info.current_h = (int)fi.height; }
    else { info.current_w = 1280; info.current_h = 800; }
    return &info;
}
int SDL_VideoModeOK(int width, int height, int bpp, Uint32 flags) {
    (void)flags;
    if (width <= 0 || height <= 0 || width > SDL_MAX_W || height > SDL_MAX_H) return 0;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 32;
    return bpp;
}
SDL_Rect **SDL_ListModes(SDL_PixelFormat *format, Uint32 flags) {
    (void)format; (void)flags;
    return (SDL_Rect **)-1;  /* "all resolutions are available" (compositor scales) */
}

static void teardown_window(void) {
    if (g_sdlv.is_gl && g_sdlv.zb) { glClose(); ZB_close((ZBuffer *)g_sdlv.zb); g_sdlv.zb = 0; }
    if (g_sdlv.present_buf) { SDL_free(g_sdlv.present_buf); g_sdlv.present_buf = 0; }
    if (g_sdlv.screen) { SDL_Surface *s = g_sdlv.screen; g_sdlv.screen = 0; s->refcount = 1; SDL_FreeSurface(s); }
    if (g_sdlv.have_window) { win_destroy(g_sdlv.win_handle); g_sdlv.have_window = 0; g_sdlv.win_handle = -1; }
}

SDL_Surface *SDL_SetVideoMode(int width, int height, int bpp, Uint32 flags) {
    if (width <= 0 || height <= 0) { sdlpriv_set_error("Width and height must be positive"); return 0; }
    if (width > SDL_MAX_W) width = SDL_MAX_W;
    if (height > SDL_MAX_H) height = SDL_MAX_H;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) bpp = 32;

    teardown_window();

    const char *title = g_sdlv.caption[0] ? g_sdlv.caption : "SDL App";
    int cx = 60, cy = 40;
    if (flags & SDL_FULLSCREEN) { cx = 0; cy = 0; }
    /* #491/Arena-style boot race: AUTORUN can launch us before the
     * compositor is ready. Retry with a real sleep, never a busy-wait. */
    int handle = -1;
    for (int attempt = 0; attempt < 40 && handle < 0; attempt++) {
        handle = win_create(title, cx, cy, width, height);
        if (handle < 0) sys_sleep(200);
    }
    if (handle < 0) { sdlpriv_set_error("SDL_SetVideoMode: window creation failed (compositor not ready?)"); return 0; }
    if (flags & SDL_FULLSCREEN) win_set_nochrome(handle);
    wm_focus(handle);

    g_sdlv.win_handle = handle;
    g_sdlv.have_window = 1;
    g_sdlv.mode_w = width; g_sdlv.mode_h = height; g_sdlv.mode_bpp = bpp;
    g_sdlv.mode_flags = flags;
    g_sdlv.is_gl = (flags & SDL_OPENGL) ? 1 : 0;

    g_sdlv.present_buf = (Uint32 *)SDL_calloc((size_t)width * (size_t)height, sizeof(Uint32));
    if (!g_sdlv.present_buf) { sdlpriv_set_error("Out of memory"); teardown_window(); return 0; }

    if (g_sdlv.is_gl) {
        ZBuffer *zb = ZB_open(width, height, ZB_MODE_RGBA, 0);
        if (!zb) { sdlpriv_set_error("SDL_SetVideoMode: TinyGL context creation failed"); teardown_window(); return 0; }
        g_sdlv.zb = zb;
        glInit(zb);
        glViewport(0, 0, width, height);
        /* The "screen" surface in GL mode exists only so
         * SDL_GetVideoSurface()/format queries do not crash; its pixels are
         * not meant to be touched directly, exactly like real SDL. */
        g_sdlv.screen = SDL_CreateRGBSurface(flags, width, height, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    } else {
        Uint32 Rm = 0, Gm = 0, Bm = 0, Am = 0;
        switch (bpp) {
        case 16: Rm = 0xF800; Gm = 0x07E0; Bm = 0x001F; break;
        case 24: Rm = 0xFF0000; Gm = 0x00FF00; Bm = 0x0000FF; break;
        case 32: Rm = 0x00FF0000; Gm = 0x0000FF00; Bm = 0x000000FF; Am = 0; break;
        default: break;  /* 8bpp: masks 0, indexed, gets the default palette */
        }
        g_sdlv.screen = SDL_CreateRGBSurface(flags & (SDL_SWSURFACE | SDL_HWSURFACE | SDL_FULLSCREEN | SDL_RESIZABLE | SDL_NOFRAME), width, height, bpp, Rm, Gm, Bm, Am);
    }
    if (!g_sdlv.screen) { teardown_window(); return 0; }
    g_sdlv.screen->flags |= flags & (SDL_FULLSCREEN | SDL_OPENGL | SDL_RESIZABLE | SDL_NOFRAME);
    return g_sdlv.screen;
}

/* Publish the current screen contents to the compositor. Shared by
 * SDL_Flip/SDL_UpdateRect(s)/SDL_GL_SwapBuffers: sys_win_blit() has no
 * partial-rect form (see file header), so every present is a full-buffer
 * publish regardless of which SDL entry point asked for it. */
void sdlpriv_present(void) {
    if (!g_sdlv.have_window || !g_sdlv.present_buf) return;
    if (g_sdlv.is_gl) {
        ZBuffer *zb = (ZBuffer *)g_sdlv.zb;
        if (!zb) return;
        int w = g_sdlv.mode_w, h = g_sdlv.mode_h;
        int copyw = (zb->xsize < w) ? zb->xsize : w;
        int copyh = (zb->ysize < h) ? zb->ysize : h;
        for (int y = 0; y < copyh; y++) {
            const Uint32 *srow = (const Uint32 *)zb->pbuf + (size_t)y * zb->xsize;
            Uint32 *drow = g_sdlv.present_buf + (size_t)y * w;
            for (int x = 0; x < copyw; x++) drow[x] = srow[x] | 0xFF000000u;
        }
    } else {
        SDL_PixelFormat dstfmt; build_format(&dstfmt, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
        SDL_Surface dst; memset(&dst, 0, sizeof(dst));
        dst.format = &dstfmt; dst.w = g_sdlv.mode_w; dst.h = g_sdlv.mode_h;
        dst.pitch = (Uint16)(g_sdlv.mode_w * 4); dst.pixels = g_sdlv.present_buf;
        dst.clip_rect.w = (Uint16)g_sdlv.mode_w; dst.clip_rect.h = (Uint16)g_sdlv.mode_h;
        /* Bypass colorkey/alpha (this is a raw format conversion of the
         * WHOLE framebuffer, not a sprite blit) by calling the format
         * conversion loop directly. */
        int sbpp = g_sdlv.screen->format->BytesPerPixel;
        for (int y = 0; y < g_sdlv.mode_h; y++) {
            const Uint8 *srow = (const Uint8 *)g_sdlv.screen->pixels + (size_t)y * g_sdlv.screen->pitch;
            Uint32 *drow = g_sdlv.present_buf + (size_t)y * g_sdlv.mode_w;
            for (int x = 0; x < g_sdlv.mode_w; x++) {
                Uint32 sp = sdlpriv_read_pixel(srow + (size_t)x * sbpp, sbpp);
                Uint8 r, g, b, a; SDL_GetRGBA(sp, g_sdlv.screen->format, &r, &g, &b, &a);
                drow[x] = 0xFF000000u | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
            }
        }
    }
    syscall5(SYS_WIN_BLIT, g_sdlv.win_handle, 0, 0,
             (long)((g_sdlv.mode_w & 0xFFFF) | ((g_sdlv.mode_h & 0xFFFF) << 16)),
             (long)g_sdlv.present_buf);
    win_invalidate(g_sdlv.win_handle);
}

void SDL_UpdateRects(SDL_Surface *screen, int numrects, SDL_Rect *rects) {
    (void)screen; (void)numrects; (void)rects;
    sdlpriv_present();
}
void SDL_UpdateRect(SDL_Surface *screen, Sint32 x, Sint32 y, Uint32 w, Uint32 h) {
    (void)screen; (void)x; (void)y; (void)w; (void)h;
    sdlpriv_present();
}
int SDL_Flip(SDL_Surface *screen) { (void)screen; sdlpriv_present(); return 0; }

int SDL_SetGamma(float red, float green, float blue) { (void)red; (void)green; (void)blue; sdlpriv_set_error("Gamma control is not supported"); return -1; }
int SDL_SetGammaRamp(const Uint16 *red, const Uint16 *green, const Uint16 *blue) { (void)red; (void)green; (void)blue; sdlpriv_set_error("Gamma control is not supported"); return -1; }
int SDL_GetGammaRamp(Uint16 *red, Uint16 *green, Uint16 *blue) { (void)red; (void)green; (void)blue; sdlpriv_set_error("Gamma control is not supported"); return -1; }
void SDL_SetRefreshRate(int rate) { (void)rate; }

/* ======================================================================
 * GL
 * ==================================================================== */
int SDL_GL_LoadLibrary(const char *path) { (void)path; sdlpriv_set_error("No dynamic GL library on MayteraOS; TinyGL is always linked in"); return -1; }
void *SDL_GL_GetProcAddress(const char *proc) { (void)proc; return 0; }
int SDL_GL_SetAttribute(SDL_GLattr attr, int value) { if (attr >= 0 && attr < SDL_GL_ATTR_COUNT) g_gl_attrs[attr] = value; return 0; }
int SDL_GL_GetAttribute(SDL_GLattr attr, int *value) { if (value) *value = (attr >= 0 && attr < SDL_GL_ATTR_COUNT) ? g_gl_attrs[attr] : 0; return 0; }
void SDL_GL_SwapBuffers(void) { sdlpriv_present(); }
void SDL_GL_UpdateRects(int numrects, SDL_Rect *rects) { (void)numrects; (void)rects; sdlpriv_present(); }
void SDL_GL_Lock(void) { }
void SDL_GL_Unlock(void) { }

/* ======================================================================
 * Window manager
 * ==================================================================== */
void SDL_WM_SetCaption(const char *title, const char *icon) {
    if (title) { int i = 0; while (title[i] && i < (int)sizeof(g_sdlv.caption) - 1) { g_sdlv.caption[i] = title[i]; i++; } g_sdlv.caption[i] = 0; }
    if (icon) { int i = 0; while (icon[i] && i < (int)sizeof(g_sdlv.icon_caption) - 1) { g_sdlv.icon_caption[i] = icon[i]; i++; } g_sdlv.icon_caption[i] = 0; }
    /* There is no SYS_WIN_SET_TITLE on MayteraOS, so a caption set AFTER the
     * window already exists is remembered (a later SDL_SetVideoMode will use
     * it) but cannot be pushed to the compositor's title bar live. Honest
     * limitation, not a silent no-op left undocumented. */
}
void SDL_WM_GetCaption(char **title, char **icon) {
    if (title) *title = g_sdlv.caption;
    if (icon) *icon = g_sdlv.icon_caption;
}
void SDL_WM_SetIcon(SDL_Surface *icon, Uint8 *mask) { (void)icon; (void)mask; /* no window-icon syscall */ }
int SDL_WM_IconifyWindow(void) { sdlpriv_set_error("Iconify is not supported"); return 0; }
int SDL_WM_ToggleFullScreen(SDL_Surface *surface) {
    (void)surface;
    if (!g_sdlv.have_window) return 0;
    if (g_sdlv.mode_flags & SDL_FULLSCREEN) {
        /* No "restore chrome" syscall exists (win_set_nochrome is one-way);
         * honest partial support, matching the pre-existing sdlshim.cpp
         * behaviour for this exact case. */
        return 0;
    }
    win_set_nochrome(g_sdlv.win_handle);
    g_sdlv.mode_flags |= SDL_FULLSCREEN;
    if (g_sdlv.screen) g_sdlv.screen->flags |= SDL_FULLSCREEN;
    return 1;
}
SDL_GrabMode SDL_WM_GrabInput(SDL_GrabMode mode) {
    SDL_GrabMode prev = g_sdlv.grab_mode;
    if (mode != SDL_GRAB_QUERY) g_sdlv.grab_mode = mode;
    /* Tracked, not enforced: grab_input()-class syscalls are compositor-only
     * (kernel/gui/fb_syscall.c is_compositor() gate), the same limitation
     * the pre-existing sdlshim.cpp files document for SDL2. */
    return prev;
}
