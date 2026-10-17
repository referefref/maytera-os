// heapzero.h - #heapdbl: which of the TWO causes zeroed a heap block header.
//
// `magic=0x0` in a kfree() refusal has two possible causes and the message
// cannot tell them apart: coalesce_blocks() setting one field to zero after a
// genuine first free, or a 4 KB bulk clear written through a shadowed
// physical==virtual pointer (#aicappersist). The policy and the measurement live
// in rustkern/heapzero.rs; these are the declarations.
#ifndef MAYTERA_HEAPZERO_H
#define MAYTERA_HEAPZERO_H

#include "../types.h"

#define HZ_INDETERMINATE  0u
#define HZ_WIPE_PAGE      1u
#define HZ_DOUBLE_FREE    2u
#define HZ_WIPE_PARTIAL   3u

uint32_t heapzero_count_zeros_rs(const uint8_t *p, uint32_t len);
uint32_t heapzero_count_magics_rs(const uint8_t *p, uint32_t len, uint32_t magic);
// NOTE: hdr_zeros/hdr_len are REPORTED evidence, not a gate. The precondition is
// the caller's: mm/heap.c calls this only when the block's magic read back as zero.
// A canonically coalesced header is 24/32 zero, not 32/32, because
// coalesce_blocks() sets one u32 and `size` survives.
uint32_t heapzero_verdict_rs(uint32_t hdr_zeros, uint32_t hdr_len,
                             uint32_t data_zeros, uint32_t data_len,
                             uint32_t page_zeros, uint32_t page_len,
                             uint32_t page_magics);
const char *heapzero_verdict_name_rs(uint32_t verdict);
int32_t heapzero_selftest_rs(uint32_t *out_checks);

#endif // MAYTERA_HEAPZERO_H
