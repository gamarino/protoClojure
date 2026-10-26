# Why grep is 8.6x slower than Babashka (2026-10-25)

An investigation, not a fix. The showcase `examples/showcase/grep.clj` took
16.7 s under protoclj against 1.93 s under bb on the 115 MB access log
(1,446,319 lines) in the
[README refresh](../2026-10-25-readme-refresh/). This report breaks those
seconds down by stage, profiles them, and tests each hypothesis with a
prototype or a micro-measurement. The prototypes live on this branch,
behind CMake options that are OFF by default; nothing here is meant to be
merged as it stands.

## Summary

The gap is explained almost entirely by four costs, none of which is the
interpreter's dispatch loop:

| Cause | Where it lives | Cost in grep | Evidence |
|---|---|---:|---|
| libstdc++ `std::regex`, breadth-first executor | protoClojure (`Regex.cpp`) | **~7.3 s** | engine alone: 7.2 s vs PCRE2 0.12 s; prototype: -7.3 s |
| `ProtoString::toStdString` decodes and re-encodes every code point | protoCore | **~2.0 s** | 1.4 µs per 79-byte line (micro); top symbol in every regex profile |
| Line input: one `UnmanagedScope` per line + `erase(0, n)` of a 64 KiB buffer per line | protoIO + protoCore + `IO.cpp` | **~2.6 s** | prototype: `a_lines` 4.7 s → 2.1 s |
| `[n total]` accumulator: 3 calls + destructuring + ~5.4 cells per step | protoClojure compiler | **~2.4 s** | `c_vector` - `c3_scalar_count` = 2.7 s (bb: 0.33 s) |
| Rest of the per-line floor: string build, cons + lazy step, the reduce call | protoCore + protoClojure | ~1.3 s | `a2_reduce` 2.5 s (prototype build) vs bb 1.2 s |

The first three prototypes together, measured end to end:
**17.6 s → 7.3 s** (3-run median; 5-run median 8.0 s on a noisier
machine), against bb's 1.9 s. Removing the `toStdString` cost (a protoCore
change, estimated, not measured) would bring it to about 5.3 s, about 2.8x
bb. The vector accumulator is the largest cost left after that.

What this rules out:

- **No tuple interning.** Vectors are `ProtoList`s in a sparse-list box
  (`VectorOps.cpp`). protoClojure's sources use no `ProtoTuple` at all, so the
  `[n total]` accumulator interns nothing. The tuple-interner hypothesis is
  false.
- **Attribute lookup is not visible.** `getOwnAttributeDirect` is about 1 %
  in every profile.
- **The pattern's shape is not the cause.** The pure literal `#"ERROR"`
  costs as much as the full pattern (15.9 s vs 17.8 s). The time goes into
  libstdc++'s per-start-position executor setup, not into backtracking.
- **The GC is not the bottleneck.** A run does 9 to 16 cycles. Sweeping runs
  on the collector thread (1 to 11 % of samples, depending on the stage).
- **Not startup.** Both runtimes start in about 10 ms.

## Method

- Input: `access.log` from `gen-data.py` (reused from
  `.agent_scratch/clj-bench-1025/data`), 115,343,436 bytes, 1,446,319 lines,
  43,593 `ERROR` lines, 1,588 matches.
- protoclj: a Release build of `74d7d87` against protoCore 3.3.0 (in a
  private prefix, checked with `ldd`). bb: 1.4.192.
- Every script prints the work it did. `run-stages.sh` compares each output
  with the expected value (`ok` / `MISMATCH`). Every run in `data/` is `ok`.
- Every timing and profiling run holds the shared build lock, and every
  timing round runs after the idle gate (`data/gate.log`). The desktop
  session keeps the load average at 6 to 9. The last round started at load 18,
  and its fifth repetition ran at load 31 (`data/full-r2.txt`, last
  line of each block), so medians are reported and that round is treated
  as the noisiest.
- Profiles: `perf record` at 999 to 1999 Hz (`perf_event_paranoid` = 1:
  user-space symbols resolve, kernel symbols do not). protoCore is built
  without frame pointers and DWARF unwinding gave no usable stacks, so
  the profiles are flat. Callers were inferred from the code and confirmed
  with the prototypes.

