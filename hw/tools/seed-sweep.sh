#!/bin/sh
# Parallel nextpnr placement-seed sweep for one FPGA top: place the
# same synthesis result under every seed and report each seed's
# achieved CPU-clock fmax, so a stale pinned DEFAULT_SEED_<top> can be
# re-picked after a design change.
#
# The synthesis JSON is built once up front; the seeds then fan out as
# independent make targets (build/<top>.s<seed>.config — see the
# seed-sweep pattern rule in the Makefile) under one `make -jN`, so
# the sweep parallelizes across cores.  Already-placed seeds are left
# alone on a re-run: adding seeds to a finished sweep only builds the
# new ones.
#
# Usage:
#   hw/tools/seed-sweep.sh BOARD=ulx3s CORE=penumbra2 \
#       [SEEDS="0 1 2 ..."] [JOBS=4]
#
# Every other argument passes through to make.  JOBS is bounded by
# memory, not cores — each nextpnr run holds the whole design.
set -eu

SEEDS=""
JOBS=4
ARGS=""
for a in "$@"; do
    case "$a" in
        SEEDS=*) SEEDS="${a#SEEDS=}" ;;
        JOBS=*)  JOBS="${a#JOBS=}" ;;
        *)       ARGS="$ARGS $a" ;;
    esac
done
[ -n "$SEEDS" ] || SEEDS="0 1 2 3 4 5 6 7 8 9"

TOP=$(make -s print-top $ARGS)
echo "── seed sweep: $TOP, seeds: $SEEDS, $JOBS jobs ──"

# The shared synthesis JSON first, alone — the parallel fan-out must
# not race several yosys runs over the same output file.
make $ARGS "build/$TOP.json"

targets=""
for s in $SEEDS; do
    targets="$targets build/$TOP.s$s.config"
done
make -j"$JOBS" $ARGS $targets

echo "── fmax per seed ($TOP) ──"
for s in $SEEDS; do
    rpt="build/$TOP.s${s}_timing.json"
    if [ -f "$rpt" ]; then
        line=$(hw/tools/timing-report.sh "$rpt" 0 2>/dev/null |
               grep -F '$glbnet$clk:' | head -1 || true)
        echo "seed $s:${line:-  no fmax line in $rpt}"
    else
        echo "seed $s:  no report ($rpt missing)"
    fi
done
