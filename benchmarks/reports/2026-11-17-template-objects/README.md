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
