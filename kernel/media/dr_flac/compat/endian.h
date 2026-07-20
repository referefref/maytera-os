#ifndef _MAYTERA_DRFLAC_ENDIAN_H
#define _MAYTERA_DRFLAC_ENDIAN_H
/* Minimal endian shim. MayteraOS targets little-endian x86-64. dr_flac only
 * needs the byte-order macros if it falls back to the system header. */
#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN 4321
#endif
#ifndef __BYTE_ORDER
#define __BYTE_ORDER __LITTLE_ENDIAN
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __LITTLE_ENDIAN
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN __BIG_ENDIAN
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER __BYTE_ORDER
#endif
#endif
