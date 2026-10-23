#!/usr/bin/env bash
#
# CLI check: sets under collection, as a file and typed at the REPL. A set is
# an object holding a ProtoMap (src/runtime/SetOps.h); the program builds,
# grows, shrinks and combines sets in loops that allocate far more than the
# 1,000,000-cell hard ceiling, keeps some as map keys and elements of other
# sets, and checks every result: a set whose elements the collector reclaimed
# shows up as a wrong count or a crash. Registered as the
# `cli/sets-survive-gc` ctest case by tests/CMakeLists.txt.
#
# Usage: sets-survive-gc.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: sets-survive-gc.sh <protoclj>}"

program() {
    printf '(defn grow [n] (loop [i 0 s #{}] (if (< i n) (recur (+ i 1) (conj s (str "k" (if (< i 500) i 0)) [i])) s)))\n'
    printf '(defn shrink [s n] (loop [i 0 s s] (if (< i n) (recur (+ i 1) (disj s [i])) s)))\n'
    printf '(defn combine [n] (loop [i 0 acc 0] (if (< i n) (recur (+ i 1) (+ acc (count (union (set (map (fn [x] (+ x i)) [1 2 3 4])) (intersection #{1 2 3} (hash-set i 1 2)))))) acc)))\n'
    printf '(def big (grow 40000))\n'
    printf '(def keyed (into {} (map (fn [i] [#{i (str i)} i]) [1 2 3 4 5])))\n'
    printf '(println (count big) (count (shrink big 40000)) (combine 60000) (get keyed #{"3" 3}) (contains? #{#{:a [1 2]}} #{[1 2] :a}))\n'
}

# grow: 500 strings + 40000 vectors; shrink leaves the 500 strings; combine:
# |{i+1..i+4} U {1 2}| (U {3} when i = 3) is 4, 5, 6, 7 for i = 0..3 and 6
# after, 22 + 6 * 59996 = 359998.
expected="40500 500 359998 3 true"
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
