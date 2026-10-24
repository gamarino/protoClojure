#!/usr/bin/env bash
#
# CLI check: JSON and EDN typed at the REPL (macros brief, Part B stage 12):
# require with aliases, cheshire and clojure.data.json, read-string and
# clojure.edn/read-string, a JSON value typed over two lines, and errors that
# leave the REPL running. Registered as the `cli/repl-data-formats` ctest
# case by tests/CMakeLists.txt.
#
# Usage: repl-data-formats.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: repl-data-formats.sh <protoclj>}"

err_file=$(mktemp)
trap 'rm -f "$err_file"' EXIT

out=$(printf '%s\n' \
    "(require '[cheshire.core :as json] '[clojure.data.json :as dj] '[clojure.edn :as edn])" \
    '(json/parse-string "{\"a\":[1,2.5,null]}" true)' \
    '(json/generate-string [1 "x" nil])' \
    '(dj/read-str "{\"k\":1}" :key-fn keyword)' \
    '(dj/write-str "a/b")' \
    '(json/parse-string "{\"a\" 1}") :after-bad-json' \
    '(edn/read-string "{:a [1 #{2}]}")' \
    "(edn/read-string \"'x\") :after-bad-edn" \
    '(read-string "(+ 1 2)")' \
    '(eval (read-string "(+ 1 2)"))' \
    '(json/parse-string "[1,' \
    '2]")' \
    '(= {:x 1} (read-string (pr-str {:x 1})))' \
    | timeout 90s "$PROTOCLJ" 2>"$err_file")
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: REPL exited $rc (expected 0)"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    exit 1
fi
expect() {
    if ! grep -qF -- "$1" <<<"$out"; then
        echo "FAIL: expected '$1' in the output"
        echo "stdout:"; sed 's/^/  /' <<<"$out"
        exit 1
    fi
}
expect_err() {
    if ! grep -qF -- "$1" "$err_file"; then
        echo "FAIL: expected '$1' in the error output"
        echo "stderr:"; sed 's/^/  /' "$err_file"
        exit 1
    fi
}
expect 'user=> {:a [1 2.5 nil]}'
expect 'user=> "[1,\"x\",null]"'
expect 'user=> {:k 1}'
expect 'user=> "\"a\\/b\""'
expect_err "JSON error: expected ':' after an object key"
expect ':after-bad-json'
expect 'user=> {:a [1 #{2}]}'
expect_err "EDN does not support the reader macro '"
expect ':after-bad-edn'
expect 'user=> (+ 1 2)'
expect 'user=> 3'
expect '#_=> [1 2]'
expect 'user=> true'
echo OK
