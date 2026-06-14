# protoClojure — Architectural Design

> **Purpose of this document.** Explain the design decisions that the
> language reference (`LANGUAGE.md`) takes for granted. Useful before
> writing code, useful for understanding *why* a feature is the way it is
> when the reference only documents *what* it does.

---

## 1. The substrate

protoClojure compiles to bytecode for a tree-walking-then-stack-based
interpreter that runs on top of the **protoCore object kernel** —
the same C++20 kernel that hosts protoJS, protoPython, and protoST. The
kernel gives us, for free, every property that Clojure usually has to fight
the host runtime for:

- **Immutability by default**: every protoCore object is immutable unless
  explicitly created mutable. A protoClojure `(assoc m :k v)` returns a new
  map via the kernel's structural-sharing `setAttribute` path.
- **Persistent collections**: protoCore ships AVL trees (`ProtoList`),
  hash-array-mapped tries (`ProtoSparseList`), ropes (`ProtoString`), and
  multisets (`ProtoMultiset`). These map onto Clojure's `PersistentVector`,
  `PersistentHashMap`, persistent strings, and `PersistentHashSet` with
  almost no adapter layer.
- **Compare-and-swap on attributes**: protoCore's `setAttributeIfEqual`
  *is* the primitive that `atom` and `swap!` need. No JVM atomics, no
  retry loop in the language layer.
- **GIL-free concurrency**: per-thread allocation arenas, concurrent
  garbage collector, no global lock. Clojure's promise of "use all your
  cores" actually lands.
- **Tagged immediates**: `SmallInteger` (56-bit inline) and `Float` are
  stored in the pointer. Arithmetic on small ints does not allocate.

The implementation cost we therefore pay is *interpretation*, *the reader*,
*the standard library*, *the REPL*, and *the macro system* — but **not**
the data model, the memory model, or concurrency.

## 2. The hybrid module system — UMD as the invisible motor

This is the most consequential architectural decision in the language.

**The surface form is Clojure.** The Clojure programmer writes:

```clojure
(ns my.app
  (:require [clojure.string :as str]
            [py/numpy :as np]
            [js/d3 :as d3]
            [my.util :refer [fmt]]))
```

**The plumbing is UMD.** Every `(:require ...)` resolves through the same
universal module-discovery chain that protoPython, protoJS, and protoST
share. The `py/` prefix says "resolve this module against the protoPython
side of the UMD registry"; `js/` says "resolve against protoJS"; an
unprefixed namespace resolves through the standard Clojure path (the local
filesystem and any registered Clojure module sources).

The Clojure programmer sees no extra system — `:require` is still
`:require`. The protoCore programmer sees no extra system either — the UMD
registry has one more language consumer.

### 2.1 Why a hybrid is the right call

The other two designs were considered and rejected:

- **"Pure Clojure namespaces, UMD only for cross-lang."** Would force the
  programmer to learn a second system for cross-runtime calls. The whole
  reason to choose protoClojure over JVM Clojure is the interop; making
  the interop feel like a second-class citizen defeats the value.
- **"UMD for everything, no Clojure namespaces."** Would offend the
  Clojure programmer's idiom expectation immediately — `:require`,
  `:as`, `:refer` are deep muscle memory.

The hybrid resolves it: Clojure syntax on top, one resolver underneath.

### 2.2 What this requires from us

- A reader extension recognising the `lang/module` prefix syntax. The
  Clojure reader accepts namespaced symbols already; we use that
  pre-existing rule.
- The UMD registry must return modules in a form that protoClojure can
  dispatch against — that already works (a foreign UMD module is a
  protoCore object with attributes; method invocation is the standard
  attribute walk that protoST and protoJS already use).
- A clear naming convention: `py/X`, `js/X`, `pst/X` (for protoST),
  `clj/X` (when we want to be explicit), unprefixed defaults to Clojure.

### 2.3 What this gives back to the rest of the ecosystem

A protoClojure module is itself a UMD module. A protoPython script can
`import` a `.clj` file and call its functions exactly as it imports a
`.py`. The substrate is symmetric — every language is a producer and a
consumer.

## 3. The reader

A standard Clojure reader, in C++. The lexer produces tokens for:

- Symbols, keywords, numbers, strings, chars, booleans, nil.
- Open/close paren, bracket, brace.
- Quote `'`, quasiquote `` ` ``, unquote `~`, splice `~@`.
- Reader macros: `#{}` (set), `#()` (anonymous fn), `#""` (regex — TBD),
  `#'` (var quote), `#_` (discard).
- Metadata `^{...}` and the shorthand `^kw` / `^Type`.

Every reader output is a protoCore object — a list is a `ProtoList`, a
vector is a `ProtoList` tagged as vector, a map is a `ProtoSparseList`
under a small mutable wrapper. Symbols are interned `ProtoString`s.

The reader is the place where "code is data" becomes literal: the result
of `(read-string "(+ 1 2)")` is a real `ProtoList` of three elements that
the same code path can evaluate.

