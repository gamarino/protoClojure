#!/bin/bash
# protoClojure BEFORE (4c5d3aa on protoCore cf52f9f5) vs AFTER (bef23b0 on
# protoCore 65760e4a), Release, clean worktrees, against Babashka 1.4.192.
# Each run behind the quiet gate (QUIET_CORES=4.0) and on a fresh copy of the
# variant's libprotoCore.
set -u
export QUIET_CORES=${QUIET_CORES:-4.0}
WS=/home/gamarino/Documentos/proyectos
M=$WS/.agent_scratch/meas_1022; OUT=$M/clj_out; mkdir -p $OUT/libs
Q="python3 $WS/protoCore/docs/reports/data/2026-10-12-mark-stack-int-compare/quiet.py"
BB=$WS/tools/bb-1.4.192/bb
bin() { echo $M/$1/protoClojure/build_release/protoclj; }
lib() { echo $M/$1/protoCore/build_release/libprotoCore.so.4; }
fresh() { local d=$OUT/libs/$2; mkdir -p $d; cp -L $(lib $1) $d/libprotoCore.so.4; export LD_LIBRARY_PATH=$d
          echo "== $2" >> $OUT/ldd.txt; ldd $(bin $1) | grep -iE "proto|ssl" >> $OUT/ldd.txt; }
# 1. self-report verification: every bench.sh workload and probe, once per variant and with bb
for f in $M/after/protoClojure/benchmarks/{fib,tak,sum-loop,reduce-list,sum-squares}.clj $M/clj_probes/*.clj; do
  echo "bb $(basename $f) last=$($BB $f 2>&1 | tail -n 1)" >> $OUT/verify.txt
  for v in before after; do
    fresh $v verify_$v
    o=$(timeout 600 $(bin $v) $f 2>&1); rc=$?
    echo "$v $(basename $f) rc=$rc last=$(echo "$o" | tail -n 1)" >> $OUT/verify.txt
  done
done
unset LD_LIBRARY_PATH
# 2. benchmarks/bench.sh (AFTER's copy: benchmarks/ is identical in both commits),
#    three rounds, order alternated per round
for r in 1 2 3; do
  order="after before"; [ $r = 2 ] && order="before after"
  for v in $order; do
    $Q > $OUT/quiet_${v}_r$r.txt 2>&1; fresh $v ${v}_r$r; echo "$(date -u +%FT%TZ) clj $v r$r"
    ( cd $M/after/protoClojure && bash benchmarks/bench.sh $(bin $v) $BB > $OUT/clj_${v}_r$r.txt 2>&1 )
  done
done
# 3. probes, perf stat -r 3 cycles, BEFORE/AFTER/bb interleaved per probe
$Q > $OUT/quiet_probes.txt 2>&1; fresh before probes_before; LB=$LD_LIBRARY_PATH; fresh after probes_after; LA=$LD_LIBRARY_PATH; unset LD_LIBRARY_PATH
echo "$(date -u +%FT%TZ) probes"
for f in $M/clj_probes/*.clj; do
  for v in before after bb; do
    case $v in before) c="env LD_LIBRARY_PATH=$LB $(bin before) $f";; after) c="env LD_LIBRARY_PATH=$LA $(bin after) $f";; bb) c="$BB $f";; esac
    chk=$($c 2>&1 | grep CHECK | tail -n 1)
    o=$(perf stat -r 3 -x, -e cycles,task-clock $c 2>&1 >/dev/null | grep -E 'cycles|task-clock' | cut -d, -f1 | paste -sd' ')
    echo "$(basename $f .clj) $v $o | $chk" >> $OUT/probes.txt
  done
done
echo DONE
