# The macros-branch regression: causes and fixes (2026-10-24)

The previous report ([`../2026-10-24-final-vs-baseline/`](../2026-10-24-final-vs-baseline/README.md))
found every `bench.sh` workload slower on `feature/macros` than on the
baseline before stage 1 (fib +7.6 %, tak +14.3 %, sum-loop +10.1 %,
reduce-list +21.4 %, sum-squares +25.0 %, map-loop +10.8 %, start-up +45 %),
with the causes not isolated. This report separates start-up from run time,
bisects both across the stage commits, fixes what could be fixed without
giving up a feature, and explains the rest.

Method, everywhere below: Release builds from clean detached worktrees
(`git status` empty at each build), protoCore 3.1.0 (`feature/lazy-object`
34eca79f) unless noted; every measured run loads a **fresh copy** of its
binary and of `libprotoCore.so.4` (`ldd` in `data/*/ldd.txt`); variants
interleaved, order rotated each round; the quiet gate (`quiet_gate.py`,
`QUIET_CORES=4`) before every variant-round; `perf stat` cycles and
instructions; every program's output checked against its expected value
(a run that does not verify is not a data point). AMD Ryzen 5 5500U, GCC
15.2.

**Run phase** = the long variant of a workload (`long/`: fib 32,
tak 24 16 8, a 10 M-step loop, 300 k- and 100 k-element lists, 2 M map
operations) minus an empty file, medians. The long variants make the run
phase 15-60x the start-up, so the two are separated.

## Final numbers

`run.sh` (`data/final/`), 5 rounds. A = baseline 8b7938a on protoCore
3.0.0, B = the same binary on 3.1.0, C = `feature/macros` before the fixes
(3b75e41), D = with the fixes (eb274bb).

| | B baseline | C before | D after | C vs B | D vs B |
|---|---:|---:|---:|---:|---:|
| start-up, Mcycles (Minstr) | 47.3 (29.4) | 67.9 (68.5) | 64.1 (53.2) | +43.6 % | **+35.7 %** |
| fib, run phase Mcycles | 3049 | 3224 | 3184 | +5.7 % | **+4.4 %** |
| tak | 1705 | 1777 | 1734 | +4.2 % | **+1.7 %** |
| sum-loop | 1471 | 1492 | 1489 | +1.4 % | **+1.2 %** |
| reduce-list | 1661 | 1649 | 1666 | -0.7 % | **+0.3 %** |
| sum-squares | 878 | 1199 | 987 | +36.5 % | **+12.3 %** |
| map-loop | 3553 | 3862 | 3714 | +8.7 % | **+4.5 %** |

`bench.sh` (ms, start-up included, best of 3, median of 5 rounds): fib
336 / 354 / 348, tak 33 / 39 / 38, sum-loop 63 / 69 / 67, reduce-list
27 / 33 / 31, sum-squares 22 / 29 / 27 (B / C / D). These workloads run
20-60 ms of which about 13 ms is start-up, so they mostly measure start-up.
A and B agree within noise: the kernel change is neutral.

## Causes, by bisect

`data/bisect/`: the 16 commits that change code, 3 rounds each (the
`summary.txt` cycle columns are run phase, the instruction columns whole
process).

### Start-up: core.clj, +12 M cycles at stage 6, +21 M at the end

- **e5a48c5 (stage 6, core.clj)**: 48.1 -> 60.0 Mcycles, 30 -> 55 M
  instructions. Later stages grow it (2aa03e1, the Part C vocabulary: +5 M);
  by 3b75e41, 69.3 Mcycles.
- Where it goes (frame-pointer builds of protoClojure and protoCore, 40
  start-ups, samples per run): baseline 180, before the fix 234. The
  difference is `loadCore` (48 samples per run, about 2.4 ms) and
  `installPrimitives` (+3.8). `ProtoSpace::ProtoSpace` is 102 samples per
  run on both sides, the dynamic linker 35: the same on both.
- Inside `loadCore`: reading 34 %, compiling and running the forms 66 %.
  The top self costs were `ProtoString::toUTF8String`, the rope character
  iterator, `memmove` and `createSymbol`'s symbol-table probes, called from
  `internNamed` (reader, syntax-quote), `globalMacro`,
  `BytecodeModule::addSymbol` and the compiler's `nameOf`: the same
  spellings converted between protoCore strings and text over and over.

