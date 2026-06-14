# protoClojure — Roadmap

> Time-boxed plan. Estimates assume single-person part-time work on a
> familiar substrate (protoCore). They will change.

---

## Phase 0 — Design (now)

**Goal:** the documents in `docs/` describe a runtime that, if
implemented, a Clojure programmer would recognise as Clojure and a
Python / JS programmer would understand without prior Lisp exposure.

**Done when:**
- `LANGUAGE.md`, `INTEROP.md`, `DESIGN.md`, `STATUS.md`, `ROADMAP.md`
  written.
- Tutorial chapter 02 (for Python/JS devs) and 03 (for Clojure devs)
  drafted to the point where a reader can disagree productively.
- A bot-or-friend code review of the docs finds no obvious gaps.

**Deliverable:** this repository's `main` branch ready to share with
two or three trusted reviewers under NDA-equivalent informal terms.
No public announcement.

---

## Phase 1 — Bootstrap interpreter (4 weeks part-time)

**Goal:** `(println "hello world")` works, run from a file and from a
basic interactive REPL.

### Week 1 — Reader and minimum eval
- C++ reader for: numbers, strings, symbols, keywords, booleans, nil,
  list, vector, map, set. Quote and reader macros to follow in week 2.
- Const-pool / bytecode module skeleton (lifted directly from protoST).
- Special forms: `def`, `if`, `do`, `quote`.
- Just enough core: `+`, `-`, `*`, `=`, `<`, `println`, `str`.
- A file runner: `protoclj some.clj` reads, compiles, runs.

