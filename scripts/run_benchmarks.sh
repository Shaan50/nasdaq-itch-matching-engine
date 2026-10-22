#!/usr/bin/env bash
# Reproduce the optimization-log table.
#
# Every replay variant runs on the same RAM-resident, mlock'ed corpus (the first N
# messages of the day). Variants are INTERLEAVED round-robin for R rounds so that drift
# in machine conditions (thermal state, other processes, memory pressure) hits every
# variant equally; the summary reports best and median per variant.
#
#   scripts/run_benchmarks.sh [ITCH_FILE] [N_MESSAGES] [ROUNDS] [VARIANT_FILTER_REGEX]
#
# Defaults: data/12302019.NASDAQ_ITCH50, 40M messages (04:00-10:00, 1.26 GB), 5 rounds.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
file=${1:-$root/data/12302019.NASDAQ_ITCH50}
n=${2:-40000000}
rounds=${3:-5}
filter=${4:-.}
bin=$root/build/release/ib_bench
out=$root/bench/results/replay_$(date +%Y%m%d_%H%M%S).jsonl
mkdir -p "$root/bench/results"

variants=(
  "parse              --mode parse"
  "naive              --mode naive"
  "std-index          --mode std-index"
  "flat-murmur-linear --mode flat --hash murmur --probe linear --table 2097152"
  "flat-ident-linear  --mode flat --hash identity --probe linear"
  "flat-murmur        --mode flat --hash murmur --table 2097152"
  "flat-ident-4M      --mode flat --hash identity --table 2097152"
  "flat-ident         --mode flat --hash identity"
  "batched-ident      --mode batched --hash identity"
  "pipeline-ident     --mode pipeline --hash identity"
)
for ((r = 1; r <= rounds; r++)); do
  for v in "${variants[@]}"; do
    read -r name args <<< "$v"
    [[ $name =~ $filter ]] || continue
    # shellcheck disable=SC2086
    "$bin" "$file" --max "$n" --load --mlock --repeat 1 --json "$out" --label "$name" $args > /dev/null
    printf "round %d %-14s done\n" "$r" "$name"
  done
done
python3 "$root/scripts/summarize_bench.py" "$out"
