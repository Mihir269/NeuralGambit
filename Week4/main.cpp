// ============================================================================
//  A small but complete alpha-beta chess engine built on Disservin's
//  header-only chess-library (https://github.com/Disservin/chess-library).
//
//  Features (everything you asked for):
//    * Negamax search with alpha-beta pruning
//    * Iterative deepening (depth 1, 2, 3, ...) with a time budget.
//        - If we run out of time mid-depth, we KEEP the best move from the
//          last fully-completed depth (exactly as requested).
//        - If we find a forced mate at any depth, we stop deepening and play
//          the moves that deliver it.
//    * Transposition table keyed on the library's Zobrist hash (board.hash()).
//    * Quiescence search (only resolves captures / checks at the leaves so the
//      static evaluation is never taken in the middle of a tactical exchange).
//    * Move ordering: TT move -> MVV/LVA captures -> promotions -> killers ->
//      history heuristic. Good ordering is what makes alpha-beta fast.
//    * UCI protocol so GUIs like Cute Chess can talk to the engine.
//
//  A NOTE ON ZOBRIST HASHING
//  -------------------------
//  Zobrist hashing assigns a random 64-bit number to every (piece, square)
//  combination (plus side-to-move, castling rights and en-passant file). The
//  hash of a position is the XOR of all those numbers, and because XOR is its
//  own inverse the hash can be updated incrementally on every move instead of
//  recomputed from scratch. The chess-library already implements exactly this
//  and exposes the running value through `board.hash()`, so we reuse it as the
//  key for our transposition table rather than duplicating (and risking bugs
//  in) the same machinery. `board.zobrist()` would recompute it from scratch;
//  `board.hash()` returns the cheap incremental value, which is what we want.
//
//  Build:  g++ -O3 -std=c++17 -march=native main.cpp -o engine
//          (chess.hpp must be in the same folder or on the include path)
// ============================================================================

#include "chess.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace chess;

// ----------------------------------------------------------------------------
//  Tunable constants
// ----------------------------------------------------------------------------
constexpr int INF      = 32000;   // a value larger than any real evaluation
constexpr int MATE      = 31000;   // score of a checkmate at the root
constexpr int MAX_PLY   = 128;     // hard ceiling on search depth (array sizes)
constexpr int MAX_DEPTH = 64;      // deepest iterative-deepening iteration

// A score whose absolute value is at least this is a "mate score": it means
// "mate found, N plies away" rather than a normal centipawn evaluation.
constexpr int MATE_THRESHOLD = MATE - MAX_PLY;

// Piece values in centipawns, indexed by PieceType's underlying int
// (PAWN=0, KNIGHT=1, BISHOP=2, ROOK=3, QUEEN=4, KING=5).
// The king has no material value because it is always on the board for both
// sides; checkmate is handled by the search via the MATE constant instead.
constexpr int PIECE_VALUE[6] = {100, 320, 330, 500, 900, 0};

// Game-phase weights used for tapered king evaluation (see evaluate()).
// Summed over all pieces, a full board gives 24; an empty board gives 0.
constexpr int PHASE_WEIGHT[6] = {0, 1, 1, 2, 4, 0};

