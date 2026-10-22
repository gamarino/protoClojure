#!/bin/bash
# Second, independent pass over the cells that moved more than 5 % in the first
# pass: protoClojure's throw_catch and zerodiv probes (order reversed: AFTER
# first), and protoST's list_append and str_concat (perf stat -r 5 cycles of
# the whole process, BEFORE/AFTER interleaved, two passes in opposite order).
set -u
export QUIET_CORES=${QUIET_CORES:-4.0}
WS=/home/gamarino/Documentos/proyectos
M=$WS/.agent_scratch/meas_1022
Q="python3 $WS/protoCore/docs/reports/data/2026-10-12-mark-stack-int-compare/quiet.py"
O=$M/clj_out/recheck; mkdir -p $O/libs
for v in before after; do mkdir -p $O/libs/$v; cp -L $M/$v/protoCore/build_release/libprotoCore.so.4 $O/libs/$v/; done
$Q > $O/quiet.txt 2>&1
for f in startup throw_catch zerodiv throw_across5; do
  for v in after before; do
    c="env LD_LIBRARY_PATH=$O/libs/$v $M/$v/protoClojure/build_release/protoclj $M/clj_probes/$f.clj"
    chk=$($c 2>&1 | grep CHECK | tail -n 1)
    o=$(perf stat -r 3 -x, -e cycles,task-clock $c 2>&1 >/dev/null | grep -E 'cycles|task-clock' | cut -d, -f1 | paste -sd' ')
    echo "$f $v $o | $chk" >> $O/probes.txt
  done
done
O=$M/st_out/recheck; mkdir -p $O/libs
for v in before after; do mkdir -p $O/libs/$v; cp -L $M/$v/protoCore/build_release/libprotoCore.so.4 $O/libs/$v/; done
$Q > $O/quiet.txt 2>&1
for pass in 1 2; do
  order="before after"; [ $pass = 2 ] && order="after before"
  for f in list_append str_concat int_sum_loop range_iterate; do
    for v in $order; do
      c="env LD_LIBRARY_PATH=$O/libs/$v $M/$v/protoST/build_release/protost $M/after/protoST/benchmarks/comparable/$f.st"
      chk=$($c 2>&1 | tail -n 1)
      o=$(perf stat -r 5 -x, -e cycles,task-clock $c 2>&1 >/dev/null | grep -E 'cycles|task-clock' | cut -d, -f1 | paste -sd' ')
      echo "pass$pass $f $v $o | $chk" >> $O/comparables.txt
    done
  done
done
echo RECHECK_DONE
# protoScala compile time, 5 more reps, interleaved, into a new directory
O=$M/sc_out_2/compile_recheck; mkdir -p $O/libs
for v in before after; do mkdir -p $O/libs/$v; cp -L $M/$v/protoCore/build_release/libprotoCore.so.4 $O/libs/$v/; done
$Q > $O/quiet.txt 2>&1
C=$M/after/protoScala/benchmarks/comparable
for rep in 1 2 3 4 5; do
  for s in $C/fib.scala $C/object_tree.scala $M/sc_probes/throw_catch.scala $M/sc_probes/throw_across5.scala $M/sc_probes/zerodiv.scala; do
    for v in before after; do
      wd=$O/rep$rep/$v/$(basename $s .scala); mkdir -p $wd
      t0=$(date +%s.%N); env LD_LIBRARY_PATH=$O/libs/$v $M/$v/protoScala/build_release/protoscalac $s -o $wd --build-so > $wd.build_log.txt 2>&1; rc=$?; t1=$(date +%s.%N)
      echo "$(basename $s .scala) $v rep$rep rc=$rc ms=$(echo "($t1-$t0)*1000" | bc) so_bytes=$(stat -c %s $wd/module.so 2>/dev/null || echo 0) cpp_lines=$(cat $wd/*.cpp 2>/dev/null | wc -l)" >> $O/compile.txt
    done
  done
done
echo RECHECK2_DONE
# protoScala probes that moved > 5 %: second pass, order reversed (AFTER first)
O=$M/sc_out_2/probe_recheck; mkdir -p $O/libs
for v in before after; do mkdir -p $O/libs/$v; cp -L $M/$v/protoCore/build_release/libprotoCore.so.4 $O/libs/$v/; done
$Q > $O/quiet.txt 2>&1
for n in startup throw_catch throw_across5 zerodiv; do
  for k in trans_after trans_before interp_after interp_before; do
    v=${k#*_}; T=$M/$v/protoScala/build_release
    case $k in
      interp_*) c="env LD_LIBRARY_PATH=$O/libs/$v $T/protoscala $M/sc_probes/$n.scala";;
      trans_*) c="env LD_LIBRARY_PATH=$O/libs/$v $T/protoscala --run-module $M/sc_out_2/probe_so/$v/$n/module.so";;
    esac
    chk=$($c 2>&1 | grep CHECK | tail -n 1)
    o=$(perf stat -r 3 -x, -e cycles,task-clock $c 2>&1 >/dev/null | grep -E 'cycles|task-clock' | cut -d, -f1 | paste -sd' ')
    echo "$n $k $o | $chk" >> $O/probes.txt
  done
done
echo RECHECK3_DONE
