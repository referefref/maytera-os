// imgfile.h - seekable, bounded-RAM reader over a disk-image FILE (#196)
//
// WHY THIS EXISTS
// ---------------
// The first #196 disk-image layer (dos/diskimg.c) read the WHOLE image into RAM
// with fat_read_file() and capped it at 16 MiB. That is fine for a 1.44 MB
// floppy and impossible for a CD: the Command & Conquer Red Alert discs this
// was built for are 653,725,696 and 677,222,400 bytes, and a SINGLE file inside
// the Allied disc (MAIN.MIX) is 454,605,294 bytes, which is larger than the
// entire kernel heap (256 MB at HEAP_VIRT_BASE). Whole-image RAM loading was
// not a tunable cap, it was the wrong shape.
//
// imgfile_t is the replacement: an open handle on the image file that serves
// arbitrary byte ranges through a small fixed cache, so the RAM cost is
// O(cache), not O(image). It is the "present an image file as a readable block
// device" layer, and nothing above it ever sees the whole image at once.
//
// BACKING STORES
// --------------
// Images live on the normal root filesystem, which on a two-partition golden is
// ext2 (#365). Both roots are supported:
//   ext2 -> ext2_resolve_path() + ext2_read_file_range()  (#572 streaming read)
//   FAT  -> fat_open() + fat_seek() + fat_read()          (streaming handle)
// The ext2 path is tried first when ext2 is mounted, mirroring the routing that
// fat_read_file() already does, so an image under /WINDIR resolves the same way
// every other file on the root does.
//
// BLOCKING
// --------
// Reads go through the ext2 / FAT drivers, which take sleeping locks. An
// imgfile_t must therefore only be read from a context that may block (a
// process or kernel thread, interrupts on). Never from an ISR. There is no
// polling anywhere in here: a cache miss performs one direct filesystem read
// and returns.
#ifndef DOS_IMGFILE_H
#define DOS_IMGFILE_H

#include "../types.h"

// Cache geometry. 32 slots x 8 KiB = 256 KiB per open image. 8 KiB is four
// ISO 9660 logical sectors, so a directory extent or a sequential file read
// costs one backing read per four sectors. Both numbers are deliberately small:
// two mounted images (A: and E:) cost 512 KiB total, which is affordable
// against a 256 MB heap in a way that a 653 MB image is not.
#define IMGF_CACHE_SLOTS 32
#define IMGF_CACHE_BLK   8192u

#define IMGF_KIND_NONE 0
#define IMGF_KIND_EXT2 1
#define IMGF_KIND_FAT  2

// Opaque to callers except for embedding by value. fat_file_t is included by
// the .c; the handle is stored as a heap pointer so this header does not have
// to pull fs/fat.h into every user.
typedef struct imgfile {
    int      kind;              // IMGF_KIND_*
    uint32_t ino;               // ext2 inode (IMGF_KIND_EXT2)
    void    *fat;               // fat_file_t * (IMGF_KIND_FAT), kmalloc'd
    uint64_t size;              // image size in bytes
    uint8_t *cache;             // IMGF_CACHE_SLOTS * IMGF_CACHE_BLK bytes
    uint64_t tag[IMGF_CACHE_SLOTS];   // cached block index, IMGF_TAG_EMPTY = free
    uint32_t age[IMGF_CACHE_SLOTS];   // LRU stamp
    uint32_t clock;             // monotonic stamp source
    uint64_t hits;              // cache hits, in blocks
    uint64_t misses;            // backing reads, in blocks
} imgfile_t;

#define IMGF_TAG_EMPTY 0xFFFFFFFFFFFFFFFFULL

// Open `path` (a native uppercase path such as "/WINDIR/RA1.ISO") for reading.
// Returns 0 on success, negative on error. On success the handle owns a cache
// allocation and MUST be closed with imgfile_close().
int  imgfile_open(imgfile_t *f, const char *path);

// Release everything the handle owns and zero it. Safe on an already-closed or
// zeroed handle.
void imgfile_close(imgfile_t *f);

// Read `len` bytes at byte offset `off` into `dst`. Short reads happen only at
// EOF. Returns the number of bytes read, or negative on error. `off`/`len` are
// clamped to the image size, so an out-of-range request reads 0, never past the
// end of the file and never past `dst`.
int64_t imgfile_read(imgfile_t *f, uint64_t off, uint64_t len, void *dst);

// Image size in bytes (0 if not open).
uint64_t imgfile_size(const imgfile_t *f);

// 1 if the handle is open.
int imgfile_is_open(const imgfile_t *f);

#endif // DOS_IMGFILE_H
