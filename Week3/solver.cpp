#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "chess.hpp"

using namespace chess;
namespace fs = std::filesystem;

struct Puzzle {
    std::string title;
    std::string fen;
    std::string solution;
    int mateInMoves = 0;
};


static constexpr int MATE_SCORE = 100000;

static std::string trim(std::string text) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };

    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

static bool looksLikeFen(const std::string& line) {
    const std::string cleaned = trim(line);
    if (cleaned.find('/') == std::string::npos) {
        return false;
    }

    std::istringstream stream(cleaned);
    std::vector<std::string> parts;
    std::string part;

    while (stream >> part) {
        parts.push_back(part);
    }

    return parts.size() >= 4 && (parts[1] == "w" || parts[1] == "b");
}

static bool looksLikeTitle(const std::string& line) {
    const std::string cleaned = trim(line);
    return cleaned.find(" vs ") != std::string::npos;
}

static bool looksLikeSolution(const std::string& line) {
    const std::string cleaned = trim(line);
    return !cleaned.empty() && std::isdigit(static_cast<unsigned char>(cleaned[0]));
}

static std::vector<Puzzle> loadTxt(const fs::path& filePath, int mateInMoves) {
    std::ifstream input(filePath);
    std::vector<Puzzle> puzzles;

    std::string line;
    std::string title;
    std::string fen;

    while (std::getline(input, line)) {
        const std::string cleaned = trim(line);
        if (cleaned.empty()) {
            continue;
        }

        if (looksLikeTitle(cleaned)) {
            title = cleaned;
            continue;
        }

        if (looksLikeFen(cleaned)) {
            fen = cleaned;
            continue;
        }

        if (!fen.empty() && looksLikeSolution(cleaned)) {
            puzzles.push_back(Puzzle{title.empty() ? filePath.filename().string() : title, fen, cleaned, mateInMoves});
            title.clear();
            fen.clear();
        }
    }

    return puzzles;
}

static std::vector<Puzzle> loadJson(const fs::path& filePath, int mateInMoves) {
    std::ifstream input(filePath);
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    std::vector<Puzzle> puzzles;
    const std::regex entry(
    R"regex("([^"]+)"\s*:\s*"([^"]*)")regex"
    );

    for (std::sregex_iterator it(content.begin(), content.end(), entry), end; it != end; ++it) {
        puzzles.push_back(Puzzle{filePath.filename().string(), (*it)[1].str(), (*it)[2].str(), mateInMoves});
    }

    return puzzles;
}

static std::vector<Puzzle> loadPuzzles(const fs::path& filePath, int mateInMoves) {
    if (filePath.extension() == ".json") {
        return loadJson(filePath, mateInMoves);
    }
    return loadTxt(filePath, mateInMoves);
}


static std::string moveLineToSan(Board board, const std::vector<Move>& line) {
    std::ostringstream out;
    int moveNumber = 1;

    for (std::size_t i = 0; i < line.size(); ++i) {
        if (board.sideToMove() == Color::WHITE) {
            out << moveNumber << ". ";
        } else if (i == 0) {
            out << moveNumber << "... ";
        }

        out << uci::moveToSan(board, line[i]);
        board.makeMove<true>(line[i]);

        if (board.sideToMove() == Color::WHITE) {
            ++moveNumber;
        }

        if (i + 1 < line.size()) {
            out << ' ';
        }
    }

    return out.str();
}

// A transposition table entry caches the result of searching a position to a
// given remaining-ply budget, keyed by the position's Zobrist hash (the chess
// library already maintains this incrementally via Board::hash()). Because the
// search uses alpha-beta pruning, a stored score isn't always the exact value —
// it may only be a lower or upper bound, depending on whether the search that
// produced it was cut short. Recording which kind it is lets later probes reuse
// it safely instead of mistaking a bound for an exact result.
//
// Only the single best move is stored (not a whole continuation) — that's all
// the search itself needs, and it keeps every node visit cheap. The full line
// is reconstructed afterwards by extractLine(), which just walks the table.
enum class Bound { EXACT, LOWER, UPPER };

struct TTEntry {
    int score = 0;
    int pliesSearched = 0;
    Bound bound = Bound::EXACT;
    Move bestMove = Move(Move::NO_MOVE);
};

