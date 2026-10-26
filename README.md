# protoClojure

> **A Clojure dialect on the protoCore object kernel — fast startup, GIL-free threads, arbitrary-precision integers by default, and a design for transparent interop with Python and JavaScript.**

protoClojure is a Clojure-inspired language runtime built on the [protoCore](https://github.com/numaes/protoCore) kernel. It is one of the language runtimes of the protoCore family — alongside [protoST](https://github.com/gamarino/protoST) (Smalltalk-inspired), [protoJS](https://github.com/gamarino/protoJS) (JavaScript) and [protoPython](https://github.com/gamarino/protoPython) (Python) — and it brings the **REPL-driven, immutability-first, data-as-substrate** stance of the Clojure community to a kernel that was already built around those three principles.

protoClojure is **not** a drop-in replacement for Clojure-JVM. There is no JVM, no Java interop, no Maven, no `clojure.java.*`. What it offers instead is the Clojure idiom — `defn`, `let`, `loop`/`recur`, closures, persistent collections, higher-order functions, keywords, multi-arity — running on a runtime that **starts in milliseconds**, **uses real OS threads with no GIL**, and **promotes to LargeInteger automatically** when an arithmetic result no longer fits in a tagged word. The design reaches the companion runtimes through the same UMD module system protoST and protoPython share, so that `(:require [py/numpy :as np])` becomes a pointer hand-off rather than a separate process; that module system is not implemented yet.

protoClojure is at an early stage: version 0.0.3, a preview tag with a Linux `.deb`; v0.1.0 is the first planned release.

## Why this exists

The Clojure community is small, technically demanding, and shares three convictions: *immutability as the default*, *data as the substrate*, *REPL-driven development*. protoCore was built on the same three principles without anyone setting out to host Clojure — they are the kernel's design stance. Putting them together is the natural step.

What protoClojure offers that JVM Clojure does not:

- **Native interop with Python and JavaScript modules (planned).** The design routes a `(:require [py/numpy :as np])` form through the same UMD plumbing protoPython and protoJS use, so the result is a real protoCore object — no FFI marshalling, no copy at the boundary, no separate process. The module system is not implemented yet; see [docs/INTEROP.md](docs/INTEROP.md) and the archived [foreign-dispatch design](docs/archive/design-specs/2026-06-14-foreign-dispatch.md).
- **Fast startup, small footprint.** No JVM warm-up: on the benchmark machine (AMD Ryzen 5 5500U), `protoclj examples/01-hello.clj` has a median wall time of 20.9 ms over 150 runs and a peak resident set size of 24 MB (what `/usr/bin/time -f %M` reports), `core.clj` loaded at start-up included, against Babashka's 9.7 ms and 15 MB on the same machine (2026-10-25, `main` 1e7b612 on protoCore 3.3.0; [report](benchmarks/reports/2026-10-25-readme-refresh/README.md)). Scripts and CLI tools are a viable form factor without GraalVM AOT: a script starting with `#!/usr/bin/env protoclj` runs as a command and chains in pipelines ([`examples/13-filter-grep.clj`](examples/13-filter-grep.clj)).
- **Arbitrary-precision integers by default.** When `(* acc n)` overflows a SmallInteger, protoCore's promoting `multiply` returns a LargeInteger and the program keeps going. `(factorial 100)` runs out of the box with plain `*`. Clojure's `*` uses long arithmetic and throws on overflow, so Babashka 1.4 fails on `(factorial 21)` written with `*`; written the portable Clojure way, with `*'`, it runs in Babashka too, and in protoClojure, where `*'` is the same function as `*` ([`examples/showcase/factorial.clj`](examples/showcase/factorial.clj)).
- **Real parallelism without a GIL.** Every protoCore-hosted runtime shares the same GIL-free concurrency model. Atoms are a protoCore compare-and-set; futures run on OS threads; actors run on a worker pool.

What protoClojure does **not** offer:

- **JVM interop.** No Java classes, no Maven, no Leiningen; the input and output functions of `clojure.java.io` and `clojure.java.shell` are provided as globals instead. The intended substitute, where applicable, is calling the Python or JavaScript ecosystem through UMD.
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

Code is data: a quoted form is an ordinary list, and a syntax-quote template builds one:

```clojure
(def form '(+ 1 2 3))
(println (first form) (rest form))   ;; => + (1 2 3)

(def xs [1 2])
(println `(+ ~@xs 10))               ;; => (clojure.core/+ 1 2 10)
```

A first macro: `defmacro` receives its arguments unevaluated, as data, and returns the code to compile in their place, at compile time, in files and at the REPL alike:

```clojure
(defmacro unless [test & body]
  `(if ~test nil (do ~@body)))

(def xs [1 2 3])
(unless (empty? xs) (println "has" (count xs) "items"))   ;; => has 3 items
(macroexpand-1 '(unless (empty? xs) (println "has" (count xs) "items")))
;; => (if (empty? xs) nil (do (println "has" (count xs) "items")))
```

The `clojure.core` macros come from `core.clj`, written in Clojure, embedded in the binary and loaded at start-up (about 4 ms of CPU):

```clojure
(defn total [items] (->> items (map :n) (reduce +)))
(when-let [t (total [{:n 1} {:n 3}])]
  (println "total" t))                           ;; => total 4
(doseq [x [1 2]] (print (-> x inc (* 10)) ""))  ;; => 20 30
(println (case (count "ab") 1 :one 2 :two :many)) ;; => :two
```

Destructuring takes values apart wherever a local is bound -- `let`, `loop`, function and macro parameters, `doseq` and the other binding macros:

```clojure
(defn describe [{:keys [name langs] :or {langs []}}]
  (let [[first-lang & others] langs]
    (str name ": " first-lang " and " (count others) " more")))
(println (describe {:name "ana" :langs ["clj" "py" "st"]}))  ;; => ana: clj and 2 more
(doseq [[k v] {:a 1}] (println k v))                        ;; => :a 1
```

Sets, with `clojure.set`'s algebra (as globals: there are no namespaces yet):

```clojure
(def seen (into #{} ["a" "b" "a"]))
(println (count seen) (contains? seen "a") (seen "z"))    ;; => 2 true nil
(println (= (union #{1 2} #{3}) #{1 2 3}) (intersection #{1 2} #{2 3}))  ;; => true #{2}
```

Regular expressions, with `clojure.string`'s functions written with or without their namespace:

```clojure
(println (re-find #"(\w+)@(\w+)" "mail joe@host"))           ;; => [joe@host joe host]
(println (re-seq #"\d+" "1 22 333"))                          ;; => (1 22 333)
(println (clojure.string/split "a, b,c" #",\s*"))             ;; => [a b c]
(println (clojure.string/replace "john smith" #"(\w+) (\w+)" "$2, $1"))  ;; => smith, john
```

Comprehensions and formatted output:

```clojure
(doseq [[item qty price] [["apple" 3 0.5] ["pear" 2 0.75]]]
  (printf "%-6s %2d x %.2f = %5.2f%n" item qty price (* qty price)))
;; => apple   3 x 0.50 =  1.50
;; => pear    2 x 0.75 =  1.50
(println (for [x (range 10) :when (odd? x) :let [y (* x x)] :while (< y 30)] y))  ;; => (1 9 25)
```

Lazy sequences, computed once, chunked as in Clojure, and walked in constant memory when nothing holds their head:

```clojure
(defn fib-seq
  ([] (fib-seq 0 1))
  ([a b] (lazy-seq (cons a (fib-seq b (+ a b))))))
(println (take 10 (fib-seq)))                              ;; => (0 1 1 2 3 5 8 13 21 34)
(println (->> (range) (filter odd?) (map (fn [x] (* x x))) (take 5)))  ;; => (1 9 25 49 81)
;; a grep over the standard input, however large it is:
(doseq [line (line-seq *in*)]
  (when (re-find #"ERROR" line) (println line)))
```

Everyday scripting vocabulary: `#(...)`, library aliases, `frequencies`, `sort-by`:

```clojure
(ns words (:require [clojure.string :as str]))
(def counts (frequencies (str/split "a b a c b a" #" ")))
(println (->> counts (sort-by (comp - val)) (map #(str (key %) ":" (val %))) (str/join " ")))
;; => a:3 b:2 c:1
(println (update-in {:stock {:apples 3}} [:stock :apples] + 2) (mod -7 3) (quot 7 2))
;; => {:stock {:apples 5}} 2 3
```

The 633 conformance fixtures under `tests/conformance/` are the executable reference for what the language accepts today (this README's examples among them: `tests/conformance/28-quote/readme-code-is-data.clj`, `tests/conformance/30-macros/readme-macros.clj`, `tests/conformance/32-core-macros/readme-core-macros.clj`, `tests/conformance/33-destructuring/readme-destructuring.clj`, `tests/conformance/34-sets/readme-sets.clj`, `tests/conformance/35-regex/readme-regex.clj`, `tests/conformance/36-format-for/readme-format-for.clj`, `tests/conformance/37-lazy-seqs/readme-lazy-seqs.clj`, `tests/conformance/38-vocabulary/readme-vocabulary.clj`; the grep is `tests/cli/lazy-grep-under-heap-limit.sh`).

## Concurrency — atoms, futures, actors

protoClojure exposes the GIL-free concurrency that protoCore already has underneath. There is no global interpreter lock; threads are real OS threads, tracked by the kernel's garbage collector.

```clojure
(defn slow-compute ([] (reduce + (range 100000))) ([x] (* x x)))

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

The per-actor mailbox is **lock-free and GC-visible**: three protoCore `ProtoMPSCQueue`s, one per priority band, plus one `claimed` flag. Senders never take a per-actor mutex; the running worker drains a whole band with one `takeAll`, which hands back that band's messages in FIFO order. The queues hang off the actor's wrapper object, so the collector traces every queued message — its function, its arguments and its promise. (Up to 0.0.1 the mailboxes were `std::atomic<ActorMessage*>` stacks of C++ heap nodes whose protoCore payloads were not rooted at all.)

#### Benchmark — actors

Measured on 2026-10-25 (`main` 1e7b612, protoCore 3.3.0) on an AMD Ryzen 5 5500U (6 cores, 12 threads) with a desktop session running (load average 5.4-5.8 at the round starts). Each row is **1,000,000 messages** with the trivial body `(inc v)`; runs take 2-8 seconds. The runner is [`benchmarks/actor-bench.sh`](benchmarks/actor-bench.sh), which verifies that every script reports the expected message count before computing a rate. Three rounds; the peak column is the best median over the worker counts 1, 2, 4, 6, 8 and 16, with the count in brackets.

| mode      | what it measures                                              | peak msg/s | 2026-09-24 |
|-----------|---------------------------------------------------------------|-----------:|-----------:|
| `single`  | 1 sender × 1 actor — per-actor pipeline floor                 | 207,082 (w=16) | 222,074 (w=2) |
| `fan-out` | 1 sender × 1000 actors (1000 msgs each) — ready queue stress  | 448,632 (w=2) | 552,486 (w=8) |
| `MPSC`    | 4 senders × 1 actor — per-actor sender contention             | 210,704 (w=4) | 201,329 (w=1) |
| `MPMC`    | 4 senders × 4 actors (round-robin) — both contention paths    | 280,113 (w=4) | 329,817 (w=16) |

These are upper bounds for a single-operation message body. Against 2026-09-24 (one run per worker count, so its peaks are favoured), `fan-out` is 19 % and `MPMC` 15 % lower, `single` 7 % lower and `MPSC` 5 % higher; the cause is not isolated. A cell varies by up to 1.6x between rounds, so the worker count at which a mode peaks is not stable; `MPMC` now peaks at 4 workers and falls to 171 k at 16. Tables with every worker count: [`benchmarks/reports/2026-10-25-readme-refresh/`](benchmarks/reports/2026-10-25-readme-refresh/README.md). The mailbox's history (the migration from atomic-pointer stacks to `ProtoMPSCQueue`s, and from a mutex before that) is in [`benchmarks/RESULTS.md`](benchmarks/RESULTS.md).

## Performance — what is measured

**Measured on 2026-10-25, `main` 1e7b612 on protoCore 3.3.0, against Babashka 1.4.192: protoClojure is faster than Babashka on call-dispatch-bound `fib` (0.93x its time) and on `loop`/`recur` arithmetic (0.45x), and slower on `tak` (1.41x), `reduce` over a list (1.54x) and `map` + `reduce` (1.61x), the last three short enough that protoclj's 11 ms of extra start-up weighs. On LargeInteger workloads Babashka does not finish with default `*` (it does with `*'`).** The harness is [`benchmarks/bench.sh`](benchmarks/bench.sh)'s method: three process runs per workload, best wall-clock time, start-up included; here repeated over five interleaved rounds with every output checked, medians of the rounds with their range. Babashka is GraalVM-native; both ran on the same machine, a desktop with a 1-minute load average of 5-7 on 12 CPUs, so compare the ratios rather than the milliseconds.

| Workload | protoclj (ms) | bb (ms) | ratio | Notes |
|---|---:|---:|---:|---|
| `fib(30)` (pure recursion) | 421 [402-454] | 455 [423-469] | **0.93×** | call-dispatch bound |
| `tak(18,12,6)` (Takeuchi) | 41 [38-46] | 29 [28-45] | 1.41× | call-dispatch bound, short |
| `sum-loop(1M)` (`loop`/`recur` arithmetic) | 77 [73-81] | 172 [165-198] | **0.45×** | tight SmallInteger loop |
| `reduce-list(10K)` (reduce + over list) | 29 [28-33] | 19 [19-20] | 1.54× | start-up bound at this size |
| `sum-squares(1K)` (lazy map + reduce) | 28 [27-34] | 17 [17-18] | 1.61× | start-up bound at this size |
| `factorial(100)` (158-digit result) | runs | **fails** with `*`; runs with `*'` | — | LargeInteger by default |

Against the previous README figures: from the June table (`ce9819d`), `fib` is 41 % and `tak` 21 % faster, while `reduce-list` and `sum-squares` went from faster than Babashka (0.75x, 0.61x) to slower (Babashka got faster on both, and protoclj's `sum-squares` pays for lazy `map` and for `core.clj` at start-up). From 2026-10-20 (protoCore 2.14.8, before macros), `fib`, `tak` and `reduce-list` moved by -5 % to -1 %, and **`sum-loop` is 18 % slower (65 -> 77 ms) and `sum-squares` 22 % slower (23 -> 28 ms)**; part of that is the about 4 ms `core.clj` adds to every start, the rest is not isolated. Raw runs, start-up, showcase and actor tables, and the load log: [`benchmarks/reports/2026-10-25-readme-refresh/`](benchmarks/reports/2026-10-25-readme-refresh/README.md).

Earlier measurements, each with its report (the README's text of 2026-10-24 for these is kept in [`benchmarks/RESULTS.md`](benchmarks/RESULTS.md#earlier-readme-performance-sections-moved-2026-10-25)):

- 2026-10-25, lists as `ProtoLinkedList`s (M47): [`reports/2026-10-25-linked-list/`](benchmarks/reports/2026-10-25-linked-list/README.md); primitives in the caller's context: [`reports/2026-10-25-primitive-context/`](benchmarks/reports/2026-10-25-primitive-context/).
- 2026-10-24, seq cost and cons lists: [`reports/2026-10-24-seq-cost/`](benchmarks/reports/2026-10-24-seq-cost/README.md); the macros branch end to end and its regression fixes (start-up +35.7 % in cycles): [`reports/2026-10-24-regression/`](benchmarks/reports/2026-10-24-regression/README.md).
- 2026-10-22, protoCore's unified exception model: [`reports/2026-10-22-unified-exceptions/`](benchmarks/reports/2026-10-22-unified-exceptions/).
- 2026-10-20, protoCore 2.14.8 and the call path: [`reports/2026-10-20/`](benchmarks/reports/2026-10-20/).
- 2026-10-19 and 2026-10-13, protoCore 2.14.7 and 2.14.6, with JVM Clojure 1.12.0 (813-864 ms as a process, 3-16x faster than protoClojure once warm): protoCore's [write-path](https://github.com/numaes/protoCore/blob/master/docs/reports/2026-10-18-write-path.md) and [integer-compare](https://github.com/numaes/protoCore/blob/master/docs/reports/2026-10-12-cross-runtime-int-compare.md) reports.
- June 2026, the first tables and the `fib(30)` trajectory (4113 -> 720 -> 620 -> 712 ms through the SmallInteger fast path and fewer lookups per call): [`benchmarks/RESULTS.md`](benchmarks/RESULTS.md).


## Showcase — the same scripts under protoclj and Babashka

protoClojure is not another Babashka. It is Clojure-style scripting on a
kernel, protoCore, that also runs Smalltalk (protoST), Python (protoPython)
and JavaScript (protoJS): the same object model, collector and GIL-free
threads under all of them. The scripts in [`examples/showcase/`](examples/showcase/)
show how far the Clojure side already goes: each runs **unchanged** under
`protoclj` and `bb` and prints a result that checks its work.

```bash
python3 -I examples/showcase/gen-data.py /tmp/data        # 190 MB of seeded input
protoclj examples/showcase/grep.clj < /tmp/data/access.log
protoclj examples/showcase/json-transform.clj /tmp/data/orders.json
protoclj examples/showcase/word-freq.clj < /tmp/data/corpus.txt
```

Measured on 2026-10-25 (5 interleaved rounds, medians, wall time and peak
RSS from `/usr/bin/time -v`, on a desktop with a load average of 3.6-4.9;
protoclj `main` 1e7b612 on protoCore 3.3.0, Babashka 1.4.192; start-up from
150 alternating runs):

| script | protoclj | bb | protoclj RSS | bb RSS |
|---|---:|---:|---:|---:|
| start-up (empty file) | 21 ms | 10 ms | 24 MB | 15 MB |
| grep over a 115 MB log on stdin | 16.7 s | 1.9 s | 140 MB | 134 MB |
| JSON transform, 300,000 records (43 MB) | 4.1 s | 2.5 s | 1,424 MB | 338 MB |
| word frequencies, 31 MB of text | 17.2 s | 4.7 s | 143 MB | 143 MB |
| `(factorial 1000)` with `*'` (10 ms resolution) | 20 ms | 10 ms | 24 MB | 18 MB |

Against 2026-10-24 (`feature/macros` on protoCore 3.1.0) protoclj is 7-20 %
faster on the three scripts and the JSON transform's peak RSS fell from
1,967 MB (it varies by 60 % between rounds); relative to bb, only the JSON
transform (2.0x -> 1.6x) and the word count (4.1x -> 3.7x) moved. Runner:
[`benchmarks/reports/2026-10-23-showcase/`](benchmarks/reports/2026-10-23-showcase/);
this run's results: [`benchmarks/reports/2026-10-25-readme-refresh/`](benchmarks/reports/2026-10-25-readme-refresh/README.md).

**Where bb wins, plainly:** start-up (about 2x), speed on these scripts
(1.6-9x: the grep's regular expressions run on `std::regex`, which is slow),
memory for large parsed data (protoCore's 64-byte cells: 4x on the JSON
document), and above all the ecosystem: bb has hundreds of libraries, pods,
tasks and Java classes; protoclj has what this README lists.

**Factorial, without spin:** with plain `*`, `(reduce * (range 1 1001))`
raises `ArithmeticException: long overflow` in bb, as in JVM Clojure, and
gives the exact 2,568-digit result in protoclj, whose `*` promotes (D14).
Written the portable way, with `*'`, it runs in both
([`factorial.clj`](examples/showcase/factorial.clj),
[`factorial-star.clj`](examples/showcase/factorial-star.clj)).

**Python and Smalltalk interop: not there yet.** `(import-module
"logical/path")` loads a module through protoCore's module discovery and
calls its members ([INTEROP.md §0](docs/INTEROP.md)); it works end to end
for native C++ modules, shown with protoCore's example geometry module in
the `protoclj_geometry` test build. Calling a protoPython or protoST module
from protoclj does not work: protoclj embeds neither runtime and no provider
bridges them, so `(import-module "py/math")` raises `FileNotFoundException:
Could not locate module py/math on the resolution chain`. That bridge is the
point of the design and the next large piece of work.

**Where protoclj is interesting today:** exact integers by default, the
whole runtime on GIL-free OS threads (atoms on compare-and-set, futures,
`pmap`, actors), every value an immutable protoCore object with structural
sharing, data streamed in constant memory under a hard heap ceiling
(`line-seq`, `parse-stream`), and a small C++ code base on a kernel that
other languages share.

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

- **Mature persistent-collection performance** — HAMT, RRB-tree, transient batching, well-tuned hashing. protoClojure uses protoCore's `ProtoList` / `ProtoMap` shapes, which are competent but not yet at the level of `clojure.lang.PersistentHashMap`.
- **JIT**. On any inner-loop workload that benefits from method-call inlining, the JVM goes through code paths protoClojure does not have.
- **Ecosystem**. Two decades of libraries, Datomic, Leiningen and deps, editors with deep CIDER / Calva integration. protoClojure has none of that today.

### vs. Babashka (Clojure on GraalVM native-image)

Babashka is the closest comparator and the one the benchmark section measures. It shares two of protoClojure's appeals — fast start-up and a low footprint — by shipping a GraalVM native-image. The differences:

- **Arbitrary-precision integers**: with plain `*`, Babashka 1.4 fails on `(factorial 21)` (`ArithmeticException: long overflow`) and protoClojure promotes; with `*'`, as portable Clojure code writes it, both compute `(factorial 1000)` exactly.
- **Concurrency substrate**: protoClojure's atoms, futures, promises, `pmap` and actors run directly on protoCore's GIL-free threads and compare-and-set.
- **REPL compilation**: protoClojure compiles every REPL form with the same compiler it uses for scripts.
- **Trade-off**: Babashka follows the Clojure-JVM `*` semantics, which means `long` arithmetic and overflow errors. protoClojure always promotes, paying a SmallInteger tag check on every operation and a LargeInteger allocation on overflow.

### vs. protoST / protoJS / protoPython (siblings on protoCore)

protoClojure is the *immutable-by-default, REPL-first* face of the protoCore family. It shares the kernel — `ProtoObject`, the garbage collector, the immutable collections, the GIL-free concurrency, the per-thread attribute cache — but expresses it through Clojure idiom: a Lisp reader, persistent vectors written `[x y z]`, keywords as values, `defn` / `fn` / `let` / `loop` / `recur`, multi-arity functions.

The property this membership is designed to confer: a value materialised in any of the runtimes is a real `ProtoObject`. Once the UMD providers exist, **a Python list is meant to reach protoClojure's `(count py-list)` through the same attribute chain walk every protoClojure call uses**, with no marshalling.

## Project status

protoClojure runs scripts and an interactive REPL, both one top-level form at a time: each form is read, compiled and run before the next is read, as in Clojure. Version 0.0.3, a preview tag (v0.1.0 is the first planned release). Shipped changes are listed in [CHANGELOG.md](CHANGELOG.md).

| Area | Status |
|---|---|
| Lexer, reader, bytecode compiler and VM | Implemented |
| `def`, `if`, `do`, `fn`, `defn`, `let`, `loop`, `recur`, `when`, `cond`, `and`, `or` | Implemented (`let` and `loop` also at the top level) |
| Closures, variadic functions, `apply`, multi-arity | Implemented |
| SmallInteger fast path, LargeInteger and float promotion | Implemented |
| Lists, vectors, maps; named arguments `& {:keys [...] :or {...} :as m}` | Implemented |
| String functions (`clojure.string`-shaped, in the global namespace) | Implemented |
| Atoms, watches, futures, promises, `pmap` | Implemented |
| Actors with priority bands | Implemented |
| Local REPL on libreadline | Implemented |
| CPack packaging | Configured; TGZ and DEB verified on Linux |
| Exceptions: `try` / `catch` / `finally`, `throw`, `ex-info` | Implemented |
| Input and output: files, `with-open`, `sh`, `getenv`, HTTP client and Ring server, TCP / UDP / TLS sockets (on protoIO) | Implemented |
| Quote `'` and syntax-quote `` ` `` / `~` / `~@` | Implemented |
| Scripts run form by form, as the REPL does (a compile error names its form's line and column) | Implemented |
| Macros: `defmacro`, `macroexpand-1`, `macroexpand`, `eval`; hygienic syntax-quote (qualified symbols, `foo#` gensyms, `gensym`) | Implemented (branch `feature/macros`; two namespaces only, `user` and `clojure.core`) |
| `clojure.core` macros (`when-let`, `if-let`, `->`, `->>`, `some->`, `cond->`, `doto`, `doseq`, `dotimes`, `case`, `condp`, ...) in a bootstrap `core.clj` | Implemented (branch `feature/macros`; no `letfn`) |
| Destructuring (`[a b & r :as all]`, `{x :x :keys [...] :strs [...] :syms [...] :or {...} :as m}`, nested) in `let`, `loop`, `fn`, `defn`, `defmacro` and the binding macros | Implemented (branch `feature/macros`) |
| Sets `#{...}`, `hash-set`, `set`, `disj`, `into`, `union` / `intersection` / `difference` | Implemented (branch `feature/macros`; `clojure.set` functions are globals) |
| Regular expressions `#"..."`, `re-find`, `re-matches`, `re-seq`, `re-matcher`, `re-groups`, `replace` / `split` with a pattern | Implemented (branch `feature/macros`; `std::regex` ECMAScript with Java's inline flags translated, D45) |
| `format`, `printf`, `for` and `doseq` with `:let` / `:when` / `:while`, `range` | Implemented (branch `feature/macros`) |
| Lazy sequences: `lazy-seq`, lazy `map` `filter` `take` `range` `iterate` `concat` `mapcat` `partition` `for` `line-seq` `re-seq` ..., `doall` / `dorun` / `realized?`, chunked as in Clojure; walks that do not hold the head run in constant memory | Implemented (branch `feature/macros`; needs protoCore 3.1.0's lazy objects, branch `feature/lazy-object`; [LANGUAGE.md §10](docs/LANGUAGE.md)) |
| `#(...)`; `ns` / `require` with `:as` aliases for clojure.string, clojure.set, clojure.java.io, clojure.java.shell; `letfn`; the everyday vocabulary (`quot rem mod sort sort-by frequencies group-by update update-in assoc-in get-in merge merge-with select-keys zipmap partial comp juxt some every? max-key min-key ...`) | Implemented (branch `feature/macros`; no full namespaces, D52) |
| `read-string`, `clojure.edn/read-string`; JSON as `cheshire.core` (`parse-string`, `parse-stream`, `generate-string`, `generate-stream`) and `clojure.data.json` (`read-str`, `read`, `write-str`, `write`) over one C++ parser and writer; `parse-long`, `parse-double`, `parse-boolean` | Implemented (branch `feature/macros`; [LANGUAGE.md §19](docs/LANGUAGE.md#19-data-formats-edn-and-json), D56-D58) |
| Full namespaces and UMD interop providers (`py/`, `js/`, `pst/`) | Planned |
| nREPL server for CIDER / Calva / Conjure | Planned for v0.1 |

`ctest` registers **873 test cases: 686 conformance fixtures, 138 unit tests** (lexer, reader, bytecode module, runtime map, map key semantics, value lifetime, the vector representation, value equality and hashing, native stack guard, double printer, exception values, rethrow) **and 49 CLI checks, counting the documentation checks** (`--help`, three checks of the adaptive heap controller -- on by default, off with `PROTOCORE_ADAPTIVE_HEAP=0`, peak RSS bounded under a small budget --, a generated program with 70,000 distinct literals of each kind, bulk collection builders under a heap limit, the reader's values under a heap limit, the garbage a `loop` makes under a heap limit, joins that park for the collector, threads waiting for a claimed lazy seq or a promise while the collector runs, actor message payloads under a heap limit, a stack overflow in the REPL, nil-valued globals in the REPL, literals and quote in the REPL, several forms per REPL line and `:load`, quoted data under a heap limit, macros at the REPL, macro expansion under a heap limit, destructuring under a heap limit, sets under a heap limit, regular expressions under a heap limit, `for` / `doseq` / `format` under a heap limit, the scripting vocabulary at the REPL, the embedded core.clj, scripts run form by form, deeply nested source, an uncaught exception, exceptions under a heap limit, `read-line` in pipelines, `*command-line-args*`, exit statuses, `sh` with `:dir` and `:env` and no shell on `PATH`, `localhost` requests to a server started without a host, an HTTP server under a heap limit, scripts run as commands through a `#!` line, lazy sequences under a heap limit, holding and not holding the head of a lazy seq, a grep over a stream larger than the heap ceiling, lazy seqs at the REPL, the tutorial's twenty-line script in a pipeline, JSON and EDN at the REPL, JSON parsed, written and streamed under heap limits, `pmap` joining every worker, and a list of 2 x 10^6 reclaimed cell by cell as it is walked under a heap limit; plus `docs/snippets`, which runs every Clojure snippet of this README, LANGUAGE.md and the tutorial). All 873 pass locally on Linux against protoCore 3.3.0 (2026-10-25, `main` 1e7b612). The benchmark numbers above are reproduced by `benchmarks/reports/2026-10-25-readme-refresh/run-all.sh`, which repeats `./benchmarks/bench.sh`'s measurement, the showcase runner and `./benchmarks/actor-bench.sh`.

What is implemented:

- Reader: integers of any size (`12345678901234567890`, `42N`), floats (`3.14`, `1e6`), strings, symbols, keywords (`:foo`), `true` / `false` / `nil`, lists `(...)`, vectors `[...]`, maps `{...}` and sets `#{...}` (a repeated key or element is a read error, as in Clojure), regular expressions `#"..."`, `@form` as `(clojure.core/deref form)`, quote `'form`, syntax-quote `` `form `` with `~` and `~@` (expanded when read, as in Clojure, with symbols qualified as `clojure.core/x` or `user/x` and `foo#` auto-gensyms), line comments (`;`), `#_` (discard the next form), commas as whitespace; strings take `\uXXXX` (surrogate pairs included) and octal escapes. The reader yields the values the program manipulates at run time — a symbol value, a string, a vector, a map — which is the base for quote and macros ([DESIGN.md §10](docs/DESIGN.md)).
- Special forms: `def`, `defn` and `fn` (single- and multi-arity, variadic, destructuring parameters, named arguments, docstrings), `defmacro` (expanded when a form is compiled, `&form` and `&env`), `let`, `loop` (also at the top level; both destructure), `recur` (in `loop` and as the implicit function-body target), `if`, `do`, `quote` (of any datum: a quoted list is a list value), `apply` (over a list, a vector or nil), `and`, `or`, `try` / `catch` / `finally`, `throw`.
- Macros from `core.clj` (written in Clojure, embedded in the binary): `when`, `when-not`, `if-not`, `cond`, `comment`, `while`, `if-let`, `when-let`, `if-some`, `when-some`, `->`, `->>`, `as->`, `some->`, `some->>`, `cond->`, `cond->>`, `doto`, `dotimes`, `doseq`, `case`, `condp`, `with-open`, `future`, `for` (lazy, with `:let` / `:when` / `:while`, as `doseq` has), `lazy-seq`, `lazy-cat`, `letfn`, `ns`; and the functions `identity some? true? false? zero? pos? neg? nnext ffirst nthnext butlast into key val constantly complement partial comp juxt fnil some every? not-any? not-every? max-key min-key get-in assoc-in update update-in merge merge-with select-keys zipmap group-by`.
- Closures: full N-level capture, including chained closures across `(fn ... (fn ... (fn ...)))`.
- Primitives (179 registered at startup, plus the `clojure.edn`, `cheshire.core` and `clojure.data.json` namespace objects):
  - arithmetic and comparison: `+ - * / inc dec quot rem mod max min compare < <= > >= = not=` (`=` compares maps, vectors and lists by value; a vector and a list with equal elements are `=`)
  - sorting and counting: `sort sort-by frequencies`; `require` (aliases for the library namespaces); `System/exit`, `System/getenv`, `System/currentTimeMillis`, `System/nanoTime`
  - output: `println print prn pr pr-str str format printf`
  - numbers: `number? integer? int? float? double? == hash bigint biginteger`, and `+' -' *' inc' dec'` (the same functions as `+ - * inc dec`, which already promote)
  - vectors: `mapv filterv peek pop`, and a vector called as a function of its index
  - reading data: `read-string` (the first form of a string, as code is read), `clojure.edn/read-string` (data only), `parse-long parse-double parse-boolean`; `pr-str` writes what they read back
  - JSON ([LANGUAGE.md §19](docs/LANGUAGE.md#19-data-formats-edn-and-json)): `cheshire.core/parse-string parse-stream generate-string generate-stream decode encode` and `clojure.data.json/read-str read write-str write`, by full name or through `require` / `ns` aliases
  - macros and symbols: `macroexpand-1 macroexpand eval gensym symbol keyword name list*`
  - lists and vectors: `list vector vec nth first second next last rest cons conj count empty? reverse coll? sequential?`, and `seq concat` (`seq` of a vector is its chunked seq, as in Clojure). A list is a chain of immutable cells holding first, rest and count, so `first`, `rest`, `count` and `conj` are O(1) (DESIGN.md M44). A vector is a protoCore `ProtoList` of its elements in a one-entry `ProtoSparseList` box, which is what carries its type; `nth` is O(log n), `vector` / `vec` are O(1), and a vector is collected like any other value (it used to be an interned `ProtoTuple`, which protoCore never frees — decision R2)
  - higher-order: `reduce pmap`
  - lazy sequences ([LANGUAGE.md §10](docs/LANGUAGE.md)): `map` (over several collections too) `filter remove take take-while drop drop-while iterate repeat repeatedly cycle range mapcat partition partition-all interleave distinct`, lazy `concat`, `line-seq`, `re-seq`, `doall dorun run! chunked-seq?`, and `realized?` of a lazy seq. A lazy seq is a protoCore lazy object, computed once and shared safely by threads (decision M28)
  - predicates: `nil? not vector? list? seq? map? string? symbol? keyword? even? odd?`
  - sets: `hash-set set set? disj union intersection difference subset? superset?`, and `conj` / `contains?` / `get` / `count` / `empty?` / a set as a function; a set is an immutable object holding a `ProtoMap` from each element to itself ([LANGUAGE.md §4.4](docs/LANGUAGE.md))
  - maps: `hash-map assoc dissoc get contains? keys vals`, and `count` / `empty?` on maps. A map is an immutable protoCore `ProtoMap` driven through protoCore's shared hashed-collection helper: keys match by value (a map or vector key is found with an equal map, vector or list, a string key with an equal string built at run time), numbers of different types are different keys, iteration order is unspecified, keys are ordinary garbage, and `count` is O(n) ([LANGUAGE.md §4.3](docs/LANGUAGE.md))
  - strings: `subs upper-case lower-case starts-with? ends-with? includes? index-of replace replace-first join split split-lines trim triml trimr blank?` (also as `clojure.string/x`, or `str/x` after `(require '[clojure.string :as str])`)
  - regular expressions: `re-pattern re-find re-matches re-seq re-matcher re-groups`, the `#"..."` literal, and `replace` / `replace-first` / `split` with a pattern ([LANGUAGE.md §2.10](docs/LANGUAGE.md))
  - identity: `identical?`
  - atoms and watches: `atom atom? deref reset! swap! compare-and-set! add-watch remove-watch`
  - futures and promises: `make-future future? realized? promise promise? deliver`
  - actors: `actor actor? send send-h send-m send-l actor-stats`
  - exceptions: `ex-info ex-data ex-message ex-cause`
- Input and output (36 more globals, [LANGUAGE.md §18](docs/LANGUAGE.md#18-input-and-output)), on the shared [protoIO](https://github.com/gamarino/protoIO) library:
  - files: `slurp` (also of `http(s)` URLs), `spit` (`:append`), `reader`, `writer`, `with-open`, `read-line`, `write`, `close`, `line-seq`, `file`, `file-seq`, `exists?`, `directory?`, `delete-file`, `make-parents`, `copy`
  - programs and the process: `sh` (`:in`, `:dir`, `:env`) answering `{:exit :out :err}`, `getenv`, `exit`, `*command-line-args*`
  - HTTP client in babashka http-client's shape (`http-get`, `http-post`, `http-put`, `http-delete`, `http-head`, `http-request`) and a Ring server (`run-server`, `stop-server`, `server-port`), each connection on its own thread
  - sockets: `tcp-connect` (with `:tls`), `tcp-listen`, `tcp-accept`, `socket-read-line`, `socket-write`, `socket-close`, `udp-socket`, `udp-send`, `udp-receive`
  - failures are `FileNotFoundException`, `ConnectException`, `SocketTimeoutException`, `UnknownHostException`, ... with ex-data `{:type :file-not-found :errno 2}`
- Exceptions: `catch` by class over a built-in hierarchy mirroring the Java classes (`Exception`, `ExceptionInfo`, `ArithmeticException`, `IOException`, `StackOverflowError`, …, simple or qualified names); errors raised by built-in functions are catchable exceptions of the class their message names; `deref` of a failed future raises `ExecutionException` with the original as its cause ([LANGUAGE.md §14](docs/LANGUAGE.md#14-exceptions)).
- VM: 40 opcodes, including SmallInteger fast-path binary opcodes (`ADD SUB MUL LT LE GT GE EQ`), arity dispatch (`MAKE_FN` / `MAKE_FN_MULTI`), `CALL_APPLY` for spread arguments, `CALL_KW` for trailing keyword arguments, `DUP` / `JUMP_IF_TRUE` for short-circuit forms, `TRY_BEGIN` / `TRY_END` / `THROW` / `EXC_MATCH` for exceptions, and `PUSH_LOCAL_CLEAR` for the last read of a local (locals clearing, so a walk does not hold the head of a lazy seq).
- Numeric semantics: SmallInteger for values that fit a tagged pointer, automatic promotion to LargeInteger on overflow (no integer operation wraps around, whether compiled to an opcode or called through `apply` / `reduce`), automatic promotion to double when any operand is a float.

What is **not yet** implemented:

- **Clojure-style `agent`** with its send/await semantics (the in-tree `actor` is the protoCore-native variant — different surface, see [docs/tutorial/13-actors.md](docs/tutorial/13-actors.md)).
- **`delay` / `force`** and `volatile!` / `vreset!` / `vswap!`.
- **Transducers, full namespaces, vars** and most of `clojure.core` beyond the primitives and `core.clj` above.
- **The UMD module system**: `(:require [py/numpy :as np])` and the foreign-dispatch protocol layer (`ns` and `require` know only the library namespaces above).
- **An nREPL server** for CIDER / Calva / Conjure. It is planned for v0.1; the local interactive REPL on libreadline ships today (see [docs/tutorial/10-repl.md](docs/tutorial/10-repl.md)).
- **JIT or threaded dispatch.** The bytecode VM is a switch loop; both are possible upgrade paths, neither is scheduled.

The live tracker is [docs/STATUS.md](docs/STATUS.md); the roadmap is [docs/ROADMAP.md](docs/ROADMAP.md). The design specifications written during development (the protoCore call-convention adoption note, the foreign-dispatch protocol, the engineering principles) are archived under [docs/archive/design-specs/](docs/archive/design-specs/).

## Getting started

protoClojure depends on [protoCore](https://github.com/numaes/protoCore), which must be built first; on [protoIO](https://github.com/gamarino/protoIO), installed or checked out next to protoClojure as `../protoIO` (then built as part of protoClojure's build and linked statically); and on the readline and OpenSSL development packages (`libreadline-dev libssl-dev` on Debian / Ubuntu, `readline-devel openssl-devel` on Fedora / RHEL, `readline openssl@3` from Homebrew on macOS). See [docs/INSTALLATION.md](docs/INSTALLATION.md).

```bash
cd protoClojure
cmake -B build_release -S .
cmake --build build_release

./build_release/protoclj                     # interactive REPL
./build_release/protoclj script.clj a b      # run a .clj file; *command-line-args* is ("a" "b")
./build_release/protoclj --version           # version

ctest --test-dir build_release -j1           # 589 cases: 438 fixtures + 124 unit tests + 27 CLI checks
./benchmarks/bench.sh                        # benchmark against Babashka
./benchmarks/actor-bench.sh                  # actor throughput, varied worker counts
```

On **Windows**, protoClojure builds natively with Visual Studio 2022 (MSVC) and runs scripts and the REPL from `cmd.exe` or PowerShell; the steps, and what differs there (no readline, `;`-separated `PATH` for `sh`, the test harness running through Git Bash), are in [docs/INSTALLATION.md — Windows (MSVC)](docs/INSTALLATION.md#windows-msvc).

The benchmark harness expects a Babashka binary at `/tmp/proto-bench/bb` by default; pass the path as the second argument to override:

```bash
./benchmarks/bench.sh ./build_release/protoclj /usr/local/bin/bb
```

### Memory

`protoclj` runs protoCore's adaptive heap controller (protoCore
[docs/GarbageCollector.md](https://github.com/numaes/protoCore/blob/master/docs/GarbageCollector.md),
"Adaptive heap controller"). The heap budget is **75 % of the smaller of
physical memory and the process memory limit** (the cgroup limit on Linux, the
job object's on Windows); within it, a soft limit starts at 128 MiB of cells and
grows only while the program waits for the collector, so a program that
allocates much more than it keeps runs in a bounded heap. **Memory is not
returned to the operating system**: the resident size stays at the run's
high-water mark. A program whose live data exceeds the budget stops with
protoCore's out-of-memory message.

| Environment variable | Effect |
|---|---|
| `PROTOCORE_HEAP_LIMIT_CELLS=<hard>` or `<soft>,<hard>` | Sets the budget (and the initial soft limit) in 64-byte cells; `1000000` is about 64 MiB. |
| `PROTOCORE_ADAPTIVE_HEAP=0` | Turns the controller off: the budget becomes a fixed hard limit. |
| `PROTOCORE_ADAPTIVE_HEAP_START=budget` | Starts the soft limit at the budget, for a run known to fit. |
| `PROTOCORE_HEAP_TRACE=1` | Prints one line per collection cycle on standard error. |

Before this default (2026-10-10), protoclj set no limit and protoCore never
started a collection by itself: a program grew its heap with every allocation
(with six allocating tasks, `coll_alloc` was killed at 16 GB). A protoclj built
against a protoCore older than 2.10.0 had no controller and kept that old
behaviour (the declared minimum is now 3.3.0).

### Packaging

protoClojure configures installable packages for Linux, macOS, and Windows via CPack — the same machinery protoST uses. The platform-appropriate generators are selected automatically; you pick which artifact you want with `cpack -G`. All artifacts contain the `protoclj` binary, the documentation, the example `.clj` scripts under `share/protoClojure/examples`, and the benchmark scripts under `share/protoClojure/benchmarks`.

```bash
# (After a successful build of protoCore and protoClojure.)
cd build_release

# --- Linux ---------------------------------------------------------------
cpack -G TGZ                # protoclojure-0.0.3-Linux.tar.gz (universal)
cpack -G DEB                # protoclojure-0.0.3-Linux.deb   (Debian / Ubuntu)
cpack -G RPM                # protoclojure-0.0.3-Linux.rpm   (Fedora / RHEL)

# --- macOS ---------------------------------------------------------------
cpack -G DragNDrop          # protoclojure-0.0.3-Darwin.dmg

# --- Windows -------------------------------------------------------------
cpack -G NSIS               # protoclojure-0.0.3-win64.exe   (NSIS installer)
cpack -G ZIP                # protoclojure-0.0.3-win64.zip   (portable)
```

The DEB artifact declares a runtime dependency on `protocore` and the RPM artifact on `protoCore` (`CPACK_DEBIAN_PACKAGE_DEPENDS` and `CPACK_RPM_PACKAGE_REQUIRES` in `CMakeLists.txt`), so the package manager fails cleanly if libprotoCore is not installed. The TGZ / DMG / NSIS / ZIP artifacts do **not** carry libprotoCore — install it from its own package first, or build it side by side and add its install prefix to your loader path. The installed `protoclj` has an `INSTALL_RPATH` of `$ORIGIN/../lib` (Linux) / `@executable_path/../lib` (macOS), so as long as protoCore lives under the same `<prefix>/lib` no environment variable is needed.

```bash
# Quick sanity check after installing the DEB:
sudo dpkg -i protoclojure-0.0.3-Linux.deb
protoclj --version
protoclj /usr/share/protoClojure/examples/02-factorial.clj
```

Only the TGZ and DEB artifacts have been verified on Linux, and the ZIP on Windows 11. The RPM, macOS and NSIS generators are configured but unverified, and no packages are published.

## Debugging in VS Code

`protoclj --dap` is a [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
server, and `editor-integration/vscode/` is the VS Code extension that uses it.
VS Code highlights Clojure itself; the extension adds the debugger (debug type
`protoclj`) and breakpoints for the `clojure` language, and coexists with Calva.

**Install.** Install `protoclj` (above); the extension finds it through the
`protoclj.path` setting, else `%LOCALAPPDATA%\Programs\proto\bin\protoclj.exe`
(Windows) or `~/.local/bin/protoclj`, else the `PATH`. Then package and install
the extension (Node.js for `npx`):

```bash
cd editor-integration/vscode
npx @vscode/vsce@3.2.1 package --skip-license
code --install-extension protoclj-debug-0.4.1.vsix
```

**Start a session.** Open a `.clj` file, click in the gutter to set breakpoints,
press **F5** — no `launch.json` needed; `examples/.vscode/launch.json` has
ready-made configurations (`program`, `args`, `stopOnEntry`).

**What works.** Line breakpoints; Continue, Step Over / Into / Out, Pause; the
call stack (named fns, anonymous ones as `fn@file.clj:line`, `<top level>`);
Locals of the selected frame by name — parameters, closure captures, `let`,
`loop` and `catch` bindings — with collections expandable; Watch, Debug Console
and hover evaluate expressions in the selected frame; the script's output in
the Debug Console; `stopOnEntry`; exception breakpoints (*All Exceptions*,
*Uncaught Exceptions*: a throw no `try`/`catch` of the script catches) that
stop before the exception propagates, with the throw's stack and locals, each
with an optional condition (a Clojure form; the exception is `ex`, e.g.
`(= "bad" (ex-message ex))`); line breakpoints with a condition, a hit count
(`N`, `>= N`, `% N`...) or a log message (`i={i}`);
every thread that runs the script (`main`, `future@file.clj:12`,
`pmap@...`, actor workers) is its own thread, and a stop stops only that
thread while the others run on; clojure.core's Clojure source, embedded in the
binary, is served read-only (step into it with `"justMyCode": false`, set
breakpoints there) and listed in Loaded Scripts; **remote debugging** over
ssh / WSL / Docker with `remote.command` + `pathMappings` (the adapter maps the
paths, so any DAP client can use it), or with VS Code's Remote extensions;
**JetBrains IDEs** through the LSP4IJ plugin (docs/debugging.md, not yet tried
in an IDE). A breakpoint binds to the first line, from
its own on, where a form starts, and stops when a frame enters that line
(every iteration of a loop; the line of a `(defn f [x]` stops at each call of
`f`).

**Limitations.** A thread blocked in native code (a `deref`, I/O) shows no
stack until it returns to the script; no "stop all threads" mode; no data or
function breakpoints, no `setVariable`; forms a macro generates have no lines; the
Variables view does not realise lazy seqs; the script's standard input is
empty under the debugger; stopping the session ends the script. Details in
[docs/debugging.md](docs/debugging.md).

## Documentation

| Document | What it covers |
|---|---|
| [docs/debugging.md](docs/debugging.md) | Debugging scripts in VS Code (`protoclj --dap`): installing, breakpoints, stepping, limitations. |
| [docs/LANGUAGE.md](docs/LANGUAGE.md) | The language reference — reader, evaluator, special forms, primitives. |
| [docs/STATUS.md](docs/STATUS.md) | Live status — what works, what departs from Clojure-JVM, what is on the list. |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Planned milestones, versioning, and how to read the plan. |
| [docs/INTEROP.md](docs/INTEROP.md) | The UMD interop design — pulling a Python or JavaScript module into a protoClojure namespace. |
| [docs/DESIGN.md](docs/DESIGN.md) | The architectural overview — reader, compiler, BytecodeModule, VM, primitives. |
| [docs/VM.md](docs/VM.md) | The bytecode VM for contributors — where to start reading the source, every opcode, worked examples from `protoclj --disasm`, how to add an opcode or a primitive. |
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
| protoIO | Shared input and output for the runtimes: files, processes, TCP, UDP, TLS and HTTP/1.1 (used by protoST, protoScala and protoClojure) | https://github.com/gamarino/protoIO |

## Why "protoClojure"?

`proto` — built on protoCore. `Clojure` — the language idiom being modelled. The name signals what it is and where it lives in the family. "Clojure-on-protoCore" is avoided because the goal is not a port of Clojure-JVM; it is a Clojure-flavoured dialect that earns its place by what protoCore can do.

## The Swarm of One

protoClojure is designed and maintained by a single architect, Gustavo Marino, working with AI coding agents that draft code, tests and documentation under human review.

The work proceeds as a measured loop — design, implement, compile, measure, keep or discard, commit — and the repository keeps the evidence: the conformance suite grew with the implementation to 274 fixtures, performance changes record their `perf stat` measurements in the commit history, and every benchmark figure in this README can be reproduced with the scripts in [`benchmarks/`](benchmarks/).

## License

Copyright (c) 2026 Gustavo Marino. Released under the MIT License; see [LICENSE](LICENSE).

protoCore, protoJS, protoPython, protoST and protoCpp are also released under the MIT License.