// ----------------------------------------------------------------------------
//  Piece-square tables (Tomasz Michniewski's "Simplified Evaluation Function")
//
//  Each table is written from White's point of view in *visual* order: the
//  first row is rank 8 (the top of the board as you look at a diagram), the
//  last row is rank 1. The library numbers squares with a1 = 0, h8 = 63, so to
//  index a table for a White piece we flip the rank with `sq ^ 56`; for a Black
//  piece we mirror it to White's perspective, which works out to plain `sq`.
// ----------------------------------------------------------------------------
constexpr int PST[5][64] = {
    // Pawn
    {  0,  0,  0,  0,  0,  0,  0,  0,
      50, 50, 50, 50, 50, 50, 50, 50,
      10, 10, 20, 30, 30, 20, 10, 10,
       5,  5, 10, 25, 25, 10,  5,  5,
       0,  0,  0, 20, 20,  0,  0,  0,
       5, -5,-10,  0,  0,-10, -5,  5,
       5, 10, 10,-20,-20, 10, 10,  5,
       0,  0,  0,  0,  0,  0,  0,  0 },
    // Knight
    {-50,-40,-30,-30,-30,-30,-40,-50,
     -40,-20,  0,  0,  0,  0,-20,-40,
     -30,  0, 10, 15, 15, 10,  0,-30,
     -30,  5, 15, 20, 20, 15,  5,-30,
     -30,  0, 15, 20, 20, 15,  0,-30,
     -30,  5, 10, 15, 15, 10,  5,-30,
     -40,-20,  0,  5,  5,  0,-20,-40,
     -50,-40,-30,-30,-30,-30,-40,-50 },
    // Bishop
    {-20,-10,-10,-10,-10,-10,-10,-20,
     -10,  0,  0,  0,  0,  0,  0,-10,
     -10,  0,  5, 10, 10,  5,  0,-10,
     -10,  5,  5, 10, 10,  5,  5,-10,
     -10,  0, 10, 10, 10, 10,  0,-10,
     -10, 10, 10, 10, 10, 10, 10,-10,
     -10,  5,  0,  0,  0,  0,  5,-10,
     -20,-10,-10,-10,-10,-10,-10,-20 },
    // Rook
    {  0,  0,  0,  0,  0,  0,  0,  0,
       5, 10, 10, 10, 10, 10, 10,  5,
      -5,  0,  0,  0,  0,  0,  0, -5,
      -5,  0,  0,  0,  0,  0,  0, -5,
      -5,  0,  0,  0,  0,  0,  0, -5,
      -5,  0,  0,  0,  0,  0,  0, -5,
      -5,  0,  0,  0,  0,  0,  0, -5,
       0,  0,  0,  5,  5,  0,  0,  0 },
    // Queen
    {-20,-10,-10, -5, -5,-10,-10,-20,
     -10,  0,  0,  0,  0,  0,  0,-10,
     -10,  0,  5,  5,  5,  5,  0,-10,
      -5,  0,  5,  5,  5,  5,  0, -5,
       0,  0,  5,  5,  5,  5,  0, -5,
     -10,  5,  5,  5,  5,  5,  0,-10,
     -10,  0,  5,  0,  0,  0,  0,-10,
     -20,-10,-10, -5, -5,-10,-10,-20 }
};

// King in the middlegame: stay tucked away behind your pawns.
constexpr int KING_MG[64] = {
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -20,-30,-30,-40,-40,-30,-30,-20,
    -10,-20,-20,-20,-20,-20,-20,-10,
     20, 20,  0,  0,  0,  0, 20, 20,
     20, 30, 10,  0,  0, 10, 30, 20
};

// King in the endgame: march to the centre to support pawns / deliver mate.
constexpr int KING_EG[64] = {
    -50,-40,-30,-20,-20,-30,-40,-50,
    -30,-20,-10,  0,  0,-10,-20,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-30,  0,  0,  0,  0,-30,-30,
    -50,-30,-30,-30,-30,-30,-30,-50
};

// ============================================================================
//  Transposition table
//
//  A flat, power-of-two array indexed by the low bits of the Zobrist key. Each
//  slot also stores the full key so we can detect index collisions (two
//  different positions landing in the same slot).
//
//  The `flag` records what kind of bound the stored score is:
//    EXACT  - the search returned a true value inside the (alpha, beta) window.
//    LOWER  - a beta cutoff happened; the real score is >= this value.
//    UPPER  - nothing beat alpha; the real score is <= this value.
// ============================================================================
enum : uint8_t { FLAG_NONE = 0, FLAG_EXACT = 1, FLAG_LOWER = 2, FLAG_UPPER = 3 };

struct TTEntry {
    uint64_t key   = 0;          // full Zobrist key (collision check)
    int32_t  score = 0;          // stored score (mate scores are ply-adjusted)
    uint16_t move  = 0;          // best move found here (Move's raw 16-bit form)
    int16_t  depth = 0;          // remaining depth this entry was searched to
    uint8_t  flag  = FLAG_NONE;  // bound type (see enum above)
};

