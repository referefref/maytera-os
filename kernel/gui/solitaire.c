// solitaire.c - Klondike Solitaire for MayteraOS
// Now using vector-rendered playing cards
#include "solitaire.h"
#include "cards.h"   // Vector playing cards
#include "window.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "syslog.h"

// External timer
extern volatile uint64_t timer_ticks;

// Colors
#define COLOR_TABLE_GREEN   0x006B8E23  // Olive green felt
#define COLOR_EMPTY_PILE    0x00507030  // Empty pile outline
#define COLOR_BUTTON_BG     0x00505050  // Button background
#define COLOR_BUTTON_TEXT   0x00FFFFFF  // Button text

// Layout constants
#define MARGIN_TOP      10
#define MARGIN_LEFT     10
#define PILE_SPACING    (CARD_WIDTH + 10)
#define TABLEAU_TOP     (MARGIN_TOP + CARD_HEIGHT + 20)

// Button dimensions
#define BUTTON_WIDTH    80
#define BUTTON_HEIGHT   24
#define BUTTON_MARGIN   10

// Simple random number generator (LCG)
static int solitaire_rand(solitaire_t *game) {
    game->rand_seed = game->rand_seed * 1103515245 + 12345;
    return (game->rand_seed >> 16) & 0x7FFF;
}

// Check if suit is red (hearts or diamonds)
static bool is_red_suit(uint8_t suit) {
    return suit == SUIT_HEARTS || suit == SUIT_DIAMONDS;
}

// Check if two suits are opposite colors
static bool is_opposite_color(uint8_t suit1, uint8_t suit2) {
    return is_red_suit(suit1) != is_red_suit(suit2);
}

// Initialize a deck of 52 cards
static void init_deck(card_t *deck) {
    int i = 0;
    for (int suit = 0; suit < 4; suit++) {
        for (int value = 1; value <= 13; value++) {
            deck[i].suit = suit;
            deck[i].value = value;
            deck[i].face_up = false;
            i++;
        }
    }
}

// Shuffle the deck using Fisher-Yates algorithm
static void shuffle_deck(solitaire_t *game, card_t *deck, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = solitaire_rand(game) % (i + 1);
        // Swap cards[i] and cards[j]
        card_t temp = deck[i];
        deck[i] = deck[j];
        deck[j] = temp;
    }
}

// Clear a pile
static void clear_pile(card_pile_t *pile) {
    pile->count = 0;
}

// Add a card to a pile
static bool pile_push(card_pile_t *pile, card_t card) {
    if (pile->count >= MAX_PILE_SIZE) return false;
    pile->cards[pile->count++] = card;
    return true;
}

// Remove top card from a pile
static bool pile_pop(card_pile_t *pile, card_t *card) {
    if (pile->count == 0) return false;
    *card = pile->cards[--pile->count];
    return true;
}

// Get top card without removing
static card_t *pile_top(card_pile_t *pile) {
    if (pile->count == 0) return NULL;
    return &pile->cards[pile->count - 1];
}

// Create solitaire game
solitaire_t *solitaire_create(void) {
    solitaire_t *game = (solitaire_t *)kmalloc(sizeof(solitaire_t));
    if (!game) return NULL;

    memset(game, 0, sizeof(solitaire_t));

    // Create window
    game->window = window_create("Solitaire", 50, 30, 600, 480);
    if (!game->window) {
        kfree(game);
        return NULL;
    }

    // Initialize random seed from timer
    game->rand_seed = timer_ticks;

    // Setup pile types
    game->stock.type = PILE_STOCK;
    game->waste.type = PILE_WASTE;
    for (int i = 0; i < FOUNDATION_PILES; i++) {
        game->foundations[i].type = PILE_FOUNDATION;
        game->foundations[i].pile_index = i;
    }
    for (int i = 0; i < TABLEAU_PILES; i++) {
        game->tableau[i].type = PILE_TABLEAU;
        game->tableau[i].pile_index = i;
    }

    // Start new game
    solitaire_new_game(game);

    return game;
}

