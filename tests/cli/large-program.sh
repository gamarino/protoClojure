#!/usr/bin/env bash
#
# CLI check: a program with 70,000 distinct literals of each kind compiles
# and runs, at top level and inside a function body, and equal literals of
# different kinds stay distinct. The constant pool, the bytecode operands
# and the operand stack have no practical limit below that size. The program
# is generated on the fly and read from standard input, so the check writes
# no files. Registered as the `cli/large-program` ctest case by
# tests/CMakeLists.txt.
#
# Usage: large-program.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: large-program.sh <protoclj>}"
N=70000
LAST=$((N - 1))

program() {
    printf '(def ints ['
    seq 0 "$LAST" | awk '{ printf "%d ", $1 }'
    printf '])\n(def strs ['
    seq 0 "$LAST" | awk '{ printf "\"s%d\" ", $1 }'
    printf '])\n(defn floats [] ['
    seq 0 "$LAST" | awk '{ printf "%d.5 ", $1 }'
    printf '])\n(def fs (floats))\n'
    printf '(println (str [1 1.0 "1" (quote a) "a" :a 1 1.0 "1" (quote a) "a" :a]))\n'
    printf '(println (count ints) (reduce + ints) (count strs) (nth strs %d) (count fs) (reduce + fs))\n' "$LAST"
}

out=$(program | "$PROTOCLJ" /dev/stdin 2>&1)
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: protoclj exited $rc"
    printf '%s\n' "$out" | tail -5
    exit 1
fi
expected_kinds='[1 1.0 "1" a "a" :a 1 1.0 "1" a "a" :a]'
# The float sum is 2.45e9, printed in JVM Clojure's scientific notation.
expected_sums="$N 2449965000 $N s$LAST $N 2.45E9"
if [[ "$(printf '%s\n' "$out" | sed -n 1p)" != "$expected_kinds" ]]; then
    echo "FAIL: literal kinds: expected $expected_kinds"
    printf '%s\n' "$out" | head -3
    exit 1
fi
if [[ "$(printf '%s\n' "$out" | sed -n 2p)" != "$expected_sums" ]]; then
    echo "FAIL: expected $expected_sums"
    printf '%s\n' "$out" | head -3
    exit 1
fi
echo OK
