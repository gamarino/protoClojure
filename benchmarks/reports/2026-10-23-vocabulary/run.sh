#!/bin/bash
# Part C (the scripting vocabulary) against Part B stage 11 (8d72602):
# start-up (an empty file, perf stat -r 20, three interleaved passes) and
# benchmarks/bench.sh (two rounds, order alternated), behind the quiet gate,
# each binary on its own fresh copy of protoCore 3.1.0's libprotoCore.so.4.
set -u
export QUIET_CORES=${QUIET_CORES:-4.0}
WS=/home/gamarino/Documentos/proyectos
S=$WS/.agent_scratch/clj-macros
R=$WS/protoClojure/benchmarks/reports/2026-10-23-vocabulary
O=$R/out; rm -rf $O; mkdir -p $O
Q="python3 $WS/protoCore/docs/reports/data/2026-10-12-mark-stack-int-compare/quiet.py"
BB=$WS/tools/bb-1.4.192/bb
declare -A BIN=( [B]=$S/partB/protoclj-final [C]=$S/partC/protoclj-C )
L=$S/partC/libs; rm -rf $L
for v in B C; do mkdir -p $L/$v; cp -L $S/protoCore-lazy-prefix/lib/libprotoCore.so.4 $L/$v/;
  echo "$v:" >> $O/ldd.txt; LD_LIBRARY_PATH=$L/$v ldd ${BIN[$v]} | grep proto >> $O/ldd.txt; done
echo "" > $O/empty.clj
$Q > $O/quiet_startup.txt 2>&1
for pass in 1 2 3; do
  for v in B C; do
    LD_LIBRARY_PATH=$L/$v perf stat -r 20 -x, -e cycles,task-clock ${BIN[$v]} $O/empty.clj 2>> $O/startup_${v}.txt >/dev/null
  done
done
for r in 1 2; do
  order="B C"; [ $r = 2 ] && order="C B"
  for v in $order; do
    $Q > $O/quiet_${v}_r$r.txt 2>&1
    ( cd $WS/protoClojure && LD_LIBRARY_PATH=$L/$v bash benchmarks/bench.sh ${BIN[$v]} $BB > $O/bench_${v}_r$r.txt 2>&1 )
  done
done
{
  for v in B C; do echo "startup $v (Mcycles, ms): $(grep -E 'cycles|task-clock' $O/startup_$v.txt | cut -d, -f1 | awk 'NR%2==1{printf "%.1f/", $1/1e6} NR%2==0{printf "%s ", $1}')"; done
  for v in B C; do echo "bench $v:"; for r in 1 2; do sed -n '3,7p' $O/bench_${v}_r$r.txt | awk '{print "  "$1, $2, $6}' | tr '\n' ' '; echo; done; done
} | tee $O/summary.txt
