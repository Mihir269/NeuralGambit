#pragma once
// ============================================================================
//  nnue.hpp -- NNUE evaluation for the engine
// ============================================================================
//
//  Loads the network trained by train_nnue_colab.py (file "mihir_v1.nnue")
//  and evaluates positions with pure integer arithmetic.
//
//  ARCHITECTURE (must mirror the Python side):  (768 -> HIDDEN)x2 -> 1
//    * 768 binary features per PERSPECTIVE:
//          idx = is_enemy*384 + piece_type*64 + square
//      where piece_type is P=0 N=1 B=2 R=3 Q=4 K=5 and, for the black
//      perspective, the square is vertically flipped (sq ^ 56) and colors
//      are swapped.  Both perspectives share one 768xHIDDEN weight matrix.
//    * The two HIDDEN-wide accumulators are concatenated
//      [side-to-move, opponent], clamped to [0, QA] (Clipped ReLU), and
//      dotted with the output weights.  Result * SCALE / (QA*QB) = centipawns
//      from the side-to-move's point of view -- same convention as negamax.
//
//  THE ACCUMULATOR TRICK (why NNUE is fast):
//    The first layer's output for a position is just the sum of the weight
//    COLUMNS of its active features, plus a bias.  A chess move only changes
//    2-4 features (piece leaves a square, appears on another, maybe a
//    capture disappears), so instead of recomputing the sum from scratch we
//    keep it cached and add/subtract the few changed columns.  Because this
//    network has no king buckets, we NEVER need a full refresh mid-search --
//    castling is just four add/sub updates like any other move.
//
//  The accumulators live on a stack indexed by search ply, so unmaking a
//  move is free: we just step back to the previous stack entry.
//
//  HOW TO INTEGRATE (details in INTEGRATION.md):
//    1. #include "nnue.hpp" after chess.hpp.
//    2. At engine startup:            nnue::net.load("mihir_v1.nnue");
//    3. At the top of searchPosition: nnue::acc.refresh(board);
//    4. In negamax AND quiescence:
//           nnue::acc.push(board, move);   // BEFORE board.makeMove(move)
//           board.makeMove(move);
//           ...
//           board.unmakeMove(move);
//           nnue::acc.pop();               // AFTER board.unmakeMove(move)
//    5. evaluate() returns nnue::acc.evaluate(board.sideToMove()) when the
//       network is loaded, else falls back to the old PST evaluation.
//
//  RULE OF THUMB: every makeMove during search must be wrapped by a
//  push/pop pair, or the accumulator drifts out of sync with the board.
// ============================================================================

#include "chess.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace nnue {

using namespace chess;

// ---------------------------------------------------------------------------
// Constants -- MUST match train_nnue_colab.py
// ---------------------------------------------------------------------------
constexpr int FEATURES   = 768;   // 2 colors x 6 piece types x 64 squares
constexpr int MAX_HIDDEN = 512;   // largest hidden size we accept from a file
constexpr int QA         = 255;   // feature-transformer quantization scale
constexpr int QB         = 64;    // output-layer quantization scale
constexpr int SCALE      = 400;   // centipawn scale
constexpr int MAX_PLY    = 256;   // accumulator stack depth (search ply)

// Keep NNUE scores well away from the engine's mate-score range so
// "found a mate" and "NNUE really likes this" can never be confused.
constexpr int EVAL_LIMIT = 8000;

// ---------------------------------------------------------------------------
// Network: the trained weights, loaded once at startup
// ---------------------------------------------------------------------------
// Binary layout of mihir_v1.nnue (little-endian):
//   "MNNU"  | u32 version=1 | u32 hidden
//   i16 ftW [768][hidden]   (feature-major: ftW[f*hidden + h])
//   i16 ftB [hidden]
//   i16 outW[2*hidden]      (side-to-move half first, then opponent half)
//   i32 outB
// ---------------------------------------------------------------------------
struct Network {
    int                  hidden = 0;
    std::vector<int16_t> ftW;      // FEATURES * hidden
    std::vector<int16_t> ftB;      // hidden
    std::vector<int16_t> outW;     // 2 * hidden
    int32_t              outB   = 0;
    bool                 loaded = false;