static std::vector<TTEntry> g_tt;     // the table itself
static uint64_t             g_ttMask; // (size - 1), size is a power of two

// Resize to roughly `mb` megabytes, rounded DOWN to a power of two.
void ttResize(size_t mb) {
    size_t entries = (mb * 1024 * 1024) / sizeof(TTEntry);
    size_t pow2 = 1024;                 // never smaller than 1024 entries
    while (pow2 * 2 <= entries) pow2 *= 2;
    g_tt.assign(pow2, TTEntry{});
    g_ttMask = pow2 - 1;
}

void ttClear() { std::fill(g_tt.begin(), g_tt.end(), TTEntry{}); }

// Returns a pointer to the matching entry, or nullptr if this position has not
// been stored (or a different position occupies the slot).
TTEntry* ttProbe(uint64_t key) {
    TTEntry& e = g_tt[key & g_ttMask];
    if (e.flag != FLAG_NONE && e.key == key) return &e;
    return nullptr;
}

// Depth-preferred replacement: overwrite empty slots, a different position, a
// shallower search of the same position, or any exact bound.
void ttStore(uint64_t key, int score, Move move, int depth, uint8_t flag) {
    TTEntry& e = g_tt[key & g_ttMask];
    if (e.flag == FLAG_NONE || e.key != key || depth >= e.depth || flag == FLAG_EXACT) {
        e.key   = key;
        e.score = static_cast<int32_t>(score);
        e.move  = move.move();
        e.depth = static_cast<int16_t>(depth);
        e.flag  = flag;
    }
}

// Mate scores are stored relative to the node they were found at, but we want
// them relative to the root when we read them back. These two helpers translate
// between "distance from this node" and "distance from root".
int scoreToTT(int s, int ply) {
    if (s >=  MATE_THRESHOLD) return s + ply;
    if (s <= -MATE_THRESHOLD) return s - ply;
    return s;
}
int scoreFromTT(int s, int ply) {
    if (s >=  MATE_THRESHOLD) return s - ply;
    if (s <= -MATE_THRESHOLD) return s + ply;
    return s;
}

// ============================================================================
//  Search-wide state
//
//  Only one search runs at a time (on its own thread), so plain globals are
//  fine. `g_stop` is the one thing the main thread touches concurrently, hence
//  it is atomic: the UCI "stop" command and the time checker both set it, and
//  the search reads it to bail out cleanly.
// ============================================================================
static std::atomic<bool> g_stop{false};
static std::thread       g_searchThread;

static uint64_t g_nodes = 0;                 // nodes visited this search

// Killer moves: two quiet moves per ply that recently caused a beta cutoff.
// They are tried early because a move that refuted a sibling often refutes here.
static Move g_killers[MAX_PLY][2];

// History heuristic: how often a quiet (from -> to) move has caused a cutoff,
// indexed by side to move. Quiet moves with a strong history get ordered first.
static int g_history[2][64][64];

// Triangular principal-variation table: g_pv[ply] holds the best line found
// from `ply` downward. After a search, g_pv[0] is the engine's intended line.
static Move g_pv[MAX_PLY][MAX_PLY];
static int  g_pvLen[MAX_PLY];

// Time management for the current search.
static std::chrono::steady_clock::time_point g_startTime;
static long long g_timeBudgetMs = 0;   // how long we may think
static bool      g_useTime      = false; // false for "go depth N" / "go infinite"

long long elapsedMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now() - g_startTime).count();
}

// Called periodically (every 2048 nodes) so we notice the clock without the
// overhead of checking on every single node.
void checkTime() {
    if (g_useTime && elapsedMs() >= g_timeBudgetMs) g_stop = true;
}

bool isMateScore(int s) { return std::abs(s) >= MATE_THRESHOLD; }

