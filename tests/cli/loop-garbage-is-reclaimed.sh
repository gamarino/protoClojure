#!/usr/bin/env bash
#
# CLI check: the garbage a `loop` makes is reclaimed while the loop runs.
#
# protoCore reclaims nothing a context has not handed over: every cell a
# context allocates is chained onto that context's young generation, the
# collector's root scan records the chain head as a root and marks the whole
# chain, and the chain is submitted only when the context is destroyed or from
# a `ProtoContext::safepoint()` the embedder calls. The VM builds one context
# per frame, so a call that RETURNS shows the collector its garbage. A `loop`
# is one frame: its context lives as long as the loop, and before the VM had a
# safepoint a loop's garbage was pinned for the loop's whole duration. Cycles
# ran and reclaimed exactly zero cells.
#
# The workload below is one such loop. It allocates a fresh string per
# iteration and keeps two SmallIntegers, so essentially everything it
# allocates is garbage, and it allocates about six times the heap ceiling this
# check imposes. It can therefore only finish if collections reclaim.
#
# A ceiling is required, and is not a crutch: protoCore's collector is started
# only from its heap-limit enforcement path (`triggerGC` is advisory AND
# unreferenced inside protoCore since the allocation-budget trigger was
# reverted), so with no ceiling protoClojure grows the heap instead of
# collecting, whatever the VM submits. That is protoCore's policy and is
# recorded as a known issue in docs/STATUS.md; this check is about
# reclamation, which the ceiling makes observable.
#
# Two-directional, so it cannot pass by accident: it also runs the same
# workload with the VM's safepoint hook disabled and FAILS if that run
# succeeds. Verified: that run aborts with protoCore's
# "live set 196519 cells, last cycle reclaimed 0 — out of memory" (exit 134).
#
# The assertions are on numbers, not on "more than zero":
#   * the program's own computed answer, so a crash cannot read as a pass;
#   * cells reclaimed, against the garbage the workload actually creates;
#   * the live set of the last cycle, which must stay near the loop's real
#     live set rather than grow with the iteration count;
#   * the heap high-water mark.
#
# The program is generated on the fly and read from standard input, so the
# check writes no files. Registered as the `cli/loop-garbage-is-reclaimed`
# ctest case by tests/CMakeLists.txt.
#
# Usage: loop-garbage-is-reclaimed.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: loop-garbage-is-reclaimed.sh <protoclj>}"

ITERATIONS=400000
LIMIT=400000
# (count "garbage-0") + ... + (count "garbage-399999"): 8 + the digit count.
EXPECTED=5488890

# The workload allocates about 2,360,000 cells (measured with no ceiling: the
# heap grows to 2,621,440 from 262,144). Reclaiming less than this would mean
# most of the garbage is still pinned, which is the defect. Deliberately not a
# "> 0" bar: forcing a cycle is not the same as submitting a young generation,
# and a cycle that submits nothing still reclaims a few stray cells.
MIN_RECLAIMED=1500000
# Cycles must actually run, or "reclaimed" means nothing.
MIN_CYCLES=3
# The loop's real live set is two SmallIntegers plus the program's globals,
# bytecode and symbols — measured at 2,164 cells. This bounds the live set
# well below what the un-submitted chain reached (196,519 cells, the whole
# ceiling minus headroom) without being tight enough to be brittle.
MAX_LIVE=50000

program() {
    cat <<CLJ
;; ONE call frame. Every iteration builds a string that nothing outlives it
;; references; the only live values are the two loop bindings. The result is
;; printed so a crash or a truncated run cannot read as a pass.
(defn churn [n]
  (loop [i 0 acc 0]
    (if (>= i n)
      acc
      (recur (+ i 1) (+ acc (count (str "garbage-" i)))))))
(println (churn $ITERATIONS))
CLJ
}

run_workload() {   # $1: extra env assignment ("" for none)
    program | timeout 300 env PROTOCLJ_GC_STATS=1 \
        PROTOCORE_HEAP_LIMIT_CELLS=$LIMIT ${1:+"$1"} "$PROTOCLJ" /dev/stdin 2>&1
}

# --- 1. With the safepoint: the workload must finish, and reclaim ------------
out=$(run_workload "")
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: protoclj exited $rc under PROTOCORE_HEAP_LIMIT_CELLS=$LIMIT;"
    echo "      a loop whose garbage is reclaimed fits in the ceiling."
    printf '%s\n' "$out" | tail -5
    exit 1
fi

answer=$(printf '%s\n' "$out" | grep -E '^[0-9]+$' | head -1)
if [[ "$answer" != "$EXPECTED" ]]; then
    echo "FAIL: the workload computed '$answer', expected $EXPECTED"
    printf '%s\n' "$out" | tail -5
    exit 1
fi

census=$(printf '%s\n' "$out" | grep -F 'protoclj gc:' | tail -1)
if [[ -z "$census" ]]; then
    echo "FAIL: PROTOCLJ_GC_STATS=1 printed no census line"
    printf '%s\n' "$out" | tail -5
    exit 1
fi

field() { printf '%s\n' "$census" | sed -n "s/.*[[:space:]]$1=\([0-9]*\).*/\1/p"; }
cycles=$(field cycles)
reclaimed=$(field reclaimed-total)
live=$(field live-last)
heap=$(field heap)

fail=0
report() { echo "  $census"; }

if [[ -z "$cycles" || -z "$reclaimed" || -z "$live" || -z "$heap" ]]; then
    echo "FAIL: could not parse the census line"; report; exit 1
fi
if (( cycles < MIN_CYCLES )); then
    echo "FAIL: only $cycles collection cycles ran (need $MIN_CYCLES);"
    echo "      'cells reclaimed' says nothing without cycles."
    fail=1
fi
if (( reclaimed < MIN_RECLAIMED )); then
    echo "FAIL: $reclaimed cells reclaimed over the run, expected at least"
    echo "      $MIN_RECLAIMED — the workload creates about 2,360,000 cells of"
    echo "      garbage, so this means the loop's young generation is still"
    echo "      being pinned rather than submitted."
    fail=1
fi
if (( live > MAX_LIVE )); then
    echo "FAIL: the last cycle found $live cells live, over the $MAX_LIVE"
    echo "      budget; the loop's own live set is two SmallIntegers, so a"
    echo "      live set that grows with the iteration count is the defect."
    fail=1
fi
if (( heap > LIMIT )); then
    echo "FAIL: the heap reached $heap cells, above its $LIMIT ceiling"
    fail=1
fi
if (( fail )); then report; exit 1; fi

# --- 2. Without the safepoint: the workload must NOT finish ------------------
# Guards the check itself. If this run succeeds, the ceiling is too generous or
# the workload no longer allocates, and part 1 above proves nothing.
off_out=$(run_workload "PROTOCLJ_NO_GC_SAFEPOINT=1")
off_rc=$?
if [[ $off_rc -eq 0 ]]; then
    echo "FAIL: the workload also finished with PROTOCLJ_NO_GC_SAFEPOINT=1, so"
    echo "      it does not actually depend on the VM's GC safepoint. Either"
    echo "      the ceiling is too high or the loop stopped allocating."
    printf '%s\n' "$off_out" | tail -5
    exit 1
fi
if [[ $off_rc -eq 124 ]]; then
    echo "FAIL: the no-safepoint run timed out rather than exhausting the heap;"
    echo "      the check cannot tell a stall from the defect it targets."
    exit 1
fi

echo "OK ($census; without the safepoint: exit $off_rc)"
