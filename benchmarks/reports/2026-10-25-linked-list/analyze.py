#!/usr/bin/env python3
"""Summarises the raw results run.sh writes (usage: analyze.py OUT)."""
import collections
import pathlib
import re
import statistics as st
import sys

OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "out")
VARIANTS = ["main", "A", "B"]
CASE_NAMES = {
    1: "list walk (rest)", 2: "vector (seq v) walk", 3: "reduce + over a list",
    4: "cons-build then count", 5: "sequential nth (vector)", 50: "  control for 5",
    6: "random nth (vector)", 60: "  control for 6", 7: "empty? + first (list)",
    8: "count of rest (list)", 9: "count of cons (list)", 10: "(vec list)",
}


def med(xs):
    return st.median(xs) if xs else float("nan")


def cases():
    path = OUT / "cases.txt"
    if not path.exists():
        return
    pc = {}
    bb = collections.defaultdict(list)
    checks = collections.defaultdict(set)
    for line in path.read_text().splitlines():
        if line.startswith("PC"):
            m = re.match(r"PC (\w+) rep=(\d+) RESULT (\d+) (\d+) (\w+) (\S+) (\S+) (\d+)", line)
            if not m:
                print("unparsed:", line[:120])
                continue
            v, rep, c, n, mode, _chk, res, ns = m.groups()
            kv = dict(re.findall(r"([\w-]+)=(\d+)", line.split("|", 1)[1]))
            d = dict(ns=int(ns), res=res,
                     cells=int(kv.get("reclaimed-total", 0)) + int(kv.get("live-last", 0)),
                     rss=int(kv.get("maxrss", 0)),
                     cycles=int(kv.get("cycles", 0)), instructions=int(kv.get("instructions", 0)))
            pc[(v, int(c), int(n), mode, int(rep))] = d
            if mode == "work":
                checks[(int(c), int(n))].add(res)
        elif line.startswith("BB"):
            m = re.search(r"RESULT (\d+) (\d+) work \S+ (\S+) (\d+)", line)
            if m:
                bb[(int(m.group(1)), int(m.group(2)))].append(int(m.group(4)))
                checks[(int(m.group(1)), int(m.group(2)))].add(m.group(3))
    reps = sorted({k[4] for k in pc})
    ns_all = sorted({k[2] for k in pc})

    def per(v, c, n, key):
        vals = []
        for r in reps:
            w, s = pc.get((v, c, n, "work", r)), pc.get((v, c, n, "setup", r))
            if w and s:
                vals.append((w[key] - s[key]) / n)
        return med(vals)

    def wall(v, c, n):
        return med([pc[(v, c, n, "work", r)]["ns"] / n for r in reps if (v, c, n, "work", r) in pc])

    print(f"## list-cost cases ({len(reps)} interleaved rounds)\n")
    print("ns per element: the case's own timer (work run). cycles per element: perf stat,")
    print("work run minus setup run, every thread (the collector's included).")
    for n in ns_all:
        print(f"\n### n = {n}\n")
        print("| case | main ns | A ns | B ns | bb ns | main cyc | A cyc | B cyc | B vs main (cyc) | B vs A (cyc) | cells/el main / B |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for c in CASE_NAMES:
            if not any((v, c, n, "work", r) in pc for v in VARIANTS for r in reps):
                continue
            cyc = {v: per(v, c, n, "cycles") for v in VARIANTS}
            ns = {v: wall(v, c, n) for v in VARIANTS}
            b = med([x / n for x in bb[(c, n)]])
            vs = lambda x, y: f"{100 * (x / y - 1):+.0f} %" if y and y == y and y > 0 else "n/a"
            print(f"| {c} {CASE_NAMES[c]} | {ns['main']:.0f} | {ns['A']:.0f} | {ns['B']:.0f} | {b:.0f} | "
                  f"{cyc['main']:.0f} | {cyc['A']:.0f} | {cyc['B']:.0f} | {vs(cyc['B'], cyc['main'])} | "
                  f"{vs(cyc['B'], cyc['A'])} | {per('main', c, n, 'cells'):.1f} / {per('B', c, n, 'cells'):.1f} |")
        rss = {v: med([pc[(v, 1, n, 'setup', r)]['rss'] / 1024 for r in reps if (v, 1, n, 'setup', r) in pc])
               for v in VARIANTS}
        print(f"\nPeak RSS of building the list (case 1 setup run), MB: "
              + ", ".join(f"{v} {rss[v]:.0f}" for v in VARIANTS))
        scyc = {v: med([pc[(v, 1, n, 'setup', r)]['cycles'] / 1e6 for r in reps if (v, 1, n, 'setup', r) in pc])
                for v in VARIANTS}
        print("Cycles of the whole setup run (start-up plus (apply list (range n))), M: "
              + ", ".join(f"{v} {scyc[v]:.0f}" for v in VARIANTS))
    bad = {k: s for k, s in checks.items() if len(s) != 1}
    print("\nChecksums: " + ("every runtime and run computed the same result per case and n"
                             if not bad else f"MISMATCH {bad}"))


def bench():
    files = sorted(OUT.glob("bench_*_r*.txt"))
    if not files:
        return
    data = collections.defaultdict(lambda: collections.defaultdict(list))
    bbd = collections.defaultdict(list)
    ok = True
    for f in files:
        v = f.name.split("_")[1]
        for line in f.read_text().splitlines():
            m = re.match(r"(\S+)\s+(\d+)\s+(\d+)\s+\S+\s+.*?(✓|✗)\s*$", line)
            if m:
                data[m.group(1)][v].append(int(m.group(2)))
                bbd[m.group(1)].append(int(m.group(3)))
                ok = ok and m.group(4) == "✓"
    print("\n## benchmarks/bench.sh (best of 3 per run, median of rounds, ms)\n")
    print("| workload | main | A | B | bb | B vs main |")
    print("|---|---:|---:|---:|---:|---:|")
    for w, per_v in data.items():
        m = {v: med(per_v[v]) for v in VARIANTS}
        print(f"| {w} | {m['main']:.0f} | {m['A']:.0f} | {m['B']:.0f} | {med(bbd[w]):.0f} | "
              f"{100 * (m['B'] / m['main'] - 1):+.1f} % |")
    print("\nResults " + ("match Babashka's in every run" if ok else "DIFFER from Babashka's"))


def startup():
    rows = {}
    for v in VARIANTS:
        p = OUT / f"startup_{v}.txt"
        if not p.exists():
            continue
        cyc, ms = [], []
        for line in p.read_text().splitlines():
            parts = line.split(",")
            if len(parts) > 2 and "cycles" in parts[2]:
                cyc.append(float(parts[0]) / 1e6)
            if len(parts) > 2 and "task-clock" in parts[2]:
                ms.append(float(parts[0]))
        rows[v] = (med(cyc), med(ms), len(cyc))
    if rows:
        print("\n## start-up, empty file (perf stat -r 50, median of rounds)\n")
        print("| variant | Mcycles | task-clock ms |")
        print("|---|---:|---:|")
        for v, (c, m, k) in rows.items():
            print(f"| {v} | {c:.1f} | {m:.1f} |")


def showcase():
    d = OUT / "showcase"
    if not d.exists():
        return
    names = ["startup", "factorial", "grep", "json", "wordfreq"]

    def parse(p):
        t = p.read_text()
        m = re.search(r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\): (?:(\d+):)?(\d+):([\d.]+)", t)
        sec = int(m.group(1) or 0) * 3600 + int(m.group(2)) * 60 + float(m.group(3))
        rss = int(re.search(r"Maximum resident set size \(kbytes\): (\d+)", t).group(1))
        return sec, rss
    print("\n## showcase scripts (wall s / peak RSS MB, median of rounds)\n")
    print("| script | main | A | B | bb | B vs main (time) | outputs |")
    print("|---|---:|---:|---:|---:|---:|---|")
    for n in names:
        res, shas, rcs = {}, set(), set()
        for v in VARIANTS + ["bb"]:
            runs = sorted(d.glob(f"{n}_{v}_r*.time"))
            vals = [parse(p) for p in runs]
            res[v] = (med([x[0] for x in vals]), med([x[1] / 1024 for x in vals]))
            shas |= {p.with_suffix(".sha").read_text().strip() for p in runs}
            rcs |= {p.with_suffix(".rc").read_text().strip() for p in runs}
        same = "same" if len(shas) == 1 and rcs == {"0"} else f"DIFFERENT ({len(shas)} digests, rc {rcs})"
        cell = lambda v: f"{res[v][0]:.2f} / {res[v][1]:.0f}"
        print(f"| {n} | {cell('main')} | {cell('A')} | {cell('B')} | {cell('bb')} | "
              f"{100 * (res['B'][0] / res['main'][0] - 1):+.0f} % | {same} |")


cases()
bench()
startup()
showcase()
gate = OUT / "gate.txt"
if gate.exists():
    print("\n## machine state at each round (gate.txt)\n")
    for line in gate.read_text().splitlines():
        print("    " + line)
