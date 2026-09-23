/* malloc.h shim - MayteraOS port (#745). userland/libc declares malloc/free
 * in stdlib.h (ISO C placement), not a separate malloc.h (glibc-ism); this
 * upstream 1990s codebase includes <malloc.h> in ~20 files. Forward it. */
#ifndef XAOS_MALLOC_H_SHIM
#define XAOS_MALLOC_H_SHIM
#include <stdlib.h>
#endif
