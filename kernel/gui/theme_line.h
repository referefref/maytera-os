// theme_line.h - shared out-struct for the theme-file line-record parse seam
// (#404 Rust strangler port of gui/theme_parser.c's untrusted-input tokenizer).
//
// The seam is the ATOM that theme_parse_ini()'s file loop repeats: read exactly
// ONE logical line out of the raw (NOT NUL-terminated) theme-file bytes and
// classify it into {skip | section-header | key=value}, with every fixed output
// field hard-capped. Themes are downloadable/editable user content (task #141),
// so `data[0..size]` is UNTRUSTED.
#ifndef THEME_LINE_H
#define THEME_LINE_H

// Freestanding kernel build: use the kernel's own fixed-width types (uint8_t /
// uint32_t / int32_t / size_t) from ../types.h, NOT the system <stdint.h> /
// <stddef.h> (the kernel is built -nostdinc and its size_t/uint64_t differ from
// the toolchain's, which would conflict). The offline differential harness has
// its own copy of this header that pulls the real <stdint.h>.
#include "../types.h"

// Caps are byte-identical to the live gui/theme_parser.c #defines.
#define TP_MAX_LINE_LEN     256
#define TP_MAX_SECTION_LEN  64
#define TP_MAX_KEY_LEN      64
#define TP_MAX_VALUE_LEN    128

// Line classification result.
#define TP_LINE_SKIP      0   // blank / comment / malformed -> ignored by loop
#define TP_LINE_SECTION   1   // [section]        -> section[] valid
#define TP_LINE_KEYVALUE  2   // key = value      -> key[]/value[] valid

// Fixed, #[repr(C)]-mirrored, sizeof-locked out-struct.
// Layout: kind@0 consumed@4 section@8 key@72 value@136 end@264. align 4.
typedef struct {
    int32_t  kind;                       // TP_LINE_*
    uint32_t consumed;                   // bytes advanced (incl trailing '\n')
    char     section[TP_MAX_SECTION_LEN];
    char     key[TP_MAX_KEY_LEN];
    char     value[TP_MAX_VALUE_LEN];
} theme_line_t;

_Static_assert(sizeof(theme_line_t) == 264, "theme_line_t sizeof locked at 264");

// C reference (verbatim gui/theme_parser.c logic) and Rust port share this ABI.
// Both return the number of input bytes consumed for this line (the advance),
// so a caller loops: p += theme_parse_line_x(data+p, size-p, &rec).
uint32_t theme_parse_line_c(const char *data, size_t size, theme_line_t *out);
uint32_t theme_parse_line_rs(const char *data, size_t size, theme_line_t *out);

#endif // THEME_LINE_H
