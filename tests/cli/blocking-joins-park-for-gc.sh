#!/usr/bin/env bash
#
# CLI check: a thread that blocks on a join leaves protoCore's running set.
#
# `ProtoThread::join` is a bare `std::thread::join` — it does NOT leave
# protoCore's running set. A registered thread that blocks there is still
# counted in `runningThreads` and never parks, so the stop-the-world quorum
# (`parkedThreads >= runningThreads`) can never be met and no collection cycle
# can start. Every thread that then needs memory waits for a cycle that cannot
# begin, and one of them is usually the thread being joined: a deadlock, not a
# slow shutdown.
#
# protoClojure blocked on four unbracketed joins — `deref` of a future, `pmap`,
# `shutdownFutures` and `ActorScheduler::shutdown` — and this fixture covers
# all four of them end to end. Verified against the code with all four
# `ProtoContext::UnmanagedScope` guards removed: every case below hangs and is
# killed by its timeout (exit 124). With them, each completes in a few seconds.
# The mechanism was read off a live backtrace of the first case: the main
# thread in `prim_deref -> std::thread::join -> pthread_clockjoin_ex`, the
# future's worker in `prim_str -> fromUTF8String -> waitForHeapHeadroom ->
# reclaimWaitLocked -> implSynchToGC`, and the collector idle in
# `gcThreadLoop`.
#
# A heap ceiling is what makes the bug reachable, because it is the only thing
# that starts a collection cycle at all (see the known issue in
# docs/STATUS.md). Each case therefore also checks its own premise: that cycles
# really ran and really reclaimed while it was waiting. And each case asserts
# the value its program computed, so a crash, a truncated run or a killed child
# cannot read as a pass.
#
# The programs are generated on the fly and read from standard input, so the
# check writes no files. Registered as the `cli/blocking-joins-park-for-gc`
# ctest case by tests/CMakeLists.txt.
#
# Usage: blocking-joins-park-for-gc.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: blocking-joins-park-for-gc.sh <protoclj>}"

LIMIT=500000
DEADLINE=90          # seconds; each case takes about 3 on a working build
MIN_CYCLES=3
MIN_RECLAIMED=1500000

# The allocating body every case shares: one fresh string per iteration, a
# SmallInteger accumulator. `(count (str "garbage-" i))` is 8 plus the digits
# of i, so the totals below are exact.
BODY='(defn allocate [n]
  (loop [i 0 acc 0]
    (if (>= i n) acc (recur (+ i 1) (+ acc (count (str "garbage-" i)))))))'

failures=0

# $1 case name, $2 expected first numeric line, $3 program text
check() {
    local name="$1" expected="$2" program="$3" out rc census cycles reclaimed answer
    out=$(printf '%s\n' "$program" | timeout $DEADLINE \
          env PROTOCLJ_GC_STATS=1 PROTOCORE_HEAP_LIMIT_CELLS=$LIMIT \
              "$PROTOCLJ" /dev/stdin 2>&1)
    rc=$?
    if [[ $rc -eq 124 ]]; then
        echo "FAIL [$name]: no progress in ${DEADLINE}s — a blocking join is"
        echo "      holding protoCore's stop-the-world quorum, so no collection"
        echo "      cycle can start and every thread that needs memory waits."
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
        echo "FAIL [$name]: no usable census line; cannot tell whether the"
        echo "      ceiling forced any collection, so the case proves nothing."
        failures=$((failures + 1)); return
    fi
    if (( cycles < MIN_CYCLES || reclaimed < MIN_RECLAIMED )); then
        echo "FAIL [$name]: premise not met — $cycles cycles, $reclaimed cells"
        echo "      reclaimed (need $MIN_CYCLES and $MIN_RECLAIMED). The case"
        echo "      passed without a thread ever having to wait for memory, so"
        echo "      it did not exercise the join at all."
        echo "      $census"
        failures=$((failures + 1)); return
    fi
    echo "  ok [$name]: $census"
}

# 1. `deref` of a future, while the future still needs collections to finish.
check "deref-future" 13888890 "$BODY
(def f (future (allocate 1000000)))
(println @f)"

# 2. `pmap`, which joins one thread per element in order: without the guard the
#    first element that needs a cycle deadlocks the whole call.
check "pmap" 12266670 "$BODY
(println (reduce + 0 (pmap allocate (list 300000 300000 300000))))"

# 3. ActorScheduler::shutdown, joining workers that are still draining. Only
#    the first message is awaited, so two are in flight when the script ends
#    and shutdown joins the pool.
check "actor-shutdown" 2688890 '(defn work [state n]
  (loop [i 0 acc state]
    (if (>= i n) acc (recur (+ i 1) (+ acc (count (str "garbage-" i)))))))
(def a (actor 0))
(println @(send a work 200000))
(send a work 200000)
(send a work 200000)'

# 4. shutdownFutures, which joins every future the script started whether or
#    not it was dereferenced. The future here is never awaited, so it is still
#    allocating when the script's last form has run.
check "shutdown-futures" 1288890 "$BODY
(def f (future (allocate 1000000)))
(println (allocate 100000))"

if (( failures )); then
    echo "FAIL: $failures of 4 join cases failed"
    exit 1
fi
echo OK