    bool load(const std::string& path) {
        loaded = false;
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;

        char magic[4];
        f.read(magic, 4);
        if (!f || std::memcmp(magic, "MNNU", 4) != 0) return false;

        std::uint32_t version = 0, h = 0;
        f.read(reinterpret_cast<char*>(&version), 4);
        f.read(reinterpret_cast<char*>(&h), 4);
        if (!f || version != 1 || h == 0 || h > MAX_HIDDEN) return false;

        hidden = static_cast<int>(h);
        ftW.resize(static_cast<size_t>(FEATURES) * hidden);
        ftB.resize(hidden);
        outW.resize(2 * static_cast<size_t>(hidden));

        f.read(reinterpret_cast<char*>(ftW.data()),  ftW.size()  * sizeof(int16_t));
        f.read(reinterpret_cast<char*>(ftB.data()),  ftB.size()  * sizeof(int16_t));
        f.read(reinterpret_cast<char*>(outW.data()), outW.size() * sizeof(int16_t));
        f.read(reinterpret_cast<char*>(&outB), sizeof(int32_t));
        if (!f) return false;

        loaded = true;
        return true;
    }
};

inline Network net;   // one global network, shared by everything

// ---------------------------------------------------------------------------
// Feature indexing
// ---------------------------------------------------------------------------
// A (piece, square) pair maps to a different index in each perspective:
//   * white perspective: square as-is,  "enemy" = black pieces
//   * black perspective: square ^ 56 (vertical flip), "enemy" = white pieces
// This is the exact mirror of fen_to_features() in the Python trainer.
// ---------------------------------------------------------------------------
inline int featureIndex(Color persp, PieceType pt, Color pieceColor, Square sq) {
    const int enemy = (pieceColor != persp) ? 1 : 0;
    const int s     = (persp == Color::WHITE) ? sq.index() : (sq.index() ^ 56);
    return enemy * 384 + static_cast<int>(pt) * 64 + s;
}

// ---------------------------------------------------------------------------
// Accumulator: the cached first-layer output, one array per perspective.
// v[0] = white's perspective, v[1] = black's perspective.
// ---------------------------------------------------------------------------
struct Accumulator {
    alignas(64) int16_t v[2][MAX_HIDDEN];   // aligned so -march=native can SIMD
};

// ---------------------------------------------------------------------------
// Evaluator: an accumulator STACK, parallel to the search ply.
//   refresh()  rebuilds ply 0 from a Board (called once per "go").
//   push()     copies the top entry and applies a move's feature deltas.
//   pop()      just steps back -- unmake costs nothing.
//   evaluate() runs the cheap output layer on the top entry.
// ---------------------------------------------------------------------------
class Evaluator {
    std::vector<Accumulator> stack{MAX_PLY + 8};
    int                      top = 0;

    // Add / subtract one feature's weight column in ONE perspective.
    void addFeature(Accumulator& a, int persp, int idx) {
        const int16_t* col = &net.ftW[static_cast<size_t>(idx) * net.hidden];
        for (int h = 0; h < net.hidden; ++h) a.v[persp][h] += col[h];
    }
    void subFeature(Accumulator& a, int persp, int idx) {
        const int16_t* col = &net.ftW[static_cast<size_t>(idx) * net.hidden];
        for (int h = 0; h < net.hidden; ++h) a.v[persp][h] -= col[h];
    }

    // Convenience: apply a piece appearing / disappearing to BOTH perspectives.
    void addPiece(Accumulator& a, PieceType pt, Color c, Square sq) {
        addFeature(a, 0, featureIndex(Color::WHITE, pt, c, sq));
        addFeature(a, 1, featureIndex(Color::BLACK, pt, c, sq));
    }
    void subPiece(Accumulator& a, PieceType pt, Color c, Square sq) {
        subFeature(a, 0, featureIndex(Color::WHITE, pt, c, sq));
        subFeature(a, 1, featureIndex(Color::BLACK, pt, c, sq));
    }

