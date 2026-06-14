# protoClojure — Status

> Living tracker. This file changes every time something is implemented,
> finished, or dropped. If a feature you expect is missing here, it is
> not implemented.

**Current state:** post-session 12. The interpreter runs. 93 conformance
fixtures + the unit tests all pass on the `main` branch. The bench
harness produces honest numbers vs Babashka 1.4.192 (see
`benchmarks/RESULTS.md`).

---

## Session-by-session progress

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
| 12 | Fewer attribute lookups per CALL (perf cleanup) | 93 |
| 13 | Maps + `& {:keys [...]}` named-arg destructuring | 103 |

The dated design specs live under `docs/superpowers/specs/`; the
memory entries for each session live under
`~/.claude/projects/-home-gamarino-Documentos-proyectos/memory/`.

---

## Implemented and stable

### Reader

- [x] Integers (SmallInteger + auto-promoted LargeInteger)
- [x] Floats — `3.14`, `1e6`, `1.5e-3`
- [x] Strings — `"hello"` (Reader wraps under `stringMarkerProto`)
- [x] Symbols (auto-interned via protoCore SymbolTable)
- [x] Keywords — `:foo`, self-evaluating constants
- [x] Booleans + nil — `true`, `false`, `nil` as compile-time literals
- [x] List `(...)`
- [x] Vector `[...]` (Reader wraps under `vectorMarkerProto`)
- [x] Line comments `;;`

### Special forms

- [x] `def`
- [x] `defn` (single-arity)
- [x] `defn` (multi-arity, with optional variadic catch-all)
- [x] `fn` (single + multi-arity)
- [x] `let`
- [x] `loop`
- [x] `recur` — inside `loop` AND at the top of a fn body (implicit recur target)
- [x] `if`
- [x] `do`
- [x] `quote` (atoms only — list quoting is a later session)
- [x] `apply` — special form, two-arg shape `(apply f coll)`
- [x] `when`, `when-not`
- [x] `cond` (with `:else` / `else`)
- [x] `and`, `or` (short-circuit via DUP + JUMP_IF_TRUE/FALSE)
- [x] Variadic `& rest` in fn params
- [x] Named-arg destructuring `& {:keys [...]}` (session 13)

### Data structures (live in the VM)

- [x] Lists — protoCore `ProtoList`
- [x] Vectors — protoCore `ProtoTuple` (O(log N) `nth`)
- [x] Maps — protoCore `ProtoSparseList` keyed by `key->getHash`, with
      `hash-map` / `assoc` / `get` / `contains?` / `keys` / `vals` /
      `map?` (session 13)
- [x] Strings — protoCore `ProtoString`
- [x] Integers — `SmallInteger` tagged + auto-promoted `LargeInteger`
- [x] Floats — `ProtoObject::fromDouble`

### Numeric semantics

- [x] SmallInt fast-path arithmetic via opcodes (`ADD SUB MUL LT LE GT GE EQ`)
- [x] Automatic promotion to LargeInteger on overflow
- [x] Automatic promotion to double when any operand is float
- [x] Print path handles SmallInt + LargeInt + Float correctly

### Closures

- [x] N-level lexical capture across nested `fn` bodies
- [x] Captures cascade — intermediate scopes create capture slots automatically
- [x] First-class fns — `((make-mul k) x)` callable head, passed around freely

### Primitives installed at startup

- [x] Arithmetic: `+ - * / inc dec`
- [x] Comparison: `< <= > >= =`
- [x] Predicates: `nil? empty? vector? list? not`
- [x] List ops: `list first rest cons count reverse`
- [x] Vector ops: `vector vec nth`
- [x] Higher-order: `map filter reduce` (via ActiveCallContext re-entry)
- [x] I/O: `println str`

### Bytecode VM

- [x] 28 opcodes — see `src/runtime/Opcodes.h`
- [x] `MAKE_FN` / `MAKE_FN_MULTI` — single + multi-arity wrappers
- [x] `CALL_APPLY` — spread-args dispatch
- [x] Split fn prototypes (`fnSingleProto` / `fnMultiProto`) for single-getAttribute CALL
- [x] SmallInt fast-path binary opcodes
- [x] DUP + JUMP_IF_TRUE for short-circuit `and` / `or`

### Tooling

- [x] `protoclj <script.clj>` — single-file evaluation
- [x] `protoclj --version` / `-h`
- [x] Conformance suite under `tests/conformance/` — discovered by glob
- [x] Bench harness `benchmarks/bench.sh` (vs Babashka)

---

## NOT yet implemented (tracked)

These are the features the docs / examples reference but the implementation
does not yet support. Calling any of them raises a clear error.

### Reader (not yet)

- [x] Maps `{...}` (session 13 — backed by `ProtoSparseList` keyed by `key->getHash`)
- [ ] Sets `#{...}`
- [ ] Quasiquote `` ` ``, unquote `~`, splice `~@`
- [ ] Anonymous-fn shorthand `#(...)`
- [ ] Var-quote `#'`
- [ ] Discard `#_`
- [ ] Metadata `^{...}` plus shorthands `^kw` `^Type`
- [ ] Reader literals `#inst` `#uuid`
- [ ] Ratio literal `1/3`
- [ ] Character literal `\a`
- [ ] Namespace-qualified symbols / keywords `foo/bar`, `:ns/kw`

### Special forms (not yet)

- [ ] `throw`
- [ ] `try` / `catch` / `finally`
- [ ] `let*` / `fn*` raw forms (only the macro names are accepted today)
- [ ] `var` form
- [ ] `case`, `if-let`, `when-let`
- [ ] Threading: `->` `->>` `as->` `some->` `some->>`

