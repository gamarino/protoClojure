#!/usr/bin/env bash
# protoClojure actor throughput probe.
#
# Each mode is a separate .clj script that, internally, fires ~1M
# messages so the runs take 1-4 seconds and the wall-clock signal is
# well above noise. The runner times each script and reports msg/s.
#
#   single     — 1 actor, 1 sender thread, 1M trivial inc messages.
#                Floor of the per-actor pipeline.
#   fan-out    — 1000 actors × 1000 msgs/actor (single sender thread).
#                Stresses the global ready-queue scheduling overhead.
#   MPSC       — 4 sender threads (futures) × 250K msgs each → 1 actor.
#                Multi-producer / single-consumer — the lock-free
#                mailbox's structural advantage.
#   MPMC       — 4 sender threads × 4 consumer actors, round-robin.
#                Exercises both per-actor send contention and the
#                global ready queue with multiple workers.
#
# PROTOCLJ_ACTOR_WORKERS is varied across runs.
set -u
PROTOCLJ="${1:-$(dirname "$0")/../build_release/protoclj}"
SINGLE="$(dirname "$0")/actor-throughput.clj"
FANOUT="$(dirname "$0")/actor-fanout.clj"
MPSC="$(dirname "$0")/actor-mpsc.clj"
MPMC="$(dirname "$0")/actor-mpmc.clj"

if [[ ! -x "$PROTOCLJ" ]]; then
    echo "protoclj not executable at $PROTOCLJ" >&2; exit 2
fi

# Total message count per mode. Used only for the msg/s arithmetic;
# each .clj script enforces its own loop count.
MSGS_SINGLE=1000000
MSGS_FANOUT=1000000   # 1000 actors × 1000 msgs each
MSGS_MPSC=1000000     # 4 producers × 250K msgs
MSGS_MPMC=1000000     # 4 producers × 250K msgs (round-robin across 4 actors)

run_one() {
    local script="$1" mode="$2" workers="$3" msgs="$4"
    local t0 t1 ms rate output expected
    t0=$(date +%s.%N)
    # Capture stdout — every bench script ends with a `(actor-stats)`
    # printout, so the runner can verify the script reached the end
    # and processed the expected number of messages. Without this, a
    # silent compile error (script exits in ~15ms) would look like
    # "infinite throughput" — bit us once already on 2026-06-14.
    output=$(PROTOCLJ_ACTOR_WORKERS="$workers" "$PROTOCLJ" "$script" 2>&1)
    t1=$(date +%s.%N)
    ms=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.0f", (b - a) * 1000 }')

    expected=$(echo "$output" | grep -oE ':messages-processed [0-9]+' | awk '{print $2}')
    if [[ -z "$expected" || "$expected" -lt "$msgs" ]]; then
        printf "%-10s %-9s %-9s %-10s %-12s  ** FAILED: %s\n" \
            "$mode" "$workers" "$msgs" "$ms" "ERR" "$(echo "$output" | head -1)"
        return
    fi
    # Use the script's self-reported count as the divisor — guards
    # against off-by-one between bash MSGS and script TOTAL.
    rate=$(awk -v m="$expected" -v t="$ms" 'BEGIN {
        if (t == 0) print "inf"; else printf "%.0f", m / (t / 1000) }')
    printf "%-10s %-9s %-9s %-10s %-12s\n" "$mode" "$workers" "$expected" "$ms" "$rate"
}

printf "%-10s %-9s %-9s %-10s %-12s\n" "mode" "workers" "messages" "wall(ms)" "msg/s"
echo   "------------------------------------------------------------"

for W in 1 2 4 6 8 16; do run_one "$SINGLE" "single"  "$W" "$MSGS_SINGLE"; done
echo
for W in 1 2 4 6 8 16; do run_one "$FANOUT" "fan-out" "$W" "$MSGS_FANOUT"; done
echo
for W in 1 2 4 6 8 16; do run_one "$MPSC"   "MPSC"    "$W" "$MSGS_MPSC";   done
echo
for W in 1 2 4 6 8 16; do run_one "$MPMC"   "MPMC"    "$W" "$MSGS_MPMC";   done
