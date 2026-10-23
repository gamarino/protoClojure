#!/usr/bin/env bash
#
# CLI check: macros typed at the REPL. A `defmacro` and a use of it on the
# same line work, because the REPL compiles each form only after the one
# before it has run. macroexpand-1 shows an expansion. A redefinition takes
# effect for the forms typed afterwards. An error in one form is reported,
# and the REPL goes on with the next form of the same line, as Clojure's
# REPL does; a macro that raises names itself. Registered as the
# `cli/repl-macros` ctest case by tests/CMakeLists.txt.
#
# Usage: repl-macros.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: repl-macros.sh <protoclj>}"

err_file=$(mktemp)
trap 'rm -f "$err_file"' EXIT

out=$(printf '%s\n' \
    "(defmacro unless [t & body] (list 'if t nil (cons 'do body))) (unless false :ran)" \
    "(macroexpand-1 '(unless x (f)))" \
    "(defmacro m [] 1) (defn old [] (m)) (defmacro m [] 2) [(old) (m)]" \
    "(/ 1 0) :after-error" \
    "(defmacro bad [] (/ 1 0)) (bad) :after-bad" \
    "(eval (list 'unless false 7))" \
    "(defmacro twice [x] (list 'do x x)) (twice (println \"hi-twice\"))" \
    '`(foo ~(+ 1 2) str)' \
    "(defmacro my-or ([] nil) ([x] x) ([x & n] \`(let [or# ~x] (if or# or# (my-or ~@n))))) (let [or 9] (my-or nil or))" | timeout 90s "$PROTOCLJ" 2>"$err_file")
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
expect ':ran'
expect 'user=> (if x nil (do (f)))'
expect '[1 2]'
expect ':after-error'
expect ':after-bad'
expect 'user=> 7'
# Syntax-quote qualifies symbols at the REPL as in a file; my-or is hygienic.
expect 'user=> (user/foo 3 clojure.core/str)'
expect '9'
# docs/tutorial/10-repl.md: a macro defined and used on one line.
if [[ $(grep -c 'hi-twice' <<<"$out") -ne 2 ]]; then
    echo "FAIL: expected (twice (println ...)) to print twice"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    exit 1
fi
expect_err 'error: ArithmeticException: Divide by zero'
expect_err 'compile error: Syntax error macroexpanding bad: ArithmeticException: Divide by zero'
echo OK
