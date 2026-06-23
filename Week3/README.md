# Week 3

This week, you're gonna start working on Chess. To start with you're gonna work on bots to solve chess puzzles.

And, you're gonna switch to C++ and we suggest you use some OOPS techniques and make class of the Engine solver (that'll be practice for OOPS)

I'm also linking a list of puzzles we compiled for you, which is obviously not the exhaustive list, you're appreciated to explore and get more puzzles and share it on the group!

Your enthusiasm to make your bot faster is what drives you

You can use this repo for the Chess Library (not compulsory to use the same, there is a faster library but a bit unintuitive): [Chess Library by Disservin](https://github.com/Disservin/chess-library/)

## What the puzzle files contain

The repository has two puzzle formats:

- The `.txt` files are human-readable collections. Each puzzle usually appears as a title line, then a FEN line, then the sample solution on the next line.
- The `.json` files are machine-friendly maps from FEN to solution string. They are easier to load directly from code.

The important part for a bot is the FEN position. The title and the sample solution are there to help you read, test, and verify your solver.

## How the solver works

The starter solver I added in [solver.cpp](solver.cpp) does three things:

1. Loads puzzles from either `.txt` or `.json` files.
2. Builds a `chess::Board` from each FEN string.
3. Searches for a forced mate by trying legal moves recursively.

The code is intentionally simple and heavily commented so you can follow the flow.

## Build idea

The Disservin library is header-only, so the solver only needs the include path to the library headers. A typical compile command looks like this:

```bash
g++ -std=c++17 -O3 -I/path/to/chess-library/include solver.cpp -o solver
```

Run it by passing the mate depth first, then the file or folder to solve:

```bash
solver 2 mate_in_2.json 3 mate_in_3.json 4 mate_in_4.json
```

If you already have the library available in your environment, only the include path may need to change.

## Fixing the `chess.hpp` include issue

The current workspace does not contain the Disservin library, so `#include "chess.hpp"` cannot resolve until you add it yourself.

Use one of these fixes:

1. Put the library headers in a folder such as `third_party/chess-library/include` and compile with `-Ithird_party/chess-library/include`.
2. In VS Code, add that same folder to your C++ `includePath` setting so IntelliSense can find `chess.hpp`.
3. If you cloned the library elsewhere, point the include path to that folder instead.

For this solver, the simplest compile command is still:

```bash
g++ -std=c++17 -O3 -Ithird_party/chess-library/include solver.cpp -o solver
```