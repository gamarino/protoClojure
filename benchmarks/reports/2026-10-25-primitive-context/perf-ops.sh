#!/usr/bin/env bash
# fib, tak and sum-loop under perf stat -r 10 (cycles, instructions), main
# against the branch, alternating, fresh copies: the cost of the operator
# redefinition check (M46) and of the frame's spare slots (M45) on the
# workloads the brief named. Usage: perf-ops.sh MAIN BRANCH LIB SCRATCH
set -u
MAIN=$1; BRANCH=$2; LIB=$3; SCRATCH=$4
B=$(cd "$(dirname "$0")/../.." && pwd)
for w in fib tak sum-loop; do
    for round in 1 2; do
        for v in main branch; do
            bin=$MAIN; [[ $v == branch ]] && bin=$BRANCH
            d=$(mktemp -d "$SCRATCH/ops.XXXX"); cp "$bin" "$d/protoclj"; cp -L "$LIB" "$d/libprotoCore.so.4"
            out=$(LD_LIBRARY_PATH=$d "$d/protoclj" "$B/$w.clj")
            echo -n "$w $v r$round result=$out "
            LD_LIBRARY_PATH=$d LC_ALL=C perf stat -r 10 -x, -e cycles,instructions "$d/protoclj" "$B/$w.clj" 2>&1 >/dev/null \
                | awk -F, '{printf "%s=%s(+-%s) ", $3, $1, $4}'
            echo
            rm -rf "$d"
        done
    done
done
