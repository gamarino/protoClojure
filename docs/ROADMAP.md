# protoClojure — Roadmap

> Planned work, grouped into milestones. The order between milestones
> reflects dependencies; no dates are estimated. What already works is
> tracked in [STATUS.md](STATUS.md); shipped changes are listed in
> [CHANGELOG.md](../CHANGELOG.md).

---

## Versioning

protoClojure has not been released. The version declared in
`CMakeLists.txt` (and printed by `protoclj --version`) is **0.0.1**, and the
repository has no release tags. **v0.1.0** is the first planned release;
the milestones under [Towards v0.1](#towards-v01) define its scope. Version
labels such as v0.2 and v0.3 in this document and in the design documents
describe intended scope, not scheduled dates.

---

## Completed

### Bootstrap interpreter

The language reads, the VM runs, and the conformance suite is real:
lexer, reader, bytecode compiler and VM; `def`, `if`, `do`, `quote`;
`fn`, `defn`, `let`, `loop`, `recur`; closures with N-level lexical
capture; variadic parameters, `apply`, `map` / `filter` / `reduce`;
multi-arity functions; `cond` / `when` / `and` / `or`; keywords;
IEEE-754 floats; vectors distinct from lists.

### Benchmark and performance pass

- A benchmark harness against Babashka 1.4.192 (`benchmarks/bench.sh`).
- SmallInteger fast-path opcodes (`ADD SUB MUL LT LE GT GE EQ`), with the
  slow path routed through protoCore's promoting `add` / `compare` /
  `multiply`: `fib(30)` went from 4113 ms to 720 ms, and LargeInteger
  results are the default (`factorial(100)` runs out of the box).
- Separate `fnSingleProto` / `fnMultiProto` function prototypes, and the
  captures attribute read only when a body has captures: `fib(30)` went
  from 720 ms to 620 ms, with 15% fewer instructions and 32% fewer L1-d
  cache misses (per `perf stat`).

The features added afterwards raised `fib(30)` to 712 ms; see
[benchmarks/RESULTS.md](../benchmarks/RESULTS.md) and the performance
follow-ups below.

### Named arguments, maps and strings

- Map literals `{:a 1 :b 2}` and `hash-map` / `assoc` / `get` /
  `contains?` / `keys` / `vals` / `map?`.
- Named-argument destructuring `& {:keys [...] :or {...} :as name}` and
  trailing keyword/value pairs at call sites via the `CALL_KW` opcode.
  Both the positional and the named half of the protoCore call convention
  now work for functions defined in protoClojure; protoST implements the
  same convention with its own call syntax.
- `clojure.string`-shaped functions in the global namespace (`subs`,
  `upper-case`, `lower-case`, `join`, `split`, `replace`, `trim`, ...).

### Concurrency primitives

- `atom`, `swap!`, `reset!`, `compare-and-set!`, `deref` / `@` on protoCore
  compare-and-set, with `add-watch` / `remove-watch`.
- `future` on OS threads, `promise` / `deliver`, `realized?`, and `pmap`
  running one thread per element.
- Actors on a worker pool, with three priority bands, a single-method
  invariant and a lock-free per-actor mailbox.

### Local REPL and packaging

- Interactive `protoclj` REPL on libreadline: multi-line input, history,
  `*1` `*2` `*3`, `:help` / `:quit` / `:load` / `:time`.
- CPack packaging (DEB, RPM, TGZ, DragNDrop, NSIS, ZIP); TGZ and DEB are
  verified on Linux.

---

## Towards v0.1

### Bootstrap `core.clj` and macros

**Goal:** stop installing primitives in C++ and start composing them in
Clojure. Macros become user-writable.

- A minimal `core.clj` shipped with the runtime; `protoclj` evaluates it
  before any user code.
- Re-implement `every?`, `some?`, `comp`, `partial`, `juxt`, the threading
  macros (`->`, `->>`, `as->`, `some->`, `some->>`), `case`, `if-let`,
  `when-let`, `dotimes`, etc., in Clojure on top of the primitives.
- Compile-time evaluation pipeline: the compiler invokes the existing
  ExecutionEngine to expand a macro form.
- `defmacro`, the quote reader macro `'`, and quasiquote (`` ` ``, `~`, `~@`).
- Move every `clojure.core` macro definable in protoClojure into
  `core.clj`. Keep only special-form-coupled macros in C++.

### Exceptions

- `try`, `catch`, `finally`, `throw`.
- `ex-info`, `ex-data`, `ex-message`.
- Stack unwinding via protoCore exceptions.

### Collections and sequences

- Sets `#{...}` with `conj` / `disj`.
- `dissoc`, `update`, `merge`, `select-keys`, `get-in`, `assoc-in`,
  `update-in`, `merge-with`, `zipmap`.
- Lazy sequences and the sequence library (`range`, `iterate`, `take`,
  `drop`, ...).
- `format` (printf-style via the host) and characters as 1-codepoint
  strings (D3).

### nREPL server

**Goal:** CIDER / Calva / Conjure can connect and edit a live protoClojure
program. The nREPL server is a v0.1 requirement (see
[DESIGN.md §9](DESIGN.md#9-the-repl)).

- bencode encoder / decoder.
- TCP server with session multiplexing.
- Operations: `eval`, `interrupt`, `clone`, `close`, `describe`,
  `load-file`.
- A `protoclj --nrepl PORT` flag.

### UMD providers

**Goal:** `(:require [py/numpy :as np])` works, and the result is a
real protoCore object reachable from Clojure code with the full call
convention.

- `ns`, `:require`, `:as`, `:refer`; the provider-registration hook in the
  runtime; the unprefixed Clojure-path resolver that loads `.clj` files.
- `py/` provider (protoPython bridge), delegating to protoPython's import
  machinery, with the foreign-dispatch protocols (`ICounted`, `ISeqable`,
  `IIndexed`, `ILookup`, `IAssociative`, `ICollection`) extended at
  provider-init time, per the archived
  [foreign-dispatch design](archive/design-specs/2026-06-14-foreign-dispatch.md).
- `js/` provider (protoJS bridge) and `pst/` provider (protoST bridge), with
  the same shape as `py/`.
- Conversion helpers `clj->py / py->clj / clj->js / js->clj / clj->pst /
  pst->clj`, and `(meta foreign-fn) → {:arglists ...}` so `:keys`
  destructuring works symmetrically across runtimes.

### Quality pass and v0.1 release

**Goal:** publishable. The benchmark is honest, the docs run, the
examples work, the tutorial holds up.

- Benchmark against **JVM Clojure** (not measured so far) and Babashka
  across four axes: startup, single-thread CPU, multi-core parallelism,
  resident memory. Workloads: fib(N), word-count, JSON parse-and-walk, a
  CPU-bound `pmap` over 4-8 cores, and a long-running daemon for resident
  memory. Publish multi-dimensional results, including unflattering ones.
- Documentation polish: every example in `LANGUAGE.md` and the tutorial
  must run.
- Examples directory: idiomatic scripts of increasing complexity (twelve
  today).
- `protoclj --version` / `--help` output review; error-message review.
- Tag `v0.1.0`.

**Done when:** a Clojure programmer can read the README, install
`protoclj`, work through the tutorial, and find that it behaves like
Clojure.

### Performance follow-ups

- Collapse the parameters threaded through the VM's `run()` into a single
  structure, to recover the per-call cost added with maps, strings and the
  concurrency primitives.
- Size `pmap` parallelism with a thread pool instead of one thread per
  element.
- Replace the global actor ready-queue mutex, which limits the
  multi-producer, multi-consumer case.
- Wait for promise delivery without polling.
- Threaded dispatch (computed goto) in the VM and a reusable
  `ProtoContext` pool for call dispatch, if `perf stat` shows they are
  worth the complexity.

---

## v0.2 and beyond

Once v0.1 has users and a backlog of feedback exists:

### v0.2 — performance and richness
- Chunked sequences (32-element chunks transparently).
- Transducers (the no-coll arity of `map`, `filter`, etc.).
- Refs + STM. (Implementation per `DESIGN.md` §6.)
- `defrecord`, `deftype`.
- `BigDecimal` `M` suffix.

### v0.3 — bigger concurrency story
- The Clojure-JVM `agent` surface (`agent`, `send-off`, `await`,
  `await-for`) on top of the actor scheduler.
- Reactive primitives (signals, watches with batching).
- Distributed actors across machines (the kernel work is shared with
  protoST).

### v0.4+ — open
Driven by what users ask for. Possible directions:
- ClojureScript-style transpilation to protoJS (a separate project in
  its own right).
- Spec or Schema-like data validation.
- Browser embedding once protoJS runs in the browser.
- A typed surface (deftype-like ad-hoc types with verification).

---

## How to read this roadmap

- Language changes ship with conformance fixtures and a commit on `main`;
  the commit message documents the rationale and, for performance work,
  the measurements.
- The order of items inside a milestone is flexible; the order between
  milestones is not — each builds on the previous one's surface.
- The roadmap is allowed to be wrong. When implementation reveals a wrong
  assumption, the plan changes and this file is rewritten.
