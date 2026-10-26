# README benchmark refresh (2026-10-25)

Every figure the README quotes, measured again on `main` 1e7b612 (lists as
protoCore `ProtoLinkedList`s, M47) against protoCore 3.3.0, and compared
with the figures the README carried before this refresh.

## Setup

- protoClojure `main` 1e7b612, Release, from a clean worktree; protoIO
  1611ff9 (0.2.2, the CI pin) built in as the sibling tree.
- protoCore tag v3.3.0 (`aedc3b63`), Release, installed into a scratch
  prefix. `ldd` of the measured binary:
  `libprotoCore.so.4 => .../.agent_scratch/clj-bench-1025/prefix/lib/libprotoCore.so.4`
  (the system protoCore in `/usr` is 3.0.0 and was not used). Full output
  and the library's digest: `out/environment.txt`.
- Babashka v1.4.192 (`tools/bb-1.4.192/bb`), the binary every earlier
  report used.
- AMD Ryzen 5 5500U, 6 cores / 12 threads, Linux 7.0.0, governor
  `powersave`, EPP `balance_power`. The ctest suite (873 cases) passed on
  this build before measuring.
- All of it ran in one session while holding the workspace build lock.
  Before every round of every part, `gate.sh` waited until no compiler,
  `make` or `ctest` ran (`waitquiet.sh`), no other protoCore runtime or `bb`
  ran, and no process outside the desktop session took more than half a
  CPU; then it logged the load average (`out/load.txt`, UTC times).

**Load.** The desktop session (browser, compositor) stayed up: the 1-minute
load average was 3.6-7.4 at the round starts (12 CPUs); the actor rounds
pushed it to 10.8 themselves. A first attempt at this session overlapped a
CPU-bound protojs process another agent had started outside the lock; it
was stopped while still in its first gate, before any measurement, and the
gate was extended to wait for stray runtimes. That process had exited by
the time the first round started (`bench-r1 ... waited 15s`). Nothing
measured here overlapped it. Because the desktop load stays, read the
ratios between interleaved runtimes rather than the absolute milliseconds,
and compare older dated figures with care: Babashka's own times moved by up
to 7 % between the earlier reports and this one.

## Results and comparison with the previous README

Peak RSS figures below are indicative, not conclusive, and possibly not
comparable across runtimes: protoCore keeps memory until it is needed and
adapts its heap ceiling, while a JVM collector returns memory and behaves
differently with each collector. Read them as what the process held, not
what it needed.

### bench.sh workloads (`bench-rounds.sh`)

Five rounds, the runtime order alternating per round; each round is
`bench.sh`'s best of three process runs (start-up included); median and
range of the five round values. 150 runs, each checked against its
expected output: 0 failed.

| workload | protoclj ms | bb ms | ratio | README table (June, `ce9819d`) | README 2026-10-20 |
|---|---:|---:|---:|---|---|
| `fib` | 421 [402-454] | 455 [423-469] | **0.93x** | 712 / 524, 1.36x | 424 / 423, 1.00x |
| `tak` | 41 [38-46] | 29 [28-45] | 1.41x | 52 / 43, 1.21x | 43 / 27, 1.59x |
| `sum-loop` | 77 [73-81] | 172 [165-198] | **0.45x** | 79 / 219, 0.36x | 65 / 162, 0.40x |
| `reduce-list` | 29 [28-33] | 19 [19-20] | 1.54x | 27 / 36, 0.75x | 30 / 18, 1.67x |
| `sum-squares` | 28 [27-34] | 17 [17-18] | 1.61x | 19 / 31, 0.61x | 23 / 17, 1.39x |

`factorial-100` with plain `*`: protoclj prints the 158-digit result;
Babashka exits 1 with `long overflow` (`out/bench/factorial-100.txt`).

What moved:

- Against the June table, which the README's bolded summary still
  described: `fib` -41 % and `tak` -21 % (protoclj now beats Babashka on
  `fib`), but `reduce-list` and `sum-squares` went from faster than
  Babashka to 1.5-1.6x slower. Babashka itself got faster on them
  (36 -> 19 ms, 31 -> 17 ms), and protoclj's `sum-squares` is +47 % (lazy
  `map`, M28, and `core.clj` at start-up: the 2026-10-24 regression
  report). The summary sentence was wrong for three of five workloads.
- Against 2026-10-20: `fib`, `tak` and `reduce-list` within -5 % to -1 %;
  **`sum-loop` +18 % (65 -> 77 ms)** and **`sum-squares` +22 %
  (23 -> 28 ms)**. Babashka moved +6 % and 0 % on them in the same
  comparison, so the ratios moved +12 % (0.40x -> 0.45x) and +16 %
  (1.39x -> 1.61x). The start-up cost
  of `core.clj` (about +4 ms since the macros merge) explains part of it;
  the rest is not isolated here. The 2026-10-25 linked-list report measured
  78 ms for `sum-loop` on `main` 9df518a, so this is not new with 1e7b612.

### Start-up and footprint (`startup.py`)

Three passes of 50 runs per script and runtime, alternating the runtime on
every run; wall time around the child process, peak RSS from `wait4`
(what `/usr/bin/time -f %M` reports). 600 runs checked: 0 failed.

