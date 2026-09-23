/* sys/cdefs.h - MayteraOS shim, OUR FILE not upstream.
 *
 * Every NetBSD-derived bsd-games source opens with
 * `#include <sys/cdefs.h>` and then `__RCSID(...)` / `__COPYRIGHT(...)` for
 * an SCCS/RCS version-string comment that is never read at runtime. This
 * recipe does not need real BSD __attribute__ plumbing (__dead, __unused,
 * __P, etc. are not used by this file set - checked with grep before writing
 * this shim, not guessed), so only the two macros actually referenced are
 * defined, matching the real NetBSD/Debian wrapper's own fallback defines.
 */
#ifndef MAYTERA_BSDGAMES_CDEFS_H
#define MAYTERA_BSDGAMES_CDEFS_H

#define __RCSID(x)      static const char rcsid[] __attribute__((__unused__)) = x
#define __COPYRIGHT(x)  static const char copyright_str[] __attribute__((__unused__)) = x

#endif
