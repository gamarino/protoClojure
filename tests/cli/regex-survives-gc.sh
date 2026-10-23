#!/usr/bin/env bash
#
# CLI check: regular expressions under collection, as a file and typed at the
# REPL. Pattern values, matchers and match results are protoCore objects
# (src/runtime/Regex.h); the program matches, splits and replaces in loops
# that allocate far more than the 1,000,000-cell hard ceiling, through
# re-seq, re-find on a matcher, split, and replace with a fn, and checks
# every count: a pattern or match the collector reclaimed shows up as a wrong
# count or a crash. Registered as the `cli/regex-survives-gc` ctest case by
# tests/CMakeLists.txt.
#
# Usage: regex-survives-gc.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: regex-survives-gc.sh <protoclj>}"

program() {
    printf '(def line "alpha=1 beta=22 gamma=333 delta=4444")\n'
    printf '(def pat (re-pattern "(\\\\w+)=(\\\\d+)"))\n'
    printf '(defn digits [n] (loop [i 0 acc 0] (if (< i n) (recur (+ i 1) (+ acc (count (re-seq #"\\d" line)) (count (split line #"\\s+")))) acc)))\n'
    printf '(defn walk [n] (loop [i 0 acc 0] (if (< i n) (recur (+ i 1) (let [m (re-matcher pat line)] (loop [k acc] (if (re-find m) (recur (+ k (count (nth (re-groups m) 2)))) k)))) acc)))\n'
    printf '(defn swap-all [n] (loop [i 0 acc 0] (if (< i n) (recur (+ i 1) (+ acc (count (replace line pat (fn [[_ k v]] (str v k)))))) acc)))\n'
    printf '(println (digits 30000) (walk 30000) (swap-all 30000))\n'
}

# digits: 10 digits + 4 pieces per round; walk: 1+2+3+4 digits per round;
# swap-all: the 36-byte line loses its four `=` (32).
expected="420000 300000 960000"
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
