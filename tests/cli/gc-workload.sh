# Sourced by the GC checks (loop-garbage-is-reclaimed.sh,
# blocking-joins-park-for-gc.sh, waiters-park-for-gc.sh): sizes their garbage
# by what the running protoCore allocates, not by a count tuned to one
# protoCore.
#
# Each check's workload is the loop
#
#     (loop [i 0 acc 0] (if (>= i n) acc (recur (+ i 1) (+ acc (count (str "garbage-" i))))))
#
# and its premise (enough collection cycles, enough cells reclaimed under the
# heap ceiling) depends on how many cells one iteration allocates. That is the
# kernel's business and changes between protoCore releases: 6.0 cells before
# protoCore 3.0.0, 4.0 from 3.0.0 (the reference the checks' iteration counts
# were chosen at), about 2.6 from 3.2.0 (a heap string is its own rope root,
# with no header cell). With a fixed count, the same check has a third less
# garbage on 3.2.0 and its premise fails although the collector works.
#
# gc_calibrate measures cells per iteration at run time: it runs the loop for
# CAL_ITERATIONS under a ceiling, so that collections run, and takes
# (cells reclaimed + half the ceiling) / iterations, the cells the cycles
# reclaimed plus the midpoint of what the last partial cycle can hold. The
# checks then scale their counts by 4.0 / measured, so the workload creates
# the garbage the check was designed for. Scaling only ever enlarges a
# workload (factor clamped to [1, 3]): nothing the checks assert is weakened.
#
# Functions:
#   gc_calibrate <protoclj>   sets GC_MILLI_CELLS (cells/iteration x 1000) and
#                             GC_SCALE_MILLI (scale factor x 1000); echoes a line
#   gc_scale <n>              n scaled, rounded up
#   gc_garbage_sum <n>        the loop's result for n: sum of (8 + digits(k)), k < n

GC_REFERENCE_MILLI=4000      # cells per iteration the checks' counts assume
CAL_ITERATIONS=1000000
CAL_LIMIT=400000

gc_calibrate() {
    local protoclj="$1" out census reclaimed
    out=$(printf '%s\n' "(defn churn [n] (loop [i 0 acc 0] (if (>= i n) acc (recur (+ i 1) (+ acc (count (str \"garbage-\" i)))))))
(println (churn $CAL_ITERATIONS))" | timeout 300 env PROTOCLJ_GC_STATS=1 \
          PROTOCORE_HEAP_LIMIT_CELLS=$CAL_LIMIT "$protoclj" /dev/stdin 2>&1)
    census=$(printf '%s\n' "$out" | grep -F 'protoclj gc:' | tail -1)
    reclaimed=$(printf '%s\n' "$census" | sed -n 's/.*reclaimed-total=\([0-9]*\).*/\1/p')
    if [[ -z "$reclaimed" ]] || (( reclaimed == 0 )); then
        # Nothing to measure (no census, or no cycle reclaimed anything): keep
        # the reference sizes; the check itself reports what went wrong.
        GC_MILLI_CELLS=$GC_REFERENCE_MILLI
    else
        GC_MILLI_CELLS=$(( (reclaimed + CAL_LIMIT / 2) * 1000 / CAL_ITERATIONS ))
        (( GC_MILLI_CELLS < 1 )) && GC_MILLI_CELLS=1
    fi
    GC_SCALE_MILLI=$(( (GC_REFERENCE_MILLI * 1000 + GC_MILLI_CELLS - 1) / GC_MILLI_CELLS ))
    (( GC_SCALE_MILLI < 1000 )) && GC_SCALE_MILLI=1000
    (( GC_SCALE_MILLI > 3000 )) && GC_SCALE_MILLI=3000
    echo "  calibration: $((GC_MILLI_CELLS / 1000)).$(printf '%03d' $((GC_MILLI_CELLS % 1000))) cells per iteration;" \
         "workloads scaled x$((GC_SCALE_MILLI / 1000)).$(printf '%03d' $((GC_SCALE_MILLI % 1000)))"
}

gc_scale() {
    echo $(( ($1 * GC_SCALE_MILLI + 999) / 1000 ))
}

gc_garbage_sum() {
    awk -v n="$1" 'BEGIN {
        total = 8 * n; lo = 0; hi = 10; d = 1
        while (lo < n) { top = (hi < n) ? hi : n; total += (top - lo) * d; lo = hi; hi *= 10; d++ }
        printf "%d\n", total }'
}