## 4. The compiler / evaluator

A single-pass compiler that lowers reader output to bytecode for a small
stack VM. The VM is conceptually the same shape as protoST's
ExecutionEngine — frames with operand stack and locals, opcodes for
push / pop / lookup / call / branch / closure-make / try-catch — with
Clojure-specific opcodes for:

- **`recur`**: a tail-only opcode that jumps to the enclosing `loop` /
  `fn` recur point without growing the C++ call stack. Necessary because
  protoCore (rightly) does not assume TCO at the host C++ level.
- **`var-get` / `var-set`**: a Clojure `def` produces a *var* — a mutable
  protoCore object whose `__value__` attribute holds the bound value.
  Var lookup is a fast attribute read on the namespace object.
- **Anonymous function creation** as a closure-over-locals — same
  mechanism as protoST blocks and protoPython lambdas.

The compiler does NOT do whole-program analysis. Each top-level form
compiles independently — the REPL relies on this, and the JVM Clojure
does it the same way.

## 5. Namespaces and vars

A namespace is a protoCore object. Its attributes are vars (or aliases to
vars). A var is a small mutable protoCore object with `__value__` and
optionally `__meta__`. `def` interns a symbol into the current namespace
and points the var at the value.

This matches the JVM Clojure semantics exactly:

- A var is a mutable indirection. Code references the var, not the value
  directly, so redefining a function in the REPL updates every caller
  without re-compilation.
- A namespace is just a map from symbol to var, with some metadata
  (aliases, refer imports). It IS the map; an `intern` is a `setAttribute`.

We get this for free from protoCore's mutable-object model with CAS.

## 6. Atoms, refs, and STM

**Atoms** are a one-liner. A `clojure.core/atom` is a mutable protoCore
object with `__value__`. `swap!` is a CAS loop using
`setAttributeIfEqual`. `reset!` is a plain `setAttribute`. `deref` /
`@` is an attribute read. The exact same primitive protoST exposed in its
`Atom` class.

**Refs and STM** are in scope for v0.2, not v0.1. The implementation path
is: each ref carries a (value, version) pair; a transaction collects a
read-set and a write-set; commit is a multi-CAS over `setAttributeIfEqual`
on every (ref, expected-version) pair. The protoCore per-cycle
mutable-shard snapshot in the GC (`335ef608`) is suggestive of how the
read-set can be made consistent across allocation cycles. This is non-trivial
but mechanical. v0.2.

**Agents** are deferred to v0.3. protoCore's actor primitives are the
natural substrate, but the Clojure `send` / `send-off` API has scheduling
semantics we want to align with the actor / task model thoughtfully.

## 7. Lazy sequences

Lazy seqs are thunks with memoised first / rest. A `LazySeq` is a small
protoCore object with `__thunk__` (the unrealised computation),
`__realized__` (a boolean), and `__first__` / `__rest__` (filled on
first deref). `lazy-seq` macro expands to a constructor; `seq`, `first`,
`rest`, `next` are the access path.

The interaction with protoCore's GC is straightforward: a lazy seq node
is just another object; the GC traces the rest pointer; an unrealised
thunk holds its captured locals through a normal closure.

The one subtlety is **chunked seqs**. JVM Clojure realises 32 elements at
a time for performance. v0.1 will skip chunking — element-at-a-time — and
add it in v0.2 if benchmarks demand it. (The whole point of "idiom over
performance" is that idiom-visible behaviour stays the same; chunking is
invisible until you watch closely.)

## 8. Protocols and multimethods

**Protocols** map onto prototype attributes. A `defprotocol` declares a
set of method names. `extend-type` adds attributes (the methods) onto the
class object. Dispatch is a normal attribute read on the receiver — the
same `getAttribute` chain walk every other language on protoCore uses.
This is *one of the places where protoClojure is closer to its substrate
than JVM Clojure* — JVM Clojure synthesises an interface; here we just
set attributes.

**Multimethods** are a *separate* dispatch system, by design. A
`defmulti` creates a multimethod object holding (dispatch-fn,
method-table). `defmethod` adds an entry to the table. Calling the
multimethod evaluates the dispatch function on the args, looks up the
table (with hierarchy fallback via `isa?` and `derive`), and invokes.
The method table is a `ProtoSparseList`; the hierarchy is a small graph
of derive relationships, also stored as protoCore attributes.

Multimethods are the place where protoClojure is *farther* from its
substrate than the rest. That's fine — Clojure's whole multimethod design
is a deliberate departure from class-based dispatch. The implementation
is mechanical.

## 9. The REPL

A respected REPL is a v0.1 hard requirement. Two layers:

- **The reader-eval-print loop itself**: read one form, eval, print,
  loop. Trivial.
