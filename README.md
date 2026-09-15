# protoClojure

> **A Clojure dialect on the protoCore object kernel — fast startup, GIL-free threads, arbitrary-precision integers by default, and a design for transparent interop with Python and JavaScript.**

protoClojure is a Clojure-inspired language runtime built on the [protoCore](https://github.com/numaes/protoCore) kernel. It is one of the language runtimes of the protoCore family — alongside [protoST](https://github.com/gamarino/protoST) (Smalltalk-inspired), [protoJS](https://github.com/gamarino/protoJS) (JavaScript) and [protoPython](https://github.com/gamarino/protoPython) (Python) — and it brings the **REPL-driven, immutability-first, data-as-substrate** stance of the Clojure community to a kernel that was already built around those three principles.

protoClojure is **not** a drop-in replacement for Clojure-JVM. There is no JVM, no Java interop, no Maven, no `clojure.java.*`. What it offers instead is the Clojure idiom — `defn`, `let`, `loop`/`recur`, closures, persistent collections, higher-order functions, keywords, multi-arity — running on a runtime that **starts in milliseconds**, **uses real OS threads with no GIL**, and **promotes to LargeInteger automatically** when an arithmetic result no longer fits in a tagged word. The design reaches the companion runtimes through the same UMD module system protoST and protoPython share, so that `(:require [py/numpy :as np])` becomes a pointer hand-off rather than a separate process; that module system is not implemented yet.

protoClojure is at an early stage: version 0.0.1, with no tagged release.

## Why this exists

The Clojure community is small, technically demanding, and shares three convictions: *immutability as the default*, *data as the substrate*, *REPL-driven development*. protoCore was built on the same three principles without anyone setting out to host Clojure — they are the kernel's design stance. Putting them together is the natural step.

What protoClojure offers that JVM Clojure does not:

- **Native interop with Python and JavaScript modules (planned).** The design routes a `(:require [py/numpy :as np])` form through the same UMD plumbing protoPython and protoJS use, so the result is a real protoCore object — no FFI marshalling, no copy at the boundary, no separate process. The module system is not implemented yet; see [docs/INTEROP.md](docs/INTEROP.md) and the archived [foreign-dispatch design](docs/archive/design-specs/2026-06-14-foreign-dispatch.md).
- **Fast startup, small footprint.** No JVM warm-up: on the benchmark machine (AMD Ryzen 5 5500U), `build_release/protoclj examples/01-hello.clj` has a median wall time of 21.6 ms over 50 runs and a peak resident set size of about 20 MB (`/usr/bin/time -f %M`), measured on 2026-09-15. Scripts and CLI tools are a viable form factor without GraalVM AOT.
- **Arbitrary-precision integers by default.** When `(* acc n)` overflows a SmallInteger, protoCore's promoting `multiply` returns a LargeInteger and the program keeps going. `(factorial 100)` runs out of the box; Babashka 1.4 fails on `(factorial 21)` because Clojure's default `*` uses long arithmetic, which requires `*'` or `(bigint 1)` for promotion.
- **Real parallelism without a GIL.** Every protoCore-hosted runtime shares the same GIL-free concurrency model. Atoms are a protoCore compare-and-set; futures run on OS threads; actors run on a worker pool.

What protoClojure does **not** offer:

- **JVM interop.** No Java classes, no `clojure.java.io`, no Maven, no Leiningen. The intended substitute, where applicable, is calling the Python or JavaScript ecosystem through UMD.
- **100% Clojure-JVM compatibility.** This is a *dialect*. The reader, the core forms, and the standard library try to feel like Clojure; details intentionally do not match where the JVM-specific design would not earn its keep on this substrate.
- **`core.async` channels.** The protoCore actor and future primitives give a different concurrency model. A CSP layer is a possible follow-up, not an immediate goal.

## A flavour of the language

A recursive `factorial` exercising `recur`, conditionals, multi-arity, and arbitrary-precision integers:

```clojure
(defn factorial
  ([n] (factorial n 1))
  ([n acc] (if (<= n 1) acc (recur (- n 1) (* acc n)))))

(println (factorial 20))   ;; => 2432902008176640000
(println (factorial 100))  ;; => 158-digit LargeInteger, no overflow
```

Closures and higher-order functions:

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

The 262 conformance fixtures under `tests/conformance/` are the executable reference for what the language accepts today.

## Concurrency — atoms, futures, actors

protoClojure exposes the GIL-free concurrency that protoCore already has underneath. There is no global interpreter lock; threads are real OS threads, tracked by the kernel's garbage collector.

```clojure
;; --- atoms: CAS-backed shared state ------------------------------------
(def counter (atom 0))
(swap! counter inc)               ;; lock-free retry on protoCore CAS
@counter                          ;; => 1
(add-watch counter :log
  (fn [k r old new] (println k old "->" new)))

;; --- futures: one OS thread per future, value materialises on @ --------
(def f (future (slow-compute)))
@f                                ;; blocks until the future completes

;; --- pmap: one OS thread per element, results in input order -----------
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

Actors run on a configurable worker pool (`PROTOCLJ_ACTOR_WORKERS`, default `max(2, cores − 2)`, cap 16). The scheduler enforces a **single-method invariant**: at most one message per actor is being processed at any instant, so the function body sees no concurrent access to the actor's state. Three priority bands (`send-h` / `send` / `send-l`) drain highest-priority-non-empty first.

The per-actor mailbox is **lock-free** (three atomic-pointer MPSC stacks plus one `claimed` flag), following protoST's mailbox design. Senders never take a per-actor mutex; the running worker drains each stack with one atomic exchange and a reversal, then processes the batch.

#### Benchmark — actors

Numbers below were measured on 2026-06-14 on an AMD Ryzen 5 5500U (6 cores, 12 threads). Each row is **1,000,000 messages** with the trivial body `(inc v)`; runs take 4-10 seconds. The runner is [`benchmarks/actor-bench.sh`](benchmarks/actor-bench.sh), which verifies that every script reports the expected message count before computing a rate.

| mode      | what it measures                                              | peak msg/s |
|-----------|---------------------------------------------------------------|-----------:|
| `single`  | 1 sender × 1 actor — per-actor pipeline floor                 |   215,100  |
| `fan-out` | 1 sender × 1000 actors (1000 msgs each) — ready queue stress  |   218,341  |
| `MPSC`    | 4 senders × 1 actor — per-actor sender contention             |   171,851  |
| `MPMC`    | 4 senders × 4 actors (round-robin) — both contention paths    |   125,424  |

These are upper bounds for a single-operation message body. Worker-count scaling: **`fan-out` peaks at `PROTOCLJ_ACTOR_WORKERS=6`** (the physical-core count) and degrades at 8 and 16 workers, when the extra workers land on SMT siblings.

Compared with the earlier mailbox (per-actor `std::mutex` + `std::deque`), the lock-free mailbox measured **+4-17% (single)**, **+30-41% (fan-out at 2-4 workers)**, **+11-19% (MPSC at 2 or more workers)** and **±0-3% (MPMC)**. The flat MPMC result is consistent with the global ready queue, not the per-actor mailbox, being MPMC's bottleneck.

## Performance — what is measured

**On call-dispatch-bound recursion (`fib`, `tak`), protoClojure is 1.2-1.4× slower than Babashka 1.4.192. On `loop`/`recur` arithmetic, `reduce` over a list and `map` + `reduce` it is faster (0.36-0.75× Babashka's time). On LargeInteger workloads Babashka does not finish with default arithmetic.** The harness is [`benchmarks/bench.sh`](benchmarks/bench.sh): three runs per workload, best wall-clock time of three, each run a single process invocation including start-up. Babashka is GraalVM-native; both runtimes ran on the same machine.

| Workload | protoclj (ms) | bb (ms) | ratio | Notes |
|---|---:|---:|---:|---|
| `fib(30)` (pure recursion) | 712 | 524 | 1.36× | call-dispatch bound |
| `tak(18,12,6)` (Takeuchi) | 52 | 43 | 1.21× | call-dispatch bound |
| `sum-loop(1M)` (`loop`/`recur` arithmetic) | 79 | 219 | **0.36×** | tight SmallInteger loop |
| `reduce-list(10K)` (reduce + over list) | 27 | 36 | 0.75× | allocation + reduce |
| `sum-squares(1K)` (map + reduce) | 19 | 31 | 0.61× | start-up bound at this size |
| `factorial(100)` (158-digit result) | runs | **fails** | — | LargeInteger by default |

These are the most recent numbers in [`benchmarks/RESULTS.md`](benchmarks/RESULTS.md), measured after maps, strings and the concurrency primitives were added. Two earlier changes shaped the trajectory:

- **SmallInteger fast-path opcodes** (commit `700f352`): the VM short-circuits `(+ x y)`, `(< x y)` and the other arithmetic and comparison operators when both operands are tagged SmallIntegers, and routes everything else through protoCore's promoting `add` / `compare` / `multiply`. `fib(30)` went from 4113 ms to 720 ms.
- **Fewer attribute lookups per call** (commit `a099b45`): separate prototypes for single- and multi-arity function wrappers let the dispatcher pick the path with `getPrototype` alone, and the captures attribute is read only when a body has captures. `fib(30)` went to 620 ms, with 15% fewer instructions and 32% fewer L1-d cache misses (per `perf stat`).

The features added afterwards cost about 15% on `fib(30)` (620 ms → 712 ms); the extra parameters threaded through every recursive VM call are the likely cause, and recovering that cost is on the [roadmap](docs/ROADMAP.md).

JVM Clojure has not been measured. It has a JIT and adaptive inlining that protoClojure does not have, and no claim is made here about protoClojure's speed relative to it.

## How protoClojure compares

Three comparators matter — JVM Clojure, Babashka, and the rest of the protoCore family.

### vs. JVM Clojure (the canonical Clojure)

JVM Clojure is the language, the ecosystem, and the JIT-compiled runtime the rest of the Clojure world means when they say "Clojure". It is fast, it is mature, and it is the right tool when you need the JVM platform (Java libraries, the existing Clojure library ecosystem, persistent collections tuned over a decade, `core.async`, transducers).

protoClojure does not try to replace it. The places where protoClojure is **architecturally different**, not just an alternative implementation, are:

- **Start-up**: no JVM start-up; see the hello-world measurement under [Why this exists](#why-this-exists).
- **Footprint**: a native binary plus the protoCore shared library.
- **Numeric default**: automatic LargeInteger promotion instead of `long` arithmetic with explicit `*'` for promotion.
- **No JVM**: no `java.*`, no Java reflection. The design substitutes the Python and JavaScript ecosystems through UMD.

The places where JVM Clojure wins on substance, not just legacy:

- **Mature persistent-collection performance** — HAMT, RRB-tree, transient batching, well-tuned hashing. protoClojure uses protoCore's `ProtoList` / `ProtoTuple` / `ProtoSparseList` shapes, which are competent but not yet at the level of `clojure.lang.PersistentHashMap`.
- **JIT**. On any inner-loop workload that benefits from method-call inlining, the JVM goes through code paths protoClojure does not have.
- **Ecosystem**. Two decades of libraries, Datomic, Leiningen and deps, editors with deep CIDER / Calva integration. protoClojure has none of that today.

### vs. Babashka (Clojure on GraalVM native-image)

Babashka is the closest comparator and the one the benchmark section measures. It shares two of protoClojure's appeals — fast start-up and a low footprint — by shipping a GraalVM native-image. The differences:

- **Arbitrary-precision integers**: Babashka 1.4 fails on `(factorial 21)` (long overflow); protoClojure promotes automatically.
- **Concurrency substrate**: protoClojure's atoms, futures, promises, `pmap` and actors run directly on protoCore's GIL-free threads and compare-and-set.
- **REPL compilation**: protoClojure compiles every REPL form with the same compiler it uses for scripts.
- **Trade-off**: Babashka follows the Clojure-JVM `*` semantics, which means `long` arithmetic and overflow errors. protoClojure always promotes, paying a SmallInteger tag check on every operation and a LargeInteger allocation on overflow.

### vs. protoST / protoJS / protoPython (siblings on protoCore)

protoClojure is the *immutable-by-default, REPL-first* face of the protoCore family. It shares the kernel — `ProtoObject`, the garbage collector, the immutable collections, the GIL-free concurrency, the per-thread attribute cache — but expresses it through Clojure idiom: a Lisp reader, persistent vectors written `[x y z]`, keywords as values, `defn` / `fn` / `let` / `loop` / `recur`, multi-arity functions.

The property this membership is designed to confer: a value materialised in any of the runtimes is a real `ProtoObject`. Once the UMD providers exist, **a Python list is meant to reach protoClojure's `(count py-list)` through the same attribute chain walk every protoClojure call uses**, with no marshalling.

## Project status

protoClojure runs scripts and an interactive REPL. Version 0.0.1; no tagged release yet. Shipped changes are listed in [CHANGELOG.md](CHANGELOG.md).

| Area | Status |
|---|---|
| Lexer, reader, bytecode compiler and VM | Implemented |
| `def`, `if`, `do`, `fn`, `defn`, `let`, `loop`, `recur`, `when`, `cond`, `and`, `or` | Implemented (`let` and `loop` only inside function bodies) |
| Closures, variadic functions, `apply`, multi-arity | Implemented |
| SmallInteger fast path, LargeInteger and float promotion | Implemented |
| Lists, vectors, maps; named arguments `& {:keys [...] :or {...} :as m}` | Implemented |
| String functions (`clojure.string`-shaped, in the global namespace) | Implemented |
| Atoms, watches, futures, promises, `pmap` | Implemented |
| Actors with priority bands | Implemented |
| Local REPL on libreadline | Implemented |
| CPack packaging | Configured; TGZ and DEB verified on Linux |
| Quote reader macro, macros, sets, lazy sequences, exceptions | Planned |
| Namespaces and UMD interop providers (`py/`, `js/`, `pst/`) | Planned |
| nREPL server for CIDER / Calva / Conjure | Planned for v0.1 |

`ctest` registers **343 test cases: 262 conformance fixtures, 77 unit tests** (lexer, reader, bytecode module, runtime map, value equality and hashing, native stack guard, double printer) **and 4 CLI checks** (`--help`, a generated program with 70,000 distinct literals of each kind, a stack overflow in the REPL, and nil-valued globals in the REPL). All of them pass. The benchmark numbers above are reproduced by `./benchmarks/bench.sh`, the actor throughput numbers by `./benchmarks/actor-bench.sh`.

What is implemented:

- Reader: integers of any size (`12345678901234567890`, `42N`), floats (`3.14`, `1e6`), strings, symbols, keywords (`:foo`), `true` / `false` / `nil`, lists `(...)`, vectors `[...]`, maps `{...}`, `@form` as `(deref form)`, line comments (`;`), commas as whitespace.
- Special forms: `def`, `defn` and `fn` (single- and multi-arity, variadic, named-argument destructuring), `let`, `loop`, `recur` (in `loop` and as the implicit function-body target), `if`, `do`, `quote` (of symbols, keywords and other atoms), `apply`, `when`, `when-not`, `cond`, `and`, `or`, `future`.
- Closures: full N-level capture, including chained closures across `(fn ... (fn ... (fn ...)))`.
- Primitives (75 registered at startup):
  - arithmetic and comparison: `+ - * / inc dec < <= > >= = not=` (`=` compares maps, vectors and lists by value; a vector and a list with equal elements are `=`)
  - output: `println str`
  - lists and vectors: `list vector vec nth first rest cons count empty? reverse`
  - higher-order: `map filter reduce pmap`
  - predicates: `nil? not vector? list? map? string?`
  - maps: `hash-map assoc get contains? keys vals` (keys are hashed and matched by value, so a map or vector key is found with an equal map, vector or list)
  - strings: `subs upper-case lower-case starts-with? ends-with? includes? index-of replace join split trim triml trimr blank?`
  - atoms and watches: `atom atom? deref reset! swap! compare-and-set! add-watch remove-watch`
  - futures and promises: `make-future future? realized? promise promise? deliver`
  - actors: `actor actor? send send-h send-m send-l actor-stats`
- VM: 29 opcodes, including SmallInteger fast-path binary opcodes (`ADD SUB MUL LT LE GT GE EQ`), arity dispatch (`MAKE_FN` / `MAKE_FN_MULTI`), `CALL_APPLY` for spread arguments, `CALL_KW` for trailing keyword arguments, and `DUP` / `JUMP_IF_TRUE` for short-circuit forms.
- Numeric semantics: SmallInteger for values that fit a tagged pointer, automatic promotion to LargeInteger on overflow (no integer operation wraps around, whether compiled to an opcode or called through `apply` / `reduce`), automatic promotion to double when any operand is a float.

What is **not yet** implemented:

- **Clojure-style `agent`** with its send/await semantics (the in-tree `actor` is the protoCore-native variant — different surface, see [docs/tutorial/13-actors.md](docs/tutorial/13-actors.md)).
- **`delay` / `force`** and `volatile!` / `vreset!` / `vswap!`.
- **The quote reader macro `'`, macros, sets, lazy sequences, exceptions** and most of `clojure.core` beyond the primitives above.
- **The UMD module system**: `ns`, `(:require [py/numpy :as np])` and the foreign-dispatch protocol layer.
- **An nREPL server** for CIDER / Calva / Conjure. It is planned for v0.1; the local interactive REPL on libreadline ships today (see [docs/tutorial/10-repl.md](docs/tutorial/10-repl.md)).
- **A `core.clj`** evaluated at startup, so that library functions are composed in Clojure instead of installed as C++ primitives.
- **JIT or threaded dispatch.** The bytecode VM is a switch loop; both are possible upgrade paths, neither is scheduled.

The live tracker is [docs/STATUS.md](docs/STATUS.md); the roadmap is [docs/ROADMAP.md](docs/ROADMAP.md). The design specifications written during development (the protoCore call-convention adoption note, the foreign-dispatch protocol, the engineering principles) are archived under [docs/archive/design-specs/](docs/archive/design-specs/).

## Getting started

protoClojure depends on [protoCore](https://github.com/numaes/protoCore), which must be built first, and on the readline development package (`libreadline-dev` on Debian / Ubuntu, `readline-devel` on Fedora / RHEL, `readline` from Homebrew on macOS).

```bash
cd protoClojure
cmake -B build_release -S .
cmake --build build_release

./build_release/protoclj                     # interactive REPL
./build_release/protoclj script.clj          # run a .clj file
./build_release/protoclj --version           # version

ctest --test-dir build_release -j1           # 343 cases: 262 fixtures + 77 unit tests + 4 CLI checks
./benchmarks/bench.sh                        # benchmark against Babashka
./benchmarks/actor-bench.sh                  # actor throughput, varied worker counts
```

The benchmark harness expects a Babashka binary at `/tmp/proto-bench/bb` by default; pass the path as the second argument to override:

```bash
./benchmarks/bench.sh ./build_release/protoclj /usr/local/bin/bb
```

### Packaging

protoClojure configures installable packages for Linux, macOS, and Windows via CPack — the same machinery protoST uses. The platform-appropriate generators are selected automatically; you pick which artifact you want with `cpack -G`. All artifacts contain the `protoclj` binary, the documentation, the example `.clj` scripts under `share/protoClojure/examples`, and the benchmark scripts under `share/protoClojure/benchmarks`.

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

The DEB artifact declares a runtime dependency on `protocore` and the RPM artifact on `protoCore` (`CPACK_DEBIAN_PACKAGE_DEPENDS` and `CPACK_RPM_PACKAGE_REQUIRES` in `CMakeLists.txt`), so the package manager fails cleanly if libprotoCore is not installed. The TGZ / DMG / NSIS / ZIP artifacts do **not** carry libprotoCore — install it from its own package first, or build it side by side and add its install prefix to your loader path. The installed `protoclj` has an `INSTALL_RPATH` of `$ORIGIN/../lib` (Linux) / `@executable_path/../lib` (macOS), so as long as protoCore lives under the same `<prefix>/lib` no environment variable is needed.

```bash
# Quick sanity check after installing the DEB:
sudo dpkg -i protoclojure-0.0.1-Linux.deb
protoclj --version
protoclj /usr/share/protoClojure/examples/02-factorial.clj
```

Only the TGZ and DEB artifacts have been verified, on Linux. The RPM, macOS and Windows generators are configured but unverified, and no packages are published.

## Documentation

| Document | What it covers |
|---|---|
| [docs/LANGUAGE.md](docs/LANGUAGE.md) | The language reference — reader, evaluator, special forms, primitives. |
| [docs/STATUS.md](docs/STATUS.md) | Live status — what works, what departs from Clojure-JVM, what is on the list. |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Planned milestones, versioning, and how to read the plan. |
| [docs/INTEROP.md](docs/INTEROP.md) | The UMD interop design — pulling a Python or JavaScript module into a protoClojure namespace. |
| [docs/DESIGN.md](docs/DESIGN.md) | The architectural overview — reader, compiler, BytecodeModule, VM, primitives. |
| [docs/TUTORIAL.md](docs/TUTORIAL.md) | Index to the dual-audience tutorial under `docs/tutorial/`. |
| [CHANGELOG.md](CHANGELOG.md) | Changes since the start of the project. |
| [docs/archive/design-specs/](docs/archive/design-specs/) | Historical design specifications — engineering principles, phase plans, foreign dispatch, call convention. |

The conformance fixtures themselves are the de facto reference for what the implementation accepts — `tests/conformance/**/*.clj`.

## Related projects

Four language runtimes (protoJS, protoPython, protoST, protoClojure) and protoCpp's C++ examples are built on protoCore.

| Project | Role | Repository |
|---|---|---|
| protoCore | C++20 object model and runtime kernel: immutable structures, concurrent GC, GIL-free threads | https://github.com/numaes/protoCore |
| protoJS | JavaScript runtime on protoCore | https://github.com/gamarino/protoJS |
| protoPython | Python 3 runtime (protopy) and ahead-of-time compiler (protopyc) on protoCore | https://github.com/gamarino/protoPython |
| protoST | Smalltalk-inspired actor language on protoCore | https://github.com/gamarino/protoST |
| protoClojure | Clojure dialect on protoCore (early stage) | https://github.com/gamarino/protoClojure |
| protoCpp | Examples and benchmarks using protoCore directly from C++ | https://github.com/gamarino/protoCpp |

## Why "protoClojure"?

`proto` — built on protoCore. `Clojure` — the language idiom being modelled. The name signals what it is and where it lives in the family. "Clojure-on-protoCore" is avoided because the goal is not a port of Clojure-JVM; it is a Clojure-flavoured dialect that earns its place by what protoCore can do.

## The Swarm of One

protoClojure is designed and maintained by a single architect, Gustavo Marino, working with AI coding agents that draft code, tests and documentation under human review.

The work proceeds as a measured loop — design, implement, compile, measure, keep or discard, commit — and the repository keeps the evidence: the conformance suite grew with the implementation to 262 fixtures, performance changes record their `perf stat` measurements in the commit history, and every benchmark figure in this README can be reproduced with the scripts in [`benchmarks/`](benchmarks/).

## License

Copyright (c) 2026 Gustavo Marino. Released under the MIT License; see [LICENSE](LICENSE).

protoCore, protoJS, protoPython, protoST and protoCpp are also released under the MIT License.
