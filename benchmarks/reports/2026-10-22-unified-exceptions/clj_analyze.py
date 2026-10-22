#!/usr/bin/env python3
"""protoClojure A/B analysis: bench.sh (best of 3 per round) -> median of the
three rounds per side, AFTER/BEFORE, against Babashka; and the probe table."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from uex_common import med, gmean, probe_table
OUT = sys.argv[1]
def parse(p):
    d = {}
    for line in open(p):
        m = re.match(r"(\S+)\s+(\d+)\s+(\d+)\s+\S+x\s+(\S+)\s+(\S+)$", line.strip())
        if m:
            d[m.group(1)] = (float(m.group(2)), float(m.group(3)), m.group(4), m.group(5))
    return d
R = {(v, r): parse(os.path.join(OUT, "clj_%s_r%d.txt" % (v, r))) for v in ("before", "after") for r in (1, 2, 3)}
allok = all(x[3] == "✓" for d in R.values() for x in d.values()) and all(len(d) == 5 for d in R.values())
print("All 6 bench.sh runs: 5 workloads each, every result matched bb:", allok)
print("| Workload | result | bb | BEFORE | AFTER | A/B | BEFORE rounds | AFTER rounds | AFTER/bb |")
print("|---|---|---:|---:|---:|---:|---|---|---:|")
rs, rb, ra = [], [], []
for k in R[("after", 1)]:
    b = [R[("before", r)][k][0] for r in (1, 2, 3)]; a = [R[("after", r)][k][0] for r in (1, 2, 3)]
    bb = med([R[(v, r)][k][1] for v in ("before", "after") for r in (1, 2, 3)])
    rs.append(med(a) / med(b)); rb.append(med(b) / bb); ra.append(med(a) / bb)
    print("| `%s` | %s | %.0f | %.0f | %.0f | %.3f | %s | %s | %.2fx |" % (k, R[("after", 1)][k][2], bb, med(b), med(a), med(a) / med(b),
          "/".join("%.0f" % x for x in b), "/".join("%.0f" % x for x in a), med(a) / bb))
print("\ngeomean A/B (5): %.3f; BEFORE/bb %.2fx; AFTER/bb %.2fx" % (gmean(rs), gmean(rb), gmean(ra)))
t, raw, su = probe_table(os.path.join(OUT, "probes.txt"), ["before", "after", "bb"], [("before", "after")])
print(); print(t); print(su); print(); print("\n".join(raw))
