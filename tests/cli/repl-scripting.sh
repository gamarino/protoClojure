#!/usr/bin/env bash
#
# CLI check: the scripting vocabulary typed at the REPL (macros brief, Part
# B): destructuring, sets, regular expressions, format / printf and for. Each
# line is one input line; the REPL prints every result. Registered as the
# `cli/repl-scripting` ctest case by tests/CMakeLists.txt.
#
# Usage: repl-scripting.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: repl-scripting.sh <protoclj>}"

err_file=$(mktemp)
trap 'rm -f "$err_file"' EXIT

out=$(printf '%s\n' \
    "(let [[a b & r] [1 2 3 4]] [a b r])" \
    "(defn area [{:keys [w h] :or {h 1}}] (* w h)) (area {:w 3})" \
    "(let [[a & b c] [1 2 3]] a) :after-bad-binding" \
    "(def s #{1 2}) [(contains? s 1) (s 3) (count (conj s 3))]" \
    "(= #{1 2} (into #{} [2 1 1]))" \
    "#{:a" ":b}" \
    "#{:dup :dup} :after-dup" \
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
expect 'user=> [1 2 (3 4)]'
expect '3'
expect ':after-bad-binding'
expect_err 'Unsupported binding form, only :as can follow & parameter'
expect '[true nil 3]'
expect 'user=> true'
# A set typed over two lines is one form; a repeated element is a read error.
if ! grep -qE '#\{(:a :b|:b :a)\}' <<<"$out"; then
    echo "FAIL: expected a two-line set #{:a :b} in the output"; sed 's/^/  /' <<<"$out"; exit 1
fi
expect_err 'Duplicate key: :dup'
echo OK