### Data structures (not yet)

- [ ] Maps — `ProtoSparseList` based, with `assoc` / `get` / `dissoc`
- [ ] Sets — `ProtoSparseList` based, with `conj` / `disj`
- [ ] Lazy seqs — `LazySeq` wrapper
- [ ] Structural equality across types — today `=` is numeric only

### Namespaces & vars (not yet)

- [ ] `ns` form
- [ ] `:require :as :refer`
- [ ] Namespaced symbol resolution
- [ ] `in-ns`, `create-ns`, `find-ns`, `the-ns`, `all-ns`
- [ ] `^:dynamic` vars + `binding`
- [ ] `alter-var-root`

### State (not yet)

- [ ] `atom`, `swap!`, `reset!`, `compare-and-set!`, `deref` / `@`
- [ ] `add-watch`, `remove-watch`
- [ ] `volatile!`, `vreset!`, `vswap!`
- [ ] `delay`, `force`, `realized?`
- [ ] `promise`, `deliver`
- [ ] `future`, `pmap`, `agent`

### Module system / UMD (not yet)

- [ ] Clojure-path resolver (unprefixed)
- [ ] `py/` provider
- [ ] `js/` provider
- [ ] `pst/` provider
- [ ] `clj/` explicit prefix
- [ ] Module cache
- [ ] Conversion helpers `clj->py` `py->clj` `clj->js` `js->clj`
      `clj->pst` `pst->clj`

### protoCore call convention — dual syntax (session 13 partial)

The dual syntax for **consuming AND generating** functions under the
protoCore call convention is what makes UMD interop transparent.
Session 13 shipped:

- [x] Positional consume — `(foo 1 2 3)` works for primitives and user fns.
- [x] Positional generate — `defn` functions reachable from protoST /
      protoPython through the bare-name attribute on the namespace.
- [x] **Named consume — trailing map literal**: `(area 3 4 {:unit :feet})`
      reaches a user fn declared with `& {:keys [unit]}` via the
      kwArgs dict; foreign callees see the map via their `kwArgs`
      ProtoMethod slot.
- [x] **Named generate** — `(defn foo [a b & {:keys [unit]}] ...)`
      produces a wrapper whose body reads `:unit` from the caller's
      kwArgs map.
- [x] Reader: map literals `{...}` (session 13).
- [ ] **Trailing kv-pair detection** — `(area 3 4 :unit :feet)` (without
      the `{}`) should be equivalent to the trailing-map form. The
      compiler needs to detect a `:keyword value` suffix and pack it
      into a map at the call site. Targeted for **session 14**.
- [ ] `:or` defaults in destructuring — the parser accepts the syntax
      today but defaults are not applied. Also session 14.
- [ ] `:as` binding (snapshot of the kwArgs map) — same.
- [ ] Keyword binding via shorthand `:strs` / `:syms` — not on the
      immediate path.

See `docs/superpowers/specs/2026-06-14-protocore-call-convention.md`
for the design.

### Core library extensions (not yet)

- [ ] `quot rem mod`
- [ ] More predicates: `true? false? zero? pos? neg? even? odd?`
- [ ] More collection: `next conj seq into concat mapcat interleave interpose`
- [ ] Map operations: `assoc dissoc update merge select-keys keys vals get-in update-in assoc-in`
- [ ] More higher-order: `comp partial juxt`
- [ ] More predicates: `every? some not-any? not-every?`
- [ ] Range / sequence: `range iterate repeat cycle take drop take-while drop-while partition partition-all`
- [ ] String ops: `re-pattern re-find re-seq re-matches format`
- [ ] `frequencies group-by sort sort-by`
- [ ] `pr-str print prn pr`
- [ ] `clojure.string` — count chars, upper, lower, split, join, subs, replace, trim

### REPL (not yet)

- [ ] Interactive `protoclj` REPL
- [ ] `*1` `*2` `*3` `*e`
- [ ] `(doc symbol)`, `(source symbol)`
- [ ] nREPL server (CIDER / Calva / Conjure compatible)

### Macros (not yet)

- [ ] Compile-time evaluation pipeline (the bootstrap)
- [ ] `clojure.core` macros defined in protoClojure
- [ ] User-defined macros (`defmacro`)

### Bootstrap (not yet)

- [ ] A real `core.clj` evaluated at startup — stops the "everything is a C++ primitive" pattern

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
| D7  | No agents in v0.x (yet — the protoST actor model will land here) | v0.3 |
| D8  | No `core.async` (likely permanent — re-imagined on actors) | v0.3+  |
| D9  | No `defrecord` / `deftype` in v0.x                         | v0.2   |
| D10 | No `BigDecimal` literal `M` suffix in v0.x                 | v0.2   |
| D11 | No `clojure.spec`                                          | v0.x   |
| D12 | `clojure.java.*` namespaces do not exist                   | (perm) |
| D13 | `read-string` strict on unregistered reader literals       | core   |
| D14 | LargeInteger promotion is automatic on `*` — no `*'` needed (CONTRA Clojure-JVM, by design) | (perm) |
| D15 | `(= 1 1.0)` returns `true` in v0.x (CONTRA Clojure-JVM where `=` is type-strict) | (perm) |

## Known issues

None blocking. Items in the "NOT yet implemented" list are scoped
features, not bugs.

## Closed items

Recorded by session. See the commit log on `main` for SHAs; the
"Session-by-session progress" table at the top is the index.
