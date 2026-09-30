#!/usr/bin/env bash
#
# CLI check: the arguments after the script's path are bound, as strings and
# in order, to *command-line-args*, flags included; with none it is nil.
# Registered as the `cli/io-command-line-args` ctest case by tests/CMakeLists.txt.
#
# Usage: io-command-line-args.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: io-command-line-args.sh <protoclj>}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat >"$work/args.clj" <<'CLJ'
(println (count *command-line-args*))
(println (join "|" *command-line-args*))
(println (nil? *command-line-args*))
CLJ

out=$(cd "$work" && timeout 90s "$PROTOCLJ" args.clj one "two words" --flag -x "" 2>"$work/err")
rc=$?
expected=$'5\none|two words|--flag|-x|\nfalse'
if [[ $rc -ne 0 || "$out" != "$expected" ]]; then
    echo "FAIL: arguments after the script (rc=$rc)"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    echo "stderr:"; sed 's/^/  /' "$work/err"
    exit 1
fi

out=$(cd "$work" && timeout 90s "$PROTOCLJ" args.clj 2>"$work/err")
expected=$'0\n\ntrue'
if [[ "$out" != "$expected" ]]; then
    echo "FAIL: no arguments must bind nil"
    echo "stdout:"; sed 's/^/  /' <<<"$out"
    echo "stderr:"; sed 's/^/  /' "$work/err"
    exit 1
fi
echo OK
