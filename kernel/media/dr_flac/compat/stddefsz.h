#ifndef _MAYTERA_DRFLAC_SIZE_H
#define _MAYTERA_DRFLAC_SIZE_H
/* dr_flac includes <stddef.h> directly (gcc freestanding) for size_t, so we do
 * not redefine size_t here. This header only provides NULL if missing. */
#ifndef NULL
#define NULL ((void*)0)
#endif
#endif
