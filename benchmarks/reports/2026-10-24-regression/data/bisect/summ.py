import sys, csv, statistics, os
rows = list(csv.reader(open(sys.argv[1])))
order = [os.path.basename(b) for b in sys.argv[2:]]
d = {}; bad = []
for name, r, w, c, i, ok in rows:
    if ok != "1": bad.append((name, r, w)); continue
    d.setdefault((name, w), []).append((float(c), float(i)))
wls = []
for _, _, w, *_ in rows:
    if w not in wls: wls.append(w)
med = lambda xs: statistics.median(xs)
print("Mcycles median (instructions M); run = workload - empty")
print("%-9s" % "commit" + "".join("%22s" % w for w in wls))
base = order[0]
for n in order:
    line = "%-9s" % n
    e = med([c for c, _ in d.get((n, "empty"), [(0, 0)])])
    for w in wls:
        xs = d.get((n, w))
        if not xs: line += "%22s" % "FAIL"; continue
        c = med([x for x, _ in xs]); i = med([y for _, y in xs])
        if w != "empty":
            c -= e
        line += "%14.1f (%5.0f)" % (c / 1e6, i / 1e6)
    print(line)
print("\nrun-phase cycles vs first column, %; [min-max spread of workload cycles %]")
eb = med([c for c, _ in d[(base, "empty")]])
for n in order:
    e = med([c for c, _ in d[(n, "empty")]]); line = "%-9s" % n
    for w in wls:
        if (n, w) not in d: line += "%12s" % "FAIL"; continue
        xs = [x for x, _ in d[(n, w)]]; bx = [x for x, _ in d[(base, w)]]
        if w == "empty": v, b0 = med(xs), med(bx)
        else: v, b0 = med(xs) - e, med(bx) - eb
        sp = 100 * (max(xs) - min(xs)) / med(xs)
        line += "%+8.1f[%2.0f]" % (100 * (v / b0 - 1), sp)
    print(line)
if bad: print("\nFAILED runs (not data points):", bad)
