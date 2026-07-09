# Playing your NNUE engine & measuring its rating — full walkthrough

Everything below assumes you've finished the integration steps in
INTEGRATION.md and have these files in one folder:

```
engine            (your compiled binary; engine.exe on Windows)
mihir_v1.nnue     (downloaded from Colab -- MUST sit next to the binary)
openings.epd
rate_engine.sh
uci_smoke_test.sh
```

---

# Part 0 — One-time setup

## Build the engine
```bash
cmake -B build && cmake --build build
cp mihir_v1.nnue build/            # the engine looks in its working directory
```

## Install the tools
- **Cute Chess (GUI + cutechess-cli)**: https://github.com/cutechess/cutechess/releases
  - Linux: `sudo apt install cutechess` often provides both `cutechess` (GUI)
    and `cutechess-cli`; otherwise use the release download.
  - Windows: the installer includes `cutechess-cli.exe` in the install folder.
- **Stockfish** (the measuring stick): https://stockfishchess.org/download/
  Download the binary for your OS; no installation needed, just note its path.

## Verify the engine is GUI-ready
```bash
chmod +x uci_smoke_test.sh
./uci_smoke_test.sh ./build/engine
```
All PASS + "NNUE network loaded" means you're good. A WARN about NNUE means
the `.nnue` file isn't next to the binary — the engine still works, but with
the old classical eval.

---

# Part 1 — Play against it yourself (Cute Chess GUI)

No script needed: the GUI launches your binary and speaks UCI to it.

1. Open Cute Chess → **Tools → Settings → Engines** tab → **+ (Add)**.
2. Fill in:
   - **Name**: `Mihir NNUE`
   - **Command**: full path to the binary, e.g. `/home/mihir/engine/build/engine`
   - **Working directory**: the folder containing `mihir_v1.nnue`
     (this is how the engine finds its network — don't skip it)
   - **Protocol**: `UCI`
3. Click OK. Then **Game → New** (`Ctrl+N`):
   - Player 1: `Human` (you)
   - Player 2: `Mihir NNUE`
   - Time control: e.g. 5 minutes per side, or "infinite" for casual play.
4. Play! The engine's thinking (depth, score, PV) shows in the engine pane.
   Score is in centipawns from the engine's point of view: +1.50 means it
   believes it's up the equivalent of 1.5 pawns.

Tip: to watch NNUE-you vs classical-you in the GUI, add the SAME binary a
second time named "Mihir Classical" with a different working directory that
does NOT contain the .nnue file — then start an engine-vs-engine game.

---

# Part 2 — Rate it against Stockfish (rate_engine.sh)

## Run it
```bash
chmod +x rate_engine.sh
./rate_engine.sh ./build/engine /path/to/stockfish
```
Keep `openings.epd` in the directory you run from. Four matches of 100 games
each (vs Stockfish limited to 1400 / 1700 / 2000 / 2300) at 10s+0.1s take
roughly 1.5–3 hours total. Edit `GAMES`, `TC`, or `LEVELS` at the top of the
script to trade time for precision.

## Read the output
After each match cutechess-cli prints a block like:

```
Score of Mihir vs SF1700: 41 - 34 - 25  [0.535] 100
...
Elo difference: 24.4 +/- 55.1
```

Meaning: 41 wins, 34 losses, 25 draws → score 53.5% → your engine performed
about 24 Elo above the 1700-limited Stockfish, i.e. roughly **1724 ± 55**.

The math behind that line (worth knowing):

```
score s   = (wins + draws/2) / games
elo diff  = 400 * log10( s / (1 - s) )
rating    = opponent_level + elo diff
```

## Getting a trustworthy number
- **Trust the level nearest 50%.** The formula is most accurate near even
  scores; a 95% blowout vs SF1400 tells you only "well above 1400".
- **Error bars shrink with games.** ±55 at 100 games → roughly ±27 at 400
  games. If two levels bracket 50% (say 58% vs 1700, 41% vs 2000), your
  rating sits between them and both estimates should roughly agree.
- **Caveats to keep you honest**: Stockfish's UCI_Elo is calibrated for
  human-like time controls, and fast games (10s+0.1s) shift things a bit.
  Treat the result as a solid estimate, not an official rating — the
  *relative* numbers (e.g. "NNUE gained +180 over classical") are more
  meaningful than the absolute ones.

## The most important match isn't vs Stockfish
Run new-you vs old-you to directly measure what NNUE bought:

```bash
cutechess-cli \
  -engine name=NNUE      cmd=./build/engine dir=./build \
  -engine name=Classical cmd=./build/engine dir=./build option.EvalFile=none \
  -each proto=uci tc=10+0.1 \
  -openings file=openings.epd format=epd order=random -repeat \
  -games 200 -concurrency 2 -pgn nnue_vs_classical.pgn
```

The reported Elo difference there is the purest measurement of your training
run. Re-run it after every new network to see if a change helped —
this is exactly how serious engines validate every single patch.

## Replaying the games
Every match saves a `.pgn`. Open it in Cute Chess (**File → Open**) to step
through games and watch where the engine shines or stumbles — losses vs the
stronger Stockfish levels are the best source of ideas for what to improve
next.
