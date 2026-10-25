# Primitives in the caller's context (2026-10-25)

Branch `perf/primitives-caller-context` against `main` at `58caf71`, and the
branch with M43's direct primitive calls compiled out (`nofast`,
`-DPROTOCLJ_NO_FAST_PRIMITIVES`), to decide whether M43 stays. Design:
`docs/DESIGN.md` §10.2 M45 (primitives), M46 (redefined operators).

## How it was measured

- Release builds from clean trees, all linking the same protoCore 3.2.0
  (master `bb2d581a`, `libprotoCore.so.3.2.0`, sha256 `17b592fc4c9c67e9`),
  checked with `ldd` (`out/environment.txt`).
- `run.sh`, 5 interleaved rounds, the variant order rotating; every run on a
  fresh copy of its binary and of the library. The machine's build lock was
  held for the whole run; before each round the gate waited for CPU idle
  >= 80 % with no compiler, make or ctest process (`out/gate.txt`: every
  round quiet, idle 80-84 %).
- `perf-ops.sh`: fib, tak, sum-loop under `perf stat -r 10`, twice each,
  alternating (`out/perf-ops.txt`).
- `analyze.py out` writes `summary.md`. Every benchmark prints its result;
  checksums agree across variants and runs, bench.sh results match
  Babashka's, showcase outputs are identical.
- `summary-run1.md` is an earlier run of the same harness on an earlier
  state of the branch. It is kept because it found a real regression:
  the showcase `word-freq` peaked at 5.2 GB, against 159 MB on main, because
  `frequencies` walked a lazy seq without handing its garbage over. Fixed
  (`0404f92`) before the run reported here.

## Results

One primitive call per iteration (cycles per call, work minus setup run):

| primitive | main | nofast | branch | branch vs main |
|---|---:|---:|---:|---:|
| `str` (2 args) | 1935 | 1449 | 1489 | -23 % |
| `assoc` | 2016 | 1721 | 1772 | -12 % |
| `get` | 391 | 350 | 349 | -11 % |
| `max` | 421 | 356 | 371 | -12 % |
| `first` (M43 direct set) | 186 | 250 | 185 | -1 % |
| `reduce` calling a fn that calls `max` | 355 | 332 | 337 | -5 % |

- Outside M43's set a primitive call is 11-23 % cheaper: no context is
  built. The general path still costs `first` 250 cycles where the direct
  call costs 185, so **M43 stays** (DESIGN.md M45).
- `nofast` is cheaper than `branch` by 3-4 % on `str`, `assoc` and `max`,
  primitives M43 does not answer: the direct-call test (one pointer-tag
  compare and a call returning nullptr) is paid by every primitive call.
  Kept: it saves 65 cycles on the hot seq calls and costs 15-50 on the rest.

Workloads the brief named (`perf stat -r 10`, two alternating runs):

| workload | main cycles | branch cycles | instructions |
|---|---:|---:|---:|
| fib | 1.365 / 1.352 G | 1.386 / 1.355 G | +2.4 % |
| tak | 122.2 / 120.6 M | 114.2 / 112.2 M | -4.2 % |
| sum-loop | 220.1 / 220.7 M | 226.1 / 227.6 M | +3.7 % |

- `sum-loop` is +3 % in cycles and `fib` +0.2 to +1.6 %: the operator
  redefinition check (one relaxed load per `ADD` .. `EQ`), the 8 spare slots
  each frame fills, and the before-call test. A first version (16 spare
  slots, the redefinition path and the submission inline) cost fib +7 %;
  moving both out of line and halving the slots brought it to this. The
  rest of sum-loop's +25 instructions per iteration is not attributed.

`benchmarks/bench.sh` (ms, best of 3, median of 5 rounds; the workloads are
short, so 1-3 ms is the resolution): fib 380 -> 393, tak 40 -> 40, sum-loop
72 -> 75, reduce-list 29 -> 30, sum-squares 28 -> 28.

Seq cost cases (`../2026-10-24-seq-cost/list-cost.clj`, ns per element by
the program's own timer; the perf cycles of these cases, work minus a setup
run that builds the list, vary by up to 2x between variants for the control
cases and are not used): at n = 10^6 the branch is within 3 % of main on
every case except `reduce` over a list, 82 -> 42 ns (-49 %), random `nth`
1977 -> 1883 (-5 %), and `count` of a cons, 337 -> 402 (+19 %; at 10^5 it is
317 -> 314, so not a consistent cost). Full tables: `summary.md`.

Start-up (empty file, perf stat -r 50): 67.7 -> 66.1 Mcycles (-2 %).

Showcase (wall s / peak RSS MB, median of 5): grep 16.27/148 -> 16.37/139,
json 4.36/1515 -> 4.14/1248 (-5 %, -18 % RSS), word-freq 17.09/159 ->
16.27/144 (-5 %, -9 % RSS), start-up and factorial unchanged.

## GC and ASan evidence

- Full suite: 872/872 on protoCore 3.2.0 (Linux); CI runs below.
- `PROTOCORE_GC_CONTEXT_THRESHOLD=200` with `PROTOCORE_HEAP_LIMIT_CELLS`
  2,000,000 (and 4,000,000): 870/872; the two failures,
  `deeply-nested-list-raises-stack-overflow` and `long-list-iterative`,
  fail identically on main (their live set exceeds the ceiling: "out of
  memory", live set 1.97 M cells).
- AddressSanitizer build of the whole suite: 41 failures, every one also
  failing on main's ASan build (42 there; main also times out
  `lazy-grep-under-heap-limit`). They are stack depth: ASan's larger frames
  overflow the 512 KiB thread stacks (`StackOverflowError`, or ASan
  `stack-overflow`, 20 reports on each). No other ASan report on either.
- `cli/primitive-callbacks-survive-gc` (new): reduce, lazy map, swap!,
  sort-by, nested reduce, conj through invoke, run!, frequencies, and the
  JSON reader's batch under a nested fold, every result checked, under a
  200-cell threshold. With `CallerSlots::safepoint` sabotaged to submit
  whatever is in flight it answered wrong sums in 2 of 3 runs, so it can
  fail.
- The JSON check on protoCore 3.1.0 (Windows floor job) failed once with a
  builder that safepointed every 64 folds; it safepoints every fold now.

## Files

`run.sh`, `analyze.py`, `prim-cost.clj`, `perf-ops.sh`; raw results in
`out/`; `summary.md` (this run), `summary-run1.md` (the earlier run).
