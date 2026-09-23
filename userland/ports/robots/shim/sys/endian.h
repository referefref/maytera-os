/* sys/endian.h - MayteraOS shim, OUR FILE not upstream.
 *
 * robots.h unconditionally includes <sys/endian.h> for htonl()/ntohl()
 * (score.c uses them to store high scores in a fixed byte order). MayteraOS
 * libc has neither a sys/endian.h nor <netinet/in.h>: htonl()/ntohl() live
 * in sys/socket.h (checked: static inline there). This shim just forwards.
 */
#ifndef MAYTERA_BSDGAMES_SYS_ENDIAN_H
#define MAYTERA_BSDGAMES_SYS_ENDIAN_H
#include <sys/socket.h>
#include <stdint.h>
/* robots.h also wants the BSD u_int32_t alias for its on-disk score struct;
 * MayteraOS libc has only the ISO uint32_t. */
typedef uint32_t u_int32_t;
#endif