// Destroy solitaire game
void solitaire_destroy(solitaire_t *game) {
    if (!game) return;
    if (game->window) {
        window_destroy(game->window);
    }
    kfree(game);
}

// Start new game
void solitaire_new_game(solitaire_t *game) {
    if (!game) return;

    // Clear all piles
    clear_pile(&game->stock);
    clear_pile(&game->waste);
    for (int i = 0; i < FOUNDATION_PILES; i++) {
        clear_pile(&game->foundations[i]);
    }
    for (int i = 0; i < TABLEAU_PILES; i++) {
        clear_pile(&game->tableau[i]);
    }

    // Clear selection
    game->selection.has_selection = false;
    game->dragging = false;

    // Initialize and shuffle deck
    card_t deck[52];
    init_deck(deck);

    // Re-seed random with current timer for variety
    game->rand_seed ^= timer_ticks;
    shuffle_deck(game, deck, 52);

    // Deal to tableau piles
    // Pile 0: 1 card, Pile 1: 2 cards, ... Pile 6: 7 cards
    int card_index = 0;
    for (int pile = 0; pile < TABLEAU_PILES; pile++) {
        for (int row = 0; row <= pile; row++) {
            card_t card = deck[card_index++];
            // Only the top card is face up
            card.face_up = (row == pile);
            pile_push(&game->tableau[pile], card);
        }
    }

    // Remaining cards go to stock (24 cards)
    while (card_index < 52) {
        pile_push(&game->stock, deck[card_index++]);
    }

    // Reset stats
    game->moves = 0;
    game->start_time = timer_ticks;
    game->state = SOLITAIRE_STATE_PLAYING;
}

// Check for win condition (all cards in foundations)
bool solitaire_check_win(solitaire_t *game) {
    int total = 0;
    for (int i = 0; i < FOUNDATION_PILES; i++) {
        total += game->foundations[i].count;
    }
    return total == 52;
}

// Check if a card can be placed on a foundation
static bool can_place_on_foundation(card_pile_t *foundation, card_t *card) {
    if (foundation->count == 0) {
        // Empty foundation accepts only Aces
        return card->value == CARD_ACE;
    }
    card_t *top = pile_top(foundation);
    // Must be same suit and one value higher
    return (card->suit == top->suit) && (card->value == top->value + 1);
}

// Check if a card can be placed on a tableau pile
static bool can_place_on_tableau(card_pile_t *tableau, card_t *card) {
    if (tableau->count == 0) {
        // Empty tableau accepts only Kings
        return card->value == CARD_KING;
    }
    card_t *top = pile_top(tableau);
    // Must be opposite color and one value lower
    return is_opposite_color(card->suit, top->suit) && (card->value == top->value - 1);
}

// Get the minimum value safely movable to foundations
// (A card is safe to auto-move if it won't block any needed plays)
static int get_min_foundation_value(solitaire_t *game) {
    int min_red = 14;
    int min_black = 14;
    for (int i = 0; i < FOUNDATION_PILES; i++) {
        int val = game->foundations[i].count; // count equals highest value
        if (is_red_suit(i)) {
            if (val < min_red) min_red = val;
        } else {
            if (val < min_black) min_black = val;
        }
    }
    // Safe to auto-move if both opposite colors have at least value-1
    return (min_red < min_black ? min_red : min_black) + 1;
}