- **nREPL server**: a TCP server speaking the [nREPL
  protocol](https://nrepl.org/) so CIDER / Calva / Conjure can connect.
  The protocol is bencode over a socket; the operations a v0.1 needs to
  support are `eval`, `load-file`, `interrupt`, `describe`, `clone`,
  `close`. Pretty-printing, completion (`complete`), and lookup
  (`info`) are stretch but high-value.

The implementation cost of nREPL is the highest in the v0.1 plan — bencode
parser, an op dispatcher, session management. We accept it because
without it the language fails the *respect* test.

## 10. Macros

Macros are functions that take and return reader output (data). The
compiler expands them at compile time: when it sees a symbol that resolves
to a macro var, it calls the macro with the unevaluated args, takes the
result, and compiles that.

The implementation requirement: macros must be *defined* in protoClojure
itself (most of `clojure.core` is macros — `when`, `if-let`, `cond`,
threading macros). That means the compiler must be able to evaluate
protoClojure code at compile time, which means the compiler must be
*itself* a protoClojure program from a certain point onward. The natural
sequencing is:

1. v0.1-a: compiler is C++ only. Bootstraps `clojure.core` without using
   macros it defines. The C++ side includes a small set of special forms
   and built-in macros (`fn`, `let`, `do`, `if`, `quote`, `def`).
2. v0.1-b: `clojure.core` macros are defined in protoClojure. The C++
   side can call into the just-compiled evaluator.
3. v0.2+: user macros work because user code runs through the same path.

This is the standard Lisp bootstrap. The complication is the protoCore
GC interaction during macro expansion — we have to be careful that the
unevaluated forms (which are data, but are protoCore objects) stay rooted
across calls into user code. The existing GC discipline patterns
(TransientPin, ProtoRootSet) carry over.

## 11. The standard library

Two phases:

- **v0.1: minimum viable `clojure.core`.** ~80 functions: arithmetic,
  comparison, predicates, collection primitives (`first`, `rest`,
  `cons`, `conj`, `assoc`, `dissoc`, `update`, `get`, `count`,
  `seq`, `into`), higher-order (`map`, `filter`, `reduce`, `apply`),
  control flow macros (`when`, `if-let`, `cond`, `case`, `->`,
  `->>`), string operations (delegating to `ProtoString`), `atom`,
  `swap!`, `reset!`, `deref`, plus a handful of utility namespaces.
- **v0.2+: `clojure.string`, `clojure.set`, `clojure.walk`,
  `clojure.edn`.** Mostly straightforward — each is a few hundred lines
  of Clojure-level code once `clojure.core` is solid.

What is *out of scope* (likely permanently):

- `clojure.java.*` — JVM-specific.
- `clojure.data.csv`, `clojure.data.json` — replaced by the Python or
  JS UMD equivalents.
- `clojure.tools.logging` — TBD; may bridge to a host logger.
- `core.async` — re-imagined on protoCore actors in a separate v0.3
  design pass, not lifted directly.

## 12. The interpreter as a Clojure program later

A long-term direction: once the C++ interpreter is solid, port chunks of
the compiler (macro-expansion, analysis passes) into protoClojure itself.
This is the natural Lisp endgame, and lines up with the protoCore
philosophy of moving runtime decisions into the language rather than the
kernel. Not a v0.1 concern — but the design should not preclude it.

## 13. Performance posture

Stated explicitly so future contributors know how to weigh tradeoffs:

> **Idiom over performance until users justify otherwise.**

A change that makes `(reduce + 0 [1 2 3 4 5])` 30% faster but turns
`reduce` into something a Clojure programmer would not recognise is
rejected. A change that makes the same expression 30% faster while the
external behaviour stays bit-identical is accepted. The yardstick for
"bit-identical" is the JVM Clojure reference behaviour, modulo the
documented departures.

Benchmarks live next to the source, run in CI once we have CI. The first
benchmark target is **startup time + simple-script throughput**, because
that is the comparison Clojure programmers will make first against
Babashka.

## 14. Out of scope (v1 line)

- No Java interop.
- No reader literals beyond the standard set (`#inst`, `#uuid` reserved
  but not implemented).
- No transducers in v0.1 — composition over `seq` is fine for first
  pass; transducers in v0.2.
- No `core.async` in v0.1.
- No record types (`defrecord`) in v0.1 — protocols suffice.
- No deftype in v0.1 — same.
- No ClojureScript-style cljs-ifying. This is one substrate.
- No `clojure.spec`. Reserved for a v0.x explicit design pass.

## 15. The risk register

Honest accounting of where this design could fail:

| Risk                                                  | Mitigation                                                                                    |
| ----------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| nREPL implementation cost is larger than the v0.1 budget | Cut scope of v0.1 stdlib first; nREPL is non-negotiable                                       |
| Macro bootstrap (§10) is harder than estimated         | Time-box phase v0.1-b; if it slips, ship v0.1 with the C++ built-in macro set and call it     |
| Persistent collection performance is much worse than JVM | Benchmark early; protoCore's structural sharing should be competitive; if not, profile and fix |
| The community reads "not 100% compatible" as "broken"  | Lead with the dialect framing in README, STATUS, and tutorial chapter 3                       |
| Clojure community is too small to materially help      | Accept it — the goal is prestige-by-association, not headcount                                |
