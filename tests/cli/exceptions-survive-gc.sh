#!/usr/bin/env bash
#
# CLI check: an exception value survives collection cycles while it is in
# flight.
#
# A thrown exception unwinds VM frames, and each frame's context hands its
# young generation to the collector as it is destroyed; the exception object,
# its message and its data map may have been allocated in exactly those
# frames. Until a catching frame stores it in a slot, only the in-flight pin
# (ClojureThrow, src/runtime/Exceptions.h) keeps it alive. The dangerous
# window is code that allocates while the exception is in flight: a future's
# worker records the error on the future after the body's frames are gone.
#
# The program throws ex-info values whose message and data are built fresh at
# the bottom of a recursion, on the main thread and inside futures, under a
# heap ceiling, and checks every caught exception for self-consistency.
#
# This is an end-to-end smoke test, NOT the proof of the pin: collections run
# only when the heap is exhausted, so one landing inside the few allocations
# of that window is rare, and the check was measured to pass with the pin
# disabled. The premise is proven deterministically by the unit test
# ExceptionsFixture.InFlightExceptionSurvivesCollections, which fails when
# the pin is disabled.
#
# Registered as the `cli/exceptions-survive-gc` ctest case by
# tests/CMakeLists.txt.
#
# Usage: exceptions-survive-gc.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: exceptions-survive-gc.sh <protoclj>}"
ROUNDS=40
PER_ROUND=50
TOTAL=$((ROUNDS * PER_ROUND * 2))
LIMIT=400000

program() {
    cat <<CLJ
(defn deep [n i]
  (if (= n 0)
    (throw (ex-info (str "failure-" i) {:i i :tag (str "payload-" i) :pair (list i i)}))
    (+ 1 (deep (- n 1) i))))
;; 1 when the exception still reads back exactly as it was built.
(defn consistent [e i]
  (let [d (ex-data e)]
    (if (and (= (ex-message e) (str "failure-" i))
             (= (:i d) i)
             (= (:tag d) (str "payload-" i))
             (= (:pair d) (list i i)))
      1
      0)))
(defn direct [i]
  (try (deep 20 i) (catch ExceptionInfo e (consistent e i))))
(defn via-future [i]
  (try @(future (deep 20 i))
       (catch ExecutionException e (consistent (ex-cause e) i))))
(defn churn [k]
  (loop [j 0 acc nil]
    (if (>= j k) acc (recur (+ j 1) (str "collectable-garbage-" j)))))
(defn round [base n]
  (loop [j 0 ok 0]
    (if (>= j n)
      ok
      (recur (+ j 1) (+ ok (direct (+ base j)) (via-future (+ base j)))))))
(defn rounds [r base ok]
  (if (= r 0)
    ok
    (let [got (round base $PER_ROUND)]
      (churn 2000)
      (rounds (- r 1) (+ base $PER_ROUND) (+ ok got)))))
(println (rounds $ROUNDS 0 0))
CLJ
}

out=$(program | PROTOCORE_HEAP_LIMIT_CELLS=$LIMIT timeout 300s "$PROTOCLJ" /dev/stdin 2>&1)
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: protoclj exited $rc under PROTOCORE_HEAP_LIMIT_CELLS=$LIMIT"
    printf '%s\n' "$out" | tail -5
    exit 1
fi
last=$(printf '%s\n' "$out" | awk 'NF{l=$0} END{print l}')
if [[ "$last" != "$TOTAL" ]]; then
    echo "FAIL: $last of $TOTAL exceptions read back intact"
    printf '%s\n' "$out" | tail -5
    exit 1
fi
echo "OK: $TOTAL exceptions intact under a ${LIMIT}-cell heap ceiling"
