# protoClojure on protoCore's unified exception model: final measurement (2026-10-22)

## Builds

| | protoClojure | protoCore | Notes |
|---|---|---|---|
| **BEFORE** | `4c5d3aa` | `cf52f9f5` | 3.0.0 calling convention, no `ProtoException` |
| **AFTER** | `bef23b0` (`feature/protocore-3-calling-convention` tip) | `65760e4a` (`feature/args-calling-convention` tip) | `427b6c3` port, `bef23b0` rethrow fix |

- **Worktrees**: clean, detached, under
  `.agent_scratch/meas_1022/{before,after}/{protoCore,protoClojure,protoIO}`.
  - protoIO `1611ff9` is used on both sides. It does not link protoCore.
- **Build**: Release, `-DCMAKE_DISABLE_FIND_PACKAGE_protoCore=ON`, so the
  developer fallback uses the sibling `../protoCore/build_release`.
  - `build3.sh` built everything one build at a time, at `-j4`, before any
    measurement.
- **Library loading**: every run loaded a fresh copy of its variant's
  `libprotoCore.so.4` through `LD_LIBRARY_PATH`. See `ldd.txt`; no run resolved
  `/usr/lib`.
- **Quiet gate**: **`QUIET_CORES=4.0`** (`quiet_*.txt`).
  - The VS Code GPU process, Chrome and PyCharm keep 2-4 CPUs busy.
  - At the gates, 3.4-3.8 CPUs were busy and the load average was 4.9-6.2.
  - **Only the interleaved BEFORE/AFTER ratios are meaningful.**
- **Self-report check** (`verify.txt`)
  - Coverage: the 5 `bench.sh` workloads and 6 probes, each run once with
    BEFORE, AFTER and Babashka.
  - Result: every result is identical to Babashka's, and rc=0.
  - In all 6 harness runs every row carries `✓`: the result matched bb.

## `benchmarks/bench.sh` against Babashka 1.4.192

- **Method**
  - `bench.sh` reports the best of 3 cold runs per workload.
  - It ran three rounds per variant, with the order alternated (A,B / B,A /
    A,B).
  - The figures are the medians of the three rounds, in ms.
  - Both sides used AFTER's copy of `bench.sh`. `benchmarks/` is identical in
    the two commits.
- **Comparison runtime**: Babashka (`tools/bb-1.4.192/bb`). JVM Clojure is not
  installed and was not measured.

| Workload | result | bb | BEFORE | AFTER | A/B | BEFORE rounds | AFTER rounds | AFTER/bb |
|---|---|---:|---:|---:|---:|---|---|---:|
| `fib` (30) | 832040 | 592 | 546 | 490 | 0.897 | 567/546/503 | 490/485/507 | 0.83x |
| `tak` | 7 | 38 | 53 | 50 | 0.943 | 53/53/47 | 50/52/50 | 1.32x |
| `sum-loop` | 500000500000 | 220 | 97 | 91 | 0.938 | 97/107/85 | 91/113/91 | 0.41x |
| `reduce-list` | 49995000 | 24 | 39 | 38 | 0.974 | 38/41/39 | 38/38/35 | 1.55x |
| `sum-squares` | 332833500 | 22 | 30 | 32 | 1.067 | 38/30/30 | 33/32/32 | 1.42x |

| | Geomean |
|---|---:|
| AFTER / BEFORE (5) | **0.962** |
| BEFORE / bb | 1.04x |
| AFTER / bb | 1.00x |

- **No regression on the normal path.**
- The 3.8 % gain is mostly `fib`: −10 %, with the rounds disjoint, 485-507 ms
  against 503-567 ms.
  - The port touched the call path; protoCore's `65760e4a` is also on the
    AFTER side.
  - This was not isolated to one cause. With best-of-3 wall time on a busy
    host, treat it as "probably a small gain".
- `sum-squares` +7 % is 2 ms on a 30 ms run. BEFORE's rounds were 38/30/30.
  **Noise at this scale; not re-measured.**

## Exception probes ([`probes/`](probes/), `probes.txt`, `recheck/probes.txt`)

- **Method**
  - `perf stat -r 3` cycles, with BEFORE, AFTER and bb interleaved per probe.
  - Each figure is cycles per iteration net of start-up: the cycles of
    `startup.clj` are subtracted, and the rest is divided by the iteration
    count. Each probe's `CHECK` line is its count, and it was verified.
- **The probes**
  - Each one is a `loop`/`recur` whose step adds `(try ... (catch ...))`.
  - `throw_across5` throws from `(deep 0)`, under 5 non-tail frames of
    `(+ 1 (deep (dec d)))`.
  - `zerodiv` is `(/ 1 z)` caught as `ArithmeticException`.
- **The BEFORE commit ran every probe.**
- The probes that moved by more than 5 % were run a second time, AFTER first:
  `recheck/probes.txt`.

| Probe | iters | BEFORE | AFTER | A/B | 2nd pass A/B | bb |
|---|---:|---:|---:|---:|---:|---:|
| `empty_loop` | 2,000,000 | 379 | 383 | 1.01x | — | 477 |
| `try_no_throw` | 2,000,000 | 404 | 401 | 0.99x | — | 1,894 |
| `throw_catch` (`ex-info` thrown and caught in the same fn) | 200,000 | 26,085 | 22,759 | **0.87x** | **0.86x** | 42,910 |
| `throw_across5` | 100,000 | 94,812 | 96,867 | 1.02x | 1.01x | 74,670 |
| `zerodiv` (`(/ 1 z)` caught) | 200,000 | 23,135 | 25,407 | **1.10x** | **1.09x** | 10,736 |

- **Both moves reproduced in a second pass run in the reverse order.**
  - **`throw_catch` is 13-14 % cheaper.**
  - **`zerodiv`, the native division by zero, is 9-10 % more expensive**:
    about +2,200 cycles per caught exception. **A regression.** A plausible
    cause, not verified: the native error now goes through `ProtoException`
    construction and selection by prototype.
- `try_no_throw` costs nothing over the loop, before and after.
- Against bb, protoClojure throws and catches in one function in half bb's
  cycles. Across 5 frames it is 1.3x bb, and on `zerodiv` 2.4x bb.

## Not measured / deviations

- No JVM Clojure (not installed). Babashka is the reference, as in earlier
  reports.
- `bench.sh` is best-of-3 wall time per round, not a median. That is the
  harness's own method, kept unchanged.
- `QUIET_CORES=4.0` instead of 2.5.

## Files

- **Scripts**
  - `build3.sh`, `clj_run.sh`
  - `recheck.sh`: shared with protoST and protoScala.
  - `clj_analyze.py` + `uex_common.py`
- **Raw data**
  - `clj_{before,after}_r{1,2,3}.txt`
  - `probes.txt`, `recheck/probes.txt`
  - `verify.txt`, `ldd.txt`, `quiet_*.txt`
  - `analysis.txt`
- **Worktrees**, kept:
  `/home/gamarino/Documentos/proyectos/.agent_scratch/meas_1022/{before,after}/protoClojure`,
  with builds in `build_release/`.
