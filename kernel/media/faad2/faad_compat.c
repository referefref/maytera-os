/* faad_compat.c - #331. The libc helpers the vendored fixed-point faad2 may
 * reference (printf/fprintf/abort/stderr, abs/labs/memchr) are already provided
 * kernel-wide by media/opus/opus_compat.c, media/tremor/tremor_compat.c and
 * string.c, so nothing unique is needed here. Kept as an (empty) placeholder so
 * the media/faad2/*.c Makefile wildcard has a stable file set. faad2 = GPLv2. */
typedef int _maytera_faad_compat_unused;
