// arc.h - MayteraOS archiver core (gzip / tar / tar.gz / zip), pure C.
// No syscalls in the core: everything operates on in-memory buffers so it can
// be unit-tested headless and shared between the kernel and userland.
#ifndef ARC_H
#define ARC_H

// Types (uint8_t/uint32_t/size_t) come from the environment via arc_port.h,
// which selects kernel types, hosted <stdint.h>, or userland libc types.
#include "arc_port.h"

// ---- Checksums ----------------------------------------------------------
// Seed crc32 with 0, adler32 with 1. Both return the running value.
uint32_t arc_crc32(uint32_t crc, const uint8_t *data, size_t len);
uint32_t arc_adler32(uint32_t adler, const uint8_t *data, size_t len);

// ---- Raw DEFLATE --------------------------------------------------------
// Compress: returns a freshly allocated buffer (set *out_len), or NULL.
// Uses fixed-Huffman blocks with LZ77 hash-chain matching; output is
// decompressible by any standard inflate (gzip, unzip, zlib).
uint8_t *arc_deflate(const uint8_t *src, size_t src_len, size_t *out_len);
// Inflate into a caller buffer of known capacity. Returns 0 on success and
// sets *out_len, or -1 on error.
int arc_inflate(const uint8_t *src, size_t src_len,
                uint8_t *dst, size_t dst_cap, size_t *out_len);

// ---- gzip (.gz, RFC 1952) ----------------------------------------------
uint8_t *arc_gzip_compress(const uint8_t *data, size_t len,
                           const char *name, size_t *out_len);
// Returns a freshly allocated decompressed buffer (set *out_len), or NULL.
// Verifies the CRC32 and ISIZE trailer.
uint8_t *arc_gzip_decompress(const uint8_t *gz, size_t gz_len, size_t *out_len);

// ---- Multi-file entries (tar / zip) ------------------------------------
typedef struct {
    char     name[256]; // path inside the archive; dirs SHOULD end with '/'
    uint8_t *data;      // file contents (NULL for a directory)
    size_t   size;      // byte count
    int      is_dir;    // non-zero for a directory entry
    uint32_t mode;      // unix mode bits (0 -> a sensible default is used)
} arc_entry;

void arc_free_entries(arc_entry *ents, int count);

// ---- tar (ustar) --------------------------------------------------------
uint8_t  *arc_tar_create(const arc_entry *ents, int n, size_t *out_len);
arc_entry *arc_tar_extract(const uint8_t *tar, size_t len, int *out_count);

// ---- tar.gz / .tgz ------------------------------------------------------
uint8_t  *arc_targz_create(const arc_entry *ents, int n, size_t *out_len);
arc_entry *arc_targz_extract(const uint8_t *gz, size_t len, int *out_count);

// ---- zip ----------------------------------------------------------------
// use_deflate: 0 = store (method 0), 1 = deflate (method 8).
uint8_t  *arc_zip_create(const arc_entry *ents, int n, int use_deflate, size_t *out_len);
arc_entry *arc_zip_extract(const uint8_t *zip, size_t len, int *out_count);


// ---- Streaming tar.gz extraction (#613) ---------------------------------
// arc_targz_extract() above decompresses the WHOLE archive into one heap
// buffer and then copies every member into its own allocation, so peak RAM is
// O(archive size). This API instead pulls the compressed bytes in bounded
// slices and pushes each member out as (header, data chunks, end) events, so
// the caller writes straight to a destination file and the working set is
// FIXED (32KB LZ77 window + 8KB input slice + one 512B tar header),
// independent of how big the archive is. arc_stream_workmem() reports the
// exact byte count.
//
// SECURITY: member names and types are validated HERE and fail closed
// (arc_path_is_safe): no absolute paths, no ".." components, no control
// bytes, bounded length, and no link/device/fifo member types. The gzip
// CRC32/ISIZE trailer is necessarily checked only after the last byte is
// emitted, so it is an integrity check, not a trust boundary: verify the
// whole archive (e.g. against a signed sha256) BEFORE calling this.
#define ARC_MEMBER_NAME_MAX 256

#define ARC_OK          0
#define ARC_E_INPUT    (-1)   // reader failed or the stream ended early
#define ARC_E_FORMAT   (-2)   // not a valid gzip/tar stream
#define ARC_E_CORRUPT  (-3)   // gzip CRC32/ISIZE trailer mismatch
#define ARC_E_UNSAFE   (-4)   // member name or type rejected
#define ARC_E_SINK     (-5)   // a sink callback failed (e.g. write error)
#define ARC_E_NOMEM    (-6)

typedef struct {
    char     name[ARC_MEMBER_NAME_MAX]; // validated, relative, no ".."
    uint64_t size;                      // declared payload bytes (0 for a dir)
    uint32_t mode;                      // unix mode bits from the header
    int      is_dir;
} arc_member;

// Pull up to `cap` compressed bytes. Return >0 bytes read, 0 for EOF, <0 error.
typedef int (*arc_read_fn)(void *ctx, uint8_t *buf, size_t cap);

// Returned by on_member/on_end to end the stream early, successfully (e.g. a
// caller that only wanted one member and has now got it). Not an error.
#define ARC_STOP        2

typedef struct {
    // Return 0 to extract, 1 to skip its payload, ARC_STOP to end the stream
    // successfully here, <0 to abort.
    int  (*on_member)(void *ctx, const arc_member *m);
    // A bounded chunk of the current member's payload. Return <0 to abort.
    int  (*on_data)(void *ctx, const uint8_t *data, size_t len);
    // The member's declared size has fully arrived. Return ARC_STOP to end
    // the stream successfully here, <0 to abort.
    int  (*on_end)(void *ctx, const arc_member *m);
    // The stream failed with a member still open: drop any partial output.
    void (*on_abort)(void *ctx);
} arc_sink;

// Returns ARC_OK or one of the ARC_E_* codes above. verify_crc=1 checks the
// gzip trailer after extraction (skip it only when the archive's integrity is
// already established some other way and the second pass is not worth it).
int arc_targz_extract_stream(arc_read_fn rd, void *rdctx,
                             const arc_sink *sink, void *sinkctx, int verify_crc);

// 1 when the name is safe to use as a relative destination path.
int arc_path_is_safe(const char *name);

// Fixed working-set size (bytes) arc_targz_extract_stream allocates.
size_t arc_stream_workmem(void);

#endif // ARC_H
