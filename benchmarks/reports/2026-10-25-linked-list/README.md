# Lists as protoCore ProtoLinkedLists (2026-10-25)

Branch `feature/linked-list-lists` (DESIGN.md M47) against `main` 9df518a,
three Release builds on one protoCore library, v3.3.0 (`aedc3b63`), in a
scratch prefix (`ldd` and the library's digest in `out/environment.txt`):

- **main**: `main` 9df518a, lists as M44's lazy-object cells;
- **A**: a second copy of the main binary, an A/A control: every difference
  between main and A is noise;
- **B**: this branch, lists as `ProtoLinkedList`s.

`run.sh` is `../2026-10-24-seq-cost/run.sh` with case 10, `(vec list)`,
added to `list-cost.clj`. Five rounds, the variant order rotated each
round, every run on a fresh copy of its binary and of the library. Desktop
session closed; the machine was 72-79 % idle at every round but two
(47 % and 57 %, `out/gate.txt`). Babashka 1.4.192 for reference. Every
runtime computed the same result in every run of every case, and the
showcase outputs are identical across runtimes. Full tables:
`out/summary.md` (`analyze.py out`).

## Reading the numbers

The case timer (ns per element) is the figure to read. The cycle columns
subtract a setup run from a work run, both whole processes with the
collector thread included, and the A/A control shows they are noise at
these sizes (main against A differs by up to a factor of 5 in a case).

## The list cases, ns per element

| case | n = 10^5 main / A / B | n = 10^6 main / A / B | bb (10^6) |
|---|---:|---:|---:|
| 1 list walk with `rest` | 247 / 250 / **179** | 245 / 268 / **217** | 169 |
| 3 `reduce +` over a list | 78 / 73 / **19** | 89 / 50 / **21** | 27 |
| 4 cons-build, then count | 332 / 296 / 310 | 474 / 461 / 462 | 352 |
| 7 `empty?` + `first` | 203 / 208 / **176** | 225 / 216 / **177** | 183 |
| 8 `(count (rest xs))` | 224 / 212 / 195 | 238 / 239 / 239 | 168 |
| 9 `(count (cons i xs))` | 294 / 345 / 297 | 439 / 382 / 428 | 242 |
| 10 `(vec list)` | 515 / 572 / **389** | 1094 / 1130 / **768** | 18 |

Vector cases (2, 5, 6) are unchanged within the A/A spread. Building the
10^6 list of the setup (`(apply list (range n))`): 8.34 -> 7.34 Gcycles and
788 -> 562 MB peak RSS.

## n = 10^4: a heap block, not a cost per element

At n = 10^4, B reads 395 ns per element against 216 for the walk with
`rest` (case 1), 447 against 218 for case 8, and 751 against 491 for the
vector walk (case 2, which does not touch a list). Each of those runs takes
about 1,020 page faults more than its setup run, and none of main's does;
no collection cycle runs (`PROTOCORE_HEAP_TRACE=1`). B's start-up uses
about 1,000 pages fewer than main's (core.clj's lists are smaller), so the
4 MB heap block that the walk's garbage first needs is mapped inside the
timed window at n = 10^4. main pays the same block at n = 8,000, where the
vector walk reads 786-944 ns for main against 518-560 for B, and at
n = 5,000, 15,000, 20,000 and 50,000 the list walk of B is as fast as
main's or faster. A cost per process, at a size that depends on where the
heap's blocks fall.

A first run of this harness measured `(vec list)` 20-40 % slower in B: the
list was converted with `ProtoLinkedList::asList`, which builds through
`ProtoContext::newList(n, items)`, a path copy per element. B now reads the
list with a cursor into a `ListBuilder`, as main did (the table above).

## benchmarks/bench.sh, start-up, showcase

| workload (ms, best of 3, median of rounds) | main | A | B | bb |
|---|---:|---:|---:|---:|
| fib | 421 | 421 | 422 | 481 |
| tak | 44 | 42 | 40 | 31 |
| sum-loop | 78 | 78 | 80 | 176 |
| reduce-list | 32 | 32 | 31 | 21 |
| sum-squares | 31 | 32 | 30 | 19 |

Start-up, empty file: 67.2 / 66.9 / 65.0 Mcycles. Showcase scripts (wall
s, median): grep 18.26 / 17.78 / 17.84, json 4.60 / 4.68 / 4.54 (peak RSS
1371 / 1375 / 1223 MB), wordfreq 18.14 / 18.05 / 18.01; all within the A/A
spread except json's RSS.

## Reproducing

    run.sh MAIN_BIN A_BIN B_BIN LIBPROTOCORE BB [OUT] [SCRATCH]
    python3 analyze.py OUT
