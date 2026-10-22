#!/usr/bin/env python3
"""Render README charts from ib_bench / engine_sim JSON results.

    scripts/plot_results.py ladder   RESULTS.jsonl  OUT.svg   # throughput per variant
    scripts/plot_results.py latency  LATENCY.jsonl  OUT.svg   # per-message-type percentile curves
    scripts/plot_results.py engine   ENGINE.jsonl   OUT.svg   # matching-engine per-op percentiles

Colors follow a validated categorical palette (fixed slot order, never cycled);
text uses ink colors, never the series color.
"""
import collections
import json
import statistics
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FuncFormatter  # noqa: E402

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
MUTED = "#a9a8a3"

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": GRID, "axes.labelcolor": INK_2, "xtick.color": INK_2, "ytick.color": INK_2,
    "text.color": INK, "font.size": 10, "axes.titlesize": 12, "axes.titleweight": "semibold",
    "axes.spines.top": False, "axes.spines.right": False, "svg.fonttype": "none",
})


def load(path):
    return [json.loads(line) for line in open(path) if line.strip()]


def ladder(path, out):
    runs = collections.OrderedDict()
    for r in load(path):
        runs.setdefault(r.get("label") or r["mode"], []).append(r["msgs_per_sec"])
    labels = [k for k in runs if k != "parse"]
    best = [max(runs[k]) / 1e6 for k in labels]
    med = [statistics.median(runs[k]) / 1e6 for k in labels]
    fig, ax = plt.subplots(figsize=(8.2, 0.42 * len(labels) + 1.3))
    y = list(range(len(labels)))[::-1]
    base = best[0]
    colors = [MUTED if lab in ("naive",) or "linear" in lab else SERIES[0] for lab in labels]
    ax.barh(y, best, height=0.62, color=colors, edgecolor=SURFACE, linewidth=2)
    ax.scatter(med, y, s=18, color=INK_2, zorder=3, label="median of runs (dot)")
    for yi, b, lab in zip(y, best, labels):
        ax.text(b + max(best) * 0.01, yi, f"{b:.1f}M  ({b / base:.1f}x)", va="center", color=INK, fontsize=9)
    ax.set_yticks(y, labels)
    ax.set_xlabel("messages / second (millions, best of interleaved runs)")
    ax.set_xlim(0, max(best) * 1.22)
    ax.grid(axis="x", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    ax.tick_params(axis="y", length=0)
    ax.legend(frameon=False, loc="upper right", fontsize=9)
    ax.set_title("Book replay throughput, NASDAQ ITCH 2019-12-30 (first 40M msgs)", loc="left")
    fig.tight_layout()
    fig.savefig(out)


QUANTILES = [0.5, 0.75, 0.9, 0.95, 0.99, 0.995, 0.999, 0.9995, 0.9999]


def quantile_from_buckets(buckets, q):
    total = sum(c for _, c in buckets)
    target = q * total
    seen = 0
    for ub, c in buckets:
        seen += c
        if seen >= target:
            return ub
    return buckets[-1][0]


def fmt_ns(v, _=None):
    if v >= 1e6:
        return f"{v / 1e6:g} ms"
    if v >= 1e3:
        return f"{v / 1e3:g} µs"
    return f"{v:g} ns"


def percentile_axes(ax):
    xs = list(range(len(QUANTILES)))
    ax.set_xticks(xs, [f"p{q * 100:g}" for q in QUANTILES])
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(FuncFormatter(fmt_ns))
    ax.grid(axis="y", color=GRID, linewidth=0.8, which="major")
    ax.set_axisbelow(True)
    return xs


def latency(path, out):
    rows = load(path)
    r = next((x for x in rows if x.get("label") == "optimized"), rows[0])
    lat = r["latency"]
    fig, ax = plt.subplots(figsize=(8.2, 4.4))
    xs = percentile_axes(ax)
    names = [n for n in ("add", "delete", "replace", "execute", "cancel") if lat.get(n, {}).get("n")]
    floor = r["ns_per_tick"]  # values below one clock tick are drawn at the tick
    for i, n in enumerate(names):
        ys = [max(floor, quantile_from_buckets(lat[n]["buckets"], q)) for q in QUANTILES]
        ax.plot(xs, ys, color=SERIES[i], linewidth=2, marker="o", markersize=4, label=f"{n} (n={lat[n]['n'] / 1e6:.1f}M)")
    ax.set_ylabel("service time per message (log)")
    ax.set_xlabel(f"percentile  ·  clock {r['clock']}, {floor:.1f} ns resolution (values at the floor are ≤ 1 tick)",
                  fontsize=8.5)
    ax.set_title("Per-message book update latency by message type (40M real messages)", loc="left")
    ax.legend(frameon=False, ncol=2, loc="upper left")
    fig.tight_layout()
    fig.savefig(out)


def engine(path, out):
    rows = load(path)
    fig, ax = plt.subplots(figsize=(8.2, 4.4))
    xs = list(range(3))
    keys, names = ["p50", "p99", "p999"], ["p50", "p99", "p99.9"]
    ops = ["passive", "aggressive", "ioc", "cancel"]
    r = rows[-1]
    width = 0.2
    for i, op in enumerate(ops):
        if op not in r:
            continue
        ax.bar([x + (i - 1.5) * width for x in xs], [max(41.67, r[op][k]) for k in keys], width=width * 0.92,
               color=SERIES[i], edgecolor=SURFACE, linewidth=1, label=op)
    ax.set_xticks(xs, names)
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(FuncFormatter(fmt_ns))
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    ax.set_ylim(top=ax.get_ylim()[1] * 4)  # headroom for the legend
    ax.set_ylabel("time per operation (log)")
    ax.set_xlabel("percentile  ·  41.7 ns clock resolution; bars at the floor are ≤ 1 tick", fontsize=8.5)
    ax.set_title(f"Matching engine latency by operation ({r['ops_per_sec'] / 1e6:.1f}M ops/s)", loc="left")
    ax.legend(frameon=False, ncol=4, loc="upper left")
    fig.tight_layout()
    fig.savefig(out)


if __name__ == "__main__":
    {"ladder": ladder, "latency": latency, "engine": engine}[sys.argv[1]](sys.argv[2], sys.argv[3])
