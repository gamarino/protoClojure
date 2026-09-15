#!/usr/bin/env bash
#
# CLI check: source forms nested deeper than the thread's stack allows raise
# a read or compile error instead of crashing, in a script and in the REPL,
# and moderately deep nesting still evaluates. The reader and the compiler
# recurse once per level of nesting; 100,000 nested lists, or 40,000 nested
# `fn` forms (which read but do not compile), crashed the process with
# SIGSEGV. The programs are generated on the fly and read from standard
# input, so the check writes no files. Registered as the
# `cli/deeply-nested-source` ctest case by tests/CMakeLists.txt.
#
# Usage: deeply-nested-source.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: deeply-nested-source.sh <protoclj>}"

err_file=$(mktemp)
trap 'rm -f "$err_file"' EXIT

# `depth` nested copies of `open`, then `inner`, then `depth` closing parens
# or brackets.
nested() {
    local depth="$1" open="$2" inner="$3" close="$4"
    awk -v d="$depth" -v o="$open" -v i="$inner" -v c="$close" \
        'BEGIN { for (k = 0; k < d; k++) printf "%s", o; printf "%s", i;
                 for (k = 0; k < d; k++) printf "%s", c; print "" }'
}

fail() {
    echo "FAIL: $1"
    echo "stdout:"; sed 's/^/  /' <<<"$2" | head -5
    echo "stderr:"; sed 's/^/  /' "$err_file" | head -5
    exit 1
}

# Runs a script from standard input; expects exit status 1 and `pattern` on
# standard error.
expect_script_error() {
    local what="$1" pattern="$2" out rc
    out=$(cat | timeout 90s "$PROTOCLJ" /dev/stdin 2>"$err_file")
    rc=$?
    [[ $rc -eq 1 ]] || fail "$what: exit status $rc (expected 1)" "$out"
    grep -qF -- "$pattern" "$err_file" || fail "$what: no '$pattern' on standard error" "$out"
}

reads='read error: StackOverflowError: forms nested too deeply'
compiles='compile error: StackOverflowError: forms nested too deeply'

nested 100000 '(+ 1 ' 0 ')' | expect_script_error "100,000 nested lists" "$reads"
{ printf '(println '; nested 100000 '[' '' ']'; printf ')\n'; } |
    expect_script_error "100,000 nested vectors" "$reads"
nested 40000 '(fn [] ' 0 ')' | expect_script_error "40,000 nested fn forms" "$compiles"

# Nesting within the limit still reads, compiles and runs.
out=$(nested 20000 '(+ 1 ' 0 ')' | sed 's/^/(println /; s/$/)/' |
      timeout 90s "$PROTOCLJ" /dev/stdin 2>"$err_file")
rc=$?
[[ $rc -eq 0 && "$out" == "20000" ]] || fail "20,000 nested lists: exit $rc" "$out"

# The REPL reports both errors and keeps evaluating.
out=$({ nested 100000 '(+ 1 ' 0 ')'; echo '(+ 40 2)';
        nested 40000 '(fn [] ' 0 ')'; echo '(+ 40 3)'; } |
      timeout 90s "$PROTOCLJ" 2>"$err_file")
rc=$?
[[ $rc -eq 0 ]] || fail "REPL: exit status $rc (expected 0)" "$out"
grep -qF -- "$reads" "$err_file" || fail "REPL: no read error" "$out"
grep -qF -- "$compiles" "$err_file" || fail "REPL: no compile error" "$out"
# Without a terminal the REPL prints its prompt on the result's line.
grep -qE '(^|=> )42$' <<<"$out" || fail "REPL: no result after the read error" "$out"
grep -qE '(^|=> )43$' <<<"$out" || fail "REPL: no result after the compile error" "$out"
echo OK
