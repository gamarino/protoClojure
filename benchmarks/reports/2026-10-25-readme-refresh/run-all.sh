#!/usr/bin/env bash
# Every measurement the README cites, in one session: bench.sh rounds against
# Babashka, start-up and peak RSS, the showcase scripts, actor throughput.
#
# Usage: run-all.sh PROTOCLJ BB DATA_DIR OUT_DIR
#
# gate.sh runs before every round of every part (IDLE_GATE, if set, inside
# it) and logs the load average and its wait to OUT/load.txt; each part's end
# is logged there too.
# Environment: BENCH_ROUNDS (5), SHOWCASE_ROUNDS (5), ACTOR_ROUNDS (3).
set -u
PROTOCLJ=${1:?usage: run-all.sh PROTOCLJ BB DATA_DIR OUT_DIR}
BB=${2:?}
DATA=${3:?}
OUT=${4:?}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
mkdir -p "$OUT"
export GATE_LOG="$OUT/load.txt"
GATE="$HERE/gate.sh"
status=0
done_part() { echo "$1 end $(date -u +%T) load $(cut -d' ' -f1-3 /proc/loadavg) status $2" >> "$GATE_LOG"; }

ROUND_GATE=$GATE ROUNDS=${BENCH_ROUNDS:-5} "$HERE/bench-rounds.sh" "$PROTOCLJ" "$BB" "$OUT/bench"; rc=$?
done_part bench $rc; (( rc )) && status=1

ROUND_GATE=$GATE python3 -I "$HERE/startup.py" "$PROTOCLJ" "$BB" "$REPO/examples" "$OUT/startup" 3 50; rc=$?
done_part startup $rc; (( rc )) && status=1

QUIET_GATE="$GATE showcase-round" RUNS=${SHOWCASE_ROUNDS:-5} \
    "$REPO/benchmarks/reports/2026-10-23-showcase/run.sh" "$PROTOCLJ" "$BB" "$DATA" "$OUT/showcase"; rc=$?
done_part showcase $rc; (( rc )) && status=1

for ((r = 1; r <= ${ACTOR_ROUNDS:-3}; ++r)); do
    "$GATE" "actors-r$r"
    "$REPO/benchmarks/actor-bench.sh" "$PROTOCLJ" > "$OUT/actors-r$r.txt" 2>&1; rc=$?
    grep -q FAILED "$OUT/actors-r$r.txt" && rc=1
    done_part "actors-r$r" $rc; (( rc )) && status=1
done

python3 -I "$HERE/analyze.py" "$OUT" | tee "$OUT/summary.md"
exit $status
