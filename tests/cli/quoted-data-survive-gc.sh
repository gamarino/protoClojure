#!/usr/bin/env bash
#
# CLI check: quoted data survive collection. A quoted list, vector or map is
# the value the reader yielded, held by the compiled module and pinned in its
# root set (src/runtime/BytecodeModule.h); PUSH_CONST pushes that very
# object. The program below quotes a nested datum inside a function, then
# allocates garbage under a 1,000,000-cell hard ceiling until several
# collection cycles have run, and walks the datum again after every round.
# A datum the collector reclaimed shows up as a wrong sum or a crash. The
# program runs twice: as a file and typed at the REPL. In both, each form is
# read into a context of its own (docs/DESIGN.md §10.1, M9), so once its
# form has been compiled nothing but the module's root set holds the datum.
# Checked to fail when the module does not pin its data.
#
# The program is read from standard input. Registered as the
# `cli/quoted-data-survive-gc` ctest case by tests/CMakeLists.txt.
#
# Usage: quoted-data-survive-gc.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: quoted-data-survive-gc.sh <protoclj>}"

program() {
    printf "(defn datum [] '(1 [2 3] {:a 4} (5 (6 7)) \"eight\"))\n"
    printf '(defn walk [d] (+ (first d) (nth (nth d 1) 0) (nth (nth d 1) 1) (:a (nth d 2))\n'
    printf '                  (first (nth d 3)) (first (nth (nth d 3) 1)) (nth (nth (nth d 3) 1) 1)))\n'
    printf '(defn churn [n] (loop [i 0 acc 0]\n'
    printf '  (if (< i n)\n'
    printf '    (recur (+ i 1) (+ acc (count (map (fn [x] [x x]) [1 2 3 4 5 6 7 8])) (walk (datum))))\n'
    printf '    acc)))\n'
    printf '(println (churn 60000) (nth (datum) 4))\n'
}

# Each round adds 8 (the count) and 28 (the datum's sum).
expected="$(( 60000 * 36 )) eight"
check() {
    local mode="$1" out="$2" rc="$3"
    if [[ $rc -ne 0 ]]; then
        echo "FAIL ($mode): protoclj exited $rc under PROTOCORE_HEAP_LIMIT_CELLS=1000000"
        printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -5
        exit 1
    fi
    local cycles
    cycles=$(grep -c '^protoCore heap:' <<<"$out")
    if (( cycles < 2 )); then
        echo "FAIL ($mode): expected at least 2 collection cycles, saw $cycles"
        exit 1
    fi
    if ! grep -q -- "$expected" <<<"$out"; then
        echo "FAIL ($mode): expected '$expected'"
        printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -5
        exit 1
    fi
    echo "OK ($mode, $cycles cycles)"
}
export PROTOCORE_HEAP_TRACE=1 PROTOCORE_ADAPTIVE_HEAP=0 PROTOCORE_HEAP_LIMIT_CELLS=1000000
out=$(program | "$PROTOCLJ" /dev/stdin 2>&1); check file "$out" $?
out=$(program | "$PROTOCLJ" 2>&1); check repl "$out" $?