using TransTable = std::unordered_map<std::uint64_t, TTEntry>;

static int searchMate(Board& board, int pliesLeft, int alpha, int beta, TransTable& tt) {
    const int alphaOrig = alpha;
    const int betaOrig = beta;
    const std::uint64_t key = board.hash();

    const auto cached = tt.find(key);
    if (cached != tt.end() && cached->second.pliesSearched >= pliesLeft) {
        const TTEntry& entry = cached->second;
        if (entry.bound == Bound::EXACT) {
            return entry.score;
        } else if (entry.bound == Bound::LOWER) {
            alpha = std::max(alpha, entry.score);
        } else {
            beta = std::min(beta, entry.score);
        }

        if (alpha >= beta) {
            return entry.score;
        }
    }

    Movelist moves;
    movegen::legalmoves(moves, board);

    if (moves.empty()) {
        // Checkmate or stalemate: a permanent fact about this position, true
        // for any search budget, so it's always safe to reuse.
        const int score = board.inCheck() ? -MATE_SCORE : 0;
        tt[key] = TTEntry{score, std::numeric_limits<int>::max(), Bound::EXACT, Move(Move::NO_MOVE)};
        return score;
    }

    if (pliesLeft == 0) {
        // Ran out of search budget without resolving the position; too cheap
        // and too depth-specific to be worth caching.
        return 0;
    }

    int bestScore = -MATE_SCORE;
    Move bestMove(Move::NO_MOVE);

    for (const auto& move : moves) {
        board.makeMove<true>(move);
        const int score = -searchMate(board, pliesLeft - 1, -beta, -alpha, tt);
        board.unmakeMove(move);

        if (score > bestScore) {
            bestScore = score;
            bestMove = move;
        }

        alpha = std::max(alpha, bestScore);
        if (alpha >= beta) {
            break;
        }
    }

    Bound bound = Bound::EXACT;
    if (bestScore <= alphaOrig) {
        bound = Bound::UPPER;
    } else if (bestScore >= betaOrig) {
        bound = Bound::LOWER;
    }
    tt[key] = TTEntry{bestScore, pliesLeft, bound, bestMove};

    return bestScore;
}

// Reconstructs the actual move sequence for a position already known to have a
// forced mate within pliesLeft plies, by repeatedly asking searchMate for the
// best move and replaying it. Because searchMate's own table lookup makes each
// of these calls cheap once the position has been (even partially) explored,
// this just reads off the line instead of re-deriving it from scratch.
static std::vector<Move> extractLine(Board board, int pliesLeft, TransTable& tt) {
    std::vector<Move> line;

    while (pliesLeft > 0) {
        const int score = searchMate(board, pliesLeft, -MATE_SCORE, MATE_SCORE, tt);
        if (score <= 0) {
            break;
        }

        const Move move = tt.at(board.hash()).bestMove;
        if (move == Move(Move::NO_MOVE)) {
            break;
        }

        line.push_back(move);
        board.makeMove<true>(move);
        --pliesLeft;
    }

    return line;
}

static std::optional<std::vector<Move>> solveOne(const Puzzle& puzzle, TransTable& tt) {
    Board board(puzzle.fen);
    const int targetPlies = std::max(0, puzzle.mateInMoves * 2 - 1);

    const int score = searchMate(board, targetPlies, -MATE_SCORE, MATE_SCORE, tt);
    if (score <= 0) {
        return std::nullopt;
    }

    return extractLine(board, targetPlies, tt);
}

// Splits a puzzle's solution text (e.g. "1. Rh8+ 2. Kg4 Nf5 ...") into plain
// SAN moves, stripping move-number prefixes like "1." / "1..." / "12.".
static std::vector<std::string> parseSanMoves(const std::string& solution) {
    std::vector<std::string> sanMoves;
    std::istringstream stream(solution);
    std::string token;

    while (stream >> token) {
        std::size_t pos = 0;
        while (pos < token.size() && (std::isdigit(static_cast<unsigned char>(token[pos])) || token[pos] == '.')) {
            ++pos;
        }

        const std::string move = token.substr(pos);
        if (!move.empty()) {
            sanMoves.push_back(move);
        }
    }

    return sanMoves;
}

