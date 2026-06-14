# protoClojure vs Babashka — benchmarks

**Date.** 2026-06-14 (session 11).
**Setup.** DEV12 (AMD Ryzen 5500U Lucienne, 6 cores), Linux 6.17.0.
**Runtimes.**
- `protoclj` — protoClojure session-11 head, `build_release/` (Release).
- `bb` — Babashka 1.4.192 (GraalVM-native).
- (JVM Clojure not measured this round; binary not installed.)

**Methodology.** Each workload is a single-form `.clj` file. The harness
runs the script through each interpreter 3 times and reports the
minimum wall-clock (cold-start + execution). No JVM warmup phase.
Result strings are compared across runtimes for correctness.

## Session-12 numbers (after split fn prototypes + captures-only-if-needed)

| Workload         | protoclj (ms) | bb (ms) | ratio   | Result        | Match |
|------------------|--------------:|--------:|--------:|---------------|:-----:|
| `fib(30)`        |           620 |     496 | 1.25×   | 832040        | ✓ |
| `tak(18,12,6)`   |            45 |      42 | 1.07×   | 7             | ✓ |
| `sum-loop(1M)`   |            66 |     196 | **0.34×** | 500000500000 | ✓ |
| `reduce-list(10K)` |          29 |      33 | 0.88×   | 49995000      | ✓ |
| `sum-squares(1K)` |           19 |      28 | 0.68×   | 332833500     | ✓ |

### Session 10 → 11 → 12 trajectory on `fib(30)`

| Round | Wall (ms) | Cycles | Instructions | IPC  | L1-d misses |
|-------|----------:|-------:|-------------:|-----:|------------:|
| s10   |      4113 | (n/a)  |       (n/a)  | (n/a)|       (n/a) |
| s11   |       720 |  2.45G |        5.93G | 2.42 |       11.2M |
| s12   |       620 |  2.05G |        5.02G | 2.45 |        7.6M |

Per-call:
- s11: 720 ms / 2.7M calls = **267 ns/call**
- s12: 620 ms / 2.7M calls = **230 ns/call**  (−14%)
- bb:  498 ms / 2.7M calls = **184 ns/call**

We are now ~25% over Babashka per call. The residual ~46 ns/call sits in
frame setup (new ProtoContext, resizeAutomaticLocals, captures
seeding, recursive run() entry) — not in attribute lookups. The next
lever is either a ProtoContext pool (D — invasive) or threaded
dispatch in run(). IPC at 2.45 with 0.07% branch-miss rate suggests
threaded dispatch will return less than expected; the pool likely
more.

Reading the ratio: `<1` means protoClojure is faster than Babashka by
that factor. Three of five workloads now run **faster than Babashka**;
`fib` and `tak` sit at 1.18–1.45× behind. Extrapolating the
bb→JVM-Clojure ratio (Babashka usually trails JVM Clojure JIT'd
by ~2–3× on steady-state compute): protoClojure is in the
**~2–4× JVM Clojure** band on these workloads — comfortably inside
the **5× JVM** target set in the brainstorm phase.

## What changed since session 10

Session 10 ratios were 8.3× / 2.3× / 8.1× / 1.5× / 0.53× (slower than bb
across the board on compute-bound rows). Sessions 11 and 12 closed that
gap. Three sets of changes:

### Session 12 — fewer getAttribute calls per CALL

The user observed (correctly): protoCore already runs a 1024-entry
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

### Session 11 — SmallInt fast-path

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
   ↔ LargeInteger ↔ Float promotion is automatic. The previous
   round did a global-namespace lookup of the operator and a full
   `dispatchCall` — strictly slower for mixed-type input.

## LargeInteger correctness

`factorial(100)` runs through to completion:

```
2432902008176640000                                                ; 20!
51090942171709440000                                               ; 21!
30414093201713378043612608166064768844377641568960512000000000000  ; 50!
93326215443944152681699238856266700490715968264381621468592963895217599993229915608941463976156518286253697920827223758251185210916864000000000000000000000000  ; 100!
```

Babashka 1.4 fails on `factorial(21)` with `long overflow` because
JVM Clojure's default `*` is long-arithmetic; the user has to write
`*'` or `(bigint 1)` for explicit promotion. **protoClojure promotes
automatically** because the SmallInt fast-path falls through to
protoCore's `multiply` when the inline result no longer fits.

Three conformance fixtures cover this:
- `tests/conformance/15-bigint/factorial-21.clj` (51090942171709440000)
- `tests/conformance/15-bigint/factorial-30.clj` (30414093201713378043612608166064768844377641568960512000000000000)
- `tests/conformance/15-bigint/large-add.clj` ((2^63 − 1) × 2 = 18446744073709551614)

Plus `benchmarks/factorial-100.clj` for the demo.

## Caveats — still

1. **Cold-start + execution.** Each run is one process; bb pays
   ~30–50 ms of GraalVM startup. JVM Clojure would pay ~1.5–2 s
   on every invocation. A future pass should use a multi-iter
   inner loop to dilute startup.
2. **No JIT warmup.** JVM Clojure under JIT would be 2–3× faster
   than Babashka on steady-state compute-bound workloads.
3. **Workloads still small.** No allocation pressure on
   `fib` / `tak` / `sum-loop`. The volume is modest on the rest.

## Where the remaining headroom is

The gap on `fib` (1.45×) and `tak` (1.18×) is now dominated by
function-call dispatch, not arithmetic. The next levers:

1. **Inline cache at CALL sites.** Today every user-fn call does
   `getPrototype` + `getAttribute(bytecodeKey)` + `getAttribute(
   capturesKey)` (+ multi-arity scan). A single-slot cache keyed
   on the wrapper identity should cut that to one pointer compare
   on the hot path. Probably worth 20–30% on `fib`.

2. **Threaded dispatch (computed-goto) in `run()`.** Switch-based
   dispatch in `run()` has a known +10–15% from going threaded;
   protoST already documented this win. Smaller absolute win now
   that the binop opcodes dominate the dispatch budget.

3. **Specialised `inc` / `dec` opcodes.** Today `(inc x)` still
   goes through PUSH_VAR + CALL to the `prim_inc` primitive,
   which does the `argAsLong` + `+ 1` work in C++. An opcode-
   level `(inc x)` → `INC` opcode would inline it the same way
   `ADD` did. Smaller win, maybe useful on `sum-to-n`-style
   loops.

## Reproducibility

```bash
./benchmarks/bench.sh                  # uses /tmp/proto-bench/bb by default
./benchmarks/bench.sh path/to/protoclj path/to/bb
```
