#!/usr/bin/env bash
#
# protoClojure conformance-fixture runner.
#
# Black-box: runs each `.clj` program through the `protoclj` binary in its own
# process, captures stdout/stderr and the exit code, and compares the result
# against a directive in the first line of the file. Same pattern protoST uses
# in tests/conformance/run_conformance.sh.
#
# Usage:
#   run.sh <path-to-protoclj> <test-file.clj>
#
# Directive format (must be the first line, a Clojure line-comment starting
# with `;;` immediately followed by the directive name):
#
#   ;; EXPECT: <text>        the program must succeed (exit 0) and the last
#                            line of stdout must equal <text> exactly.
#
#   ;; EXPECT-ERROR           the program must fail (non-zero exit).
#   ;; EXPECT-ERROR: <text>   with text, the error output must contain <text>.
#
#   ;; XFAIL: <text>          a test expected to fail today. The body after
#   ;; XFAIL-ERROR...         `XFAIL:` is itself an EXPECT directive describing
#                             the spec-correct behaviour. The runner inverts
#                             the verdict: passes when the program does NOT
#                             match the expectation; fails loudly if it
#                             unexpectedly does (the deviation got fixed —
#                             the XFAIL marker should be removed).
#
# Exactly one CTest case is registered per file by tests/CMakeLists.txt, so one
# failure pins one non-conforming program.

set -u

PROTOCLJ="${1:?usage: run.sh <protoclj> <file.clj>}"
FILE="${2:?usage: run.sh <protoclj> <file.clj>}"

if [[ ! -x "$PROTOCLJ" ]]; then
    echo "FAIL: protoclj binary not executable: $PROTOCLJ"
    exit 1
fi
if [[ ! -f "$FILE" ]]; then
    echo "FAIL: test file not found: $FILE"
    exit 1
fi

# Parse the directive from the first line.
first_line=$(head -n 1 "$FILE")

# Strip leading whitespace, then expect `;;` followed by a directive name.
# Use bash-side parsing rather than sed so the helper has zero deps.
directive=""
expected=""
case "$first_line" in
    *EXPECT-ERROR:*)
        directive="EXPECT-ERROR"
        expected="${first_line#*EXPECT-ERROR: }"
        ;;
    *EXPECT-ERROR*)
        directive="EXPECT-ERROR"
        expected=""
        ;;
    *EXPECT:*)
        directive="EXPECT"
        expected="${first_line#*EXPECT: }"
        ;;
    *XFAIL-ERROR:*)
        directive="XFAIL-ERROR"
        expected="${first_line#*XFAIL-ERROR: }"
        ;;
    *XFAIL-ERROR*)
        directive="XFAIL-ERROR"
        expected=""
        ;;
    *XFAIL:*)
        directive="XFAIL"
        expected="${first_line#*XFAIL: }"
        ;;
    *)
        echo "FAIL: no recognised directive in first line of $FILE"
        echo "  first line was: $first_line"
        exit 1
        ;;
esac

# Run with the test file's directory as CWD so relative requires resolve.
FILE_DIR=$(cd "$(dirname "$FILE")" && pwd)
FILE_BASE=$(basename "$FILE")

# Capture stdout and stderr separately. Wall-clock timeout (90s) catches an
# infinite loop without hanging the suite forever.
stdout_file=$(mktemp)
stderr_file=$(mktemp)
trap 'rm -f "$stdout_file" "$stderr_file"' EXIT

cd "$FILE_DIR"
timeout 90s "$PROTOCLJ" "$FILE_BASE" \
    >"$stdout_file" 2>"$stderr_file"
exit_code=$?

# Convenience: the last non-empty line of stdout is what EXPECT: matches
# against (matches protoST's run_conformance.sh).
last_line=$(awk 'NF{ last=$0 } END{ print last }' "$stdout_file")

case "$directive" in
    EXPECT)
        if [[ $exit_code -ne 0 ]]; then
            echo "FAIL: program exited $exit_code (expected 0)"
            echo "stdout:"; sed 's/^/  /' "$stdout_file"
            echo "stderr:"; sed 's/^/  /' "$stderr_file"
            exit 1
        fi
        if [[ "$last_line" != "$expected" ]]; then
            echo "FAIL: stdout mismatch"
            echo "  expected: $expected"
            echo "  got:      $last_line"
            echo "full stdout:"; sed 's/^/  /' "$stdout_file"
            exit 1
        fi
        exit 0
        ;;
    EXPECT-ERROR)
        if [[ $exit_code -eq 0 ]]; then
            echo "FAIL: program exited 0 (expected non-zero)"
            echo "stdout:"; sed 's/^/  /' "$stdout_file"
            exit 1
        fi
        if [[ -n "$expected" ]]; then
            if ! grep -q -F -- "$expected" "$stderr_file" "$stdout_file"; then
                echo "FAIL: error output did not contain expected substring"
                echo "  expected (substring): $expected"
                echo "stderr:"; sed 's/^/  /' "$stderr_file"
                exit 1
            fi
        fi
        exit 0
        ;;
    XFAIL|XFAIL-ERROR)
        # Inverted: passes if the program does NOT match the spec-correct
        # expectation. The directive body describes the SPEC-correct
        # behaviour; we want to demonstrate today's program does not yet
        # match it.
        spec_directive="${directive#XFAIL}"  # "" or "-ERROR"
        # Re-run the EXPECT/EXPECT-ERROR check internally to learn whether
        # today's behaviour matches the spec. Inline the check rather than
        # re-execing the binary.
        if [[ -z "$spec_directive" ]]; then
            # XFAIL — spec-correct would EXPECT.
            if [[ $exit_code -eq 0 && "$last_line" == "$expected" ]]; then
                echo "FAIL: XFAIL passed unexpectedly — remove the XFAIL marker"
                exit 1
            fi
            exit 0
        else
            # XFAIL-ERROR — spec-correct would EXPECT-ERROR.
            if [[ $exit_code -ne 0 ]]; then
                if [[ -z "$expected" ]] || \
                   grep -q -F -- "$expected" "$stderr_file" "$stdout_file"; then
                    echo "FAIL: XFAIL-ERROR passed unexpectedly — remove the marker"
                    exit 1
                fi
            fi
            exit 0
        fi
        ;;
esac

echo "FAIL: internal: directive parsing fell through"
exit 1
