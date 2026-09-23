/* sdl_rwops.c - SDL_RWops (file/memory I/O abstraction) and the endian
 * read/write helpers. Part of the MayteraOS SDL 1.2 backend (task #745,
 * docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 2 #7).
 *
 * File-backed RWops go through the real userland/libc stdio.h
 * (fopen/fread/fwrite/fseek/ftell/fclose): libSDL is a plain C library (not
 * the C++ translation unit the old sdlshim.cpp files were forced to be to
 * avoid pulling in MayteraOS libc headers alongside a vendored C++ engine),
 * so there is no reason to route around libc here.
 */
#include "sdl_priv.h"
#include <stdio.h>
#include <string.h>

static int rw_file_seek(SDL_RWops *ctx, int offset, int whence) {
    FILE *f = (FILE *)ctx->hidden.stdio.fp;
    if (fseek(f, offset, whence) != 0) return -1;
    return (int)ftell(f);
}
static int rw_file_read(SDL_RWops *ctx, void *ptr, int size, int maxnum) {
    return (int)fread(ptr, (size_t)size, (size_t)maxnum, (FILE *)ctx->hidden.stdio.fp);
}
static int rw_file_write(SDL_RWops *ctx, const void *ptr, int size, int num) {
    return (int)fwrite(ptr, (size_t)size, (size_t)num, (FILE *)ctx->hidden.stdio.fp);
}
static int rw_file_close(SDL_RWops *ctx) {
    int rc = 0;
    if (ctx->hidden.stdio.autoclose && ctx->hidden.stdio.fp) rc = fclose((FILE *)ctx->hidden.stdio.fp);
    SDL_FreeRW(ctx);
    return rc;
}

SDL_RWops *SDL_AllocRW(void) { return (SDL_RWops *)SDL_calloc(1, sizeof(SDL_RWops)); }
void SDL_FreeRW(SDL_RWops *area) { SDL_free(area); }

SDL_RWops *SDL_RWFromFP(FILE *fp, int autoclose) {
    SDL_RWops *rw = SDL_AllocRW();
    if (!rw) return 0;
    rw->seek = rw_file_seek; rw->read = rw_file_read; rw->write = rw_file_write; rw->close = rw_file_close;
    rw->type = 0;
    rw->hidden.stdio.fp = fp;
    rw->hidden.stdio.autoclose = autoclose;
    return rw;
}
SDL_RWops *SDL_RWFromFile(const char *file, const char *mode) {
    if (!file || !mode) { sdlpriv_set_error("SDL_RWFromFile: NULL path or mode"); return 0; }
    FILE *fp = fopen(file, mode);
    if (!fp) { sdlpriv_set_error("Couldn't open %s", file); return 0; }
    return SDL_RWFromFP(fp, 1);
}

static int rw_mem_seek(SDL_RWops *ctx, int offset, int whence) {
    Uint8 *newpos;
    switch (whence) {
    case RW_SEEK_SET: newpos = ctx->hidden.mem.base + offset; break;
    case RW_SEEK_CUR: newpos = ctx->hidden.mem.here + offset; break;
    case RW_SEEK_END: newpos = ctx->hidden.mem.stop + offset; break;
    default: sdlpriv_set_error("Unknown seek whence"); return -1;
    }
    if (newpos < ctx->hidden.mem.base) newpos = ctx->hidden.mem.base;
    if (newpos > ctx->hidden.mem.stop) newpos = ctx->hidden.mem.stop;
    ctx->hidden.mem.here = newpos;
    return (int)(ctx->hidden.mem.here - ctx->hidden.mem.base);
}
static int rw_mem_read(SDL_RWops *ctx, void *ptr, int size, int maxnum) {
    size_t avail = (size_t)(ctx->hidden.mem.stop - ctx->hidden.mem.here);
    size_t want = (size_t)size * (size_t)maxnum;
    size_t n = (want > avail) ? (size > 0 ? avail / (size_t)size : 0) : (size_t)maxnum;
    if (n > 0) { SDL_memcpy(ptr, ctx->hidden.mem.here, n * (size_t)size); ctx->hidden.mem.here += n * (size_t)size; }
    return (int)n;
}
static int rw_mem_write(SDL_RWops *ctx, const void *ptr, int size, int num) {
    size_t avail = (size_t)(ctx->hidden.mem.stop - ctx->hidden.mem.here);
    size_t want = (size_t)size * (size_t)num;
    size_t n = (want > avail) ? (size > 0 ? avail / (size_t)size : 0) : (size_t)num;
    if (n > 0) { SDL_memcpy(ctx->hidden.mem.here, ptr, n * (size_t)size); ctx->hidden.mem.here += n * (size_t)size; }
    return (int)n;
}
static int rw_mem_close(SDL_RWops *ctx) { SDL_FreeRW(ctx); return 0; }
static int rw_mem_write_const(SDL_RWops *ctx, const void *ptr, int size, int num) {
    (void)ctx; (void)ptr; (void)size; (void)num;
    sdlpriv_set_error("Can't write to read-only memory"); return 0;
}

