#!/usr/bin/env bash
#
# CLI check: an exception no catch clause handles stops a script with exit
# status 1 and the runtime-error report on standard error, which names the
# exception's class; every finally block between the throw and the top level
# has run first. The REPL reports the same exception and keeps going.
# Registered as the `cli/uncaught-exception` ctest case by tests/CMakeLists.txt.
#
# Usage: uncaught-exception.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: uncaught-exception.sh <protoclj>}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat >"$work/uncaught.clj" <<'CLJ'
(defn inner [] (try (throw (ex-info "boom" {:code 7}))
                    (finally (println "inner finally"))))
(try (inner) (finally (println "outer finally")))
(println "not reached")
CLJ

out=$(cd "$work" && timeout 90s "$PROTOCLJ" uncaught.clj 2>"$work/err")
rc=$?
if [[ $rc -ne 1 ]]; then
    echo "FAIL: script exited $rc (expected 1)"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    echo "stderr:"; sed 's/^/  /' "$work/err"
    exit 1
fi
expected_out=$'inner finally\nouter finally'
if [[ "$out" != "$expected_out" ]]; then
    echo "FAIL: stdout mismatch (the finally blocks must run, innermost first)"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    exit 1
fi
if ! grep -qF "uncaught.clj: runtime error: ExceptionInfo: boom {:code 7}" "$work/err"; then
    echo "FAIL: stderr lacks the runtime-error report naming the class"
    echo "stderr:"; sed 's/^/  /' "$work/err"
    exit 1
fi

# The REPL reports the exception and evaluates the next form.
out=$(printf '%s\n' \
    '(throw (ex-info "repl-boom" {}))' \
    '(+ 40 2)' | timeout 90s "$PROTOCLJ" 2>"$work/err")
rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: REPL exited $rc (expected 0)"
    echo "stderr:"; sed 's/^/  /' "$work/err"
    exit 1
fi
if ! grep -qF "ExceptionInfo: repl-boom" "$work/err"; then
    echo "FAIL: the REPL did not report the exception"
    echo "stderr:"; sed 's/^/  /' "$work/err"
    exit 1
fi
if ! grep -qE '(^|=> )42$' <<<"$out"; then
    echo "FAIL: the REPL did not evaluate the form after the exception"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    exit 1
fi
echo OK
