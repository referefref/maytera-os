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
// #740 adds a THIRD backing that is not a file at all:
//   BLKDEV -> blk_read() at a raw byte offset on the ROOT BLOCK DEVICE
// That is what lets a data volume live in the ~122 GB of unpartitioned tail on
// the boot stick instead of inside the 1.62 GB OS image. It matters that this
// is a backing kind and not a new layer: an image in the stick's tail is now
// the same object as an image in a file, so the ISO parser, the 256 KiB cache,
// the drive-letter placement, the mount generation, the refcount, the read
// turnstile and the fat_open() redirect are all reused unchanged rather than
// reimplemented for "the USB case". See rustkern/usbvol.rs for the on-disk
// contract and why the tail rather than a third GPT partition.
//
// The offset is chosen (rustkern/usbvol.rs, USBVOL_BASE) to sit ABOVE the #375
// TO-RAM window, so fs/blockdev.c serves these reads from the DEVICE and never
// from the RAM copy of the root. Reading a multi-gigabyte volume through the
// RAM copy would defeat the entire point: the volume exists precisely because
// it does not fit in the image, and it would not fit in RAM either.
//
// BLOCKING
// --------
// Reads go through the ext2 / FAT drivers, which take sleeping locks, or (for
// the #740 block-device backing) through blk_read(), which on a USB root waits
// on the MSC transfer. An imgfile_t must therefore only be read from a context
// that may block (a process or kernel thread, interrupts on). Never from an
// ISR. There is no polling anywhere in here: a cache miss performs one direct
// backing read and returns.
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

// [no-ticket] READAHEAD. A miss used to fetch exactly ONE block, so a
// sequential stream off a mounted disc paid one USB round trip per 8 KiB: 128
// commands per megabyte. On a real USB mass-storage device a command costs a
// FIXED ~121-148 us whatever its size (measured; see rustkern/imgra.rs), so the
// round-trip count was the entire cost and the block layer was 99% device.
//
// On a detected sequential stream the miss now fetches up to IMGF_RA_MAX
// CONSECUTIVE blocks into a CONSECUTIVE run of slots, which is ONE contiguous
// buffer and therefore one backing read. 8 blocks = 64 KiB per fetch.
// fs/blockdev.c splits that into 32 KB SCSI commands (BLK_USB_CHUNK), which is
// exactly where the measured device curve saturates, so a larger window would
// buy nothing on the device while widening the kmalloc-contiguity exposure the
// #614 note warns about. 8 of 32 slots is a quarter of the cache, leaving 24
// for the directory walk and any second stream.
#define IMGF_RA_MAX      8u

// rustkern/imgra.rs owns the policy. The struct is shared by value; the width
// lock is in imgfile.c.
//
// Several independent stream positions, not one, because a drive letter is ONE
// imgfile handle: the file being streamed, the ISO directory walk that resolved
// it and any second open file all share this state. With a single last-position
// variable ANY interleaving resets the window on every other miss and readahead
// never engages. See the module header for why that is the normal case.
#define IMGRA_STREAMS 4
typedef struct imgra_stream {
    uint64_t next_seq;
    uint32_t win;
    uint32_t lru;
} imgra_stream_t;
typedef struct imgra {
    imgra_stream_t st[IMGRA_STREAMS];
    uint32_t clock;
    uint32_t _pad;
    uint64_t n_seq;
    uint64_t n_rand;
    uint64_t n_fetch;
    uint64_t n_blocks;
} imgra_t;

#define IMGF_KIND_NONE   0
#define IMGF_KIND_EXT2   1
#define IMGF_KIND_FAT    2
#define IMGF_KIND_BLKDEV 3

// #740: the SYNTHETIC PATH that selects the raw block-device backing.
//
//   /@USBVOL:<ch>:<dr>:<base16>:<len16>
//
// where <ch>/<dr> are the two-hex-digit ATA channel/drive identity blk_read()
// wants (ignored on the USB path) and <base16>/<len16> are 16-hex-digit byte
// offsets into the DEVICE. dos/usbvol.c builds it; nothing else should.
//
// A synthetic path rather than a second open() entry point, deliberately. The
// layer above (dos/diskimg.c) turns a path into a mounted drive and does a
// great deal on the way (format probe, placement policy, mount generation,
// refcount, read turnstile). Adding a parallel entry point would mean either
// duplicating that or threading a new parameter through all of it; a path that
// imgfile_open() recognises means diskimg.c needs NO change at all and a
// device-backed mount is the same object as a file-backed one everywhere else
// in the kernel, including in the Ring-3 diskimg_info_t view, where the path
// field then says exactly where the bytes come from.
//
// It is also shaped to survive drvmap_path_ok_rs(), which diskimg_mount_idx()
// applies BEFORE opening anything: absolute, no backslash, no control bytes,
// no ".." component. That check is not bypassed for this path.
#define IMGF_BLKDEV_PREFIX "/@USBVOL:"

// Opaque to callers except for embedding by value. fat_file_t is included by
// the .c; the handle is stored as a heap pointer so this header does not have
// to pull fs/fat.h into every user.
typedef struct imgfile {
    int      kind;              // IMGF_KIND_*
    uint32_t ino;               // ext2 inode (IMGF_KIND_EXT2)
    void    *fat;               // fat_file_t * (IMGF_KIND_FAT), kmalloc'd
    uint64_t base;              // device byte offset (IMGF_KIND_BLKDEV)
    uint8_t  ch, dr;            // blk_read channel/drive (IMGF_KIND_BLKDEV)
    uint64_t size;              // image size in bytes
    uint8_t *cache;             // IMGF_CACHE_SLOTS * IMGF_CACHE_BLK bytes
    uint64_t tag[IMGF_CACHE_SLOTS];   // cached block index, IMGF_TAG_EMPTY = free
    uint32_t age[IMGF_CACHE_SLOTS];   // LRU stamp
    uint32_t clock;             // monotonic stamp source
    uint64_t hits;              // cache hits, in blocks
    uint64_t misses;            // backing reads, in blocks
    uint8_t *cache_raw;         // the kmalloc() pointer; `cache` is it aligned up
    imgra_t  ra;                // [no-ticket] readahead state (rustkern/imgra.rs)
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

// [no-ticket] Force readahead off (window pinned to 1 block, the pre-change
// behaviour). Set from /CONFIG/CDRAOFF.CFG and by dos/cdbench.c's control arm.
void imgfile_readahead_set_disabled(int off);
int  imgfile_readahead_disabled(void);

// Image size in bytes (0 if not open).
uint64_t imgfile_size(const imgfile_t *f);

// 1 if the handle is open.
int imgfile_is_open(const imgfile_t *f);

#endif // DOS_IMGFILE_H
