"""Shared helpers for the 2026-10-22 unified-exceptions analyses (protoST,
protoScala, protoClojure): medians, geomeans and the perf-stat probe table."""
import math, re, statistics

def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None

def gmean(xs):
    xs = [x for x in xs if x]
    return math.exp(sum(map(math.log, xs)) / len(xs)) if xs else None

def probes(path):
    """{probe: {variant: (cycles, task_clock_ms, check)}} from probes.txt lines
    `name variant cycles taskclock | CHECK n`."""
    out = {}
    for line in open(path):
        m = re.match(r"(\S+) (\S+) (\S+) (\S+) \| ?(.*)$", line.strip())
        if not m:
            continue
        n, v, cyc, tc, chk = m.groups()
        try:
            out.setdefault(n, {})[v] = (float(cyc), float(tc), chk.strip())
        except ValueError:
            out.setdefault(n, {})[v] = (None, None, chk.strip() or line.strip())
    return out

def probe_table(path, variants, ab_pairs, labels=None):
    """Markdown rows: cycles per iteration net of the startup probe's cycles,
    iterations taken from the CHECK value (each probe's CHECK is its count)."""
    P = probes(path)
    st = P.get("startup", {})
    rows, raw = [], []
    order = ["empty_loop", "try_no_throw", "throw_catch", "throw_across5", "zerodiv"]
    for n in order + sorted(set(P) - set(order) - {"startup"}):
        if n not in P:
            continue
        cells, net = [], {}
        iters = None
        for v in variants:
            c = P[n].get(v)
            if c and c[0] is not None and st.get(v) and st[v][0] is not None:
                k = re.search(r"CHECK (\d+)", c[2])
                iters = int(k.group(1)) if k else None
                net[v] = (c[0] - st[v][0]) / iters if iters else None
            else:
                net[v] = None
            raw.append("%s %s cycles=%s task_ms=%s check=%r" % (n, v, c and c[0], c and c[1], c and c[2]))
        line = "| `%s` | %s |" % (n, "{:,}".format(iters) if iters else "?")
        for v in variants:
            line += " %s |" % (("{:,.0f}".format(net[v]) if net[v] >= 0 else "<0 (noise)") if net[v] is not None else "n/a")
        for a, b in ab_pairs:
            line += " %s |" % ("%.2fx" % (net[b] / net[a]) if net.get(a) and net.get(b) and net[a] > 0 else "n/a")
        rows.append(line)
    hdr = "| Probe | iters | " + " | ".join(labels or variants) + " | " + " | ".join(
        "%s/%s" % (b, a) for a, b in ab_pairs) + " |"
    sep = "|---|---:|" + "---:|" * (len(variants) + len(ab_pairs))
    startup = "startup cycles: " + ", ".join("%s %s" % (v, "{:,.0f}".format(st[v][0]) if st.get(v) and st[v][0] else "n/a") for v in variants)
    return "\n".join([hdr, sep] + rows), raw, startup
