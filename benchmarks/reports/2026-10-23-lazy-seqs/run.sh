#!/bin/bash
# Part B stage 11 (lazy sequences) against Part A (ed3cf5a, eager map /
# filter / for / range): benchmarks/bench.sh, 3 rounds, order alternated,
# each behind the quiet gate; the macro loop, the keyword-map loop and five
# sequence idioms, 5 interleaved rounds of perf stat cycles; start-up (an
# empty file), perf stat -r 20, two interleaved passes. Both binaries run on
# their own fresh copy of the same libprotoCore.so.4 (protoCore 3.1.0, branch
# feature/lazy-object; Part A's binary was built against 3.0.0, which 3.1.0
# keeps ABI-compatible). Every program prints its result; the script keeps
# them.
set -u
export QUIET_CORES=${QUIET_CORES:-4.0}
WS=/home/gamarino/Documentos/proyectos
S=$WS/.agent_scratch/clj-macros
R=$WS/protoClojure/benchmarks/reports/2026-10-23-lazy-seqs
O=$R/out; rm -rf $O; mkdir -p $O
Q="python3 $WS/protoCore/docs/reports/data/2026-10-12-mark-stack-int-compare/quiet.py"
BB=$WS/tools/bb-1.4.192/bb
declare -A BIN=( [A]=$S/partA/protoclj-new [B]=$S/partB/protoclj-final )
for v in A B; do mkdir -p $O/libs/$v; cp -L $S/protoCore-lazy-prefix/lib/libprotoCore.so.4 $O/libs/$v/;
  echo "$v:" >> $O/ldd.txt; LD_LIBRARY_PATH=$O/libs/$v ldd ${BIN[$v]} | grep proto >> $O/ldd.txt; done
for r in 1 2 3; do
  order="A B"; [ $r = 2 ] && order="B A"
  for v in $order; do
    $Q > $O/quiet_${v}_r$r.txt 2>&1
    ( cd $WS/protoClojure && LD_LIBRARY_PATH=$O/libs/$v bash benchmarks/bench.sh ${BIN[$v]} $BB > $O/bench_${v}_r$r.txt 2>&1 )
  done
done
$Q > $O/quiet_loops.txt 2>&1
for r in 1 2 3 4 5; do
  for v in A B; do
    for f in $R/macro-loop.clj $R/map-loop.clj $R/idioms/*.clj; do
      n=$(basename $f .clj)
      res=$(LD_LIBRARY_PATH=$O/libs/$v perf stat -x, -e cycles -o $O/tmp.$v ${BIN[$v]} $f | tr '\n' ' ')
      echo "$v $res" >> $O/results_$n.txt
      echo "$v $(grep cycles $O/tmp.$v | cut -d, -f1)" >> $O/cycles_$n.txt
    done
  done
done
echo "" > $O/empty.clj
$Q > $O/quiet_startup.txt 2>&1
for pass in 1 2; do
  for v in A B; do
    LD_LIBRARY_PATH=$O/libs/$v perf stat -r 20 -x, -e cycles,task-clock ${BIN[$v]} $O/empty.clj 2>> $O/startup_${v}.txt >/dev/null
  done
done
{
  for f in $O/cycles_*.txt; do
    n=$(basename $f .txt); n=${n#cycles_}
    for v in A B; do
      grep "^$v " $f | awk '{print $2}' | sort -n | awk -v v=$v -v n=$n '{a[NR]=$1} END{printf "%-16s %s median %6.0f Mcycles (min %6.0f max %6.0f)\n", n, v, a[int((NR+1)/2)]/1e6, a[1]/1e6, a[NR]/1e6}'
    done
  done
  for v in A B; do echo "bench $v:"; for r in 1 2 3; do sed -n '3,7p' $O/bench_${v}_r$r.txt | awk '{print "  "$1, $2}' | tr '\n' ' '; echo; done; done
  for v in A B; do echo "startup $v: $(grep -E 'cycles|task-clock' $O/startup_$v.txt | cut -d, -f1 | tr '\n' ' ')"; done
} | tee $O/summary.txt
