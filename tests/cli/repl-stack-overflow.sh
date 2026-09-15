#!/usr/bin/env bash
#
# CLI check: a StackOverflowError in the REPL is reported like any other
# runtime error and the session continues. Unbounded recursion used to crash
# the process with SIGSEGV. Registered as the `cli/repl-stack-overflow`
# ctest case by tests/CMakeLists.txt.
#
# Usage: repl-stack-overflow.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: repl-stack-overflow.sh <protoclj>}"

err_file=$(mktemp)
trap 'rm -f "$err_file"' EXIT

out=$(printf '%s\n' \
    '(defn forever [n] (+ 1 (forever n)))' \
    '(forever 0)' \
    '(+ 40 2)' | timeout 90s "$PROTOCLJ" 2>"$err_file")
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: REPL exited $rc (expected 0)"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    echo "stderr:"; sed 's/^/  /' "$err_file"
    exit 1
fi
if ! grep -q "StackOverflowError" "$err_file"; then
    echo "FAIL: no StackOverflowError on standard error"
    echo "stderr:"; sed 's/^/  /' "$err_file"
    exit 1
fi
# Without a terminal the REPL prints its prompt on the result's line.
if ! grep -qE '(^|=> )42$' <<<"$out"; then
    echo "FAIL: the REPL did not evaluate the form after the error"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    exit 1
fi
echo OK
