#!/usr/bin/env bash
# The macros-branch regression, before and after its fixes. Four variants,
# Release builds from clean worktrees:
#
#   A  baseline (main 8b7938a, before macros stage 1) on protoCore 3.0.0
#   B  the same baseline binary on protoCore 3.1.0 (feature/lazy-object)
#   C  feature/macros before the fixes (3b75e41) on 3.1.0
#   D  feature/macros with the fixes on 3.1.0
#
# Every round runs each variant from a FRESH copy of its binary and of its
# libprotoCore.so.4 (the file-placement lottery), in an order rotated each
# round, behind the quiet gate. Three parts:
#
#   * benchmarks/bench.sh (best of 3 per workload, one process per run, so
#     start-up included), ROUNDS rounds; bench.sh checks results against bb;
#   * start-up: an empty file, perf stat -r 10 per round;
#   * run phase: the long variants in long/ (fib 32, tak 24 16 8, 10 M-step
#     loop, 300 k / 100 k-element lists, 2 M map ops), perf stat cycles and
#     instructions per round; run = workload - empty file (medians). Each
#     program's output is compared with its expected value.
#
# Usage: run.sh A_BIN A_LIB B_LIB C_BIN D_BIN BB OUT   (B, C and D share B_LIB)
set -u
A_BIN=${1:?}; A_LIB=${2:?}; B_LIB=${3:?}; C_BIN=${4:?}; D_BIN=${5:?}; BB=${6:?}; OUT=${7:?}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
ROUNDS=${ROUNDS:-5}
Q=${QUIET_GATE:-true}
mkdir -p "$OUT"
declare -A BIN=([A]=$A_BIN [B]=$A_BIN [C]=$C_BIN [D]=$D_BIN)
declare -A LIB=([A]=$A_LIB [B]=$B_LIB [C]=$B_LIB [D]=$B_LIB)
declare -A EXP=([fib]="2178309" [tak]="9" [sum-loop]="50000005000000" [reduce-list]="44999850000"
                [sum-squares]="333328333350000" [map-loop]="map-loop 2000007000002")
orders=("A B C D" "B C D A" "C D A B" "D A B C")
fresh() {  # fresh <variant> <round>: a new directory with copies of the binary and library
    local d="$OUT/fresh/$1_r$2"; mkdir -p "$d"
    cp -L "${BIN[$1]}" "$d/protoclj"; cp -L "${LIB[$1]}" "$d/libprotoCore.so.4"; echo "$d"
}
: > "$OUT/ldd.txt"; : > "$OUT/run_phase.csv"; : > "$OUT/log.txt"
for ((r = 1; r <= ROUNDS; ++r)); do
    for v in ${orders[$(( (r - 1) % 4 ))]}; do
        d=$(fresh "$v" "$r")
        [[ $r == 1 ]] && { echo "$v: ${BIN[$v]}" >> "$OUT/ldd.txt"; LD_LIBRARY_PATH=$d ldd "$d/protoclj" | grep -i protocore >> "$OUT/ldd.txt"; }
        $Q > "$OUT/quiet_${v}_r$r.txt" 2>&1
        echo "$v r$r $(date -u +%T) load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT/log.txt"
        ( cd "$REPO" && LD_LIBRARY_PATH=$d bash benchmarks/bench.sh "$d/protoclj" "$BB" > "$OUT/bench_${v}_r$r.txt" 2>&1 )
        for w in empty fib tak sum-loop reduce-list sum-squares map-loop; do
            rep=1; [[ $w == empty ]] && rep=10
            res=$(cd "$HERE/long" && LC_ALL=C LD_LIBRARY_PATH=$d perf stat -r $rep -x, -e cycles,instructions \
                  -o "$d/perf_$w.txt" "$d/protoclj" "$w.clj" 2>&1 | tail -1)
            ok=1; [[ $w != empty && "$res" != "${EXP[$w]}" ]] && ok=0
            echo "$v,$r,$w,$(grep ',cycles' "$d/perf_$w.txt" | cut -d, -f1),$(grep ',instructions' "$d/perf_$w.txt" | cut -d, -f1),$ok" >> "$OUT/run_phase.csv"
        done
        rm -rf "$d"
    done
done
python3 -I - "$OUT" "$ROUNDS" <<'PY' | tee "$OUT/summary.txt"
import pathlib, statistics, sys, csv
out, rounds = pathlib.Path(sys.argv[1]), int(sys.argv[2])
V = "ABCD"; med = statistics.median
work = ["fib", "tak", "sum-loop", "reduce-list", "sum-squares"]
ms = {v: {w: [] for w in work} for v in V}; ok = True
for v in V:
    for r in range(1, rounds + 1):
        for line in (out / f"bench_{v}_r{r}.txt").read_text().splitlines():
            f = line.split()
            if f and f[0] in work:
                ms[v][f[0]].append(int(f[1]))
                if "✓" not in line: ok = False; print(f"  result mismatch: {v} r{r} {line}")
print(f"bench.sh, ms (median of {rounds} rounds of best-of-3, start-up included; ranges in brackets)")
print(f"{'workload':12}" + "".join(f"{v:>14}" for v in V) + f"{'C vs B':>9}{'D vs B':>9}")
for w in work:
    m = {v: med(ms[v][w]) for v in V}
    print(f"{w:12}" + "".join(f"{m[v]:6.0f} [{min(ms[v][w])}-{max(ms[v][w])}]".rjust(14) for v in V)
          + f"{100*(m['C']/m['B']-1):+8.1f}%{100*(m['D']/m['B']-1):+8.1f}%")
rows = list(csv.reader(open(out / "run_phase.csv")))
bad = [r for r in rows if r[5] != "1"]
d = {}
for v, r, w, c, i, k in rows:
    if k == "1": d.setdefault((v, w), []).append((float(c), float(i)))
wl = ["empty", "fib", "tak", "sum-loop", "reduce-list", "sum-squares", "map-loop"]
print("\nperf stat, M cycles (M instructions), medians; run phase = workload - empty file")
print(f"{'':12}" + "".join(f"{v:>20}" for v in V) + f"{'C vs B':>9}{'D vs B':>9}")
for w in wl:
    vals = {}
    for v in V:
        c = med([x for x, _ in d[(v, w)]]); i = med([y for _, y in d[(v, w)]])
        if w != "empty":
            c -= med([x for x, _ in d[(v, "empty")]]); i -= med([y for _, y in d[(v, "empty")]])
        vals[v] = (c / 1e6, i / 1e6)
    print(f"{w:12}" + "".join(f"{vals[v][0]:9.1f} ({vals[v][1]:7.1f})".rjust(20) for v in V)
          + f"{100*(vals['C'][0]/vals['B'][0]-1):+8.1f}%{100*(vals['D'][0]/vals['B'][0]-1):+8.1f}%")
print("all bench.sh results matched bb's" if ok else "RESULT MISMATCH")
print("all run-phase outputs verified" if not bad else f"FAILED RUNS: {bad}")
PY
