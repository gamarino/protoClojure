#!/usr/bin/env bash
#
# CLI check: a thread that waits for another thread does not hold up a
# collection.
#
# Two waits poll instead of blocking on a kernel object: reading a lazy seq
# another thread has claimed and is realising, and `deref` of an undelivered
# promise. A registered thread that polls with a bare OS sleep or yield stays
# in protoCore's stop-the-world quorum and never reaches a safepoint, so no
# collection can start; under a heap ceiling the thread it waits for then
# blocks for memory, and the program deadlocks. Both waits go through
# protoCore's `ProtoThread::yield` / `sleepFor` (3.1.0), which leave the
# quorum for the wait.
#
# Verified against the code: with `waitABit` (src/runtime/LazySeq.cpp) polling
# with a bare `std::this_thread::yield` / `sleep_for`, the lazy-seq case is
# killed by its timeout.
#
# Each case runs under a heap ceiling, checks its own premise (cycles really
# ran and reclaimed while it waited) and asserts the value it computed, so a
# crash or a killed child cannot read as a pass.
#
# Usage: waiters-park-for-gc.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: waiters-park-for-gc.sh <protoclj>}"

LIMIT=500000
DEADLINE=90
MIN_CYCLES=3
MIN_RECLAIMED=1500000

BODY='(defn allocate [n]
  (loop [i 0 acc 0]
    (if (>= i n) acc (recur (+ i 1) (+ acc (count (str "garbage-" i)))))))'

failures=0

check() {
    local name="$1" expected="$2" program="$3" out rc census cycles reclaimed answer
    out=$(printf '%s\n' "$program" | timeout $DEADLINE \
          env PROTOCLJ_GC_STATS=1 PROTOCORE_HEAP_LIMIT_CELLS=$LIMIT \
              "$PROTOCLJ" /dev/stdin 2>&1)
    rc=$?
    if [[ $rc -eq 124 ]]; then
        echo "FAIL [$name]: no progress in ${DEADLINE}s -- a waiting thread is"
        echo "      holding protoCore's stop-the-world quorum."
        failures=$((failures + 1)); return
    fi
    if [[ $rc -ne 0 ]]; then
        echo "FAIL [$name]: protoclj exited $rc under PROTOCORE_HEAP_LIMIT_CELLS=$LIMIT"
        printf '%s\n' "$out" | tail -4
        failures=$((failures + 1)); return
    fi
    answer=$(printf '%s\n' "$out" | grep -E '^[0-9]+$' | head -1)
    if [[ "$answer" != "$expected" ]]; then
        echo "FAIL [$name]: computed '$answer', expected $expected"
        printf '%s\n' "$out" | tail -4
        failures=$((failures + 1)); return
    fi
    census=$(printf '%s\n' "$out" | grep -F 'protoclj gc:' | tail -1)
    cycles=$(printf '%s\n' "$census" | sed -n 's/.*[[:space:]]cycles=\([0-9]*\).*/\1/p')
    reclaimed=$(printf '%s\n' "$census" | sed -n 's/.*reclaimed-total=\([0-9]*\).*/\1/p')
    if [[ -z "$cycles" || -z "$reclaimed" ]]; then
        echo "FAIL [$name]: no usable census line"
        failures=$((failures + 1)); return
    fi
    if (( cycles < MIN_CYCLES || reclaimed < MIN_RECLAIMED )); then
        echo "FAIL [$name]: premise not met -- $cycles cycles, $reclaimed cells reclaimed"
        echo "      $census"
        failures=$((failures + 1)); return
    fi
    echo "  ok [$name]: $census"
}

# 1. The main thread reads a lazy seq the future has claimed: the thunk
#    delivers `started` first, so by the time the main thread reads `s` the
#    future is realising it, and the realisation needs collections.
check "lazy-seq-claimed-by-another-thread" 13888890 "$BODY
(def started (promise))
(def s (lazy-seq (do (deliver started true) (cons (allocate 1000000) nil))))
(def f (future (first s)))
@started
(println (first s))"

# 2. deref of a promise delivered by a future that needs collections first.
check "promise-deref" 13888890 "$BODY
(def p (promise))
(future (deliver p (allocate 1000000)))
(println @p)"

if (( failures )); then
    echo "FAIL: $failures of 2 waiter cases failed"
    exit 1
fi
echo OK
