# protoClojure — Roadmap

> Session-by-session plan. Each session is roughly an afternoon. The
> swarm-of-one cadence (see README §"The Swarm of One") makes the unit
> of progress smaller and more honest than week-by-week milestones.

---

## Phase 0 — Bootstrap interpreter (sessions 1-9, CLOSED)

**Goal:** the language reads, the VM runs, the conformance suite is
real, and the implementation rivals Babashka semantically.

Done:
- Sessions 1-3: Lexer, reader, bytecode VM, `println`.
- Session 4: `def`, `if`, `do`, integer arithmetic, comparisons.
- Session 5: `fn`, `defn`, `let`, `loop`, `recur`.
- Session 6: Closures with N-level lexical capture.
- Session 7: Variadic `& rest`, `apply`, `map` / `filter` / `reduce`.
- Session 8: Multi-arity `defn`, `cond` / `when` / `and` / `or`, keywords.
- Session 9: IEEE-754 floats, vectors distinct from lists.

Snapshot: 90 conformance fixtures pass, the surface covers everything
in the tutorial's first four chapters.

---

## Phase 1 — Honest benchmark + perf pass (sessions 10-12, CLOSED)

**Goal:** measure against Babashka, identify the levers, ship the wins.

Done:
- Session 10: Babashka 1.4.192 installed locally; first bench harness;
  numbers are honest (and at the time, ~8× slower than bb on compute-
  bound).
- Session 11: SmallInt fast-path binary opcodes (`ADD SUB MUL LT LE GT GE EQ`),
  routed slow-path through protoCore's promoting `add` / `compare` /
  `multiply`. fib 4113 ms → 720 ms. LargeInteger by default
  (factorial(100) runs out of the box).
- Session 12: Split `fnMarkerProto` into `fnSingleProto` + `fnMultiProto`;
  skip captures read when body has none. fib 720 → 620 ms.
  `perf stat` confirmed: −15% instructions, −32% L1-d misses.

Snapshot: 93 conformance fixtures, 3 of 5 bench rows faster than bb,
1.07-1.25× bb on the call-heavy ones, in the 2-4× JVM Clojure band
on extrapolation — inside the "5× JVM Clojure single-thread" target.

---

## Phase 2 — UMD-ready call convention + collections (NEXT)

**Promoted to the front of the queue** following the realisation that
interop with foreign modules requires the **full** protoCore call
convention (positional + named), in BOTH directions (consume and
generate). Without this, the "transparent UMD interop" promise in the
README is partial: positional calls work today, named-arg calls do not,
so `(np/zeros [3 3] :dtype :float64)` is not yet reachable.

The protoST runtime already implements this on its side (see
`protoST/docs/superpowers/specs/2026-06-13-protocore-call-syntax.md` —
the `recv name(p1, k=v)` dual syntax). protoClojure's spec is
`docs/superpowers/specs/2026-06-14-protocore-call-convention.md`; this
phase implements its named-arg half.

### Session 13 — Named-argument support in `defn` and call sites

