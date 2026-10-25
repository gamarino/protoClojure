#!/usr/bin/env bash
# Primitives in the caller's context (2026-10-25): main against the branch,
# and the branch without the VM's direct primitive calls (fastPrimitiveCall
# compiled out with -DPROTOCLJ_NO_FAST_PRIMITIVES), to decide M43's fate.
# Adapted from ../2026-10-24-seq-cost/run.sh (same cases, bench, start-up and
# showcase sections); the quiet gate here WAITS until the machine is quiet.
#
# Usage:
#   run.sh MAIN_BIN NOFAST_BIN BRANCH_BIN LIBPROTOCORE BB OUT SCRATCH
#
# Every run uses a fresh copy of its binary and of the protoCore library
# (the library file placement effect). The caller holds the machine's build
# lock for the whole run. Before each round the gate samples CPU idle over
# 10 s until it is at least 80 % with no compiler, make, ninja or ctest
# process (at most 120 attempts; a round that starts without a quiet sample
# is marked NOT-QUIET in gate.txt). REPS (default 5) rounds, the variant
# order rotating.
#
#   micro     prim-cost.clj: one primitive call per iteration (str, assoc,
#             get, max: outside the direct set; first: inside it; reduce
#             calling max), `work` and `setup`, perf stat cycles;
#   cases     ../2026-10-24-seq-cost/list-cost.clj, every case at each n;
#   bench     benchmarks/bench.sh, once per variant per round;
#   startup   an empty file, perf stat -r 50;
#   showcase  examples/showcase, wall time and peak RSS, outputs compared.
set -u
MAIN=${1:?usage: run.sh MAIN NOFAST BRANCH LIB BB OUT SCRATCH}
NOFAST=${2:?}; BRANCH=${3:?}; LIB=${4:?}; BB=${5:?}; OUT=${6:?}; SCRATCH=${7:?}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
SEQ=$HERE/../2026-10-24-seq-cost
REPS=${REPS:-5}
SECTIONS=${SECTIONS:-"micro cases bench startup showcase"}
NMICRO=${NMICRO:-1000000}
NS=${NS:-"10000 100000 1000000"}
CASES="1 2 3 4 5 50 6 60 7 8 9"
VARIANTS="main nofast branch"
mkdir -p "$OUT" "$SCRATCH"
declare -A BIN=([main]=$MAIN [nofast]=$NOFAST [branch]=$BRANCH)
orders=("main nofast branch" "nofast branch main" "branch main nofast")

{
    echo "date: $(date -u +%FT%TZ)"
    echo "host: $(uname -srm), $(nproc) cpus"
    for v in $VARIANTS; do
        echo "$v: ${BIN[$v]} ($(sha256sum "${BIN[$v]}" | cut -c1-16))"
        LD_LIBRARY_PATH=$(dirname "$LIB") ldd "${BIN[$v]}" | grep -i protocore
    done
    echo "library: $LIB ($(sha256sum "$LIB" | cut -c1-16))"
    echo "bb: $BB ($("$BB" --version 2>&1 | head -1))"
} > "$OUT/environment.txt"

gate() {
    local a b c d e f g h a2 b2 c2 d2 e2 f2 g2 h2 idle builds tries=0
    while :; do
        read -r _ a b c d e f g h _ < /proc/stat; sleep 10
        read -r _ a2 b2 c2 d2 e2 f2 g2 h2 _ < /proc/stat
        local t=$(( (a2+b2+c2+d2+e2+f2+g2+h2)-(a+b+c+d+e+f+g+h) ))
        idle=$(( 100*(d2-d)/t )); builds=$(pgrep -c -x 'cc1plus|cc1|cmake|make|gmake|ninja|ctest')
        tries=$((tries + 1))
        if (( idle >= 80 && builds == 0 )); then
            echo "$(date -u +%T) $1 idle=$idle% builds=$builds tries=$tries load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT/gate.txt"
            return
        fi
        if (( tries >= 120 )); then
            echo "$(date -u +%T) $1 NOT-QUIET idle=$idle% builds=$builds tries=$tries" >> "$OUT/gate.txt"
            return
        fi
    done
}

fresh() {
    local d; d=$(mktemp -d "$SCRATCH/fresh.XXXX")
    cp "${BIN[$1]}" "$d/protoclj"; cp -L "$LIB" "$d/libprotoCore.so.4"
    echo "$d"
}

perfline() { grep -v '^#' "$1" | grep -v '^$' | awk -F, '{printf "%s=%s ", $3, $1}'; }

