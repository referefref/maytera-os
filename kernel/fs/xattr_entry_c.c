// xattr_entry_c.c - #404 batch-2 extraction of the on-disk xattr entry-walk from
// fs/xattr.c (the loop body shared by xattr_get / xattr_list). Kept as the
// rollback reference; the Rust drop-in xattr_entry_next_rs (rustkern.rs) confines
// the reads.
//
// WHAT THIS IS, PRECISELY (corrected 2026-07-16 by the #404 3-way drift audit;
// the previous header claimed this function "reproduces EXACTLY what the live C
// did, INCLUDING the missing bounds", which was NOT TRUE and is now fixed):
//
//   * It DOES retain the ORIGINAL's missing bounds: a single loop step reads the
//     8-byte entry header and derives name/value offsets from the on-disk
//     (attacker-controlled) name_len/value_len with no check that they stay
//     inside the block. That is the MAYTERA-SEC-2026-0011 over-read, retained on
//     purpose so a -DRUST_XATTR rollback restores the pre-b823 BEHAVIOR and not
//     merely the pre-b823 flag.
//   * It is NOT a verbatim transliteration. The ORIGINAL walked a 64-bit
//     `uint8_t *ptr` guarded by `ptr < end`; this seam's ABI exposes a uint32_t
//     byte-offset cursor instead. Where a crafted length pushes the cursor past
//     2^32 the two representations cannot agree exactly, so the cursor CLAMPS to
//     `len` (see the walk-termination note below). Clamping reproduces the
//     ORIGINAL's OBSERVABLE behavior (the walk terminates); an earlier
//     `(uint32_t)` truncation did NOT, and wrapped instead.
//
// Behavior on WELL-FORMED input is byte-identical to the original (audited: 0
// mismatches over 150,000 blocks / 486,075 entries).
//
// MAYTERA-SEC-2026-0011 (CWE-125): under -DRUST_XATTR the live get/list paths use
// the Rust seam and this over-read cannot occur; without the flag they use this
// reference (today's behavior). C-fallback hardening tracked as #508.
#include "xattr.h"

int xattr_entry_next_c(const uint8_t *block, uint32_t len, uint32_t *pos, xattr_entry_t *out) {
    uint32_t p = *pos;

    // Mirrors the live loop guard `ptr < end`. The ONLY bound the C applies: the
    // entry must merely START inside the block. It does NOT check that the 8-byte
    // header, the name, or the value fit.
    if (p >= len) {
        return 0;
    }

    // Live code: reads entry->name_len/value_len/namespace_id. OOB READ #1
    // (CWE-125): if the entry starts within [0,len) but fewer than 8 bytes remain
    // (len - p < 8), this reads the header PAST the end of the kmalloc'd buffer.
    const xattr_entry_header_t *entry = (const xattr_entry_header_t *)(block + p);
    uint16_t name_len  = entry->name_len;
    uint32_t value_len = entry->value_len;
    uint8_t  ns        = entry->namespace_id;

    // name_off/value_off/next may all point PAST `len`: the caller's
    // strcmp(attr_name,...) is OOB READ #2 and memcpy(...,attr_value,value_len) is
    // OOB READ #3. Computed in 64-bit to match the live pointer arithmetic, which
    // could not wrap.
    uint64_t name_off  = (uint64_t)p + (uint64_t)sizeof(xattr_entry_header_t);
    uint64_t value_off = name_off + (uint64_t)name_len;
    uint64_t next      = value_off + (uint64_t)value_len;

    // DRIFT FIX (#404 3-way drift audit, 2026-07-16). CLAMP, do not truncate.
    //
    // The ORIGINAL (fs/xattr.c:385-397 pre-b823) advanced a 64-bit pointer:
    //     uint8_t *ptr = file_data + sizeof(xattr_file_header_t);
    //     uint8_t *end = file_data + file_size;
    //     for (i = 0; i < header->attr_count && ptr < end; i++) { ...
    //         ptr += sizeof(xattr_entry_header_t);
    //         ptr += entry->name_len;
    //         ptr += entry->value_len; }
    // so a crafted name_len/value_len near 2^32 put `ptr` FAR past `end`, the
    // `ptr < end` guard failed, and the walk TERMINATED.
    //
    // This seam's cursor is a uint32_t byte offset, so the previous
    // `*pos = (uint32_t)next;` TRUNCATED that same arithmetic and WRAPPED, which
    // made the walk CONTINUE from a bogus low offset where the original stopped:
    // value_len=0xFFFFFFFF, p=0, name_len=0 gives next=0x100000007 -> *pos=7.
    // Measured 87,159/600,000 (14.5%) divergences from the ORIGINAL on malformed
    // input, 87,159/87,159 of them wraps. Byte-identical on well-formed input,
    // i.e. wrong on exactly the hostile input this seam exists to handle, and
    // invisible to a 2-way rs-vs-c differential by construction.
    //
    // Clamping any out-of-range cursor to `len` re-enters the `p >= len` guard at
    // the top of the next call and terminates the walk, which is precisely what
    // the original's `ptr < end` did. This is a REPRESENTATION fix, not a bounds
    // check: it does NOT confine the OOB reads (those stay, by design, so the
    // rollback path stays honest). Clamping `*pos` specifically to `len` is safe
    // because EVERY cursor value >= len is observationally identical (the walk
    // stops and the caller uses `pos` only as this cursor), so no reachable case
    // changes.
    //
    // name_off/value_off are DELIBERATELY NOT clamped to `len`. They are consumed
    // by the caller as ADDRESSES (`entries + ent.name_off`), and the original
    // really did hand out a pointer PAST the block whenever an entry started
    // inside [0,len) with fewer than 8 bytes left: that IS advisory 0011's OOB
    // read #2, and clamping it to `len` would silently move the caller's read to
    // a DIFFERENT address, inventing new drift in the reachable domain instead of
    // removing it. They are guarded only at the u32 REPRESENTATION boundary, so
    // every representable value (i.e. every reachable one) is passed through
    // exactly as the original computed it, and only a value that cannot be
    // expressed at all is pinned to the top of the range rather than aliased back
    // down to a valid low offset. Reachability: name_off exceeds 2^32 only for
    // len > 0xFFFFFFF7 (a ~4 GiB .xat block), which cannot occur; that domain is
    // NOT exactly reproducible under this u32 cursor ABI and is not claimed to be.
    out->name_off     = (name_off  > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)name_off;
    out->value_off    = (value_off > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)value_off;
    out->value_len    = value_len;
    out->name_len     = name_len;
    out->namespace_id = ns;
    out->_pad         = 0;

    *pos = (next > (uint64_t)len) ? len : (uint32_t)next;
    return 1;
}
