#ifndef URL_H
#define URL_H

#include "../types.h"

// URL structure
typedef struct {
    char scheme[16];      // "http", "https", "ftp"
    char host[256];       // hostname or IP
    uint16_t port;        // port number (80 for http, 443 for https)
    char path[1024];      // path component starting with /
    char query[512];      // query string (after ?)
    char fragment[256];   // fragment (after #)
    char userinfo[128];   // user:password (optional)
} url_t;

// Parse a URL string into components
// Returns true on success, false on parse error.
// #404/#498 Phase P: pure Tier-2 parse seam. url_parse() is the live dispatcher;
// it routes to url_parse_rs (Rust, rustkern.rs) under -DRUST_URL, else the
// verbatim url_parse_c. url_t is the FFI struct (its #[repr(C)] mirror UrlParsed
// lives in rustkern.rs; sizeof asserted == 2194 in url.c so it can never drift).
bool url_parse(const char *url_string, url_t *out);
// Verbatim C reference (kept for the boot differential + trivial rollback).
bool url_parse_c(const char *url_string, url_t *out);
// Rust port (rustkern.rs): bounds-checked slice writes for every out field + a
// capped input scan (URL_MAX_INPUT); returns 1 on success, 0 on parse error.
extern int url_parse_rs(const char *url_string, url_t *out);
// Boot-time differential + perf + security self-test (logs [RUST-DIFF] url,
// [RUST-SEC] url, [RUST-PERF] url). Runs regardless of -DRUST_URL.
void url_rust_selftest(void);

// Resolve a relative URL against a base URL
// relative can be: "/path", "path", "../path", "?query", "#fragment", or full URL
void url_resolve_relative(const url_t *base, const char *relative, url_t *out);

// Convert URL back to string
// Returns length written (excluding null terminator)
int url_to_string(const url_t *url, char *buf, size_t buf_size);

// URL encode a string (spaces become %20, etc.)
int url_encode(const char *src, char *dst, size_t dst_size);

// URL decode a string (%20 becomes space, etc.)
int url_decode(const char *src, char *dst, size_t dst_size);

// Get default port for scheme
uint16_t url_default_port(const char *scheme);

#endif