// ============================================================================
//  Evaluation
//
//  Returns a score in centipawns from the perspective of the side to move
//  (positive = good for the player about to move). This "negamax" convention
//  lets the search negate the child score and reuse one code path for both
//  colours.
//
//  We use material + piece-square tables, and "taper" the king table between
//  its middlegame and endgame versions according to how much material is left.
// ============================================================================
int evaluate(const Board& board) {
    int score = 0;   // from White's perspective; we flip the sign at the end
    int phase = 0;   // 24 = full board, 0 = bare kings

    // Walk all 64 squares. (Iterating squares is simple and the evaluation is
    // not the search bottleneck; bitboard iteration would be a micro-opt.)
    for (int i = 0; i < 64; ++i) {
        Piece pc = board.at(Square(i));
        PieceType pt = pc.type();
        if (pt == PieceType::NONE) continue;

        int t = static_cast<int>(pt);
        if (t == 5) continue;            // king is scored separately (tapered)

        Color c = pc.color();
        // White reads the visually-flipped square; Black reads it as-is, which
        // mirrors the table to Black's side of the board.
        int idx = (c == Color::WHITE) ? (i ^ 56) : i;

        int s = PIECE_VALUE[t] + PST[t][idx];
        if (c == Color::WHITE) score += s; else score -= s;
        phase += PHASE_WEIGHT[t];
    }

    if (phase > 24) phase = 24;          // promotions could in theory exceed 24

    // Tapered king evaluation: blend middlegame and endgame tables by phase.
    int wk = board.kingSq(Color::WHITE).index();
    int bk = board.kingSq(Color::BLACK).index();
    int wkIdx = wk ^ 56;                 // White: flip to visual order
    int bkIdx = bk;                      // Black: mirror to White's perspective
    int wKing = (KING_MG[wkIdx] * phase + KING_EG[wkIdx] * (24 - phase)) / 24;
    int bKing = (KING_MG[bkIdx] * phase + KING_EG[bkIdx] * (24 - phase)) / 24;
    score += wKing - bKing;

    return (board.sideToMove() == Color::WHITE) ? score : -score;
}

// ============================================================================
//  Move ordering
//
//  We do not sort the whole list up front. Instead we assign each move a score
//  and then, in the search loop, pull the highest-scored remaining move into
//  place one at a time (a partial selection sort). This is cheap and usually a
//  beta cutoff happens after only the first move or two, so fully sorting would
//  be wasted work.
// ============================================================================
int scoreMove(const Board& board, Move m, Move ttMove, int ply, int us) {
    // 1) The move the transposition table recommends is almost always best.
    if (m == ttMove) return 1 << 20;

    // 2) Captures, ranked by MVV/LVA (Most Valuable Victim, Least Valuable
    //    Attacker): grabbing a queen with a pawn is tried before grabbing a
    //    pawn with a queen. Castling is encoded as "king captures rook" in this
    //    library, so we must exclude it before asking isCapture().
    bool isCap = (m.typeOf() != Move::CASTLING) && board.isCapture(m);
    if (isCap) {
        int victimVal;
        if (m.typeOf() == Move::ENPASSANT) {
            victimVal = PIECE_VALUE[0];           // en-passant victim is a pawn
        } else {
            PieceType v = board.at<PieceType>(m.to());
            victimVal = (v == PieceType::NONE) ? 0 : PIECE_VALUE[static_cast<int>(v)];
        }
        int attacker = static_cast<int>(board.at<PieceType>(m.from())); // 0..5
        return 100000 + victimVal * 16 - attacker;
    }

    // 3) Promotions (that are not also captures) are strong, ranked by the new
    //    piece's value (queen first).
    if (m.typeOf() == Move::PROMOTION) {
        return 90000 + PIECE_VALUE[static_cast<int>(m.promotionType())];
    }

    // 4) Killer moves for this ply.
    if (m == g_killers[ply][0]) return 80000;
    if (m == g_killers[ply][1]) return 70000;

    // 5) Everything else: the history score for this quiet move (capped so it
    //    can never jump above the killer band).
    int h = g_history[us][m.from().index()][m.to().index()];
    return h > 60000 ? 60000 : h;
}

