# Seq cost and cons lists (2026-10-24)

Branch `perf/seq-cost-cons-lists`, three Release builds from clean worktrees,
all on protoCore master bb2d581a (3.2.0) in a scratch prefix (`ldd` in
`out/environment.txt`):

- **main**: `e1a4136`, before this work;
- **A**: `e2b1e53`, the VM answers the hot seq primitives without a call
  context (DESIGN.md M43);
- **B**: `563cf14`, lists as chains of immutable cells (M44) on top of A.

`run.sh` ran every section in 5 rounds, the variant order rotated each
round, each run on a fresh copy of its binary and of the library. The
machine was 82-87 % idle with no build running at every round
(`out/gate.txt`, load average 2.2-7.3 on 12 CPUs: a desktop session).
Babashka 1.12.200 for reference. Every runtime computed the same result in
every run of every case; the showcase outputs are byte-identical across
runtimes and runs.

## The list-cost cases

`list-cost.clj` is the 2026-10-24 harness of the brief, unchanged. ns per
element is the case's own timer around the work (median of 5); cycles per
element are `perf stat` cycles of the whole process (the collector thread
included) of a `work` run minus a `setup` run. At n = 10^6 the setup
dominates the process (main: 17.7 Gcycles to build the list) and the
collector's timing differs between the two runs, so the cycle difference is
noise there; the case timer is the figure to read at 10^6, the cycles at
10^4. Full tables for the three sizes: `out/summary.txt`.

n = 10^6, ns per element:

| case | main | A | B | bb | B vs main |
|---|---:|---:|---:|---:|---:|
| 1 list walk with `rest` | 1304 | 1181 | **288** | 175 | -78 % |
| 2 vector `(seq v)` walk | 848 | 567 | 608 | 252 | -28 % |
| 3 `reduce +` over a list | 161 | 117 | **85** | 24 | -47 % |
| 4 cons-build, then count | 993 | 1027 | **432** | 319 | -56 % |
| 5 sequential `nth` (vector) | 389 | 292 | 292 | 273 | -25 % |
| 6 random `nth` (vector) | 2141 | 2081 | 2003 | 783 | -6 % |
| 7 `empty?` + `first` (list) | 331 | 211 | 195 | 186 | -41 % |
| 8 `(count (rest xs))` (list) | 1401 | 1334 | **207** | 169 | -85 % |
| 9 `(count (cons i xs))` (list) | 1453 | 1397 | **342** | 249 | -76 % |

n = 10^4, cycles per element (work minus setup):

| case | main | A | B |
|---|---:|---:|---:|
| 1 list walk | 3529 | 2588 | 628 |
| 2 vector seq walk | 2491 | 1841 | 1072 |
| 3 reduce over a list | 769 | 455 | 233 |
| 4 cons-build, count | 2158 | 1506 | 890 |
| 7 empty? + first | 950 | 755 | 477 |
| 8 count of rest | 3834 | 3601 | 891 |

Building the 10^6 list of the setup (`(apply list (range n))`): main 17.7
Gcycles and 1540 MB peak RSS, B 8.5 Gcycles and 803 MB. Garbage per element
walked (cells reclaimed plus live at the end, per element, n = 10^6): list
walk 6.4 (main) against 0.4 (B); cons-build 15.9 against 2.0.

## Where the cycles of a primitive call went (Stage A)

`perf record` of `(if (empty? xs) 1 (first xs))` in a loop, main: the
dispatch loop 26 %, the `CALL` path 14 %, the protoCore context the call
built for the primitive 19 % (constructor 8 %, destructor 7 %, slot clearing
and the current-context switch 4 %), the global lookup of the name 7 %, the
primitives themselves 6 %, the list's `getAt` and `getSize` 5 %. A list
`rest` added a path copy: 14.8 cells per element. Stage A removes the
context for the primitives that read nil, lists, vectors, cons cells and
chunks without running code (M43); after it, the dispatch loop is about
half of a primitive call (switch dispatch, mostly indirect-branch cost) and
the global lookup 10 %. Stage B removes the path copy: `rest` of a list
cell is a field read, and reading a cell is two cache lines (the object
cell and its field list) plus the prototype's link for the type test.

