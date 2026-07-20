#ifndef _MAYTERA_DRFLAC_ASSERT_H
#define _MAYTERA_DRFLAC_ASSERT_H
/* Freestanding kernel: assertions compile to nothing. */
#ifndef assert
#define assert(expr) ((void)0)
#endif
#endif
