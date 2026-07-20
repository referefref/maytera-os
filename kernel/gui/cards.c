// cards.c - Vector playing cards implementation for MayteraOS
// Scalable vector rendering of playing cards without bitmaps

#include "cards.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../string.h"

// ============================================================================
// Internal Helper Functions
// ============================================================================

// Draw filled circle using midpoint algorithm
static void draw_filled_circle(int32_t cx, int32_t cy, int32_t radius, uint32_t color) {
    if (radius <= 0) return;

    for (int32_t y = -radius; y <= radius; y++) {
        int32_t half_width = 0;
        // Calculate x extent for this y using circle equation
        int32_t y2 = y * y;
        int32_t r2 = radius * radius;
        if (y2 <= r2) {
            // sqrt approximation using integer math
            int32_t x_sq = r2 - y2;
            half_width = 0;
            while ((half_width + 1) * (half_width + 1) <= x_sq) {
                half_width++;
            }
        }
        // Draw horizontal line
        for (int32_t x = -half_width; x <= half_width; x++) {
            fb_put_pixel(cx + x, cy + y, color);
        }
    }
}

// Draw filled triangle
static void draw_filled_triangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                  int32_t x3, int32_t y3, uint32_t color) {
    // Sort vertices by y coordinate
    if (y1 > y2) { int32_t t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    if (y1 > y3) { int32_t t = x1; x1 = x3; x3 = t; t = y1; y1 = y3; y3 = t; }
    if (y2 > y3) { int32_t t = x2; x2 = x3; x3 = t; t = y2; y2 = y3; y3 = t; }

    if (y3 == y1) return;  // Degenerate triangle

    // Scan line fill
    for (int32_t y = y1; y <= y3; y++) {
        int32_t xa, xb;

        // Calculate x intersections with triangle edges
        // Edge from (x1,y1) to (x3,y3) is always active
        if (y3 != y1) {
            xa = x1 + (x3 - x1) * (y - y1) / (y3 - y1);
        } else {
            xa = x1;
        }

        if (y < y2) {
            // Upper part: edge from (x1,y1) to (x2,y2)
            if (y2 != y1) {
                xb = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
            } else {
                xb = x1;
            }
        } else {
            // Lower part: edge from (x2,y2) to (x3,y3)
            if (y3 != y2) {
                xb = x2 + (x3 - x2) * (y - y2) / (y3 - y2);
            } else {
                xb = x2;
            }
        }

        if (xa > xb) { int32_t t = xa; xa = xb; xb = t; }

        for (int32_t x = xa; x <= xb; x++) {
            fb_put_pixel(x, y, color);
        }
    }
}

// Draw a single character at position
static void draw_card_char(int32_t x, int32_t y, char c, uint32_t color) {
    const uint8_t *glyph = font_get_glyph(c);
    if (!glyph) return;

    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                fb_put_pixel(x + col, y + row, color);
            }
        }
    }
}

// Draw text at position
static void draw_card_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    while (*text) {
        draw_card_char(x, y, *text, color);
        x += 8;
        text++;
    }
}

// ============================================================================
// Vector Shape Drawing Functions
// ============================================================================

void card_draw_heart(int32_t cx, int32_t cy, int32_t size, uint32_t color) {
    // Heart shape: two circles on top, triangle bottom
    int32_t radius = size / 3;
    int32_t circle_offset = size / 4;

    // Draw two overlapping circles at top
    draw_filled_circle(cx - circle_offset, cy - radius/2, radius, color);
    draw_filled_circle(cx + circle_offset, cy - radius/2, radius, color);

    // Draw triangle bottom
    int32_t top_y = cy - radius/2 + radius/2;
    int32_t bottom_y = cy + size/2;
    int32_t half_width = size/2 + radius/4;

    draw_filled_triangle(cx - half_width, top_y, cx + half_width, top_y,
                        cx, bottom_y, color);

    // Fill gap between circles and triangle
    int32_t fill_top = cy - radius/2;
    int32_t fill_bottom = top_y + 2;
    for (int32_t y = fill_top; y <= fill_bottom; y++) {
        int32_t w = half_width * (fill_bottom - y + 4) / (fill_bottom - fill_top + 4);
        for (int32_t x = cx - w; x <= cx + w; x++) {
            fb_put_pixel(x, y, color);
        }
    }
}

