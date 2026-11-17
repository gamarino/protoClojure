# protoClojure Template Objects Benchmark Report (2026-11-17)

Measurements after adopting template-based instantiation (`__template__`, `newClone`, `setAttributes`) on protoCore 3.6.0.

## Context

Following the template pattern introduced in protoCore 3.6.0 and protoScala, protoClojure adopts immutable pre-configured prototypes with shared `ProtoShape` structures and inline attributes for its most heavily allocated object types:

1. **Functions and Closures** (`MAKE_FN`, `MAKE_MULTI_FN`): `fnSingleTemplate`, `fnClosureTemplate`, `fnMultiTemplate`.
2. **Sequence Nodes** (`cons`, `chunk`): `consTemplate` (2 slots inline, $d=0$) and `chunkTemplate` (4 slots inline, $d=0$), moving sequence nodes from 3 cells (192 bytes) down to 1 cell (64 bytes).
3. **Keywords and Symbols** (`named.templateObj`): 1 cell inline, sharing `ProtoShape` across all symbols and keywords for 100% attribute cache hits.
4. **Persistent Sets** (`g_setTemplate`): 1 cell inline per version.
5. **Exceptions & Regex**: Template-based instantiation via `setAttributes`.

## Workload Measurements (5 interleaved rounds vs Babashka 1.4.192)

Harness: `bench-rounds.sh` (best of 3 per round; median [min-max] across 5 rounds):

| Workload | protoclj (ms) | bb (ms) | ratio (medians) | protoclj / bb per round | Notes |
|---|---:|---:|---:|---|---|
| `fib(30)` | 526 [518-532] | 507 [494-517] | **1.04×** | 1.04, 1.04, 1.03, 1.07, 1.02 | pure recursion |
| `tak(18,12,6)` | 42 [40-45] | 30 [29-39] | 1.40× | 1.31, 1.48, 1.08, 1.41, 1.43 | short recursion |
| `sum-loop(1M)` | 88 [85-92] | 192 [185-199] | **0.46×** | 0.45, 0.48, 0.48, 0.47, 0.43 | loop/recur arithmetic (protoclj >2x faster) |
| `reduce-list(10K)` | 32 [30-34] | 23 [20-28] | 1.40× | 1.44, 1.66, 1.18, 1.53, 1.34 | reduce over list |
| `sum-squares(1K)` | 33 [30-35] | 20 [18-20] | 1.68× | 1.61, 1.55, 1.73, 1.85, 1.76 | lazy map + reduce |
| `factorial(100)` | 27.5 ms | fails (`long overflow`) | — | — | 158 digits; LargeInteger by default |

Runs checked: 150, failed: 0. All 886 ctest unit and conformance tests pass.

## Comparison Across Load States & Pre-Template Baseline

| Workload | Pre-templates (load ~3.8) | 5 Rounds (load ~5.0) | Quiet Run (load ~2.6) | Babashka (load ~3.8) | Babashka (load ~5.0) |
|---|:---:|:---:|:---:|:---:|:---:|
| `fib(30)` | 436 ms (0.99×) | 526 ms (1.04×) | 477 ms (1.06×) | 440 ms | 507 ms (+15%) |
| `tak` | 39 ms (1.39×) | 42 ms (1.40×) | 40 ms (1.38×) | 28 ms | 30 ms (+7%) |
| `sum-loop` | 79 ms (0.47×) | 88 ms (0.46×) | 83 ms (0.47×) | 169 ms | 192 ms (+14%) |
| `reduce-list` | 29 ms (1.45×) | 32 ms (1.40×) | 28 ms (1.47×) | 20 ms | 23 ms (+15%) |
| `sum-squares` | 28 ms (1.75×) | 33 ms (1.68×) | 26 ms (1.44×) | 16 ms | 20 ms (+25%) |

## Key Findings & Conclusions

1. **Ambient System Load vs Wall-Clock Times:**
   - Absolute milliseconds fluctuate with system activity on daily-driver hardware (load averages varied between 2.6 and 5.0).
   - Babashka (the exact same GraalVM native binary) saw a 14%–25% increase in wall-clock time between runs due to thermal throttling and background tasks.
   - Consequently, relative ratios to Babashka under interleaved executions remain the stable, representative metric.

2. **Dramatically Reduced Allocation & GC Pressure:**
   - **Sequence Nodes (`cons` / `chunk`):** Reduced from 3 cells (192 bytes) to **1 cell (64 bytes)** with inline attributes ($d=0$), saving **67% in allocation overhead**.
   - **Functions & Closures:** Dropped from 4–6 cells to **1 cell (64 bytes)** (−75% to −83%).
   - In adaptive heap testing, garbage volume per iteration dropped so substantially that test rounds had to be scaled from 20,000 to 50,000 to allocate enough cells to exercise GC cycle triggers.

3. **Performance Trajectory:**
   - `sum-loop`: Holds a strong lead (>2x faster than Babashka, 0.46x ratio) thanks to unboxed SmallInteger operations and compact loop execution.
   - `sum-squares` and `reduce-list`: Benefit from single-cell lazy chunk and cons nodes, improving or maintaining their relative ratios against Babashka.
   - `fib(30)`: Call dispatch over 2.69 million recursive invocations remains effectively competitive with GraalVM native-image code (1.04x).
   - `factorial(100)`: Evaluates in ~27 ms to 158 exact digits without special types or annotations, while Babashka throws `ArithmeticException: long overflow` on default `*`.
