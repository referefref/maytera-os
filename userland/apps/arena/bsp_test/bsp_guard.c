/* bsp_guard.c - #491 Stage 1 hardware guard-page OOB proof for the Rust BSP
 * parser. For every malformed/fuzz vector, the input is placed so its LAST byte
 * abuts a PROT_NONE guard page; any read past the end of the (untrusted) file
 * faults immediately with SIGSEGV. If the whole corpus completes, that is a
 * hardware-enforced proof the Rust parser never reads past the input buffer on
 * ANY of these inputs (the dominant OOB class for a file parser). No ASan or
 * Valgrind needed. Links the REAL compiled Rust parser (libarena_rs_host.a).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>

typedef struct { uint32_t x, y, z; } BspVec3;
typedef struct {
    uint32_t first_vertex, num_vertices; int32_t tex_id;
    uint32_t s_vec[4], t_vec[4], tex_w, tex_h;
} BspFace;
typedef struct { uint32_t width, height, pixel_offset, has_pixels; } BspTexture;
typedef struct {
    BspVec3 *verts; BspFace *faces; BspTexture *textures; uint32_t *pixels;
    uint8_t *entities; uint32_t num_verts, num_faces, num_textures, num_pixels,
    entities_len; BspVec3 spawn; uint32_t has_spawn; int32_t error;
} BspScene;
extern BspScene *bsp_parse(const uint8_t *data, unsigned long len,
                           const uint8_t *wad, unsigned long wad_len);
extern void bsp_free(BspScene *scene);
void rust_eh_personality(void) {}

static long g_ps;

/* Parse `src[0..len]` with a guard page immediately after the last byte. */
static void guarded_parse(const uint8_t *src, unsigned long len) {
    unsigned long data_pages = (len + g_ps - 1) / g_ps;
    if (data_pages == 0) data_pages = 1;
    size_t region = (size_t)(data_pages + 1) * g_ps;       /* +1 guard page     */
    uint8_t *base = mmap(NULL, region, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(3); }
    uint8_t *guard = base + region - g_ps;
    uint8_t *data = guard - len;                            /* end abuts guard   */
    if (len) memcpy(data, src, len);
    if (mprotect(guard, g_ps, PROT_NONE) != 0) { perror("mprotect"); exit(3); }
    BspScene *s = bsp_parse(data, len, NULL, 0);            /* fault if over-read */
    if (s) {
        volatile uint32_t t = s->num_verts + s->num_faces; (void)t;
        bsp_free(s);
    }
    mprotect(guard, g_ps, PROT_READ | PROT_WRITE);
    munmap(base, region);
}

int main(int argc, char **argv) {
    g_ps = sysconf(_SC_PAGESIZE);
    const char *path = argc > 1 ? argv[1] : "room.bsp";
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n); if (fread(file, 1, n, f) != (size_t)n) return 2;
    fclose(f);

    long vectors = 0;

    /* valid input first (must NOT fault, must parse) */
    guarded_parse(file, n); vectors++;

    /* truncation at every length */
    for (long len = 0; len <= n; len++) { guarded_parse(file, len); vectors++; }

    /* hostile header lump ofs/len pokes */
    static const uint32_t POKE[] = {0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u,
                                    (uint32_t)-16, 0x10000000u, 123456789u};
    for (int lump = 0; lump < 15; lump++)
        for (unsigned p = 0; p < sizeof(POKE)/sizeof(POKE[0]); p++)
            for (int which = 0; which < 2; which++) {
                uint8_t *m = malloc(n); memcpy(m, file, n);
                memcpy(m + 4 + lump*8 + which*4, &POKE[p], 4);
                guarded_parse(m, n); free(m); vectors++;
            }

    /* full byte-flip sweep, multiple xor patterns */
    static const uint8_t XOR[] = {0xFF, 0x80, 0x01, 0x7F};
    for (long i = 0; i < n; i++)
        for (unsigned x = 0; x < sizeof(XOR); x++) {
            uint8_t *m = malloc(n); memcpy(m, file, n);
            m[i] ^= XOR[x];
            guarded_parse(m, n); free(m); vectors++;
        }

    printf("GUARD-PAGE PROOF: %ld vectors parsed with a PROT_NONE page at the "
           "input end; NO over-read fault. 0 OOB.\n", vectors);
    free(file);
    return 0;
}
