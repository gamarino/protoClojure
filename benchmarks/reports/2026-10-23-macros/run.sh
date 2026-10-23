#!/bin/bash
# Baseline (main 8b7938a) vs stage 6 (e5a48c5): benchmarks/bench.sh, 3 rounds,
# order alternated per round, each behind the quiet gate, on fresh copies of
# libprotoCore.so.4; a macro-heavy loop (cond/when/when-not), 5 runs each,
# interleaved; start-up (empty file) perf stat -r 20, two interleaved passes,
# baseline / stage 3 / stage 6 (stage 6 - stage 3 = the core.clj load).
set -u
export QUIET_CORES=${QUIET_CORES:-4.0}
WS=/home/gamarino/Documentos/proyectos
S=$WS/.agent_scratch/clj-macros
O=$S/s6/out; mkdir -p $O/libs/base $O/libs/s3 $O/libs/s6
Q="python3 $WS/protoCore/docs/reports/data/2026-10-12-mark-stack-int-compare/quiet.py"
BB=$WS/tools/bb-1.4.192/bb
bin() { case $1 in base) echo $S/baseline/wt/build_release/protoclj;; s3) echo $S/stage3/wt/build_release/protoclj;; s6) echo $S/s6/wt/build_release/protoclj;; esac; }
for v in base s3 s6; do cp -L $WS/protoCore/build_release/libprotoCore.so.4 $O/libs/$v/; done
: > $O/ldd.txt
for v in base s3 s6; do echo "$v:" >> $O/ldd.txt; LD_LIBRARY_PATH=$O/libs/$v ldd $(bin $v) | grep proto >> $O/ldd.txt; done
for r in 1 2 3; do
  order="base s6"; [ $r = 2 ] && order="s6 base"
  for v in $order; do
    $Q > $O/quiet_${v}_r$r.txt 2>&1
    ( cd $S/baseline/wt && LD_LIBRARY_PATH=$O/libs/$v bash benchmarks/bench.sh $(bin $v) $BB > $O/bench_${v}_r$r.txt 2>&1 )
  done
done
$Q > $O/quiet_macro.txt 2>&1
for r in 1 2 3 4 5; do
  for v in base s6; do
    LD_LIBRARY_PATH=$O/libs/$v /usr/bin/time -f "$v %e %U" $(bin $v) $S/s6/macro-loop.clj >> $O/macro_${v}.txt 2>&1
  done
done
echo "" > $O/empty.clj
$Q > $O/quiet_startup.txt 2>&1
for pass in 1 2; do
  for v in base s3 s6; do
    LD_LIBRARY_PATH=$O/libs/$v perf stat -r 20 -x, -e cycles,task-clock $(bin $v) $O/empty.clj 2>> $O/startup_${v}.txt >/dev/null
  done
done
echo DONE > $O/done.txt