## Stage breakdown

Medians in ms (3 runs each; `data/stages-r1.txt`, `data/variants-r1.txt`,
`data/vec-r1.txt`). "Gap" is protoclj minus bb.

| Stage | Script | protoclj | bb | Gap |
|---|---|---:|---:|---:|
| a. `line-seq` + `count` | `a_lines` | 5,101 | 1,307 | 3,794 |
| a2. + `reduce` with a scalar, one fn call per line | `a2_reduce` | 5,984 | 1,280 | 4,704 |
| b. + `re-find` (full pattern), scalar accumulator | `b_refind` | 17,780 | 1,752 | 16,028 |
| b2. + `re-find` with the literal `#"ERROR"` | `b2_literal` | 15,892 | 1,667 | 14,225 |
| b3. `str/includes?` prefilter, then `re-find` | `b3_includes` | 9,302 | 1,494 | 7,808 |
| c. `[n total]` accumulator, no regex | `c_vector` | 7,012 | 1,684 | 5,328 |
| d. `parse-long` on a constant per line | `d_parselong` | 5,871 | 1,395 | 4,476 |
| e. full grep, `loop`/`recur`, two locals | `e_loop` | 16,710 | 2,521 | 14,189 |
| f. full grep (`grep.clj`) | `f_full` | 18,031 | 2,074 | 15,957 |

Reading the table:

- **Lines (a):** 3.5 µs per line against bb's 0.9 µs before any user code runs.
- **Regex (b - a2):** +11.8 s for protoclj, +0.5 s for bb. That is 8.2 µs
  per call, for 1.45 million calls.
- **Prefilter (b3):** `str/includes?` alone costs 3.3 s, 2.3 µs per call,
  for what is a `std::string::find`. The time goes into the two
  `toStdString` conversions it makes (see below). The prefilter still
  halves the regex stage, because only 43,593 lines reach `std::regex`.
- **`parse-long` (d - a2):** nothing.
- **`loop`/`recur` (e) vs the vector `reduce` (f):** -1.3 s. This is the
  vector accumulator's cost in the full script.

## Prototypes, end to end

Builds on this branch, all against the same protoCore 3.3.0 prefix:

- `base`: `74d7d87` unchanged, with protoIO 0.2.2 as shipped.
- `head`: protoIO with an offset into the read buffer instead of
  `buf.erase(0, pos + 1)` for every line (`protoio-buffered-line.patch`).
- `io`: `head`, plus `line-seq` taking a line already in protoIO's buffer
  without `blocking()`, so without an `UnmanagedScope`.
  `-DPROTOCLJ_BUFFERED_LINE_PROTOTYPE=ON` with protoIO's prototype
  `readBufferedLine`.
- `pcre`: `head`, plus `re-find` through PCRE2 10.x with its JIT.
  `-DPROTOCLJ_PCRE2_PROTOTYPE=ON`.
- `both`: `io` + `pcre`.

Medians in ms (3 runs, `data/variants-r1.txt`; the last row is 5 runs from
the noisier `data/full-r2.txt`):

| Stage | base | head | io | pcre | both | bb |
|---|---:|---:|---:|---:|---:|---:|
| `a_lines` | 4,682 | 3,736 | 2,264 | 3,597 | 2,106 | 1,066 |
| `a2_reduce` | 5,044 | 4,543 | 2,492 | 3,738 | 2,583 | 1,160 |
| `b_refind` | 17,933 | 15,404 | 11,134 | 7,214 | 5,090 | 1,689 |
| `b3_includes` | 8,884 | 7,755 | 5,688 | 7,189 | 5,635 | 1,290 |
| `c_vector` | 6,933 | 5,927 | 4,983 | 6,396 | 4,724 | 1,665 |
| `e_loop` | 15,880 | 15,619 | 11,300 | 7,652 | 5,052 | 2,478 |
| `f_full` | 17,636 | 16,061 | 13,004 | 8,720 | 7,271 | 1,901 |
| `f_full` (5 runs) | 16,813 | 16,469 | 13,363 | 10,458 | 7,981 | 1,880 |

