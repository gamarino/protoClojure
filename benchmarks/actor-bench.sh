#!/usr/bin/env bash
# protoClojure actor throughput probe.
#
# Runs the throughput script across multiple worker counts. The mode
# (single vs fan-out) is controlled inside actor-throughput.clj; this
# wrapper varies only PROTOCLJ_ACTOR_WORKERS.
set -u
PROTOCLJ="${1:-$(dirname "$0")/../build_release/protoclj}"
SCRIPT="$(dirname "$0")/actor-throughput.clj"
MESSAGES=100000

if [[ ! -x "$PROTOCLJ" ]]; then
    echo "protoclj not executable at $PROTOCLJ" >&2; exit 2
fi

FANOUT="$(dirname "$0")/actor-fanout.clj"

run_one() {
    local script="$1" mode="$2" workers="$3"
    local t0 t1 ms rate
    t0=$(date +%s.%N)
    PROTOCLJ_ACTOR_WORKERS="$workers" "$PROTOCLJ" "$script" > /dev/null 2>&1
    t1=$(date +%s.%N)
    ms=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.0f", (b - a) * 1000 }')
    rate=$(awk -v m="$MESSAGES" -v t="$ms" 'BEGIN {
        if (t == 0) print "inf"; else printf "%.0f", m / (t / 1000) }')
    printf "%-10s %-9s %-9s %-10s %-12s\n" "$mode" "$workers" "$MESSAGES" "$ms" "$rate"
}

printf "%-10s %-9s %-9s %-10s %-12s\n" "mode" "workers" "messages" "wall(ms)" "msg/s"
echo   "------------------------------------------------------------"

for W in 1 2 4 8 16; do run_one "$SCRIPT" "single" "$W";  done
echo
for W in 1 2 4 8 16; do run_one "$FANOUT" "fan-out" "$W"; done
