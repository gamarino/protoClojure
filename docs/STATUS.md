# protoClojure — Status

> Living tracker. This file changes every time something is implemented,
> finished, or dropped. If a feature you expect is not listed as
> implemented here, it is not implemented.

**Current state.** Version 0.0.1, no tagged release. The interpreter runs
scripts and an interactive REPL. `ctest` registers 161 test cases: 130
conformance fixtures under `tests/conformance/` and 31 GoogleTest unit
tests for the lexer and reader (`tests/unit/`). Against the current
protoCore, 158 pass; the three failures are described under
[Known issues](#known-issues). Benchmark numbers against Babashka 1.4.192
are in [`benchmarks/RESULTS.md`](../benchmarks/RESULTS.md). Shipped changes
are listed in [`CHANGELOG.md`](../CHANGELOG.md).

---

## Feature coverage by conformance directory

Features in the order they were implemented, with the conformance
directories that cover them.

| Feature | Conformance directories | Fixtures |
|---|---|---:|
| Binary, lexer, reader, bytecode VM, `println` | `00-binary`, `01-literals` | 2 |
| `def`, `if`, `do`, integer arithmetic, comparisons, `str` | `02-special-forms`, `03-arithmetic` | 9 |
| `fn`, `defn`, `let`, `loop`, `recur` | `04-functions`, `05-recursion` | 6 |
| Closures with N-level lexical capture | `06-closures` | 6 |
| Variadic `& rest`, `apply`, list operations, `map` / `filter` / `reduce` | `07-variadic`, `08-collections`, `09-higher-order` | 12 |
| Multi-arity `defn`, `cond` / `when` / `and` / `or`, booleans, keywords | `10-multi-arity`, `11-sugar-forms`, `12-literals` | 14 |
| IEEE-754 floats, vectors distinct from lists | `13-floats`, `14-vectors` | 10 |
| LargeInteger promotion | `15-bigint` | 3 |
| Maps, `& {:keys [...]}` named-argument destructuring | `16-maps`, `17-kw-destructuring` | 10 |
| Trailing keyword/value pairs, `:or`, `:as` | `18-kw-callsite`, `19-or-and-as` | 10 |
| `clojure.string`-shaped string functions | `20-strings` | 13 |
| Atoms | `21-atoms` | 10 |
| Futures and `pmap` on OS threads | `22-futures` | 10 |
| Watches, promises | `23-watches`, `24-promises` | 7 |
| Actors | `25-actors` | 8 |
| Interactive REPL | — (no conformance fixtures) | — |

The design specifications written during development are archived under
[`docs/archive/design-specs/`](archive/design-specs/).

---

## Implemented and stable

### Reader

- [x] Integers (SmallInteger + auto-promoted LargeInteger)
- [x] Floats — `3.14`, `1e6`, `1.5e-3`
- [x] Strings — `"hello"`, with `\n \t \r \\ \" \0` escapes
- [x] Symbols (interned via the protoCore symbol table)
- [x] Keywords — `:foo`, self-evaluating constants
- [x] Booleans + nil — `true`, `false`, `nil` as compile-time literals
- [x] Lists `(...)`
- [x] Vectors `[...]`
- [x] Maps `{...}`
- [x] `@form` — read as `(deref form)`
- [x] Line comments `;`; commas as whitespace

### Special forms

- [x] `def`
- [x] `defn` (single-arity)
- [x] `defn` (multi-arity, with optional variadic catch-all)
- [x] `fn` (single + multi-arity); `(fn name [args] ...)` is accepted (see D18)
- [x] `let` — inside function bodies only
- [x] `loop` — inside function bodies only
- [x] `recur` — inside `loop` and at the top of a fn body (implicit recur target)
- [x] `if`
- [x] `do`
- [x] `quote` — of symbols, keywords and other atoms (list quoting is not implemented)
- [x] `apply` — two-argument shape `(apply f list)`
- [x] `when`, `when-not`
- [x] `cond` (with `:else` / `else`)
- [x] `and`, `or` (short-circuit via DUP + JUMP_IF_TRUE/FALSE)
- [x] `future` — `(future body...)` compiles to `(make-future (fn [] body...))`
- [x] Variadic `& rest` in fn params
- [x] Named-argument destructuring `& {:keys [...] :or {...} :as name}`

### Data structures

- [x] Lists — protoCore `ProtoList`
- [x] Vectors — protoCore `ProtoTuple` (O(log N) `nth`)
- [x] Maps — protoCore `ProtoSparseList` keyed by `key->getHash`, with
      `hash-map` / `assoc` / `get` / `contains?` / `keys` / `vals` / `map?`
- [x] Strings — protoCore `ProtoString`
- [x] Integers — `SmallInteger` tagged + auto-promoted `LargeInteger`
- [x] Floats — `ProtoObject::fromDouble`

### Numeric semantics

- [x] SmallInteger fast-path arithmetic via opcodes (`ADD SUB MUL LT LE GT GE EQ`)
- [x] Automatic promotion to LargeInteger on overflow
- [x] Automatic promotion to double when any operand is a float
- [x] Integer division truncates: `(/ 10 4)` is `2`
- [x] Print path handles SmallInteger, LargeInteger and floats

### Closures

- [x] N-level lexical capture across nested `fn` bodies
- [x] Captures cascade — intermediate scopes create capture slots automatically
- [x] First-class fns — `((make-mul k) x)` callable head, passed around freely

### Primitives installed at startup (74)

- [x] Arithmetic and comparison: `+ - * / inc dec < <= > >= =`
- [x] Output: `println str`
- [x] Lists and vectors: `list vector vec nth first rest cons count empty? reverse`
- [x] Higher-order: `map filter reduce pmap`
- [x] Predicates: `nil? not vector? list? map? string?`
- [x] Maps: `hash-map assoc get contains? keys vals`
- [x] Strings: `subs upper-case lower-case starts-with? ends-with?
      includes? index-of replace join split trim triml trimr blank?`;
      `count` / `empty?` / `reverse` also accept strings
- [x] Atoms and watches: `atom atom? deref reset! swap! compare-and-set!
      add-watch remove-watch`
- [x] Futures and promises: `make-future future? realized? promise
      promise? deliver`
- [x] Actors: `actor actor? send send-h send-m send-l actor-stats`

### Bytecode VM

- [x] 29 opcodes — see `src/runtime/Opcodes.h`
- [x] `MAKE_FN` / `MAKE_FN_MULTI` — single + multi-arity wrappers
- [x] `CALL_APPLY` — spread-arguments dispatch
- [x] `CALL_KW` — trailing keyword/value pairs; the map is kept for
      keyword-based callees and expanded back into positional pairs otherwise
- [x] Split fn prototypes (`fnSingleProto` / `fnMultiProto`) for one attribute lookup per single-arity call
- [x] SmallInteger fast-path binary opcodes
- [x] DUP + JUMP_IF_TRUE for short-circuit `and` / `or`

### Concurrency

- [x] `atom`, `swap!`, `reset!`, `compare-and-set!`, `deref` / `@`, `atom?`
- [x] `add-watch`, `remove-watch` — callbacks run as `(f key atom old new)`
      after a successful update; exceptions thrown by a watch are swallowed
- [x] `future`, `future?`, `realized?` on OS threads; future threads are
      joined before the runtime shuts down
- [x] `promise`, `deliver`, `promise?`; `deref` blocks on a pending future
      or promise
- [x] `pmap` runs each element on its own OS thread; `(pmap fib (list 30 30 30 30))`
      measured a 3.25× wall-clock speedup over `map` on 2026-06-14
      ([`benchmarks/RESULTS.md`](../benchmarks/RESULTS.md))
- [x] **Actors** (`actor`, `send`, `send-h` / `send-m` / `send-l`, `actor?`,
      `actor-stats`) on a configurable worker pool (`PROTOCLJ_ACTOR_WORKERS`,
      default `max(2, cores-2)`, cap 16). Three priority bands, single-method
      invariant, lock-free per-actor mailbox. Peak throughput with a trivial
      body, 1,000,000 messages per mode: 215,100 msg/s (single), 218,341
      (fan-out), 171,851 (MPSC), 125,424 (MPMC) — see the README benchmark
      section.

### REPL

- [x] Interactive `protoclj` REPL on libreadline (history persisted to
      `~/.protoclj_history`, multi-line continuation prompt `#_=>`,
      `:help` / `:quit` / `:load` / `:time` meta-commands)
- [x] `*1` `*2` `*3` bindings to the last three evaluation results

### Tooling

- [x] `protoclj <script.clj>` — single-file evaluation
- [x] `protoclj` with no arguments — interactive REPL
- [x] `protoclj --version` / `--help` / `-h`
- [x] Conformance suite under `tests/conformance/` — discovered by glob
- [x] Bench harnesses `benchmarks/bench.sh` (vs Babashka) and `benchmarks/actor-bench.sh`
- [x] CPack packaging — TGZ and DEB verified on Linux; RPM, DragNDrop, NSIS
      and ZIP configured but unverified

---

## NOT yet implemented (tracked)

These are features that the language reference, the tutorial or the
examples describe, but the implementation does not support. Using them
raises a read, compile or runtime error.

### Reader (not yet)

- [ ] Quote `'form` (the `(quote atom)` special form works)
- [ ] Sets `#{...}`
- [ ] Quasiquote `` ` ``, unquote `~`, splice `~@`
- [ ] Anonymous-fn shorthand `#(...)`
- [ ] Var-quote `#'`
- [ ] Discard `#_`
- [ ] Metadata `^{...}` plus shorthands `^kw` `^Type`
- [ ] Reader literals `#inst` `#uuid`, regex `#"..."`
- [ ] Ratio literal `1/3`
- [ ] Character literal `\a`
- [ ] Namespace-qualified symbols / keywords `foo/bar`, `:ns/kw`

### Special forms (not yet)

- [ ] `let` and `loop` at the top level of a file (only inside function bodies today)
- [ ] Vector and map destructuring in `let` and in parameter vectors (only `& {:keys ...}` works)
- [ ] Docstrings in `defn`
- [ ] `(apply f x y coll)` with leading arguments; `apply` over a vector
- [ ] List quoting `(quote (1 2 3))`
- [ ] `throw`
- [ ] `try` / `catch` / `finally`
- [ ] `let*` / `fn*` raw forms
- [ ] `var` form
- [ ] `case`, `if-let`, `when-let`
- [ ] Threading: `->` `->>` `as->` `some->` `some->>`

### Data structures (not yet)

- [ ] Sets — `ProtoSparseList` based, with `conj` / `disj`
- [ ] Lazy seqs — `LazySeq` wrapper
- [ ] Structural equality across collections — `=` compares numbers,
      but two equal lists built separately are not `=`
- [ ] Keywords, maps and vectors as functions (`(:a m)`, `(m :a)`, `(v 0)`)
- [ ] `count` on maps

### Namespaces & vars (not yet)

- [ ] `ns` form
- [ ] `:require :as :refer`
- [ ] Namespaced symbol resolution
- [ ] `in-ns`, `create-ns`, `find-ns`, `the-ns`, `all-ns`
- [ ] `^:dynamic` vars + `binding`
- [ ] `alter-var-root`

### State (not yet)

- [ ] `volatile!`, `vreset!`, `vswap!`
- [ ] `delay`, `force`
- [ ] `agent` (the Clojure-JVM family — different semantics from `actor`)

### Module system / UMD (not yet)

- [ ] Clojure-path resolver (unprefixed)
- [ ] `py/` provider
- [ ] `js/` provider
- [ ] `pst/` provider
- [ ] `clj/` explicit prefix
- [ ] Module cache
- [ ] Conversion helpers `clj->py` `py->clj` `clj->js` `js->clj`
      `clj->pst` `pst->clj`

### protoCore call convention — dual syntax

The dual syntax for **consuming and generating** functions under the
protoCore call convention is what makes UMD interop transparent. All
four shapes work end to end:

- [x] Positional consume — `(foo 1 2 3)` works for primitives and user fns.
- [x] Positional generate — `defn` functions are ordinary callables with
      positional arguments.
- [x] Named consume — trailing map literal `(area 3 4 {:unit :feet})`.
- [x] Named consume — trailing kv pairs `(area 3 4 :unit :feet)` (the
      Clojure idiom). The compiler detects the `:keyword value` suffix and
      packages it into a kwArgs map via the `CALL_KW` opcode; the VM
      keeps the map if the callee is kw-based, otherwise unpacks it
      back into positional kv pairs so primitives like `assoc` and
      `get` keep working unchanged.
- [x] Named generate — `(defn foo [a b & {:keys [unit]}] ...)`.
- [x] **`:or` defaults** — `(defn foo [& {:keys [unit] :or {unit :meters}}] ...)`.
      The body prologue evaluates the default whenever the slot is nil
      (covers both "missing" and "explicit nil" — deviation D16).
- [x] **`:as` snapshot** — `(defn foo [& {:keys [...] :as opts}] ...)`.
      `opts` receives the raw kwArgs map (or nil when none was supplied).
- [x] Reader: map literals `{...}`.

Still planned:
- [ ] Keyword shorthand `:strs` / `:syms`.
- [ ] Per-key destructuring outside the kwArgs map (`{x :x}` binding).

See the archived
[call-convention design](archive/design-specs/2026-06-14-protocore-call-convention.md)
for the rationale.

### Core library extensions (not yet)

- [ ] `quot rem mod`
- [ ] More predicates: `true? false? zero? pos? neg? even? odd?`
- [ ] More collection: `next conj seq into concat mapcat interleave interpose`
- [ ] Map operations: `dissoc update merge select-keys get-in update-in assoc-in`
- [ ] More higher-order: `comp partial juxt`
- [ ] More predicates: `every? some not-any? not-every?`
- [ ] Range / sequence: `range iterate repeat cycle take drop take-while drop-while partition partition-all`
- [ ] Regex: `re-pattern re-find re-seq re-matches`
- [ ] `format` (printf-style)
- [ ] `frequencies group-by sort sort-by`
- [ ] `pr-str print prn pr`

### REPL and command line (not yet)

- [ ] `*e` (last exception) — needs exception objects, planned with `try` / `catch`
- [ ] `(doc symbol)`, `(source symbol)` — needs docstring storage on `defn`
- [ ] `protoclj -e <expr>`
- [ ] nREPL server (CIDER / Calva / Conjure compatible) — planned for v0.1

### Macros (not yet)

- [ ] Compile-time evaluation pipeline (the bootstrap)
- [ ] `clojure.core` macros defined in protoClojure
- [ ] User-defined macros (`defmacro`)

### Bootstrap (not yet)

- [ ] A `core.clj` evaluated at startup — replaces the "everything is a C++ primitive" pattern

---

## Intentional deviations from Clojure-JVM

See `LANGUAGE.md` for the full discussion. Summary:

| ID  | Departure                                                  | Track  |
| --- | ---------------------------------------------------------- | ------ |
| D1  | No JVM interop                                             | core   |
| D2  | No Java classes                                            | core   |
| D3  | Characters are 1-codepoint strings                         | core   |
| D4  | No chunked sequences in v0.x                               | v0.2   |
| D5  | No transducers in v0.x                                     | v0.2   |
| D6  | No STM in v0.x                                             | v0.2   |
| D7  | No Clojure-JVM `agent` in v0.x; the protoCore-native `actor` ships instead | v0.3 |
| D8  | No `core.async` (likely permanent — re-imagined on actors) | v0.3+  |
| D9  | No `defrecord` / `deftype` in v0.x                         | v0.2   |
| D10 | No `BigDecimal` literal `M` suffix in v0.x                 | v0.2   |
| D11 | No `clojure.spec`                                          | v0.x   |
| D12 | `clojure.java.*` namespaces do not exist                   | (perm) |
| D13 | `read-string` strict on unregistered reader literals       | core   |
| D14 | LargeInteger promotion is automatic on `*` — no `*'` needed (CONTRA Clojure-JVM, by design) | (perm) |
| D15 | `(= 1 1.0)` returns `true` in v0.x (CONTRA Clojure-JVM where `=` is type-strict) | (perm) |
| D16 | `:or` defaults fire on **explicit nil** as well as missing keys (CONTRA JVM-Clojure where only missing keys take the default) | v0.2 |
| D17 | String ops (`upper-case`, `lower-case`, `split`, `reverse`, `trim`, `index-of`) are byte-level / ASCII-correct only; multi-byte UTF-8 codepoints traverse as bytes (CONTRA JVM-Clojure which is codepoint-aware) | v0.2 |
| D18 | `(fn name [args] body)` — the name is accepted by the compiler but dropped; self-reference via `name` inside the body is not supported (use `defn` for self-recursion). Planned for v0.2 via wrapper-into-slot capture. | v0.2 |

## Known issues

- **Three conformance fixtures fail against the current protoCore:**
  `16-maps/assoc-multi.clj`, `18-kw-callsite/assoc-still-works.clj` and
  `19-or-and-as/as-snapshot.clj`. Maps print their entries in the
  iteration order of the underlying `ProtoSparseList`, which is keyed by
  hash, and the fixtures expect a specific order: `assoc-multi.clj`
  expects `{:a 1, :b 2, :c 3}` and the binary prints `{:a 1, :c 3, :b 2}`.
- **Promise `deref` polls.** A pending promise is checked every millisecond
  (with the thread marked unmanaged so garbage collection can proceed);
  adequate for hand-off latency, not for sub-millisecond waits.
- **`pmap` spawns one OS thread per element**, which is wasteful for large
  collections.
- **Actor `MPMC` throughput** is limited by the global ready-queue mutex.

## History

Shipped changes are listed in [`CHANGELOG.md`](../CHANGELOG.md); the commit
log on `main` records the rationale and measurements for each change.