## benchmarks/bench.sh, start-up, showcase

| bench.sh (ms, best of 3, median of 5) | main | A | B | bb |
|---|---:|---:|---:|---:|
| fib | 395 | 368 | 384 | 424 |
| tak | 39 | 39 | 39 | 26 |
| sum-loop | 72 | 71 | 71 | 164 |
| reduce-list | 31 | 32 | 28 | 17 |
| sum-squares | 27 | 27 | 27 | 16 |

Start-up, empty file (`perf stat -r 50`, median of 5): main 63.9 Mcycles
(17.6 ms task-clock), A 63.8 (17.0), **B 66.3 (17.8): +3.9 % cycles**.

Showcase (`examples/showcase`, wall s / peak RSS MB, median of 5):

| script | main | A | B | bb |
|---|---:|---:|---:|---:|
| start-up | 0.02 / 23 | 0.02 / 23 | 0.02 / 23 | 0.00 / 14 |
| factorial | 0.03 / 24 | 0.03 / 24 | 0.03 / 24 | 0.01 / 17 |
| grep | 15.53 / 148 | 15.64 / 148 | 15.55 / 148 | 1.71 / 133 |
| json | 5.09 / 1521 | 4.27 / 1522 | 4.44 / 1418 | 2.27 / 296 |
| wordfreq | 16.63 / 163 | 16.58 / 162 | 16.43 / 158 | 4.21 / 142 |

## What got slower, or did not move

- **Start-up +3.9 % cycles** (B against A; +0.2-0.8 ms task-clock): core.clj's
  lists are four protoCore cells per element instead of about one, and the
  forms macros build with `cons` are read through a seq walk. Profiled: the
  difference is spread over cell allocation (`allocCell`, the empty
  attribute list every lazy object gets) and page faults; not one hot spot.
- **`(vec list)` is O(n log n) where it was O(1)**: the vector's storage is a
  `ProtoList` and the list is not one any more. Measured separately: 1074 ms
  and 546 MB more for a list of 10^6 (main: 0 ms). protoCore has no bulk
  `ProtoList` constructor (`newList(n, items)` appends one by one).
- **Vector seq walk, B against A, +7 % at 10^6** (608 against 567 ns; +5 % at
  10^4, equal at 10^5): not isolated; the code it runs is the same apart
  from the order of the type tests.
- **Memory per list element**: four protoCore cells (256 bytes) for a list
  cell, a lazy object (object cell, parent link, empty attribute list)
  published with its one-cell field list. A ProtoList list was about one AVL
  node per element. `(into () (range 10^6))` peaks at 613 MB (B) against
  576 MB (main), because main's `conj` copied paths that the collector had
  to reclaim; a list that is kept costs more.
- Random `nth` on a vector (case 6) did not move: it is cache-bound on the
  AVL nodes of the vector's storage, 2.6x Babashka's time.
- The showcase scripts `grep` and `wordfreq` did not move: their time is
  spent elsewhere (9x and 4x Babashka's, not investigated here).

## The opportunity left (not taken)

protoCore has no one-cell immutable record (a prototype and N traced
slots). With one, a list cell, a cons and a chunk would be one cell instead
of four and a walk would touch one cache line per element instead of two
or three; that is the protoCore extension M44 asks for. Carrying a kind
marker in the field list, so the type test would not read the prototype's
link, was considered and not done: it duplicates what the prototype says.

## Rerunning

```sh
# Release builds of main, A and B from clean worktrees, against one protoCore
REPS=5 ./run.sh MAIN/protoclj A/protoclj B/protoclj /path/libprotoCore.so.4 /path/bb out /scratch/dir
python3 analyze.py out
```

`out/` holds the raw data: `cases.txt` (one line per run: the case's
result and timer, the collector's statistics, peak RSS, `perf stat`),
`bench_<variant>_r<round>.txt`, `startup_<variant>.txt`, `showcase/` (`/usr/bin/time -v`
per run, exit status and the digest of the output), `gate.txt` and
`environment.txt`; `summary.txt` is `analyze.py`'s output.