### Week 2 — Functions, let, recur
- `fn*`, `let*`, `loop*`, `recur`. Closures.
- Bytecode for call / push-local / push-const / branch / return.
- Quote forms and reader macros (`'`, `` ` ``, `~`, `~@`, `#(...)`).
- The persistent collection constructors (vector / map / set
  literals call into the protoCore primitives).

### Week 3 — Namespaces and the Clojure-path provider
- `ns` form + the `ns-symbol-resolver` chain.
- Clojure path resolver (load `.clj` files from `CLOJURE_PATH`).
- `:require :as :refer` bindings into the current namespace.
- Vars: `def` interns, var lookup goes through the namespace
  attribute table.
- The minimal `clojure.core` namespace as a `.clj` file shipped with
  the runtime.

### Week 4 — Errors, the REPL, the first demo
- `try / catch / finally`, `throw`, `ex-info`.
- An interactive prompt: read-eval-print loop on stdin.
- A first demo script that exercises every feature so far.
- Performance smoke benchmark — **primary baseline JVM Clojure**,
  secondary Babashka. The yardstick is the four-axis tuple
  (startup, single-thread CPU, multi-core, RSS), not a single
  number. See `docs/superpowers/specs/2026-06-14-phase-1-bootstrap-interpreter.md`
  §11 for the full framework.

**Done when:** the 10-line factorial / fizzbuzz / word-count examples
from the README work, run from file, and at the REPL.

**Decision point:** if single-thread steady-state is *catastrophic*
(>10× JVM Clojure OR >150ms startup), pause and profile before
continuing. If within 5× JVM single-thread AND winning on the
other three axes, continue — the target is met and "success" for
v0.1 is on the table.

---

## Phase 2 — Persistent collections at full strength (2 weeks)

**Goal:** every collection operation a Clojure programmer expects
works and is correct, even if not yet fast.

- Lazy sequences (`lazy-seq`, `map`, `filter`, `take`, `drop`, `range`,
  `iterate`, `repeat`, `cycle`, `concat`, `mapcat`, `partition`,
  `partition-all`, `interleave`, `interpose`).
- Higher-order: `reduce`, `apply`, `comp`, `partial`, `juxt`,
  `complement`, `every-pred`, `some-fn`.
- Map ops: `assoc-in`, `update-in`, `get-in`, `select-keys`, `merge`,
  `merge-with`, `zipmap`.
- Threading macros `->`, `->>`, `as->`, `some->`, `some->>`.
- `into`, with the variadic 2-arg, 3-arg, and (no transducer) 4-arg
  forms.
- `frequencies`, `group-by`, `sort`, `sort-by`, `distinct`.
- String ops: `str`, `format` (via host's printf-style), `subs`.

**Done when:** every example in the Clojure cheatsheet that doesn't
require JVM interop, transducers, or STM works.

---

## Phase 3 — Macros and the bootstrap (2 weeks)

**Goal:** macros defined in protoClojure work, including the
`clojure.core` macro vocabulary (`when`, `if-let`, `cond`, `case`,
`when-let`, `dotimes`, `for`, etc.).

- Compile-time evaluation pipeline: the compiler can call
  back into the (already-built) interpreter to expand a macro.
- GC root discipline during macro expansion (TransientPin around
  the unevaluated form, ProtoRootSet for held continuations).
- `defmacro`. Quasiquote handling that resolves symbols against the
  defining namespace.
- Move every `clojure.core` macro definable in protoClojure into
  `core.clj`. Keep only the absolute-minimum special-form-coupled
  macros in C++.

**Done when:** the `for` macro works correctly:

```clojure
(for [x [1 2 3] y [4 5]] [x y])
;; => ([1 4] [1 5] [2 4] [2 5] [3 4] [3 5])
```

---

## Phase 4 — UMD interop providers (2 weeks)

**Goal:** the `py/`, `js/`, `pst/`, and `clj/` providers all work and
the worked example in `INTEROP.md` §10 runs.

- The provider registry wired in at runtime startup.
- The `py/` provider delegating to protoPython's import machinery.
- The `js/` provider delegating to protoJS's `require`.
- The `pst/` provider delegating to protoST's `Import`.
- Conversion helpers `clj->py / py->clj / clj->js / js->clj / clj->pst / pst->clj`.
- Function metadata bridge (`meta` on a foreign function returns the
  right `:arglists`).

**Done when:** the tri-runtime demo script in `INTEROP.md` §10 runs
end-to-end and produces a chart.

---

## Phase 5 — nREPL (2-3 weeks)

**Goal:** CIDER / Calva / Conjure can connect and edit a live protoClojure
program.

- bencode encoder / decoder in C++.
- TCP server with session multiplexing.
- Operations: `eval`, `interrupt`, `clone`, `close`, `describe`,
  `load-file`.
- Per-session bindings stack.
- A `protoclj --nrepl PORT` flag.
- Integration test with a real CIDER session (manual at first; the
  CI test using `cider-nrepl` operations is v0.x).

**Done when:** the documented CIDER quickstart works on a fresh
workstation in under 5 minutes.

---

## Phase 6 — Quality pass and v0.1 release (1-2 weeks)

- Bench against **JVM Clojure** (primary) and Babashka (secondary)
  across the four axes (startup, single-thread CPU, multi-core
  parallel, RSS). Workloads include fib(N), word-count, JSON
  parse-and-walk, a CPU-bound `pmap` over 4-8 cores (this is where
  protoClojure has a structural advantage worth showing), and a
  long-running daemon to measure RSS footprint. Publish honest
  multi-dimensional results — even when unflattering on the
  single-thread axis. The framing is the tuple, never a single
  number; see `docs/superpowers/specs/2026-06-14-phase-1-bootstrap-interpreter.md`
  §11 for the full rationale.
- Documentation polish: every example in `LANGUAGE.md` and the tutorial
  must run.
- Examples directory: 8-10 idiomatic scripts of increasing complexity.
- `protoclj --version` output, `--help` output, error message review.
- Tag `v0.1.0`, announce in the order:
  1. Trusted reviewers (private)
  2. Clojurians Slack #announce (no Hacker News)
  3. The cross-runtime demo as a separate post

**Done when:** a Clojure programmer can read the README, install
protoclj, work through chapter 4 of the tutorial, and report back
that it felt like Clojure.

---

## v0.2 — chunked seqs, transducers, STM, defrecord, BigDecimal

Once v0.1 has been seen by ~50 users and a backlog of feedback exists,
v0.2 focuses on the *performance and richness* layer:

- Chunked sequences (32-element chunks transparently).
- Transducers (the no-coll arity of `map`, `filter`, etc.).
- Refs + STM. Implementation per `DESIGN.md` §6.
- `defrecord`, `deftype`.
- `BigDecimal` `M` suffix.
- `clojure.string`, `clojure.set`, `clojure.walk`, `clojure.edn`.

## v0.3 — agents, the actor / CSP layer, broader stdlib

- Agents (`agent`, `send`, `send-off`, `await`).
- The protoCore actor primitives surfaced for direct use.
- A first pass at the CSP-like API (not necessarily `core.async`
  surface).
- `clojure.pprint`, `clojure.zip`.

## v0.4+ — open

Driven by what the community asks for. Possible directions:

- ClojureScript-style CLJS transpilation TO protoJS (huge — a full
  separate project).
- Spec or Schema-like data validation.
- Browser embedding once protoJS-in-browser is real.
