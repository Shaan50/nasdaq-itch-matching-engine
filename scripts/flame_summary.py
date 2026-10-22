#!/usr/bin/env python3
"""Print the hottest frames (self and inclusive %) of a folded-stack profile."""
import collections
import sys

path, n = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 12
self_t, incl, total = collections.Counter(), collections.Counter(), 0
for line in open(path):
    stack, cnt = line.rsplit(" ", 1)
    cnt = int(cnt)
    total += cnt
    frames = stack.split(";")
    self_t[frames[-1]] += cnt
    for f in set(frames):
        incl[f] += cnt
print(f"{path}: {total} samples")
print("  inclusive:", ", ".join(f"{f} {100 * c / total:.0f}%" for f, c in incl.most_common(n) if 100 * c / total < 99.5))
print("  self     :", ", ".join(f"{f} {100 * c / total:.0f}%" for f, c in self_t.most_common(n)))
