#!/usr/bin/env bash
# Profile an ironbook tool with the built-in SIGPROF sampler and render a flamegraph.
#
#   scripts/flamegraph_macos.sh OUT.svg -- BINARY [ARGS...]
#
# Works without root or Instruments (macOS `sample` cannot capture running threads on
# recent releases without elevated privileges). Build with the `profile` preset
# (-O3 -g -fno-omit-frame-pointer). Also works on Linux, where perf is preferred
# (scripts/flamegraph_linux.sh).
set -euo pipefail
out=$1; shift; [[ ${1:-} == -- ]] && shift
root=$(cd "$(dirname "$0")/.." && pwd)
fg=$root/build/FlameGraph
[[ -d $fg ]] || git clone -q --depth 1 https://github.com/brendangregg/FlameGraph "$fg"
bin=$1
prof=$(mktemp -t ironbook).prof
[[ $(uname) == Darwin ]] && dsymutil "$bin" 2>/dev/null || true
IRONBOOK_PROFILE=$prof IRONBOOK_PROFILE_HZ=${HZ:-1999} "$@"
python3 "$root/scripts/symbolize_profile.py" "$bin" "$prof" ${LINES:+--lines} > "${out%.svg}.folded"
"$fg/flamegraph.pl" --title "$(basename "$out" .svg)" --width 1400 --minwidth 0.3 \
  "${out%.svg}.folded" > "$out"
echo "wrote $out"