// Auto-complete: move safe cards to foundation
void solitaire_auto_complete(solitaire_t *game) {
    bool moved;
    do {
        moved = false;
        int safe_value = get_min_foundation_value(game) + 1;

        // Check waste pile
        card_t *waste_top = pile_top(&game->waste);
        if (waste_top && waste_top->value <= safe_value) {
            for (int f = 0; f < FOUNDATION_PILES; f++) {
                if (can_place_on_foundation(&game->foundations[f], waste_top)) {
                    card_t card;
                    pile_pop(&game->waste, &card);
                    pile_push(&game->foundations[f], card);
                    game->moves++;
                    moved = true;
                    break;
                }
            }
        }

        // Check tableau piles
        for (int t = 0; t < TABLEAU_PILES && !moved; t++) {
            card_t *tab_top = pile_top(&game->tableau[t]);
            if (tab_top && tab_top->face_up && tab_top->value <= safe_value) {
                for (int f = 0; f < FOUNDATION_PILES; f++) {
                    if (can_place_on_foundation(&game->foundations[f], tab_top)) {
                        card_t card;
                        pile_pop(&game->tableau[t], &card);
                        pile_push(&game->foundations[f], card);
                        // Flip new top card if needed
                        card_t *new_top = pile_top(&game->tableau[t]);
                        if (new_top && !new_top->face_up) {
                            new_top->face_up = true;
                        }
                        game->moves++;
                        moved = true;
                        break;
                    }
                }
            }
        }
    } while (moved);

    // Check for win
    if (solitaire_check_win(game)) {
        game->state = SOLITAIRE_STATE_WON;
    }
}