- Reader: map literal `{:a 1 :b 2}` (today's gap that blocks everything else here).
- Compiler: detect map-destructuring `& {:keys [...] :or {...}}` in fn params.
- Compiler: emit `CALL_KW` opcode (operand = const-pool index of a
  mangled selector with sorted named keys; same shape as protoST's
  `SEND_CALL`). Stack layout matches the protoST design: receiver,
  positionals in source order, named values in alphabetical-key order.
- VM: `CALL_KW` dispatch path. Reuses `dispatchCall` for the positional
  part; threads `kwArgs` (a `ProtoSparseList`) through to the receiver.
- Generate side: `defn` with `& {:keys}` produces a wrapper whose
  signature advertises the named-arg keys, so foreign callers
  (protoST, protoPython) reach the right argument by name.

### Session 14 — Maps as a first-class collection

- `ProtoSparseList`-backed maps; `assoc`, `dissoc`, `get`, `contains?`,
  `keys`, `vals`, `merge`, `update`, `get-in`, `assoc-in`, `update-in`.
- `select-keys`, `merge-with`, `zipmap`.
- Print path for maps.

### Session 15 — Strings as a first-class collection

- `clojure.string` ops: `count`, `upper-case`, `lower-case`, `split`,
  `join`, `subs`, `replace`, `trim`.
- `format` (printf-style via the host).
- Char support — strings as a sequence of 1-codepoint strings (D3).

**Phase 2 done when:** the worked example in `INTEROP.md` §10
type-checks against the language as written, even if the providers
aren't wired in yet.

---

## Phase 3 — Bootstrap `core.clj` + macros

**Goal:** stop installing primitives in C++ and start composing them in
Clojure. Macros become user-writable.

### Session 16 — `core.clj` evaluated at startup
- A minimal `core.clj` shipped with the runtime; `protoclj` evaluates it
  before any user code.
- Re-implementing `every?`, `some?`, `comp`, `partial`, `juxt`,
  threading macros (`->`, `->>`, `as->`, `some->`, `some->>`), `case`,
  `if-let`, `when-let`, `dotimes`, etc., in Clojure on top of the
  primitives.

### Session 17 — `defmacro` + compile-time evaluation
- Compile-time eval pipeline: compiler invokes the existing
  ExecutionEngine to expand a macro form.
- `defmacro` syntax + quasiquote (`` ` ``, `~`, `~@`).
- Move every `clojure.core` macro definable in protoClojure into
  `core.clj`. Keep only special-form-coupled macros in C++.

### Session 18 — Exceptions: `try`, `catch`, `finally`, `throw`
- `ex-info`, `ex-data`, `ex-message`.
- Stack-unwinding via protoCore exceptions.

---

## Phase 4 — REPL + nREPL

**Goal:** CIDER / Calva / Conjure can connect and edit a live protoClojure
program.

### Session 19 — Interactive `protoclj` REPL
- Read-eval-print loop on stdin; multi-line input; history.
- `*1` `*2` `*3` `*e` bindings.
- `(doc symbol)`, `(source symbol)`.

### Session 20-21 — nREPL server
- bencode encoder / decoder.
- TCP server with session multiplexing.
- Operations: `eval`, `interrupt`, `clone`, `close`, `describe`,
  `load-file`.
- A `protoclj --nrepl PORT` flag.

---

## Phase 5 — UMD providers

**Goal:** `(:require [py/numpy :as np])` works, and the result is a
real protoCore object reachable from Clojure code with the full call
convention (set up in Phase 2).

### Session 22 — Provider registry + `clj/` resolver
- The provider-registration hook in the runtime.
- The unprefixed Clojure-path resolver — loads `.clj` files.
- `:require :as :refer`.

### Session 23 — `py/` provider (protoPython bridge)
- Delegates to protoPython's import machinery.
- Foreign-dispatch protocols extended at provider-init time
  (`ICounted`, `ISeqable`, `IIndexed`, `ILookup`, `IAssociative`,
  `ICollection`), per
  `docs/superpowers/specs/2026-06-14-foreign-dispatch.md`.

### Session 24 — `js/` provider (protoJS bridge) + `pst/` (protoST)
- Same shape as `py/`. The protocol extensions are per-provider files
  in Clojure (~40-80 lines each).

### Session 25 — Conversion helpers
- `clj->py / py->clj / clj->js / js->clj / clj->pst / pst->clj`.
- `(meta foreign-fn) → {:arglists ...}` so `:keys` destructuring works
  symmetrically across runtimes.

---

## Phase 6 — Concurrency primitives

**Goal:** what protoCore already provides under the hood, exposed as
Clojure idiom.

### Session 26-27 — `atom`, `swap!`, `reset!`, `compare-and-set!`, `deref` / `@`
- Built on protoCore CAS.
- `add-watch`, `remove-watch`.

### Session 28 — `future`, `promise`, `deliver`, `realized?`
- Built on protoCore Future.

### Session 29 — `pmap` and parallel reduce
- Leveraging protoCore's worker pool (the same one protoST exposes for
  actors).

### Session 30+ — agents and the protoST-style actor model
- Track 9 of protoST's actor system surfaced through Clojure idiom.

---

## Phase 7 — Quality pass + v0.1 release

**Goal:** publishable. The benchmark is honest, the docs run, the
examples work, the tutorial holds up.

- Bench against **JVM Clojure** (primary, once the CLI is installed
  and a baseline is measured) and Babashka (secondary) across the
  four axes: startup, single-thread CPU, multi-core parallel, RSS.
  Workloads: fib(N), word-count, JSON parse-and-walk, a CPU-bound
  `pmap` over 4-8 cores (the structural advantage we should show),
  long-running daemon for RSS. Publish honest multi-dimensional
  results — even when unflattering on single-thread.
- Documentation polish: every example in `LANGUAGE.md` and the
  tutorial must run.
- Examples directory: 10-15 idiomatic scripts of increasing complexity.
- `protoclj --version` / `--help` output review; error-message review.
- Tag `v0.1.0`, announce in the order:
  1. Trusted reviewers (private).
  2. Clojurians Slack #announce (no Hacker News).
  3. The cross-runtime demo as a separate post.

**Done when:** a Clojure programmer can read the README, install
protoclj, work through the tutorial, and report back that it felt
like Clojure (not "felt like Clojure-ish").

---

## v0.2 and beyond

Once v0.1 has been seen by ~50 users and a real backlog of feedback
exists:

### v0.2 — performance and richness
- Chunked sequences (32-element chunks transparently).
- Transducers (the no-coll arity of `map`, `filter`, etc.).
- Refs + STM. (Implementation per `DESIGN.md` §6.)
- `defrecord`, `deftype`.
- `BigDecimal` `M` suffix.
- Threaded dispatch / computed-goto in the VM (if the benchmark says
  it's worth the complexity).
- Reused ProtoContext pool for hot-path call dispatch (if `perf stat`
  shows the frame setup is the bottleneck).

### v0.3 — bigger concurrency story
- Reactive primitives (signals, watches with batching).
- Distributed actors across machines (the kernel work is shared with
  protoST).

### v0.4+ — open
Driven by what the community asks for. Possible directions:
- ClojureScript-style CLJS transpilation TO protoJS (huge — a full
  separate project).
- Spec or Schema-like data validation.
- Browser embedding once protoJS-in-browser is real.
- A typed surface (deftype-like ad-hoc types with verification).

---

## How to read this roadmap

- "Session" = roughly an afternoon of focused work. The unit reflects
  the swarm-of-one cadence; estimates in weeks would be misleading.
- Sessions ship one logical change with conformance fixtures, a memory
  entry, and a commit on `main`. The commit message documents the
  rationale, the perf trajectory (when applicable), and the next
  candidates.
- The order between sessions inside a phase is flexible; the order
  between phases is not — each builds on the previous one's surface.
- The roadmap is allowed to be wrong. When a session reveals a wrong
  assumption (as session 11 → 12 did re: inline caches), the next
  session's plan moves accordingly and this file gets rewritten.
