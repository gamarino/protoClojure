#!/usr/bin/env bash
#
# CLI check: primitives that call back into Clojure code, under collection.
#
# A primitive runs in its caller's context, with no context of its own
# (src/runtime/CallerSlots.h, docs/DESIGN.md §10.2, M45). What it holds
# across a callback is protected by the caller frame's young chain, by its
# arguments' slots, or by CallerSlots in the frame's spare slots, and a long
# loop hands the frame's garbage to the collector under a SafepointPermit.
# This program exercises each shape with callbacks that allocate: reduce
# over a range and over a list, a lazy map reduced, swap! in a loop,
# sort-by with a key fn, reduce inside a reduce callback, a primitive passed
# directly as the reducing fn (conj, called through invoke), run! and
# frequencies over a lazy map. It runs under a 1,000,000-cell hard ceiling
# with PROTOCORE_GC_CONTEXT_THRESHOLD=200, so contexts submit their young
# chains at nearly every safepoint and collection cycles run while the
# primitives hold their values: a value the collector reclaimed shows up as
# a wrong result or a crash. Every result is checked; the file and the REPL
# are both run.
#
# Registered as the `cli/primitive-callbacks-survive-gc` ctest case by
# tests/CMakeLists.txt.
#
# Usage: primitive-callbacks-survive-gc.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: primitive-callbacks-survive-gc.sh <protoclj>}"

program() {
    printf '%s\n' '(defn m-reduce [] (let [m (reduce (fn [m i] (assoc m (str "k" (mod i 100)) (vec (range (mod i 7))))) {} (range 60000))] (+ (count m) (reduce + (map count (vals m))))))'
    printf '%s\n' '(defn l-reduce [] (reduce (fn [acc s] (+ acc (count (str s "-" s)))) 0 (apply list (range 20000))))'
    printf '%s\n' '(defn lazy-map [] (reduce + (map (fn [i] (count (str "x" i))) (range 60000))))'
    printf '%s\n' '(defn atoms [] (let [a (atom [])] (dotimes [i 20000] (swap! a (fn [v] (if (> (count v) 50) [(str i)] (conj v (str i)))))) (count @a)))'
    printf '%s\n' '(defn sorted [] (first (sort-by (fn [i] (str (- 100000 i))) (range 5000))))'
    printf '%s\n' '(defn nested [] (reduce (fn [acc i] (+ acc (reduce (fn [a j] (+ a (count (str j)))) 0 (range (mod i 20))))) 0 (range 6000)))'
    printf '%s\n' '(defn direct [] (count (reduce conj [] (map str (range 40000)))))'
    printf '%s\n' '(defn runs [] (let [n (atom 0)] (run! (fn [i] (swap! n + (count (str i)))) (range 40000)) @n))'
    printf '%s\n' '(defn freqs [] (count (frequencies (map (fn [i] (str (mod i 1000))) (range 40000)))))'
    printf '%s\n' '(println (m-reduce) (l-reduce) (lazy-map) (atoms) (sorted) (nested) (direct) (runs) (freqs))'
}

expected="397 197780 348890 8 0 70500 40000 188890 1000"
check() {
    local mode="$1" out="$2" rc="$3"
    if [[ $rc -ne 0 ]]; then
        echo "FAIL ($mode): protoclj exited $rc under PROTOCORE_HEAP_LIMIT_CELLS=1000000"
        printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -5
        exit 1
    fi
    local cycles
    cycles=$(grep -c '^protoCore heap:' <<<"$out")
    if (( cycles < 3 )); then
        echo "FAIL ($mode): expected at least 3 collection cycles, saw $cycles"
        exit 1
    fi
    if ! grep -qF -- "$expected" <<<"$out"; then
        echo "FAIL ($mode): expected '$expected'"
        printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -5
        exit 1
    fi
    echo "OK ($mode, $cycles cycles)"
}
export PROTOCORE_HEAP_TRACE=1 PROTOCORE_ADAPTIVE_HEAP=0 PROTOCORE_HEAP_LIMIT_CELLS=1000000
export PROTOCORE_GC_CONTEXT_THRESHOLD=200
out=$(program | "$PROTOCLJ" /dev/stdin 2>&1); check file "$out" $?
out=$(program | "$PROTOCLJ" 2>&1); check repl "$out" $?
