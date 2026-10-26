#!/usr/bin/env bash
# benchmarks/bench.sh repeated over ROUNDS interleaved rounds, with every run
# checked against the value its script must print.
#
# Each round measures every workload the way bench.sh does (RUNS process
# invocations, start-up included, best wall time of the RUNS), alternating
# which runtime goes first from one round to the next. A run counts only if
# it exits with status 0 and prints the expected value; anything else is
# recorded as FAILED and makes the script exit non-zero, so a crash can never
# pass for a fast run.
#
# Usage: bench-rounds.sh PROTOCLJ BB OUT_DIR
# Environment: ROUNDS (default 5), RUNS (default 3, as in bench.sh),
# ROUND_GATE (a command run before each round, e.g. gate.sh; default none).
set -u
PROTOCLJ=${1:?usage: bench-rounds.sh PROTOCLJ BB OUT_DIR}
BB=${2:?usage: bench-rounds.sh PROTOCLJ BB OUT_DIR}
OUT=${3:?usage: bench-rounds.sh PROTOCLJ BB OUT_DIR}
HERE=$(cd "$(dirname "$0")" && pwd)
BENCH=$(cd "$HERE/../.." && pwd)
ROUNDS=${ROUNDS:-5}
RUNS=${RUNS:-3}
mkdir -p "$OUT"
RAW="$OUT/bench-raw.tsv"
echo -e "round\tworkload\truntime\trun\tms\tstatus\toutput" > "$RAW"

# workload | expected stdout
WORKLOADS=(
    "fib|832040"
    "tak|7"
    "sum-loop|500000500000"
    "reduce-list|49995000"
    "sum-squares|332833500"
)

failures=0
one_run() {  # round workload runtime binary expected run
    local round=$1 w=$2 label=$3 bin=$4 expected=$5 i=$6
    local t0 t1 ms out rc status
    t0=$(date +%s.%N)
    out=$("$bin" "$BENCH/$w.clj" 2>&1)
    rc=$?
    t1=$(date +%s.%N)
    ms=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", (b - a) * 1000 }')
    status=ok
    if [[ $rc -ne 0 || "$out" != "$expected" ]]; then
        status="FAILED(rc=$rc)"; failures=$((failures + 1))
    fi
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$round" "$w" "$label" "$i" "$ms" "$status" \
        "$(printf '%s' "$out" | tr '\n\t' '  ' | cut -c1-80)" >> "$RAW"
}

for ((r = 1; r <= ROUNDS; ++r)); do
    if [[ -n "${ROUND_GATE:-}" ]]; then $ROUND_GATE "bench-r$r"; fi
    echo "bench round $r $(date -u +%T) load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT/rounds.txt"
    if (( r % 2 == 1 )); then order="protoclj bb"; else order="bb protoclj"; fi
    for entry in "${WORKLOADS[@]}"; do
        IFS='|' read -r w expected <<<"$entry"
        for label in $order; do
            bin=$PROTOCLJ; [[ $label == bb ]] && bin=$BB
            for ((i = 1; i <= RUNS; ++i)); do one_run "$r" "$w" "$label" "$bin" "$expected" "$i"; done
        done
    done
done

# factorial-100 with plain `*`: protoclj must print the 158-digit result;
# Babashka is expected to fail with a long overflow. Both outcomes recorded.
{
    out=$("$PROTOCLJ" "$BENCH/factorial-100.clj" 2>&1); rc=$?
    echo "protoclj factorial-100: rc=$rc digits=$(printf '%s' "$out" | tr -cd '0-9' | wc -c)"
    if [[ $rc -ne 0 || $(printf '%s' "$out" | tr -cd '0-9' | wc -c) -ne 158 ]]; then
        failures=$((failures + 1)); echo "  FAILED: protoclj did not print a 158-digit result"
    fi
    out=$("$BB" "$BENCH/factorial-100.clj" 2>&1); rc=$?
    echo "bb factorial-100: rc=$rc first line: $(printf '%s' "$out" | grep -m1 -i 'overflow' || printf '%s' "$out" | head -1)"
} > "$OUT/factorial-100.txt"

echo "failures: $failures" >> "$OUT/rounds.txt"
exit $(( failures > 0 ))
