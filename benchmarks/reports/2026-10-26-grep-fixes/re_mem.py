import re, time
pat = re.compile(r"ERROR .* latency=(\d{4,})ms")
lines = ["2026-10-01T12:00:00Z INFO  GET /api/items/%d 200 latency=%dms user=u%d" % (i, i % 900, i % 77) for i in range(20000)]
lines[7] = "2026-10-01T12:00:00Z ERROR GET /api/x 500 latency=1234ms user=u1"
t0 = time.time(); n = 0
for l in lines:
    if pat.search(l): n += 1
t1 = time.time()
lit = re.compile("ERROR"); k = 0
for l in lines:
    if lit.search(l): k += 1
t2 = time.time()
print("searches", len(lines), "matches", n, k, "full us/search", round((t1 - t0) * 1e6 / len(lines), 1), "literal us/search", round((t2 - t1) * 1e6 / len(lines), 1))
