#!/usr/bin/env bash
#
# CLI check: a grep-like script reads a stream larger than the heap ceiling.
#
# `line-seq` over `*in*` reads one line at a time (docs/DESIGN.md §10.2,
# M28-M30): a program that walks the lines without holding the head of the
# seq keeps only the current line alive, so it reads a stream of any length
# in constant memory. The stream here is 500,000 lines (about 30 MB, two
# cells or more per line) under a 300,000-cell ceiling (19.2 MB): reading it
# in memory would need several times the ceiling, and protoCore aborts with
# "out of memory" when the live set cannot fit. Three idioms are checked:
#
#   * doseq over (line-seq *in*), the way a Clojure grep is written;
#   * (count (filter ... (line-seq *in*))), where the walk is a primitive's
#     and the seq is an argument (releaseArg);
#   * a let-bound seq handed to a function that filters and counts it, which
#     holds the head unless the local is cleared at its last use (locals
#     clearing, M30).
#
# A fourth run holds the head on purpose -- the lines are counted twice --
# and must fail with protoCore's out-of-memory abort, as JVM Clojure fails
# with OutOfMemoryError: the ceiling is real, and the three runs above pass
# because they release the lines, not because the ceiling is loose.
#
# Usage: lazy-grep-under-heap-limit.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: lazy-grep-under-heap-limit.sh <protoclj>}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

lines=500000
stream() {
    awk -v n="$lines" 'BEGIN { for (i = 1; i <= n; i++) printf "line %d of the stream, with padding to make it longer\n", i }'
}
# Lines whose number contains 7777: 7777, 17777, ..., 77770-77779, ... (awk
# counts them the same way, so the expectation follows the stream).
expected=$(stream | grep -c 7777)

cat > "$TMP/doseq.clj" <<'CLJ'
(def n (atom 0))
(doseq [line (line-seq *in*)] (when (re-find #"7777" line) (swap! n inc)))
(println "matches" @n)
CLJ
cat > "$TMP/count.clj" <<'CLJ'
(println "matches" (count (filter (fn [l] (re-find #"7777" l)) (line-seq *in*))))
CLJ
cat > "$TMP/let.clj" <<'CLJ'
(defn matching [lines] (filter (fn [l] (re-find #"7777" l)) lines))
(let [lines (line-seq *in*)] (println "matches" (count (matching lines))))
CLJ
cat > "$TMP/held.clj" <<'CLJ'
(let [lines (line-seq *in*)] (println (count lines) (count lines)))
CLJ

export PROTOCORE_HEAP_TRACE=1 PROTOCORE_ADAPTIVE_HEAP=0 PROTOCORE_HEAP_LIMIT_CELLS=300000
for prog in doseq count let; do
    out=$(stream | "$PROTOCLJ" "$TMP/$prog.clj" 2>&1); rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "FAIL ($prog): exit $rc under PROTOCORE_HEAP_LIMIT_CELLS=300000"
        printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -5
        exit 1
    fi
    cycles=$(grep -c '^protoCore heap:' <<<"$out")
    if (( cycles < 3 )); then
        echo "FAIL ($prog): expected at least 3 collection cycles, saw $cycles"
        exit 1
    fi
    if ! grep -qx "matches $expected" <<<"$out"; then
        echo "FAIL ($prog): expected 'matches $expected'"
        printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -5
        exit 1
    fi
    echo "OK ($prog: $expected matches over $lines lines, $cycles cycles)"
done

out=$(stream | "$PROTOCLJ" "$TMP/held.clj" 2>&1); rc=$?
if [[ $rc -eq 0 ]] || ! grep -q "out of memory" <<<"$out"; then
    echo "FAIL (held): holding the head of $lines lines must exhaust a 300,000-cell heap (exit $rc)"
    printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -3
    exit 1
fi
echo "OK (held: out of memory, exit $rc, as JVM Clojure's OutOfMemoryError)"
