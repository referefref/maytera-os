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

#endif // ARC_H
