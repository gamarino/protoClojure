#!/usr/bin/env bash
#
# CLI check: a long list is collected cell by cell as it is walked.
#
# A Clojure list is a chain of immutable cells, four protoCore cells per
# element (docs/DESIGN.md, M44), so a list of 1,000,000 elements holds about
# 4,000,000 cells. Under a ceiling of 6,000,000 cells two such lists cannot be
# alive at once. Each program below builds a second list while it walks the
# first (rev), or builds and walks one list after another, without holding
# the head of the list it walks: it can only finish if the cells already
# walked are reclaimed while the walk goes on. The walk itself allocates
# nothing (rest of a list cell is a field), and the VM and the compiler let go
# of the head (locals clearing, cleared argument slots; M30).
#
# Assertions are on numbers: each program's computed sums, and the number of
# collection cycles the heap trace reports (a run that never collected would
# prove nothing). Two-directional: the same rev with the head held by a local
# needs both lists at once and must exhaust the heap.
#
# Usage: list-walk-under-heap-limit.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: list-walk-under-heap-limit.sh <protoclj>}"
export PROTOCORE_HEAP_TRACE=1 PROTOCORE_ADAPTIVE_HEAP=0 PROTOCORE_HEAP_LIMIT_CELLS=6000000

prelude='(defn make [n] (loop [i 0 acc ()] (if (< i n) (recur (inc i) (conj acc i)) acc)))
(defn rev [l] (loop [l l acc ()] (if (empty? l) acc (recur (rest l) (conj acc (first l))))))
(defn walk [l] (loop [l l acc 0] (if (empty? l) acc (recur (rest l) (+ acc (first l))))))'
programs=(
  '(println (walk (rev (make 1000000)))) (println (walk (rev (make 1000000))))'
  '(println (walk (make 1000000))) (println (walk (make 1000000))) (println (walk (make 1000000)))'
  '(println (reduce + (rev (make 1000000)))) (println (count (rev (make 1000000))))'
)
expected=(
  '499999500000 499999500000'
  '499999500000 499999500000 499999500000'
  '499999500000 1000000'
)

for i in "${!programs[@]}"; do
    p="$prelude ${programs[$i]}"
    out=$(printf '%s\n' "$p" | "$PROTOCLJ" /dev/stdin 2>&1); rc=$?
    got=$(grep -v '^protoCore heap:' <<<"$out" | tr '\n' ' ' | sed 's/ $//')
    cycles=$(grep '^protoCore heap:' <<<"$out" | tail -1 | sed -n 's/.* cycle=\([0-9]*\).*/\1/p')
    if [[ $rc -ne 0 || "$got" != "${expected[$i]}" ]]; then
        echo "FAIL: ${programs[$i]} -> exit $rc, printed '$got', expected '${expected[$i]}'"
        exit 1
    fi
    if [[ -z "$cycles" ]] || (( cycles < 2 )); then
        echo "FAIL: ${programs[$i]} -> ${cycles:-no} collection cycles; the ceiling was never reached"
        exit 1
    fi
    echo "OK ($cycles cycles): ${programs[$i]}"
done

held='(let [l (make 1000000)] (println (walk (rev l)) (count l)))'
out=$(printf '%s\n' "$prelude $held" | "$PROTOCLJ" /dev/stdin 2>&1); rc=$?
if [[ $rc -eq 0 ]] || ! grep -q "out of memory" <<<"$out"; then
    echo "FAIL (held): $held must exhaust the heap (exit $rc)"
    grep -v '^protoCore heap:' <<<"$out" | tail -3
    exit 1
fi
echo "OK (held, out of memory): $held"
