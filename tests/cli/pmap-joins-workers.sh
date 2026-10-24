#!/usr/bin/env bash
#
# CLI check: pmap joins every worker thread it starts, including the ones
# that have already finished when pmap gets to them.
#
# A native thread that has finished keeps its stack mapped (and protoCore's
# std::thread object allocated) until it is joined. pmap used to join only
# the workers it found still running, so a quick (pmap f coll) left most of
# its threads unjoined until the process exited: 21 calls over 64 elements
# grew the virtual size by about 40 GB (one stack per element), and
# ThreadSanitizer reported the threads as leaked. Every worker is joined now,
# finished or not, and the virtual size stays flat.
#
# The program reads its own VmSize from /proc (Linux only; the check is
# skipped elsewhere).
#
# Usage: pmap-joins-workers.sh <path-to-protoclj>
set -u
PROTOCLJ="${1:?usage: pmap-joins-workers.sh <protoclj>}"
# Under MSYS2 / Git Bash the shell has a /proc of its own, which the native
# Windows protoclj cannot read.
if [[ ! -r /proc/self/status ]] || command -v cygpath >/dev/null 2>&1; then
    echo "SKIP: protoclj cannot read /proc/self/status here"
    exit 0
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/pmap.clj" <<'CLJ'
(defn vm-kb []
  (let [line (first (filter (fn [l] (clojure.string/starts-with? l "VmSize:"))
                            (clojure.string/split-lines (slurp "/proc/self/status"))))]
    (parse-long (re-find #"\d+" line))))
(defn spin [] (reduce + (pmap inc (range 64))))
(spin)
(def before (vm-kb))
(def sums (vec (for [_ (range 20)] (spin))))
(def after (vm-kb))
(println "sums" (count (filter #(= 2080 %) sums)))
(println "growth-mb" (quot (- after before) 1024))
CLJ
out=$("$PROTOCLJ" "$TMP/pmap.clj" 2>&1); rc=$?
if [[ $rc -ne 0 ]]; then
    echo "FAIL: exit $rc"; printf '%s\n' "$out" | tail -5; exit 1
fi
if ! grep -qx "sums 20" <<<"$out"; then
    echo "FAIL: expected 'sums 20' (every pmap computed its elements)"; printf '%s\n' "$out"; exit 1
fi
growth=$(sed -n 's/^growth-mb \(-\{0,1\}[0-9]*\)$/\1/p' <<<"$out")
if [[ -z "$growth" ]]; then
    echo "FAIL: no growth line"; printf '%s\n' "$out"; exit 1
fi
# 1280 unjoined workers cost tens of GB of address space; joined ones leave
# at most the C library's cached stacks and arenas behind.
if (( growth > 2048 )); then
    echo "FAIL: virtual size grew by ${growth} MB over 20 pmap calls (workers not joined)"
    exit 1
fi
echo "PASS: 20 pmap calls of 64 elements, virtual size grew by ${growth} MB"