// ============================================================================
//  Quiescence search
//
//  At a normal leaf we cannot trust the static evaluation if the position is in
//  the middle of a capture sequence (we might be "up a queen" only because it
//  is about to be recaptured). So instead of returning evaluate() directly, we
//  keep searching *only* captures (and all moves when in check) until the
//  position is quiet, which removes the horizon effect for tactics.
// ============================================================================
int quiescence(Board& board, int alpha, int beta, int ply) {
    if (g_stop.load(std::memory_order_relaxed)) return 0;
    if ((g_nodes & 2047) == 0) checkTime();
    ++g_nodes;

    if (ply >= MAX_PLY - 1) return evaluate(board);

    bool inCheck = board.inCheck();
    int bestScore;

    if (!inCheck) {
        // "Stand pat": the side to move can usually decline to capture, so the
        // static eval is a lower bound on what they can achieve here.
        int standPat = evaluate(board);
        bestScore = standPat;
        if (standPat >= beta) return standPat;          // already too good
        if (standPat > alpha) alpha = standPat;
    } else {
        // When in check we cannot stand pat (we are forced to respond), and we
        // must consider every legal reply so we do not miss a checkmate.
        bestScore = -INF;
    }

    Movelist moves;
    if (inCheck)
        movegen::legalmoves<movegen::MoveGenType::ALL>(moves, board);
    else
        movegen::legalmoves<movegen::MoveGenType::CAPTURE>(moves, board);

    if (inCheck && moves.empty()) return -MATE + ply;   // checkmate

    int n = moves.size();
    int scores[256], idx[256];
    int us = (board.sideToMove() == Color::WHITE) ? 0 : 1;
    for (int i = 0; i < n; ++i) {
        idx[i] = i;
        scores[i] = scoreMove(board, moves[i], Move(Move::NO_MOVE), ply, us);
    }

    for (int i = 0; i < n; ++i) {
        // Selection step: bring the best-scored remaining move to position i.
        int bi = i;
        for (int j = i + 1; j < n; ++j)
            if (scores[idx[j]] > scores[idx[bi]]) bi = j;
        std::swap(idx[i], idx[bi]);
        Move move = moves[idx[i]];

        // Delta pruning: if even winning this capture for free cannot lift us
        // near alpha, skip it. (Disabled while in check, where we search all.)
        if (!inCheck && move.typeOf() != Move::PROMOTION) {
            int victimVal;
            if (move.typeOf() == Move::ENPASSANT) {
                victimVal = PIECE_VALUE[0];
            } else {
                PieceType v = board.at<PieceType>(move.to());
                victimVal = (v == PieceType::NONE) ? 0 : PIECE_VALUE[static_cast<int>(v)];
            }
            if (bestScore + victimVal + 200 < alpha) continue;
        }

        board.makeMove(move);
        int score = -quiescence(board, -beta, -alpha, ply + 1);
        board.unmakeMove(move);

        if (g_stop.load(std::memory_order_relaxed)) return 0;

        if (score > bestScore) {
            bestScore = score;
            if (score > alpha) {
                alpha = score;
                if (score >= beta) break;               // beta cutoff
            }
        }
    }
    return bestScore;
}

