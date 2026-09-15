#!/usr/bin/env bash
#
# CLI check: in the REPL a global bound to nil resolves to nil, including
# `*1` before any evaluation (the session seeds it with nil). Both used to
# fail with "unable to resolve symbol". Registered as the
# `cli/repl-nil-global` ctest case by tests/CMakeLists.txt.
#
# Usage: repl-nil-global.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: repl-nil-global.sh <protoclj>}"

err_file=$(mktemp)
trap 'rm -f "$err_file"' EXIT

out=$(printf '%s\n' \
    '(nil? *1)' \
    '(def q nil)' \
    '[q (nil? q)]' | timeout 90s "$PROTOCLJ" 2>"$err_file")
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
if ! grep -qE '(^|=> )true$' <<<"$out"; then
    echo "FAIL: *1 did not read as nil before any evaluation"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    exit 1
fi
if ! grep -qE '(^|=> )\[nil true\]$' <<<"$out"; then
    echo "FAIL: a global bound to nil did not resolve"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    exit 1
fi
echo OK