// Plays the puzzle out move by move: on the bot's turn it searches for its own
// best move, and on the opponent's turn it simply plays the reply given in the
// puzzle's known solution. This keeps the printed line in sync with the puzzle
// data instead of letting the bot's own defense search wander off and pick a
// different (but equally "won") continuation.
//
// A single transposition table is shared across all of this puzzle's searches
// (one per bot turn). Each bot turn searches a position that's a sub-tree of
// the very first, full-depth search, so later, shallower searches reuse work
// the first one already did instead of repeating it from scratch.
static std::optional<std::vector<Move>> playOutPuzzle(const Puzzle& puzzle) {
    TransTable tt;

    const auto sanMoves = parseSanMoves(puzzle.solution);
    if (sanMoves.empty()) {
        return solveOne(puzzle, tt);
    }

    Board board(puzzle.fen);
    std::vector<Move> line;

    for (std::size_t i = 0; i < sanMoves.size(); ++i) {
        if (i % 2 == 0) {
            // Bot's turn: search for the best move, using only as many plies as
            // are left in the known solution.
            const int pliesLeft = static_cast<int>(sanMoves.size() - i);
            const int score = searchMate(board, pliesLeft, -MATE_SCORE, MATE_SCORE, tt);

            if (score <= 0) {
                // Solution text didn't line up with an actual forced mate here
                // (e.g. malformed/branching solution text) — fall back.
                return solveOne(puzzle, tt);
            }

            const Move move = tt.at(board.hash()).bestMove;
            board.makeMove<true>(move);
            line.push_back(move);
        } else {
            // Opponent's turn: play the move straight from the puzzle data.
            try {
                const Move move = uci::parseSan(board, sanMoves[i]);
                board.makeMove<true>(move);
                line.push_back(move);
            } catch (const std::exception&) {
                // Solution text isn't a clean SAN move list — fall back.
                return solveOne(puzzle, tt);
            }
        }
    }

    return line;
}

static void solveFile(const fs::path& filePath, int mateInMoves) {
    const auto puzzles = loadPuzzles(filePath, mateInMoves);

    std::cout << "\n" << filePath.filename().string() << "\n";
    std::cout << std::string(filePath.filename().string().size(), '-') << "\n";

    std::size_t solved = 0;
    for (const auto& puzzle : puzzles) {
        const auto solution = playOutPuzzle(puzzle);
        std::string solutionText;

        if (solution) {
            solutionText = moveLineToSan(Board(puzzle.fen), *solution);
        }

        std::cout << "Title: " << puzzle.title << "\n";
        std::cout << "FEN  : " << puzzle.fen << "\n";

        if (!solutionText.empty()) {
            ++solved;
            std::cout << "Bot  : " << solutionText << "\n";
        } else {
            std::cout << "Bot  : no forced mate found\n";
        }

        if (!puzzle.solution.empty()) {
            std::cout << "Data : " << puzzle.solution << "\n";
        }

        std::cout << "\n";
    }

    std::cout << "Solved " << solved << " / " << puzzles.size() << " puzzles in " << filePath.filename().string()
              << "\n";
}

int main(int argc, char** argv) {
    if (argc < 3 || ((argc - 1) % 2 != 0)) {
        std::cerr << "Usage: solver <mate-in-moves> <file-or-folder> [more pairs...]\n";
        std::cerr << "Example: solver 2 mate_in_2.json 3 mate_in_3.json 4 mate_in_4.json\n";
        return 1;
    }

    for (int i = 1; i < argc; i += 2) {
        const int mateInMoves = std::stoi(argv[i]);
        const fs::path path = argv[i + 1];

        if (fs::is_directory(path)) {
            for (const auto& entry : fs::directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    const auto ext = entry.path().extension().string();
                    if (ext == ".txt" || ext == ".json") {
                        solveFile(entry.path(), mateInMoves);
                    }
                }
            }
        } else if (fs::is_regular_file(path)) {
            solveFile(path, mateInMoves);
        } else {
            std::cerr << "Skipping unknown path: " << path.string() << "\n";
        }
    }

    return 0;
}