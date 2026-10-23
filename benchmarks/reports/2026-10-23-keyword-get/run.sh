#!/bin/bash
# Part A: stage 6 (e5a48c5) vs the fix vs stage 10 (1f14ba4, no fix) on the
# keyword-map loop and the macro loop, 9 interleaved rounds of perf stat
# cycles, each binary on its own fresh copy of libprotoCore.so.4. The loop
# prints its result; the script checks it.
set -u
export QUIET_CORES=${QUIET_CORES:-4.0}
WS=/home/gamarino/Documentos/proyectos
S=$WS/.agent_scratch/clj-macros
O=$S/partA/out; rm -rf $O; mkdir -p $O
Q="python3 $WS/protoCore/docs/reports/data/2026-10-12-mark-stack-int-compare/quiet.py"
declare -A BIN=( [s6]=$S/s6/wt/build_release/protoclj [s10]=$S/partA/protoclj-s10 [fix]=$S/partA/protoclj-new )
for v in s6 s10 fix; do mkdir -p $O/libs/$v; cp -L $WS/protoCore/build_release/libprotoCore.so.4 $O/libs/$v/; LD_LIBRARY_PATH=$O/libs/$v ldd ${BIN[$v]} | grep proto >> $O/ldd.txt; done
$Q > $O/quiet.txt 2>&1
for r in 1 2 3 4 5 6 7 8 9; do
  for v in s6 s10 fix; do
    for w in map-loop:$S/s10/map-loop.clj macro-loop:$S/s6/macro-loop.clj; do
      n=${w%%:*}; f=${w#*:}
      res=$(LD_LIBRARY_PATH=$O/libs/$v perf stat -x, -e cycles -o $O/tmp.$v ${BIN[$v]} $f)
      echo "$res" >> $O/results_$n.txt
      echo "$v $(grep cycles $O/tmp.$v | cut -d, -f1)" >> $O/$n.txt
    done
  done
done
for n in map-loop macro-loop; do
  echo "== $n (results: $(sort -u $O/results_$n.txt | tr '\n' ' '))"
  for v in s6 s10 fix; do
    grep "^$v " $O/$n.txt | awk '{print $2}' | sort -n | awk -v v=$v '{a[NR]=$1} END{printf "%s median %.0f Mcycles (min %.0f max %.0f)\n", v, a[int((NR+1)/2)]/1e6, a[1]/1e6, a[NR]/1e6}'
  done
done | tee $O/summary.txt
