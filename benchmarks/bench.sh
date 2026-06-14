#!/usr/bin/env bash
# protoClojure vs Babashka exploratory benchmark — wall-clock best-of-3.
# Each workload runs once per invocation; we measure cold-start + run.
# That penalises both runtimes equally for startup (bb ~50ms native,
# protoclj ~5ms).
#
# Usage: bench.sh [protoclj-binary] [bb-binary]

set -u
PROTOCLJ="${1:-$(dirname "$0")/../build_release/protoclj}"
BB="${2:-/tmp/proto-bench/bb}"

WORKLOADS=(fib tak sum-loop reduce-list sum-squares)
RUNS=3
HERE="$(dirname "$0")"

if [[ ! -x "$PROTOCLJ" ]]; then
    echo "protoclj not executable at: $PROTOCLJ" >&2; exit 2
fi
if [[ ! -x "$BB" ]]; then
    echo "bb not executable at: $BB" >&2; exit 2
fi

# Run a script via $1 (interpreter), $2 (script path), $RUNS times; print
# the minimum wall-clock in milliseconds. Also captures the script's
# stdout (first run only) for sanity-checking the result.
measure() {
    local interp="$1" script="$2"
    local best=""
    local result=""
    for ((i = 0; i < RUNS; ++i)); do
        local t0 t1
        t0=$(date +%s.%N)
        result=$("$interp" "$script" 2>&1)
        t1=$(date +%s.%N)
        local ms
        ms=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.0f", (b - a) * 1000 }')
        if [[ -z "$best" ]] || (( ms < best )); then best="$ms"; fi
    done
    printf "%s|%s" "$best" "$result"
}

printf "%-15s %12s %12s %10s %20s\n" \
    "workload" "protoclj(ms)" "bb(ms)" "ratio" "result"
echo   "--------------------------------------------------------------------------------"

for w in "${WORKLOADS[@]}"; do
    pcj=$(measure "$PROTOCLJ" "$HERE/$w.clj")
    bb=$(measure "$BB"        "$HERE/$w.clj")
    p_ms=${pcj%%|*}; p_out=${pcj#*|}
    b_ms=${bb%%|*};  b_out=${bb#*|}

    ratio=$(awk -v p="$p_ms" -v b="$b_ms" 'BEGIN {
        if (b == 0) print "inf"; else printf "%.2fx", p / b }')
    match="✓"
    if [[ "$p_out" != "$b_out" ]]; then match="✗"; fi

    # Strip newlines for one-line display
    short=$(printf "%s" "$p_out" | tr -d '\n' | cut -c1-18)
    printf "%-15s %12s %12s %10s   %-18s %s\n" \
        "$w" "$p_ms" "$b_ms" "$ratio" "$short" "$match"
done
