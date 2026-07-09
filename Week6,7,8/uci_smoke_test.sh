#!/usr/bin/env bash
# ============================================================================
#  uci_smoke_test.sh -- verify the engine is ready for Cute Chess / matches
# ============================================================================
#  Pipes a scripted UCI conversation into the engine and checks the replies.
#  Run this BEFORE adding the engine to any GUI: if this passes, the GUI
#  will work; if it fails, the output tells you what's wrong.
#
#  Usage:   ./uci_smoke_test.sh /path/to/engine
# ============================================================================
set -euo pipefail
ENGINE=${1:?usage: ./uci_smoke_test.sh <path-to-engine>}

ENGINE_DIR=$(cd "$(dirname "$ENGINE")" && pwd)
cd "$ENGINE_DIR"   # engine looks for mihir_v1.nnue in its working directory

echo "--- sending: uci / isready / position startpos / go movetime 2000 ---"
OUT=$(printf 'uci\nisready\nposition startpos\ngo movetime 2000\nquit\n' \
      | timeout 20 "$ENGINE")

echo "$OUT" | tail -15
echo "----------------------------------------------------------------------"

ok=true
echo "$OUT" | grep -q "^uciok"        && echo "PASS  speaks UCI (uciok)"        || { echo "FAIL  no uciok";        ok=false; }
echo "$OUT" | grep -q "^readyok"      && echo "PASS  responds to isready"       || { echo "FAIL  no readyok";      ok=false; }
echo "$OUT" | grep -q "^bestmove"     && echo "PASS  search returns a bestmove" || { echo "FAIL  no bestmove";     ok=false; }
echo "$OUT" | grep -q "NNUE loaded"   && echo "PASS  NNUE network loaded"       || echo "WARN  NNUE NOT loaded -- is mihir_v1.nnue next to the binary? (engine will use classical eval)"

$ok && echo ">>> Engine is ready for Cute Chess and cutechess-cli." \
    || { echo ">>> Fix the FAILs above before using a GUI."; exit 1; }
