# protoClojure vs Babashka — benchmarks

> **Most recent figures (2026-10-25, `main` 1e7b612 on protoCore 3.3.0):**
> every number the README quotes, measured again — `bench.sh` workloads
> against Babashka 1.4.192 (fib 0.93x, tak 1.41x, sum-loop 0.45x,
> reduce-list 1.54x, sum-squares 1.61x of Babashka's time; sum-loop +18 %
> and sum-squares +22 % since 2026-10-20), start-up (20.9 ms, 24 MB),
> the showcase scripts and actor throughput (fan-out -19 % and MPMC -15 %
> at their peaks against 2026-09-24), in
> [`reports/2026-10-25-readme-refresh/`](reports/2026-10-25-readme-refresh/README.md).
> The README's earlier performance text is kept at the end of this file,
> under [Earlier README performance sections](#earlier-readme-performance-sections-moved-2026-10-25).
>
> Earlier (2026-10-24, branch `feature/macros`): the regression
> found end to end, its causes and fixes, in
> [`reports/2026-10-24-regression/`](reports/2026-10-24-regression/README.md)
> (after the fixes: start-up +35.7 %, run phase fib +4.4 %, tak +1.7 %,
> sum-loop +1.2 %, reduce-list +0.3 %, sum-squares +12.3 %, map-loop +4.5 %
> against the baseline before macros); the measurement that found it, in
> [`reports/2026-10-24-final-vs-baseline/`](reports/2026-10-24-final-vs-baseline/README.md);
> and the showcase scripts against Babashka in
> [`reports/2026-10-23-showcase/`](reports/2026-10-23-showcase/README.md).

> **Note (2026-09-15):** this is a dated record; its tables are unchanged.
> The following statements in it are wrong or unsupported:
>
> - The per-call figures divide by "2.7M calls"; `fib(30)` makes
>   2,692,537 calls (2 × fib(31) − 1).
> - The `ce9819d` regression was first given as "~10 ns/call on fib";
>   (712 − 620) ms / 2,692,537 calls ≈ 34 ns/call. The text below is
>   corrected.
> - The per-call list under `a099b45` quotes bb at 498 ms; the `a099b45`
>   table records 496 ms (498 ms is the bb time recorded in the commit
>   message of `700f352`). The 184 ns/call figure corresponds to 496 ms.
> - "`fib` and `tak` sit at 1.18–1.45× behind" repeats the ratios of the
>   `700f352` run (commit message). The `a099b45` table gives 1.25× and
>   1.07×; the `ce9819d` table gives 1.36× and 1.21×.
> - The `pmap` timings were taken with `/tmp/map_perf.clj` and
>   `/tmp/pmap_perf.clj`, which are not in the repository. According to
>   the commit message of `ce9819d`, the workload was
>   `(pmap fib (list 30 30 30 30))` against the equivalent `map`.
>
> Removed from this record: the extrapolation from Babashka to JVM
> Clojure and the JVM start-up and JIT estimates (JVM Clojure was never
> measured), and the "remaining headroom" list written after `700f352`,
> which stated that every call performs three attribute lookups
> (superseded by `a099b45`, after which a single-arity call performs
> one) and repeated the 1.18–1.45× ratios.

**Date.** 2026-06-14. This snapshot collects the measurements taken that
day across successive builds, from the first benchmark (commit `ac85269`)
through the addition of watches, promises and parallel `pmap` (commit
`ce9819d`). Build labels below refer to those commits.
**Setup.** AMD Ryzen 5 5500U (6 cores), Linux 6.17.0.
**Runtimes.**
- `protoclj` — protoClojure, `build_release/` (Release), at the commits named in each section.
- `bb` — Babashka 1.4.192 (GraalVM-native).
- JVM Clojure was not measured; no JVM Clojure installation was available.

**Methodology.** Each workload is a single-form `.clj` file. The harness
runs the script through each interpreter 3 times and reports the
minimum wall-clock (cold-start + execution). No JVM warmup phase.
Result strings are compared across runtimes for correctness.

## Scripting vocabulary (Part C) against stage 11 (`8d72602`), 2026-10-23

What the Part C additions to `core.clj` (DESIGN.md M34) cost at start-up.
Script and raw output: [`reports/2026-10-23-vocabulary/`](reports/2026-10-23-vocabulary/),
quiet gate (`QUIET_CORES=4.0`), fresh copies of the same protoCore 3.1.0
library, interleaved.

Start-up, an empty file, `perf stat -r 20`, three passes: stage 11 63.6 /
62.7 / 62.8 M cycles (16-17 ms), Part C 69.7 / 67.2 / 67.9 M cycles (17-19
ms): **+4.5 to 6 M cycles, about 1.2-1.6 ms**, for the 28 functions and two
macros `core.clj` gained. With the stage-6 load measured at 3.4 ms, `core.clj`
now costs about 5 ms of every start, under the brief's 10 ms; moving the
functions to C++ or precompiling `core.clj` would recover it.

`bench.sh`, two rounds (ms, start-up included): stage 11 fib 356 / 363, tak
36 / 37, sum-loop 66 / 66, reduce-list 32 / 32, sum-squares 27 / 27; Part C
fib 344 / 340, tak 38 / 38, sum-loop 69 / 69, reduce-list 33 / 33,
sum-squares 28 / 29. The 1-3 ms on the short workloads is the start-up
above; no run-time path changed.

## Lazy sequences (stage 11) against Part A (`ed3cf5a`), 2026-10-23

Whether making `map`, `filter`, `for`, `range` and `seq` lazy (DESIGN.md
M28-M30), and the VM's locals and argument clearing, slowed the eager
workloads. Script, programs and raw output:
[`reports/2026-10-23-lazy-seqs/`](reports/2026-10-23-lazy-seqs/). Both
binaries on their own fresh copy of the same `libprotoCore.so.4` (protoCore
3.1.0, branch `feature/lazy-object`; Part A's binary, built against 3.0.0,
runs on it unchanged), behind the quiet gate (`QUIET_CORES=4.0`), order
alternated; every program prints its result and both binaries printed the
same.

`bench.sh`, best-of-3 per round, the three rounds (ms, start-up included):

| workload | Part A | stage 11 |
|---|---|---|
| fib | 334 / 347 / 345 | 345 / 346 / 344 |
| tak | 37 / 38 / 37 | 37 / 40 / 36 |
| sum-loop | 66 / 66 / 66 | 65 / 65 / 66 |
| reduce-list | 32 / 31 / 31 | 30 / 31 / 31 |
| sum-squares (lazy `map` over a list now) | 27 / 25 / 27 | 27 / 26 / 27 |

Unchanged within the rounds' spread.

Five interleaved rounds of `perf stat` cycles, medians (Mcycles, min-max):

| program | Part A | stage 11 | |
|---|---|---|---|
| `macro-loop.clj` (5 M user-fn calls) | 5504 (5425-5561) | 5641 (5552-5677) | **+2.5 %** |
| `map-loop.clj` (keyword `get` / `assoc`) | 3691 (3639-3747) | 3805 (3687-4397) | **+3.1 %** |
| `doseq` over a 300,000-element vector | 3737 (3586-4003) | 3376 (3312-3838) | -9.7 % |
| `loop` / `next` over the same vector | 3144 (3037-3422) | 2843 (2814-2929) | -9.6 % |
| `reduce` of `map` and `count` of `filter` over it | 4532 (4009-4712) | 2979 (2945-3529) | -34 % |
| `map` over a 100,000-element list | 1840 (1836-1892) | 1819 (1749-1874) | -1.1 % |
| nested `for` with `:when`, 90,000 pairs | 1224 (1065-1248) | 406 (394-422) | -67 % |

The two loops that make no sequence at all are 2.5-3 % slower, with
overlapping ranges for the map loop. Not explained further: the VM now
clears each argument slot as a call copies it (one store per argument) and
has one more opcode in its dispatch switch; on `fib` an A/B build measured
0.5 % fewer instructions than Part A at equal or higher cycles, the pattern
of a code-layout change rather than of work added. The sequence idioms are
faster: chunked `map` / `filter` over vectors and ranges, and a `for` that no
longer concatenates its inner results into a vector.

Start-up, an empty file, `perf stat -r 20`, two passes: Part A 61.07 M /
61.07 M cycles (16 ms), stage 11 61.88 M / 62.53 M cycles (16-17 ms): +0.8 M
cycles, about 0.2 ms; `core.clj` lost the eager `range` and `__for-step`
and gained `lazy-seq` and `lazy-cat`.

A grep over the standard input (`doseq` over `(line-seq *in*)` with
`re-find`, 300,000 lines of 60 bytes): 3.0 s, against 2.2 s for the same
match in a `read-line` loop; the difference, about 3 µs per line, is the lazy
seq and `doseq`'s `seq` / `first` / `next` calls, and the rest is mostly
`std::regex` and string conversion (perf). Under a 1,000,000-cell ceiling a
3,000,000-line (182 MB) stream ran in 74 MB of resident memory with a last
live set of 6,719 cells.

## Keyword `get` / `assoc`: the stage-10 regression fixed, 2026-10-23

Stage 10's `map-loop.clj` ran 7 % slower than stage 6 because the map key
semantics asked every object key whether it is a set (`keyIsIdentity`,
`src/runtime/MapOps.cpp`), one out-of-line `getPrototype` per keyword. The
fix: (a) a hit in the named-value cache (`isKnownNamed`,
`src/runtime/Named.h`), a direct-mapped table of the interned keywords and
symbols, answers "identity key" before the prototype is read; a recorded
address can never be reused because named values are never collected; (b)
the VM's call path tests for a keyword callable before it asks whether the
callable is a map or a set, and `setPrototype()` is inline. Script and raw
output: [`reports/2026-10-23-keyword-get/`](reports/2026-10-23-keyword-get/):
stage 6 (`e5a48c5`), stage 10 (`1f14ba4`) and the fix, 9 interleaved rounds
of `perf stat` cycles, each binary on its own fresh copy of
`libprotoCore.so.4`, behind the quiet gate (`QUIET_CORES=4.0`). Medians
(Mcycles, min-max):

| loop | stage 6 | stage 10 | fix |
|---|---|---|---|
| `map-loop.clj` | 3658 (3587-3686) | 3929 (3843-4016), +7.4 % | 3688 (3639-3748), +0.8 % |
| `macro-loop.clj` | 5213 (5186-5443) | 5251 (5220-5594) | 5308 (5226-5500), +1.8 % |

Four builds were measured (the cache alone, plus the inline
`setPrototype()`, plus the reordered call test, both together); the
map-loop median of the fix ranged from +0.0 % to +3.1 % against stage 6 and
the macro loop's from -0.6 % to +1.8 %, with overlapping ranges: what
remains is inside the run-to-run and layout noise of this machine. The
macro loop touches no map; its +1-2 % in the chosen build is not explained
by the change and was +0.6 % in another build of the same source minus the
call reorder.

## Scripting vocabulary: stage 6 (`e5a48c5`) against stage 10 (`71d34d2`), 2026-10-23

Whether Part B stages 7-10 (destructuring, sets, regular expressions,
`format`, `for`, the larger `core.clj`) changed run-time speed or start-up.
Script, workloads and raw output:
[`reports/2026-10-23-scripting/`](reports/2026-10-23-scripting/). Clean
worktree builds of both commits, each on its own fresh copy of the same
`libprotoCore.so.4` (checked with `ldd`), behind the quiet gate
(`QUIET_CORES=4.0`; 1.3-2.3 busy CPUs at the gates), order alternated.

`bench.sh`, best-of-3 per round, median of 3 rounds (ms, start-up included):

| workload | stage 6 | stage 10 |
|---|---|---|
| fib | 348 | 354 |
| tak | 37 | 38 |
| sum-loop | 69 | 69 |
| reduce-list | 30 | 31 |
| sum-squares | 25 | 27 |

Every result matched Babashka's; the differences are inside the rounds'
spread (fib 343-366 on stage 10, 347-353 on stage 6).

Interleaved loops, 5 runs each, median wall time: `macro-loop.clj` 1.49 s
(stage 6) and 1.51 s (stage 10), inside the spread (1.48-1.55 s).
`map-loop.clj` (2,000,000 rounds of keyword `get`, keyword-as-fn, map-as-fn
and `assoc`): **1.02 s against 1.09 s, +7 %** (user time 0.87 s against
0.95 s), outside the spread. An A/B build without the set test in
`keyIsIdentity` (MapOps.cpp: every object key is now asked whether it is a
set, one `getPrototype` per keyword key) ran at 0.88 s of user time against
0.915 s for stage 10 and 0.85 s for stage 6, on four interleaved runs: the
set test explains about half of it; the rest is not explained and may be
the library placement effect seen before. Fixed afterwards; see the next
section.

Start-up, an empty file, `perf stat -r 20`, two interleaved passes: stage 6
16.5 / 15.5 ms and 60.7 M / 58.3 M cycles, stage 10 15.9 / 17.0 ms and
60.6 M / 60.6 M cycles. The larger `core.clj` (`into`, `range`, the `doseq`
and `for` helpers) costs about 1 M cycles (0.3 ms), inside the spread.

## Macros branch: `main` (`8b7938a`) against stage 6 (`e5a48c5`), 2026-10-23

Whether the macro work (`defmacro`, the embedded `core.clj`, `when` /
`when-not` / `cond` / `with-open` / `future` moved to `core.clj`) changed
run-time speed or start-up. Script and the macro-heavy workload:
[`reports/2026-10-23-macros/`](reports/2026-10-23-macros/). Clean worktree
builds of both commits, each on its own fresh copy of the same
`libprotoCore.so.4` (checked with `ldd`), every run behind the quiet gate
(`QUIET_CORES=4.0`; busy CPUs 1.8-3.1 at the gates), order alternated.

`bench.sh`, wall-clock best-of-3 per round, median of 3 rounds (ms; each run
includes start-up):

| workload | main | stage 6 |
|---|---|---|
| fib | 379 | 379 |
| tak | 34 | 38 |
| sum-loop | 71 | 71 |
| reduce-list | 29 | 32 |
| sum-squares | 27 | 27 |

Every result matched Babashka's. The 3-4 ms on the short workloads is the
start-up below; the long ones did not move.

Macro-heavy loop (`macro-loop.clj`: `cond`, `when`, `when-not` in a
5,000,000-iteration body; C++ forms on `main`, `core.clj` macros on stage
6; both print 19998890), 5 interleaved runs: median 1.65 s (`main`) and
1.66 s (stage 6), inside the run-to-run spread (1.63-1.69 s).

Start-up, an empty file, `perf stat -r 20`, two interleaved passes:

| build | task-clock (ms) | cycles |
|---|---|---|
| `main` | 12.7 / 13.2 | 48.4 M / 48.1 M |
| stage 3 (`c2a0c4a`, no `core.clj`) | 13.6 / 14.0 | 48.9 M / 46.1 M |
| stage 6 | 17.6 / 16.7 | 60.7 M / 58.4 M |

Loading `core.clj` (stage 6 against stage 3) costs about 3.4 ms of CPU and
12 M cycles per process, under the 10 ms budget; it is not cached or
precompiled.

## Numbers at commit `ce9819d` (after maps, strings, atoms, futures, watches, promises and parallel `pmap`)

| Workload         | protoclj (ms) | bb (ms) | ratio   | Result        | Match |
|------------------|--------------:|--------:|--------:|---------------|:-----:|
| `fib(30)`        |           712 |     524 | 1.36×   | 832040        | ✓ |
| `tak(18,12,6)`   |            52 |      43 | 1.21×   | 7             | ✓ |
| `sum-loop(1M)`   |            79 |     219 | 0.36×   | 500000500000  | ✓ |
| `reduce-list(10K)` |          27 |      36 | 0.75×   | 49995000      | ✓ |
| `sum-squares(1K)` |           19 |      31 | 0.61×   | 332833500     | ✓ |

vs commit `a099b45` (the perf high-water mark): fib 620→712 (+15%),
tak 45→52 (+16%), sum-loop 66→79 (+20%). Between the two commits, five
new prototype pointers and ten new key pointers were added to the
`run()` signature and threaded through every recursive call, which is
the likely cause. The cost is about 34 ns/call on fib; protoClojure
remains faster than Babashka on 3 of 5 workloads. Collapsing the `run()`
parameters into a single struct is a possible way to recover this cost.

**New at `ce9819d` — parallel pmap on real OS threads:**

  $ time protoclj /tmp/map_perf.clj    # 4× fib(30) via sequential map
    real    0m2.800s   user    0m2.790s

  $ time protoclj /tmp/pmap_perf.clj   # 4× fib(30) via pmap
    real    0m0.862s   user    0m3.323s

  Wall-clock speedup: 3.25×. Same pattern as the explicit futures added
  in commit `5830237`, but the surface is now `(pmap f coll)`, as in
  Clojure.

## Numbers at commit `a099b45` (after split fn prototypes + captures-only-if-needed)

| Workload         | protoclj (ms) | bb (ms) | ratio   | Result        | Match |
|------------------|--------------:|--------:|--------:|---------------|:-----:|
| `fib(30)`        |           620 |     496 | 1.25×   | 832040        | ✓ |
| `tak(18,12,6)`   |            45 |      42 | 1.07×   | 7             | ✓ |
| `sum-loop(1M)`   |            66 |     196 | **0.34×** | 500000500000 | ✓ |
| `reduce-list(10K)` |          29 |      33 | 0.88×   | 49995000      | ✓ |
| `sum-squares(1K)` |           19 |      28 | 0.68×   | 332833500     | ✓ |

### Trajectory on `fib(30)`: `ac85269` → `700f352` → `a099b45`

| Build     | Wall (ms) | Cycles | Instructions | IPC  | L1-d misses |
|-----------|----------:|-------:|-------------:|-----:|------------:|
| `ac85269` |      4113 | (n/a)  |       (n/a)  | (n/a)|       (n/a) |
| `700f352` |       720 |  2.45G |        5.93G | 2.42 |       11.2M |
| `a099b45` |       620 |  2.05G |        5.02G | 2.45 |        7.6M |

Per-call:
- `700f352`: 720 ms / 2.7M calls = **267 ns/call**
- `a099b45`: 620 ms / 2.7M calls = **230 ns/call**  (−14%)
- bb:  498 ms / 2.7M calls = **184 ns/call**

Per call, protoClojure is now ~25% slower than Babashka. The residual ~46 ns/call sits in
frame setup (new ProtoContext, resizeAutomaticLocals, captures
seeding, recursive run() entry) — not in attribute lookups. The next
lever is either a ProtoContext pool (D — invasive) or threaded
dispatch in run(). IPC at 2.45 with 0.07% branch-miss rate suggests
threaded dispatch will return less than expected; the pool likely
more.

Reading the ratio: `<1` means protoClojure is faster than Babashka by
that factor. Three of five workloads now run **faster than Babashka**;
`fib` and `tak` sit at 1.18–1.45× behind.

## What changed since the first benchmark (`ac85269`)

The first benchmark's ratios were 8.3× / 2.3× / 8.1× / 1.5× / 0.53×
(slower than bb across the board on compute-bound rows). Commits
`700f352` and `a099b45` closed that gap. The changes:

### `a099b45` — fewer getAttribute calls per CALL

protoCore already runs a 1024-entry
per-thread attribute cache plus a mutable-snapshot cache, so adding
a parallel inline cache at the protoClojure layer would be redundant.
The real lever is to **ask for fewer attribute lookups per call.**

Concretely:

1. **Split fn prototypes.** Single-arity wrappers now use
   `fnSingleProto`; multi-arity use `fnMultiProto`. The CALL handler
   picks the path by `getPrototype` alone — no more
   `getAttribute(aritiesKey)` probe on every single-arity call. (The
   negative-hit was cheap in the cache but still a load + compare
   per call.)

2. **Captures-only-if-needed.** When the body's `captureCount() == 0`
   (top-level defns: fib, sum-to-n, factorial, ...), the dispatcher
   skips `getAttribute(capturesKey)` entirely.

3. **arityKey vestigial-write removed.** The wrapper used to carry
   `__arity__` redundantly; the dispatcher always reads arity from
   `subMod->arity()` instead.

Single-arity CALL goes from 3 getAttribute → **1** (just `bytecodeKey`).

`perf stat` confirmation on fib(30):
- instructions: 5.93G → 5.02G (−15%)
- L1-d misses: 11.2M → 7.6M (−32%)
- branch-miss / IPC unchanged → no new dispatch overhead

### `700f352` — SmallInt fast-path

1. **SmallInt fast-path binary opcodes** (`ADD` `SUB` `MUL` `LT` `LE`
   `GT` `GE` `EQ`). The compiler now emits these directly for
   `(+ x y)` / `(< x y)` / etc. when the operator name is not
   shadowed by a local binding. The VM short-circuits on the hot
   path — both operands tagged SmallInt — with a one-line C++
   arithmetic op and a re-tagged push, skipping the `ProtoMethod`
   indirection, the ProtoList wrap of args, and the per-arg tag
   checks the primitive used to do.

2. **Slow-path routed through protoCore directly.** When operands
   are not both SmallInt (Float, LargeInteger, mismatched), the VM
   calls `a->add(ctx, b)` / `a->multiply(...)` / `a->compare(...)`
   on protoCore's promoting arithmetic API. This is the
   "infinite-precision path" the kernel already implements: SmallInt
   ↔ LargeInteger ↔ Float promotion is automatic. Before this
   change, the VM did a global-namespace lookup of the operator and a
   full `dispatchCall`.

## LargeInteger correctness

`factorial(100)` runs through to completion:

```
2432902008176640000                                                ; 20!
51090942171709440000                                               ; 21!
30414093201713378043612608166064768844377641568960512000000000000  ; 50!
93326215443944152681699238856266700490715968264381621468592963895217599993229915608941463976156518286253697920827223758251185210916864000000000000000000000000  ; 100!
```

Babashka 1.4 fails on `factorial(21)` with `long overflow` because
JVM Clojure's default `*` is long-arithmetic; the programmer has to write
`*'` or `(bigint 1)` for explicit promotion. **protoClojure promotes
automatically** because the SmallInt fast-path falls through to
protoCore's `multiply` when the inline result no longer fits.

Three conformance fixtures cover this:
- `tests/conformance/15-bigint/factorial-21.clj` (51090942171709440000)
- `tests/conformance/15-bigint/factorial-30.clj` (30414093201713378043612608166064768844377641568960512000000000000)
- `tests/conformance/15-bigint/large-add.clj` ((2^63 − 1) × 2 = 18446744073709551614)

Plus `benchmarks/factorial-100.clj` for the demo.

## Caveats — still

1. **Cold-start + execution.** Each run is one process, so start-up
   time is included for both runtimes. A multi-iteration inner loop
   would dilute start-up.
2. **Workloads still small.** No allocation pressure on
   `fib` / `tak` / `sum-loop`. The volume is modest on the rest.

## Reproducibility

```bash
./benchmarks/bench.sh                  # uses /tmp/proto-bench/bb by default
./benchmarks/bench.sh path/to/protoclj path/to/bb
```

---

# Actor mailboxes: three `ProtoMPSCQueue`s per actor (2026-09-24)

**What changed.** Each actor's three priority mailboxes moved from
`std::atomic<ActorMessage*>` stacks of C++ heap nodes to three protoCore
`ProtoMPSCQueue`s (platform Track C; PMQ-SPEC §6 step 4). The point of the
change is GC safety, not speed: the old heap nodes held the message's
function, arguments and promise as pointers **nothing rooted**, so a
collection between a send and its handler could free a payload the mailbox
was the only reference to.

**Setup.** AMD Ryzen 5 5500U (6 cores, 12 threads), Linux 7.0.0.
`benchmarks/actor-bench.sh`, 1,000,000 messages per mode, both binaries built
Release against protoCore 2.1.0. BEFORE is `a6b722d~1`, AFTER is `a6b722d`.

**Contention.** The machine was shared with a desktop session (editor,
browser) throughout; two sibling agents were benchmarking other runtimes in
the same workspace, so a first pair of runs was measured at load average 12
and is discarded — the tables below are the second pair, taken back to back
with the load average at 5.6 (BEFORE) and 5.8 (AFTER) when each started, in
the order AFTER then BEFORE so the order cannot flatter the new code. The
BEFORE column reproduces the numbers recorded on an idle machine on
2026-09-16 (313,578 msg/s single, 554,939 fan-out, 282,886 MPSC, 359,197
MPMC) to within a few percent, which is the check that these runs were not
themselves contended. Every run self-reports the messages it processed and
the runner verifies the count, so none of these rows is a crash.

| mode | workers | BEFORE msg/s | AFTER msg/s | change |
|---|---|---|---|---|
| single | 1 | 298,063 | 201,288 | −32% |
| single | 2 | 313,775 | 222,074 | −29% |
| single | 4 | 312,695 | 220,751 | −29% |
| single | 6 | 310,270 | 219,829 | −29% |
| single | 8 | 293,513 | 213,265 | −27% |
| single | 16 | 303,859 | 211,149 | −31% |
| fan-out | 1 | 241,371 | 249,750 | +3% |
| fan-out | 2 | 274,424 | 402,091 | +47% |
| fan-out | 4 | 304,229 | 548,246 | +80% |
| fan-out | 6 | 366,569 | 515,198 | +41% |
| fan-out | 8 | 395,570 | 552,486 | +40% |
| fan-out | 16 | 398,565 | 533,903 | +34% |
| MPSC | 1 | 199,960 | 201,329 | +1% |
| MPSC | 2 | 254,388 | 187,547 | −26% |
| MPSC | 4 | 294,899 | 198,649 | −33% |
| MPSC | 6 | 290,360 | 200,683 | −31% |
| MPSC | 8 | 272,406 | 201,329 | −26% |
| MPSC | 16 | 287,109 | 199,005 | −31% |
| MPMC | 1 | 242,132 | 190,187 | −21% |
| MPMC | 2 | 327,119 | 268,891 | −18% |
| MPMC | 4 | 326,692 | 329,708 | +1% |
| MPMC | 6 | 350,141 | 324,466 | −7% |
| MPMC | 8 | 366,034 | 300,302 | −18% |
| MPMC | 16 | 351,619 | 329,817 | −6% |

**Reading it.** The split is consistent and has a plausible mechanism, which
is worth more than the individual percentages:

- **Fan-out is 34–80% faster.** 1,000 actors × 1,000 messages is dominated by
  the *producer* side. A push now takes one cell from protoCore's per-thread
  arena; it used to take one `new ActorMessage`, i.e. the process-wide
  allocator, from several threads at once.
- **Single-actor, MPSC and MPMC are 6–33% slower.** All three are bounded by
  one *consumer*, and the consumer got more work: `takeAll` builds a
  `ProtoList` of the batch (one node per message) and each message is a
  three-element `ProtoList` read by index, where the old drain walked an
  intrusive pointer chain and read three struct fields.
- Resident memory on the single-actor run rises from 2.5 GB to 3.9 GB, for the
  same reason: a message is now two cells the collector can see rather than
  one malloc'd node the worker freed by hand.

**Verdict.** A 6–33% throughput cost on single-consumer workloads, a 34–80%
gain on many-actor workloads, and mailboxes whose payloads the collector can
see. The cost is on the side of the pipeline that a `ProtoMPSCQueue` cannot
make cheaper — protoCore's `takeAll` returns a `ProtoList`, and building it is
inherent — so closing the remaining gap would be a protoCore change (a drain
that hands back the node chain without materialising a list), not a
protoClojure one.

---

# Vectors off the interned `ProtoTuple` (2026-09-24, decision R2)

**What changed.** A vector became the `ProtoList` of its elements inside a
one-entry `ProtoSparseList` box, where it was an interned `ProtoTuple` that
protoCore never frees (`CHANGELOG.md`, Decisions, R2).

**Setup.** Same machine as the actor tables above, settled (load average 4.7
at the start of the run, no other benchmark running). `/usr/bin/time`, three
runs of the access probe and two of the build probe, both self-reporting the
work done (`:count`, `:sum`) so a crash cannot read as a win. The scripts are
reproduced below rather than committed, because they probe a representation
rather than a language feature.

| probe | BEFORE (interned tuple) | AFTER (boxed ProtoList) |
|---|---|---|
| build a 200,000-element vector with `vec`, then one full `nth` scan | 0.76, 0.81 s / 545 MB | 0.47, 0.49 s / 296 MB |
| build a 20,000-element vector, then 50 full `nth` scans | 0.73, 0.65, 0.59 s | 0.65, 0.61, 0.69 s |

**Reading it.** Construction dominates the first probe and the box wins it
outright: `vec` of a list is now O(1), where building a tuple hashed and
interned every node. The second probe isolates indexed access, where the new
representation is structurally worse — a `ProtoList` node holds one element
against a `ProtoTuple` node's four, so about twice the node hops — and the two
are indistinguishable: at 20,000 elements the extra hops sit below the
interpreter's own per-iteration cost. A random-access workload on a vector of
millions of elements is where the difference would show; no such workload
exists in the suite or the benchmarks.

```clojure
;; access probe (vec-nth): one build, R full scans
(defn build [n] (loop [i 0 acc (list)] (if (>= i n) (vec acc) (recur (+ i 1) (cons i acc)))))
(defn scan  [v n] (loop [i 0 sum 0] (if (>= i n) sum (recur (+ i 1) (+ sum (nth v i))))))
(defn scans [v n r] (loop [k 0 sum 0] (if (>= k r) sum (recur (+ k 1) (+ sum (scan v n))))))
(def N 20000) (def R 50) (def v (build N))
(println :count (count v) :scans R :sum (scans v N R))
```

---

# Earlier README performance sections (moved 2026-10-25)

The text below is what README.md carried before the 2026-10-25 refresh, moved
here unchanged except for heading levels and link paths, so that the README
keeps only the current measurement and pointers. The figures are dated and
superseded by [`reports/2026-10-25-readme-refresh/`](reports/2026-10-25-readme-refresh/README.md).

### Actors (README of 2026-09-24)

Numbers below were measured on 2026-09-24 on an AMD Ryzen 5 5500U (6 cores, 12 threads), on a machine shared with a desktop session (load average 5.8 at the start of the run, no other benchmark running). Each row is **1,000,000 messages** with the trivial body `(inc v)`; runs take 2-5 seconds. The runner is [`benchmarks/actor-bench.sh`](actor-bench.sh), which verifies that every script reports the expected message count before computing a rate. The peak column is the best of the worker counts 1, 2, 4, 6, 8 and 16, with the count in brackets.

| mode      | what it measures                                              | peak msg/s |
|-----------|---------------------------------------------------------------|-----------:|
| `single`  | 1 sender × 1 actor — per-actor pipeline floor                 | 222,074 (w=2) |
| `fan-out` | 1 sender × 1000 actors (1000 msgs each) — ready queue stress  | 552,486 (w=8) |
| `MPSC`    | 4 senders × 1 actor — per-actor sender contention             | 201,329 (w=1) |
| `MPMC`    | 4 senders × 4 actors (round-robin) — both contention paths    | 329,817 (w=16) |

These are upper bounds for a single-operation message body. Worker-count scaling on this machine: `single` and `MPSC` are flat across worker counts (the single consumer is the bound); `fan-out` peaks at 4-8 workers and stays within 4% of the peak up to 16; `MPMC` grows with the worker count.

Against the pre-migration mailbox (three atomic-pointer stacks of C++ heap nodes), measured back to back on the same machine: **fan-out is 34-80% faster** — a push now takes one cell from protoCore's per-thread arena instead of one `new` from the process-wide allocator — while **single, MPSC and MPMC are 6-33% slower**, because all three are bounded by one consumer and the consumer now materialises each batch as a `ProtoList`. The full before/after tables, the contention notes and the mechanism are in [`benchmarks/RESULTS.md`](RESULTS.md). What the migration bought is mailboxes the collector can see: the old nodes held the message's function, arguments and promise as pointers nothing rooted.

Measured on 2026-06-14, the move from a per-actor `std::mutex` + `std::deque` to the lock-free atomic-pointer stacks (the representation the queues replaced) had measured **+4-17% (single)**, **+30-41% (fan-out at 2-4 workers)**, **+11-19% (MPSC at 2 or more workers)** and **±0-3% (MPMC)**.

### Performance — what is measured (README of 2026-10-24)

**Seq cost and cons lists (measured 2026-10-24).** `main` e1a4136 against
the VM's direct seq primitives (A, e2b1e53, rebased as dd1ba9e) and lists
as cells (B, 563cf14, rebased as 47f4c50), clean Release worktrees on
protoCore 3.2.0, fresh copies of each
binary and library per run, 5 rotated rounds, the machine 82-87 % idle.
The list-cost cases at n = 10^6, ns per element (each case's own timer):

| case | main | A | B | Babashka |
|---|---:|---:|---:|---:|
| list walk with `rest` | 1304 | 1181 | **288** | 175 |
| vector `(seq v)` walk | 848 | 567 | 608 | 252 |
| `reduce +` over a list | 161 | 117 | **85** | 24 |
| cons-build, then count | 993 | 1027 | **432** | 319 |
| sequential `nth` (vector) | 389 | 292 | 292 | 273 |
| random `nth` (vector) | 2141 | 2081 | 2003 | 783 |
| `empty?` + `first` (list) | 331 | 211 | 195 | 186 |

`bench.sh` moved by -3 % to +0 % (`reduce-list` -10 %); start-up is +3.9 %
cycles (core.clj's lists are four protoCore cells per element), and
`(vec list)` is O(n log n) where it was O(1). Details, profiles and what got
slower: [`benchmarks/reports/2026-10-24-seq-cost/`](reports/2026-10-24-seq-cost/README.md).

**The macros branch end to end, after the regression fixes (measured
2026-10-24).** Baseline `main` 8b7938a against `feature/macros` before
(3b75e41) and after (eb274bb) the fixes, clean Release worktrees on
protoCore 3.1.0 (the baseline also on 3.0.0, which changes nothing), fresh
copies of each binary and library per run, 5 rotated rounds behind a quiet
gate. Run phase = a long variant of each workload minus an empty file,
`perf stat` cycles:

| | baseline | before fixes | after fixes | after vs baseline |
|---|---:|---:|---:|---:|
| start-up (empty file) | 47.3 Mcycles | 67.9 Mcycles | 64.1 Mcycles | **+35.7 %** (about +4 ms) |
| `fib` run phase | 3049 Mcycles | 3224 | 3184 | +4.4 % |
| `tak` | 1705 | 1777 | 1734 | +1.7 % |
| `sum-loop` | 1471 | 1492 | 1489 | +1.2 % |
| `reduce-list` | 1661 | 1649 | 1666 | +0.3 % |
| `sum-squares` (lazy `map`) | 878 | 1199 | 987 | +12.3 % |
| map-loop (keyword `get` / `assoc`) | 3553 | 3862 | 3714 | +4.5 % |

`benchmarks/bench.sh` (ms, one process per run, start-up included): fib
336 / 354 / 348, tak 33 / 39 / 38, sum-loop 63 / 69 / 67, reduce-list
27 / 33 / 31, sum-squares 22 / 29 / 27 (baseline / before / after); these
short programs mostly measure start-up.

What costs what: start-up is `core.clj`, read and compiled at every start
(the intern caches took 4 M cycles off it; the rest needs a snapshot or
on-demand compilation, STATUS.md "Known issues"). `sum-squares` pays for
`map` being lazy, as in Clojure (a lazy object, a cons and a context per
element). map-loop crosses the collector's initial 2 M-cell soft limit,
which the baseline missed by 66 k cells, and runs one cycle. `fib` and `tak`
run 1-2 % more instructions; the rest of their difference is the dispatch
loop's code layout, which moves either way with unrelated edits (not
isolated). Bisect, profiles and the two reverted experiments:
[`benchmarks/reports/2026-10-24-regression/`](reports/2026-10-24-regression/README.md).

**protoCore's unified exception model (measured 2026-10-22).**
- Branch `feature/protocore-3-calling-convention` at `bef23b0`, against
  `4c5d3aa` (protoCore `cf52f9f5`). Both Release builds, interleaved, three rounds. The machine had 2-4 busy CPUs, so only the ratios count, not the milliseconds.

| | AFTER / BEFORE |
|---|---:|
| `bench.sh` geomean | 0.962 (`fib` −10 %, rounds do not overlap; AFTER / Babashka 1.00x) |
| `try` that does not throw | 0.99 |
| `throw` and `catch`, same function | **0.87** (confirmed in a reversed pass: 0.86) |
| throw caught 5 frames up | 1.02 |
| `(/ 1 0)` caught | **1.10** (confirmed: 1.09), about +2,200 cycles; likely the error factory building the `ArithmeticException`, not isolated |

- No JVM Clojure was available. Babashka is the comparable, at 42,910
  cycles for a same-function catch and 10,736 for a caught `(/ 1 0)`.
- Raw results: [`benchmarks/reports/2026-10-22-unified-exceptions/`](reports/2026-10-22-unified-exceptions/).

**protoCore 2.14.8 and the call path (measured 2026-10-20).**
`benchmarks/bench.sh` against Babashka 1.4.192, three rounds, each run behind
a quiet gate (1.0-2.4 busy CPUs) and on a fresh copy of protoCore 2.14.8;
`main` before (`354e728`) and after (`ef3b328`) merging `perf/call-path`,
alternating, Release; medians of the three rounds (each round is bench.sh's
best of three).

| Workload | protoclj ms | bb ms | ratio | before the merge | 2.14.7 (2026-10-19) |
|---|---:|---:|---:|---:|---:|
| `fib` | 424 | 423 | **1.00x** | 437 ms, 1.03x | 489 ms, 1.20x |
| `tak` | 43 | 27 | 1.59x | 45 ms, 1.67x | 46 ms, 1.74x |
| `sum-loop` | 65 | 162 | **0.40x** | 67 ms, 0.41x | 64 ms, 0.41x |
| `reduce-list` | 30 | 18 | 1.67x | 30 ms, 1.67x | 30 ms, 1.67x |
| `sum-squares` | 23 | 17 | 1.39x | 22 ms, 1.33x | 23 ms, 1.35x |

`fib` now matches Babashka: 489 -> 437 ms comes from protoCore 2.14.8's
context slot stack (a call's 64-127 slots no longer go through
`new`/`delete`), 437 -> 424 ms from the call path. The call path's changes
(own reads of the function wrapper's attributes and of globals, one
argument copy, the result rooted in the caller's stack slot) move every
workload by -4 % to +4.5 %, within the spread; what they fix is that a
returned object was reachable from no root between the callee's return and
the caller's next store. The JVM figures of 2026-10-19 below are unchanged.
Raw results: [`benchmarks/reports/2026-10-20/`](reports/2026-10-20/).

**protoCore 2.14.7 (measured 2026-10-19).** 2.14.7 changes the write path: a
new value for an existing key copies the root-to-key path without rebalancing,
and allocating a cell no longer takes a spinlock. The same binaries on
protoCore 2.14.6 and 2.14.7, each run loading a fresh copy of the library,
three rounds: `benchmarks/bench.sh` against Babashka 1.4.192 with 2.14.7:
`fib` 489 ms (1.20x bb), `tak` 46 ms (1.74x), `sum-loop` 64 ms (0.41x),
`reduce-list` 30 ms (1.67x), `sum-squares` 23 ms (1.35x), unchanged from
2.14.6 (geomean 0.0 %). The same five programs on Clojure 1.12.0 on OpenJDK 25
(first measured here, median of three): 813-864 ms as a process (JVM
start-up), 6-30 ms in steady state inside one process, 3-16x faster than
protoClojure once warm. An `assoc` costs about 1,225 cycles. Measured under
TLP's battery power profile (powersave, EPP balance_power): compare ratios,
not absolute times, with older figures. Details: [protoCore's
report](https://github.com/numaes/protoCore/blob/master/docs/reports/2026-10-18-write-path.md).

**protoCore 2.14.6 (measured 2026-10-13).**  `/` on two integers reaches
protoCore's `divide`, which no longer allocates a temporary: a loop of
2,000,000 divisions, the same commit (`359d8f6`) built against protoCore
2.14.5 and 2.14.6, -36 % cycles (A/A +1.9 %).  `benchmarks/bench.sh` (none
of its workloads divides) against Babashka 1.4.192, three rounds, with
2.14.6: `fib` 471 ms (1.17x bb), `tak` 45 ms (1.73x), `sum-loop` 64 ms
(0.41x), `reduce-list` 29 ms (1.61x), `sum-squares` 22 ms (1.38x); only
`fib` moved (-7 %, not explained by this change).  These differ from the
table below, which was measured in June at an older commit. Measured under TLP's battery power profile (powersave, EPP balance_power): compare ratios, not absolute times, with older figures.  Details:
[protoCore's report](https://github.com/numaes/protoCore/blob/master/docs/reports/2026-10-12-cross-runtime-int-compare.md).

**On call-dispatch-bound recursion (`fib`, `tak`), protoClojure is 1.2-1.4× slower than Babashka 1.4.192. On `loop`/`recur` arithmetic, `reduce` over a list and `map` + `reduce` it is faster (0.36-0.75× Babashka's time). On LargeInteger workloads Babashka does not finish with default `*` (it does with `*'`).** The harness is [`benchmarks/bench.sh`](bench.sh): three runs per workload, best wall-clock time of three, each run a single process invocation including start-up. Babashka is GraalVM-native; both runtimes ran on the same machine.

| Workload | protoclj (ms) | bb (ms) | ratio | Notes |
|---|---:|---:|---:|---|
| `fib(30)` (pure recursion) | 712 | 524 | 1.36× | call-dispatch bound |
| `tak(18,12,6)` (Takeuchi) | 52 | 43 | 1.21× | call-dispatch bound |
| `sum-loop(1M)` (`loop`/`recur` arithmetic) | 79 | 219 | **0.36×** | tight SmallInteger loop |
| `reduce-list(10K)` (reduce + over list) | 27 | 36 | 0.75× | allocation + reduce |
| `sum-squares(1K)` (map + reduce) | 19 | 31 | 0.61× | start-up bound at this size |
| `factorial(100)` (158-digit result) | runs | **fails** with `*`; runs with `*'` | — | LargeInteger by default |

These are the most recent numbers in [`benchmarks/RESULTS.md`](RESULTS.md), measured after maps, strings and the concurrency primitives were added. Two earlier changes shaped the trajectory:

- **SmallInteger fast-path opcodes** (commit `700f352`): the VM short-circuits `(+ x y)`, `(< x y)` and the other arithmetic and comparison operators when both operands are tagged SmallIntegers, and routes everything else through protoCore's promoting `add` / `compare` / `multiply`. `fib(30)` went from 4113 ms to 720 ms.
- **Fewer attribute lookups per call** (commit `a099b45`): separate prototypes for single- and multi-arity function wrappers let the dispatcher pick the path with `getPrototype` alone, and the captures attribute is read only when a body has captures. `fib(30)` went to 620 ms, with 15% fewer instructions and 32% fewer L1-d cache misses (per `perf stat`).

The features added afterwards cost about 15% on `fib(30)` (620 ms → 712 ms); the extra parameters threaded through every recursive VM call are the likely cause, and recovering that cost is on the [roadmap](../docs/ROADMAP.md).

JVM Clojure was first measured on 2026-10-19 (the protoCore 2.14.7 paragraph above): 813-864 ms per program as a process, dominated by JVM start-up, and 3-16x faster than protoClojure once its JIT is warm inside one process.

### Showcase (README of 2026-10-24)

Measured on 2026-10-24 (5 interleaved rounds, medians, wall time and peak
RSS from `/usr/bin/time -v`, behind a quiet gate on a desktop with 2 busy
CPUs; protoclj `feature/macros` on protoCore 3.1.0, Babashka 1.4.192):

| script | protoclj | bb | protoclj RSS | bb RSS |
|---|---:|---:|---:|---:|
| start-up (empty file) | 24 ms | 11 ms | 23 MB | 17 MB |
| grep over a 115 MB log on stdin | 18.0 s | 2.1 s | 146 MB | 136 MB |
| JSON transform, 300,000 records (43 MB) | 5.2 s | 2.6 s | 1,967 MB | 339 MB |
| word frequencies, 31 MB of text | 19.2 s | 4.7 s | 159 MB | 144 MB |
| `(factorial 1000)` with `*'` | 30 ms | 10 ms | 24 MB | 20 MB |

Raw results and the runner: [`benchmarks/reports/2026-10-23-showcase/`](reports/2026-10-23-showcase/).

**Where bb wins, plainly:** start-up (about 2x), speed on these scripts
(2-9x: the grep's regular expressions run on `std::regex`, which is slow),
memory for large parsed data (protoCore's 64-byte cells: 6x on the JSON
document), and above all the ecosystem: bb has hundreds of libraries, pods,
tasks and Java classes; protoclj has what this README lists.

