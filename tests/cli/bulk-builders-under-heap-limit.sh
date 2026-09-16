#!/usr/bin/env bash
#
# CLI check: the native bulk builders stay inside a bounded heap.
#
# Every primitive that grows a protoCore collection one element at a time
# leaves ~log2(N) cells of garbage behind per append (ProtoList is immutable:
# `appendLast` rebuilds one root-to-leaf path). protoCore only shows a
# context's allocations to the collector when that context is DESTROYED, so a
# primitive that appends N times inside one long-lived context pins every
# intermediate version until it returns — the heap fills with garbage that is
# unreachable but has never been submitted for collection.
#
# This check runs a program whose live data is a few hundred thousand cells
# under a 2,000,000-cell hard ceiling, but whose *intermediate* versions run
# to several million if they are not reclaimed as the build proceeds. It
# exercises a large collection built four different ways — a vector literal
# (reader + call-argument packing + `vector`), `reverse`, `map` and `filter` —
# and a large string (`join`). Before the bulk-builder fix every one of these
# aborted with "protoCore: heap hard limit ... out of memory".
#
# The program is generated on the fly and read from standard input, so the
# check writes no files. Registered as the `cli/bulk-builders-under-heap-limit`
# ctest case by tests/CMakeLists.txt.
#
# Usage: bulk-builders-under-heap-limit.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: bulk-builders-under-heap-limit.sh <protoclj>}"
N=50000
LAST=$((N - 1))

program() {
    printf '(def base ['
    seq 0 "$LAST" | awk '{ printf "%d ", $1 }'
    printf '])\n'
    printf '(def rev (reverse base))\n'
    printf '(def incd (map (fn [x] (+ x 1)) base))\n'
    printf '(def small (filter (fn [x] (< x 10)) base))\n'
    printf '(def joined (join "," base))\n'
    printf '(println (count base) (count rev) (count incd) (count small) (count joined))\n'
}

# The joined string is every element rendered in decimal, separated by one
# comma: sum of the decimal widths plus N-1 separators.
joined_len=$(awk -v n="$N" 'BEGIN {
    total = 0
    for (i = 0; i < n; i++) total += length(i "")
    print total + n - 1
}')
expected="$N $N $N 10 $joined_len"

out=$(program | PROTOCORE_HEAP_LIMIT_CELLS=2000000 "$PROTOCLJ" /dev/stdin 2>&1)
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: protoclj exited $rc under PROTOCORE_HEAP_LIMIT_CELLS=2000000"
    printf '%s\n' "$out" | tail -5
    exit 1
fi
if [[ "$(printf '%s\n' "$out" | awk 'NF { last = $0 } END { print last }')" != "$expected" ]]; then
    echo "FAIL: expected $expected"
    printf '%s\n' "$out" | tail -3
    exit 1
fi
echo OK
