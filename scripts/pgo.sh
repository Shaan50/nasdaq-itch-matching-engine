#!/usr/bin/env bash
# Profile-guided optimization (clang instrumentation PGO) + LTO for the replay hot path.
#
#   scripts/pgo.sh [ITCH_FILE]
#
# 1. build an instrumented ib_bench, 2. train it on the first 10M messages of the day
# (all replay modes), 3. merge the profile, 4. rebuild with -fprofile-instr-use + LTO
# into build/pgo. Compare with scripts/run_benchmarks.sh-style interleaved runs.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
file=${1:-$root/data/12302019.NASDAQ_ITCH50}
profdata=$(xcrun --find llvm-profdata 2>/dev/null || command -v llvm-profdata)
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DIRONBOOK_BUILD_TESTS=OFF -DIRONBOOK_BUILD_BENCHMARKS=OFF)

cmake -S "$root" -B "$root/build/pgo-gen" "${common[@]}" -DIRONBOOK_PGO=generate > /dev/null
cmake --build "$root/build/pgo-gen" --target ib_bench > /dev/null
rm -rf "$root/build/pgo-profiles" && mkdir -p "$root/build/pgo-profiles"
for mode in "flat --hash identity" "batched --hash identity" "pipeline --hash identity"; do
  # shellcheck disable=SC2086
  LLVM_PROFILE_FILE="$root/build/pgo-profiles/%p.profraw" \
    "$root/build/pgo-gen/ib_bench" "$file" --max 10000000 --load --mode $mode > /dev/null
done
"$profdata" merge -o "$root/build/ironbook.profdata" "$root"/build/pgo-profiles/*.profraw

cmake -S "$root" -B "$root/build/pgo" "${common[@]}" -DIRONBOOK_PGO="$root/build/ironbook.profdata" \
  -DIRONBOOK_LTO=ON > /dev/null
cmake --build "$root/build/pgo" > /dev/null
echo "PGO+LTO build ready: build/pgo/ib_bench"
