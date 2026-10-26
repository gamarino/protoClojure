import sys, time
t0 = time.time(); lines = 0
with open(sys.argv[1]) as f:
    for line in f:
        lines += 1
print("lines", lines, "ms", int((time.time() - t0) * 1000))
