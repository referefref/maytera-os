// ipp.h - Internet Printing Protocol (IPP) client + print manager (#318)
//
// MayteraOS network printing. Builds IPP/1.1 binary requests, POSTs them over
// the kernel HTTP/1.1 client (net/wget.c http_post) to a CUPS/IPP server on
// port 631, and parses the IPP response. Also generates simple PostScript test
// pages, and persists printer configs to /CONFIG/PRINTERS.CFG.
#ifndef IPP_H
#define IPP_H

#include "../types.h"

// ---------------------------------------------------------------------------
// IPP wire constants (RFC 8011 / RFC 2910)
// ---------------------------------------------------------------------------
#define IPP_VERSION_MAJOR             1
#define IPP_VERSION_MINOR             1

// Operation IDs
#define IPP_OP_PRINT_JOB              0x0002
#define IPP_OP_VALIDATE_JOB           0x0004
#define IPP_OP_GET_JOB_ATTRIBUTES     0x0009
#define IPP_OP_GET_PRINTER_ATTRIBUTES 0x000B

// Status codes
#define IPP_STATUS_OK                 0x0000  // successful-ok
#define IPP_STATUS_OK_IGNORED         0x0001  // successful-ok-ignored-or-substituted-attributes
#define IPP_STATUS_OK_CONFLICTING     0x0002

// Delimiter / group tags
#define IPP_TAG_OPERATION             0x01
#define IPP_TAG_JOB                   0x02
#define IPP_TAG_END                   0x03
#define IPP_TAG_PRINTER               0x04
#define IPP_TAG_UNSUPPORTED_GRP       0x05

// Value tags
#define IPP_TAG_INTEGER               0x21
#define IPP_TAG_BOOLEAN               0x22
#define IPP_TAG_ENUM                  0x23
#define IPP_TAG_TEXT                  0x41  // textWithoutLanguage
#define IPP_TAG_NAME                  0x42  // nameWithoutLanguage
#define IPP_TAG_KEYWORD               0x44
#define IPP_TAG_URI                   0x45
#define IPP_TAG_CHARSET               0x47
#define IPP_TAG_LANGUAGE              0x48  // naturalLanguage
#define IPP_TAG_MIMETYPE             0x49  // mimeMediaType

// ---------------------------------------------------------------------------
// Printer configuration
// ---------------------------------------------------------------------------
#define PRINT_MAX_PRINTERS  8
#define PRINT_NAME_LEN      32
#define PRINT_HOST_LEN      64
#define PRINT_QUEUE_LEN     64

typedef struct {
    char     name[PRINT_NAME_LEN];   // friendly name
    char     host[PRINT_HOST_LEN];   // IPv4 dotted-quad or hostname
    char     queue[PRINT_QUEUE_LEN]; // CUPS queue / printer name
    uint16_t port;                   // IPP port (631 default)
    int      is_default;             // 1 = default printer
    int      valid;                  // slot in use
} printer_cfg_t;

// ---------------------------------------------------------------------------
// Low-level IPP client
//   Return: >=0 IPP status-code on a completed exchange; <0 transport error.
// ---------------------------------------------------------------------------
// Discover/validate a printer. Logs returned attributes to serial; if info_out
// is non-NULL, writes a short human-readable summary into it.
int ipp_get_printer_attributes(const char *host, uint16_t port, const char *queue,
                               char *info_out, int info_cap);

// Submit a document to a queue (Print-Job operation).
int ipp_print_job(const char *host, uint16_t port, const char *queue,
                  const char *job_name, const char *doc_format,
                  const void *doc, uint32_t doc_len);

// Submit a document to an explicit HTTP/IPP resource path, e.g. "/ipp/print"
// for a direct AirPrint printer or "/printers/<queue>" for CUPS.
int ipp_print_job_res(const char *host, uint16_t port, const char *resource,
                      const char *job_name, const char *doc_format,
                      const void *doc, uint32_t doc_len);

// Print an image FILE to host:port<resource>. A .jpg/.jpeg is sent as raw
// image/jpeg (the printer rasterizes); other formats are decoded to RGB and
// embedded as a PostScript colorimage page. Returns IPP status (>=0) or <0.
int ipp_print_image(const char *host, uint16_t port, const char *resource,
                    const char *path);

// ---------------------------------------------------------------------------
// PostScript generation
//   Render 'title' + newline-separated 'text' into a minimal PostScript page.
//   Returns bytes written (excluding NUL). Pass buf=NULL,cap=0 to size only.
// ---------------------------------------------------------------------------
int ps_generate_text_page(const char *title, const char *text, char *buf, int cap);

// Render decoded RGB pixels (0x00RRGGBB) as a PostScript colorimage page,
// downscaled so the larger pixel dimension is <= maxdim. Returns bytes or <0.
int ps_generate_image_page(const uint32_t *px, uint32_t sw, uint32_t sh,
                           int maxdim, char *buf, int cap);

// ---------------------------------------------------------------------------
// Print manager (high level + persistence)
// ---------------------------------------------------------------------------
void print_init(void);                          // load /CONFIG/PRINTERS.CFG
int  print_list(printer_cfg_t *out, int max);   // copy configured printers, returns count
int  print_add(const char *name, const char *host, uint16_t port,
               const char *queue, int make_default); // add + persist
int  print_remove(const char *name);            // remove + persist

// Render plain text as PostScript and print to a named printer (NULL=default).
int  print_job_text(const char *printer_name, const char *title, const char *text);
// Submit a raw document (already formatted) to a named printer (NULL=default).
int  print_job_doc(const char *printer_name, const char *job_name,
                   const char *doc_format, const void *doc, uint32_t len);
// Print an image file to a configured printer (NULL=default). JPEG is sent
// raw; other formats are decoded and PostScript-embedded.
int  print_job_image(const char *printer_name, const char *path);
// Print the current framebuffer (screen) to a configured printer (NULL=default).
int  print_job_screen(const char *printer_name);

// Gated boot self-tests (no-op unless /CONFIG/PRINTTEST.CFG or
// /CONFIG/PRINTIMG.CFG is present).
void print_start_deferred_selftest(void);

#endif // IPP_H
