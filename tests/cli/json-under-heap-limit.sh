#!/usr/bin/env bash
#
# CLI check: JSON is parsed and written under a heap ceiling (macros brief,
# Part B stage 12; src/runtime/Json.cpp).
#
# The parser builds each container in a context of its own and folds an
# object's entries into its map in batches inside short-lived contexts, so
# the intermediate versions a large document leaves behind are reclaimed
# while it is still being read; only the parsed values stay. Four runs:
#
#   * a 20,000-record document (1.8 MB, `\u` escapes in every record) parsed
#     whole with keyword keys under a 400,000-cell ceiling (25.6 MB): the
#     parse needs several collection cycles, and the checksums of the
#     records prove no value was lost or corrupted by them;
#   * the same records generated back to JSON, parsed again and compared
#     with the first parse, under a 700,000-cell ceiling (both parses are
#     live at once);
#   * 200,000 records of newline-delimited JSON (14 MB) read one value at a
#     time with parse-stream from standard input, with a key fn written in
#     Clojure (so every key goes through the VM while the parser holds its
#     state), under a 300,000-cell ceiling: memory stays flat;
#   * the whole document under a 150,000-cell ceiling, which its parsed
#     records alone exceed: protoCore must abort with "out of memory", so the
#     ceilings above are real.
#
# Usage: json-under-heap-limit.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: json-under-heap-limit.sh <protoclj>}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

awk -v n=20000 'BEGIN { printf "["; for (i = 1; i <= n; i++) { if (i > 1) printf ","; printf "{\"id\":%d,\"name\":\"user \\u00e9 %d\",\"tags\":[\"t1\",\"t2\",\"t3\"],\"score\":%d.5,\"active\":%s}\n", i, i, i % 100, (i % 3 == 0 ? "true" : "false") } printf "]\n" }' > "$TMP/doc.json"
awk -v n=200000 'BEGIN { for (i = 1; i <= n; i++) printf "{\"id\":%d,\"name\":\"user %d\",\"tags\":[\"t1\",\"t2\",\"t3\"],\"score\":%d.5}\n", i, i, i % 100 }' > "$TMP/records.ndjson"

cat > "$TMP/whole.clj" <<'CLJ'
(require '[cheshire.core :as json])
(def recs (json/parse-string (slurp (first *command-line-args*)) true))
(println "records" (count recs) "ids" (reduce + (map :id recs)) "active" (count (filter :active recs))
         "tags" (reduce + (map (comp count :tags) recs))
         "names" (count (filter #(clojure.string/starts-with? (:name %) "user é ") recs))
         "scores" (reduce + (map :score recs)))
CLJ
cat > "$TMP/again.clj" <<'CLJ'
(require '[cheshire.core :as json])
(def recs (json/parse-string (slurp (first *command-line-args*)) true))
(def again (json/parse-string (json/generate-string recs) true))
(println "equal" (= recs again) (count again))
CLJ
cat > "$TMP/stream.clj" <<'CLJ'
(require '[cheshire.core :as json])
(loop [n 0 ids 0 tags 0]
  (if-let [r (json/parse-stream *in* (fn [k] (keyword (str "k-" k))))]
    (recur (inc n) (+ ids (:k-id r)) (+ tags (count (:k-tags r))))
    (println "records" n "ids" ids "tags" tags)))
CLJ

export PROTOCORE_HEAP_TRACE=1 PROTOCORE_ADAPTIVE_HEAP=0
run() {  # name limit expected-line command...
    local name=$1 limit=$2 expected=$3; shift 3
    local out rc cycles
    out=$(PROTOCORE_HEAP_LIMIT_CELLS=$limit "$@" 2>&1); rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "FAIL ($name): exit $rc under PROTOCORE_HEAP_LIMIT_CELLS=$limit"
        printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -5
        exit 1
    fi
    cycles=$(grep -c '^protoCore heap:' <<<"$out")
    if (( cycles < 3 )); then
        echo "FAIL ($name): expected at least 3 collection cycles, saw $cycles"
        exit 1
    fi
    if ! grep -qxF "$expected" <<<"$out"; then
        echo "FAIL ($name): expected '$expected'"
        printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -5
        exit 1
    fi
    echo "ok ($name): $cycles cycles under $limit cells"
}
run whole 600000 "records 20000 ids 200010000 active 6666 tags 60000 names 20000 scores 1000000.0" \
    "$PROTOCLJ" "$TMP/whole.clj" "$TMP/doc.json"
run again 700000 "equal true 20000" "$PROTOCLJ" "$TMP/again.clj" "$TMP/doc.json"
run stream 300000 "records 200000 ids 20000100000 tags 600000" \
    sh -c '"$0" "$1" < "$2"' "$PROTOCLJ" "$TMP/stream.clj" "$TMP/records.ndjson"

out=$(PROTOCORE_HEAP_LIMIT_CELLS=150000 "$PROTOCLJ" "$TMP/whole.clj" "$TMP/doc.json" 2>&1); rc=$?
if [[ $rc -eq 0 ]] || ! grep -q 'out of memory' <<<"$out"; then
    echo "FAIL (control): the whole document under 150000 cells should run out of memory (exit $rc)"
    printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -3
    exit 1
fi
echo "PASS"