// ============================================================================
//  Main alpha-beta search (negamax form)
//
//  Returns the score of `board` searched `depth` plies deep, from the side to
//  move's perspective, given the (alpha, beta) window. `ply` is the distance
//  from the root and is used for mate scoring and the PV / killer tables.
// ============================================================================
int negamax(Board& board, int depth, int alpha, int beta, int ply) {
    if (g_stop.load(std::memory_order_relaxed)) return 0;
    if ((g_nodes & 2047) == 0) checkTime();
    ++g_nodes;

    g_pvLen[ply] = 0;                    // assume no PV from here until proven
    const bool root = (ply == 0);
    const bool inCheck = board.inCheck();

    // Draw detection. isRepetition(1) treats the first repeat as a draw, which
    // is what an engine wants (avoid walking into a draw when winning, or claim
    // one when losing). We never do this at the root, which always has moves.
    if (!root && (board.isRepetition(1) || board.isHalfMoveDraw() ||
                  board.isInsufficientMaterial()))
        return 0;

    if (ply >= MAX_PLY - 1) return evaluate(board);

    // Check extension: searching one ply deeper when in check helps us see
    // through forcing sequences instead of stopping in the middle of one.
    if (inCheck) ++depth;

    // Drop into quiescence at the horizon.
    if (depth <= 0) return quiescence(board, alpha, beta, ply);

    // ---- Transposition table probe ----------------------------------------
    uint64_t key = board.hash();
    Move ttMove = Move(Move::NO_MOVE);
    if (TTEntry* e = ttProbe(key)) {
        ttMove = Move(e->move);                          // use for move ordering
        // Only trust the stored *score* if it was searched at least as deep as
        // we need now, and never cut off at the root (we want a real move).
        if (!root && e->depth >= depth) {
            int s = scoreFromTT(e->score, ply);
            if (e->flag == FLAG_EXACT) return s;
            if (e->flag == FLAG_LOWER && s >= beta)  return s;
            if (e->flag == FLAG_UPPER && s <= alpha) return s;
        }
    }

    // ---- Generate and order moves -----------------------------------------
    Movelist moves;
    movegen::legalmoves<movegen::MoveGenType::ALL>(moves, board);

    // No legal moves: checkmate (adjusted by ply so faster mates score higher)
    // or stalemate (a draw).
    if (moves.empty()) return inCheck ? -MATE + ply : 0;

    int n = moves.size();
    int scores[256], idx[256];
    int us = (board.sideToMove() == Color::WHITE) ? 0 : 1;
    for (int i = 0; i < n; ++i) {
        idx[i] = i;
        scores[i] = scoreMove(board, moves[i], ttMove, ply, us);
    }

    int origAlpha = alpha;
    int bestScore = -INF;
    Move bestMove = Move(Move::NO_MOVE);

    // ---- Search every move -------------------------------------------------
    for (int i = 0; i < n; ++i) {
        int bi = i;
        for (int j = i + 1; j < n; ++j)
            if (scores[idx[j]] > scores[idx[bi]]) bi = j;
        std::swap(idx[i], idx[bi]);
        Move move = moves[idx[i]];

        board.makeMove(move);
        int score = -negamax(board, depth - 1, -beta, -alpha, ply + 1);
        board.unmakeMove(move);

        // If time ran out during the child search, abandon this node. The
        // partial result is discarded by the iterative-deepening loop.
        if (g_stop.load(std::memory_order_relaxed)) return 0;

        if (score > bestScore) {
            bestScore = score;
            bestMove  = move;

            if (score > alpha) {
                alpha = score;

                // Record this move as the start of the principal variation from
                // this ply, then append the child's PV behind it.
                g_pv[ply][0] = move;
                for (int k = 0; k < g_pvLen[ply + 1]; ++k)
                    g_pv[ply][k + 1] = g_pv[ply + 1][k];
                g_pvLen[ply] = g_pvLen[ply + 1] + 1;

                if (score >= beta) {
                    // Beta cutoff: this position is too good, the opponent will
                    // avoid it, so we stop. Reward quiet cutoff moves so they
                    // are searched earlier next time.
                    bool quiet = (move.typeOf() != Move::CASTLING) &&
                                 !board.isCapture(move) &&
                                 move.typeOf() != Move::PROMOTION;
                    if (move.typeOf() == Move::CASTLING) quiet = true;
                    if (quiet) {
                        if (!(move == g_killers[ply][0])) {
                            g_killers[ply][1] = g_killers[ply][0];
                            g_killers[ply][0] = move;
                        }
                        g_history[us][move.from().index()][move.to().index()]
                            += depth * depth;
                    }
                    break;
                }
            }
        }
    }

    // ---- Store the result in the transposition table ----------------------
    uint8_t flag = (bestScore <= origAlpha) ? FLAG_UPPER
                 : (bestScore >= beta)      ? FLAG_LOWER
                                            : FLAG_EXACT;
    ttStore(key, scoreToTT(bestScore, ply), bestMove, depth, flag);

    return bestScore;
}

