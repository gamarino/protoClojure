#!/usr/bin/env bash
#
# CLI check: for, doseq with modifiers and format under collection, as a file
# and typed at the REPL. for builds its result through closures and
# intermediate lists (core.clj, __for-step), doseq loops through its
# modifiers, and format builds strings; the program runs them in loops that
# allocate far more than the 1,000,000-cell hard ceiling and checks every
# total: a value the collector reclaimed shows up as a wrong total or a
# crash. Registered as the `cli/for-format-survive-gc` ctest case by
# tests/CMakeLists.txt.
#
# Usage: for-format-survive-gc.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: for-format-survive-gc.sh <protoclj>}"

program() {
    printf '(defn grid [n] (count (for [x (range n) y (range n) :when (< y x) :let [s (format "%%d:%%d" x y)] :while (< (count s) 9)] s)))\n'
    printf '(defn rounds [n] (loop [i 0 acc 0] (if (< i n) (recur (+ i 1) (+ acc (grid 12))) acc)))\n'
    printf '(def total (atom 0))\n'
    printf '(doseq [i (range 20000) :let [s (format "%%05d|%%.2f" i 1.5)] :when (even? i) [a b] [(split s #"\\|")]] (swap! total + (count a) (count b)))\n'
    printf '(println (rounds 4000) @total)\n'
}

# grid 12: the pairs y < x of 0..11 = 66 per round; doseq: 10000 even i,
# each "00000" and "1.50" = 9 characters.
expected="264000 90000"
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
