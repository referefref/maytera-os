/* chess.h - Maytera Chess: complete rules engine (portable, libc-only).
 * Board is an 8x8 mailbox: sq = rank*8 + file, a1 = 0, h8 = 63.
 * Pieces are signed: +type = white, -type = black, 0 = empty. */
#ifndef MAYTERA_CHESS_H
#define MAYTERA_CHESS_H

enum { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };
enum { WHITE = 0, BLACK = 1 };

/* castling-rights bits */
#define CR_WK 1
#define CR_WQ 2
#define CR_BK 4
#define CR_BQ 8

/* move flags */
#define MF_CAPTURE   1
#define MF_EP        2   /* en-passant capture */
#define MF_CASTLE_K  4
#define MF_CASTLE_Q  8
#define MF_DBLPUSH   16
#define MF_PROMO     32

typedef struct {
    signed char from, to;
    signed char promo;   /* promotion piece type (QUEEN/ROOK/BISHOP/KNIGHT) or 0 */
    signed char flags;
} Move;

typedef struct {
    signed char board[64];
    signed char side;        /* WHITE / BLACK to move */
    signed char castle;      /* CR_* bits */
    signed char ep;          /* en-passant target square, or -1 */
    int halfmove;            /* 50-move rule counter (plies since pawn/capture) */
    int fullmove;
} Position;

/* result codes */
enum { RES_NONE = 0, RES_CHECKMATE, RES_STALEMATE, RES_DRAW_50,
       RES_DRAW_MATERIAL, RES_DRAW_REPETITION };

#define MAX_MOVES 256
#define MAX_HIST  1024

typedef struct {
    Position pos;
    /* undo + repetition history */
    Position hist[MAX_HIST];
    Move     histmove[MAX_HIST];
    int      histn;
    /* redo stack (for undo/redo UI) */
    Position redo[MAX_HIST];
    Move     redomove[MAX_HIST];
    int      redon;
} Game;

/* ---- engine API ---- */
void chess_new(Game *g);
int  chess_gen_legal(const Position *p, Move *out);   /* returns count */
int  chess_in_check(const Position *p, int color);
int  chess_is_attacked(const Position *p, int sq, int byColor);
void chess_make(Position *p, Move m);                 /* apply (no history) */
int  chess_result(const Position *p);                 /* RES_* for side-to-move */

/* game-level (with history for undo/redo + threefold) */
int  chess_play(Game *g, Move m);   /* validates legal, pushes history; 1 ok 0 illegal */
int  chess_undo(Game *g);
int  chess_redo(Game *g);
int  chess_game_result(const Game *g);   /* includes threefold repetition */

/* helpers */
int  chess_find_move(const Position *p, int from, int to, Move *out); /* any legal from->to (promo defaults Q) */
void chess_san(const Position *p, Move m, char *buf);  /* algebraic notation into buf (>=8) */
unsigned long chess_hash(const Position *p);

#endif
