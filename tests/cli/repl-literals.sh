#!/usr/bin/env bash
#
# CLI check: literals typed at the REPL evaluate as they do in a file. The
# reader yields runtime values (src/reader/Reader.h): strings, keywords,
# vectors, maps, nil and booleans, and a map literal's forms are evaluated in
# source order. Each input line below is one form; the REPL prints each
# result after its prompt. Registered as the `cli/repl-literals` ctest case
# by tests/CMakeLists.txt.
#
# Usage: repl-literals.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: repl-literals.sh <protoclj>}"

err_file=$(mktemp)
trap 'rm -f "$err_file"' EXIT

out=$(printf '%s\n' \
    '"hi"' \
    '[:kw]' \
    '[1 "two" :three nil true false]' \
    '{:a [1 {:b 2}]}' \
    '(def ks [:c :b :a])' \
    '(def trace (atom ""))' \
    '(defn note [x] (swap! trace (fn [t] (str t x))) x)' \
    '(count {:a (note "a") :b (note "b") :c (note "c")})' \
    '@trace' \
    '(string? "x")' \
    '(vector? [1])' \
    '(map? {})' | timeout 90s "$PROTOCLJ" 2>"$err_file")
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: REPL exited $rc (expected 0)"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    exit 1
fi
if [[ -s "$err_file" ]]; then
    echo "FAIL: the REPL reported an error"
    echo "stderr:"; sed 's/^/  /' "$err_file"
    exit 1
fi
# Without a terminal the REPL prints its prompt on the result's line.
expect() {
    if ! grep -qF -- "user=> $1" <<<"$out"; then
        echo "FAIL: expected a result line '$1'"
        echo "stdout:"; sed 's/^/  /' <<<"$out"
        exit 1
    fi
}
expect '"hi"'
expect '[:kw]'
expect '[1 "two" :three nil true false]'
expect '{:a [1 {:b 2}]}'
expect '3'
expect '"abc"'
[[ $(grep -c 'user=> true$' <<<"$out") -eq 3 ]] || { echo "FAIL: a predicate was not true"; echo "$out"; exit 1; }
echo OK
