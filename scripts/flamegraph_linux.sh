#!/usr/bin/env bash
# Record with Linux perf (frame-pointer call graphs) and render a flamegraph SVG.
#
#   scripts/flamegraph_linux.sh OUT.svg -- CMD [ARGS...]
set -euo pipefail
out=$1; shift; [[ ${1:-} == -- ]] && shift
root=$(cd "$(dirname "$0")/.." && pwd)
fg=$root/build/FlameGraph
[[ -d $fg ]] || git clone -q --depth 1 https://github.com/brendangregg/FlameGraph "$fg"
tmp=$(mktemp -d)
perf record -F 2999 --call-graph=fp -o "$tmp/perf.data" -- "$@"
perf script -i "$tmp/perf.data" | "$fg/stackcollapse-perf.pl" > "$tmp/folded.txt"
"$fg/flamegraph.pl" --title "$(basename "$out" .svg)" --width 1400 "$tmp/folded.txt" > "$out"
cp "$tmp/folded.txt" "${out%.svg}.folded"
# Hardware counters for the optimization log (cache / TLB / branch behaviour).
perf stat -e cycles,instructions,cache-misses,LLC-load-misses,dTLB-load-misses,branch-misses -- "$@" 2>&1 | tail -15