The prototype build `both` passes 872 of the 873 ctest tests. The one
failure, `conformance/35-regex/long-subject.clj`, is the PCRE2 prototype's.
On a 300,000-character subject, `(a|b)*` exhausts PCRE2's default 32 KiB
JIT stack (`rc = -46`, "JIT stack limit reached"), and the prototype answers
nil for every negative return code. With a 64 MiB JIT stack assigned
(`pcre2_jit_stack_assign`) it matches (checked separately). A real
integration has to assign one and raise `StackOverflowError` for the
match-limit errors, as the std::regex path does.

## Root causes, with evidence

### 1. libstdc++ `std::regex` (protoClojure; about 7.3 s, 47 % of the gap)

`Regex.cpp` compiles every pattern as an ECMAScript `std::regex`. With
libstdc++ it adds the `__polynomial` flag (the breadth-first executor),
chosen so that matching never recurses on the native stack (D45). For every
start position, `regex_search` sets up an executor, allocating and clearing
its state vectors. Most grep lines do not match, so the search tries every
position in the line. Hence the cost: ~90 ns per subject byte, whatever the
pattern.

`micro/regex_engines.cpp`, the same 1,446,319 lines and the same pattern,
outside any runtime:

| Engine | ms | ns/line |
|---|---:|---:|
| `std::regex` `__polynomial` (what protoclj uses) | 7,200 | 4,978 |
| `std::regex` `__polynomial`, literal `ERROR` | 5,895 | 4,076 |
| `std::regex` backtracking (default executor) | 2,347 | 1,623 |
| `memmem("ERROR ")` prefilter + `__polynomial` | 584 | 404 |
| PCRE2 interpreter | 125 | 86 |
| PCRE2 JIT | 76 | 53 |

Profile of `b_refind` on `base` (`data/perf-b_refind.txt`): libstdc++'s
regex `_Executor` frames add up to 19.8 % of the samples, and
malloc/free/new/memset, mostly the executor's per-position vectors, to
another 14.9 %. The PCRE2 prototype takes `f_full` from 16.1 s to 8.7 s. In the `both`
build, `pcre2_match_8` is 0.6 % of `b_refind`'s samples.

Java's `java.util.regex`, which bb uses, scans for a literal prefix with
Boyer-Moore before it runs the matcher. That is why bb's regex stage costs
0.5 s.

**Affects other runtimes:** protoPython's `re` module (`ReModule.cpp`) is
also built on `std::regex`, over `char32_t`. It very likely pays the same
cost or more. This was not measured here.

### 2. `ProtoString::toStdString` (protoCore; about 2.0 s, 13 %)

Every string primitive (`re-find`, `includes?`, `starts-with?`,
`upper-case`, ...) starts with `asProtoString(v)->toStdString(ctx)`. In
protoCore 3.3.0, `toUTF8String` walks the rope with a
`RopeCharacterIterator`, decodes one code point at a time, and re-encodes
each one with `appendUTF8CodePoint`. Yet the leaves already hold UTF-8
bytes.

`micro/protocore_line_ops.cpp`, linking the same protoCore, over the same
lines:

| Operation | ns/line (79 bytes average) |
|---|---:|
| `fromUTF8Buffer` (building the line string) | 486-533 |
| `toStdString` | **1,368-1,582** (about 17 ns per byte) |
| empty `UnmanagedScope` | 312-338 |

A per-leaf `memcpy` would cost roughly what copying a `std::string` costs,
tens of ns per line. In the `both` build, `toUTF8String` +
`RopeCharacterIterator::next` + `isString` are 34 % of `b_refind`'s samples
and 20 % of `f_full`'s (`data/perf-both-*.txt`). They are the largest item
left. In `b3_includes`, the two conversions per `includes?` call explain
most of the 2.3 µs per call.

**Affects every runtime.** protoPython has about 726 `toStdString` /
`toUTF8String` call sites, protoJS 327, protoST 56 and protoScala 23.
Any string-heavy workload in any runtime pays about 17 ns per byte per
conversion.

