# protoClojure

> **A Clojure dialect on the protoCore object kernel — fast startup, true parallelism, infinite-precision integers by default, transparent interop with Python and JavaScript.**

protoClojure is a Clojure-inspired language runtime built on the [protoCore](../protoCore) kernel. It is the fourth member of the protoCore language family — alongside [protoST](../protoST) (Smalltalk), [protoJS](../protoJS) (JavaScript) and [protoPython](../protoPython) (Python) — and it brings the **REPL-driven, immutability-first, data-as-substrate** stance of the Clojure community to a kernel that was already built around those three principles.

protoClojure is **not** a drop-in replacement for Clojure-JVM. There is no JVM, no Java interop, no Maven, no `clojure.java.*`. What it is, instead, is the Clojure idiom — `defn`, `let`, `loop`/`recur`, closures, persistent collections, higher-order functions, keywords, multi-arity — running on a runtime that **starts in milliseconds**, **uses real OS threads with no GIL**, and **promotes to LargeInteger automatically** when an arithmetic result no longer fits in a tagged word. The companion runtimes are reached through the same UMD module system protoST and protoPython already share, so `(:require [py/numpy :as np])` is the same kind of pointer hand-off, not a separate process.

## Why this exists

The Clojure community is small, technically demanding, and shares three convictions: *immutability as the default*, *data as the substrate*, *REPL-driven development*. protoCore was built on the same three principles without anyone setting out to host Clojure — they are the kernel's design stance. Putting them together is the natural step.

What protoClojure offers that JVM Clojure does not:

- **Native interop with Python and JavaScript modules**, through the same UMD plumbing protoPython and protoJS use. A `(:require [py/numpy :as np])` form pulls a NumPy module in and the result is a real protoCore object — no FFI marshalling, no copy at the boundary, no separate process. *(Module system shipping in a coming session; the foreign-dispatch spec is in `docs/superpowers/specs/2026-06-14-foreign-dispatch.md`.)*
- **Fast startup, small footprint.** No JVM warm-up: process start is milliseconds, not seconds. Scripts and CLI tools become a viable form factor without reaching for GraalVM AOT.
- **Infinite-precision integers by default.** When `(* acc n)` overflows a SmallInteger, protoCore's promoting `multiply` returns a LargeInteger and the program keeps going. `(factorial 100)` runs out of the box; Babashka 1.4 and JVM Clojure both fail on `(factorial 21)` unless the programmer writes `*'` or `(bigint 1)` explicitly.
- **Real parallelism without the GIL.** Every protoCore-hosted runtime shares the same GIL-free concurrency model. Atoms are a protoCore CAS, not a Clojure abstraction over a JVM primitive. *(Concurrent primitives exposed in a coming session — the kernel support is already there.)*

What protoClojure does **not** offer:

- **JVM interop.** No Java classes, no `clojure.java.io`, no Maven, no Leiningen. The substitute, where applicable, is calling the Python or JavaScript ecosystem through UMD.
- **100% Clojure-JVM compatibility.** This is a *dialect*. The reader, the core forms, and the standard library try to feel like Clojure; details intentionally do not match where the JVM-specific design would not earn its keep on this substrate.
- **`core.async` channels (yet).** The protoCore actor / future primitives give a different concurrency model that is closer in spirit to the Hickey designs. A CSP layer is a follow-up, not an immediate goal.

## A flavour of the language

A recursive `factorial` exercising loop/recur, conditional, multi-arity, and infinite-precision integers:

```clojure
(defn factorial
  ([n] (factorial n 1))
  ([n acc] (if (<= n 1) acc (recur (- n 1) (* acc n)))))

(println (factorial 20))   ;; => 2432902008176640000
(println (factorial 100))  ;; => 158-digit LargeInteger, no overflow
```

Closures and higher-order:

```clojure
(defn make-adder [n] (fn [x] (+ x n)))
(def add5 (make-adder 5))
(println (add5 10))   ;; => 15