// ============================================================================
//  UCI search limits
// ============================================================================
struct Limits {
    long long wtime = -1, btime = -1, winc = 0, binc = 0, movetime = -1;
    int  movestogo = 0;
    int  depth     = -1;
    bool infinite  = false;
    bool useTime   = false;
};

// Decide how many milliseconds to think for. With a real clock we spend a
// fraction of the remaining time plus most of the increment, but never more
// than 80% of what is left (so we do not flag).
long long computeThinkTime(const Limits& lim, Color side) {
    const long long OVERHEAD = 30;       // leave a little slack for I/O latency

    if (lim.movetime > 0)
        return std::max<long long>(5, lim.movetime - OVERHEAD);

    long long remain = (side == Color::WHITE) ? lim.wtime : lim.btime;
    long long inc    = (side == Color::WHITE) ? lim.winc  : lim.binc;
    if (remain < 0) return 1LL << 60;    // no clock given -> effectively unbounded

    long long mtg = (lim.movestogo > 0) ? lim.movestogo : 30;
    long long t   = remain / mtg + inc * 3 / 4;
    long long cap = remain * 8 / 10;
    if (t > cap) t = cap;
    t -= OVERHEAD;
    if (t < 5) t = 5;
    return t;
}

// Print one UCI "info" line summarising the just-completed iteration.
void printInfo(int depth, int score) {
    long long ms  = elapsedMs(); if (ms < 1) ms = 1;
    long long nps = g_nodes * 1000 / ms;

    std::cout << "info depth " << depth;
    if (isMateScore(score)) {
        int matePlies = MATE - std::abs(score);
        int mateMoves = (matePlies + 1) / 2;             // plies -> full moves
        std::cout << " score mate " << (score > 0 ? mateMoves : -mateMoves);
    } else {
        std::cout << " score cp " << score;
    }
    std::cout << " nodes " << g_nodes << " nps " << nps << " time " << ms << " pv";
    for (int i = 0; i < g_pvLen[0]; ++i)
        std::cout << " " << uci::moveToUci(g_pv[0][i]);
    std::cout << "\n" << std::flush;
}

// ============================================================================
//  Iterative deepening driver
//
//  Search depth 1, then 2, then 3, ... Each completed depth gives us a better
//  move *and* better move-ordering hints (TT, killers, history) for the next.
//  We stop when:
//    * the clock runs out (keep the last completed depth's move), or
//    * we prove a forced mate (no point searching deeper), or
//    * we hit the requested / maximum depth.
// ============================================================================
Move searchPosition(Board& board, const Limits& lim) {
    // Reset per-search state.
    g_nodes = 0;
    std::memset(g_history, 0, sizeof(g_history));
    std::memset(g_killers, 0, sizeof(g_killers));
    g_startTime = std::chrono::steady_clock::now();

    g_useTime      = lim.useTime;
    g_timeBudgetMs = computeThinkTime(lim, board.sideToMove());
    int maxDepth   = (lim.depth > 0) ? lim.depth : MAX_DEPTH;

    // A legal fallback move in case we are stopped before depth 1 even finishes.
    Movelist rootMoves;
    movegen::legalmoves<movegen::MoveGenType::ALL>(rootMoves, board);
    if (rootMoves.empty()) return Move(Move::NO_MOVE);
    Move bestMove = rootMoves[0];

    for (int depth = 1; depth <= maxDepth; ++depth) {
        int score = negamax(board, depth, -INF, INF, 0);

        // If the clock cut this iteration short, the result is incomplete:
        // discard it and keep the move from the previous completed depth.
        if (g_stop.load(std::memory_order_relaxed)) break;

        bestMove = g_pv[0][0];           // best move from this completed depth
        printInfo(depth, score);

        if (isMateScore(score)) break;   // forced mate found -> just play it

        // If we have already used more than half our budget, the next (roughly
        // 3-5x more expensive) iteration almost certainly will not finish, so
        // stop now and spend the saved time on later moves.
        if (g_useTime && elapsedMs() > g_timeBudgetMs / 2) break;
    }

    return bestMove;
}

