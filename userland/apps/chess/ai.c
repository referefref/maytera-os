/* ai.c - Maytera Chess computer opponent: alpha-beta (negamax) with a
 * material + piece-square evaluation, move ordering (MVV-LVA + promotions),
 * quiescence on captures, and iterative deepening to a per-difficulty depth.
 * Pure function of the position; the caller runs it on a worker thread so the
 * UI stays responsive. */
#include "chess.h"
#include <string.h>

#define TYPE(pc) ((pc) < 0 ? -(pc) : (pc))
#define COLOR(pc) ((pc) < 0 ? BLACK : WHITE)
#define FILE_OF(sq) ((sq) & 7)
#define RANK_OF(sq) ((sq) >> 3)

static const int val[7] = {0, 100, 320, 330, 500, 900, 20000};

/* piece-square tables (from white's view, a1=0). Midgame-ish. */
static const int pst_pawn[64] = {
   0,  0,  0,  0,  0,  0,  0,  0,
   5, 10, 10,-20,-20, 10, 10,  5,
   5, -5,-10,  0,  0,-10, -5,  5,
   0,  0,  0, 20, 20,  0,  0,  0,
   5,  5, 10, 25, 25, 10,  5,  5,
  10, 10, 20, 30, 30, 20, 10, 10,
  50, 50, 50, 50, 50, 50, 50, 50,
   0,  0,  0,  0,  0,  0,  0,  0 };
static const int pst_knight[64] = {
 -50,-40,-30,-30,-30,-30,-40,-50,
 -40,-20,  0,  5,  5,  0,-20,-40,
 -30,  5, 10, 15, 15, 10,  5,-30,
 -30,  0, 15, 20, 20, 15,  0,-30,
 -30,  5, 15, 20, 20, 15,  5,-30,
 -30,  0, 10, 15, 15, 10,  0,-30,
 -40,-20,  0,  0,  0,  0,-20,-40,
 -50,-40,-30,-30,-30,-30,-40,-50 };
static const int pst_bishop[64] = {
 -20,-10,-10,-10,-10,-10,-10,-20,
 -10,  5,  0,  0,  0,  0,  5,-10,
 -10, 10, 10, 10, 10, 10, 10,-10,
 -10,  0, 10, 10, 10, 10,  0,-10,
 -10,  5,  5, 10, 10,  5,  5,-10,
 -10,  0,  5, 10, 10,  5,  0,-10,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -20,-10,-10,-10,-10,-10,-10,-20 };
static const int pst_rook[64] = {
   0,  0,  0,  5,  5,  0,  0,  0,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
  -5,  0,  0,  0,  0,  0,  0, -5,
   5, 10, 10, 10, 10, 10, 10,  5,
   0,  0,  0,  0,  0,  0,  0,  0 };
static const int pst_queen[64] = {
 -20,-10,-10, -5, -5,-10,-10,-20,
 -10,  0,  5,  0,  0,  0,  0,-10,
 -10,  5,  5,  5,  5,  5,  0,-10,
   0,  0,  5,  5,  5,  5,  0, -5,
  -5,  0,  5,  5,  5,  5,  0, -5,
 -10,  0,  5,  5,  5,  5,  0,-10,
 -10,  0,  0,  0,  0,  0,  0,-10,
 -20,-10,-10, -5, -5,-10,-10,-20 };
static const int pst_king[64] = {
  20, 30, 10,  0,  0, 10, 30, 20,
  20, 20,  0,  0,  0,  0, 20, 20,
 -10,-20,-20,-20,-20,-20,-20,-10,
 -20,-30,-30,-40,-40,-30,-30,-20,
 -30,-40,-40,-50,-50,-40,-40,-30,
 -30,-40,-40,-50,-50,-40,-40,-30,
 -30,-40,-40,-50,-50,-40,-40,-30,
 -30,-40,-40,-50,-50,-40,-40,-30 };

static const int *pst_for(int type) {
    switch (type) {
        case PAWN: return pst_pawn; case KNIGHT: return pst_knight;
        case BISHOP: return pst_bishop; case ROOK: return pst_rook;
        case QUEEN: return pst_queen; case KING: return pst_king;
    }
    return pst_pawn;
}

