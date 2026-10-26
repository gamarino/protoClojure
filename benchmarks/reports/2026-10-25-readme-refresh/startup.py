#!/usr/bin/env python3
"""Start-up wall time and peak RSS, protoclj and Babashka interleaved.

For each pass and each script, runs every runtime N times, alternating the
runtime on every run. Wall time is measured with perf_counter around the
child; peak RSS comes from os.wait4's rusage (ru_maxrss, KiB on Linux), the
figure /usr/bin/time -f %M reports. A run counts only if it exits with
status 0 and prints the expected output.

Usage: startup.py PROTOCLJ BB EXAMPLES_DIR OUT_DIR [PASSES] [N]
Environment: ROUND_GATE, a command run (with the label startup-pN) before
each pass; default none.
"""
import json
import os
import pathlib
import statistics
import subprocess
import sys
import time

protoclj, bb, examples, out = sys.argv[1:5]
passes = int(sys.argv[5]) if len(sys.argv) > 5 else 3
n = int(sys.argv[6]) if len(sys.argv) > 6 else 50
out = pathlib.Path(out)
out.mkdir(parents=True, exist_ok=True)
scripts = {
    "hello": (os.path.join(examples, "01-hello.clj"), "hello, world\n"),
    "empty": (os.path.join(examples, "showcase", "empty.clj"), ""),
}
runtimes = {"protoclj": protoclj, "bb": bb}


def run(binary, script, expected):
    t0 = time.perf_counter()
    p = subprocess.Popen([binary, script], stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    data = p.stdout.read()
    _, status, ru = os.wait4(p.pid, 0)
    wall = (time.perf_counter() - t0) * 1000.0
    p.returncode = os.waitstatus_to_exitcode(status)
    ok = p.returncode == 0 and data.decode(errors="replace") == expected
    return wall, ru.ru_maxrss, ok


raw = []
failures = 0
for ps in range(1, passes + 1):
    if os.environ.get("ROUND_GATE"):
        subprocess.run(os.environ["ROUND_GATE"].split() + [f"startup-p{ps}"], check=True)
    for name, (script, expected) in scripts.items():
        for i in range(n):
            order = ["protoclj", "bb"] if i % 2 == 0 else ["bb", "protoclj"]
            for label in order:
                wall, rss, ok = run(runtimes[label], script, expected)
                failures += 0 if ok else 1
                raw.append({"pass": ps, "script": name, "runtime": label,
                            "run": i + 1, "ms": round(wall, 3), "rss_kb": rss, "ok": ok})

(out / "startup-raw.json").write_text(json.dumps(raw, indent=0))
lines = []
for name in scripts:
    for label in runtimes:
        rows = [r for r in raw if r["script"] == name and r["runtime"] == label and r["ok"]]
        per_pass = [statistics.median(r["ms"] for r in rows if r["pass"] == ps)
                    for ps in range(1, passes + 1)]
        rss = [r["rss_kb"] for r in rows]
        lines.append(
            f"{name:6} {label:9} wall median {statistics.median(r['ms'] for r in rows):6.1f} ms "
            f"(per-pass medians {', '.join(f'{x:.1f}' for x in per_pass)}; "
            f"p10-p90 {statistics.quantiles([r['ms'] for r in rows], n=10)[0]:.1f}-"
            f"{statistics.quantiles([r['ms'] for r in rows], n=10)[-1]:.1f}) | "
            f"peak RSS median {statistics.median(rss) / 1024:5.1f} MB "
            f"(min {min(rss) / 1024:.1f}, max {max(rss) / 1024:.1f}) | runs {len(rows)}")
lines.append(f"failures: {failures}")
(out / "startup-summary.txt").write_text("\n".join(lines) + "\n")
print("\n".join(lines))
sys.exit(1 if failures else 0)
