#!/usr/bin/env bash
# Times every stage script under protoclj and bb, verifies each output against
# the expected work, and prints one line per run: runtime stage ms status.
#
#   run-stages.sh PROTOCLJ BB INPUT REPEATS [STAGE...]
#
# RUNTIMES (default "clj bb") picks which of the two run; LABEL (default
# "clj") names the protoclj binary in the output, for comparing builds.
#
# Each run takes the shared build lock ($LOCK) for its own duration; the
# caller runs the idle gate first.
set -u
here=$(cd "$(dirname "$0")" && pwd)
clj=$1 bb=$2 input=$3 reps=$4; shift 4
stages=("$@")
lock=${LOCK:-/home/gamarino/Documentos/proyectos/.agent_scratch/build.lock}
[[ ${#stages[@]} -eq 0 ]] && stages=(a_lines a2_reduce b_refind b2_literal b3_includes c_vector d_parselong e_loop f_full)
declare -A expect=(
  [a_lines]="lines 1446319"
  [a2_reduce]="lines 1446319"
  [b_refind]="matches 1588"
  [b2_literal]="matches 43593"
  [b3_includes]="matches 1588"
  [c_vector]="lines 1446319 chars 113897117"
  [c2_vector_noinc]="lines 1446319 chars 113897117"
  [c3_scalar_count]="chars 113897117"
  [d_parselong]="sum 1784757646"
  [e_loop]="matches 1588 latency-ms-total 2053520"
  [f_full]="matches 1588 latency-ms-total 2053520"
)
for s in "${stages[@]}"; do
  for ((r = 0; r < reps; r++)); do
    for rt in ${RUNTIMES:-clj bb}; do
      bin=$clj; [[ $rt == bb ]] && bin=$bb
      # Timed inside the lock, so waiting for it is not measured.
      res=$(flock "$lock" bash -c 't0=$(date +%s%N)
        o=$(timeout 600 "$0" "$1" < "$2" 2>&1 | tail -1)
        echo "$(( ($(date +%s%N) - t0) / 1000000 ))|$o"' "$bin" "$here/stages/$s.clj" "$input")
      ms=${res%%|*} out=${res#*|}
      status=ok; [[ "$out" == "${expect[$s]}" ]] || status="MISMATCH($out)"
      name=$rt; [[ $rt == clj ]] && name=${LABEL:-clj}
      echo "$name $s $ms $status"
    done
  done
done