/* evaluation from the side-to-move's perspective (negamax convention) */
static int evaluate(const Position *p) {
    int score = 0;
    for (int i = 0; i < 64; i++) {
        signed char pc = p->board[i];
        if (!pc) continue;
        int t = TYPE(pc), c = COLOR(pc);
        int s = val[t];
        const int *pst = pst_for(t);
        s += (c == WHITE) ? pst[i] : pst[(7 - RANK_OF(i)) * 8 + FILE_OF(i)];
        score += (c == WHITE) ? s : -s;
    }
    return (p->side == WHITE) ? score : -score;
}

/* MVV-LVA-ish ordering key */
static int move_score(const Position *p, Move m) {
    int s = 0;
    if (m.flags & MF_CAPTURE) {
        int victim = (m.flags & MF_EP) ? PAWN : TYPE(p->board[m.to]);
        int att = TYPE(p->board[m.from]);
        s += 1000 + val[victim] * 8 - val[att];
    }
    if (m.flags & MF_PROMO) s += 900 + val[m.promo];
    return s;
}

static void order_moves(const Position *p, Move *mv, int n) {
    int keys[MAX_MOVES];
    for (int i = 0; i < n; i++) keys[i] = move_score(p, mv[i]);
    for (int i = 1; i < n; i++) {         /* insertion sort, descending */
        Move mtmp = mv[i]; int ktmp = keys[i]; int j = i - 1;
        while (j >= 0 && keys[j] < ktmp) { mv[j+1] = mv[j]; keys[j+1] = keys[j]; j--; }
        mv[j+1] = mtmp; keys[j+1] = ktmp;
    }
}

#define INF 1000000

static long g_nodes;

static int quiesce(const Position *p, int alpha, int beta) {
    int stand = evaluate(p);
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;
    Move mv[MAX_MOVES];
    int n = chess_gen_legal(p, mv);
    order_moves(p, mv, n);
    for (int i = 0; i < n; i++) {
        if (!(mv[i].flags & (MF_CAPTURE|MF_PROMO))) continue;
        g_nodes++;
        Position t = *p; chess_make(&t, mv[i]);
        int sc = -quiesce(&t, -beta, -alpha);
        if (sc >= beta) return beta;
        if (sc > alpha) alpha = sc;
    }
    return alpha;
}

static int negamax(const Position *p, int depth, int alpha, int beta) {
    if (depth == 0) return quiesce(p, alpha, beta);
    Move mv[MAX_MOVES];
    int n = chess_gen_legal(p, mv);
    if (n == 0) return chess_in_check(p, p->side) ? -(INF - 1) : 0;  /* mate/stalemate */
    if (p->halfmove >= 100) return 0;
    order_moves(p, mv, n);
    int best = -INF;
    for (int i = 0; i < n; i++) {
        g_nodes++;
        Position t = *p; chess_make(&t, mv[i]);
        int sc = -negamax(&t, depth - 1, -beta, -alpha);
        if (sc > best) best = sc;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }
    return best;
}

/* choose a move for the side to move at the given difficulty (0=easy..2=hard).
 * Returns 1 and fills *out, or 0 if there are no legal moves.
 * seed varies the tie-breaking so easy play is not deterministic. */
int ai_pick(const Position *p, int difficulty, unsigned int seed, Move *out) {
    Move mv[MAX_MOVES];
    int n = chess_gen_legal(p, mv);
    if (n == 0) return 0;
    order_moves(p, mv, n);
    int maxdepth = (difficulty <= 0) ? 2 : (difficulty == 1) ? 3 : 4;

    Move best = mv[0];
    /* iterative deepening keeps the best move from the last completed depth */
    for (int d = 1; d <= maxdepth; d++) {
        g_nodes = 0;
        int alpha = -INF, beta = INF, bestsc = -INF;
        Move dbest = mv[0];
        for (int i = 0; i < n; i++) {
            Position t = *p; chess_make(&t, mv[i]);
            int sc = -negamax(&t, d - 1, -beta, -alpha);
            /* easy: add small pseudo-random jitter so it blunders a little */
            if (difficulty == 0) { seed = seed * 1664525u + 1013904223u; sc += (int)((seed >> 24) % 40) - 20; }
            if (sc > bestsc) { bestsc = sc; dbest = mv[i]; }
            if (sc > alpha) alpha = sc;
        }
        best = dbest;
        /* node budget guard: stop deepening if the tree got huge */
        if (g_nodes > 4000000L) break;
    }
    *out = best;
    return 1;
}
