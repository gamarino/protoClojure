# The macros branch, baseline against final (2026-10-24)

`run.sh` in this directory, behind protoCore's quiet gate (`QUIET_CORES=4.0`,
1.7-2.0 busy CPUs at the gates, load average 3-6.5 on 12 CPUs while it ran;
`out/log.txt`). Release builds from clean worktrees:

- **baseline**: `main` 8b7938a, before macros stage 1;
- **final**: `feature/macros` 3b75e41 (stage 13 code; later commits change
  documentation only).

Variants, each on its own fresh copy of the library (`out/ldd.txt`):
A = baseline on protoCore 3.0.0, B = baseline on protoCore 3.1.0
(`feature/lazy-object` 34eca79f, ABI-compatible), C = final on 3.1.0.
A against B is the kernel, B against C protoClojure.

## Results

`benchmarks/bench.sh` (best of 3 per run, median of 5 rounds, order rotated;
ms, ranges in brackets) and `map-loop.clj` (perf stat cycles, 5 rounds):

| workload | A base / 3.0.0 | B base / 3.1.0 | C final / 3.1.0 | C vs B |
|---|---:|---:|---:|---:|
| fib | 377 [369-378] | 370 [365-382] | 398 [384-399] | **+7.6 %** |
| tak | 34 [34-35] | 35 [34-37] | 40 [40-41] | **+14.3 %** |
| sum-loop | 70 [67-75] | 69 [68-72] | 76 [74-79] | **+10.1 %** |
| reduce-list | 27 [27-28] | 28 [27-29] | 34 [33-35] | **+21.4 %** |
| sum-squares | 24 [23-25] | 24 [22-24] | 30 [29-32] | **+25.0 %** |
| map-loop (Mcycles) | 3627 | 3609 | 3998 | **+10.8 %** |
| start-up, empty file (Mcycles) | 48.2 | 48.0 | 69.5 | **+45 %** |
| start-up (ms task-clock) | 13.4 | 13.1 | 18.9 | +5.8 ms |

Every program printed the same result in all variants and bench.sh's
results matched Babashka's (`out/summary.txt`).

## Reading

- **The kernel is neutral**: A and B agree within their ranges everywhere.
- **Every workload regressed, and none of the C ranges overlaps B's.**
- **Start-up costs +21.5 M cycles (about 6 ms of CPU).** Each bench.sh run
  is one process including start-up, so this alone accounts for most of the
  +6 ms of `reduce-list` and `sum-squares`, which run about 30 ms in all, and
  about 5 of map-loop's +11 %. The start-up growth is the bootstrap
  `core.clj` (macros, the Part C vocabulary, lazy-seq support) and the larger
  set of built-ins; the earlier per-stage reports put `core.clj` at about
  5 ms. The split between `core.clj` and the rest is likely but not
  isolated here.
- **Beyond start-up, `fib` (+28 ms, of which about 6 ms is start-up) and
  `tak` (+5-6 ms) are slower in the run itself**, by roughly 5-15 %. Likely
  causes are the lazy-seq / locals-clearing changes of stage 11 and the
  macros-era compiler and call-path changes; none is isolated. The per-stage
  reports in this directory's siblings measured smaller steps one at a time;
  their sum is not this number, and this one is the end-to-end truth.
- Babashka 1.4.192 in the same runs: fib 458 ms, tak 29, sum-loop 172,
  reduce-list 19, sum-squares 18 (medians). protoclj final: fib 0.87x bb,
  sum-loop 0.44x, tak 1.38x, reduce-list 1.79x, sum-squares 1.67x.

Reproduce: `ROUNDS=5 QUIET_GATE=... ./run.sh BASELINE_BIN BASELINE_LIB FINAL_BIN FINAL_LIB BB`.
