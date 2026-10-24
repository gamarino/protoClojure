#!/usr/bin/env bash
# The macros branch, start to finish: the baseline (main 8b7938a, before
# macros stage 1) against the branch's final code (feature/macros 3b75e41),
# both built Release from clean worktrees.
#
#   A  baseline on its own kernel, protoCore 3.0.0
#   B  baseline on protoCore 3.1.0 (branch feature/lazy-object), which the
#      final code needs; 3.1.0 is ABI-compatible with 3.0.0
#   C  final on protoCore 3.1.0
#
# A against B isolates the kernel change, B against C the protoClojure
# change. Each binary runs on its own fresh copy of its libprotoCore.so.4.
#
#   * benchmarks/bench.sh (fib, tak, sum-loop, reduce-list, sum-squares;
#     best of 3 per workload per round), ROUNDS rounds, the variant order
#     rotated each round, each run behind the quiet gate;
#   * map-loop.clj (keyword-keyed map reads and assoc), perf stat cycles,
#     ROUNDS interleaved rounds;
#   * start-up: an empty file, perf stat -r 20, three interleaved passes.
#
# Every program prints its result; bench.sh checks protoclj's against bb's.
# Usage: run.sh BASELINE_BIN BASELINE_LIB FINAL_BIN FINAL_LIB BB [OUT]
set -u
BASE_BIN=${1:?}; BASE_LIB=${2:?}; FINAL_BIN=${3:?}; FINAL_LIB=${4:?}; BB=${5:?}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${6:-$HERE/out}
ROUNDS=${ROUNDS:-5}
REPO=$(cd "$HERE/../../.." && pwd)
Q=${QUIET_GATE:-true}
mkdir -p "$OUT/libs/A" "$OUT/libs/B" "$OUT/libs/C"
cp -L "$BASE_LIB" "$OUT/libs/A/libprotoCore.so.4"
cp -L "$FINAL_LIB" "$OUT/libs/B/libprotoCore.so.4"
cp -L "$FINAL_LIB" "$OUT/libs/C/libprotoCore.so.4"
declare -A BIN=( [A]=$BASE_BIN [B]=$BASE_BIN [C]=$FINAL_BIN )
: > "$OUT/ldd.txt"
for v in A B C; do
    echo "$v: ${BIN[$v]}" >> "$OUT/ldd.txt"
    LD_LIBRARY_PATH=$OUT/libs/$v ldd "${BIN[$v]}" | grep -i protocore >> "$OUT/ldd.txt"
done
orders=("A B C" "B C A" "C A B")
for ((r = 1; r <= ROUNDS; ++r)); do
    for v in ${orders[$(( (r - 1) % 3 ))]}; do
        $Q > "$OUT/quiet_${v}_r$r.txt" 2>&1
        echo "bench $v r$r $(date -u +%T) load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT/log.txt"
        ( cd "$REPO" && LD_LIBRARY_PATH=$OUT/libs/$v bash benchmarks/bench.sh "${BIN[$v]}" "$BB" \
            > "$OUT/bench_${v}_r$r.txt" 2>&1 )
    done
done
$Q > "$OUT/quiet_loops.txt" 2>&1
for ((r = 1; r <= ROUNDS; ++r)); do
    for v in ${orders[$(( (r - 1) % 3 ))]}; do
        res=$(LC_ALL=C LD_LIBRARY_PATH=$OUT/libs/$v perf stat -x, -e cycles -o "$OUT/tmp.$v" "${BIN[$v]}" "$HERE/map-loop.clj" | tr '\n' ' ')
        echo "$v $res" >> "$OUT/results_map-loop.txt"
        echo "$v $(grep cycles "$OUT/tmp.$v" | cut -d, -f1)" >> "$OUT/cycles_map-loop.txt"
    done
done
rm -f "$OUT"/tmp.?
echo "" > "$OUT/empty.clj"
$Q > "$OUT/quiet_startup.txt" 2>&1
for pass in 1 2 3; do
    for v in A B C; do
        LC_ALL=C LD_LIBRARY_PATH=$OUT/libs/$v perf stat -r 20 -x, -e cycles,task-clock "${BIN[$v]}" "$OUT/empty.clj" \
            2>> "$OUT/startup_${v}.txt" > /dev/null
    done
done
python3 -I - "$OUT" "$ROUNDS" <<'PY' | tee "$OUT/summary.txt"
import pathlib, statistics, sys
out, rounds = pathlib.Path(sys.argv[1]), int(sys.argv[2])
work = ["fib", "tak", "sum-loop", "reduce-list", "sum-squares"]
ms = {v: {w: [] for w in work} for v in "ABC"}
ok = True
for v in "ABC":
    for r in range(1, rounds + 1):
        for line in (out / f"bench_{v}_r{r}.txt").read_text().splitlines():
            f = line.split()
            if f and f[0] in work:
                ms[v][f[0]].append(int(f[1]))
                if "✓" not in line: ok = False; print(f"  result mismatch: {v} r{r} {line}")
med = lambda xs: statistics.median(xs) if xs else float("nan")
print(f"{'workload':12} {'A base/3.0':>11} {'B base/3.1':>11} {'C final/3.1':>12} {'C vs B':>8} {'C vs A':>8}   (ms, median of {rounds} rounds of best-of-3; ranges in brackets)")
for w in work:
    a, b, c = med(ms["A"][w]), med(ms["B"][w]), med(ms["C"][w])
    rng = lambda v: f"[{min(ms[v][w])}-{max(ms[v][w])}]"
    print(f"{w:12} {a:6.0f} {rng('A'):>9} {b:6.0f} {rng('B'):>9} {c:6.0f} {rng('C'):>9} {100*(c/b-1):+7.1f}% {100*(c/a-1):+7.1f}%")
cyc = {v: [] for v in "ABC"}
for line in (out / "cycles_map-loop.txt").read_text().splitlines():
    v, n = line.split()
    cyc[v].append(int(n))
a, b, c = (med(cyc[v]) / 1e6 for v in "ABC")
print(f"{'map-loop':12} A {a:.0f} B {b:.0f} C {c:.0f} Mcycles (median), C vs B {100*(c/b-1):+.1f}%, C vs A {100*(c/a-1):+.1f}%")
res = {l.split()[0]: " ".join(l.split()[1:]) for l in (out / "results_map-loop.txt").read_text().splitlines()}
print(f"  map-loop output: {res}")
for v in "ABC":
    rows = [l.split(",") for l in (out / f"startup_{v}.txt").read_text().splitlines() if "," in l]
    cy = [float(x[0]) / 1e6 for x in rows if len(x) > 2 and x[2] == "cycles"]
    tc = [float(x[0]) for x in rows if len(x) > 2 and x[2] == "task-clock"]
    print(f"start-up {v}: {med(cy):.1f} Mcycles, {med(tc):.1f} ms task-clock (median of 3 passes of 20 runs: {', '.join(f'{x:.1f}' for x in cy)} Mcycles)")
print("all bench results matched bb's" if ok else "RESULT MISMATCH")
PY
