# protoClojure — Status

> Living tracker. This file changes every time something is implemented,
> finished, or dropped. If a feature you expect is missing here, it is
> not implemented.

**Current phase: design.** The interpreter is not implemented. The
documents in `docs/` are the design spec for v0.1.

---

## What is in this repository today

- Language reference draft (`docs/LANGUAGE.md`)
- Module / interop design (`docs/INTEROP.md`)
- Architectural design (`docs/DESIGN.md`)
- Roadmap (`docs/ROADMAP.md`)
- Two-audience tutorial outline (`docs/TUTORIAL.md`)

## What is NOT in this repository today

- The C++ interpreter (`src/`, `test/`)
- The `clojure.core` standard library
- The nREPL server
- A `protoclj` binary
- Examples in `examples/` (placeholders only)
- CI / benchmarks

---

## Implemented (v0.1 progress)

Nothing yet. Will tick off as features land.

### Reader

- [ ] Numbers (integer, float, ratio, radix-N)
- [ ] Strings, characters
- [ ] Symbols, keywords (incl. namespace-qualified)
- [ ] Booleans, nil
- [ ] List `(...)`
- [ ] Vector `[...]`
- [ ] Map `{...}`
- [ ] Set `#{...}`
- [ ] Quote `'`, quasiquote `` ` ``, unquote `~`, splice `~@`
- [ ] Anonymous fn `#(...)`
- [ ] Var-quote `#'`
- [ ] Discard `#_`
- [ ] Metadata `^{...}` plus shorthands `^kw` `^Type`
- [ ] Reader literals `#inst` `#uuid` (parse-only in v0.1)

### Evaluator / special forms

- [ ] `def`
- [ ] `if`
- [ ] `do`
- [ ] `let*` (and `let` macro on top)
- [ ] `loop*` + `recur`
- [ ] `fn*` (and `fn`, `defn`)
- [ ] `quote`
- [ ] `var`
- [ ] `throw`
- [ ] `try` / `catch` / `finally`

### Data structures

- [ ] List (`ProtoList` linked mode)
- [ ] Vector (`ProtoList` indexed mode)
- [ ] Map (`ProtoSparseList`)
- [ ] Set (`ProtoSparseList` keyed)
- [ ] Lazy seq (`LazySeq` wrapper)
- [ ] Structural equality `=`
- [ ] Hashing for collections

### Namespaces & vars

- [ ] `ns` form
- [ ] `:require :as :refer`
- [ ] Namespaced symbol resolution
- [ ] `in-ns`, `create-ns`, `find-ns`, `the-ns`, `all-ns`
- [ ] `^:dynamic` vars + `binding`
- [ ] `alter-var-root`

### State

- [ ] `atom`, `swap!`, `reset!`, `compare-and-set!`, `deref` / `@`
- [ ] `add-watch`, `remove-watch`
- [ ] `volatile!`, `vreset!`, `vswap!`
- [ ] `delay`, `force`, `realized?`
- [ ] `promise`, `deliver`

### Module system (UMD hybrid)

- [ ] Clojure-path resolver (unprefixed)
- [ ] `py/` provider
- [ ] `js/` provider
- [ ] `pst/` provider
- [ ] `clj/` explicit prefix
- [ ] Module cache
- [ ] Conversion helpers `clj->py` `py->clj` `clj->js` `js->clj`
      `clj->pst` `pst->clj`

### Core library

- [ ] Arithmetic primitives (`+ - * / quot rem mod inc dec`)
- [ ] Comparison (`= == not= < <= > >=`)
- [ ] Predicates (`nil? true? false? zero? pos? neg? even? odd?`)
- [ ] Collection primitives (`first rest next cons conj count empty? seq`)
- [ ] Map operations (`get assoc dissoc update merge select-keys keys vals`)
- [ ] Higher-order (`map filter reduce apply comp partial juxt`)
- [ ] Control macros (`when when-not if-let when-let cond case`)
- [ ] Threading (`-> ->> as-> some-> some->>`)
- [ ] String basics (`str format`)
- [ ] `re-pattern`, `re-find`, `re-seq`, `re-matches`
- [ ] `frequencies`, `group-by`, `sort`, `sort-by`
- [ ] `range`, `iterate`, `repeat`, `cycle`, `take`, `drop`,
      `take-while`, `drop-while`, `partition`, `partition-all`
- [ ] `into`, `concat`, `mapcat`, `interleave`, `interpose`
- [ ] `pr-str`, `print`, `println`, `prn`, `pr`

### REPL

- [ ] Interactive `protoclj` REPL (local, no nREPL)
- [ ] `*1` `*2` `*3` `*e`
- [ ] `(doc symbol)`, `(source symbol)`
- [ ] nREPL server: `eval`, `interrupt`, `clone`, `close`, `describe`,
      `load-file`
- [ ] nREPL `info`, `complete` (stretch)
- [ ] Smooth CIDER session

### Macros

- [ ] Compile-time evaluation pipeline (the bootstrap)
- [ ] `clojure.core` macros defined in protoClojure
- [ ] User-defined macros

### Standard library namespaces (post-`clojure.core`)

- [ ] `clojure.string`
- [ ] `clojure.set`
- [ ] `clojure.walk`
- [ ] `clojure.edn`
- [ ] `clojure.pprint`

---

## Intentional deviations from Clojure-JVM

See `LANGUAGE.md` §13 for the canonical list. Summary of the v0.1
deviations:

| ID  | Departure                                                  | Track  |
| --- | ---------------------------------------------------------- | ------ |
| D1  | No JVM interop                                             | core   |
| D2  | No Java classes                                            | core   |
| D3  | Characters are 1-codepoint strings                         | core   |
| D4  | No chunked sequences in v0.1                               | v0.2   |
| D5  | No transducers in v0.1                                     | v0.2   |
| D6  | No STM in v0.1                                             | v0.2   |
| D7  | No agents in v0.1                                          | v0.3   |
| D8  | No `core.async` (likely permanent — re-imagined on actors) | v0.3+  |
| D9  | No `defrecord` / `deftype` in v0.1                         | v0.2   |
| D10 | No `BigDecimal` literal `M` suffix in v0.1                 | v0.2   |
| D11 | No `clojure.spec`                                          | v0.x   |
| D12 | `clojure.java.*` namespaces do not exist                   | (perm) |
| D13 | `read-string` strict on unregistered reader literals       | core   |

## Known issues

None yet — there is no code to have issues.

## Closed items

Empty. Future releases will record resolutions here with commit SHAs,
mirroring protoST's STATUS layout.
