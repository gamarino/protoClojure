#!/usr/bin/env bash
#
# CLI check: quote and syntax-quote typed at the REPL. The REPL prints each
# result readably after its prompt. A line that ends in a reader macro (`'`
# here) is incomplete: the REPL waits for the form on the next line, as
# Clojure's does. `~` outside a syntax-quote is a compile error that names
# the problem, and the session goes on. Registered as the `cli/repl-quote`
# ctest case by tests/CMakeLists.txt.
#
# Usage: repl-quote.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: repl-quote.sh <protoclj>}"

err_file=$(mktemp)
trap 'rm -f "$err_file"' EXIT

out=$(printf '%s\n' \
    "'(a \"s\" [1 :k])" \
    "(first '(x y))" \
    "(def v [2 3])" \
    '`(f ~@v ~(count v))' \
    "'" \
    'sym' \
    '~v' \
    "(symbol? 'sym)" | timeout 90s "$PROTOCLJ" 2>"$err_file")
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
expect 'user=> (a "s" [1 :k])'
expect 'user=> x'
expect 'user=> (f 2 3 2)'
# The quote on its own line waited for `sym` on the continuation prompt.
expect '#_=> sym'
expect 'user=> true'
if ! grep -qF 'unquote (~) used outside a syntax-quote' "$err_file"; then
    echo "FAIL: ~ outside a syntax-quote did not report a clear error"
    echo "stderr:"; sed 's/^/  /' "$err_file"
    exit 1
fi
echo OK
