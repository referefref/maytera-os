/* engine.c - Maytera Chess rules engine. Full legal move generation incl.
 * castling, en passant, promotion; check/checkmate/stalemate; draws by 50-move,
 * insufficient material, threefold repetition; SAN notation. libc-only. */
#include "chess.h"
#include <string.h>

#define FILE_OF(sq) ((sq) & 7)
#define RANK_OF(sq) ((sq) >> 3)
#define ONBOARD(f,r) ((f) >= 0 && (f) < 8 && (r) >= 0 && (r) < 8)
#define SQ(f,r) ((r) * 8 + (f))
#define TYPE(pc) ((pc) < 0 ? -(pc) : (pc))
#define COLOR(pc) ((pc) < 0 ? BLACK : WHITE)   /* only valid when pc != 0 */

static const int knight_d[8][2] = {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
static const int king_d[8][2]   = {{0,1},{1,1},{1,0},{1,-1},{0,-1},{-1,-1},{-1,0},{-1,1}};
static const int bishop_d[4][2] = {{1,1},{1,-1},{-1,-1},{-1,1}};
static const int rook_d[4][2]   = {{0,1},{1,0},{0,-1},{-1,0}};

void chess_new(Game *g) {
    memset(g, 0, sizeof(*g));
    Position *p = &g->pos;
    static const signed char back[8] = {ROOK,KNIGHT,BISHOP,QUEEN,KING,BISHOP,KNIGHT,ROOK};
    for (int f = 0; f < 8; f++) {
        p->board[SQ(f,0)] = back[f];        /* white back rank */
        p->board[SQ(f,1)] = PAWN;
        p->board[SQ(f,6)] = -PAWN;
        p->board[SQ(f,7)] = -back[f];       /* black back rank */
    }
    p->side = WHITE;
    p->castle = CR_WK|CR_WQ|CR_BK|CR_BQ;
    p->ep = -1;
    p->halfmove = 0;
    p->fullmove = 1;
    g->histn = 0;
    g->redon = 0;
}

/* is `sq` attacked by any piece of color byColor? */
int chess_is_attacked(const Position *p, int sq, int byColor) {
    int f = FILE_OF(sq), r = RANK_OF(sq);
    int sign = (byColor == WHITE) ? 1 : -1;
    /* pawns: a white pawn on (f-1,r-1)/(f+1,r-1) attacks sq */
    int pr = r - sign;   /* rank the attacking pawn would sit on */
    for (int df = -1; df <= 1; df += 2) {
        int af = f + df;
        if (ONBOARD(af, pr) && p->board[SQ(af,pr)] == sign * PAWN) return 1;
    }
    /* knights */
    for (int i = 0; i < 8; i++) {
        int af = f + knight_d[i][0], ar = r + knight_d[i][1];
        if (ONBOARD(af,ar) && p->board[SQ(af,ar)] == sign * KNIGHT) return 1;
    }
    /* king */
    for (int i = 0; i < 8; i++) {
        int af = f + king_d[i][0], ar = r + king_d[i][1];
        if (ONBOARD(af,ar) && p->board[SQ(af,ar)] == sign * KING) return 1;
    }
    /* bishops/queen (diagonals) */
    for (int i = 0; i < 4; i++) {
        int af = f, ar = r;
        for (;;) {
            af += bishop_d[i][0]; ar += bishop_d[i][1];
            if (!ONBOARD(af,ar)) break;
            signed char pc = p->board[SQ(af,ar)];
            if (pc) {
                if (COLOR(pc) == byColor && (TYPE(pc) == BISHOP || TYPE(pc) == QUEEN)) return 1;
                break;
            }
        }
    }
    /* rooks/queen (orthogonals) */
    for (int i = 0; i < 4; i++) {
        int af = f, ar = r;
        for (;;) {
            af += rook_d[i][0]; ar += rook_d[i][1];
            if (!ONBOARD(af,ar)) break;
            signed char pc = p->board[SQ(af,ar)];
            if (pc) {
                if (COLOR(pc) == byColor && (TYPE(pc) == ROOK || TYPE(pc) == QUEEN)) return 1;
                break;
            }
        }
    }
    return 0;
}

static int king_sq(const Position *p, int color) {
    signed char k = (color == WHITE) ? KING : -KING;
    for (int i = 0; i < 64; i++) if (p->board[i] == k) return i;
    return -1;
}

int chess_in_check(const Position *p, int color) {
    int ks = king_sq(p, color);
    if (ks < 0) return 0;
    return chess_is_attacked(p, ks, color ^ 1);
}

/* apply move to position (updates side, castle, ep, clocks). No legality check. */
void chess_make(Position *p, Move m) {
    signed char pc = p->board[m.from];
    int color = COLOR(pc);
    int type = TYPE(pc);
    int newep = -1;

    p->halfmove++;
    if (type == PAWN || p->board[m.to] != EMPTY) p->halfmove = 0;

    /* en-passant capture removes the pawn behind the target */
    if (m.flags & MF_EP) {
        int capsq = SQ(FILE_OF(m.to), RANK_OF(m.from));
        p->board[capsq] = EMPTY;
        p->halfmove = 0;
    }
    /* move the piece */
    p->board[m.to] = pc;
    p->board[m.from] = EMPTY;

    /* promotion */
    if (m.flags & MF_PROMO)
        p->board[m.to] = (signed char)((color == WHITE) ? m.promo : -m.promo);

    /* castling: move the rook too */
    if (m.flags & MF_CASTLE_K) {
        int r = RANK_OF(m.from);
        p->board[SQ(5,r)] = p->board[SQ(7,r)];
        p->board[SQ(7,r)] = EMPTY;
    } else if (m.flags & MF_CASTLE_Q) {
        int r = RANK_OF(m.from);
        p->board[SQ(3,r)] = p->board[SQ(0,r)];
        p->board[SQ(0,r)] = EMPTY;
    }

    /* double push sets ep target */
    if (m.flags & MF_DBLPUSH)
        newep = SQ(FILE_OF(m.from), (RANK_OF(m.from) + RANK_OF(m.to)) / 2);

    /* update castling rights */
    if (type == KING) {
        if (color == WHITE) p->castle &= ~(CR_WK|CR_WQ);
        else p->castle &= ~(CR_BK|CR_BQ);
    }
    /* rook moved or captured from its home square */
    if (m.from == SQ(0,0) || m.to == SQ(0,0)) p->castle &= ~CR_WQ;
    if (m.from == SQ(7,0) || m.to == SQ(7,0)) p->castle &= ~CR_WK;
    if (m.from == SQ(0,7) || m.to == SQ(0,7)) p->castle &= ~CR_BQ;
    if (m.from == SQ(7,7) || m.to == SQ(7,7)) p->castle &= ~CR_BK;

    p->ep = (signed char)newep;
    if (color == BLACK) p->fullmove++;
    p->side ^= 1;
}

/* add a move, expanding promotions */
static void add_move(Move *out, int *n, int from, int to, int flags, int promo_rank) {
    if (promo_rank) {
        int base = flags | MF_PROMO;
        static const int pr[4] = {QUEEN, ROOK, BISHOP, KNIGHT};
        for (int i = 0; i < 4; i++) {
            out[*n].from = (signed char)from; out[*n].to = (signed char)to;
            out[*n].promo = (signed char)pr[i]; out[*n].flags = (signed char)base;
            (*n)++;
        }
    } else {
        out[*n].from = (signed char)from; out[*n].to = (signed char)to;
        out[*n].promo = 0; out[*n].flags = (signed char)flags;
        (*n)++;
    }
}

/* pseudo-legal move generation for side to move */
static int gen_pseudo(const Position *p, Move *out) {
    int n = 0;
    int me = p->side, opp = me ^ 1;
    int sign = (me == WHITE) ? 1 : -1;
    for (int sq = 0; sq < 64; sq++) {
        signed char pc = p->board[sq];
        if (pc == 0 || COLOR(pc) != me) continue;
        int f = FILE_OF(sq), r = RANK_OF(sq), t = TYPE(pc);
        if (t == PAWN) {
            int dir = sign;
            int r1 = r + dir;
            int startrank = (me == WHITE) ? 1 : 6;
            int promorank = (me == WHITE) ? 7 : 0;
            if (ONBOARD(f, r1) && p->board[SQ(f,r1)] == EMPTY) {
                add_move(out, &n, sq, SQ(f,r1), 0, (r1 == promorank));
                int r2 = r + 2*dir;
                if (r == startrank && p->board[SQ(f,r2)] == EMPTY)
                    add_move(out, &n, sq, SQ(f,r2), MF_DBLPUSH, 0);
            }
            for (int df = -1; df <= 1; df += 2) {
                int cf = f + df;
                if (!ONBOARD(cf, r1)) continue;
                signed char tc = p->board[SQ(cf,r1)];
                if (tc != 0 && COLOR(tc) == opp)
                    add_move(out, &n, sq, SQ(cf,r1), MF_CAPTURE, (r1 == promorank));
                else if (p->ep >= 0 && SQ(cf,r1) == p->ep)
                    add_move(out, &n, sq, SQ(cf,r1), MF_CAPTURE|MF_EP, 0);
            }
        } else if (t == KNIGHT) {
            for (int i = 0; i < 8; i++) {
                int af = f + knight_d[i][0], ar = r + knight_d[i][1];
                if (!ONBOARD(af,ar)) continue;
                signed char tc = p->board[SQ(af,ar)];
                if (tc == 0) add_move(out, &n, sq, SQ(af,ar), 0, 0);
                else if (COLOR(tc) == opp) add_move(out, &n, sq, SQ(af,ar), MF_CAPTURE, 0);
            }
        } else if (t == KING) {
            for (int i = 0; i < 8; i++) {
                int af = f + king_d[i][0], ar = r + king_d[i][1];
                if (!ONBOARD(af,ar)) continue;
                signed char tc = p->board[SQ(af,ar)];
                if (tc == 0) add_move(out, &n, sq, SQ(af,ar), 0, 0);
                else if (COLOR(tc) == opp) add_move(out, &n, sq, SQ(af,ar), MF_CAPTURE, 0);
            }
            /* castling */
            int hr = (me == WHITE) ? 0 : 7;
            if (r == hr && f == 4 && !chess_is_attacked(p, sq, opp)) {
                int kbit = (me == WHITE) ? CR_WK : CR_BK;
                int qbit = (me == WHITE) ? CR_WQ : CR_BQ;
                if ((p->castle & kbit) && p->board[SQ(5,hr)] == 0 && p->board[SQ(6,hr)] == 0
                    && !chess_is_attacked(p, SQ(5,hr), opp) && !chess_is_attacked(p, SQ(6,hr), opp))
                    add_move(out, &n, sq, SQ(6,hr), MF_CASTLE_K, 0);
                if ((p->castle & qbit) && p->board[SQ(3,hr)] == 0 && p->board[SQ(2,hr)] == 0
                    && p->board[SQ(1,hr)] == 0
                    && !chess_is_attacked(p, SQ(3,hr), opp) && !chess_is_attacked(p, SQ(2,hr), opp))
                    add_move(out, &n, sq, SQ(2,hr), MF_CASTLE_Q, 0);
            }
        } else {
            const int (*dirs)[2]; int ndir;
            if (t == BISHOP) { dirs = bishop_d; ndir = 4; }
            else if (t == ROOK) { dirs = rook_d; ndir = 4; }
            else { dirs = king_d; ndir = 8; }  /* queen */
            for (int i = 0; i < ndir; i++) {
                int af = f, ar = r;
                for (;;) {
                    af += dirs[i][0]; ar += dirs[i][1];
                    if (!ONBOARD(af,ar)) break;
                    signed char tc = p->board[SQ(af,ar)];
                    if (tc == 0) add_move(out, &n, sq, SQ(af,ar), 0, 0);
                    else { if (COLOR(tc) == opp) add_move(out, &n, sq, SQ(af,ar), MF_CAPTURE, 0); break; }
                }
            }
        }
    }
    return n;
}

int chess_gen_legal(const Position *p, Move *out) {
    Move pseudo[MAX_MOVES];
    int np = gen_pseudo(p, pseudo);
    int n = 0, me = p->side;
    for (int i = 0; i < np; i++) {
        Position t = *p;
        chess_make(&t, pseudo[i]);
        if (!chess_in_check(&t, me)) out[n++] = pseudo[i];
    }
    return n;
}

int chess_result(const Position *p) {
    Move mv[MAX_MOVES];
    int n = chess_gen_legal(p, mv);
    if (n == 0)
        return chess_in_check(p, p->side) ? RES_CHECKMATE : RES_STALEMATE;
    if (p->halfmove >= 100) return RES_DRAW_50;
    /* insufficient material: K vs K, K+minor vs K, K+B vs K+B same color */
    int wn = 0, bn = 0, wb = 0, bb = 0, others = 0, wbsq = 0, bbsq = 0;
    for (int i = 0; i < 64; i++) {
        signed char pc = p->board[i]; if (!pc) continue;
        int t = TYPE(pc);
        if (t == KING) continue;
        if (t == KNIGHT) { if (COLOR(pc)==WHITE) wn++; else bn++; }
        else if (t == BISHOP) { if (COLOR(pc)==WHITE){wb++;wbsq=(FILE_OF(i)+RANK_OF(i))&1;} else {bb++;bbsq=(FILE_OF(i)+RANK_OF(i))&1;} }
        else others++;
    }
    if (!others) {
        int minors = wn + bn + wb + bb;
        if (minors == 0) return RES_DRAW_MATERIAL;                 /* K vs K */
        if (minors == 1) return RES_DRAW_MATERIAL;                 /* K+minor vs K */
        if (minors == 2 && wb == 1 && bb == 1 && wbsq == bbsq)     /* K+B vs K+B same colour */
            return RES_DRAW_MATERIAL;
    }
    return RES_NONE;
}

/* simple position hash (board + side + castle + ep file) for repetition */
unsigned long chess_hash(const Position *p) {
    unsigned long h = 1469598103934665603UL;
    for (int i = 0; i < 64; i++) { h ^= (unsigned char)(p->board[i] + 7); h *= 1099511628211UL; }
    h ^= (unsigned long)(p->side + 1) * 0x9e3779b97f4a7c15UL;
    h ^= (unsigned long)(p->castle + 1) * 0x100000001b3UL;
    h ^= (unsigned long)((p->ep < 0 ? 0 : FILE_OF(p->ep) + 1)) * 0xff51afd7ed558ccdUL;
    return h;
}

int chess_find_move(const Position *p, int from, int to, Move *out) {
    Move mv[MAX_MOVES];
    int n = chess_gen_legal(p, mv);
    Move *best = 0;
    for (int i = 0; i < n; i++) {
        if (mv[i].from == from && mv[i].to == to) {
            /* prefer queen promotion by default; caller can override promo */
            if (!(mv[i].flags & MF_PROMO)) { *out = mv[i]; return 1; }
            if (mv[i].promo == QUEEN) { best = &mv[i]; }
        }
    }
    if (best) { *out = *best; return 1; }
    return 0;
}

int chess_play(Game *g, Move m) {
    Move legal[MAX_MOVES];
    int n = chess_gen_legal(&g->pos, legal);
    int ok = 0;
    for (int i = 0; i < n; i++)
        if (legal[i].from == m.from && legal[i].to == m.to &&
            (!(legal[i].flags & MF_PROMO) || legal[i].promo == m.promo)) { m = legal[i]; ok = 1; break; }
    if (!ok) return 0;
    if (g->histn < MAX_HIST) {
        g->hist[g->histn] = g->pos;
        g->histmove[g->histn] = m;
        g->histn++;
    }
    chess_make(&g->pos, m);
    g->redon = 0;   /* new move clears redo */
    return 1;
}

int chess_undo(Game *g) {
    if (g->histn == 0) return 0;
    g->histn--;
    if (g->redon < MAX_HIST) {
        g->redo[g->redon] = g->pos;
        g->redomove[g->redon] = g->histmove[g->histn];
        g->redon++;
    }
    g->pos = g->hist[g->histn];
    return 1;
}

int chess_redo(Game *g) {
    if (g->redon == 0) return 0;
    g->redon--;
    Move m = g->redomove[g->redon];
    if (g->histn < MAX_HIST) {
        g->hist[g->histn] = g->pos;
        g->histmove[g->histn] = m;
        g->histn++;
    }
    chess_make(&g->pos, m);
    return 1;
}

int chess_game_result(const Game *g) {
    int r = chess_result(&g->pos);
    if (r != RES_NONE) return r;
    /* threefold repetition over the game history */
    unsigned long cur = chess_hash(&g->pos);
    int reps = 1;
    for (int i = 0; i < g->histn; i++)
        if (chess_hash(&g->hist[i]) == cur) reps++;
    if (reps >= 3) return RES_DRAW_REPETITION;
    return RES_NONE;
}

/* ---- SAN notation ---- */
static char pchar(int type) {
    switch (type) { case KNIGHT: return 'N'; case BISHOP: return 'B';
        case ROOK: return 'R'; case QUEEN: return 'Q'; case KING: return 'K'; }
    return '?';
}

void chess_san(const Position *p, Move m, char *buf) {
    char *o = buf;
    signed char pc = p->board[m.from];
    int type = TYPE(pc);
    if (m.flags & MF_CASTLE_K) { strcpy(buf, "O-O"); goto suffix; }
    if (m.flags & MF_CASTLE_Q) { strcpy(buf, "O-O-O"); goto suffix; }
    if (type == PAWN) {
        if (m.flags & MF_CAPTURE) { *o++ = 'a' + FILE_OF(m.from); *o++ = 'x'; }
        *o++ = 'a' + FILE_OF(m.to); *o++ = '1' + RANK_OF(m.to);
        if (m.flags & MF_PROMO) { *o++ = '='; *o++ = pchar(m.promo); }
        *o = 0;
        goto suffix;
    }
    *o++ = pchar(type);
    /* disambiguation: any other same-type piece that can also go to m.to? */
    {
        Move mv[MAX_MOVES]; int n = chess_gen_legal(p, mv);
        int sameFile = 0, sameRank = 0, ambig = 0;
        for (int i = 0; i < n; i++) {
            if (mv[i].to == m.to && mv[i].from != m.from &&
                TYPE(p->board[mv[i].from]) == type) {
                ambig = 1;
                if (FILE_OF(mv[i].from) == FILE_OF(m.from)) sameFile = 1;
                if (RANK_OF(mv[i].from) == RANK_OF(m.from)) sameRank = 1;
            }
        }
        if (ambig) {
            if (!sameFile) *o++ = 'a' + FILE_OF(m.from);
            else if (!sameRank) *o++ = '1' + RANK_OF(m.from);
            else { *o++ = 'a' + FILE_OF(m.from); *o++ = '1' + RANK_OF(m.from); }
        }
    }
    if (m.flags & MF_CAPTURE) *o++ = 'x';
    *o++ = 'a' + FILE_OF(m.to); *o++ = '1' + RANK_OF(m.to);
    *o = 0;
suffix:
    {
        Position t = *p;
        chess_make(&t, m);
        int o2len = 0; while (buf[o2len]) o2len++;
        if (chess_in_check(&t, t.side)) {
            Move mv[MAX_MOVES];
            int n = chess_gen_legal(&t, mv);
            buf[o2len++] = (n == 0) ? '#' : '+';
            buf[o2len] = 0;
        }
    }
}