SDL_RWops *SDL_RWFromMem(void *mem, int size) {
    SDL_RWops *rw = SDL_AllocRW();
    if (!rw) return 0;
    rw->seek = rw_mem_seek; rw->read = rw_mem_read; rw->write = rw_mem_write; rw->close = rw_mem_close;
    rw->type = 1;
    rw->hidden.mem.base = (Uint8 *)mem;
    rw->hidden.mem.here = (Uint8 *)mem;
    rw->hidden.mem.stop = (Uint8 *)mem + size;
    return rw;
}
SDL_RWops *SDL_RWFromConstMem(const void *mem, int size) {
    SDL_RWops *rw = SDL_RWFromMem((void *)mem, size);
    if (rw) rw->write = rw_mem_write_const;
    return rw;
}

/* ---------------------------------------------------------------------
 * Endian-aware reads/writes.
 * ------------------------------------------------------------------- */
Uint16 SDL_ReadLE16(SDL_RWops *src) { Uint8 b[2]; if (SDL_RWread(src, b, 2, 1) != 1) return 0; return (Uint16)(b[0] | (b[1] << 8)); }
Uint16 SDL_ReadBE16(SDL_RWops *src) { Uint8 b[2]; if (SDL_RWread(src, b, 2, 1) != 1) return 0; return (Uint16)((b[0] << 8) | b[1]); }
Uint32 SDL_ReadLE32(SDL_RWops *src) { Uint8 b[4]; if (SDL_RWread(src, b, 4, 1) != 1) return 0; return (Uint32)b[0] | ((Uint32)b[1] << 8) | ((Uint32)b[2] << 16) | ((Uint32)b[3] << 24); }
Uint32 SDL_ReadBE32(SDL_RWops *src) { Uint8 b[4]; if (SDL_RWread(src, b, 4, 1) != 1) return 0; return ((Uint32)b[0] << 24) | ((Uint32)b[1] << 16) | ((Uint32)b[2] << 8) | (Uint32)b[3]; }
Uint64 SDL_ReadLE64(SDL_RWops *src) { Uint32 lo = SDL_ReadLE32(src); Uint32 hi = SDL_ReadLE32(src); return (Uint64)lo | ((Uint64)hi << 32); }
Uint64 SDL_ReadBE64(SDL_RWops *src) { Uint32 hi = SDL_ReadBE32(src); Uint32 lo = SDL_ReadBE32(src); return (Uint64)lo | ((Uint64)hi << 32); }

int SDL_WriteLE16(SDL_RWops *dst, Uint16 value) { Uint8 b[2] = { (Uint8)value, (Uint8)(value >> 8) }; return SDL_RWwrite(dst, b, 2, 1); }
int SDL_WriteBE16(SDL_RWops *dst, Uint16 value) { Uint8 b[2] = { (Uint8)(value >> 8), (Uint8)value }; return SDL_RWwrite(dst, b, 2, 1); }
int SDL_WriteLE32(SDL_RWops *dst, Uint32 value) { Uint8 b[4] = { (Uint8)value, (Uint8)(value >> 8), (Uint8)(value >> 16), (Uint8)(value >> 24) }; return SDL_RWwrite(dst, b, 4, 1); }
int SDL_WriteBE32(SDL_RWops *dst, Uint32 value) { Uint8 b[4] = { (Uint8)(value >> 24), (Uint8)(value >> 16), (Uint8)(value >> 8), (Uint8)value }; return SDL_RWwrite(dst, b, 4, 1); }
int SDL_WriteLE64(SDL_RWops *dst, Uint64 value) { int a = SDL_WriteLE32(dst, (Uint32)value); int b = SDL_WriteLE32(dst, (Uint32)(value >> 32)); return a && b; }
int SDL_WriteBE64(SDL_RWops *dst, Uint64 value) { int a = SDL_WriteBE32(dst, (Uint32)(value >> 32)); int b = SDL_WriteBE32(dst, (Uint32)value); return a && b; }
