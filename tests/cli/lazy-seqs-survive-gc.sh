#!/usr/bin/env bash
#
# CLI check: lazy sequences under collection, as a file and typed at the
# REPL. Lazy seqs, cons cells, chunks and native steps are protoCore objects
# (src/runtime/LazySeq.h); the program builds and walks pipelines that
# allocate far more than the 1,000,000-cell hard ceiling -- filter and map
# over a chunked range, a nested for with :when, map over two endless seqs,
# a recursive lazy-seq, re-seq over a long string -- and reads a seq it
# realised (doall) and kept, after every one of those, so a cached element
# the collector freed shows up as a wrong count or a crash. Every result is
# checked. Registered as the `cli/lazy-seqs-survive-gc` ctest case.
#
# Usage: lazy-seqs-survive-gc.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: lazy-seqs-survive-gc.sh <protoclj>}"

program() {
    printf '(defn squares [] (reduce + (map (fn [x] (* x x)) (filter odd? (range 200000)))))\n'
    printf '(defn pairs [] (count (for [x (range 300) y (range 300) :when (= 300 (+ x y))] [x y])))\n'
    printf '(defn zipped [] (reduce + (take 1000 (map + (iterate inc 0) (cycle [1 2 3])))))\n'
    printf '(defn nat [n] (lazy-seq (cons n (nat (inc n)))))\n'
    printf '(defn naturals [] (reduce + (take 100000 (nat 0))))\n'
    printf '(defn numbers [] (count (re-seq #"\\d+" (apply str (map (fn [i] (str i " ")) (range 50000))))))\n'
    printf '(def held (doall (map str (range 20000))))\n'
    printf '(defn joined [] (count (apply str held)))\n'
    printf '(println (squares) (pairs) (zipped) (naturals) (numbers) (joined) (squares) (joined))\n'
}

expected="1333333333300000 299 501499 4999950000 50000 88890 1333333333300000 88890"
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
out=$(program | "$PROTOCLJ" /dev/stdin 2>&1); check file "$out" $?
out=$(program | "$PROTOCLJ" 2>&1); check repl "$out" $?
