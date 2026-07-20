// cards.h - Vector playing cards for MayteraOS
// Scalable vector rendering of playing cards without bitmaps
#ifndef CARDS_H
#define CARDS_H

#include "../types.h"

// Default card dimensions (can be scaled)
#define DEFAULT_CARD_WIDTH      60
#define DEFAULT_CARD_HEIGHT     80

// Card suit values (compatible with solitaire.h SUIT_* values)
#define CARD_SUIT_HEARTS    0
#define CARD_SUIT_DIAMONDS  1
#define CARD_SUIT_CLUBS     2
#define CARD_SUIT_SPADES    3

// Card values
#define CARD_VALUE_ACE      1
#define CARD_VALUE_JACK     11
#define CARD_VALUE_QUEEN    12
#define CARD_VALUE_KING     13

// Card colors
#define CARD_COLOR_WHITE        0x00FFFEF8  // Card face background
#define CARD_COLOR_RED          0x00CC2020  // Hearts and Diamonds
#define CARD_COLOR_BLACK        0x00202020  // Clubs and Spades
#define CARD_COLOR_BACK_BASE    0x00203080  // Card back base color
#define CARD_COLOR_BACK_PATTERN 0x00405090  // Card back pattern
#define CARD_COLOR_BORDER       0x00404040  // Card border
#define CARD_COLOR_HIGHLIGHT    0x00FFFF00  // Selection highlight

// Card back pattern types
typedef enum {
    CARD_BACK_CROSSHATCH,
    CARD_BACK_DIAMOND,
    CARD_BACK_WEAVE,
    CARD_BACK_SOLID
} card_back_pattern_t;

// Card rendering context for scaled rendering
typedef struct {
    int32_t x;          // Top-left X position
    int32_t y;          // Top-left Y position
    int32_t width;      // Card width (for scaling)
    int32_t height;     // Card height (for scaling)
    bool selected;      // Draw selection highlight
} card_render_ctx_t;

// ============================================================================
// Vector Shape Drawing Functions (primitives for suits)
// ============================================================================

// Draw a heart shape at position with given size
void card_draw_heart(int32_t cx, int32_t cy, int32_t size, uint32_t color);

// Draw a diamond shape at position with given size
void card_draw_diamond(int32_t cx, int32_t cy, int32_t size, uint32_t color);

// Draw a club shape at position with given size
void card_draw_club(int32_t cx, int32_t cy, int32_t size, uint32_t color);

// Draw a spade shape at position with given size
void card_draw_spade(int32_t cx, int32_t cy, int32_t size, uint32_t color);

// Draw any suit at position with given size
void card_draw_suit(uint8_t suit, int32_t cx, int32_t cy, int32_t size, uint32_t color);

// ============================================================================
// Card Face Rendering
// ============================================================================

// Draw a card face with value and suit
// Uses default dimensions (CARD_WIDTH x CARD_HEIGHT)
void card_draw_face(int32_t x, int32_t y, uint8_t value, uint8_t suit, bool selected);

// Draw a card face with custom dimensions (scalable)
void card_draw_face_scaled(card_render_ctx_t *ctx, uint8_t value, uint8_t suit);

// Draw pip patterns for number cards (A, 2-10)
void card_draw_pips(card_render_ctx_t *ctx, uint8_t value, uint8_t suit);

// Draw face card (J, Q, K) artwork
void card_draw_face_figure(card_render_ctx_t *ctx, uint8_t value, uint8_t suit);

// ============================================================================
// Card Back Rendering
// ============================================================================

// Draw card back with default pattern
void card_draw_back(int32_t x, int32_t y);

// Draw card back with custom dimensions
void card_draw_back_scaled(int32_t x, int32_t y, int32_t width, int32_t height);

// Draw card back with specific pattern
void card_draw_back_pattern(int32_t x, int32_t y, int32_t width, int32_t height,
                            card_back_pattern_t pattern, uint32_t color1, uint32_t color2);

// ============================================================================
// Utility Functions
// ============================================================================

// Get the color for a suit (red or black)
uint32_t card_get_suit_color(uint8_t suit);

// Check if suit is red (hearts or diamonds)
bool card_is_red_suit(uint8_t suit);

// Get display character for value ('A', '2'-'9', '10', 'J', 'Q', 'K')
void card_get_value_string(uint8_t value, char *buf);

// Get display character for suit
char card_get_suit_char(uint8_t suit);

// Draw empty card outline (for empty piles)
void card_draw_empty(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t color);

// Draw card with rounded corners effect
void card_draw_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                            uint32_t fill_color, uint32_t border_color, int32_t radius);

#endif // CARDS_H
