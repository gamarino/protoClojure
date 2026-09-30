#!/usr/bin/env bash
#
# CLI check: (exit n) ends the script at once with status n, after flushing
# what it printed; (exit) is status 0; an uncaught I/O exception is status 1.
# Registered as the `cli/io-exit-codes` ctest case by tests/CMakeLists.txt.
#
# Usage: io-exit-codes.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: io-exit-codes.sh <protoclj>}"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

check() {  # check <name> <expected-rc> <expected-stdout> <source>
    printf '%s\n' "$4" >"$work/$1.clj"
    out=$(cd "$work" && timeout 90s "$PROTOCLJ" "$1.clj" 2>"$work/err")
    rc=$?
    if [[ $rc -ne $2 || "$out" != "$3" ]]; then
        echo "FAIL: $1: rc=$rc (expected $2)"
        echo "stdout:"; sed 's/^/  /' <<<"$out"
        echo "stderr:"; sed 's/^/  /' "$work/err"
        exit 1
    fi
}

check exit7 7 "before" '(println "before") (exit 7) (println "after")'
check exit0 0 "x" '(println "x") (exit)'
check exit-in-fn 3 "" '(defn f [] (exit 3)) (f) (println "not reached")'
check exit-in-future 4 "" '@(future (exit 4)) (println "not reached")'
check uncaught-io 1 "" '(slurp "/nonexistent-dir-protoclj/x")'
if ! grep -q "FileNotFoundException" "$work/err"; then
    echo "FAIL: the uncaught I/O error must name FileNotFoundException"
    sed 's/^/  /' "$work/err"
    exit 1
fi
echo OK
