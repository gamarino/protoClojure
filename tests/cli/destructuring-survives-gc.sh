#!/usr/bin/env bash
#
# CLI check: destructuring under collection, as a file and typed at the REPL.
# The compiler rewrites a binding form into the symbol bindings it stands for
# (docs/DESIGN.md §10.1, M23) and compiles the rewritten forms; macros in the
# bodies run at compile time and allocate garbage meanwhile, under a
# 1,000,000-cell hard ceiling, so collection cycles run while the rewritten
# forms are being compiled: a form the collector reclaimed shows up as a
# wrong sum or a crash. The program covers fn parameters, a destructuring
# let and loop inside a function, a defmacro whose parameters destructure,
# and eval of a destructuring let in a loop, and its run-time destructuring
# allocates while it walks large collections.
#
# Registered as the `cli/destructuring-survives-gc` ctest case by
# tests/CMakeLists.txt.
#
# Usage: destructuring-survives-gc.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: destructuring-survives-gc.sh <protoclj>}"

program() {
    printf '(defn churn [n] (loop [i 0 acc 0] (if (< i n) (recur (+ i 1) (+ acc (count (map (fn [x] [x x]) [1 2 3 4 5 6 7 8])))) acc)))\n'
    printf '(defmacro wrap [& body] (churn 12000) (cons (quote do) body))\n'
    printf '(defmacro add-pair [[a b] {:keys [c] :or {c 0}}] (churn 12000) (list (quote +) a b c))\n'
    printf '(defn f [[a b & more] {:keys [c d] :as m}]\n'
    printf '  (wrap (let [[x & ys] (list a b c d) {:strs [s]} {"s" (count more)}]\n'
    printf '    (wrap (+ x (count ys) s (add-pair [1 2] {:c 3}) (count m))))))\n'
    printf '(defn walk [v] (loop [[x & xs] v acc 0] (if x (recur xs (+ acc (first x) (churn 1))) acc)))\n'
    printf '(defn evals [n] (loop [i 0 acc 0] (if (< i n) (recur (+ i 1) (+ acc (eval (quote (let [[p q] [1 2] {r :r} {:r 3}] (+ p q r)))))) acc)))\n'
    printf '(println (f [1 2 3 4 5] {:c 3 :d 4}) (walk (vec (map (fn [i] [i i]) (vec (reverse (list* 1 2 3 4 5 (list))))))) (evals 20))\n'
}

# f: x=1, 3 more names, s=3, add-pair 6, m has 2 entries = 15;
# walk sums 1..5 plus 8 per churn round = 15 + 40 = 55; evals 20 * 6.
expected="15 55 120"
check() {
    local mode="$1" out="$2" rc="$3"
    if [[ $rc -ne 0 ]]; then
        echo "FAIL ($mode): protoclj exited $rc under PROTOCORE_HEAP_LIMIT_CELLS=1000000"
        printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -5
        exit 1
    fi
    local cycles
    cycles=$(grep -c '^protoCore heap:' <<<"$out")
    if (( cycles < 3 )); then
        echo "FAIL ($mode): expected at least 3 collection cycles, saw $cycles"
        exit 1
    fi
    if ! grep -qF -- "$expected" <<<"$out"; then
        echo "FAIL ($mode): expected '$expected'"
        printf '%s\n' "$out" | grep -v '^protoCore heap:' | tail -5
        exit 1
    fi
    echo "OK ($mode, $cycles cycles)"
}
export PROTOCORE_HEAP_TRACE=1 PROTOCORE_ADAPTIVE_HEAP=0 PROTOCORE_HEAP_LIMIT_CELLS=1000000
out=$(program | "$PROTOCLJ" /dev/stdin 2>&1); check file "$out" $?
out=$(program | "$PROTOCLJ" 2>&1); check repl "$out" $?
