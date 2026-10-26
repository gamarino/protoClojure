#!/usr/bin/env bash
# Interleaved before/after/bb rounds over the showcase scripts. Every run's
# stdout must equal bb's for the same script (sha256), else MISMATCH.
# Usage: showcase-ab.sh ROUNDS OUTDIR
set -u
R=${1:-5}; OUT=${2:?}; mkdir -p "$OUT"
H=/home/gamarino/Documentos/proyectos/.agent_scratch/grep-fixes
SHOW=$H/protoClojure/examples/showcase
DATA=/home/gamarino/Documentos/proyectos/.agent_scratch/clj-bench-1025/data
GATE=/home/gamarino/Documentos/proyectos/.agent_scratch/clj-bench-1025/clj/benchmarks/reports/2026-10-25-readme-refresh/gate.sh
LOCK=/home/gamarino/Documentos/proyectos/.agent_scratch/build.lock
declare -A BIN=([before]=$H/bench/before/protoclj [after]=$H/bench/after/protoclj [bb]=/home/gamarino/Documentos/proyectos/tools/bb-1.4.192/bb)
W=("grep|grep.clj|$DATA/access.log|" "json|json-transform.clj|-|$DATA/orders.json" "wordfreq|word-freq.clj|$DATA/corpus.txt|")
export GATE_LOG=$OUT/gate.log
for ((r=1;r<=R;r++)); do
  "$GATE" "round-$r"
  for w in "${W[@]}"; do
    IFS='|' read -r name script in arg <<<"$w"
    case $((r%3)) in 1) order="before after bb";; 2) order="after bb before";; 0) order="bb before after";; esac
    for v in $order; do
      cmd=("${BIN[$v]}" "$SHOW/$script"); [[ -n $arg ]] && cmd+=("$arg")
      t=$OUT/${name}_${v}_r$r
      if [[ $in == - ]]; then flock "$LOCK" /usr/bin/time -f "%e %M" -o $t.time "${cmd[@]}" < /dev/null > $t.out 2> $t.err
      else flock "$LOCK" /usr/bin/time -f "%e %M" -o $t.time "${cmd[@]}" < "$in" > $t.out 2> $t.err; fi
      echo "$? $(sha256sum < $t.out | cut -c1-16)" > $t.rc
    done
  done
done
python3 -I - "$OUT" "$R" <<'PY'
import sys,statistics as st
out,R=sys.argv[1],int(sys.argv[2])
for name in ["grep","json","wordfreq"]:
    row=[]
    ref=[open(f"{out}/{name}_bb_r{r}.rc").read().split()[1] for r in range(1,R+1)]
    for v in ["before","after","bb"]:
        ts=[];ms=[];ok=True
        for r in range(1,R+1):
            e,m=open(f"{out}/{name}_{v}_r{r}.time").read().split()[-2:]
            rc,h=open(f"{out}/{name}_{v}_r{r}.rc").read().split()
            ok &= rc=="0" and h==ref[0]
            ts.append(float(e)); ms.append(int(m)/1024)
        row.append(f"{v} {st.median(ts):.2f}s [{min(ts):.2f}-{max(ts):.2f}] {st.median(ms):.0f}MB {'ok' if ok else 'MISMATCH'}")
    print(name, " | ".join(row))
PY
