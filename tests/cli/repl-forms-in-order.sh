#!/usr/bin/env bash
#
# CLI check: the REPL reads, compiles and runs every form of an input, one
# at a time and in order, including several forms on one line (each result
# is printed); an error stops that input, and the session goes on. `:load`
# runs a file the same way and reports errors with the file's positions.
# Registered as the `cli/repl-forms-in-order` ctest case by
# tests/CMakeLists.txt.
#
# Usage: repl-forms-in-order.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: repl-forms-in-order.sh <protoclj>}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
printf '(def loaded 41)\n(def also (+ loaded 1))\n' > "$work/ok.clj"
printf '(def before-error 7)\n  (def)\n(def after-error 8)\n' > "$work/bad.clj"

out=$(printf '%s\n' \
    '(def a 1) (def b (+ a 1)) b' \
    '(def c 3) (def) (def d 4)' \
    'c' \
    ":load $work/ok.clj" \
    'also' \
    ":load $work/bad.clj" \
    'before-error' | timeout 90s "$PROTOCLJ" 2>"$work/err")
rc=$?
err=$(cat "$work/err")
fail() { echo "FAIL: $1"; echo "stdout:"; sed 's/^/  /' <<<"$out"; echo "stderr:"; sed 's/^/  /' <<<"$err"; exit 1; }
[[ $rc -eq 0 ]] || fail "REPL exited $rc"
# Three results from the first line, one per form, in order: a definition
# echoes as its var, as in Clojure's REPL (M31).
grep -qE "^user=> #'user/a$" <<<"$out" || fail "first form's result"
grep -qx "#'user/b" <<<"$out" || fail "second form's result"
grep -qx '2' <<<"$out" || fail "third form's result"
# The error in the middle of the second line: c was defined, d was not.
grep -qF 'compile error: def: expects (def name expr)' <<<"$err" || fail "no compile error"
grep -qE '^user=> 3$' <<<"$out" || fail "c was not defined before the error"
grep -qF "loaded $work/ok.clj (2 forms)" <<<"$out" || fail ":load of a clean file"
grep -qE '^user=> 42$' <<<"$out" || fail "a definition made by :load"
grep -qF "$work/bad.clj:2:3: compile error:" <<<"$err" || fail ":load error position"
grep -qE '=> 7$' <<<"$out" || fail ":load ran the forms before the error"
echo OK