(defn compose [f g] (fn [x] (f (g x))))
(println ((compose inc (fn [x] (* x x))) 6))   ;; => 37
```

Multi-arity, variadic, the higher-order pipeline:

```clojure
(defn show [a & rest] (println a rest))
(show 1 2 3 4)   ;; => 1 (2 3 4)

(println (reduce + (map (fn [x] (* x x)) [1 2 3 4 5])))   ;; => 55

(defn my-reduce
  ([f coll] (my-reduce f (first coll) (rest coll)))
  ([f acc coll]
   (if (empty? coll) acc (recur f (f acc (first coll)) (rest coll)))))

(println (my-reduce + 100 (list 1 2 3)))   ;; => 106
```

The 160 conformance fixtures under `tests/conformance/` cover everything the language supports today.

## Concurrency — atoms, futures, actors

protoClojure exposes the GIL-free concurrency that protoCore already has underneath. There is no global interpreter lock; threads are real OS threads, tracked by the kernel's GC quorum.

```clojure
;; --- atoms: CAS-backed shared state ------------------------------------
(def counter (atom 0))
(swap! counter inc)               ;; lock-free retry on protoCore CAS
@counter                          ;; => 1
(add-watch counter :log
  (fn [k r old new] (println k old "->" new)))

;; --- futures: a real OS thread per future, value materialises on @ -----
(def f (future (slow-compute)))
@f                                ;; blocks (goUnmanaged) until done

;; --- pmap: fan out over the worker pool, gather in input order ---------
(pmap slow-compute [1 2 3 4 5])

;; --- promises: hand off a value across threads -------------------------
(def p (promise))
(future (deliver p 42))
@p                                ;; => 42

