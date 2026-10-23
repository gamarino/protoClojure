#!/usr/bin/env bash
#
# CLI check: macro expansion under collection. A macro runs at compile time on
# the unevaluated forms of its call and returns a new form, which the compiler
# then compiles (docs/DESIGN.md §10.1, M10). The forms involved -- the call,
# the arguments, the `&env` map, the expansion and every expansion nested in
# it -- must stay reachable while the macro runs and while the expansion is
# compiled. The program below makes each macro allocate garbage while it
# holds a large form it built, under a 1,000,000-cell hard ceiling, so
# collection cycles run in the middle of expansions: a form the collector
# reclaimed shows up as a wrong sum or a crash. It covers an expansion inside
# a function (with locals, so `&env` is a map), nested expansions (a macro
# whose expansion calls macros), a recursive macro, a top-level `do` that
# defines a macro and uses it, and `eval` of a macro call in a loop. The
# program runs twice: as a file and typed at the REPL.
#
# The program is read from standard input. Registered as the
# `cli/macro-expansion-survives-gc` ctest case by tests/CMakeLists.txt.
#
# Usage: macro-expansion-survives-gc.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: macro-expansion-survives-gc.sh <protoclj>}"

program() {
    printf '(defn churn [n] (loop [i 0 acc 0] (if (< i n) (recur (+ i 1) (+ acc (count (map (fn [x] [x x]) [1 2 3 4 5 6 7 8])))) acc)))\n'
    printf '(defmacro sum-of-incs [n]\n'
    printf '  (let [items (loop [i 0 acc (list)] (if (< i n) (recur (+ i 1) (cons (list (quote inc) i) acc)) acc))\n'
    printf '        junk (churn 6000)]\n'
    printf '    (cons (quote +) items)))\n'
    printf '(defmacro wrap [& body] (churn 6000) (cons (quote do) body))\n'
    printf '(defmacro countdown [n] (if (= n 0) 0 (list (quote +) 1 (list (quote countdown) (- n 1)))))\n'
    printf '(defmacro env-size [] (churn 5000) (count &env))\n'
    printf '(defmacro then-sum [n] (list (quote do) (list (quote wrap) 0)\n'
    printf '  (cons (quote +) (loop [i 0 acc (list)] (if (< i n) (recur (+ i 1) (cons (list (quote inc) i) acc)) acc)))))\n'
    printf '(defn f [a b] (let [c 3] (wrap (wrap (+ (sum-of-incs 300) (env-size) (countdown 40) (then-sum 100))))))\n'
    printf '(do (defmacro late [] (churn 6000) (list (quote quote) [1 [2 3] {:k 4}])) (def late-v (late)))\n'
    printf '(defn evals [n] (loop [i 0 acc 0] (if (< i n) (recur (+ i 1) (+ acc (eval (quote (sum-of-incs 50))))) acc)))\n'
    printf '(println (f 1 2) (churn 30000) late-v (evals 20))\n'
}

# (sum-of-incs 300) = 45150, three locals, (countdown 40) = 40,
# (then-sum 100) = 5050 (its fresh sum is compiled after a nested expansion
# allocated garbage); churn 30000
# counts 8 per round; (sum-of-incs 50) = 1275 per eval.
expected="50243 240000 [1 [2 3] {:k 4}] $(( 20 * 1275 ))"
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
