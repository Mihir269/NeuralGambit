# Integrating NNUE into the engine — exact steps

Five small edits to `main.cpp`. The golden rule behind all of them:
**every `makeMove` during search gets a `push` before it and a `pop` after
its matching `unmakeMove`.** That keeps the cached accumulator in perfect
sync with the board.

---

## 1. Include the header

Right after the existing include of the chess library:

```cpp
#include "chess.hpp"
#include "nnue.hpp"      // <-- add this line
```

Put `nnue.hpp` in the same folder as `main.cpp`, and add it to your CMake
sources if you list headers explicitly (not required for the build to work).

## 2. Load the network at startup (in `main()`)

At the top of `main()`, before the UCI loop:

```cpp
std::string evalFile = "mihir_v1.nnue";
if (nnue::net.load(evalFile))
    std::cout << "info string NNUE loaded: " << evalFile << std::endl;
else
    std::cout << "info string NNUE not found, using classical eval" << std::endl;
```

Optional but recommended — let GUIs/scripts point at the file. In your UCI
handler, advertise the option after the `uci` command:

```cpp
std::cout << "option name EvalFile type string default mihir_v1.nnue" << std::endl;
```

and in the `setoption` handler:

```cpp
if (name == "EvalFile") {
    if (nnue::net.load(value))
        std::cout << "info string NNUE loaded: " << value << std::endl;
    else
        std::cout << "info string failed to load: " << value << std::endl;
}
```

(The rating script relies on `dir=` so the default relative path works, but
`option.EvalFile=/abs/path` is a nice escape hatch.)

## 3. Refresh once per search (in `searchPosition()`)

The GUI sets up positions by making moves on the UCI board *outside* the
search, so the accumulator starts stale. Rebuild it once at the very top of
`searchPosition()`, before iterative deepening begins:

```cpp
Move searchPosition(Board& board, const Limits& lim) {
    if (nnue::net.loaded)
        nnue::acc.refresh(board);      // <-- add this
    ...
}
```

This is the ONLY full rebuild ever needed — no king buckets means no
mid-search refreshes, ever.

## 4. Wrap every make/unmake in the search

In **both** `negamax()` and `quiescence()`, find each place that does:

```cpp
board.makeMove(move);
...
board.unmakeMove(move);
```

and change it to:

```cpp
nnue::acc.push(board, move);   // BEFORE makeMove (needs the pre-move board)
board.makeMove(move);
...
board.unmakeMove(move);
nnue::acc.pop();               // AFTER unmakeMove
```

`push` must come *before* `makeMove` because it inspects the board to see
what's being captured. `pop` costs literally one integer decrement.

Watch for early exits: if a code path does `unmakeMove` and then `return`s
or `continue`s (e.g. legality/repetition checks, beta cutoffs), the `pop()`
must happen on that path too. Easiest audit: search the file for every
`unmakeMove` and confirm a `pop()` follows each one.

If you later add null-move pruning: `makeNullMove()` changes no pieces, so
the accumulator needs no delta — but pushing a plain copy keeps the
stack aligned with ply. Simplest correct pattern:

```cpp
nnue::acc.push_null();   // if you add one: memcpy top -> top+1, ++top
board.makeNullMove();
...
board.unmakeNullMove();
nnue::acc.pop();
```

(You don't have null move today, so nothing to do yet.)

## 5. Route `evaluate()` through the network

Keep your existing PST evaluation as a fallback (rename it
`evaluateClassical`), and make `evaluate()` dispatch:

```cpp
int evaluate(const Board& board) {
    if (nnue::net.loaded)
        return nnue::acc.evaluate(board.sideToMove());
    return evaluateClassical(board);   // your old Michniewski eval
}
```

Both return centipawns from the side-to-move's perspective, so nothing else
in the search changes — quiescence stand-pat, delta pruning margins, TT
scores all keep working.

> Note: `nnue::acc.evaluate` ignores the `board` argument's pieces — it
> trusts the accumulator. That's the whole speed trick, and it's why steps
> 3–4 must be right. If you ever see weird play, temporarily add
> `assert(nnue::acc.check(board));` at the top of `evaluate()` in a debug
> build — it compares the accumulator against a fresh rebuild and will
> pinpoint any desync instantly.

## 6. Build & sanity-check

```
cmake -B build && cmake --build build
cd build && cp /path/to/mihir_v1.nnue .
./engine
uci
position startpos
go movetime 2000
```

You should see `info string NNUE loaded: mihir_v1.nnue` and normal search
output. Expect a somewhat lower nodes-per-second than the PST eval (NNUE
does more work per node) but noticeably stronger play — that's the trade
working as intended.

## 7. Measure the improvement

Before rating it against Stockfish, run the most meaningful match of all —
new engine vs old engine:

```
cutechess-cli \
  -engine name=NNUE      cmd=./engine dir=. \
  -engine name=Classical cmd=./engine dir=. option.EvalFile=nonexistent \
  -each proto=uci tc=10+0.1 \
  -openings file=openings.epd format=epd order=random -repeat \
  -games 200 -concurrency 2 -pgn nnue_vs_classical.pgn
```

(Pointing `EvalFile` at a missing file makes that side fall back to the
classical eval — same binary, two brains.) Then run `rate_engine.sh` for
absolute numbers.
