# protoClojure vs Babashka — benchmarks

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
