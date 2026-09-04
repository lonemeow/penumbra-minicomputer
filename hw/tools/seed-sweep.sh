#!/bin/sh
# Parallel nextpnr placement-seed sweep for one FPGA top: place the
# same synthesis result under every seed and report each seed's
# achieved fmax on every clock domain, so a stale pinned
# DEFAULT_SEED_<top> can be re-picked after a design change.
#
# Every domain, because a seed is only usable if all of them close. A
# machine top carries several — CPU, SDRAM, USB, video — and the one
# that fails after a change is not necessarily the one the change
# touched: placement pressure moves whichever domain was closest to its
# constraint, not the one whose logic grew.
#
# The synthesis JSON is built once up front; the seeds then fan out as
# independent make targets (build/<top>.s<seed>.config — see the
# seed-sweep pattern rule in the Makefile) under one `make -jN`, so
# the sweep parallelizes across cores.  Already-placed seeds are left
# alone on a re-run: adding seeds to a finished sweep only builds the
# new ones.
#
# Each seed's nextpnr output lands in its own build/<top>.s<seed>.log
# rather than the shared stdout, so a parallel sweep prints only the
# summary and every run's detail — utilisation, congestion, critical
# path — survives for inspection afterwards.
#
# Usage:
#   hw/tools/seed-sweep.sh BOARD=ulx3s CORE=penumbra2 \
#       [SEEDS="0 1 2 ..."] [JOBS=4] [PNR_TIMEOUT=900]
#
# Every other argument passes through to make.  JOBS is bounded by
# memory, not cores — each nextpnr run holds the whole design.
# PNR_TIMEOUT bounds one seed (see the pattern rule in the Makefile).
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

# -k: the seeds are independent placements, so one that fails or trips
# PNR_TIMEOUT must not stop the rest — a dead seed is a result the summary
# reports, not an error worth abandoning the sweep for.  The sweep's own exit
# status follows make's, so a failed seed still surfaces to the caller.
make -k -j"$JOBS" $ARGS $targets || sweep_status=$?

# Cell counts come out of packing, which runs before placement, so
# every seed reports the same utilisation — print it once, from
# whichever seed log is available.
for s in $SEEDS; do
    log="build/$TOP.s${s}.log"
    if [ -f "$log" ] && grep -q 'Device utilisation:' "$log"; then
        echo "── device utilisation ($TOP) ──"
        sed -n '/Device utilisation:/,/^Info: Placed/p' "$log" |
            grep -E 'TRELLIS_COMB|TRELLIS_FF|TRELLIS_IO|DP16KD|EHXPLLL' || true
        break
    fi
done

echo "── fmax per seed ($TOP) ──"
for s in $SEEDS; do
    rpt="build/$TOP.s${s}_timing.json"
    log="build/$TOP.s${s}.log"
    if [ -f "$rpt" ]; then
        # One line per seed: every domain, and a verdict that is FAIL if
        # any single one misses. Picking a seed on the CPU clock alone
        # can select a placement that breaks a peripheral.
        hw/tools/timing-report.sh "$rpt" 0 2>/dev/null |
        awk -v seed="$s" '
            /achieved \(constraint/ {
                name = $1; sub(/^\$glbnet\$/, "", name); sub(/:$/, "", name)
                got = $2 + 0
                for (i = 1; i <= NF; i++) if ($i == "(constraint") want = $(i+1) + 0
                if (got + 0 < want + 0) { bad = bad " " name; nfail++ }
                out = out sprintf("  %s %.2f/%g", name, got, want)
            }
            END {
                printf "seed %s: %s   %s%s\n", seed,
                       (nfail ? "FAIL" : "pass"), out,
                       (nfail ? "   [short:" bad " ]" : "")
            }'
    elif [ -f "$log" ] && grep -q 'TIMED OUT' "$log"; then
        echo "seed $s:  timed out — placement unusable for this netlist"
    else
        echo "seed $s:  no report ($rpt missing)"
    fi
done

exit "${sweep_status:-0}"