// Draw a single character at position (for UI text)
static void draw_char(int x, int y, char c, uint32_t color) {
    const uint8_t *glyph = font_get_glyph(c);
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
static void draw_text(int x, int y, const char *text, uint32_t color) {
    for (int i = 0; text[i]; i++) {
        draw_char(x + i * 8, y, text[i], color);
    }
}

// Draw centered text
static void draw_text_centered(int x, int y, int width, const char *text, uint32_t color) {
    int text_width = strlen(text) * 8;
    draw_text(x + (width - text_width) / 2, y, text, color);
}

// Draw a button
static void draw_button(int x, int y, int w, int h, const char *text, bool hover) {
    uint32_t bg = hover ? 0x00707070 : COLOR_BUTTON_BG;
    fb_fill_rect(x, y, w, h, bg);
    fb_draw_rect(x, y, w, h, 0x00808080);
    draw_text_centered(x, y + (h - 16) / 2, w, text, COLOR_BUTTON_TEXT);
}

// Get pile position on screen
static void get_pile_position(solitaire_t *game, pile_type_t type, int index, int *x, int *y) {
    *x = game->area_x + MARGIN_LEFT;
    *y = game->area_y + MARGIN_TOP;

    switch (type) {
        case PILE_STOCK:
            // Top left
            break;
        case PILE_WASTE:
            *x += PILE_SPACING;
            break;
        case PILE_FOUNDATION:
            *x += PILE_SPACING * (3 + index);
            break;
        case PILE_TABLEAU:
            *x += PILE_SPACING * index;
            *y = game->area_y + TABLEAU_TOP;
            break;
    }
}

// Draw the game using vector cards
void solitaire_draw(solitaire_t *game) {
    if (!game || !game->window) return;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(game->window, &wx, &wy, &ww, &wh);

    game->area_x = wx;
    game->area_y = wy;
    game->area_w = ww;
    game->area_h = wh;

    // Clear background with green felt color
    fb_fill_rect(wx, wy, ww, wh, COLOR_TABLE_GREEN);

    // Draw "New Game" button
    int button_x = wx + ww - BUTTON_WIDTH - BUTTON_MARGIN;
    int button_y = wy + BUTTON_MARGIN;
    draw_button(button_x, button_y, BUTTON_WIDTH, BUTTON_HEIGHT, "New Game", false);

    // Draw stock pile
    int pile_x, pile_y;
    get_pile_position(game, PILE_STOCK, 0, &pile_x, &pile_y);
    if (game->stock.count > 0) {
        // Use vector card back
        card_draw_back(pile_x, pile_y);
        // Show count
        char count_str[4];
        itoa(game->stock.count, count_str, 10);
        draw_text(pile_x + CARD_WIDTH / 2 - 4, pile_y + CARD_HEIGHT + 2, count_str, 0x00FFFFFF);
    } else {
        card_draw_empty(pile_x, pile_y, CARD_WIDTH, CARD_HEIGHT, COLOR_EMPTY_PILE);
        // Draw "refresh" indicator
        draw_text(pile_x + CARD_WIDTH / 2 - 8, pile_y + CARD_HEIGHT / 2 - 8, "<<", 0x00AAAAAA);
    }

    // Draw waste pile
    get_pile_position(game, PILE_WASTE, 0, &pile_x, &pile_y);
    if (game->waste.count > 0) {
        card_t *top = pile_top(&game->waste);
        bool selected = game->selection.has_selection &&
                        game->selection.source_type == PILE_WASTE;
        // Use vector card face
        card_draw_face(pile_x, pile_y, top->value, top->suit, selected);
    } else {
        card_draw_empty(pile_x, pile_y, CARD_WIDTH, CARD_HEIGHT, COLOR_EMPTY_PILE);
    }

    // Draw foundation piles
    for (int i = 0; i < FOUNDATION_PILES; i++) {
        get_pile_position(game, PILE_FOUNDATION, i, &pile_x, &pile_y);
        if (game->foundations[i].count > 0) {
            card_t *top = pile_top(&game->foundations[i]);
            // Use vector card face
            card_draw_face(pile_x, pile_y, top->value, top->suit, false);
        } else {
            card_draw_empty(pile_x, pile_y, CARD_WIDTH, CARD_HEIGHT, COLOR_EMPTY_PILE);
            // Draw suit hint using vector suit
            uint32_t hint_color = card_is_red_suit(i) ? 0x00803030 : 0x00305030;
            card_draw_suit(i, pile_x + CARD_WIDTH / 2, pile_y + CARD_HEIGHT / 2, 16, hint_color);
        }
    }

    // Draw tableau piles
    for (int i = 0; i < TABLEAU_PILES; i++) {
        get_pile_position(game, PILE_TABLEAU, i, &pile_x, &pile_y);

        if (game->tableau[i].count == 0) {
            card_draw_empty(pile_x, pile_y, CARD_WIDTH, CARD_HEIGHT, COLOR_EMPTY_PILE);
            // King indicator using vector spade
            card_draw_suit(CARD_SUIT_SPADES, pile_x + CARD_WIDTH / 2, pile_y + CARD_HEIGHT / 2, 12, 0x00507030);
        } else {
            int card_y = pile_y;
            for (int j = 0; j < game->tableau[i].count; j++) {
                card_t *card = &game->tableau[i].cards[j];
                bool selected = game->selection.has_selection &&
                                game->selection.source_type == PILE_TABLEAU &&
                                game->selection.source_pile == i &&
                                j >= game->selection.card_index;

                // Don't draw if being dragged
                if (game->dragging &&
                    game->drag_source_type == PILE_TABLEAU &&
                    game->drag_source_pile == i &&
                    j >= game->drag_source_index) {
                    continue;
                }

                if (card->face_up) {
                    // Use vector card face
                    card_draw_face(pile_x, card_y, card->value, card->suit, selected);
                } else {
                    // Use vector card back
                    card_draw_back(pile_x, card_y);
                }

                // Offset for next card
                if (card->face_up) {
                    card_y += CARD_STACK_OFFSET;
                } else {
                    card_y += 3;  // Smaller offset for face-down cards
                }
            }
        }
    }

    // Draw dragged cards using vector cards
    if (game->dragging && game->drag_card_count > 0) {
        int drag_y = game->drag_y;
        for (int i = 0; i < game->drag_card_count; i++) {
            card_draw_face(game->drag_x, drag_y,
                          game->drag_cards[i].value,
                          game->drag_cards[i].suit, true);
            drag_y += CARD_STACK_OFFSET;
        }
    }

    // Draw moves counter
    char moves_str[32] = "Moves: ";
    char num_str[12];
    itoa(game->moves, num_str, 10);
    strcat(moves_str, num_str);
    draw_text(wx + BUTTON_MARGIN, wy + wh - 20, moves_str, 0x00FFFFFF);

    // Draw win screen
    if (game->state == SOLITAIRE_STATE_WON) {
        // Semi-transparent overlay (just draw a darker rectangle)
        fb_fill_rect(wx + ww / 4, wy + wh / 3, ww / 2, wh / 3, 0x00203020);
        fb_draw_rect(wx + ww / 4, wy + wh / 3, ww / 2, wh / 3, CARD_COLOR_HIGHLIGHT);

        draw_text_centered(wx + ww / 4, wy + wh / 3 + 30, ww / 2, "YOU WIN!", CARD_COLOR_HIGHLIGHT);

        char win_msg[48] = "Completed in ";
        itoa(game->moves, num_str, 10);
        strcat(win_msg, num_str);
        strcat(win_msg, " moves");
        draw_text_centered(wx + ww / 4, wy + wh / 3 + 60, ww / 2, win_msg, 0x00FFFFFF);

        draw_text_centered(wx + ww / 4, wy + wh / 3 + 100, ww / 2, "Click 'New Game' to play again", 0x00AAAAAA);
    }
}

// Check if point is inside a rectangle
static bool point_in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

// Handle click on stock pile
static void handle_stock_click(solitaire_t *game) {
    if (game->stock.count > 0) {
        // Draw one card to waste
        card_t card;
        pile_pop(&game->stock, &card);
        card.face_up = true;
        pile_push(&game->waste, card);
        game->moves++;
    } else if (game->waste.count > 0) {
        // Reset: move all waste back to stock
        while (game->waste.count > 0) {
            card_t card;
            pile_pop(&game->waste, &card);
            card.face_up = false;
            pile_push(&game->stock, card);
        }
        game->moves++;
    }
    game->selection.has_selection = false;
}

// Try to move selected cards to a destination
static bool try_move_to(solitaire_t *game, pile_type_t dest_type, int dest_pile) {
    if (!game->selection.has_selection) return false;

    // Get source card
    card_t *source_card = NULL;
    card_pile_t *source_pile_ptr = NULL;

    if (game->selection.source_type == PILE_WASTE) {
        source_pile_ptr = &game->waste;
        source_card = pile_top(source_pile_ptr);
    } else if (game->selection.source_type == PILE_TABLEAU) {
        source_pile_ptr = &game->tableau[game->selection.source_pile];
        if (game->selection.card_index < source_pile_ptr->count) {
            source_card = &source_pile_ptr->cards[game->selection.card_index];
        }
    } else if (game->selection.source_type == PILE_FOUNDATION) {
        source_pile_ptr = &game->foundations[game->selection.source_pile];
        source_card = pile_top(source_pile_ptr);
    }

    if (!source_card) return false;

    // Try to place on destination
    if (dest_type == PILE_FOUNDATION) {
        // Can only move single cards to foundation
        if (game->selection.card_count != 1) return false;

        if (can_place_on_foundation(&game->foundations[dest_pile], source_card)) {
            card_t card = {0, 0, false};
            pile_pop(source_pile_ptr, &card);
            pile_push(&game->foundations[dest_pile], card);

            // Flip new top card in tableau if needed
            if (game->selection.source_type == PILE_TABLEAU) {
                card_t *new_top = pile_top(source_pile_ptr);
                if (new_top && !new_top->face_up) {
                    new_top->face_up = true;
                }
            }

            game->moves++;
            game->selection.has_selection = false;
            return true;
        }
    } else if (dest_type == PILE_TABLEAU) {
        if (can_place_on_tableau(&game->tableau[dest_pile], source_card)) {
            // Move all selected cards
            int num_cards = game->selection.card_count;
            card_t cards_to_move[13];

            // Pop cards from source (in reverse order)
            for (int i = num_cards - 1; i >= 0; i--) {
                pile_pop(source_pile_ptr, &cards_to_move[i]);
            }

            // Push to destination
            for (int i = 0; i < num_cards; i++) {
                pile_push(&game->tableau[dest_pile], cards_to_move[i]);
            }

            // Flip new top card in source tableau if needed
            if (game->selection.source_type == PILE_TABLEAU) {
                card_t *new_top = pile_top(source_pile_ptr);
                if (new_top && !new_top->face_up) {
                    new_top->face_up = true;
                }
            }

            game->moves++;
            game->selection.has_selection = false;
            return true;
        }
    }

    return false;
}

// Find which pile was clicked
static bool find_clicked_pile(solitaire_t *game, int mx, int my,
                               pile_type_t *type, int *pile_index, int *card_index) {
    int pile_x, pile_y;

    // Check stock
    get_pile_position(game, PILE_STOCK, 0, &pile_x, &pile_y);
    if (point_in_rect(mx, my, pile_x, pile_y, CARD_WIDTH, CARD_HEIGHT)) {
        *type = PILE_STOCK;
        *pile_index = 0;
        *card_index = game->stock.count - 1;
        return true;
    }

    // Check waste
    get_pile_position(game, PILE_WASTE, 0, &pile_x, &pile_y);
    if (point_in_rect(mx, my, pile_x, pile_y, CARD_WIDTH, CARD_HEIGHT)) {
        *type = PILE_WASTE;
        *pile_index = 0;
        *card_index = game->waste.count - 1;
        return true;
    }

    // Check foundations
    for (int i = 0; i < FOUNDATION_PILES; i++) {
        get_pile_position(game, PILE_FOUNDATION, i, &pile_x, &pile_y);
        if (point_in_rect(mx, my, pile_x, pile_y, CARD_WIDTH, CARD_HEIGHT)) {
            *type = PILE_FOUNDATION;
            *pile_index = i;
            *card_index = game->foundations[i].count - 1;
            return true;
        }
    }

    // Check tableau piles
    for (int i = 0; i < TABLEAU_PILES; i++) {
        get_pile_position(game, PILE_TABLEAU, i, &pile_x, &pile_y);

        if (game->tableau[i].count == 0) {
            if (point_in_rect(mx, my, pile_x, pile_y, CARD_WIDTH, CARD_HEIGHT)) {
                *type = PILE_TABLEAU;
                *pile_index = i;
                *card_index = -1;  // Empty pile
                return true;
            }
        } else {
            // Calculate pile height
            int pile_height = CARD_HEIGHT;
            int card_y = pile_y;
            for (int j = 0; j < game->tableau[i].count - 1; j++) {
                if (game->tableau[i].cards[j].face_up) {
                    card_y += CARD_STACK_OFFSET;
                } else {
                    card_y += 3;
                }
            }
            pile_height = (card_y - pile_y) + CARD_HEIGHT;

            if (point_in_rect(mx, my, pile_x, pile_y, CARD_WIDTH, pile_height)) {
                // Find which card was clicked
                card_y = pile_y;
                for (int j = 0; j < game->tableau[i].count; j++) {
                    int offset = game->tableau[i].cards[j].face_up ? CARD_STACK_OFFSET : 3;
                    int next_y = (j == game->tableau[i].count - 1) ? card_y + CARD_HEIGHT : card_y + offset;

                    if (my >= card_y && my < next_y) {
                        *type = PILE_TABLEAU;
                        *pile_index = i;
                        *card_index = j;
                        return true;
                    }
                    card_y += offset;
                }
                // Default to top card
                *type = PILE_TABLEAU;
                *pile_index = i;
                *card_index = game->tableau[i].count - 1;
                return true;
            }
        }
    }

    return false;
}

// Handle mouse down
static void handle_mouse_down(solitaire_t *game, int mx, int my) {
    // Check "New Game" button
    int button_x = game->area_x + game->area_w - BUTTON_WIDTH - BUTTON_MARGIN;
    int button_y = game->area_y + BUTTON_MARGIN;
    if (point_in_rect(mx, my, button_x, button_y, BUTTON_WIDTH, BUTTON_HEIGHT)) {
        solitaire_new_game(game);
        return;
    }

    if (game->state != SOLITAIRE_STATE_PLAYING) return;

    pile_type_t clicked_type;
    int clicked_pile, clicked_card;

    if (!find_clicked_pile(game, mx, my, &clicked_type, &clicked_pile, &clicked_card)) {
        game->selection.has_selection = false;
        return;
    }

    // Handle stock click
    if (clicked_type == PILE_STOCK) {
        handle_stock_click(game);
        return;
    }

    // If we have a selection and clicked elsewhere, try to move
    if (game->selection.has_selection) {
        // Clicked on a different pile - try to move
        if (clicked_type != game->selection.source_type ||
            clicked_pile != game->selection.source_pile) {
            if (try_move_to(game, clicked_type, clicked_pile)) {
                solitaire_auto_complete(game);
                if (solitaire_check_win(game)) {
                    game->state = SOLITAIRE_STATE_WON;
                }
                return;
            }
        }
        // Clear selection
        game->selection.has_selection = false;
    }

    // New selection
    if (clicked_type == PILE_WASTE && game->waste.count > 0) {
        game->selection.has_selection = true;
        game->selection.source_type = PILE_WASTE;
        game->selection.source_pile = 0;
        game->selection.card_index = game->waste.count - 1;
        game->selection.card_count = 1;

        // Start dragging
        game->dragging = true;
        game->drag_cards[0] = *pile_top(&game->waste);
        game->drag_card_count = 1;
        game->drag_source_type = PILE_WASTE;
        game->drag_source_pile = 0;
        game->drag_source_index = game->waste.count - 1;

        int pile_x, pile_y;
        get_pile_position(game, PILE_WASTE, 0, &pile_x, &pile_y);
        game->drag_offset_x = mx - pile_x;
        game->drag_offset_y = my - pile_y;
        game->drag_x = pile_x;
        game->drag_y = pile_y;
    }
    else if (clicked_type == PILE_FOUNDATION && game->foundations[clicked_pile].count > 0) {
        // Can select top card of foundation (to move back to tableau)
        game->selection.has_selection = true;
        game->selection.source_type = PILE_FOUNDATION;
        game->selection.source_pile = clicked_pile;
        game->selection.card_index = game->foundations[clicked_pile].count - 1;
        game->selection.card_count = 1;
    }
    else if (clicked_type == PILE_TABLEAU) {
        if (clicked_card >= 0 && game->tableau[clicked_pile].cards[clicked_card].face_up) {
            // Select this card and all cards on top of it
            game->selection.has_selection = true;
            game->selection.source_type = PILE_TABLEAU;
            game->selection.source_pile = clicked_pile;
            game->selection.card_index = clicked_card;
            game->selection.card_count = game->tableau[clicked_pile].count - clicked_card;

            // Start dragging
            game->dragging = true;
            game->drag_card_count = game->selection.card_count;
            game->drag_source_type = PILE_TABLEAU;
            game->drag_source_pile = clicked_pile;
            game->drag_source_index = clicked_card;

            for (int i = 0; i < game->drag_card_count; i++) {
                game->drag_cards[i] = game->tableau[clicked_pile].cards[clicked_card + i];
            }

            // Calculate card position for offset
            int pile_x, pile_y;
            get_pile_position(game, PILE_TABLEAU, clicked_pile, &pile_x, &pile_y);
            int card_y = pile_y;
            for (int j = 0; j < clicked_card; j++) {
                if (game->tableau[clicked_pile].cards[j].face_up) {
                    card_y += CARD_STACK_OFFSET;
                } else {
                    card_y += 3;
                }
            }
            game->drag_offset_x = mx - pile_x;
            game->drag_offset_y = my - card_y;
            game->drag_x = pile_x;
            game->drag_y = card_y;
        }
    }
}

// Handle mouse move (for dragging)
static void handle_mouse_move(solitaire_t *game, int mx, int my) {
    if (game->dragging) {
        game->drag_x = mx - game->drag_offset_x;
        game->drag_y = my - game->drag_offset_y;
    }
}

// Handle mouse up
static void handle_mouse_up(solitaire_t *game, int mx, int my) {
    if (!game->dragging) return;

    // Find where we dropped
    pile_type_t drop_type;
    int drop_pile, drop_card;

    if (find_clicked_pile(game, mx, my, &drop_type, &drop_pile, &drop_card)) {
        // Try to move to this location
        if (try_move_to(game, drop_type, drop_pile)) {
            solitaire_auto_complete(game);
            if (solitaire_check_win(game)) {
                game->state = SOLITAIRE_STATE_WON;
            }
        }
    }

    game->dragging = false;
    game->selection.has_selection = false;
}

// Handle double click (auto-move to foundation)
static void handle_double_click(solitaire_t *game, int mx, int my) {
    pile_type_t clicked_type;
    int clicked_pile, clicked_card;

    if (!find_clicked_pile(game, mx, my, &clicked_type, &clicked_pile, &clicked_card)) {
        return;
    }

    card_t *card = NULL;
    card_pile_t *source = NULL;

    if (clicked_type == PILE_WASTE && game->waste.count > 0) {
        source = &game->waste;
        card = pile_top(source);
    } else if (clicked_type == PILE_TABLEAU && clicked_card >= 0) {
        source = &game->tableau[clicked_pile];
        // Only if clicking top card
        if (clicked_card == source->count - 1 && source->cards[clicked_card].face_up) {
            card = &source->cards[clicked_card];
        }
    }

    if (!card || !source) return;

    // Try to move to any foundation
    for (int f = 0; f < FOUNDATION_PILES; f++) {
        if (can_place_on_foundation(&game->foundations[f], card)) {
            card_t moved_card = {0, 0, false};
            pile_pop(source, &moved_card);
            pile_push(&game->foundations[f], moved_card);

            // Flip new top in tableau
            if (clicked_type == PILE_TABLEAU) {
                card_t *new_top = pile_top(source);
                if (new_top && !new_top->face_up) {
                    new_top->face_up = true;
                }
            }

            game->moves++;
            solitaire_auto_complete(game);
            if (solitaire_check_win(game)) {
                game->state = SOLITAIRE_STATE_WON;
            }
            return;
        }
    }
}

// Track for double-click detection
static uint64_t last_click_time = 0;
static int last_click_x = 0;
static int last_click_y = 0;
#define DOUBLE_CLICK_TIME 30  // Timer ticks (~300ms at 100Hz)
#define DOUBLE_CLICK_DISTANCE 10

// Event handling
void solitaire_handle_event(solitaire_t *game, gui_event_t *event) {
    if (!game || !event) return;

    // Convert screen coords to window coords
    int mx = event->mouse_x;
    int my = event->mouse_y;

    if (event->type == EVENT_MOUSE_DOWN) {
        // Check for double click
        uint64_t now = timer_ticks;
        int dx = mx - last_click_x;
        int dy = my - last_click_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;

        if ((now - last_click_time) < DOUBLE_CLICK_TIME &&
            dx < DOUBLE_CLICK_DISTANCE && dy < DOUBLE_CLICK_DISTANCE) {
            handle_double_click(game, mx, my);
            last_click_time = 0;  // Reset to prevent triple-click
        } else {
            handle_mouse_down(game, mx, my);
            last_click_time = now;
            last_click_x = mx;
            last_click_y = my;
        }
    }
    else if (event->type == EVENT_MOUSE_MOVE) {
        handle_mouse_move(game, mx, my);
    }
    else if (event->type == EVENT_MOUSE_UP) {
        handle_mouse_up(game, mx, my);
    }
    else if (event->type == EVENT_KEY_DOWN) {
        switch (event->keycode) {
            case 'n': case 'N':
                solitaire_new_game(game);
                break;
            case 'a': case 'A':
                // Force auto-complete
                if (game->state == SOLITAIRE_STATE_PLAYING) {
                    solitaire_auto_complete(game);
                    if (solitaire_check_win(game)) {
                        game->state = SOLITAIRE_STATE_WON;
                    }
                }
                break;
            case 0x1B:  // Escape
                game->selection.has_selection = false;
                game->dragging = false;
                break;
        }
    }
}

// Launch solitaire
void solitaire_launch(void) {
    LOG_INFO("[Solitaire] Game launched with vector cards");
    solitaire_t *game = solitaire_create();
    if (!game) {
        LOG_ERROR("[Solitaire] Failed to create game");
        kprintf("[Solitaire] Failed to create game\n");
        return;
    }

    // Register with window manager
    wm_register_app(game->window, game,
                    (app_event_handler_t)solitaire_handle_event,
                    (app_draw_handler_t)solitaire_draw,
                    (app_destroy_handler_t)solitaire_destroy);

    kprintf("[Solitaire] Game launched with vector cards\n");
}
