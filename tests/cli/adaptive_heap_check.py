#!/usr/bin/env python3
"""Checks protoCore's adaptive heap controller as the runtime configures it.

Runs one program of the runtime under test with PROTOCORE_HEAP_TRACE=1 and
reads protoCore's per-cycle trace from its standard error.  The controller
prints one line per cycle of the form

    protoCore heap: space=0x... cycle=N L=... w=... S=a->b H=... ...

and a space with fixed limits prints

    protoCore heap: space=0x... fixed cycle=N L=... soft=... hard=... ...

Modes:

  --expect on    at least one controller line and no fixed line: the runtime
                 enables the controller by default.
  --expect off   at least one fixed line and no controller line: the
                 PROTOCORE_ADAPTIVE_HEAP=0 switch turns it off.

With --max-rss-mb the child's peak resident set size must stay at or below
that bound (os.wait4 on Linux and macOS, GetProcessMemoryInfo on Windows), and
with --min-cycles the run must have completed at least that many cycles: a
run that allocated less garbage than its heap could hold proves nothing about
a bound.  The program must print --sentinel on standard output, so a program
that failed before doing its work cannot pass.

Usage:
  adaptive_heap_check.py --expect on|off --sentinel TEXT [--set VAR=VALUE]...
      [--unset VAR]... [--min-cycles N] [--max-rss-mb M] -- COMMAND...
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

CONTROLLER_LINE = re.compile(r"^protoCore heap: space=\S+ cycle=(\d+) ", re.M)
FIXED_LINE = re.compile(r"^protoCore heap: space=\S+ fixed cycle=(\d+) ", re.M)


def run_measured(cmd, env):
    """Runs cmd; returns (exit code, stdout, stderr, peak RSS in bytes)."""
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        proc = subprocess.Popen(cmd, stdout=out, stderr=err, env=env)
        if os.name == "nt":
            proc.wait()
            peak = windows_peak_working_set(proc)
            code = proc.returncode
        else:
            _, status, usage = os.wait4(proc.pid, 0)
            proc.returncode = os.waitstatus_to_exitcode(status)
            code = proc.returncode
            # ru_maxrss: kilobytes on Linux, bytes on macOS.
            peak = usage.ru_maxrss if sys.platform == "darwin" else usage.ru_maxrss * 1024
        out.seek(0)
        err.seek(0)
        return (code, out.read().decode("utf-8", "replace"),
                err.read().decode("utf-8", "replace"), peak)


def windows_peak_working_set(proc):
    import ctypes
    from ctypes import wintypes

    class ProcessMemoryCounters(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    counters = ProcessMemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    get_info = ctypes.windll.kernel32.K32GetProcessMemoryInfo
    get_info.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessMemoryCounters), wintypes.DWORD]
    get_info.restype = wintypes.BOOL
    # Popen keeps the process handle open until the object is collected.
    if not get_info(wintypes.HANDLE(int(proc._handle)), ctypes.byref(counters), counters.cb):
        raise OSError(ctypes.get_last_error(), "K32GetProcessMemoryInfo failed")
    return counters.PeakWorkingSetSize


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--expect", choices=("on", "off"), required=True)
    parser.add_argument("--sentinel", required=True)
    parser.add_argument("--set", action="append", default=[], metavar="VAR=VALUE")
    parser.add_argument("--unset", action="append", default=[], metavar="VAR")
    parser.add_argument("--min-cycles", type=int, default=1)
    parser.add_argument("--max-rss-mb", type=float, default=None)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    cmd = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not cmd:
        parser.error("no command given")

    env = dict(os.environ)
    for name in args.unset:
        env.pop(name, None)
    for assignment in args.set:
        name, _, value = assignment.partition("=")
        env[name] = value
    env["PROTOCORE_HEAP_TRACE"] = "1"

    code, out, err, peak = run_measured(cmd, env)
    controller = CONTROLLER_LINE.findall(err)
    fixed = FIXED_LINE.findall(err)
    cycles = len(controller) + len(fixed)
    peak_mb = peak / (1024.0 * 1024.0)
    print(f"exit={code} controller_cycles={len(controller)} fixed_cycles={len(fixed)} "
          f"peak_rss_mb={peak_mb:.1f}")

    failures = []
    if code != 0:
        failures.append(f"the program exited with {code}")
    if args.sentinel not in out:
        failures.append(f"the program did not print {args.sentinel!r}: it did not finish its work")
    if args.expect == "on":
        if not controller:
            failures.append("no adaptive-controller cycle line: the controller is not enabled")
        if fixed:
            failures.append(f"{len(fixed)} fixed-limit cycle lines: a fixed limit is in force")
    else:
        if controller:
            failures.append(f"{len(controller)} adaptive-controller cycle lines with the switch off")
        if not fixed:
            failures.append("no fixed-limit cycle line: the switch did not apply a fixed limit")
    if cycles < args.min_cycles:
        failures.append(f"{cycles} cycles, fewer than the {args.min_cycles} required: the run "
                        "did not allocate enough garbage to exercise the bound")
    if args.max_rss_mb is not None and peak_mb > args.max_rss_mb:
        failures.append(f"peak RSS {peak_mb:.1f} MB exceeds the bound of {args.max_rss_mb} MB")

    if failures:
        sys.stdout.write("--- stdout (tail) ---\n" + out[-2000:] + "\n")
        sys.stdout.write("--- stderr (tail) ---\n" + err[-4000:] + "\n")
        for failure in failures:
            print("FAIL: " + failure)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
