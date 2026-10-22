#!/bin/bash
# Sequential Release builds of protoST, protoScala and protoClojure, BEFORE and
# AFTER, each against its sibling protoCore worktree (developer fallback,
# -DCMAKE_DISABLE_FIND_PACKAGE_protoCore=ON). One build at a time, -j4.
set -u
M=/home/gamarino/Documentos/proyectos/.agent_scratch/meas_1022
F="-DCMAKE_BUILD_TYPE=Release -DCMAKE_DISABLE_FIND_PACKAGE_protoCore=ON"
for v in before after; do
  cd $M/$v/protoST && cmake -S . -B build_release $F > $M/build_${v}_st_cfg.log 2>&1 && \
    cmake --build build_release --target protost -j4 > $M/build_${v}_st.log 2>&1; echo "st $v rc=$?"
done
for v in before after; do
  cd $M/$v/protoClojure && cmake -S . -B build_release $F > $M/build_${v}_clj_cfg.log 2>&1 && \
    cmake --build build_release --target protoclj -j4 > $M/build_${v}_clj.log 2>&1; echo "clj $v rc=$?"
done
for v in before after; do
  cd $M/$v/protoScala && cmake -S . -B build_release $F -DPROTOSCALA_PROTOST_INTEROP=OFF > $M/build_${v}_sc_cfg.log 2>&1 && \
    cmake --build build_release --target protoscala protoscalac protoScala -j4 > $M/build_${v}_sc.log 2>&1; echo "sc $v rc=$?"
done
echo BUILD_DONE
