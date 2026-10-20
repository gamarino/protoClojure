#!/bin/bash
# protoClojure benchmarks/bench.sh against Babashka 1.4.192 on protoCore
# 2.14.8: main before (354e728) and after (ef3b328) merging perf/call-path,
# alternating sides, three rounds, each run behind the quiet gate and on a
# fresh copy of the library.
set -u
WS=/home/gamarino/Documentos/proyectos
OUT=$WS/.agent_scratch/copy/clj_suite; mkdir -p $OUT/libs
Q="python3 $WS/protoCore/docs/reports/data/2026-10-12-mark-stack-int-compare/quiet.py"
LIB=$WS/.agent_scratch/installers-oct2026/stage2148/usr/lib/libprotoCore.so.3
for r in 1 2 3; do
  for side in before after; do
    [ $side = before ] && B=$WS/.agent_scratch/copy/clj_before/build/protoclj || B=$WS/protoClojure/build_2148/protoclj
    $Q > $OUT/quiet_${side}_r$r.txt 2>&1
    d=$OUT/libs/${side}_r$r; mkdir -p $d; cp -L $LIB $d/; export LD_LIBRARY_PATH=$d
    echo "$(date -u +%FT%TZ) clj $side r$r"
    ( cd $WS/protoClojure && bash benchmarks/bench.sh $B $WS/tools/bb-1.4.192/bb > $OUT/clj_${side}_r$r.txt 2>&1 )
  done
done
echo DONE
