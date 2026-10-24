#!/usr/bin/env bash
#
# CLI check: the twenty-line script of tutorial chapter 14 ("A script in
# twenty lines"), shipped as examples/15-top-words.clj, run in a pipeline:
# ns with a library alias, #(...), mapcat over a lazy line-seq of *in*,
# frequencies, sort-by over juxt / comp, format, *command-line-args*.
#
# Usage: script-top-words.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: script-top-words.sh <protoclj>}"
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
script="$SRC/examples/15-top-words.clj"
out=$(printf 'The fox and the dog.\nThe end.\n' | "$PROTOCLJ" "$script" 2>&1); rc=$?
expected='the    3
and    1
dog    1'
if [[ $rc -ne 0 || "$out" != "$expected" ]]; then
    echo "FAIL: exit $rc"; printf '%s\n' "$out"; exit 1
fi
out=$(printf 'b a\n' | "$PROTOCLJ" "$script" --total 2>&1); rc=$?
if [[ $rc -ne 0 || "$out" != $'a      1\nb      1\ndone' ]]; then
    echo "FAIL (--total): exit $rc"; printf '%s\n' "$out"; exit 1
fi
echo OK
