#!/usr/bin/env bash
# Pretty-print the most useful bits of a nextpnr-ecp5 JSON timing report.
#
# Usage: timing-report.sh [report.json] [top_n_paths]
# Defaults: build/ulx3s_top_timing.json, 5 paths.

set -euo pipefail

REPORT="${1:-build/ulx3s_top_timing.json}"
TOP_N="${2:-5}"

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

printf '── LUT/FF/RAM utilization ──\n'
jq -r '
    (.utilization // {}) | to_entries |
    map(select(.value.used > 0)) |
    if length == 0 then "  (no utilization data)"
    else .[] | "  \(.key): \(.value.used)/\(.value.available)"
    end
' "$REPORT"
