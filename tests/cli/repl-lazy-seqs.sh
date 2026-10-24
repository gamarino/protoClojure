#!/usr/bin/env bash
#
# CLI check: lazy seqs at the REPL. Defining a lazy seq runs nothing: the
# definition echoes as the var, `#'user/s`, as in Clojure's REPL (M31). The
# REPL prints a result by realising it, so side effects of the body appear
# when the value is printed, once; a seq the REPL printed is not computed
# again. An endless seq can be defined and taken from.
#
# Usage: repl-lazy-seqs.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: repl-lazy-seqs.sh <protoclj>}"
input='(def s (map (fn [x] (println "realise" x) (* 10 x)) (list 1 2)))
(println "defined")
s
s
(def nat (iterate inc 0))
(take 3 nat)
(realized? (lazy-seq nil))
'
out=$(printf '%s' "$input" | "$PROTOCLJ" 2>&1)
expected="#'user/s
defined
realise 1
realise 2
(10 20)
(10 20)
#'user/nat
(0 1 2)
false"
got=$(sed 's/^user=> //' <<<"$out" | grep -v '^nil$')
if [[ "$got" != *"$expected"* ]]; then
    echo "FAIL: REPL output"
    echo "--- expected (in order):"; printf '%s\n' "$expected"
    echo "--- got:"; printf '%s\n' "$out"
    exit 1
fi
echo OK
