#!/usr/bin/env bash
# Lists as ProtoLinkedList (2026-10-25): main against itself (an A/A
# control: the same binary, so the difference is the noise floor) against B
# (lists as protoCore 3.3.0 ProtoLinkedLists), with Babashka for reference.
# Copied from ../2026-10-24-seq-cost/run.sh; case 10, `(vec list)`, added. Every variant runs on a fresh copy of its binary and of the
# protoCore library for every run, so no run reuses a file another run
# loaded (the library file placement effect).
#
# Usage:
#   run.sh MAIN_BIN A_BIN B_BIN LIBPROTOCORE BB [OUT] [SCRATCH]
#
#   *_BIN         protoclj binaries built Release from clean worktrees
#   LIBPROTOCORE  the libprotoCore.so.4 they link (checked with ldd, out/ldd.txt)
#   BB            a Babashka binary
#   OUT           raw results (default ./out)
#   SCRATCH       where the fresh copies and the showcase inputs go
#                 (default: a directory under /tmp; nothing is left there)
#
# Environment: REPS (default 5) rounds; SECTIONS (default "cases bench
# startup showcase"); NS (default "10000 100000 1000000").
#
#   cases     list-cost.clj: each case at each n, `work` and `setup` runs
#             (the difference is the case's own cost), perf stat cycles and
#             instructions, protoCore GC statistics and peak RSS; Babashka once
#             per case and n (its own timer, no perf);
#   bench     benchmarks/bench.sh (best of 3 per workload) once per variant
#             per round;
#   startup   an empty file, perf stat -r 50, once per variant per round;
#   showcase  examples/showcase (wall time and peak RSS, /usr/bin/time -v),
#             every runtime per round, outputs compared.
#
# The variant order rotates every round. analyze.py summarises out/.
set -u
MAIN=${1:?usage: run.sh MAIN_BIN A_BIN B_BIN LIBPROTOCORE BB [OUT] [SCRATCH]}
ABIN=${2:?}; BBIN=${3:?}; LIB=${4:?}; BB=${5:?}
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
OUT=${6:-$HERE/out}
SCRATCH=${7:-$(mktemp -d /tmp/linked-list.XXXX)}
REPS=${REPS:-5}
SECTIONS=${SECTIONS:-"cases bench startup showcase"}
NS=${NS:-"10000 100000 1000000"}
CASES="1 2 3 4 5 50 6 60 7 8 9 10"
mkdir -p "$OUT" "$SCRATCH"
declare -A BIN=([main]=$MAIN [A]=$ABIN [B]=$BBIN)
orders=("main A B" "A B main" "B main A")

{
    echo "date: $(date -u +%FT%TZ)"
    echo "host: $(uname -srm), $(nproc) cpus"
    for v in main A B; do
        echo "$v: ${BIN[$v]}"
        LD_LIBRARY_PATH=$(dirname "$LIB") ldd "${BIN[$v]}" | grep -i protocore
    done
    echo "library: $LIB ($(sha256sum "$LIB" | cut -c1-16))"
    echo "bb: $BB ($("$BB" --version 2>&1 | head -1))"
} > "$OUT/environment.txt"

# CPU idle over 10 s and the build processes running, before each round.
gate() {
    local a b c d e f g h a2 b2 c2 d2 e2 f2 g2 h2
    read -r _ a b c d e f g h _ < /proc/stat; sleep 10
    read -r _ a2 b2 c2 d2 e2 f2 g2 h2 _ < /proc/stat
    local t=$(( (a2+b2+c2+d2+e2+f2+g2+h2)-(a+b+c+d+e+f+g+h) ))
    echo "$(date -u +%T) $1 idle=$(( 100*(d2-d)/t ))% builds=$(pgrep -c -x 'cc1plus|cc1|cmake|make|ninja|ctest') load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT/gate.txt"
}

# A fresh directory holding copies of variant $1's binary and library.
fresh() {
    local d; d=$(mktemp -d "$SCRATCH/fresh.XXXX")
    cp "${BIN[$1]}" "$d/protoclj"; cp -L "$LIB" "$d/libprotoCore.so.4"
    echo "$d"
}

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
                            "$F/protoclj" "$HERE/list-cost.clj" "$c" "$n" "$mode" > "$F/out" 2> "$F/err"
                        res=$(grep RESULT "$F/out")
                        gc=$(grep "protoclj gc" "$F/err" | sed 's/protoclj gc: //')
                        rss=$(grep maxrss "$F/err")
                        pf=$(grep -v '^#' "$F/perf" | grep -v '^$' | awk -F, '{printf "%s=%s ", $3, $1}')
                        echo "PC $v rep=$r $res | $gc | $rss | $pf" >> "$RAW"
                        rm -rf "$F"
                    done
                done
                echo "BB rep=$r $("$BB" "$HERE/list-cost.clj" "$c" "$n" work | grep RESULT)" >> "$RAW"
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
            for v in ${orders[$(( (r - 1) % 3 ))]} bb; do
                tag="$OUT/showcase/${name}_${v}_r$r"
                if [[ $v == bb ]]; then F=""; cmd=("$BB" "$SHOW/$script")
                else F=$(fresh "$v"); cmd=("$F/protoclj" "$SHOW/$script"); fi
                [[ -n "$arg" ]] && cmd+=("$arg")
                if [[ "$in" == "-" ]]; then
                    LD_LIBRARY_PATH=$F /usr/bin/time -v -o "$tag.time" "${cmd[@]}" < /dev/null > "$tag.out" 2> "$tag.err"
                else
                    LD_LIBRARY_PATH=$F /usr/bin/time -v -o "$tag.time" "${cmd[@]}" < "$in" > "$tag.out" 2> "$tag.err"
                fi
                echo $? > "$tag.rc"
                [[ -n "$F" ]] && rm -rf "$F"
            done
        done
    done
    # The outputs are compared by analyze.py; they are large, so only their
    # digests are kept.
    for f in "$OUT"/showcase/*.out; do sha256sum "$f" | cut -c1-16 > "${f%.out}.sha"; rm -f "$f"; done
    find "$OUT/showcase" -name '*.err' -size 0 -delete
fi
echo "done $(date -u +%T)" >> "$OUT/gate.txt"
