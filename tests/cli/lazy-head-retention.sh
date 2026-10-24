#!/usr/bin/env bash
#
# CLI check: walking a lazy seq without holding its head runs in constant
# memory; holding the head keeps every element, as in Clojure.
#
# Each program realises 1,000,000 elements (several times the 300,000-cell
# ceiling once the cells of the seq are counted). The ones that do not hold
# the head must finish with a small last live set; the ones that hold it must
# abort with protoCore's out-of-memory error, as JVM Clojure fails with
# OutOfMemoryError. What lets the first group go (docs/DESIGN.md §10.2):
#   * lazy seqs are protoCore lazy objects, not mutable objects, so a
#     realised chain is not rooted by the global mutable table (M28);
#   * a local read for the last time is cleared (locals clearing), the VM
#     clears argument slots once a call has taken them, and the walking
#     functions (dorun, count, reduce, last, run!) drop their argument (M30).
#
# Usage: lazy-head-retention.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: lazy-head-retention.sh <protoclj>}"
export PROTOCORE_HEAP_TRACE=1 PROTOCORE_ADAPTIVE_HEAP=0 PROTOCORE_HEAP_LIMIT_CELLS=300000

released=(
  '(dorun (map inc (range 1000000))) (println "ok")'
  '(let [s (map inc (range 1000000))] (dorun s)) (println "ok")'
  '(defn walk [s] (dorun s)) (walk (map inc (range 1000000))) (println "ok")'
  '(println (if (= 500000500000 (reduce + (map inc (range 1000000)))) "ok" "wrong sum"))'
  '(defn f [] (let [s (map inc (range 1000000))] (doseq [x s] x))) (f) (println "ok")'
  '(println (if (= 1000000 (last (map inc (range 1000000)))) "ok" "wrong last"))'
)
held=(
  '(let [s (map inc (range 1000000))] (dorun s) (println (count s)))'
  '(def s (map inc (range 1000000))) (dorun s) (println "kept")'
)

for p in "${released[@]}"; do
    out=$(printf '%s\n' "$p" | "$PROTOCLJ" /dev/stdin 2>&1); rc=$?
    live=$(grep '^protoCore heap:' <<<"$out" | tail -1 | sed -n 's/.* L=\([0-9]*\).*/\1/p')
    if [[ $rc -ne 0 ]] || ! grep -qx ok <<<"$out"; then
        echo "FAIL (released): $p -> exit $rc"
        printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -3
        exit 1
    fi
    if [[ -z "$live" ]] || (( live > 50000 )); then
        echo "FAIL (released): $p -> last live set ${live:-unknown} cells, expected under 50,000"
        exit 1
    fi
    echo "OK (released, last live set $live cells): $p"
done
for p in "${held[@]}"; do
    out=$(printf '%s\n' "$p" | "$PROTOCLJ" /dev/stdin 2>&1); rc=$?
    if [[ $rc -eq 0 ]] || ! grep -q "out of memory" <<<"$out"; then
        echo "FAIL (held): $p must exhaust the heap (exit $rc)"
        printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -3
        exit 1
    fi
    echo "OK (held, out of memory): $p"
done
