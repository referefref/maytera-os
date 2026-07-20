// m3d_gcode.h - MayteraOS "3D Print" (#396) - M3D Micro G-code command model.
//
// Faithful C port of M33-Fio's "shared library source/gcode.cpp" (donovan6000,
// GPL). Implements the same parse -> ASCII -> binary framing the M3D printer's
// firmware expects: a 4-byte little-endian data-type bitmask, the present
// parameters packed in a fixed order (int16 / int32 / float32 / int8 / string),
// and a trailing Fletcher-16 checksum.
//
// The number formatting helpers (m3d_dtoa / m3d_atod) intentionally reproduce
// C++ std::to_string(double) == printf("%f") (6 decimals) and std::stod so that
// this port's ASCII output is byte-identical to M33-Fio's for the same input
// (see host_test/). The freestanding MayteraOS libc has no %f printf, so we
// never rely on it: the same code serves the host diff test and the OS build.
#ifndef M3D_GCODE_H
#define M3D_GCODE_H

#include <stdint.h>
#include <stdbool.h>

// Parameter offsets (index into param[]). Mirrors M33-Fio's ORDER string
// "NMGXYZE FTSP    IJRD" and orderToOffset[] table.
#define GC_N 0
#define GC_M 1
#define GC_G 2
#define GC_X 3
#define GC_Y 4
#define GC_Z 5
#define GC_E 6
#define GC_F 8
#define GC_T 9
#define GC_S 10
#define GC_P 11
#define GC_STRING 15   // string value (M23/M28/M29/M30/M32/M117 payload)
#define GC_I 16
#define GC_J 17
#define GC_R 18
#define GC_D 19

#define GC_ORDER_LEN 20
#define GC_VAL_MAX   96    // max characters per parameter value (string payload)
#define GC_LINE_MAX  256

// Initial data type (bit 7 and bit 12 set) - matches M33-Fio's 0x1080 sentinel
// meaning "no real parameters yet".
#define GC_EMPTY_DATATYPE 0x1080u

typedef struct {
    uint32_t dataType;
    char     param[GC_ORDER_LEN][GC_VAL_MAX];
    char     hostCommand[GC_LINE_MAX];   // "@..." host commands are passed through
    char     original[GC_LINE_MAX];
} m3d_gcode_t;

// --- Number formatting (shared, %f-compatible, no libc float printf needed) ---
// Format v with exactly 6 decimals like C++ std::to_string(double)/printf("%f").
void   m3d_dtoa(double v, char *out);
// Format an integer like std::to_string(long).
void   m3d_ltoa(long v, char *out);
// Parse a decimal double like std::stod (leading numeric text).
double m3d_atod(const char *s);
// Parse a leading int like std::stoi.
int    m3d_atoi_g(const char *s);

// --- Gcode ------------------------------------------------------------------
void m3d_gcode_clear(m3d_gcode_t *g);
// Parse a text g-code line. Returns true if it produced real g-code (or a host
// command). Comments (';') and checksums ('*') are stripped.
bool m3d_gcode_parse(m3d_gcode_t *g, const char *line);

bool m3d_gcode_is_empty(const m3d_gcode_t *g);
bool m3d_gcode_is_host(const m3d_gcode_t *g);
bool m3d_gcode_has_value(const m3d_gcode_t *g, char parameter);
bool m3d_gcode_has_parameter(const m3d_gcode_t *g, char parameter);
const char *m3d_gcode_get_value(const m3d_gcode_t *g, char parameter);
void m3d_gcode_set_value(m3d_gcode_t *g, char parameter, const char *value);
void m3d_gcode_remove_parameter(m3d_gcode_t *g, char parameter);

// Reconstruct the canonical ASCII command (as M33-Fio's getAscii()). out must
// hold at least GC_LINE_MAX*2 bytes for very wide commands.
void m3d_gcode_get_ascii(const m3d_gcode_t *g, char *out);

// Produce the M3D binary packet (data-type mask + packed params + Fletcher-16).
// Writes up to `cap` bytes into out, returns the packet length (or -1 if it
// would overflow cap).
//
// This is the M3D *Micro* wire format (32-bit data-type, 0x1080 sentinel,
// 2-byte M/G, string tail): byte-identical to M33-Fio's gcode.py getBinary.
int  m3d_gcode_get_binary(const m3d_gcode_t *g, uint8_t *out, int cap);

// Produce the M3D *Pro* binary packet (Repetier "Binary V2"), the framing the
// Pro's FirmwareController accepts (PROTOCOL.md #406 sec 4, from the decompiled
// M3DSpooling RepetierHost.model.GCode). It differs from the Micro format:
//   fields   uint16 LE (sentinel bit 0x80; V2 marker bit 0x1000)
//   fields2  uint16 LE (I=1, J=2, R=4)
//   textLen  uint8     (only when a string parameter is present)
//   N uint16; M uint16; G uint16; X,Y,Z,E,F float32; T uint8; S,P int32;
//   I,J,R float32; text bytes (no 16-byte padding); Fletcher-16.
// Same parsed m3d_gcode_t; only the wire framing differs. Returns the packet
// length or -1 on overflow.
int  m3d_gcode_get_binary_v2(const m3d_gcode_t *g, uint8_t *out, int cap);

// Offset for a parameter letter, or -1 if it is not a recognized parameter.
int  m3d_gcode_offset(char parameter);

#endif // M3D_GCODE_H