  public:
    // Rebuild the ply-0 accumulator from scratch. Called once at the start
    // of every search (and by the debug checker). ~30 column adds -- cheap.
    void refresh(const Board& board) {
        top = 0;
        Accumulator& a = stack[0];
        for (int p = 0; p < 2; ++p)
            for (int h = 0; h < net.hidden; ++h) a.v[p][h] = net.ftB[h];

        for (int sqi = 0; sqi < 64; ++sqi) {
            const Square sq(sqi);
            const Piece  pc = board.at(sq);
            if (pc != Piece::NONE) addPiece(a, pc.type(), pc.color(), sq);
        }
    }

    // Compute the child accumulator from the current one, applying the
    // feature changes of `move`.  MUST be called BEFORE board.makeMove(move)
    // because we need the pre-move board to know what is being captured.
    void push(const Board& board, Move move) {
        Accumulator&       a    = stack[top + 1];
        const Accumulator& prev = stack[top];
        ++top;

        // Start from a copy of the parent (1 KB memcpy -- very fast).
        std::memcpy(&a, &prev, sizeof(Accumulator));

        const Color  us   = board.sideToMove();
        const Square from = move.from();
        const Square to   = move.to();

        if (move.typeOf() == Move::CASTLING) {
            // The library encodes castling as "king takes own rook":
            // from = king square, to = ROOK square.  Real destinations come
            // from the same helpers Board::makeMove uses internally.
            const bool   kingSide = to > from;
            const Square kingTo   = Square::castling_king_square(kingSide, us);
            const Square rookTo   = Square::castling_rook_square(kingSide, us);

            subPiece(a, PieceType::KING, us, from);
            subPiece(a, PieceType::ROOK, us, to);
            addPiece(a, PieceType::KING, us, kingTo);
            addPiece(a, PieceType::ROOK, us, rookTo);
            return;
        }

        // --- captured piece disappears (if any) ---
        if (move.typeOf() == Move::ENPASSANT) {
            // Captured pawn is NOT on `to`; ep_square() flips the rank by one.
            subPiece(a, PieceType::PAWN, ~us, to.ep_square());
        } else {
            const Piece victim = board.at(to);
            if (victim != Piece::NONE)
                subPiece(a, victim.type(), victim.color(), to);
        }

        // --- the moving piece leaves `from` and lands on `to` ---
        const PieceType mover = board.at(from).type();
        subPiece(a, mover, us, from);
        if (move.typeOf() == Move::PROMOTION)
            addPiece(a, move.promotionType(), us, to);   // pawn becomes N/B/R/Q
        else
            addPiece(a, mover, us, to);
    }

    // Unmake: the parent accumulator is still on the stack, untouched.
    void pop() { --top; }

    // The output layer: clamp (Clipped ReLU), multiply, sum. int32 cannot
    // overflow: |sum| <= 2*512*255*32767 + bias, comfortably inside int64,
    // and in practice trained weights keep it far smaller -- we accumulate
    // in int64 anyway for total safety.
    int evaluate(Color stm) const {
        const Accumulator& a  = stack[top];
        const int16_t*     us = a.v[static_cast<int>(stm)];
        const int16_t*     th = a.v[static_cast<int>(~stm)];

        std::int64_t sum = net.outB;
        for (int h = 0; h < net.hidden; ++h) {
            const int au = std::clamp(static_cast<int>(us[h]), 0, QA);
            const int at = std::clamp(static_cast<int>(th[h]), 0, QA);
            sum += static_cast<std::int64_t>(au) * net.outW[h];
            sum += static_cast<std::int64_t>(at) * net.outW[net.hidden + h];
        }

        const int cp = static_cast<int>(sum * SCALE / (QA * QB));
        return std::clamp(cp, -EVAL_LIMIT, EVAL_LIMIT);
    }

    // Debug helper: rebuilds a fresh accumulator for `board` and compares it
    // with the incrementally-maintained top of stack. Call it inside search
    // (e.g. under #ifndef NDEBUG) if you ever suspect a sync bug.
    bool check(const Board& board) {
        Evaluator fresh;
        fresh.refresh(board);
        return std::memcmp(&fresh.stack[0].v, &stack[top].v,
                           2 * net.hidden * sizeof(int16_t)) == 0;
    }
};

inline Evaluator acc;   // one global evaluator, used by the search thread

}   // namespace nnue
