// browser.c - Web Browser GUI application for MayteraOS
#include "browser.h"
#include "window.h"
#include "desktop.h"
#include "icons.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../drivers/mouse.h"
#include "../net/wget.h"
#include "../net/https.h"
#include "../net/dns.h"
#include "../net/url.h"
#include "syslog.h"

// PS/2 typematic repeat filter: minimum ticks between identical chars
// Timer runs at ~20Hz (50ms/tick), so 3 ticks = ~150ms
#define BROWSER_KEY_REPEAT_TICKS 3

extern volatile uint64_t timer_ticks;

// Colors
#define BROWSER_BG              0x00FFFFFF  // White page background
#define BROWSER_FG              0x00000000  // Black text
#define BROWSER_TOOLBAR_BG      0x00F0F0F0  // Light gray toolbar
#define BROWSER_URLBAR_BG       0x00FFFFFF  // White URL bar
#define BROWSER_URLBAR_BORDER   0x00A0A0A0  // Gray border
#define BROWSER_URLBAR_FOCUS    0x004A90D9  // Blue focus border
#define BROWSER_STATUSBAR_BG    0x00E8E8E8  // Light gray status bar
#define BROWSER_LINK_COLOR      0x000000EE  // Blue links
#define BROWSER_BTN_BG          0x00E0E0E0  // Button background
#define BROWSER_BTN_HOVER       0x00C8C8C8  // Button hover
#define BROWSER_BTN_DISABLED    0x00B0B0B0  // Disabled button

// Home page
#define BROWSER_HOME_URL    "about:home"

// Forward declarations
static void browser_parse_html(browser_t *browser);
static void browser_fetch_page(browser_t *browser, const char *url);
static int snprintf_simple(char *buf, int size, const char *fmt, ...);

// Button positions (relative to toolbar)
static browser_button_t g_toolbar_buttons[BROWSER_BTN_COUNT] = {
    { 4, 4, 28, 28, "<", "Back", 0 },
    { 36, 4, 28, 28, ">", "Forward", 0 },
    { 68, 4, 28, 28, "R", "Reload", 1 },
    { 100, 4, 28, 28, "H", "Home", 1 },
    { 0, 0, 28, 28, "Go", "Go", 1 }  // Position set dynamically
};

// Draw text helper
static void browser_draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    const uint8_t *glyph;
    while (*text) {
        glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(x + col, y + row, color);
                    }
                }
            }
        }
        x += FONT_WIDTH;
        text++;
    }
}

// Draw centered text
static void browser_draw_text_centered(int32_t x, int32_t y, int32_t w, const char *text, uint32_t color) {
    int text_w = strlen(text) * FONT_WIDTH;
    int tx = x + (w - text_w) / 2;
    browser_draw_text(tx, y, text, color);
}

// Draw toolbar button
static void browser_draw_button(browser_t *browser, int btn_idx, int32_t wx, int32_t wy) {
    browser_button_t *btn = &g_toolbar_buttons[btn_idx];
    int32_t bx = wx + btn->x;
    int32_t by = wy + btn->y;

    uint32_t bg_color = BROWSER_BTN_BG;
    uint32_t fg_color = 0x00404040;

    if (!btn->enabled) {
        bg_color = BROWSER_BTN_DISABLED;
        fg_color = 0x00808080;
    } else if (browser->hover_button == btn_idx) {
        bg_color = BROWSER_BTN_HOVER;
    }

    // Draw button background
    fb_fill_rect(bx, by, btn->w, btn->h, bg_color);
    fb_draw_rect(bx, by, btn->w, btn->h, 0x00808080);

    // Draw icon/label
    if (btn->icon) {
        int text_y = by + (btn->h - FONT_HEIGHT) / 2;
        browser_draw_text_centered(bx, text_y, btn->w, btn->icon, fg_color);
    }
}

