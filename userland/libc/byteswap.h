// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// byteswap.h - bswap_16/32/64 for MayteraOS userland.
//
// These are gcc intrinsics, not library calls, so this header needs no object
// file behind it and nothing can be missing at link time. -fno-builtin (which
// the libc and app builds both use) only stops gcc recognising library
// function NAMES; an explicit __builtin_bswapN is still expanded inline to the
// bswap instruction.
#ifndef LIBC_BYTESWAP_H
#define LIBC_BYTESWAP_H

#include <stdint.h>

#define bswap_16(x) ((uint16_t)__builtin_bswap16((uint16_t)(x)))
#define bswap_32(x) ((uint32_t)__builtin_bswap32((uint32_t)(x)))
#define bswap_64(x) ((uint64_t)__builtin_bswap64((uint64_t)(x)))

// glibc spells the internal ones with underscores; some ported code reaches
// for those directly.
#define __bswap_16(x) bswap_16(x)
#define __bswap_32(x) bswap_32(x)
#define __bswap_64(x) bswap_64(x)

#endif // LIBC_BYTESWAP_H
