#!/usr/bin/env python3
"""Turn an ironbook SamplingProfiler capture into folded stacks for flamegraph.pl.

    scripts/symbolize_profile.py BINARY PROFILE [--lines] > out.folded

Addresses are symbolized with inline frames recovered from DWARF:
  macOS : atos -i (run `dsymutil BINARY` first so atos finds the debug map)
  Linux : addr2line -i -f -C
Template arguments and parameter lists are stripped so the graph stays readable;
--lines appends file:line to leaf frames.
"""
import collections
import os
import re
import subprocess
import sys
import tempfile


def simplify(name: str) -> str:
    name = re.sub(r"\s*\(in [^)]*\)", "", name)
    loc = re.search(r"\(([^()]*:\d+)\)\s*$", name)
    name = re.sub(r"\s*\([^()]*:\d+\)\s*$", "", name)
    # Drop nested template argument lists and parameter lists.
    prev = None
    while prev != name:
        prev = name
        name = re.sub(r"<[^<>]*>", "", name)
    name = re.sub(r"\([^()]*\)", "", name)
    name = re.sub(r"\([^()]*\)", "", name)
    name = re.sub(r"\s+const\s*$", "", name.strip())  # trailing cv-qualifier of a member fn
    name = name.split(" ")[-1] if name else "??"
    name = name.replace("(anonymous namespace)::", "").replace("{lambda}", "lambda")
    return name, (loc.group(1) if loc else None)


def symbolize_macos(binary, load, addrs):
    with tempfile.NamedTemporaryFile("w", delete=False) as f:
        f.write("\n".join(hex(a) for a in addrs))
        path = f.name
    out = subprocess.run(["atos", "-o", binary, "-l", hex(load), "-i", "-f", path],
                         capture_output=True, text=True).stdout
    os.unlink(path)
    # With -i, atos prints each address's inline chain on consecutive lines and separates
    # addresses with a blank line.
    groups = [g for g in out.split("\n\n")]
    groups = [g.strip("\n") for g in groups]
    if groups and groups[-1] == "":
        groups.pop()
    if len(groups) != len(addrs):  # fall back to one call per address
        groups = [subprocess.run(["atos", "-o", binary, "-l", hex(load), "-i", hex(a)],
                                 capture_output=True, text=True).stdout.strip("\n") for a in addrs]
    return [" ;; ".join(g.splitlines()) for g in groups]


def symbolize_linux(binary, load, addrs):
    res = []
    for a in addrs:
        out = subprocess.run(["addr2line", "-i", "-f", "-C", "-e", binary, hex(a - load)],
                             capture_output=True, text=True).stdout.splitlines()
        frames = [f"{out[i]} ({os.path.basename(out[i + 1])})" for i in range(0, len(out) - 1, 2)]
        res.append(" ;; ".join(frames) if frames else hex(a))
    return res


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    binary, prof = sys.argv[1], sys.argv[2]
    with_lines = "--lines" in sys.argv
    with open(prof) as f:
        header = f.readline()
        load = int(re.search(r"load=(0x[0-9a-f]+)", header).group(1), 16)
        samples = [[int(x, 16) for x in line.split()] for line in f if line.strip()]

    # Return addresses point after the call; step back one byte to land inside it.
    adj = [[s[0]] + [a - 1 for a in s[1:]] for s in samples]
    uniq = sorted({a for s in adj for a in s})
    lines = (symbolize_macos if sys.platform == "darwin" else symbolize_linux)(binary, load, uniq)
    table = {}
    for a, text in zip(uniq, lines):
        frames = []
        for raw in text.split(" ;; "):
            raw = raw.strip()
            if re.fullmatch(r"(0x)?[0-9a-fA-F]+", raw):
                frames.append(("[system]", None))
            else:
                frames.append(simplify(raw))
        table[a] = frames  # innermost first

    folded = collections.Counter()
    for s in adj:
        stack = []
        for i, a in enumerate(s):
            for j, (fn, loc) in enumerate(table.get(a, [("??", None)])):
                label = fn + (f" [{loc}]" if with_lines and i == 0 and j == 0 and loc else "")
                stack.append(label)
        stack.reverse()  # root first
        dedup = []
        for fr in stack:
            if not dedup or dedup[-1] != fr:
                dedup.append(fr)
        # Collapse leading unresolved runtime frames (dyld start etc.).
        while len(dedup) > 1 and dedup[0] == "[system]":
            dedup.pop(0)
        folded[";".join(dedup)] += 1
    for stack, n in folded.most_common():
        print(f"{stack} {n}")


if __name__ == "__main__":
    main()