// Draw toolbar
static void browser_draw_toolbar(browser_t *browser) {
    if (!browser || !browser->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(browser->window, &wx, &wy, &ww, &wh);

    // Toolbar background
    fb_fill_rect(wx, wy, ww, BROWSER_TOOLBAR_HEIGHT, BROWSER_TOOLBAR_BG);

    // Draw navigation buttons
    for (int i = 0; i < BROWSER_BTN_GO; i++) {
        browser_draw_button(browser, i, wx, wy);
    }

    // URL bar
    int urlbar_x = wx + 140;
    int urlbar_y = wy + 6;
    int urlbar_w = ww - 180;  // Leave space for Go button
    int urlbar_h = BROWSER_URLBAR_HEIGHT;

    // URL bar background
    fb_fill_rect(urlbar_x, urlbar_y, urlbar_w, urlbar_h, BROWSER_URLBAR_BG);

    // URL bar border
    uint32_t border_color = browser->address_focused ? BROWSER_URLBAR_FOCUS : BROWSER_URLBAR_BORDER;
    fb_draw_rect(urlbar_x, urlbar_y, urlbar_w, urlbar_h, border_color);
    if (browser->address_focused) {
        fb_draw_rect(urlbar_x - 1, urlbar_y - 1, urlbar_w + 2, urlbar_h + 2, border_color);
    }

    // URL text
    int text_y = urlbar_y + (urlbar_h - FONT_HEIGHT) / 2;
    int max_chars = (urlbar_w - 8) / FONT_WIDTH;
    char display_url[256];
    strncpy(display_url, browser->address_text, max_chars);
    display_url[max_chars] = '\0';

    // Draw selection highlight if there's a selection
    if (browser->address_focused &&
        browser->address_selection_start != browser->address_selection_end) {
        int sel_start = browser->address_selection_start < browser->address_selection_end ?
                        browser->address_selection_start : browser->address_selection_end;
        int sel_end = browser->address_selection_start > browser->address_selection_end ?
                      browser->address_selection_start : browser->address_selection_end;

        // Clamp to visible range
        if (sel_start < 0) sel_start = 0;
        if (sel_end > max_chars) sel_end = max_chars;
        if (sel_start < max_chars && sel_end > 0) {
            int sel_x = urlbar_x + 4 + sel_start * FONT_WIDTH;
            int sel_w = (sel_end - sel_start) * FONT_WIDTH;
            fb_fill_rect(sel_x, text_y, sel_w, FONT_HEIGHT, 0x003390FF);  // Blue highlight

            // Draw selected text in white
            char sel_text[256];
            int sel_len = sel_end - sel_start;
            strncpy(sel_text, browser->address_text + sel_start, sel_len);
            sel_text[sel_len] = '\0';
            browser_draw_text(sel_x, text_y, sel_text, 0x00FFFFFF);  // White text

            // Draw non-selected text before selection
            if (sel_start > 0) {
                char before_text[256];
                strncpy(before_text, browser->address_text, sel_start);
                before_text[sel_start] = '\0';
                browser_draw_text(urlbar_x + 4, text_y, before_text, BROWSER_FG);
            }

            // Draw non-selected text after selection
            if (sel_end < (int)strlen(display_url)) {
                int after_x = urlbar_x + 4 + sel_end * FONT_WIDTH;
                browser_draw_text(after_x, text_y, browser->address_text + sel_end, BROWSER_FG);
            }
        } else {
            browser_draw_text(urlbar_x + 4, text_y, display_url, BROWSER_FG);
        }
    } else {
        browser_draw_text(urlbar_x + 4, text_y, display_url, BROWSER_FG);
    }

    // Cursor if focused (only when no selection)
    if (browser->address_focused &&
        browser->address_selection_start == browser->address_selection_end) {
        int cursor_x = urlbar_x + 4 + browser->address_cursor * FONT_WIDTH;
        fb_fill_rect(cursor_x, text_y, 2, FONT_HEIGHT, BROWSER_FG);
    }

    // Go button
    g_toolbar_buttons[BROWSER_BTN_GO].x = ww - 36;
    g_toolbar_buttons[BROWSER_BTN_GO].y = 4;
    browser_draw_button(browser, BROWSER_BTN_GO, wx, wy);

    // Separator line
    fb_fill_rect(wx, wy + BROWSER_TOOLBAR_HEIGHT - 1, ww, 1, 0x00C0C0C0);
}

// Draw status bar
static void browser_draw_statusbar(browser_t *browser) {
    if (!browser || !browser->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(browser->window, &wx, &wy, &ww, &wh);

    int32_t sy = wy + wh - BROWSER_STATUSBAR_HEIGHT;

    // Status bar background
    fb_fill_rect(wx, sy, ww, BROWSER_STATUSBAR_HEIGHT, BROWSER_STATUSBAR_BG);
    fb_fill_rect(wx, sy, ww, 1, 0x00C0C0C0);

    // Status text
    browser_draw_text(wx + 4, sy + 3, browser->status, 0x00404040);
}

// Draw page content
static void browser_draw_content(browser_t *browser) {
    if (!browser || !browser->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(browser->window, &wx, &wy, &ww, &wh);

    // Content area
    int32_t cx = wx;
    int32_t cy = wy + BROWSER_TOOLBAR_HEIGHT;
    int32_t cw = ww;
    int32_t ch = wh - BROWSER_TOOLBAR_HEIGHT - BROWSER_STATUSBAR_HEIGHT;

    // Clear content area
    fb_fill_rect(cx, cy, cw, ch, BROWSER_BG);

    // Draw content based on state
    if (browser->state == BROWSER_STATE_LOADING) {
        browser_draw_text_centered(cx, cy + ch / 2 - 8, cw, "Loading...", 0x00808080);
        return;
    }

    if (browser->state == BROWSER_STATE_ERROR) {
        browser_draw_text_centered(cx, cy + ch / 2 - 16, cw, "Error loading page", 0x00FF0000);
        browser_draw_text_centered(cx, cy + ch / 2 + 8, cw, browser->status, 0x00808080);
        return;
    }

    // Draw about:home
    if (strcmp(browser->url, BROWSER_HOME_URL) == 0) {
        browser_draw_text_centered(cx, cy + 50, cw, "Welcome to MayteraOS Browser", 0x00404040);
        browser_draw_text_centered(cx, cy + 80, cw, "Enter a URL above to browse the web", 0x00808080);
        browser_draw_text_centered(cx, cy + 120, cw, "Try: http://info.cern.ch", BROWSER_LINK_COLOR);
        return;
    }

    // Draw rendered text lines
    for (int i = 0; i < browser->line_count; i++) {
        text_line_t *line = &browser->lines[i];
        int ly = line->y - browser->scroll_y;

        if (ly < -FONT_HEIGHT || ly > ch) continue;  // Off screen

        uint32_t color = line->link ? BROWSER_LINK_COLOR : line->color;
        browser_draw_text(cx + line->x, cy + ly, line->text, color);
    }
}

// Create browser
browser_t *browser_create(void) {
    browser_t *browser = kzalloc(sizeof(browser_t));
    if (!browser) return NULL;

    browser->window = window_create("Browser", 100, 50, BROWSER_WIDTH, BROWSER_HEIGHT);
    if (!browser->window) {
        kfree(browser);
        return NULL;
    }

    // Set minimum size
    browser->window->flags |= WINDOW_FLAG_RESIZABLE;

    // Initialize state
    strcpy(browser->url, BROWSER_HOME_URL);
    strcpy(browser->title, "MayteraOS Browser");
    strcpy(browser->address_text, BROWSER_HOME_URL);
    strcpy(browser->status, "Ready");
    browser->state = BROWSER_STATE_IDLE;
    browser->hover_button = -1;
    browser->dock_index = -1;

    // Initialize history
    browser->history[0] = kmalloc(strlen(BROWSER_HOME_URL) + 1);
    if (browser->history[0]) {
        strcpy(browser->history[0], BROWSER_HOME_URL);
        browser->history_count = 1;
        browser->history_pos = 0;
    }

    // Initialize line buffer
    browser->line_capacity = 256;
    browser->lines = kmalloc(browser->line_capacity * sizeof(text_line_t));

    // Update button states
    g_toolbar_buttons[BROWSER_BTN_BACK].enabled = (browser->history_pos > 0);
    g_toolbar_buttons[BROWSER_BTN_FORWARD].enabled = (browser->history_pos < browser->history_count - 1);

    kprintf("[Browser] Created browser window\n");

    return browser;
}

// Destroy browser
void browser_destroy(browser_t *browser) {
    if (!browser) return;

    // Free history
    for (int i = 0; i < browser->history_count; i++) {
        if (browser->history[i]) kfree(browser->history[i]);
    }

    // Free page data
    if (browser->page_data) kfree(browser->page_data);

    // Free lines
    if (browser->lines) {
        for (int i = 0; i < browser->line_count; i++) {
            if (browser->lines[i].text) kfree(browser->lines[i].text);
        }
        kfree(browser->lines);
    }

    // Destroy window
    if (browser->window) window_destroy(browser->window);

    kfree(browser);
    kprintf("[Browser] Destroyed browser\n");
}

// Add line to content
static void browser_add_line(browser_t *browser, const char *text, int x, int y, uint32_t color, int is_link, const char *link_url) {
    if (browser->line_count >= browser->line_capacity) {
        browser->line_capacity *= 2;
        browser->lines = krealloc(browser->lines, browser->line_capacity * sizeof(text_line_t));
    }

    text_line_t *line = &browser->lines[browser->line_count++];
    line->text = kmalloc(strlen(text) + 1);
    strcpy(line->text, text);
    line->x = x;
    line->y = y;
    line->color = color;
    line->bold = 0;
    line->link = is_link;
    if (link_url) {
        strncpy(line->link_url, link_url, sizeof(line->link_url) - 1);
    } else {
        line->link_url[0] = '\0';
    }
}

// Simple HTML parser - extracts text content
static void browser_parse_html(browser_t *browser) {
    if (!browser->page_data || browser->page_len == 0) return;

    // Clear existing lines
    for (int i = 0; i < browser->line_count; i++) {
        if (browser->lines[i].text) kfree(browser->lines[i].text);
    }
    browser->line_count = 0;

    const char *p = (const char *)browser->page_data;
    const char *end = p + browser->page_len;

    int x = 10;
    int y = 10;
    int line_height = FONT_HEIGHT + 4;
    // #89: wrap text to the LIVE window content width so the page reflows
    // when the window is resized (was hardcoded BROWSER_WIDTH - 40).
    int max_width = BROWSER_WIDTH - 40;
    if (browser->window) {
        int32_t bwx, bwy, bww, bwh;
        window_get_content_bounds(browser->window, &bwx, &bwy, &bww, &bwh);
        max_width = bww - 40;
        if (max_width < 80) max_width = 80;
        browser->layout_width = bww;   // remember width this layout used
        browser->content_width = max_width;
    }

    char line_buf[256];
    int line_pos = 0;
    int in_tag = 0;
    int skip_content = 0;  // Skip script/style content
    char current_link[512] = {0};
    int in_link = 0;

    while (p < end) {
        char c = *p++;

        if (c == '<') {
            // Flush current text
            if (line_pos > 0) {
                line_buf[line_pos] = '\0';
                browser_add_line(browser, line_buf, x, y, BROWSER_FG, in_link, in_link ? current_link : NULL);
                x += line_pos * FONT_WIDTH;
                line_pos = 0;
            }

            in_tag = 1;
            char tag[64];
            int tag_pos = 0;

            // Read tag name
            while (p < end && *p != '>' && *p != ' ' && tag_pos < 63) {
                tag[tag_pos++] = *p++;
            }
            tag[tag_pos] = '\0';

            // Skip to end of tag
            while (p < end && *p != '>') {
                // Check for href in anchor tags
                if (strncmp(tag, "a", 2) == 0 || strncmp(tag, "A", 2) == 0) {
                    if (strncmp(p, "href=\"", 6) == 0 || strncmp(p, "HREF=\"", 6) == 0) {
                        p += 6;
                        int href_pos = 0;
                        while (p < end && *p != '"' && href_pos < 511) {
                            current_link[href_pos++] = *p++;
                        }
                        current_link[href_pos] = '\0';
                        in_link = 1;
                    }
                }
                p++;
            }
            if (p < end) p++;  // Skip '>'

            // Handle tags
            if (strcmp(tag, "br") == 0 || strcmp(tag, "BR") == 0 ||
                strcmp(tag, "br/") == 0 || strcmp(tag, "BR/") == 0) {
                x = 10;
                y += line_height;
            } else if (strcmp(tag, "p") == 0 || strcmp(tag, "P") == 0 ||
                       strcmp(tag, "/p") == 0 || strcmp(tag, "/P") == 0) {
                x = 10;
                y += line_height * 2;
            } else if (strcmp(tag, "h1") == 0 || strcmp(tag, "H1") == 0 ||
                       strcmp(tag, "h2") == 0 || strcmp(tag, "H2") == 0 ||
                       strcmp(tag, "h3") == 0 || strcmp(tag, "H3") == 0) {
                x = 10;
                y += line_height;
            } else if (tag[0] == '/' && (tag[1] == 'h' || tag[1] == 'H')) {
                x = 10;
                y += line_height;
            } else if (strcmp(tag, "/a") == 0 || strcmp(tag, "/A") == 0) {
                in_link = 0;
                current_link[0] = '\0';
            } else if (strcmp(tag, "script") == 0 || strcmp(tag, "SCRIPT") == 0 ||
                       strcmp(tag, "style") == 0 || strcmp(tag, "STYLE") == 0) {
                skip_content = 1;
            } else if (strcmp(tag, "/script") == 0 || strcmp(tag, "/SCRIPT") == 0 ||
                       strcmp(tag, "/style") == 0 || strcmp(tag, "/STYLE") == 0) {
                skip_content = 0;
            } else if (strcmp(tag, "title") == 0 || strcmp(tag, "TITLE") == 0) {
                // Extract title
                char title[256];
                int title_pos = 0;
                while (p < end && *p != '<' && title_pos < 255) {
                    if (*p != '\r' && *p != '\n') {
                        title[title_pos++] = *p;
                    }
                    p++;
                }
                title[title_pos] = '\0';
                strncpy(browser->title, title, sizeof(browser->title) - 1);
                window_set_title(browser->window, browser->title);
            }

            in_tag = 0;
        } else if (!in_tag && !skip_content) {
            // Handle entities
            if (c == '&') {
                if (strncmp(p, "nbsp;", 5) == 0) { c = ' '; p += 5; }
                else if (strncmp(p, "lt;", 3) == 0) { c = '<'; p += 3; }
                else if (strncmp(p, "gt;", 3) == 0) { c = '>'; p += 3; }
                else if (strncmp(p, "amp;", 4) == 0) { c = '&'; p += 4; }
                else if (strncmp(p, "quot;", 5) == 0) { c = '"'; p += 5; }
                else { c = ' '; while (p < end && *p != ';') p++; if (p < end) p++; }
            }

            // Skip control characters
            if (c == '\r') continue;

            // Handle newlines and spaces
            if (c == '\n' || c == '\t') c = ' ';

            // Collapse multiple spaces
            if (c == ' ' && line_pos > 0 && line_buf[line_pos - 1] == ' ') continue;

            // Add character
            if (c >= ' ' && c < 127) {
                line_buf[line_pos++] = c;

                // Wrap lines
                if (x + line_pos * FONT_WIDTH > max_width) {
                    // Find last space
                    int wrap_pos = line_pos - 1;
                    while (wrap_pos > 0 && line_buf[wrap_pos] != ' ') wrap_pos--;
                    if (wrap_pos == 0) wrap_pos = line_pos;

                    line_buf[wrap_pos] = '\0';
                    browser_add_line(browser, line_buf, x, y, BROWSER_FG, in_link, in_link ? current_link : NULL);

                    // Move remainder to start
                    x = 10;
                    y += line_height;
                    int remain = line_pos - wrap_pos - 1;
                    if (remain > 0) {
                        memmove(line_buf, line_buf + wrap_pos + 1, remain);
                    }
                    line_pos = remain > 0 ? remain : 0;
                }
            }
        }
    }

    // Flush remaining text
    if (line_pos > 0) {
        line_buf[line_pos] = '\0';
        browser_add_line(browser, line_buf, x, y, BROWSER_FG, in_link, in_link ? current_link : NULL);
        y += line_height;
    }

    browser->content_height = y + 20;
}

// Fetch page
static void browser_fetch_page(browser_t *browser, const char *url) {
    LOG_INFO("[Browser] Fetching page");
    browser->state = BROWSER_STATE_LOADING;
    snprintf_simple(browser->status, sizeof(browser->status), "Loading %s...", url);

    // Free old data
    if (browser->page_data) {
        kfree(browser->page_data);
        browser->page_data = NULL;
        browser->page_len = 0;
    }

    // Handle special URLs
    if (strcmp(url, BROWSER_HOME_URL) == 0) {
        browser->state = BROWSER_STATE_COMPLETE;
        strcpy(browser->status, "Ready");
        return;
    }

    // Determine protocol
    int is_https = (strncmp(url, "https://", 8) == 0);
    int is_http = (strncmp(url, "http://", 7) == 0);

    if (!is_http && !is_https) {
        // Assume http if no protocol
        char full_url[BROWSER_MAX_URL];
        snprintf_simple(full_url, sizeof(full_url), "http://%s", url);
        strcpy(browser->address_text, full_url);
        strcpy(browser->url, full_url);
        is_http = 1;
    }

    int ret;
    int status;

    if (is_https) {
        ret = https_get(url, &browser->page_data, &browser->page_len, &status);
    } else {
        ret = wget_fetch(url, &browser->page_data, &browser->page_len, &status);
    }

    if (ret < 0) {
        browser->state = BROWSER_STATE_ERROR;
        snprintf_simple(browser->status, sizeof(browser->status), "Error: %s",
                        is_https ? https_strerror(ret) : wget_strerror(ret));
        return;
    }

    if (status >= 400) {
        browser->state = BROWSER_STATE_ERROR;
        snprintf_simple(browser->status, sizeof(browser->status), "HTTP Error: %d", status);
        return;
    }

    // Parse content
    browser_parse_html(browser);

    browser->state = BROWSER_STATE_COMPLETE;
    browser->scroll_x = 0;
    browser->scroll_y = 0;
    snprintf_simple(browser->status, sizeof(browser->status), "Done - %d bytes", browser->page_len);
}

// Navigate to URL
void browser_navigate(browser_t *browser, const char *url) {
    if (!browser || !url) return;

    kprintf("[Browser] Navigate to: %s\n", url);

    // Update URL bar
    strncpy(browser->url, url, sizeof(browser->url) - 1);
    strncpy(browser->address_text, url, sizeof(browser->address_text) - 1);
    browser->address_cursor = strlen(browser->address_text);

    // Add to history
    if (browser->history_pos < browser->history_count - 1) {
        // Clear forward history
        for (int i = browser->history_pos + 1; i < browser->history_count; i++) {
            if (browser->history[i]) kfree(browser->history[i]);
            browser->history[i] = NULL;
        }
        browser->history_count = browser->history_pos + 1;
    }

    if (browser->history_count < BROWSER_MAX_HISTORY) {
        browser->history[browser->history_count] = kmalloc(strlen(url) + 1);
        if (browser->history[browser->history_count]) {
            strcpy(browser->history[browser->history_count], url);
            browser->history_count++;
            browser->history_pos = browser->history_count - 1;
        }
    }

    // Update button states
    g_toolbar_buttons[BROWSER_BTN_BACK].enabled = (browser->history_pos > 0);
    g_toolbar_buttons[BROWSER_BTN_FORWARD].enabled = 0;

    // Fetch the page
    browser_fetch_page(browser, url);

    // Redraw after fetch (state, status bar, and content all changed)
    wm_invalidate_all();
}

// Navigation functions
void browser_back(browser_t *browser) {
    if (!browser || browser->history_pos <= 0) return;

    browser->history_pos--;
    const char *url = browser->history[browser->history_pos];

    strncpy(browser->url, url, sizeof(browser->url) - 1);
    strncpy(browser->address_text, url, sizeof(browser->address_text) - 1);
    browser->address_cursor = strlen(browser->address_text);

    g_toolbar_buttons[BROWSER_BTN_BACK].enabled = (browser->history_pos > 0);
    g_toolbar_buttons[BROWSER_BTN_FORWARD].enabled = (browser->history_pos < browser->history_count - 1);

    browser_fetch_page(browser, url);
    wm_invalidate_all();
}

void browser_forward(browser_t *browser) {
    if (!browser || browser->history_pos >= browser->history_count - 1) return;

    browser->history_pos++;
    const char *url = browser->history[browser->history_pos];

    strncpy(browser->url, url, sizeof(browser->url) - 1);
    strncpy(browser->address_text, url, sizeof(browser->address_text) - 1);
    browser->address_cursor = strlen(browser->address_text);

    g_toolbar_buttons[BROWSER_BTN_BACK].enabled = (browser->history_pos > 0);
    g_toolbar_buttons[BROWSER_BTN_FORWARD].enabled = (browser->history_pos < browser->history_count - 1);

    browser_fetch_page(browser, url);
    wm_invalidate_all();
}

void browser_reload(browser_t *browser) {
    if (!browser) return;
    browser_fetch_page(browser, browser->url);
    wm_invalidate_all();
}

void browser_stop(browser_t *browser) {
    if (!browser) return;
    browser->state = BROWSER_STATE_IDLE;
    strcpy(browser->status, "Stopped");
}

void browser_home(browser_t *browser) {
    if (!browser) return;
    browser_navigate(browser, BROWSER_HOME_URL);
}

// Draw browser
void browser_draw(browser_t *browser) {
    if (!browser || !browser->window) return;

    // #89: if the window width changed since the page was laid out, re-wrap
    // the text to the new width (cheap: reflows from in-memory page_data, no
    // refetch) so content fills the resized window instead of staying at the
    // old width. Then clamp scroll to the new content height.
    if (browser->page_data && browser->page_len > 0 &&
        browser->state != BROWSER_STATE_LOADING) {
        int32_t wx, wy, ww, wh;
        window_get_content_bounds(browser->window, &wx, &wy, &ww, &wh);
        if (ww != browser->layout_width) {
            browser_parse_html(browser);
            int32_t ch = wh - BROWSER_TOOLBAR_HEIGHT - BROWSER_STATUSBAR_HEIGHT;
            if (browser->scroll_y > browser->content_height - ch)
                browser->scroll_y = browser->content_height - ch;
            if (browser->scroll_y < 0) browser->scroll_y = 0;
        }
    }

    browser_draw_toolbar(browser);
    browser_draw_content(browser);
    browser_draw_statusbar(browser);
}

// Simple snprintf for status messages
static int snprintf_simple(char *buf, int size, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    int written = 0;
    while (*fmt && written < size - 1) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 's') {
                const char *s = __builtin_va_arg(ap, const char *);
                while (*s && written < size - 1) buf[written++] = *s++;
            } else if (*fmt == 'd') {
                int val = __builtin_va_arg(ap, int);
                char tmp[16]; int neg = 0;
                if (val < 0) { neg = 1; val = -val; }
                int i = 0;
                do { tmp[i++] = '0' + (val % 10); val /= 10; } while (val > 0);
                if (neg && written < size - 1) buf[written++] = '-';
                while (i > 0 && written < size - 1) buf[written++] = tmp[--i];
            }
            fmt++;
        } else {
            buf[written++] = *fmt++;
        }
    }

    buf[written] = '\0';
    __builtin_va_end(ap);
    return written;
}

