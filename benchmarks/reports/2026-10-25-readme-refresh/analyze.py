#!/usr/bin/env python3
"""Summarise run-all.sh's output directory as Markdown.

bench.sh rounds: per round, the best of the RUNS runs of each workload (what
bench.sh prints); then the median and range of those per-round bests.
Actors: msg/s per mode and worker count, median and range over the rounds,
and each mode's peak (best median over the worker counts).

Usage: analyze.py OUT_DIR
"""
import collections
import pathlib
import re
import statistics
import sys

out = pathlib.Path(sys.argv[1])

print("## bench.sh workloads (ms, best of 3 per round; median [min-max] of rounds)\n")
rows = [l.split("\t") for l in (out / "bench" / "bench-raw.tsv").read_text().splitlines()[1:]]
bad = [r for r in rows if r[5] != "ok"]
best = collections.defaultdict(dict)
for rnd, w, rt, _, ms, st, _ in rows:
    if st != "ok":
        continue
    key = (w, rt)
    best[key][rnd] = min(best[key].get(rnd, float("inf")), float(ms))
print("| workload | protoclj | bb | ratio (medians) | protoclj / bb per round |")
print("|---|---:|---:|---:|---|")
workloads = list(dict.fromkeys(r[1] for r in rows))
for w in workloads:
    p, b = best[(w, "protoclj")], best[(w, "bb")]
    pm, bm = statistics.median(p.values()), statistics.median(b.values())
    per = ", ".join(f"{p[r] / b[r]:.2f}" for r in sorted(p) if r in b)
    print(f"| {w} | {pm:.0f} [{min(p.values()):.0f}-{max(p.values()):.0f}] | "
          f"{bm:.0f} [{min(b.values()):.0f}-{max(b.values()):.0f}] | {pm / bm:.2f}x | {per} |")
print(f"\nRuns checked: {len(rows)}, failed: {len(bad)}.")
fact = out / "bench" / "factorial-100.txt"
if fact.exists():
    print("\n```\n" + fact.read_text().strip() + "\n```")

st = out / "startup" / "startup-summary.txt"
if st.exists():
    print("\n## Start-up (3 passes x 50 runs, interleaved)\n\n```\n" + st.read_text().strip() + "\n```")

sc = out / "showcase" / "summary.txt"
if sc.exists():
    print("\n## Showcase (run.sh of 2026-10-23-showcase)\n\n```\n" + sc.read_text().strip() + "\n```")

rates = collections.defaultdict(list)
failed = 0
for f in sorted(out.glob("actors-r*.txt")):
    for line in f.read_text().splitlines():
        if "FAILED" in line:
            failed += 1
            continue
        m = re.match(r"^(single|fan-out|MPSC|MPMC)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)", line)
        if m:
            rates[(m.group(1), int(m.group(2)))].append(int(m.group(5)))
if rates:
    print("\n## Actors (msg/s; median [min-max] over rounds)\n")
    workers = sorted({w for _, w in rates})
    print("| mode | " + " | ".join(f"w={w}" for w in workers) + " | peak (median) |")
    print("|---|" + "---:|" * (len(workers) + 1))
    for mode in ("single", "fan-out", "MPSC", "MPMC"):
        cells, peak = [], (0, 0)
        for w in workers:
            v = rates.get((mode, w), [])
            if v:
                med = statistics.median(v)
                peak = max(peak, (med, w))
                cells.append(f"{med:,.0f} [{min(v):,}-{max(v):,}]")
            else:
                cells.append("-")
        print(f"| {mode} | " + " | ".join(cells) + f" | {peak[0]:,.0f} (w={peak[1]}) |")
    print(f"\nFailed actor runs: {failed}.")

load = out / "load.txt"
if load.exists():
    print("\n## Load\n\n```\n" + load.read_text().strip() + "\n```")