**Fix (ac1ec76)**: per-thread caches, keyed by the run's `NamedLayout`
(a new `epoch` field, so a process that builds several spaces never reads
another run's values), for `internNamed`, `internSymbol` and
`namedSpellingText`; `addSymbol` looks in its module's index before
interning. Start-up 67.9 -> 64.1 Mcycles, 68.5 -> 53.2 M instructions.

**Not fixed, and why.** What remains, +17 M cycles, is reading and
compiling 81 top-level forms. The brief's other candidates:

- Built-ins registered lazily, and `clojure.set` / JSON / regex loaded on
  first use: all of `installPrimitives` costs 3.8 samples per run more than
  the baseline (about 0.75 M cycles). Not worth a lookup hook on every
  global miss.
- Precompiling or snapshotting `core.clj` at build time, or compiling its
  definitions on first use: the only remaining large saving (up to the
  whole `loadCore`, about 10 M cycles), but it needs a serialised form of
  bytecode modules and their constant pools (quoted data, named values,
  macro functions), or a resolution hook at every global miss, macro lookup
  and syntax-quote resolution, with a thread-safe once-per-definition load
  that parks for the collector. A design decision, left for the maintainer
  (STATUS.md).

### sum-squares: lazy map over a list (8d72602, stage 11)

+38.7 % run phase at 8d72602, +35 % at the end; instructions +35 %. `map`
became lazy (as in Clojure). Each step took the rest of its source list with
`removeFirst`, which rebuilds part of the list's tree: O(log n) allocations
per element, 13 % of the run.

**Fix (ac1ec76)**: the lazy steps (map, filter, take, take-while, cycle,
concat, interleave, for, distinct) read a list through an unchunked view of
it one element on (`walkMore`), as `SeqWalk::remaining` already did; and
they root their step in their own scope's slot instead of opening one more
context per element (`lazy5At`). +36.5 % -> +12.3 %.

**The rest is the feature**: per element one lazy object, one cons, a
realisation context and the call of `f`, where the eager `map` appended to
one list. A list is not chunked in Clojure either; chunking it would change
when `f` runs.

### map-loop: sets and seqs as map keys (a5729b7, stage 8)

+12.6 % at a5729b7 (instructions +7 %), +6.6 % after ed3cf5a, +11.7 % at
the end. Two parts:

- **The collector's first cycle.** map-loop allocates about 2.03 M cells;
  the adaptive heap's initial soft limit is 2 M (2,097,152) cells. The
  baseline ends at 2,031,616 and never collects; the branch, with core.clj's
  cells, reaches the limit and runs one cycle (`PROTOCLJ_GC_STATS=1`:
  cycles=0 against cycles=1), whose sweep is about 340 M instructions. With
  `PROTOCORE_ADAPTIVE_HEAP_START=budget` neither collects, and the branch is
  +5.3 % instructions, +3.2 % cycles. Any program near that size moves its
  first collection earlier by core.clj's allocation; over a longer run the
  number of cycles is the same within one.
- **The key tests sets and seqs need**: `get`, `contains?` and keyword
  calls asked "is it a set?" before "is it a map?". **Fix (ac1ec76)**: map
  first, `isSet` inline: -79 M instructions on map-loop. The same reorder in
  the dispatch loop's `callLookup` saved nothing and cost fib +2.5 % and tak
  +2-6 % cycles at equal instructions (layout); reverted (eb274bb).

What remains without the collector, +2 % cycles: frame-context destruction
(`~ProtoContext`, about +15 % of its own count) and the argument-slot
clearing of stage 11; not isolated further.

### fib, tak, sum-loop: layout, mostly

Run-phase instructions: fib +0.8 %, tak +2.0 %, sum-loop +0.8 %. Cycles
move more than instructions, and in both directions, between commits that
do not touch the paths these programs run:

- fib at commits with the same instruction count (9,231-9,235 M, stages
  6-10): -0.9 % to +8.3 % cycles; 2aa03e1 +12.4 % and 69405c5 +3.6 % with
  identical instructions (9,192 M).
- a529207 (stage 12) adds 90-128 M instructions to executeFrame on
  sum-loop and fib: its only change there is the NUL test of a string
  constant in `materialise`. Moving that test out of line restores the
  instruction count (sum-loop 6,346 M, fib 9,181 M, below the baseline) but
  costs fib +2.6 % and tak +3 % cycles over four rounds: kept as it is.
- Disabling locals clearing (PUSH_LOCAL_CLEAR and the argument-slot
  clearing, stage 11, an experiment build) moves tak -5 % and fib +4 %
  cycles.
- tak's extra instructions include globals lookups that miss protoCore's
  attribute cache more often (`implGetAt` +58 %): the globals object holds
  more names (core.clj's definitions, stored under both `x` and
  `clojure.core/x`, and the new built-ins), and the cache is direct-mapped
  by address.

So fib's +4.4 % and tak's +1.7 % are the dispatch loop's code layout on
top of about 1-2 % more instructions; the layout part is **not isolated**
to a change, and local edits to the loop move it either way, as the two
reverted experiments above show.

## Reproduce

```
ROUNDS=5 QUIET_GATE=./quiet_gate.py ./run.sh A_BIN A_LIB(3.0.0) B_LIB(3.1.0) C_BIN D_BIN BB OUT
```

`data/bisect/measure.sh` and `summ.py` produced the bisect table (paths in
them point at the session's scratch tree).