### 3. Line input (protoIO, protoCore, `IO.cpp`; about 2.6 s, 17 %)

`line-seq` calls `nextLine` once per line, and `nextLine` goes through
`blocking(ctx, [&] { return protoio::readLine(fd); })`. Each call costs two
things:

1. **`buf.erase(0, pos + 1)` per line** (`protoio::detail::takeLine`). After
   each 64 KiB `fill`, every line moves the rest of the buffer to the front,
   about 32 KiB on average. memmove is 15.5 % of `a_lines` on `base`.
   Prototype `head` (an offset, compacted only before the next `fill`):
   `a_lines` 4.7 s → 3.7 s.
2. **One `UnmanagedScope` per line**, although a 64 KiB buffer holds about
   800 lines and needs one `read(2)`. Leaving and rejoining the quorum
   (`multispace::goOut` / `comeBack`) takes `ProtoSpace::globalMutex`, a
   recursive mutex, twice. It also calls `gcCV.notify_all()` every time,
   which wakes the idle collector thread with a futex syscall. On `base`,
   40 % of `a_lines`'s samples are in the kernel: 23 % on the
   `protocore-gc` thread, which is woken once per line, and 17 % on the
   mutator. An empty `UnmanagedScope` costs 330 ns in isolation. Prototype
   `io` (take a line already buffered without `blocking()`): `a_lines`
   3.7 s → 2.3 s.

**Affects other runtimes:** protoST's `readLine` (`io_prims.cpp`) uses the
same `blocking()` + `protoio::readLine` pattern, so it pays both costs per
line. The `notify_all` on every `UnmanagedScope` entry is protoCore-wide:
any runtime that wraps short native calls in a scope pays it.

### 4. The `[n total]` accumulator (protoClojure; about 2.4 s, 15 %)

`(fn [[n total] line] [(inc n) (+ total ...)])` compiles to five `CALL`s
through `PUSH_VAR`: `nth`, `nth`, `inc`, `count` and `vector` (`--disasm`).
Each step also allocates about 5.4 more cells: 23.4 M cells reclaimed for
`c_vector` against 15.6 M for `a2_reduce` (`PROTOCLJ_GC_STATS=1`), and the
collector runs 16 to 19 cycles instead of 9 or 10. Measured costs:

- In context: `c_vector` - `c3_scalar_count` = 2.7 s (1.9 µs per step); bb
  pays 0.33 s.
- Without I/O, through `reduce` over `(range N)`
  (`micro/vec_reduce.clj`): 1.3 s; bb pays 0.3 s.
- Through `loop` with an inline `let` destructuring
  (`micro/vec_loop.clj`): 0.7 s.
- One primitive call (`inc` → `(+ n 1)`, `c2_vector_noinc`): about 175 ns.

The step costs more in context (1.9 µs) than in isolation (0.9 µs). The
likely cause is the collector, which runs twice as often while lines are
also being allocated. This is not proven.

### 5. The per-line floor that remains

