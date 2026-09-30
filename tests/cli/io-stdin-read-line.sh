#!/usr/bin/env bash
#
# CLI check: (read-line) reads standard input line by line and answers nil at
# its end, so a script works as a filter in a pipeline; at once-closed
# standard input the first read-line is nil.
# Registered as the `cli/io-stdin-read-line` ctest case by tests/CMakeLists.txt.
#
# Usage: io-stdin-read-line.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: io-stdin-read-line.sh <protoclj>}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat >"$work/upper.clj" <<'CLJ'
(defn pump [n]
  (let [line (read-line)]
    (if (nil? line)
      n
      (do (println (upper-case line)) (recur (+ n 1))))))
(println "lines:" (pump 0))
CLJ

out=$(printf 'one\ntwo\r\nthree' | timeout 90s "$PROTOCLJ" "$work/upper.clj" 2>"$work/err")
rc=$?
expected=$'ONE\nTWO\nTHREE\nlines: 3'
if [[ $rc -ne 0 || "$out" != "$expected" ]]; then
    echo "FAIL: filter over a pipe (rc=$rc)"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    echo "stderr:"; sed 's/^/  /' "$work/err"
    exit 1
fi

# Its output piped on to another program.
out=$(printf 'a\nb\n' | timeout 90s "$PROTOCLJ" "$work/upper.clj" 2>"$work/err" | tr 'A-Z' 'a-z' | head -n 2)
if [[ "$out" != $'a\nb' ]]; then
    echo "FAIL: pipeline through tr and head"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    exit 1
fi

out=$(timeout 90s "$PROTOCLJ" "$work/upper.clj" </dev/null 2>"$work/err")
if [[ "$out" != "lines: 0" ]]; then
    echo "FAIL: read-line at the end of standard input must be nil"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    echo "stderr:"; sed 's/^/  /' "$work/err"
    exit 1
fi
echo OK