;; --- actors: serialised mailbox, three priority bands ------------------
(def acc (actor 0))
(send acc inc)                    ;; medium-priority enqueue
(send-h acc + 100)                ;; high-priority enqueue
(send-l acc - 5)                  ;; low-priority enqueue
@(send acc inc)                   ;; the send returns a promise; deref waits
@acc                              ;; current value, no message round-trip
```

Actors run on a configurable worker pool (`PROTOCLJ_ACTOR_WORKERS`, default `max(2, cores − 2)`, cap 16). The kernel enforces a **single-method invariant**: at most one message per actor is being processed at any instant, so the function body sees no concurrent access to the actor's state. Three priority bands (`send-h` / `send` / `send-l`) drain highest-priority-non-empty first.

On a 2026-06-14 measurement (Ryzen 5500U), the upper bound on a single actor with a trivial `inc` body is **~5M msg/s** — the cost is the send path, the mailbox enqueue, and the promise completion. Adding workers does not raise the single-actor rate (the invariant forbids parallelism within one actor); under fan-out across 1000 actors the throughput is **~250K msg/s** with the same trivial body. The full numbers are in [`benchmarks/actor-bench.sh`](benchmarks/actor-bench.sh).

## Performance — what is measured

**On compute-bound recursion, protoClojure is within ~25% of Babashka 1.4. On tight `loop`/`recur` arithmetic it is ~3× faster than Babashka. On LargeInteger workloads Babashka does not finish.** Numbers below were taken with Babashka 1.4.192 (GraalVM-native) on the same hardware. The harness is `benchmarks/bench.sh` — three runs per workload, wall-clock best-of-three, single invocation including cold start.

| Workload | protoclj (ms) | bb (ms) | ratio | Notes |
|---|---:|---:|---:|---|
| `fib(30)` (pure recursion) | 620 | 496 | 1.25× | call-dispatch bound |
| `tak(18,12,6)` (Takeuchi) | 45 | 42 | 1.07× | call-dispatch bound |
| `sum-loop(1M)` (`loop`/`recur` arithmetic) | 66 | 196 | **0.34×** | tight SmallInt loop |
| `reduce-list(10K)` (reduce + over list) | 29 | 33 | 0.88× | allocation + reduce |
| `sum-squares(1K)` (map + reduce) | 19 | 28 | 0.68× | startup-bound at this size |
| `factorial(100)` (158-digit result) | runs | **fails** | — | LargeInteger by default |

The single biggest win in the perf trajectory was **SmallInt fast-path opcodes** (session 11): the VM short-circuits `(+ x y)` / `(< x y)` etc. when both operands are tagged SmallInt, and routes everything else through protoCore's promoting `add` / `compare` / `multiply`. That collapsed `fib(30)` from 4113 ms (session 10) to 720 ms.

The follow-up (session 12) was **fewer attribute lookups per CALL** — splitting the function-marker prototype into `fnSingleProto` / `fnMultiProto` so the dispatcher picks the path via `getPrototype` alone, and skipping the captures attribute read when the body has no captures. That brought `fib(30)` to 620 ms with **15% fewer instructions and 32% fewer L1-d misses** (per `perf stat`).

### How we read this against the original "5× JVM Clojure" goal

Babashka typically trails JVM Clojure JIT'd by ~2-3× on steady-state compute. Extrapolating the table above, protoClojure is in the **~2-4× JVM-Clojure band** on these workloads — comfortably **inside** the "5× JVM Clojure single-thread" target stated at design time. **We are not faster than JVM Clojure.** JVM Clojure has a JIT, decades of tuning, and adaptive inlining; on the same hardware it would beat protoClojure on every row of the table. What protoClojure has, instead, is `factorial(100)` working without thought, a 5 ms cold start, and a kernel small enough to read and modify in a single session — see the discussion under "How protoClojure compares" below.

The benchmark sources are in [`benchmarks/`](benchmarks/); the dated report is in [`benchmarks/RESULTS.md`](benchmarks/RESULTS.md).

## How protoClojure compares

Three comparators matter — JVM Clojure, Babashka, and the rest of the family — and each is honest in a different way.

### vs. JVM Clojure (the canonical Clojure)

JVM Clojure is the language, the ecosystem, and the JIT-compiled runtime the rest of the Clojure world means when they say "Clojure". It is fast, it is mature, and it is the right tool when you need the JVM platform (Java libraries, the existing Clojure library ecosystem, persistent collections tuned over a decade, `core.async`, transducers).

protoClojure does not try to replace it. The places where protoClojure is **architecturally different**, not just an alternative implementation, are:

- **Cold-start**: 5 ms vs ~1.5-2 s.
- **Footprint**: a few MB of compiled native code + protoCore vs a JVM and its libraries.
- **Numeric default**: LargeInteger automatic vs `long`-arithmetic with explicit `*'` for promotion.
- **No JVM**: no `java.*`, no Java reflection. The Python and JavaScript companion ecosystems instead, through UMD.

The places where JVM Clojure wins on substance, not just legacy:

- **Mature persistent-collection performance** — HAMT, RRB-tree, transient batching, well-tuned hash. protoClojure uses protoCore's `ProtoList` / `ProtoTuple` shapes, which are competent but not yet at the level of `clojure.lang.PersistentHashMap`.
- **JIT**. On any inner-loop workload that benefits from method-call inlining, the JVM goes through code paths protoClojure does not have.
- **Ecosystem**. Two decades of libraries, Datomic, Lein/Deps, well-known editors with deep CIDER/Calva integration. protoClojure has none of that today.

### vs. Babashka (Clojure-on-GraalVM-native)

Babashka is the closest comparator and the one the bench section measures. Babashka shares two of protoClojure's appeals — fast startup, low footprint — by AOT-compiling a Clojure subset through GraalVM native-image. The differences:

