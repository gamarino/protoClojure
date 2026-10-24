#!/usr/bin/env python3
"""Quiet gate (copied from protoCore fm.py): waits until no build/test runs and
fewer than QUIET_CORES CPUs are busy over 3 s; prints load and busy CPUs."""
import os, subprocess, time
BUSY = ("cc1plus", "cc1", "ctest", "proto_tests", "ld", "collect2", "make", "ninja",
        "cmake", "cpack", "test262_runner")
Q = float(os.environ.get("QUIET_CORES", "4.0"))
def busy_cores(s=3.0):
    def snap():
        v = [int(x) for x in open("/proc/stat").readline().split()[1:]]
        return sum(v), v[3] + v[4]
    t0, i0 = snap(); time.sleep(s); t1, i1 = snap()
    return (1.0 - (i1 - i0) / float(max(1, t1 - t0))) * (os.cpu_count() or 1)
while True:
    comms = subprocess.run(["ps", "-eo", "comm"], capture_output=True, text=True).stdout.split()
    busy = sorted(set(c for c in comms if c in BUSY)); c = busy_cores()
    if not busy and c <= Q:
        print("gate ok load=%.2f busy_cpus=%.2f" % (os.getloadavg()[0], c)); break
    print("waiting busy=%s cpus=%.2f" % (busy, c), flush=True); time.sleep(10)
