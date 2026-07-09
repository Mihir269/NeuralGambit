#!/usr/bin/env bash
# ============================================================================
#  rate_engine.sh -- estimate the engine's Elo by playing Stockfish
# ============================================================================
#
#  IDEA
#  ----
#  Stockfish has a built-in handicap mode: with UCI_LimitStrength=true and
#  UCI_Elo=N it deliberately plays at roughly Elo N (minimum ~1320).  So we
#  run matches against Stockfish locked at several levels and look for the
#  level where the score is about 50% -- that's approximately our rating.
#
#  For any single match, the Elo DIFFERENCE follows from the score s
#  (s = (wins + draws/2) / games):
#
#        diff = 400 * log10( s / (1 - s) )
#        our_rating ~= opponent_elo + diff
#
#  e.g. scoring 65% vs "Stockfish 1700" =>  1700 + 400*log10(.65/.35)
#                                        =  1700 + 108  ~ 1808.
#  cutechess-cli prints this diff for you after every match, with error bars.
#  With 100 games the error is about +-50 Elo; more games = tighter estimate.
#
#  PREREQUISITES
#  -------------
#    * cutechess-cli   (ships with Cute Chess; on Linux often a separate
#                       package -- or download from the Cute Chess releases)
#    * stockfish       (https://stockfishchess.org/download/)
#    * your engine built, and mihir_v1.nnue placed next to the binary
#    * openings.epd in the current directory (variety, else games repeat)
#
#  USAGE
#  -----
#    ./rate_engine.sh /path/to/your/engine /path/to/stockfish
# ============================================================================
set -euo pipefail

ENGINE=${1:?usage: ./rate_engine.sh <your-engine> <stockfish>}
SF=${2:?usage: ./rate_engine.sh <your-engine> <stockfish>}

GAMES=100            # games per Stockfish level (even number: colors swap)
TC="10+0.1"          # 10 seconds + 0.1s increment; fast but fair
LEVELS="1400 1700 2000 2300"   # Stockfish handicap levels to test against
CONCURRENCY=2        # parallel games; keep <= physical cores / 2

ENGINE_DIR=$(cd "$(dirname "$ENGINE")" && pwd)   # so the .nnue file is found

for ELO in $LEVELS; do
    echo "=============================================================="
    echo "  Match: your engine  vs  Stockfish limited to Elo $ELO"
    echo "=============================================================="
    cutechess-cli \
        -engine name=Mihir    cmd="$ENGINE" dir="$ENGINE_DIR" \
        -engine name="SF$ELO" cmd="$SF" \
                option.UCI_LimitStrength=true option.UCI_Elo="$ELO" \
        -each proto=uci tc="$TC" timemargin=200 \
        -openings file=openings.epd format=epd order=random \
        -repeat \
        -games "$GAMES" \
        -concurrency "$CONCURRENCY" \
        -recover \
        -ratinginterval 10 \
        -pgn "match_vs_sf${ELO}.pgn"
    echo
done

echo "Done. Look at each match's final 'Elo difference' line:"
echo "  your rating ~= stockfish_level + reported_difference"
echo "The most reliable estimate comes from the level closest to a 50% score."
echo "PGNs saved as match_vs_sfNNNN.pgn -- open them in Cute Chess to replay."
