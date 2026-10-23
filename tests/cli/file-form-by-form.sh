#!/usr/bin/env bash
#
# CLI check: a script is read, compiled and run one top-level form at a
# time, as the REPL and Clojure's `load` do (docs/DESIGN.md §10.1, M9). The
# forms before a failing one have run and printed; the failure stops the
# script with exit status 1 and names its position: a read error where
# reading stopped, a compile error where its top-level form starts. A clean
# script exits 0. Registered as the `cli/file-form-by-form` ctest case by
# tests/CMakeLists.txt.
#
# Usage: file-form-by-form.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: file-form-by-form.sh <protoclj>}"
PROTOCLJ="$(cd "$(dirname "$PROTOCLJ")" && pwd)/$(basename "$PROTOCLJ")"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && sed 's/^/  /' <<<"$2"; exit 1; }

run() {  # run <name> <source>: sets out, err, rc
    printf '%s' "$2" > "$work/$1.clj"
    out=$(cd "$work" && "$PROTOCLJ" "$1.clj" 2>"$work/err")
    rc=$?
    err=$(cat "$work/err")
}

# A compile error in the third form: the first two have run.
run compile '(println "one")
(def x 2)
  (def)
(println "never")
'
[[ $rc -eq 1 ]] || fail "compile error: exit status $rc, expected 1"
[[ "$out" == "one" ]] || fail "compile error: the forms before it did not run" "$out"
grep -qF 'compile.clj:3:3: compile error: def: expects (def name expr)' <<<"$err" \
    || fail "compile error: position or message" "$err"

# A read error in the second form: the first has run.
run read '(println "first")
(println "second"
'
[[ $rc -eq 1 ]] || fail "read error: exit status $rc, expected 1"
[[ "$out" == "first" ]] || fail "read error: the form before it did not run" "$out"
grep -qF 'read.clj:2:1: read error: unterminated list' <<<"$err" || fail "read error: position or message" "$err"

# A runtime error in the second form.
run runtime '(println "a")
(throw (ex-info "boom" {}))
(println "never")
'
[[ $rc -eq 1 ]] || fail "runtime error: exit status $rc, expected 1"
[[ "$out" == "a" ]] || fail "runtime error: output" "$out"
grep -qF 'runtime.clj: runtime error: ExceptionInfo: boom' <<<"$err" || fail "runtime error: message" "$err"

# A clean script: every form, in order, exit 0; a later form sees what an
# earlier one defined.
run clean '(def a 1)
(defn f [] (+ a 1))
(println (f))
(def a 10)
(println (f))
'
[[ $rc -eq 0 ]] || fail "clean script: exit status $rc" "$err"
[[ "$out" == $'2\n11' ]] || fail "clean script: output" "$out"
echo OK
