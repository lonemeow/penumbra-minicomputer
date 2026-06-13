#!/usr/bin/env bash
# Pretty-print the most useful bits of a nextpnr-ecp5 JSON timing report.
#
# Usage: timing-report.sh [--detail=none|rollup|full] [report.json] [top_n_paths]
# Defaults: --detail=none, build/ulx3s_penumbra1_top_timing.json, 5 paths.
#
# --detail controls per-path expansion of the top N paths (via timing-path.py):
#   none   — path endpoints only (default)
#   rollup — per-module rollup, so you can see which subsystem owns each path
#   full   — full hop-by-hop trace (every LUT and route on the path)
# Bare --detail is an alias for rollup; legacy 0/1 map to none/rollup.

set -euo pipefail

DETAIL=none
POS=()
for arg in "$@"; do
    case "$arg" in
        --detail)   DETAIL=rollup ;;            # bare flag = rollup (back-compat)
        --detail=*) DETAIL="${arg#--detail=}" ;;
        *)          POS+=("$arg") ;;
    esac
done

# Accept legacy numeric forms: 0=none, 1=rollup.
case "$DETAIL" in
    0) DETAIL=none ;;
    1) DETAIL=rollup ;;
esac
case "$DETAIL" in
    none|rollup|full) ;;
    *) echo "error: --detail must be none, rollup, or full (got '$DETAIL')" >&2; exit 1 ;;
esac

REPORT="${POS[0]:-build/ulx3s_penumbra1_top_timing.json}"
TOP_N="${POS[1]:-5}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ ! -f "$REPORT" ]]; then
    echo "error: $REPORT not found — run 'make fpga' first" >&2
    exit 1
fi

if ! command -v jq >/dev/null 2>&1; then
    echo "error: jq is required but not installed" >&2
    exit 1
fi

printf '═══ nextpnr timing report: %s ═══\n\n' "$REPORT"

printf '── fmax per clock domain ──\n'
jq -r '
    (.fmax // {}) | to_entries |
    if length == 0 then "(no fmax data)"
    else .[] |
        "  \(.key): \(.value.achieved | . * 100 | round / 100) MHz achieved" +
        (if .value.constraint then " (constraint \(.value.constraint | . * 100 | round / 100) MHz)" else "" end)
    end
' "$REPORT"
printf '\n'

printf '── top %s critical paths ──\n' "$TOP_N"
jq -r --argjson n "$TOP_N" '
    def round2: . * 100 | round / 100;
    (.critical_paths // []) as $cps |
    if ($cps | length) == 0 then "  (no critical_paths in report — check --detailed-timing-report flag)"
    else
        $cps[:$n] | to_entries[] |
        .key as $i | .value as $p |
        ($p.path | map(.delay // 0) | add) as $total |
        ($p.path[0]) as $src |
        ($p.path[-1]) as $dst |
        "[\($i + 1)] \($p.from) → \($p.to)  " +
        "total: \($total | round2) ns  (\($p.path | length) hops)",
        "    from: \($src.from.cell // "?").\($src.from.port // "?")",
        "    to:   \($dst.to.cell   // "?").\($dst.to.port   // "?")",
        ""
    end
' "$REPORT"

if [[ "$DETAIL" != "none" ]]; then
    if [[ "$DETAIL" == "full" ]]; then
        hops_arg=""        # full hop-by-hop trace
        printf '── per-path full hop traces (top %s) ──\n\n' "$TOP_N"
    else
        hops_arg="--no-hops"   # module rollup only
        printf '── per-path module rollups (top %s) ──\n\n' "$TOP_N"
    fi
    # Only iterate as many paths as the report actually contains.
    available="$(jq '.critical_paths // [] | length' "$REPORT")"
    last=$(( available < TOP_N ? available : TOP_N ))
    for i in $(seq 1 "$last"); do
        python3 "$SCRIPT_DIR/timing-path.py" "$REPORT" "$i" $hops_arg
    done
fi

printf '── LUT/FF/RAM utilization ──\n'
jq -r '
    (.utilization // {}) | to_entries |
    map(select(.value.used > 0)) |
    if length == 0 then "  (no utilization data)"
    else .[] | "  \(.key): \(.value.used)/\(.value.available)"
    end
' "$REPORT"
