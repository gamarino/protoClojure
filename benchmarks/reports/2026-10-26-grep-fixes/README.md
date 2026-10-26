# The grep fixes: PCRE2 and buffered line reading (2026-10-26)

Follow-up to the grep-gap investigation (branch `investigate/grep-gap`,
commit 3c99290, `benchmarks/reports/2026-10-25-grep-gap/README.md`, not
merged), which attributed the showcase grep's 16.7 s (Babashka: 1.9 s) to
`std::regex` (7.3 s), a per-line `UnmanagedScope` in `line-seq`, protoIO
erasing the front of its buffer for every line, and the `[n total]`
accumulator. This report measures what was fixed.

## What changed

| Item | Change | Where |
|---|---|---|
| protoIO line reading | The read buffer keeps an offset (`head`); compaction happens only when a read appends. New `readBufferedLine`, which answers only from the buffer, never reads and never waits (try-lock). protoIO 0.2.3, main 7920332. | protoIO |
| `line-seq` / `read-line` | `readLineOf` tries `readBufferedLine` first and enters `blocking()` only when a read is needed (DESIGN.md M49). Semantics unchanged. | `src/runtime/IO.cpp` |
| Regular expressions | PCRE2 with its JIT on every platform, replacing `std::regex` (DESIGN.md M48, D45 rewritten). | `src/runtime/Regex.{h,cpp}`, `Primitives.cpp` |
| `[n total]` accumulator | Measured, not changed (see below). | - |

## Showcase, 5 interleaved rounds

`showcase-ab.sh` (in this directory): every round is gated by
`gate.sh` (load log: `showcase-gate.log`), the order of the three runtimes
rotates from round to round, every run holds the build lock, and every
run's stdout must have the same SHA-256 as Babashka's (all 45 runs: `ok`).
Wall time and peak RSS from `/usr/bin/time`; medians, [min-max].

- before: `main` 74d7d87 with protoIO 1611ff9 (the binary the 2026-10-25
  README refresh measured), protoCore 3.3.0 from the agent prefix.
- after: branch `perf/grep-path` (PCRE2 10.46 from the system, protoIO
  7920332), same protoCore (verified with `ldd`).
- bb: Babashka 1.4.192.

| script | before | after | bb | after / bb |
|---|---:|---:|---:|---:|
| grep, 115 MB log on stdin | 18.89 s [16.49-23.59] | **7.93 s** [6.78-8.67] | 1.94 s | 4.1x |
| JSON transform, 43 MB | 4.81 s [3.77-5.13] | 4.78 s [4.09-5.89] | 2.67 s | 1.8x |
| word frequencies, 31 MB | 17.03 s [15.58-19.82] | **13.46 s** [13.13-16.42] | 4.64 s | 2.9x |

Peak RSS did not move (grep 139 -> 140 MB, word count 144 -> 142 MB, JSON
1,328 -> 1,277 MB, which varies by tens of percent between rounds). As in
the README, **RSS figures are indicative, not conclusive**: protoCore keeps
memory until it is needed and adapts its heap ceiling.

The machine was loaded (load average 3-8 during the rounds); the "before"
grep median is 18.9 s here against 16.7 s on 2026-10-25. The grep is 2.4x
faster and still 4.1x slower than bb. What remains was not profiled in
this run.

## bench.sh, 5 interleaved rounds (no regression)

`bench-rounds.sh` of `2026-10-25-readme-refresh`, after against before
(the script's "bb" column is the before binary here), best of 3 per round,
median of 5 rounds, every output checked (`bench-raw.tsv`, 0 failures):

| workload | before | after | delta |
|---|---:|---:|---:|
| fib | 418 ms | 416 ms | -0.5 % |
| tak | 39 ms | 38 ms | -1.8 % |
| sum-loop | 76 ms | 75 ms | -0.5 % |
| reduce-list | 28 ms | 28 ms | -1.8 % |
| sum-squares | 26 ms | 27 ms | +3.9 % (1 ms) |

## The `[n total]` accumulator: measured, not changed

`vec_reduce.clj` reduces `(range 1446319)` (the grep's line count) with a
scalar accumulator and with `(fn [[n total] i] [(inc n) (+ total i)])`:
about 450 ms against about 1,600 ms. A profile puts the vector form's cost
in allocation and collection (sweep about 30 %, AVL insertion, the vector's
two-cell box), not in call dispatch. Two direct-call fast paths were tried
in `fastPrimitiveCall` (M43): `nth` with a not-found argument (what
destructuring emits) and `vector` of at most five items built with one bulk
`newList`. `perf stat -r 5`, twice each: instructions -8.7 % (9.24 G ->
8.43 G), user cycles 7.9 / 7.6 G -> 7.3 / 7.5 G, wall time within noise
(1.73 / 1.86 s -> 1.87 / 1.61 s). Not measurable end to end, so both were
reverted. A real gain would need fewer cells per small vector, which is a
protoCore representation question, not a compiler one.

## protoPython's `re` (measured, not changed): a proposal

`protoPython/src/library/ReModule.cpp` matches with `std::wregex` (UTF-32;
a `char32_t` `std::basic_regex` on Windows), ECMAScript grammar (line 505):
the same engine family the grep investigation found slow. Measured with
`protoPython/build_release/src/runtime/protopy` (main 86fa6f35, its own
bundled protoCore), against CPython 3 on the same machine:

| loop | protopy | CPython |
|---|---:|---:|
| `re_mem.py`: 20,000 in-memory `search`es, `ERROR .* latency=(\d{4,})ms` | 47.5 us each | 0.5 us each |
| same, literal pattern `ERROR` | 31.6 us each | 0.4 us each |
| `grep_re.py`: 200,000 log lines from a file, search each | 146.9 s | 0.10 s |
| `lines_only.py`: the same file loop without `re` | 83.6 s | 0.055 s |
| `lines_only.py` on the first 50,000 lines | 3.0 s | - |

Two findings. (1) `re` costs 30-50 us per search on 70-byte lines, about
100x CPython, even for a literal; the same move to PCRE2 (its 32-bit
library, `pcre2-32`, matches UTF-32 strings directly) would apply. (2)
Independently of `re`, iterating a file by lines is superlinear: 50,000
lines take 3.0 s (60 us a line), 200,000 take 83.6 s (418 us a line). That
is worth an investigation of its own before any regex work, since it
dominates a grep-shaped script. Neither was changed here.

## Files

`showcase-ab.sh`, `showcase-summary.txt`, `showcase-gate.log`,
`bench-raw.tsv`, `bench-rounds.txt`, `vec_reduce.clj`, `grep_re.py`,
`re_mem.py`, `lines_only.py`. Data: `examples/showcase/gen-data.py`.