| | protoclj | bb | previous README |
|---|---:|---:|---|
| `examples/01-hello.clj`, median wall | 20.9 ms (pass medians 20.7-21.2) | 9.7 ms | 21.6 ms (2026-09-15) |
| empty file, median wall | 20.9 ms (20.5-21.7) | 9.7 ms | 24 ms vs bb 11 ms (2026-10-24, perf stat) |
| peak RSS (either script) | 24.2 MB (23.8-24.7) | 15.4 MB | "about 20 MB" (2026-09-15); 23 vs 17 MB (2026-10-24) |

Start-up is 3 % below the 2026-09-15 figure even though `core.clj` is now
loaded at every start; peak RSS is 4 MB (+21 %) above it. Babashka starts
about 2.2x faster and uses 9 MB less.

### Showcase (`../2026-10-23-showcase/run.sh`, unchanged)

Five interleaved rounds, medians, the same seeded inputs (identical
digests to the 2026-10-24 run). Outputs identical across runtimes and runs.

| script | protoclj s [range] | bb s | ratio | protoclj RSS MB [range] | bb RSS MB | 2026-10-24 (protoclj / bb, RSS) |
|---|---:|---:|---:|---:|---:|---|
| grep, 115 MB log | 16.69 [15.75-19.47] | 1.93 | 8.6x | 140 | 134 | 18.04 / 2.08 s, 146 MB |
| JSON transform, 43 MB | 4.13 [3.99-4.58] | 2.51 | 1.6x | 1,424 [1,066-1,710] | 338 | 5.18 / 2.60 s, 1,967 MB |
| word frequencies, 31 MB | 17.20 [16.79-17.72] | 4.67 | 3.7x | 143 | 143 | 19.17 / 4.69 s, 159 MB |
| `(factorial 1000)` with `*'` | 0.02 | 0.01 | - | 24 | 18 | 0.03 / 0.01 s |

protoclj moved -7 % (grep), -20 % (JSON) and -10 % (word frequencies);
Babashka moved -7 %, -3 % and 0 % in the same comparison, so the grep's
ratio to Babashka is unchanged and the JSON transform and word count
improved (2.0x -> 1.6x, 4.1x -> 3.7x). The JSON transform's peak RSS fell
from 1,967 to 1,424 MB but varies by 60 % between rounds (the collector's
timing); it is still 4x Babashka's. `perf stat` task-clock for the empty
script (`out/showcase/summary.txt`): protoclj 18.4 ms, bb 7.4 ms of CPU.

### Actors (`benchmarks/actor-bench.sh`, unchanged)

Three rounds; msg/s per worker count, median [min-max] of the rounds; the
peak is the best median over worker counts 1, 2, 4, 6, 8 and 16. Every run
reported its full 1,000,000 messages: 0 failed.

| mode | peak msg/s (median) | best single run | 2026-09-24 (one run) | change of the peak |
|---|---:|---:|---:|---:|
| `single` | 207,082 (w=16) | 239,120 (w=8) | 222,074 (w=2) | -7 % |
| `fan-out` | 448,632 (w=2) | 482,625 (w=2) | 552,486 (w=8) | **-19 %** |
| `MPSC` | 210,704 (w=4) | 217,439 (w=4) | 201,329 (w=1) | +5 % |
| `MPMC` | 280,113 (w=4) | 306,938 (w=6) | 329,817 (w=16) | **-15 %** |

The 2026-09-24 figures are the best of one run per worker count, which
favours them against a median of three; even against the best single run
of this session, `fan-out` (-13 %) and `MPMC` (-7 %) are lower. The spread
within a cell is up to 1.6x (`single` at w=1: 128-215 k), so the worker
count at which a mode peaks is not stable from round to round. The shape
also changed: `MPMC` no longer grows with the worker count, it peaks at 4
and falls to 171 k at 16. Nothing here isolates the cause; candidates are
the protoCore and protoClojure changes of the month in between, and the
desktop load (the 2026-09-24 run started at load 5.8, comparable). Full table:
`out/summary.md`.

## Files

- `run-all.sh` drives the session; `gate.sh` is the per-round gate;
  `bench-rounds.sh` is `bench.sh` repeated, interleaved and checked;
  `startup.py` measures start-up and RSS; `analyze.py` writes
  `out/summary.md`.
- `out/environment.txt` (machine, commits, `ldd`), `out/load.txt` (the
  gate's log), `out/bench/bench-raw.tsv` (every run), `out/startup/`,
  `out/showcase/` (every run's time, output and exit status),
  `out/actors-r*.txt`.

## Reproducing

    run-all.sh PROTOCLJ BB DATA_DIR OUT_DIR

with `IDLE_GATE` naming a command that waits for builds to finish (here
`.agent_scratch/prim-ctx/tools/waitquiet.sh`), run under
`flock .agent_scratch/build.lock`. `DATA_DIR` is generated by
`examples/showcase/gen-data.py` when missing. The command used:

    flock .agent_scratch/build.lock env IDLE_GATE=.../waitquiet.sh \
        run-all.sh clj-build/protoclj tools/bb-1.4.192/bb data \
        benchmarks/reports/2026-10-25-readme-refresh/out