if [[ " $SECTIONS " == *" micro "* ]]; then
    RAW=$OUT/micro.txt; : > "$RAW"
    for ((r = 1; r <= REPS; ++r)); do
        gate "micro r$r"
        for c in str assoc get max first reduce-max; do
            for v in ${orders[$(( (r - 1) % 3 ))]}; do
                for mode in work setup; do
                    F=$(fresh "$v")
                    LD_LIBRARY_PATH=$F LC_ALL=C perf stat -x, -o "$F/perf" -e cycles,instructions \
                        "$F/protoclj" "$HERE/prim-cost.clj" "$c" "$NMICRO" "$mode" > "$F/out" 2> "$F/err"
                    echo "PC $v rep=$r $(grep RESULT "$F/out") | $(perfline "$F/perf")" >> "$RAW"
                    rm -rf "$F"
                done
            done
        done
    done
fi

if [[ " $SECTIONS " == *" cases "* ]]; then
    RAW=$OUT/cases.txt; : > "$RAW"
    for ((r = 1; r <= REPS; ++r)); do
        gate "cases r$r"
        for n in $NS; do
            for c in $CASES; do
                for v in ${orders[$(( (r - 1) % 3 ))]}; do
                    for mode in work setup; do
                        F=$(fresh "$v")
                        LD_LIBRARY_PATH=$F PROTOCLJ_GC_STATS=1 LC_ALL=C perf stat -x, -o "$F/perf" \
                            -e cycles,instructions /usr/bin/time -f "maxrss=%M" \
                            "$F/protoclj" "$SEQ/list-cost.clj" "$c" "$n" "$mode" > "$F/out" 2> "$F/err"
                        echo "PC $v rep=$r $(grep RESULT "$F/out") | $(grep maxrss "$F/err") | $(perfline "$F/perf")" >> "$RAW"
                        rm -rf "$F"
                    done
                done
            done
        done
    done
fi

if [[ " $SECTIONS " == *" bench "* ]]; then
    for ((r = 1; r <= REPS; ++r)); do
        gate "bench r$r"
        for v in ${orders[$(( (r - 1) % 3 ))]}; do
            F=$(fresh "$v")
            ( cd "$REPO" && LD_LIBRARY_PATH=$F bash benchmarks/bench.sh "$F/protoclj" "$BB" \
                > "$OUT/bench_${v}_r$r.txt" 2>&1 )
            rm -rf "$F"
        done
    done
fi

if [[ " $SECTIONS " == *" startup "* ]]; then
    echo "" > "$SCRATCH/empty.clj"
    for ((r = 1; r <= REPS; ++r)); do
        gate "startup r$r"
        for v in ${orders[$(( (r - 1) % 3 ))]}; do
            F=$(fresh "$v")
            LD_LIBRARY_PATH=$F LC_ALL=C perf stat -r 50 -x, -e cycles,task-clock \
                "$F/protoclj" "$SCRATCH/empty.clj" 2>> "$OUT/startup_$v.txt" > /dev/null
            rm -rf "$F"
        done
    done
fi

if [[ " $SECTIONS " == *" showcase "* ]]; then
    SHOW=$REPO/examples/showcase
    DATA=$SCRATCH/data
    [[ -f "$DATA/access.log" ]] || python3 -I "$SHOW/gen-data.py" "$DATA" > /dev/null
    WORKLOADS=("startup|empty.clj|-|" "factorial|factorial.clj|-|" "grep|grep.clj|$DATA/access.log|"
               "json|json-transform.clj|-|$DATA/orders.json" "wordfreq|word-freq.clj|$DATA/corpus.txt|")
    mkdir -p "$OUT/showcase"
    for ((r = 1; r <= REPS; ++r)); do
        gate "showcase r$r"
        for w in "${WORKLOADS[@]}"; do
            IFS='|' read -r name script in arg <<<"$w"
            for v in ${orders[$(( (r - 1) % 3 ))]}; do
                tag="$OUT/showcase/${name}_${v}_r$r"
                F=$(fresh "$v"); cmd=("$F/protoclj" "$SHOW/$script")
                [[ -n "$arg" ]] && cmd+=("$arg")
                if [[ "$in" == "-" ]]; then
                    LD_LIBRARY_PATH=$F /usr/bin/time -v -o "$tag.time" "${cmd[@]}" < /dev/null > "$tag.out" 2> "$tag.err"
                else
                    LD_LIBRARY_PATH=$F /usr/bin/time -v -o "$tag.time" "${cmd[@]}" < "$in" > "$tag.out" 2> "$tag.err"
                fi
                echo $? > "$tag.rc"
                rm -rf "$F"
            done
        done
    done
    for f in "$OUT"/showcase/*.out; do sha256sum "$f" | cut -c1-16 > "${f%.out}.sha"; rm -f "$f"; done
    find "$OUT/showcase" -name '*.err' -size 0 -delete
fi
echo "done $(date -u +%T)" >> "$OUT/gate.txt"
