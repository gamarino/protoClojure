#!/usr/bin/env bash
#
# CLI check: an HTTP server answering 400 requests whose handler makes garbage
# runs under a heap ceiling far below the total it allocates: each request's
# values are handed to the collector when the request ends, the handler stays
# pinned while the server runs, and collection cycles happen while connection
# threads are blocked in I/O.
# Registered as the `cli/http-server-under-heap-limit` ctest case.
#
# Usage: http-server-under-heap-limit.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: http-server-under-heap-limit.sh <protoclj>}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat >"$work/serve.clj" <<'CLJ'
(defn garbage [n]
  (loop [i 0 acc 0]
    (if (< i n) (recur (+ i 1) (+ acc (count (str "garbage-" i)))) acc)))
(defn handler [req]
  {:status 200 :body (str "sum=" (garbage 2000) " q=" (:query-string req))})
(def s (run-server handler {:port 0}))
(def base (str "http://127.0.0.1:" (server-port s) "/?"))
(defn run [i ok]
  (if (< i 400)
    (let [r (http-get (str base "n" i))]
      (recur (+ i 1) (if (ends-with? (:body r) (str " q=n" i)) (+ ok 1) ok)))
    ok))
(println "answered" (run 0 0))
(stop-server s)
CLJ

out=$(cd "$work" && PROTOCLJ_GC_STATS=1 PROTOCORE_HEAP_LIMIT_CELLS=200000 \
      timeout 90s "$PROTOCLJ" serve.clj 2>"$work/err")
rc=$?
if [[ $rc -ne 0 || "$out" != "answered 400" ]]; then
    echo "FAIL: rc=$rc"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    echo "stderr:"; sed 's/^/  /' "$work/err"
    exit 1
fi
cycles=$(grep -o 'cycles=[0-9]*' "$work/err" | head -1 | cut -d= -f2)
if [[ -z "$cycles" || "$cycles" -eq 0 ]]; then
    echo "FAIL: no collection cycle ran (the workload must need one)"
    sed 's/^/  /' "$work/err"
    exit 1
fi
echo "OK ($cycles cycles)"
