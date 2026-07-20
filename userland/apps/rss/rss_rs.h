/* rss_rs.h - C <-> Rust (userland, Ring-3) FFI for the MayteraOS feed parser.
 *
 * The parser (rss_rs.rs) is a no_std + alloc Rust staticlib linked into the rss
 * app ELF. It reads UNTRUSTED feed bytes (arbitrary network URLs) and never
 * panics or reads out of bounds. It handles RSS 2.0, Atom 1.0, and RSS 1.0/RDF
 * in one pass. All strings are malloc'd C strings on the ONE shared libc heap;
 * release the whole tree with rss_free(). Keep this struct layout byte-identical
 * to the #[repr(C)] structs in rss_rs.rs.
 */
#ifndef RSS_RS_H
#define RSS_RS_H

#include "../../libc/types.h"

/* Feed format codes (mirror FMT_* in rss_rs.rs). */
#define RSS_FMT_UNKNOWN 0
#define RSS_FMT_RSS2    1
#define RSS_FMT_ATOM    2
#define RSS_FMT_RDF     3

/* One feed item. Every field is a non-NULL, NUL-terminated C string (possibly
 * empty ""), so the C side can use it without NULL checks. */
typedef struct {
    char *title;
    char *link;
    char *description;   /* description / summary / content:encoded */
    char *pub_date;      /* pubDate / dc:date / updated / published */
    char *author;        /* author / atom author>name / dc:creator */
    char *enclosure;     /* enclosure url / link rel="enclosure" href */
    char *guid;          /* guid / atom id */
    /* Inline image for the item, "" when it has none. Found in priority order:
     * an <img src> inside the HTML body, then a media:content whose medium is
     * an image, then a media:thumbnail, then an image enclosure. The URL may be
     * RELATIVE and is attacker-controlled: resolve it against the feed URL and
     * check the scheme before fetching. image_title is xkcd's hover joke. */
    char *image_url;
    char *image_alt;
    char *image_title;
} rss_item_t;

/* A parsed feed. `error` is nonzero when nothing parseable was found (partial
 * data may still be present). `items` is NULL when item_count == 0. */
typedef struct {
    int         format;        /* RSS_FMT_* */
    int         error;         /* 0 = ok; nonzero = nothing/partial */
    char       *title;
    char       *link;
    char       *description;
    rss_item_t *items;
    int         item_count;
} rss_feed_t;

/* Parse `len` bytes at `data`. Returns NULL only on allocation failure. The
 * result must be released with rss_free(). Safe on malformed/hostile input. */
rss_feed_t *rss_parse(const unsigned char *data, unsigned long len);

/* Free a feed returned by rss_parse (all strings + the item array). */
void rss_free(rss_feed_t *feed);

/* Static human-readable name for a RSS_FMT_* code (never freed). */
const char *rss_format_name(int fmt);

/* size_of::<CItem>() as the Rust side sees it. Compare against
 * sizeof(rss_item_t) before the first rss_parse(): if the two definitions ever
 * drift, every string pointer past the drift is garbage. */
unsigned int rss_abi_item_size(void);

#endif /* RSS_RS_H */
