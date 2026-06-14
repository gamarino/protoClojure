# protoClojure vs Babashka — exploratory baseline

**Date.** 2026-06-14 (session 10).
**Setup.** DEV12 (AMD Ryzen 5500U Lucienne, 6 cores), Linux 6.17.0.
**Runtimes.**
- `protoclj` — protoClojure session-9 head, `build_release/` (Release, single-thread build).
- `bb` — Babashka 1.4.192 (GraalVM-native).
- (JVM Clojure not measured this round; binary not installed.)

**Methodology.** Each workload is a single-form `.clj` file. The harness
runs the script through each interpreter 3 times and reports the
minimum wall-clock (cold-start + execution). No JVM warmup phase.
Result strings are compared across runtimes for correctness.

## Numbers

| Workload         | protoclj (ms) | bb (ms) | ratio       | Result        | Match |
|------------------|--------------:|--------:|------------:|---------------|:-----:|
| `fib(30)`        |          4113 |     495 | **8.3×**    | 832040        | ✓ |
| `tak(18,12,6)`   |           100 |      43 | **2.3×**    | 7             | ✓ |
| `sum-loop(1M)`   |          1634 |     201 | **8.1×**    | 500000500000  | ✓ |
| `reduce-list(10K)` |          47 |      32 | 1.5×        | 49995000      | ✓ |
| `sum-squares(1K)` |          18 |      34 | **0.53×**   | 332833500     | ✓ |

Reading the ratio column: a value `>1` means protoClojure took that
many times longer than Babashka. `0.53×` means protoClojure was
nearly twice as fast — a startup-cost artefact (bb's GraalVM cold-
start dominates the 18ms total).

## Caveats — for honesty

1. **Cold-start + execution.** Each run is one process invocation;
   Babashka pays ~30–50 ms of GraalVM startup. JVM Clojure (not
   measured here) would pay ~1.5–2.0 s on every invocation. A
   future pass should either (a) use a multi-iteration harness
   inside each script to dilute startup, or (b) measure with
   `time` after a one-iteration warmup.

2. **No JIT warmup.** JVM Clojure under JIT would be roughly
   2–3× faster than Babashka on steady-state compute-bound
   workloads. Extrapolating from the bb baseline: protoClojure
   would be ~20× slower than JVM Clojure on `fib` /
   `sum-loop` at steady state. Well off the **5× single-thread**
   target stated at the brainstorm phase.

3. **Workloads are small but representative.** No allocation
   pressure on `fib` / `tak` / `sum-loop`. `reduce-list` /
   `sum-squares` do allocate (lists from `cons`) but the volumes
   are modest.

## What the ratios point at

- **8.3× on `fib`, 8.1× on `sum-loop`** — both compute-bound.
  The bytecode VM has switch-based dispatch (no threaded /
  computed-goto), no inline cache at CALL sites, and goes
  through the `ProtoMethod` indirection on every arithmetic
  primitive. Each of those is a known 1.3-2× lever in similar
  interpreters.

- **2.3× on `tak`** — closer. `tak` is call-heavy with small
  bodies; the FUNCTION DISPATCH layer is in scope but the
  ARITHMETIC is amortised over the recursion. Suggests the
  primitive-arith path is the bigger cost.

- **`sum-squares` ≤ bb** — purely a startup artefact at this
  workload size; not informative about steady-state.

## Where the headroom is — leading candidates for session 11+

In rough order of expected impact:

1. **Threaded dispatch / computed-goto in `run()`.**
   protoST has documented +10–15% from this; the protoClojure VM
   is structurally similar. Single-day work, large win.

2. **Inline cache at CALL sites.** Today each CALL on a user fn
   does `getPrototype` + `getAttribute(bytecodeKey)` +
   `getAttribute(capturesKey)` (+ multi-arity scan). A single-slot
   cache keyed on the wrapper's identity should cut that to one
   pointer compare on the hot path. Pattern is known from protoJS.

3. **SmallInt fast-path for `+` / `-` / `*` / `<` etc.** Today
   `(+ 1 2)` goes through `prim_plus` which is a C++ function with
   args wrapped in a ProtoList plus tag checks. A bytecode-level
   shortcut: when both stack tops are tagged SmallInt, do the op
   inline. Likely halves `fib` / `sum-loop` cost.

4. **Reduce `MAKE_FN` allocations in tight call recursion.** Less
   important for these workloads (fib doesn't make new fns), but
   relevant for higher-order pipelines.

## Reproducibility

```bash
# from the repo root
./benchmarks/bench.sh

# or with explicit binaries
./benchmarks/bench.sh path/to/protoclj path/to/bb
```

The harness uses `date +%s.%N` for wall-clock; resolution is
nanoseconds but the practical noise floor is ~1 ms on this box.
The output table is what RESULTS.md above was filled from.
