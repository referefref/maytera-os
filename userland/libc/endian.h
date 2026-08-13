// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// endian.h - byte order macros and the htobe/htole/betoh/letoh families.
//
// Header only: every conversion is a gcc intrinsic or the identity, so there
// is no object file behind this and nothing can be missing at link time.
//
// MayteraOS userland is x86-64, which is little-endian. That is asserted at
// compile time below rather than assumed, so if this libc is ever built for a
// big-endian target the build STOPS instead of silently producing code that
// swaps the wrong way round.
#ifndef LIBC_ENDIAN_H
#define LIBC_ENDIAN_H

#include <stdint.h>
#include "byteswap.h"

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "userland/libc/endian.h: this libc targets little-endian x86-64 only"
#endif

#define __BYTE_ORDER __LITTLE_ENDIAN

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN    __BIG_ENDIAN
#endif
#ifndef PDP_ENDIAN
#define PDP_ENDIAN    __PDP_ENDIAN
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER    __BYTE_ORDER
#endif

// Host is little-endian: the "le" direction is the identity, the "be"
// direction is a bswap. Both directions of a swap are the same operation,
// which is why *toh and hto* share definitions.
#define htobe16(x) bswap_16(x)
#define htole16(x) ((uint16_t)(x))
#define be16toh(x) bswap_16(x)
#define le16toh(x) ((uint16_t)(x))

#define htobe32(x) bswap_32(x)
#define htole32(x) ((uint32_t)(x))
#define be32toh(x) bswap_32(x)
#define le32toh(x) ((uint32_t)(x))

#define htobe64(x) bswap_64(x)
#define htole64(x) ((uint64_t)(x))
#define be64toh(x) bswap_64(x)
#define le64toh(x) ((uint64_t)(x))

// BSD spellings, which a fair amount of ported code uses.
#define betoh16(x) be16toh(x)
#define letoh16(x) le16toh(x)
#define betoh32(x) be32toh(x)
#define letoh32(x) le32toh(x)
#define betoh64(x) be64toh(x)
#define letoh64(x) le64toh(x)

#endif // LIBC_ENDIAN_H