- **BigInt**: Babashka 1.4 fails on `(factorial 21)` (long overflow); protoClojure promotes automatically.
- **Concurrency**: Babashka runs Clojure code through the same native-compiled subset; protoCore-class GC, atoms, futures are not exposed in the same way. *(protoClojure's exposure is a coming session.)*
- **Mutation of bytecode at the REPL**: Babashka's native-image is a closed image; redefinitions go through a Clojure-side interpreter shim. protoClojure compiles fresh on every form at the REPL because the compiler is the same binary.
- **Tradeoff cost**: Babashka takes the Clojure-JVM `*` semantics literally, which means `long`-arithmetic and overflow. protoClojure takes the simpler-but-distinct path of always-promoting arithmetic, paying the SmallInt-tag check on every op and the LargeInteger allocation on overflow.

### vs. protoST / protoJS / protoPython (siblings on protoCore)

protoClojure is the *immutable-by-default, REPL-first* face of the protoCore family. It shares the kernel — `ProtoObject`, the GC, the immutable collections, the GIL-free concurrency, the per-thread attribute cache — but expresses them through Clojure idiom: a Lisp reader, persistent vectors written `[x y z]`, keywords as values, `defn` / `fn` / `let` / `loop` / `recur` as special forms, multi-arity via the same wrapper machinery as protoST's method dispatch.

The interesting property that this membership confers: a value materialised in any of the four runtimes is a real `ProtoObject`. **A Python list reaches protoClojure's `(count py-list)` through the same chain walk every protoClojure call does**, with no marshalling. Where Babashka would need its own bridges, protoClojure inherits them from the kernel.

## Project status

protoClojure runs. Nineteen development sessions have landed (each numbered, dated, and traced in the commit log on `main`):

| Session | What it shipped | Conformance count |
|---:|---|---:|
| 1-3 | Lexer, reader, bytecode VM, `println` | 32 |
| 4 | `def`, `if`, `do`, integer arithmetic, comparisons | 42 |
| 5 | `fn`, `defn`, `let`, `loop`, `recur` | 48 |
| 6 | Closures with N-level lexical capture | 54 |
| 7 | Variadic `& rest`, `apply`, `map` / `filter` / `reduce` | 66 |
| 8 | Multi-arity `defn`, `cond` / `when` / `and` / `or`, keywords | 80 |
| 9 | IEEE-754 floats, vectors distinct from lists | 90 |
| 10 | First benchmark vs Babashka | 90 |
| 11 | SmallInt fast-path opcodes + LargeInteger promotion | 93 |
| 12 | Fewer attribute lookups per CALL | 93 |
| 13 | Maps `{:a 1}` + named-arg destructuring `& {:keys [...]}` | 108 |
| 14 | Trailing kv pairs + `:or` + `:as` — dual call convention closed | 117 |
| 15 | `clojure.string`-shaped surface (15 string primitives) | 132 |
| 16 | `atom`, `swap!`, `reset!`, `deref`, `@` — CAS on protoCore | 138 |
| 17 | `future` on real OS threads (4× wall-clock on parallel workload) | 144 |
| 18 | `add-watch`/`remove-watch`, `promise`/`deliver`, parallel `pmap`, named anon fn | 152 |
| 19 | Actor system: worker pool, 3 priority bands, ~5M msg/s upper bound | 160 |

The suite stands at **160 conformance fixtures + the unit tests** (`ctest`, single-threaded). The benchmark numbers above are reproduced by `./benchmarks/bench.sh` on the same build; the actor throughput numbers by `./benchmarks/actor-bench.sh`.

What is implemented and stable:

- Lexer: integers, floats (`3.14`, `1e6`), strings, symbols, keywords (`:foo`), vectors (`[x y z]`), lists (`(...)`), comments (`;;`).
- Special forms: `def`, `defn` (single + multi-arity), `fn`, `let`, `loop`, `recur` (in both `loop` and the implicit fn-body recur target), `if`, `do`, `quote`, `apply`, `when`, `when-not`, `cond`, `and`, `or`.
- Literals: `true`, `false`, `nil`, integers (SmallInt + LargeInteger), floats, strings, keywords, vectors, lists.
- Closures: full N-level capture cascade, including chained closures across `(fn ... (fn ... (fn ...)))`.
- Primitives: `+ - * / inc dec < <= > >= = str println list vector vec nth vector? list? first rest cons count empty? nil? not reverse map filter reduce apply`.
- VM: 28 opcodes including SmallInt fast-path binary opcodes (`ADD SUB MUL LT LE GT GE EQ`), arity dispatch (`MAKE_FN` / `MAKE_FN_MULTI`), CALL APPLY for variadic dispatch, DUP and JUMP_IF_TRUE for short-circuit forms.
- Numeric semantics: SmallInt for tagged-pointer-fit values, automatic promotion to LargeInteger on overflow, automatic promotion to double when any operand is float.

What is **not yet** implemented (and tracked):

- **Clojure-style `agent`** with its specific send/await semantics (the in-tree `actor` is the protoCore-native variant — different surface, see [docs/tutorial/13-actors.md](docs/tutorial/13-actors.md)).
- **`delay` / `force`** and `volatile!` / `vreset!` / `vswap!`.
- **The UMD module system**: `(:require [py/numpy :as np])` and the foreign-dispatch protocol layer.
- **A real REPL** with nREPL compatibility (CIDER, Calva, Conjure).
- **A `core.clj`** evaluated at startup so we stop installing primitives in C++ and start composing them in Clojure.
- **JIT or threaded-dispatch**. The current bytecode VM is a clean switch loop and runs surprisingly well on a modern CPU; both are upgrade paths, neither is queued yet.

The live tracker is `docs/STATUS.md`; the roadmap is `docs/ROADMAP.md`. The full design specs (the protoCore call-convention adoption note, the foreign-dispatch protocol, the engineering principles) live under `docs/superpowers/specs/`.

## Getting started

protoClojure depends on [protoCore](../protoCore), which must be built first.

```bash
cd protoClojure
cmake -B build_release -S .
cmake --build build_release

./build_release/protoclj script.clj          # run a .clj file
./build_release/protoclj --version           # version

cd build_release && ctest -j1                # run the conformance suite (160/160)
cd .. && ./benchmarks/bench.sh               # run the benchmark vs Babashka
./benchmarks/actor-bench.sh                  # actor throughput, varied worker counts
```

The benchmark harness expects a Babashka binary at `/tmp/proto-bench/bb` by default; pass the path as the second argument to override:

```bash
./benchmarks/bench.sh ./build_release/protoclj /usr/local/bin/bb
```

The single-threaded build constraint reflects a local DEV12-specific issue (see `MEMORY.md`); concurrent build is supported by the CMakeLists.

### Packaging

protoClojure ships installable packages on Linux, macOS, and Windows via CPack — the same machinery protoST uses. The platform-appropriate generator is selected automatically; you pick which artifact you want with `cpack -G`. All artifacts contain the `protoclj` binary, the documentation, the example `.clj` scripts under `share/protoClojure/examples`, and the benchmark scripts under `share/protoClojure/benchmarks`.

```bash
# (After a successful build of protoCore and protoClojure.)
cd build_release

# --- Linux ---------------------------------------------------------------
cpack -G TGZ                # protoclojure-0.0.1-Linux.tar.gz (universal)
cpack -G DEB                # protoclojure-0.0.1-Linux.deb   (Debian / Ubuntu)
cpack -G RPM                # protoclojure-0.0.1-Linux.rpm   (Fedora / RHEL)

# --- macOS ---------------------------------------------------------------
cpack -G DragNDrop          # protoclojure-0.0.1-Darwin.dmg

# --- Windows -------------------------------------------------------------
cpack -G NSIS               # protoclojure-0.0.1-win64.exe   (NSIS installer)
cpack -G ZIP                # protoclojure-0.0.1-win64.zip   (portable)
```

The DEB and RPM artifacts declare `protocore` as a runtime dependency, so the package manager will fail cleanly if libprotoCore is not installed. The TGZ / DMG / NSIS / ZIP artifacts do **not** carry libprotoCore — install it from its own package first, or build it side-by-side and add its install prefix to your loader path. The installed `protoclj` already has an `INSTALL_RPATH` of `$ORIGIN/../lib` (Linux) / `@executable_path/../lib` (macOS), so as long as protoCore lives under the same `<prefix>/lib` no environment variable is needed.

```bash
# Quick sanity check after installing the DEB:
sudo dpkg -i protoclojure-0.0.1-Linux.deb
protoclj --version
protoclj /usr/share/protoClojure/examples/02-factorial.clj
```

The macOS and Windows generators are configured but **unverified on a Linux build host** — they will work where `cpack` recognises the platform; the in-tree CI to produce them on real Apple/Windows runners is a future-session item.

## Documentation

| Document | What it covers |
|---|---|
| [docs/LANGUAGE.md](docs/LANGUAGE.md) | The language reference — reader, evaluator, special forms, primitives. |
| [docs/STATUS.md](docs/STATUS.md) | Live status — what works, what departs from Clojure-JVM, what is on the list. |
| [docs/ROADMAP.md](docs/ROADMAP.md) | The session-by-session roadmap and how to contribute. |
| [docs/INTEROP.md](docs/INTEROP.md) | The UMD interop story — pulling a Python or JavaScript module into a protoClojure namespace. |
| [docs/DESIGN.md](docs/DESIGN.md) | The architectural overview — reader, compiler, BytecodeModule, VM, primitives. |
| [docs/TUTORIAL.md](docs/TUTORIAL.md) | Index to the dual-audience tutorial under `docs/tutorial/`. |
| [docs/superpowers/specs/](docs/superpowers/specs/) | Dated design specifications — engineering principles, phase plans, foreign dispatch, call convention. |

The conformance fixtures themselves are the de facto reference for what the implementation accepts — `tests/conformance/**/*.clj`.

## Related projects

- **[protoCore](../protoCore)** — the prototype-based kernel: object model, GC, immutable collections (`ProtoList` / `ProtoTuple` / `ProtoString` / `ProtoSparseList`), GIL-free concurrency, the per-thread `AttributeCache` and `MutableValueCache` that protoClojure's CALL handler leans on instead of re-implementing.
- **[protoST](../protoST)** — Smalltalk runtime on protoCore, with the first-class actor model. The blueprint for the concurrency-primitive exposure protoClojure will follow.
- **[protoPython](../protoPython)** — Python 3.14 runtime on protoCore. Source of the bytecode-format pattern that protoClojure's `ExecutionEngine` adopts.
- **[protoJS](../protoJS)** — JavaScript runtime on protoCore. Source of the `JSSymbols`-style stable interned-key pattern that protoClojure uses for its `__bytecode__` / `__captures__` / `__arities__` markers.

## Why "protoClojure"?

`proto` — built on protoCore. `Clojure` — the language idiom we're modelling. The name signals what it is and where it lives in the family. We avoid "Clojure-on-protoCore" because the goal is not a port of Clojure-JVM; it's a Clojure-flavoured dialect that earns its place by what protoCore can do.

## The Swarm of One

**This is what changed.** protoClojure went from zero to a working Clojure dialect rivalling Babashka on compute-bound workloads in roughly twelve focused sessions — half of them in a single afternoon. That is not the result of a particularly fast typist. It is the result of a **single architect, paired with a swarm of specialised AI agents, treating each session as a hypothesis-test loop**: design → emit → compile → measure with `perf stat` → keep or discard → commit.

The cost of "let's see if it works" collapsed. In session 12 we read protoCore's `AttributeCache` implementation, understood the hash, traced the mutable-snapshot path, redesigned the wrapper schema to ask for fewer lookups, measured the result with `perf stat`, validated the 15% instruction-count drop, and committed — in under an hour. Without the swarm, the same hour buys you the *reading* of the cache. The build, the redesign, the measurement, the commit live on a different week.

This matters past protoClojure. The criterion for "is this worth building" was, until recently, set by the *cost of building* — runtime engineering was a 2-year project for a team. With the swarm, a single architect can produce a runtime that **runs, benchmarks, and improves under its own measurements**, with traceable per-session commits and a conformance suite that grew with the implementation. The "interesting but not practical" projects move into the "implementable in an afternoon" column.

protoClojure is the proof. The point is the pattern.

## License

protoClojure is released under the [MIT License](LICENSE) — the same licence as protoCore, protoST, protoJS, and protoPython.
