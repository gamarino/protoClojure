#!/bin/bash
# Usage: measure.sh OUTDIR ROUNDS bin1 bin2 ...   (bins are paths)
# Each round, rotated order; each (round, bin) runs from a FRESH copy of the
# binary and of libprotoCore (LD_LIBRARY_PATH), behind the quiet gate.
# perf stat cycles+instructions per workload; output verified per run.
set -u
S=/home/gamarino/Documentos/proyectos/.agent_scratch/clj-regression
LIB=${LIB:-/home/gamarino/Documentos/proyectos/.agent_scratch/clj-macros/protoCore-lazy-prefix/lib/libprotoCore.so.4}
OUT=$(realpath -m $1); ROUNDS=$2; shift 2; BINS=(); for b in "$@"; do BINS+=("$(realpath $b)"); done; N=${#BINS[@]}
WLS=${WLS:-"empty fib tak sum-loop reduce-list sum-squares map-loop"}
mkdir -p $OUT/fresh; : > $OUT/raw.csv
declare -A EXP=([empty]="" [fib]="2178309" [tak]="9" [sum-loop]="50000005000000" [reduce-list]="44999850000" [sum-squares]="333328333350000" [map-loop]="map-loop 2000007000002")
for ((r=1; r<=ROUNDS; ++r)); do
  for ((k=0; k<N; ++k)); do
    b=${BINS[$(( (k + r) % N ))]}; name=$(basename $b)
    d=$OUT/fresh/r${r}_$name; mkdir -p $d; cp -L $b $d/protoclj; cp -L $LIB $d/libprotoCore.so.4
    [[ $r == 1 ]] && LD_LIBRARY_PATH=$d ldd $d/protoclj | grep -i protocore | sed "s|^|$name |" >> $OUT/ldd.txt
    $S/gate.py >> $OUT/gate.txt 2>&1
    for w in $WLS; do
      rep=1; [[ $w == empty ]] && rep=10
      res=$(cd $S/wl && LC_ALL=C LD_LIBRARY_PATH=$d perf stat -r $rep -x, -e cycles,instructions -o $d/perf_$w.txt $d/protoclj $w.clj 2>&1 | tail -1)
      ok=1; [[ $w != empty && "$res" != "${EXP[$w]}" ]] && ok=0
      [[ $w == tak && $ok == 0 && $r == 1 ]] && echo "tak output: $res" >> $OUT/notes.txt
      cyc=$(grep ',cycles' $d/perf_$w.txt | cut -d, -f1); ins=$(grep ',instructions' $d/perf_$w.txt | cut -d, -f1)
      echo "$name,$r,$w,$cyc,$ins,$ok" >> $OUT/raw.csv
    done
  done
done
python3 -I $S/summ.py $OUT/raw.csv "${BINS[@]}" | tee $OUT/summary.txt