void card_draw_diamond(int32_t cx, int32_t cy, int32_t size, uint32_t color) {
    // Diamond: four triangles forming a rhombus
    int32_t hw = size / 2;      // Half width
    int32_t hh = size * 2 / 3;  // Half height (taller than wide)

    // Draw as two triangles meeting in middle
    draw_filled_triangle(cx, cy - hh, cx - hw, cy, cx + hw, cy, color);
    draw_filled_triangle(cx, cy + hh, cx - hw, cy, cx + hw, cy, color);
}

void card_draw_club(int32_t cx, int32_t cy, int32_t size, uint32_t color) {
    // Club: three circles in clover pattern + stem
    int32_t radius = size / 3;
    int32_t offset = size / 4;

    // Top circle
    draw_filled_circle(cx, cy - offset, radius, color);

    // Left and right circles
    draw_filled_circle(cx - offset - 2, cy + offset/2, radius, color);
    draw_filled_circle(cx + offset + 2, cy + offset/2, radius, color);

    // Stem
    int32_t stem_top = cy + radius/2;
    int32_t stem_bottom = cy + size/2;
    int32_t stem_width = size / 6;

    draw_filled_triangle(cx - stem_width*2, stem_bottom,
                        cx + stem_width*2, stem_bottom,
                        cx, stem_top, color);
}

void card_draw_spade(int32_t cx, int32_t cy, int32_t size, uint32_t color) {
    // Spade: inverted heart + stem
    int32_t radius = size / 3;
    int32_t circle_offset = size / 4;

    // Draw pointed top (inverted triangle)
    int32_t top_y = cy - size/2;
    int32_t mid_y = cy - radius/4;
    int32_t half_width = size/2 + radius/4;

    draw_filled_triangle(cx, top_y, cx - half_width, mid_y, cx + half_width, mid_y, color);

    // Draw two circles at bottom of spade body
    draw_filled_circle(cx - circle_offset, cy + radius/4, radius, color);
    draw_filled_circle(cx + circle_offset, cy + radius/4, radius, color);

    // Fill gap between triangle and circles
    int32_t fill_top = mid_y - 2;
    int32_t fill_bottom = cy + radius/4;
    for (int32_t y = fill_top; y <= fill_bottom; y++) {
        int32_t progress = (y - fill_top) * 100 / (fill_bottom - fill_top + 1);
        int32_t w = (half_width * (100 - progress) + (circle_offset + radius) * progress) / 100;
        for (int32_t x = cx - w; x <= cx + w; x++) {
            fb_put_pixel(x, y, color);
        }
    }

    // Stem
    int32_t stem_top = cy + radius/2;
    int32_t stem_bottom = cy + size/2;
    int32_t stem_width = size / 6;

    draw_filled_triangle(cx - stem_width*2, stem_bottom,
                        cx + stem_width*2, stem_bottom,
                        cx, stem_top, color);
}