// ============================================================================
//  UCI protocol loop
// ============================================================================
Limits parseGo(std::istringstream& iss) {
    Limits lim;
    std::string t;
    while (iss >> t) {
        if      (t == "wtime")     iss >> lim.wtime;
        else if (t == "btime")     iss >> lim.btime;
        else if (t == "winc")      iss >> lim.winc;
        else if (t == "binc")      iss >> lim.binc;
        else if (t == "movestogo") iss >> lim.movestogo;
        else if (t == "movetime")  iss >> lim.movetime;
        else if (t == "depth")     iss >> lim.depth;
        else if (t == "infinite")  lim.infinite = true;
        else if (t == "nodes")     { long long x; iss >> x; }   // accepted, ignored
    }
    // We are on the clock only if a real time control was given and the GUI did
    // not ask for an unbounded ("infinite") search.
    lim.useTime = !lim.infinite &&
                  (lim.movetime > 0 || lim.wtime >= 0 || lim.btime >= 0);
    return lim;
}

// Make sure any previous search thread has finished before we touch shared
// state or start a new one.
void stopSearch() {
    g_stop = true;
    if (g_searchThread.joinable()) g_searchThread.join();
}

int main() {
    std::ios::sync_with_stdio(false);

    Board board(constants::STARTPOS);
    ttResize(64);                        // default 64 MB hash table

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string token;
        iss >> token;

        if (token == "uci") {
            std::cout << "id name MiniAlphaBeta 1.0\n";
            std::cout << "id author Your Name\n";
            std::cout << "option name Hash type spin default 64 min 1 max 4096\n";
            std::cout << "uciok\n" << std::flush;
        }
        else if (token == "isready") {
            std::cout << "readyok\n" << std::flush;
        }
        else if (token == "ucinewgame") {
            stopSearch();
            ttClear();
            board = Board(constants::STARTPOS);
        }
        else if (token == "setoption") {
            // Forms we handle: "setoption name Hash value N"
            //                  "setoption name Clear Hash"
            std::string w, name, value;
            bool readingValue = false;
            while (iss >> w) {
                if (w == "name")  continue;
                if (w == "value") { readingValue = true; continue; }
                if (readingValue) value += value.empty() ? w : " " + w;
                else              name  += name.empty()  ? w : " " + w;
            }
            if (name == "Hash" && !value.empty()) {
                stopSearch();
                ttResize(static_cast<size_t>(std::stoul(value)));
            } else if (name == "Clear Hash") {
                stopSearch();
                ttClear();
            }
        }
        else if (token == "position") {
            stopSearch();
            std::string sub;
            iss >> sub;
            if (sub == "startpos") {
                board = Board(constants::STARTPOS);
                std::string maybeMoves;
                if (iss >> maybeMoves && maybeMoves == "moves") {
                    std::string mv;
                    while (iss >> mv) board.makeMove(uci::uciToMove(board, mv));
                }
            }
            else if (sub == "fen") {
                std::string fen, part;
                bool hasMoves = false;
                while (iss >> part) {
                    if (part == "moves") { hasMoves = true; break; }
                    fen += fen.empty() ? part : " " + part;
                }
                board = Board(fen);
                if (hasMoves) {
                    std::string mv;
                    while (iss >> mv) board.makeMove(uci::uciToMove(board, mv));
                }
            }
        }
        else if (token == "go") {
            Limits lim = parseGo(iss);
            stopSearch();                 // join any previous search
            g_stop = false;

            // Run the search on its own thread so the main thread stays free to
            // receive "stop" / "quit" while we think. The board is copied into
            // the thread so the search can freely make/unmake moves on it.
            Board snapshot = board;
            g_searchThread = std::thread([snapshot, lim]() mutable {
                Move best = searchPosition(snapshot, lim);
                std::cout << "bestmove " << uci::moveToUci(best) << std::endl;
            });
        }
        else if (token == "stop") {
            g_stop = true;                // search notices, finishes, prints move
        }
        else if (token == "quit") {
            stopSearch();
            break;
        }
        // Unknown tokens are silently ignored, as the UCI spec requires.
    }

    stopSearch();
    return 0;
}
