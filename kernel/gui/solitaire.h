// solitaire.h - Klondike Solitaire for MayteraOS
#ifndef SOLITAIRE_H
#define SOLITAIRE_H

#include "window.h"

// Card dimensions
#define CARD_WIDTH      60
#define CARD_HEIGHT     80
#define CARD_SPACING    4
#define CARD_STACK_OFFSET 15  // Vertical offset for stacked face-up cards

// Game layout
#define TABLEAU_PILES   7
#define FOUNDATION_PILES 4

// Card suits
typedef enum {
    SUIT_HEARTS = 0,
    SUIT_DIAMONDS = 1,
    SUIT_CLUBS = 2,
    SUIT_SPADES = 3
} card_suit_t;

// Card values (1 = Ace, 11 = Jack, 12 = Queen, 13 = King)
#define CARD_ACE    1
#define CARD_JACK   11
#define CARD_QUEEN  12
#define CARD_KING   13

// Card structure
typedef struct {
    uint8_t value;      // 1-13 (Ace to King)
    uint8_t suit;       // 0-3 (Hearts, Diamonds, Clubs, Spades)
    bool face_up;       // Is card face up?
} card_t;

// Pile types
typedef enum {
    PILE_STOCK,
    PILE_WASTE,
    PILE_FOUNDATION,
    PILE_TABLEAU
} pile_type_t;

// Card pile (max 24 cards in any pile)
#define MAX_PILE_SIZE 24

typedef struct {
    card_t cards[MAX_PILE_SIZE];
    int count;
    pile_type_t type;
    int pile_index;     // For foundation/tableau: which pile (0-3 or 0-6)
} card_pile_t;

// Game states
typedef enum {
    SOLITAIRE_STATE_PLAYING,
    SOLITAIRE_STATE_WON,
    SOLITAIRE_STATE_MENU
} solitaire_state_t;

// Selection state
typedef struct {
    bool has_selection;
    pile_type_t source_type;
    int source_pile;    // Which pile (0 for stock/waste, 0-3 for foundation, 0-6 for tableau)
    int card_index;     // Index in the pile (for tableau, can select multiple cards)
    int card_count;     // Number of cards selected (for moving runs)
} selection_t;

// Solitaire game state
typedef struct {
    window_t *window;

    // Game state
    solitaire_state_t state;

    // Card piles
    card_pile_t stock;              // Draw pile (face down)
    card_pile_t waste;              // Discard pile from stock
    card_pile_t foundations[FOUNDATION_PILES];  // 4 foundation piles (one per suit)
    card_pile_t tableau[TABLEAU_PILES];         // 7 tableau piles

    // Selection
    selection_t selection;

    // Dragging state
    bool dragging;
    int drag_x, drag_y;             // Current drag position
    int drag_offset_x, drag_offset_y; // Offset from card corner
    card_t drag_cards[13];          // Cards being dragged (max king down to ace)
    int drag_card_count;
    pile_type_t drag_source_type;
    int drag_source_pile;
    int drag_source_index;

    // Game area (content bounds)
    int32_t area_x, area_y;
    int32_t area_w, area_h;

    // Statistics
    int moves;
    uint64_t start_time;

    // Random seed
    uint32_t rand_seed;

} solitaire_t;

// Create solitaire game
solitaire_t *solitaire_create(void);

// Destroy solitaire game
void solitaire_destroy(solitaire_t *game);

// Start new game
void solitaire_new_game(solitaire_t *game);

// Event handling
void solitaire_handle_event(solitaire_t *game, gui_event_t *event);

// Drawing
void solitaire_draw(solitaire_t *game);

// Check for win condition
bool solitaire_check_win(solitaire_t *game);

// Auto-complete (move cards to foundation when safe)
void solitaire_auto_complete(solitaire_t *game);

// Launch solitaire game
extern void solitaire_launch(void);

#endif // SOLITAIRE_H
