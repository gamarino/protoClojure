#!/usr/bin/env bash
#
# CLI check: the reader's runtime values survive collection while a large
# source is read and compiled under a small heap. The reader yields runtime
# values (src/reader/Reader.h): a map literal is a ProtoMap, a vector a boxed
# ProtoList, a keyword or symbol an interned named value and a string a
# protoCore string. The program below is one 20000-entry map literal whose
# values are small vectors, maps and strings, followed by many top-level
# forms full of literals, all under a 1,000,000-cell hard ceiling, so
# collections run while forms are read and compiled. Any form the collector
# reclaimed too early shows up as a wrong count or sum, or a crash.
#
# The program is generated on the fly and read from standard input. Registered
# as the `cli/reader-values-under-heap-limit` ctest case by tests/CMakeLists.txt.
#
# Usage: reader-values-under-heap-limit.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: reader-values-under-heap-limit.sh <protoclj>}"
N=20000

program() {
    printf '(def big {'
    seq 0 $((N - 1)) | awk '{ printf ":k%d [%d {:s \"v%d\"}] ", $1, $1, $1 }'
    printf '})\n'
    printf '(defn sum-firsts [m] (reduce + 0 (map (fn [k] (first (get m k))) (keys m))))\n'
    for i in $(seq 1 300); do
        printf '(def f%d [{:a %d :b "s%d"} [:x :y %d] {"k" [%d]}])\n' "$i" "$i" "$i" "$i" "$i"
    done
    printf '(println (count big) (sum-firsts big) (:s (nth (get big :k77) 1)) (:a (first f300)))\n'
}

expected="$N $(( (N - 1) * N / 2 )) v77 300"
out=$(program | PROTOCORE_ADAPTIVE_HEAP=0 PROTOCORE_HEAP_LIMIT_CELLS=1000000 "$PROTOCLJ" /dev/stdin 2>&1)
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: protoclj exited $rc under PROTOCORE_HEAP_LIMIT_CELLS=1000000"
    printf '%s\n' "$out" | tail -5
    exit 1
fi
last=$(printf '%s\n' "$out" | awk 'NF { last = $0 } END { print last }')
if [[ "$last" != "$expected" ]]; then
    echo "FAIL: expected '$expected', got '$last'"
    exit 1
fi
echo OK