// Event handler
void browser_on_event(void *app_data, gui_event_t *event) {
    browser_t *browser = (browser_t *)app_data;
    if (!browser) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(browser->window, &wx, &wy, &ww, &wh);

    switch (event->type) {
        case EVENT_MOUSE_MOVE: {
            int mx = event->mouse_x - wx;
            int my = event->mouse_y - wy;

            // Check toolbar buttons
            browser->hover_button = -1;
            if (my >= 0 && my < BROWSER_TOOLBAR_HEIGHT) {
                for (int i = 0; i < BROWSER_BTN_COUNT; i++) {
                    browser_button_t *btn = &g_toolbar_buttons[i];
                    if (mx >= btn->x && mx < btn->x + btn->w &&
                        my >= btn->y && my < btn->y + btn->h) {
                        if (btn->enabled) {
                            browser->hover_button = i;
                        }
                        break;
                    }
                }
            }
            break;
        }

        case EVENT_MOUSE_DOWN: {
            int mx = event->mouse_x - wx;
            int my = event->mouse_y - wy;

            // URL bar click
            int urlbar_x = 140;
            int urlbar_w = ww - 180;
            if (my >= 6 && my < 30 && mx >= urlbar_x && mx < urlbar_x + urlbar_w) {
                if (!browser->address_focused) {
                    // First click - focus and select all; reset repeat filter
                    browser->address_focused = 1;
                    browser->last_key_char = 0;
                    browser->last_key_tick = 0;
                    browser->address_selection_start = 0;
                    browser->address_selection_end = strlen(browser->address_text);
                    browser->address_cursor = browser->address_selection_end;
                } else {
                    // Already focused - position cursor and clear selection
                    int rel_x = mx - urlbar_x - 4;
                    browser->address_cursor = rel_x / FONT_WIDTH;
                    if (browser->address_cursor > (int)strlen(browser->address_text)) {
                        browser->address_cursor = strlen(browser->address_text);
                    }
                    browser->address_selection_start = browser->address_cursor;
                    browser->address_selection_end = browser->address_cursor;
                }
            } else {
                browser->address_focused = 0;
                browser->address_selection_start = 0;
                browser->address_selection_end = 0;
            }

            // Toolbar button click
            if (my >= 0 && my < BROWSER_TOOLBAR_HEIGHT) {
                for (int i = 0; i < BROWSER_BTN_COUNT; i++) {
                    browser_button_t *btn = &g_toolbar_buttons[i];
                    if (btn->enabled && mx >= btn->x && mx < btn->x + btn->w &&
                        my >= btn->y && my < btn->y + btn->h) {
                        switch (i) {
                            case BROWSER_BTN_BACK: browser_back(browser); break;
                            case BROWSER_BTN_FORWARD: browser_forward(browser); break;
                            case BROWSER_BTN_RELOAD: browser_reload(browser); break;
                            case BROWSER_BTN_HOME: browser_home(browser); break;
                            case BROWSER_BTN_GO: browser_navigate(browser, browser->address_text); break;
                        }
                        break;
                    }
                }
            }

            // Content click - check for links
            int content_y = BROWSER_TOOLBAR_HEIGHT;
            int content_h = wh - BROWSER_TOOLBAR_HEIGHT - BROWSER_STATUSBAR_HEIGHT;
            if (my >= content_y && my < content_y + content_h) {
                int rel_y = my - content_y + browser->scroll_y;
                int rel_x = mx;

                for (int i = 0; i < browser->line_count; i++) {
                    text_line_t *line = &browser->lines[i];
                    if (line->link && rel_y >= line->y && rel_y < line->y + FONT_HEIGHT) {
                        int text_start = line->x;
                        int text_end = text_start + strlen(line->text) * FONT_WIDTH;
                        if (rel_x >= text_start && rel_x < text_end) {
                            // Click on link
                            if (line->link_url[0]) {
                                // Handle relative URLs
                                if (line->link_url[0] == '/') {
                                    // Absolute path - get host from current URL
                                    url_t parsed;
                                    if (url_parse(browser->url, &parsed)) {
                                        char full_url[BROWSER_MAX_URL];
                                        snprintf_simple(full_url, sizeof(full_url),
                                                        "%s://%s%s",
                                                        parsed.scheme, parsed.host, line->link_url);
                                        browser_navigate(browser, full_url);
                                    }
                                } else if (strncmp(line->link_url, "http", 4) == 0) {
                                    browser_navigate(browser, line->link_url);
                                } else {
                                    // Relative URL
                                    url_t base, resolved;
                                    if (url_parse(browser->url, &base)) {
                                        url_resolve_relative(&base, line->link_url, &resolved);
                                        char full_url[BROWSER_MAX_URL];
                                        url_to_string(&resolved, full_url, sizeof(full_url));
                                        browser_navigate(browser, full_url);
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
            }
            break;
        }

        case EVENT_KEY_DOWN: {
            if (browser->address_focused) {
                char c = event->key_char;
                int keycode = event->keycode;
                int has_selection = (browser->address_selection_start != browser->address_selection_end);

                // Filter PS/2 typematic repeat for printable characters
                if (c >= ' ' && c < 127) {
                    uint64_t now = timer_ticks;
                    if (c == (char)browser->last_key_char &&
                        (now - browser->last_key_tick) < BROWSER_KEY_REPEAT_TICKS) {
                        break;  // Suppress duplicate
                    }
                    browser->last_key_char = (uint32_t)c;
                    browser->last_key_tick = now;
                }

                if (keycode == 0x0A || keycode == 0x0D) {  // Enter
                    browser_navigate(browser, browser->address_text);
                    browser->address_focused = 0;
                    browser->address_selection_start = 0;
                    browser->address_selection_end = 0;
                } else if (keycode == 0x08) {  // Backspace
                    if (has_selection) {
                        // Delete selection
                        int sel_start = browser->address_selection_start < browser->address_selection_end ?
                                        browser->address_selection_start : browser->address_selection_end;
                        int sel_end = browser->address_selection_start > browser->address_selection_end ?
                                      browser->address_selection_start : browser->address_selection_end;
                        int len = strlen(browser->address_text);
                        memmove(browser->address_text + sel_start,
                                browser->address_text + sel_end,
                                len - sel_end + 1);
                        browser->address_cursor = sel_start;
                        browser->address_selection_start = sel_start;
                        browser->address_selection_end = sel_start;
                    } else if (browser->address_cursor > 0) {
                        int len = strlen(browser->address_text);
                        memmove(browser->address_text + browser->address_cursor - 1,
                                browser->address_text + browser->address_cursor,
                                len - browser->address_cursor + 1);
                        browser->address_cursor--;
                    }
                } else if (keycode == 0x82) {  // Left arrow
                    if (browser->address_cursor > 0) browser->address_cursor--;
                    browser->address_selection_start = browser->address_cursor;
                    browser->address_selection_end = browser->address_cursor;
                } else if (keycode == 0x83) {  // Right arrow
                    if (browser->address_cursor < (int)strlen(browser->address_text)) {
                        browser->address_cursor++;
                    }
                    browser->address_selection_start = browser->address_cursor;
                    browser->address_selection_end = browser->address_cursor;
                } else if (c >= ' ' && c < 127) {
                    if (has_selection) {
                        // Replace selection with typed character
                        int sel_start = browser->address_selection_start < browser->address_selection_end ?
                                        browser->address_selection_start : browser->address_selection_end;
                        int sel_end = browser->address_selection_start > browser->address_selection_end ?
                                      browser->address_selection_start : browser->address_selection_end;
                        int len = strlen(browser->address_text);
                        memmove(browser->address_text + sel_start + 1,
                                browser->address_text + sel_end,
                                len - sel_end + 1);
                        browser->address_text[sel_start] = c;
                        browser->address_cursor = sel_start + 1;
                        browser->address_selection_start = browser->address_cursor;
                        browser->address_selection_end = browser->address_cursor;
                    } else {
                        int len = strlen(browser->address_text);
                        if (len < BROWSER_MAX_URL - 1) {
                            memmove(browser->address_text + browser->address_cursor + 1,
                                    browser->address_text + browser->address_cursor,
                                    len - browser->address_cursor + 1);
                            browser->address_text[browser->address_cursor] = c;
                            browser->address_cursor++;
                        }
                    }
                }
                wm_invalidate_all();
            } else {
                // Page scrolling
                int keycode = event->keycode;
                if (keycode == 0x80) {  // Up
                    browser->scroll_y -= 20;
                    if (browser->scroll_y < 0) browser->scroll_y = 0;
                } else if (keycode == 0x81) {  // Down
                    browser->scroll_y += 20;
                } else if (keycode == 0x49) {  // Page Up
                    browser->scroll_y -= 200;
                    if (browser->scroll_y < 0) browser->scroll_y = 0;
                } else if (keycode == 0x51) {  // Page Down
                    browser->scroll_y += 200;
                }
                wm_invalidate_all();
            }
            break;
        }

        case EVENT_WINDOW_CLOSE:
            wm_unregister_app(browser->app_id);
            browser_destroy(browser);
            break;

        default:
            break;
    }
}

// Draw callback
void browser_on_draw(void *app_data) {
    browser_t *browser = (browser_t *)app_data;
    browser_draw(browser);
}

// Destroy callback
void browser_on_destroy(void *app_data) {
    browser_t *browser = (browser_t *)app_data;
    browser_destroy(browser);
}

// Launch browser (kernel-side entry point, called by apps/browser_launcher.c)
void browser_launch_kernel(void) {
    LOG_INFO("[Browser] Application launched");
    browser_launch_url(NULL);
}

// Launch with URL
void browser_launch_url(const char *url) {
    browser_t *browser = browser_create();
    if (!browser) {
        LOG_ERROR("[Browser] Failed to create browser window");
        kprintf("[Browser] Failed to create browser\n");
        return;
    }

    browser->app_id = wm_register_app(
        browser->window,
        (void *)browser,
        browser_on_event,
        browser_on_draw,
        browser_on_destroy
    );

    if (browser->app_id < 0) {
        LOG_ERROR("[Browser] Failed to register with window manager");
        kprintf("[Browser] Failed to register with window manager\n");
        browser_destroy(browser);
        return;
    }

    kprintf("[Browser] Launched with app_id %d\n", browser->app_id);

    if (url && strcmp(url, BROWSER_HOME_URL) != 0) {
        browser_navigate(browser, url);
    }
}
