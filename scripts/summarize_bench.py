#!/usr/bin/env python3
"""Summarize ib_bench JSONL results: best / median throughput per variant label."""
import collections
import json
import statistics
import sys

runs = collections.defaultdict(list)
order = []
for line in open(sys.argv[1]):
    r = json.loads(line)
    label = r.get("label") or r["mode"]
    if label not in runs:
        order.append(label)
    runs[label].append(r)

base = None
print("| variant | best (M msg/s) | median (M msg/s) | ns/msg (best) | runs |")
print("|---|---:|---:|---:|---:|")
for label in order:
    rates = [r["msgs_per_sec"] for r in runs[label]]
    best, med = max(rates), statistics.median(rates)
    print(f"| {label} | {best / 1e6:.2f} | {med / 1e6:.2f} | {1e9 / best:.1f} | {len(rates)} |")