void card_draw_suit(uint8_t suit, int32_t cx, int32_t cy, int32_t size, uint32_t color) {
    switch (suit) {
        case CARD_SUIT_HEARTS:
            card_draw_heart(cx, cy, size, color);
            break;
        case CARD_SUIT_DIAMONDS:
            card_draw_diamond(cx, cy, size, color);
            break;
        case CARD_SUIT_CLUBS:
            card_draw_club(cx, cy, size, color);
            break;
        case CARD_SUIT_SPADES:
            card_draw_spade(cx, cy, size, color);
            break;
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

uint32_t card_get_suit_color(uint8_t suit) {
    return card_is_red_suit(suit) ? CARD_COLOR_RED : CARD_COLOR_BLACK;
}

bool card_is_red_suit(uint8_t suit) {
    return suit == CARD_SUIT_HEARTS || suit == CARD_SUIT_DIAMONDS;
}

void card_get_value_string(uint8_t value, char *buf) {
    switch (value) {
        case CARD_VALUE_ACE:   buf[0] = 'A'; buf[1] = 0; break;
        case 10:               buf[0] = '1'; buf[1] = '0'; buf[2] = 0; break;
        case CARD_VALUE_JACK:  buf[0] = 'J'; buf[1] = 0; break;
        case CARD_VALUE_QUEEN: buf[0] = 'Q'; buf[1] = 0; break;
        case CARD_VALUE_KING:  buf[0] = 'K'; buf[1] = 0; break;
        default:
            buf[0] = '0' + value;
            buf[1] = 0;
            break;
    }
}

char card_get_suit_char(uint8_t suit) {
    switch (suit) {
        case CARD_SUIT_HEARTS:   return 'H';
        case CARD_SUIT_DIAMONDS: return 'D';
        case CARD_SUIT_CLUBS:    return 'C';
        case CARD_SUIT_SPADES:   return 'S';
        default: return '?';
    }
}

// ============================================================================
// Card Drawing Primitives
// ============================================================================

void card_draw_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                            uint32_t fill_color, uint32_t border_color, int32_t radius) {
    // Draw filled rectangle
    fb_fill_rect(x, y, w, h, fill_color);

    // Draw border
    fb_draw_rect(x, y, w, h, border_color);

    // Simple corner rounding by drawing small filled corners
    if (radius > 0 && radius < 4) {
        // Just cut corners with fill color to simulate rounding
        // Top-left
        fb_put_pixel(x, y, fill_color);
        // Top-right
        fb_put_pixel(x + w - 1, y, fill_color);
        // Bottom-left
        fb_put_pixel(x, y + h - 1, fill_color);
        // Bottom-right
        fb_put_pixel(x + w - 1, y + h - 1, fill_color);
    }
}

void card_draw_empty(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color) {
    // Draw dashed/empty pile outline
    fb_draw_rect(x, y, width, height, color);
    fb_draw_rect(x + 1, y + 1, width - 2, height - 2, color);
}

// ============================================================================
// Card Face Rendering
// ============================================================================

void card_draw_pips(card_render_ctx_t *ctx, uint8_t value, uint8_t suit) {
    uint32_t color = card_get_suit_color(suit);
    int32_t pip_size = ctx->width / 5;  // Scale pip size with card

    // Calculate positions for pip grid
    int32_t cx = ctx->x + ctx->width / 2;
    int32_t left_x = ctx->x + ctx->width / 4;
    int32_t right_x = ctx->x + ctx->width * 3 / 4;

    int32_t top_y = ctx->y + ctx->height / 4;
    int32_t mid_y = ctx->y + ctx->height / 2;
    int32_t bot_y = ctx->y + ctx->height * 3 / 4;
    int32_t q1_y = ctx->y + ctx->height * 3 / 8;
    int32_t q3_y = ctx->y + ctx->height * 5 / 8;

    // Draw pips based on card value
    switch (value) {
        case 1:  // Ace - single large center pip
            card_draw_suit(suit, cx, mid_y, pip_size * 2, color);
            break;

        case 2:
            card_draw_suit(suit, cx, top_y, pip_size, color);
            card_draw_suit(suit, cx, bot_y, pip_size, color);
            break;

        case 3:
            card_draw_suit(suit, cx, top_y, pip_size, color);
            card_draw_suit(suit, cx, mid_y, pip_size, color);
            card_draw_suit(suit, cx, bot_y, pip_size, color);
            break;

        case 4:
            card_draw_suit(suit, left_x, top_y, pip_size, color);
            card_draw_suit(suit, right_x, top_y, pip_size, color);
            card_draw_suit(suit, left_x, bot_y, pip_size, color);
            card_draw_suit(suit, right_x, bot_y, pip_size, color);
            break;

        case 5:
            card_draw_suit(suit, left_x, top_y, pip_size, color);
            card_draw_suit(suit, right_x, top_y, pip_size, color);
            card_draw_suit(suit, cx, mid_y, pip_size, color);
            card_draw_suit(suit, left_x, bot_y, pip_size, color);
            card_draw_suit(suit, right_x, bot_y, pip_size, color);
            break;

        case 6:
            card_draw_suit(suit, left_x, top_y, pip_size, color);
            card_draw_suit(suit, right_x, top_y, pip_size, color);
            card_draw_suit(suit, left_x, mid_y, pip_size, color);
            card_draw_suit(suit, right_x, mid_y, pip_size, color);
            card_draw_suit(suit, left_x, bot_y, pip_size, color);
            card_draw_suit(suit, right_x, bot_y, pip_size, color);
            break;

        case 7:
            card_draw_suit(suit, left_x, top_y, pip_size, color);
            card_draw_suit(suit, right_x, top_y, pip_size, color);
            card_draw_suit(suit, cx, q1_y, pip_size, color);
            card_draw_suit(suit, left_x, mid_y, pip_size, color);
            card_draw_suit(suit, right_x, mid_y, pip_size, color);
            card_draw_suit(suit, left_x, bot_y, pip_size, color);
            card_draw_suit(suit, right_x, bot_y, pip_size, color);
            break;

        case 8:
            card_draw_suit(suit, left_x, top_y, pip_size, color);
            card_draw_suit(suit, right_x, top_y, pip_size, color);
            card_draw_suit(suit, cx, q1_y, pip_size, color);
            card_draw_suit(suit, left_x, mid_y, pip_size, color);
            card_draw_suit(suit, right_x, mid_y, pip_size, color);
            card_draw_suit(suit, cx, q3_y, pip_size, color);
            card_draw_suit(suit, left_x, bot_y, pip_size, color);
            card_draw_suit(suit, right_x, bot_y, pip_size, color);
            break;

        case 9:
            card_draw_suit(suit, left_x, top_y, pip_size, color);
            card_draw_suit(suit, right_x, top_y, pip_size, color);
            card_draw_suit(suit, left_x, q1_y, pip_size, color);
            card_draw_suit(suit, right_x, q1_y, pip_size, color);
            card_draw_suit(suit, cx, mid_y, pip_size, color);
            card_draw_suit(suit, left_x, q3_y, pip_size, color);
            card_draw_suit(suit, right_x, q3_y, pip_size, color);
            card_draw_suit(suit, left_x, bot_y, pip_size, color);
            card_draw_suit(suit, right_x, bot_y, pip_size, color);
            break;

        case 10:
            card_draw_suit(suit, left_x, top_y, pip_size, color);
            card_draw_suit(suit, right_x, top_y, pip_size, color);
            card_draw_suit(suit, cx, top_y + pip_size, pip_size, color);
            card_draw_suit(suit, left_x, q1_y, pip_size, color);
            card_draw_suit(suit, right_x, q1_y, pip_size, color);
            card_draw_suit(suit, left_x, q3_y, pip_size, color);
            card_draw_suit(suit, right_x, q3_y, pip_size, color);
            card_draw_suit(suit, cx, bot_y - pip_size, pip_size, color);
            card_draw_suit(suit, left_x, bot_y, pip_size, color);
            card_draw_suit(suit, right_x, bot_y, pip_size, color);
            break;
    }
}

void card_draw_face_figure(card_render_ctx_t *ctx, uint8_t value, uint8_t suit) {
    uint32_t color = card_get_suit_color(suit);
    int32_t cx = ctx->x + ctx->width / 2;
    int32_t cy = ctx->y + ctx->height / 2;

    // Draw a stylized figure in the center
    // This is a simplified representation using geometric shapes

    // Draw crown/hat for royalty
    int32_t crown_top = cy - ctx->height / 4;
    int32_t crown_bottom = cy - ctx->height / 8;
    int32_t crown_width = ctx->width / 3;

    if (value == CARD_VALUE_KING) {
        // Draw simple crown shape
        draw_filled_triangle(cx - crown_width/2, crown_bottom,
                            cx + crown_width/2, crown_bottom,
                            cx, crown_top, color);
        // Crown points
        draw_filled_triangle(cx - crown_width/3, crown_bottom - 4,
                            cx, crown_bottom - 4,
                            cx - crown_width/6, crown_top + 4, color);
        draw_filled_triangle(cx, crown_bottom - 4,
                            cx + crown_width/3, crown_bottom - 4,
                            cx + crown_width/6, crown_top + 4, color);
    } else if (value == CARD_VALUE_QUEEN) {
        // Draw crown with curved top (approximated)
        fb_fill_rect(cx - crown_width/2, crown_bottom - 8, crown_width, 8, color);
        draw_filled_circle(cx - crown_width/4, crown_bottom - 8, 4, color);
        draw_filled_circle(cx, crown_bottom - 12, 5, color);
        draw_filled_circle(cx + crown_width/4, crown_bottom - 8, 4, color);
    } else if (value == CARD_VALUE_JACK) {
        // Draw cap/hat
        fb_fill_rect(cx - crown_width/2, crown_bottom - 6, crown_width, 6, color);
        draw_filled_circle(cx, crown_bottom - 10, 6, color);
    }

    // Draw face (simple oval)
    int32_t face_top = crown_bottom + 2;
    int32_t face_radius = ctx->width / 6;
    draw_filled_circle(cx, face_top + face_radius, face_radius, 0x00FFD0A0);  // Skin tone

    // Draw body
    int32_t body_top = face_top + face_radius * 2;
    int32_t body_bottom = cy + ctx->height / 4;
    fb_fill_rect(cx - ctx->width/4, body_top, ctx->width/2, body_bottom - body_top, color);

    // Draw large suit symbol at bottom
    card_draw_suit(suit, cx, body_bottom + ctx->height/8, ctx->width / 4, color);
}

void card_draw_face_scaled(card_render_ctx_t *ctx, uint8_t value, uint8_t suit) {
    uint32_t border_color = ctx->selected ? CARD_COLOR_HIGHLIGHT : CARD_COLOR_BORDER;
    uint32_t suit_color = card_get_suit_color(suit);

    // Draw card background with border
    card_draw_rounded_rect(ctx->x, ctx->y, ctx->width, ctx->height,
                          CARD_COLOR_WHITE, border_color, 2);

    // Draw selection highlight if selected
    if (ctx->selected) {
        fb_draw_rect(ctx->x + 1, ctx->y + 1, ctx->width - 2, ctx->height - 2, CARD_COLOR_HIGHLIGHT);
    }

    // Draw corner values and suits
    char value_str[3];
    card_get_value_string(value, value_str);

    // Top-left corner
    int32_t corner_x = ctx->x + 4;
    int32_t corner_y = ctx->y + 4;
    draw_card_text(corner_x, corner_y, value_str, suit_color);
    card_draw_suit(suit, corner_x + 4, corner_y + 20, 8, suit_color);

    // Bottom-right corner
    int32_t br_x = ctx->x + ctx->width - 12;
    int32_t br_y = ctx->y + ctx->height - 36;
    draw_card_text(br_x, br_y, value_str, suit_color);
    card_draw_suit(suit, br_x + 4, br_y + 20, 8, suit_color);

    // Draw center content based on value
    if (value >= 1 && value <= 10) {
        card_draw_pips(ctx, value, suit);
    } else {
        card_draw_face_figure(ctx, value, suit);
    }
}

void card_draw_face(int32_t x, int32_t y, uint8_t value, uint8_t suit, bool selected) {
    card_render_ctx_t ctx = {
        .x = x,
        .y = y,
        .width = DEFAULT_CARD_WIDTH,
        .height = DEFAULT_CARD_HEIGHT,
        .selected = selected
    };
    card_draw_face_scaled(&ctx, value, suit);
}

// ============================================================================
// Card Back Rendering
// ============================================================================

void card_draw_back_pattern(int32_t x, int32_t y, int32_t width, int32_t height,
                            card_back_pattern_t pattern, uint32_t color1, uint32_t color2) {
    // Draw base rectangle
    fb_fill_rect(x, y, width, height, color1);
    fb_draw_rect(x, y, width, height, CARD_COLOR_BORDER);

    // Draw inner border
    fb_draw_rect(x + 3, y + 3, width - 6, height - 6, color2);

    // Draw pattern
    switch (pattern) {
        case CARD_BACK_CROSSHATCH:
            for (int32_t py = y + 4; py < y + height - 4; py += 4) {
                for (int32_t px = x + 4; px < x + width - 4; px += 4) {
                    fb_put_pixel(px, py, color2);
                }
            }
            break;

        case CARD_BACK_DIAMOND:
            for (int32_t py = y + 6; py < y + height - 6; py += 8) {
                for (int32_t px = x + 6; px < x + width - 6; px += 8) {
                    // Small diamond
                    fb_put_pixel(px, py - 2, color2);
                    fb_put_pixel(px - 1, py - 1, color2);
                    fb_put_pixel(px + 1, py - 1, color2);
                    fb_put_pixel(px - 2, py, color2);
                    fb_put_pixel(px + 2, py, color2);
                    fb_put_pixel(px - 1, py + 1, color2);
                    fb_put_pixel(px + 1, py + 1, color2);
                    fb_put_pixel(px, py + 2, color2);
                }
            }
            break;

        case CARD_BACK_WEAVE:
            for (int32_t py = y + 4; py < y + height - 4; py += 2) {
                int32_t offset = ((py - y) / 2) % 4;
                for (int32_t px = x + 4 + offset; px < x + width - 4; px += 4) {
                    fb_put_pixel(px, py, color2);
                    fb_put_pixel(px + 1, py, color2);
                }
            }
            break;

        case CARD_BACK_SOLID:
        default:
            // Just the base color, already drawn
            break;
    }
}

void card_draw_back_scaled(int32_t x, int32_t y, int32_t width, int32_t height) {
    card_draw_back_pattern(x, y, width, height, CARD_BACK_CROSSHATCH,
                          CARD_COLOR_BACK_BASE, CARD_COLOR_BACK_PATTERN);
}

void card_draw_back(int32_t x, int32_t y) {
    card_draw_back_scaled(x, y, DEFAULT_CARD_WIDTH, DEFAULT_CARD_HEIGHT);
}