In the `both` build, `a2_reduce` takes 2.5 s against bb's 1.2 s. Its parts:
building each line's string (`fromUTF8Buffer` copies the bytes twice
through `std::vector`s and builds 3 leaves and 2 internal nodes, about
0.5 µs, or 0.7 s), the cons and the lazy step, and the `reduce` call
(about 290 ns per call against bb's 100 ns). A run allocates about 10.8
cells per line.

## Per-line accounting, `grep.clj` on `base`

| Item | s |
|---|---:|
| `std::regex` | 7.3 |
| Line input (erase + `UnmanagedScope`) | 2.6 |
| `toStdString` in `re-find` | 2.0 |
| Vector accumulator | 2.4 |
| Rest of the floor (string build, cons, call) | 1.3 |
| **Explained gap** | **15.6** |
| Measured gap (17.6 - 1.9) | 15.7 |

The items overlap a little: the GC work is shared, and the 5.5 % of
`toUTF8String` samples in `a_lines` has no caller identified.
The total is a sum of separately measured deltas, not a single controlled
experiment.

## Proposals, ranked by measured or estimated gain

| # | Proposal | Belongs in | Gain on grep | Status |
|---|---|---|---:|---|
| 1 | Replace `std::regex` with PCRE2. Its syntax is much closer to Java's than ECMAScript is: possessive quantifiers, lookbehind, `\A`/`\z`, inline flags, named groups. Assign a large JIT stack, map PCRE2's limit errors to `StackOverflowError`, and keep the translation layer only where Java and PCRE2 differ. This adds a dependency, packaged on every target (vcpkg/Homebrew for Windows/macOS). | protoClojure | **-7.3 s** measured (16.1 → 8.7 s) | prototype; 1 conformance failure, cause known |
| 1b | If no new dependency is acceptable: a required-literal prefilter (`memmem` for the pattern's literal prefix before `regex_search`), as Java does. It only helps patterns with a literal prefix. | protoClojure | about -6.6 s (engine micro: 7.2 → 0.58 s) | estimated |
| 2 | `toUTF8String`: copy each leaf's bytes (`memcpy`) instead of decoding and re-encoding per code point. Optionally add a zero-copy leaf-span visitor, so primitives can search without building a `std::string`. | **protoCore** (proposal only) | about -2.0 s (1.4 µs → ~0.05 µs per line) | estimated from the micro; also helps every runtime |
| 3 | protoIO: an offset into the read buffer instead of `erase(0, n)` per line. | protoIO | **-0.9 s** measured (`a_lines` 4.7 → 3.7 s) | prototype (`protoio-buffered-line.patch`) |
| 4 | Take a line that is already buffered without an `UnmanagedScope`: a non-blocking `readBufferedLine` in protoIO, used by `line-seq`/`read-line` in protoClojure and the same way in protoST. | protoIO + protoClojure (+ protoST) | **-1.5 s** measured (`a_lines` 3.7 → 2.3 s) | prototype |
| 4b | Make `UnmanagedScope` cheap: only `notify_all` when a collector is waiting for the quorum (`stwFlag` / a waiting count), and a fast path that skips `globalMutex` when only one space is registered. | **protoCore** (proposal only) | ≤ 0.5 s on grep (330 ns × lines) once #4 is in; more for any runtime that uses short scopes | estimated |
| 5 | Compile the accumulator: an opcode for vector literals, and `nth` with a constant index (`NTH_CONST`) for sequential destructuring. Also consider `inc`/`dec` as opcodes, as `+` already is. | protoClojure | about -1 to -1.5 s (isolated cost 1.3 s; in context 2.7 s) | estimated |
| 6 | `fromUTF8Buffer`: build the leaves straight from the caller's buffer when there are no pending bytes, instead of copying it twice through `std::vector`. | **protoCore** (proposal only) | about -0.3 s | estimated |
| 7 | Already measured, for the user: write accumulators with `loop`/`recur`. | documentation | -1.3 s (e vs f on `base`) | measured |

With #1, #3 and #4 (measured): 7.3 s. Adding #2 (estimated): about 5.3 s.
Adding #5: about 4 s, roughly 2x bb. The remaining floor is the per-line
string build, the lazy cons, and calls at about 290 ns.

## Files

- `stages/*.clj`: the stage scripts. Each prints its work.
- `run-stages.sh`: the runner. It verifies every output and times each run
  inside the build lock.
- `median.py`: the medians of runner output files.
- `micro/regex_engines.cpp`, `micro/protocore_line_ops.cpp`,
  `micro/vec_loop.clj`, `micro/vec_reduce.clj`: the micro-measurements. Build
  commands are in each header.
- `protoio-buffered-line.patch`: the protoIO prototype, against protoIO
  `1611ff9` (0.2.2).
- `data/`: the raw runs, the gate log and the flat perf summaries.

The prototype code on this branch is in `CMakeLists.txt`
(`PROTOCLJ_PCRE2_PROTOTYPE`, `PROTOCLJ_BUFFERED_LINE_PROTOTYPE`, both OFF),
`Regex.h/.cpp`, the 2-argument `re-find` in `Primitives.cpp`, and `nextLine`
in `IO.cpp`. The buffered-line option does not build against an unpatched
protoIO.
