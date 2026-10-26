import re, sys, time
pat = re.compile(r"ERROR .* latency=(\d{4,})ms")
t0 = time.time()
n = 0; total = 0; lines = 0
with open(sys.argv[1]) as f:
    for line in f:
        lines += 1
        m = pat.search(line)
        if m:
            n += 1; total += int(m.group(1))
t1 = time.time()
print("lines", lines, "matches", n, "total", total, "ms", int((t1 - t0) * 1000))
