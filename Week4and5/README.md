# MiniAlphaBeta — a small UCI chess engine in C++

A compact but complete alpha-beta chess engine built on
[Disservin's chess-library](https://github.com/Disservin/chess-library).
It implements everything in the brief:

- **Negamax + alpha-beta pruning** — the minimax core, with the sign trick that
  lets one function serve both sides.
- **Iterative deepening with a time budget** — searches depth 1, 2, 3, …; if the
  clock runs out mid-depth it plays the best move from the last *completed*
  depth; if it finds a forced mate it stops deepening immediately.
- **Transposition table** keyed on the library's incremental Zobrist hash
  (`board.hash()`), with depth-preferred replacement and mate-score correction.
- **Quiescence search** — at the leaves it resolves captures (and all replies
  when in check) so the static evaluation is never taken mid-exchange.
- **Move ordering** — TT move → MVV/LVA captures → promotions → killer moves →
  history heuristic. This is what makes the pruning actually fast.
- **UCI protocol** — talks to any UCI GUI such as Cute Chess.

## 1. Get the library header

The engine needs `chess.hpp` (a single header). Either:

- let CMake download it automatically (the default — see below), or
- grab it yourself from the
  [chess-library repo](https://github.com/Disservin/chess-library) and place it
  next to `main.cpp`.

## 2. Build

### With CMake (recommended)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The engine binary will be at `build/engine` (or `build/Release/engine.exe` on
Windows with MSVC).

### Or with a one-line g++ invocation

If `chess.hpp` is in the same folder:

```bash
g++ -O3 -std=c++17 -march=native -pthread main.cpp -o engine
```

## 3. Quick sanity check from the terminal

You can drive the engine by hand using UCI commands:

```
uci
position startpos
go movetime 2000
```

It should print `info` lines for each depth and finish with a `bestmove`.

## 4. Play it in Cute Chess

1. Open Cute Chess → **Tools → Settings → Engines → Add (+)**.
2. Set **Command** to the path of your compiled `engine` binary.
3. Leave **Protocol** as **UCI**. Click **OK**.
4. Start a new game (**Game → New**, `Ctrl+N`), pick your engine as one player
   (and a human or another engine as the opponent), choose a time control, and
   watch it play.

To make engine-vs-engine matches, add a second engine the same way and select
both in the New Game dialog.

## Tuning knobs

- **Hash size**: set via the UCI `Hash` option (default 64 MB), e.g. in Cute
  Chess engine settings, or `setoption name Hash value 256`.
- **Strength vs. speed**: the evaluation uses classic
  [Simplified Evaluation](https://www.chessprogramming.org/Simplified_Evaluation_Function)
  piece-square tables. It's intentionally readable rather than maximally strong;
  the search is where most of the playing strength comes from.

## How the pieces fit together (file map)

Everything lives in `main.cpp`, in top-to-bottom reading order:

1. Constants, piece values, and piece-square tables.
2. `evaluate()` — material + tapered PST, from the side-to-move's perspective.
3. Transposition table (`ttProbe` / `ttStore`) and mate-score helpers.
4. `scoreMove()` — the move-ordering heuristic.
5. `quiescence()` — the tactical leaf search.
6. `negamax()` — the main alpha-beta search with TT, killers, history, PV.
7. `searchPosition()` — iterative deepening + time management.
8. `main()` — the UCI command loop.
