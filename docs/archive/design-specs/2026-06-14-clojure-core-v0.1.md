# `clojure.core` for v0.1 — implementation spec

> **Purpose.** Decide, before any code is written, exactly which
> functions and macros ship in v0.1's `clojure.core`, in what
> implementation language (C++ vs protoClojure), in what bootstrap
> order, and with which arities. The goal is to make the
> implementation phase mechanical: every entry below is a unit of
> work with a clear definition of done.

**Date.** 2026-06-14.
**Status.** Approved internally; subject to revision when v0.1 begins
implementation. Changes after that point go through a numbered
revision section at the end.

---

## 1. Scope

`clojure.core` in JVM Clojure has ~600 publicly exported vars. We are
not shipping ~600 in v0.1. The v0.1 target is **about 130 names**
covering the operations any real Clojure script needs:

- Arithmetic, comparison, numeric predicates.
- Boolean logic.
- The four collection types: construction, access, structural
  modification.
- Sequence vocabulary, all element-at-a-time (chunked seqs in v0.2).
- Higher-order: `map`, `filter`, `reduce`, `apply`, `comp`, `partial`.
- Threading macros (`->`, `->>`, `as->`, `some->`, `some->>`,
  `cond->`, `cond->>`).
- Control-flow macros (`when`, `if-let`, `cond`, `case`, etc.).
- `atom` + the CAS primitives (no STM, no agents).
- Protocols (`defprotocol`, `extend-type`, `extend-protocol`).
- Multimethods (`defmulti`, `defmethod`, `derive`, `isa?`).
- Strings (only the basics; `clojure.string` lands in v0.2).
- Regex (delegating to host `std::regex`).
- Printing (`println`, `print`, `prn`, `pr`, `str`, `format`).
- Exception construction and matching.
- UMD interop conversion (`clj->py`, `py->clj`, `clj->js`, `js->clj`,
  `clj->pst`, `pst->clj`).
- A bare-minimum I/O surface (`slurp`, `spit`).

Out of scope for v0.1 (with explicit deferral targets in §8):
transducers, chunked seqs, `defrecord`/`deftype`, STM (refs), agents,
`core.async`, `clojure.spec`, `BigDecimal` `M` literals, the
`clojure.string`/`clojure.set`/`clojure.walk`/`clojure.edn` namespaces,
the JVM-specific namespaces.

The list of names that v0.1 actually exports is the catalog in §5.
Anything not on the catalog raises `Unable to resolve symbol` at use
time. **No silent stubs.**

---

## 2. The decisions, summarised

Five up-front decisions that shape the rest of the document.

1. **Two-tier implementation, not three.** Every name is either in
   C++ (compiled into the `protoclj` binary) or in protoClojure (in
   `clojure/core.clj`, loaded at startup). There is no third "C++
   intrinsic with Clojure adapter" tier; if a name benefits from C++,
   it goes there fully.

2. **C++ holds the floor, Clojure holds everything else.** The C++
   side ships *only* what the rest cannot be written without:
   special forms, the seven bootstrap macros, ~40 primitive
   functions. Everything beyond that is in `core.clj`.

3. **One `core.clj`, not several files.** JVM Clojure does this and
   it works. The single-file approach makes the bootstrap order
   visible — you read top to bottom and every form is defined before
   its first use. Splitting into multiple files lands in v0.x when
   `core.clj` exceeds ~2000 lines.

4. **Idiom over performance, at the boundary line.** A function
   landing in C++ pays it back in measurable hot-path performance
   AND has a behaviour that does not change. A function landing in
   Clojure is iterated on freely. The boundary is set conservatively
   — when in doubt, put it in Clojure first; move to C++ later if
   benchmarks ask for it.

5. **Arity discipline: ship the common arities; raise on the rest.**
   `map` ships its 1-coll and 2-coll arities; calling 3-coll raises
   `Arity not yet supported in v0.1`. Same for `reduce`, `apply`,
   etc. The behaviour the caller gets is honest; the work to extend
   is bounded.

---

## 3. Implementation tiers

### 3.1 C++ specials — the irreducible set

These are *not* functions or macros; they are forms the compiler
recognises before dispatching to anything else. They must be in C++
because they cannot be defined in terms of anything else.

| Form              | Notes                                                                  |
|-------------------|------------------------------------------------------------------------|
| `if`              | Two- and three-arg                                                     |
| `do`              | Empty-body returns nil                                                 |
| `let*`            | Bindings vector; user-facing `let` is a macro on top                   |
| `loop*`           | Like `let*` plus recur target; user-facing `loop` is a macro           |
| `recur`           | Tail-position-enforced jump                                            |
| `fn*`             | Single-arity form; user-facing `fn` is a macro                         |
| `quote`           | Returns its arg unevaluated                                            |
| `def`             | Interns a var; takes optional docstring and metadata                   |
| `var`             | Returns a var by symbol                                                |
| `throw`           | Raises an exception object                                             |
| `try`/`catch`/`finally` | Standard exception handling                                       |
| `.` (dot)         | Reserved; raises "JVM interop not supported" in v0.1                   |
| `new`             | Reserved; raises "JVM interop not supported" in v0.1                   |
| `set!`            | Used by `binding`'s dynamic-var rebinding; supported for `^:dynamic` vars only |
| `monitor-enter`/`monitor-exit` | Reserved; raise "JVM monitors not supported"              |

These twelve forms are the compiler's complete special vocabulary.
Every other named construct in Clojure is a macro that expands to
them.

### 3.2 C++ bootstrap macros — seven names, the rest cannot start without them

We need a small set of macros hard-coded in C++ so that `core.clj`
itself can use them before `core.clj` re-defines them in Clojure.

| Macro          | Why C++                                                              |
|----------------|----------------------------------------------------------------------|
| `defn`         | `core.clj` is essentially a list of `defn` and `defmacro` forms      |
| `defmacro`     | Same                                                                 |
| `let`          | Used inside macro bodies everywhere                                  |
| `fn`           | Same                                                                 |
| `loop`         | Same                                                                 |
| `when`         | Used in almost every macro body; nice to have without re-defining    |
| `ns`           | Read at the top of every `.clj` file; needs to work before `core.clj` is loaded |

After `core.clj` loads, these are **re-defined in Clojure**, and the
Clojure version takes precedence (`def` replaces the var's value).
The C++ versions only run during bootstrap; they are correct but
not perfectly featured. The Clojure versions provide the full
docstrings, metadata propagation, and pre/post condition handling
that the v0.1 reference describes.

### 3.3 C++ primitive functions — ~40 names

Functions that must be in C++ because they touch the kernel directly
or are on the hot path for everything else. Listed by category.

**Tagged-immediate arithmetic** (fast paths exploit SmallInteger
tagging):

```
+  -  *  /  quot  rem  mod  inc  dec
=  ==  not=  <  <=  >  >=  compare
hash
```

**Boolean primitives**:

```
not
```

(Note: `and` and `or` are macros — they short-circuit, so they
cannot be functions. They live in `core.clj`.)

**Type predicates** (one-liners checking protoCore primitive
prototypes; fast because they are pointer comparisons):

```
nil?  true?  false?  some?
number?  integer?  float?  ratio?
string?  symbol?  keyword?  boolean?
fn?  ifn?
```

**Identity** (used by macros and threading):

```
identity  constantly
```

**Collection primitives** (need C++ access to ProtoList,
ProtoSparseList):

```
count  first  rest  next  cons  conj
seq    empty
get    assoc  dissoc
list   vector  hash-map  hash-set
```

**Atoms** (CAS primitives):

```
atom  swap!  reset!  compare-and-set!  deref
```

**Strings** (delegate to ProtoString):

```
str    subs   format
```

**Printing** (writes to host stdout):

```
print  println  pr  prn  newline  flush
```

**Reader** (turns a string into a form):

```
read-string
```

**Error construction**:

```
ex-info  ex-data  ex-message  ex-cause
```

**Var operations**:

```
var-get  var-set
```

**Foreign dispatch entry points** (must know about UMD):

```
clj->py  py->clj
clj->js  js->clj
clj->pst pst->clj
```

**Total count**: 67 names. Some entries here are pairs (`var-get` and
`var-set` count as two); the practical implementation count is a
little higher.

### 3.4 The Clojure layer — `clojure/core.clj`

Everything else. Defined by `(defn ...)` and `(defmacro ...)` forms
in one file, loaded by the runtime at startup right after the C++
primitives are installed.

The full catalog is in §5; the count is **~85 names** added on top
of the C++ tier, giving a total user-visible v0.1 surface of
**~130 names**.

The file structure (one file, sections of related forms):

```
clojure/core.clj
├── 0. Forward declarations (declare).
├── 1. Re-define bootstrap macros (defn, defmacro, let, fn, loop,
│      when, ns) with full v0.1 semantics.
├── 2. Boolean macros: and, or.
├── 3. Control macros: when-not, when-let, if-let, cond, case,
│      condp, do, comment, declare, defonce, defn-.
├── 4. Threading macros: ->, ->>, as->, some->, some->>,
│      cond->, cond->>, doto.
├── 5. Iteration macros: dotimes, doseq, for.
├── 6. Delay and laziness: delay, lazy-seq, force.
├── 7. Collection vocabulary built on the C++ primitives:
│      empty?, not-empty, peek, pop, nth, last, butlast,
│      keys, vals, contains?, find, select-keys, update,
│      get-in, assoc-in, update-in, dissoc-in, merge,
│      merge-with, into, vec, set, set?, vector?, map?,
│      list?, coll?, seq?, sequential?, associative?,
│      reverse, concat, mapcat.
├── 8. Sequence operations: range, iterate, repeat, repeatedly,
│      cycle, take, drop, take-while, drop-while, partition,
│      partition-all, distinct, interleave, interpose,
│      sort, sort-by, frequencies, group-by, zipmap.
├── 9. Higher-order combinators: map, filter, reduce, apply,
│      comp, partial, juxt, complement, every-pred, some-fn,
│      memoize, fnil, every?, some, not-any?, not-every?,
│      doall, dorun.
├── 10. Symbol/keyword/var helpers: name, namespace, symbol,
│       keyword, var, find-var.
├── 11. Math helpers: min, max, abs, even?, odd?, zero?, pos?,
│       neg?.
├── 12. String helpers (in core, not clojure.string): print-str,
│       pr-str.
├── 13. I/O: slurp, spit.
├── 14. Regex: re-pattern, re-find, re-seq, re-matches.
├── 15. Atoms watcher API: add-watch, remove-watch.
├── 16. Volatiles + promises + delays: volatile!, vreset!,
│       vswap!, promise, deliver, realized?, future.
├── 17. Protocols: defprotocol, extend-type, extend-protocol,
│       satisfies?, extends?.
├── 18. Multimethods: defmulti, defmethod, isa?, derive,
│       underive, prefer-method, methods, get-method,
│       remove-method, make-hierarchy.
├── 19. The implicit-`refer` setup so user namespaces see
│       everything above by default.
```

Each section is a few dozen lines. Total file size estimate:
**900-1200 lines of Clojure**.

---

## 4. Bootstrap order

Step-by-step, what the runtime does at startup. Each step depends
only on the steps above it.

1. **Initialise protoCore.** Allocate a ProtoSpace, set up
   bootstrap prototypes (Object, Number, String, etc.).
2. **Install C++ specials.** Register the compiler's special-form
   recognition table. `if`, `do`, `let*`, etc. are now valid.
3. **Install C++ primitive functions.** Each of §3.3's ~67 names
   becomes a callable cell interned in `clojure.core`. References
   like `+` and `count` resolve.
4. **Install C++ bootstrap macros.** §3.2's seven names get vars in
   `clojure.core` pointing at C++-implemented macro objects.
   `defn`, `defmacro`, `let`, `fn`, `loop`, `when`, `ns` are now
   valid.
5. **Load `clojure/core.clj`.** The reader reads it. The compiler
   compiles each top-level form sequentially. Each `(defn ...)` or
   `(defmacro ...)` interns a var; subsequent forms can reference
   it. By the end, the Clojure-layer ~85 names are installed.
6. **Replace C++ bootstrap macros with their Clojure
   re-definitions.** `core.clj` redefines `defn`, `defmacro`, etc.
   in pure Clojure; those vars now point at the Clojure-side
   implementations. From the user's perspective, everything is
   in Clojure.
7. **Set up the user namespace.** Switch to `user`. Apply the
   default `refer-clojure` so `clojure.core` vars are visible.
8. **Ready.** The REPL prompt appears, or the user's script begins.

The whole sequence runs in **under 50 ms** as a startup target —
loading `core.clj` is the dominant cost, ~1000 lines of Clojure to
parse and compile. JVM Clojure does the equivalent in 600-1500ms
because the JVM itself is slow to start; protoCore has no warm-up
tax.

---

## 5. The catalog

Every name in v0.1, listed in alphabetical order. Each entry has:

- **Impl**: `C` (C++) or `clj` (defined in `core.clj`).
- **Kind**: `fn` (function), `m` (macro), `s` (special form).
- **Arities**: the arities supported in v0.1. `[1]` = arity-1 only;
  `[1 2]` = arity-1 and arity-2; `[1+]` = arity-1 with optional
  trailing args; `[any]` = variadic from the start; `[m]` for
  macros (no fixed arity but called by shape).
- **Notes**: short pointer to where the function lives in the
  bootstrap, or what is unusual.

Names are ordered alphabetically. Macros and specials are tagged in
the Kind column.

| Name                  | Impl | Kind | Arities  | Notes                                          |
|-----------------------|------|------|----------|------------------------------------------------|
| `*`                   | C    | fn   | `[0+]`   | Identity element 1                             |
| `+`                   | C    | fn   | `[0+]`   | Identity element 0                             |
| `-`                   | C    | fn   | `[1+]`   | Unary form is negation                         |
| `/`                   | C    | fn   | `[1+]`   | Returns Ratio when args are integers           |
| `<`                   | C    | fn   | `[1+]`   | Variadic: monotonic chain                      |
| `<=`                  | C    | fn   | `[1+]`   | Variadic: monotonic chain                      |
| `=`                   | C    | fn   | `[1+]`   | Value equality, structural for collections     |
| `==`                  | C    | fn   | `[1+]`   | Numeric equality across types                  |
| `>`                   | C    | fn   | `[1+]`   | Variadic: monotonic chain                      |
| `>=`                  | C    | fn   | `[1+]`   | Variadic: monotonic chain                      |
| `abs`                 | clj  | fn   | `[1]`    |                                                |
| `add-watch`           | clj  | fn   | `[3]`    |                                                |
| `and`                 | clj  | m    | `[m]`    | Short-circuit                                  |
| `apply`               | clj  | fn   | `[2 3 4 5+]` | Up to 4 prepended args + final seq         |
| `as->`                | clj  | m    | `[m]`    |                                                |
| `assoc`               | C    | fn   | `[3+]`   | Variadic key/val pairs                         |
| `assoc-in`            | clj  | fn   | `[3]`    |                                                |
| `atom`                | C    | fn   | `[1]`    | No options map in v0.1                         |
| `boolean?`            | C    | fn   | `[1]`    |                                                |
| `butlast`             | clj  | fn   | `[1]`    |                                                |
| `case`                | clj  | m    | `[m]`    | Constant-time dispatch on compile-time keys    |
| `clj->js`             | C    | fn   | `[1 2]`  | Second arg is an options map: `:deep true`     |
| `clj->pst`            | C    | fn   | `[1 2]`  | Same                                           |
| `clj->py`             | C    | fn   | `[1 2]`  | Same                                           |
| `coll?`               | clj  | fn   | `[1]`    |                                                |
| `compare`             | C    | fn   | `[2]`    | Returns -1/0/1                                 |
| `compare-and-set!`    | C    | fn   | `[3]`    |                                                |
| `complement`          | clj  | fn   | `[1]`    |                                                |
| `comp`                | clj  | fn   | `[0+]`   | `(comp)` is `identity`                          |
| `concat`              | clj  | fn   | `[0+]`   | Lazy                                           |
| `cond`                | clj  | m    | `[m]`    |                                                |
| `cond->`              | clj  | m    | `[m]`    |                                                |
| `cond->>`             | clj  | m    | `[m]`    |                                                |
| `condp`               | clj  | m    | `[m]`    |                                                |
| `conj`                | C    | fn   | `[1+]`   | Position depends on collection type            |
| `cons`                | C    | fn   | `[2]`    | First arg, rest is seq                         |
| `constantly`          | C    | fn   | `[1]`    |                                                |
| `contains?`           | clj  | fn   | `[2]`    | Key/index membership, NOT value membership     |
| `count`               | C    | fn   | `[1]`    | Uses protocol on foreign UMD objects (§7)      |
| `cycle`               | clj  | fn   | `[1]`    | Lazy, infinite                                 |
| `dec`                 | C    | fn   | `[1]`    |                                                |
| `declare`             | clj  | m    | `[m]`    | Forward references via `def name`              |
| `def`                 | C    | s    | `[s]`    | Special form                                   |
| `defmacro`            | C    | m    | `[m]`    | C++ at bootstrap, Clojure after `core.clj`     |
| `defmethod`           | clj  | m    | `[m]`    |                                                |
| `defmulti`            | clj  | m    | `[m]`    |                                                |
| `defn`                | C    | m    | `[m]`    | C++ at bootstrap, Clojure after `core.clj`     |
| `defn-`               | clj  | m    | `[m]`    | Private function                               |
| `defonce`             | clj  | m    | `[m]`    |                                                |
| `defprotocol`         | clj  | m    | `[m]`    |                                                |
| `delay`               | clj  | m    | `[m]`    |                                                |
| `deliver`             | clj  | fn   | `[2]`    |                                                |
| `deref`               | C    | fn   | `[1]`    | Works on atoms, delays, promises, futures      |
| `derive`              | clj  | fn   | `[2 3]`  |                                                |
| `disj`                | clj  | fn   | `[2+]`   | Variadic on a set                              |
| `dissoc`              | C    | fn   | `[2+]`   | Variadic keys                                  |
| `do`                  | C    | s    | `[s]`    |                                                |
| `doall`               | clj  | fn   | `[1]`    |                                                |
| `dorun`               | clj  | fn   | `[1]`    |                                                |
| `doseq`               | clj  | m    | `[m]`    | No nested bindings in v0.1; v0.2 adds them     |
| `dotimes`             | clj  | m    | `[m]`    |                                                |
| `doto`                | clj  | m    | `[m]`    |                                                |
| `drop`                | clj  | fn   | `[2]`    | Lazy                                           |
| `drop-while`          | clj  | fn   | `[2]`    | Lazy                                           |
| `empty`               | C    | fn   | `[1]`    | Returns the empty of the same coll type        |
| `empty?`              | clj  | fn   | `[1]`    |                                                |
| `even?`               | clj  | fn   | `[1]`    |                                                |
| `every?`              | clj  | fn   | `[2]`    |                                                |
| `every-pred`          | clj  | fn   | `[1+]`   |                                                |
| `ex-cause`            | C    | fn   | `[1]`    |                                                |
| `ex-data`             | C    | fn   | `[1]`    |                                                |
| `ex-info`             | C    | fn   | `[2 3]`  | (msg data) and (msg data cause)                |
| `ex-message`          | C    | fn   | `[1]`    |                                                |
| `extend-protocol`     | clj  | m    | `[m]`    |                                                |
| `extend-type`         | clj  | m    | `[m]`    |                                                |
| `extends?`            | clj  | fn   | `[2]`    |                                                |
| `false?`              | C    | fn   | `[1]`    |                                                |
| `filter`              | clj  | fn   | `[2]`    | Lazy. 1-arg transducer arity raises in v0.1    |
| `find`                | clj  | fn   | `[2]`    | Returns `[k v]` MapEntry or nil                |
| `find-var`            | clj  | fn   | `[1]`    |                                                |
| `first`               | C    | fn   | `[1]`    |                                                |
| `float?`              | C    | fn   | `[1]`    |                                                |
| `flush`               | C    | fn   | `[0]`    |                                                |
| `fn`                  | C    | m    | `[m]`    | C++ at bootstrap, Clojure after `core.clj`     |
| `fn?`                 | C    | fn   | `[1]`    |                                                |
| `fnil`                | clj  | fn   | `[2 3 4]`|                                                |
| `for`                 | clj  | m    | `[m]`    | Comprehension; lazy                            |
| `force`               | clj  | fn   | `[1]`    |                                                |
| `format`              | C    | fn   | `[1+]`   | Delegates to host printf-style                 |
| `frequencies`         | clj  | fn   | `[1]`    |                                                |
| `future`              | clj  | m    | `[m]`    | Schedules a body on the protoCore thread pool  |
| `get`                 | C    | fn   | `[2 3]`  | Third arg is default                           |
| `get-in`              | clj  | fn   | `[2 3]`  |                                                |
| `get-method`          | clj  | fn   | `[2]`    |                                                |
| `group-by`            | clj  | fn   | `[2]`    |                                                |
| `hash`                | C    | fn   | `[1]`    | Consistent with `=`                            |
| `hash-map`            | C    | fn   | `[0+]`   |                                                |
| `hash-set`            | C    | fn   | `[0+]`   |                                                |
| `identity`            | C    | fn   | `[1]`    |                                                |
| `if`                  | C    | s    | `[s]`    |                                                |
| `if-let`              | clj  | m    | `[m]`    |                                                |
| `ifn?`                | C    | fn   | `[1]`    |                                                |
| `inc`                 | C    | fn   | `[1]`    |                                                |
| `integer?`            | C    | fn   | `[1]`    |                                                |
| `interleave`          | clj  | fn   | `[2+]`   | Lazy                                           |
| `interpose`           | clj  | fn   | `[2]`    | Lazy                                           |
| `into`                | clj  | fn   | `[1 2]`  | 3-arg transducer form raises in v0.1           |
| `isa?`                | clj  | fn   | `[2 3]`  |                                                |
| `iterate`             | clj  | fn   | `[2]`    | Lazy                                           |
| `js->clj`             | C    | fn   | `[1 2]`  |                                                |
| `juxt`                | clj  | fn   | `[1+]`   |                                                |
| `keys`                | clj  | fn   | `[1]`    |                                                |
| `keyword`             | clj  | fn   | `[1 2]`  | `(keyword "foo")` and `(keyword "ns" "name")`  |
| `keyword?`            | C    | fn   | `[1]`    |                                                |
| `last`                | clj  | fn   | `[1]`    |                                                |
| `lazy-seq`            | clj  | m    | `[m]`    |                                                |
| `let`                 | C    | m    | `[m]`    | C++ at bootstrap, Clojure after `core.clj`     |
| `list`                | C    | fn   | `[0+]`   |                                                |
| `list?`               | clj  | fn   | `[1]`    |                                                |
| `loop`                | C    | m    | `[m]`    | C++ at bootstrap, Clojure after `core.clj`     |
| `map`                 | clj  | fn   | `[2 3+]` | 1-arg transducer raises; 3+ uses multi-coll    |
| `map?`                | clj  | fn   | `[1]`    |                                                |
| `mapcat`              | clj  | fn   | `[2]`    | Lazy                                           |
| `max`                 | clj  | fn   | `[1+]`   |                                                |
| `memoize`             | clj  | fn   | `[1]`    |                                                |
| `merge`               | clj  | fn   | `[0+]`   |                                                |
| `merge-with`          | clj  | fn   | `[2+]`   |                                                |
| `methods`             | clj  | fn   | `[1]`    |                                                |
| `min`                 | clj  | fn   | `[1+]`   |                                                |
| `mod`                 | C    | fn   | `[2]`    |                                                |
| `name`                | clj  | fn   | `[1]`    | Symbol or keyword to its bare-name string      |
| `namespace`           | clj  | fn   | `[1]`    |                                                |
| `neg?`                | clj  | fn   | `[1]`    |                                                |
| `newline`             | C    | fn   | `[0]`    |                                                |
| `next`                | C    | fn   | `[1]`    | Returns nil on empty                           |
| `nil?`                | C    | fn   | `[1]`    |                                                |
| `not`                 | C    | fn   | `[1]`    |                                                |
| `not=`                | C    | fn   | `[1+]`   |                                                |
| `not-any?`            | clj  | fn   | `[2]`    |                                                |
| `not-empty`           | clj  | fn   | `[1]`    |                                                |
| `not-every?`          | clj  | fn   | `[2]`    |                                                |
| `ns`                  | C    | m    | `[m]`    | C++ at bootstrap, Clojure after `core.clj`     |
| `nth`                 | clj  | fn   | `[2 3]`  |                                                |
| `number?`             | C    | fn   | `[1]`    |                                                |
| `odd?`                | clj  | fn   | `[1]`    |                                                |
| `or`                  | clj  | m    | `[m]`    | Short-circuit                                  |
| `partial`             | clj  | fn   | `[1+]`   |                                                |
| `partition`           | clj  | fn   | `[2 3 4]`| Lazy                                           |
| `partition-all`       | clj  | fn   | `[2 3]`  | Lazy                                           |
| `peek`                | C    | fn   | `[1]`    | Type-dependent (last for vec, first for list)  |
| `pop`                 | C    | fn   | `[1]`    | Type-dependent                                 |
| `pos?`                | clj  | fn   | `[1]`    |                                                |
| `pr`                  | C    | fn   | `[0+]`   |                                                |
| `pr-str`              | clj  | fn   | `[0+]`   |                                                |
| `prefer-method`       | clj  | fn   | `[3]`    |                                                |
| `print`               | C    | fn   | `[0+]`   |                                                |
| `print-str`           | clj  | fn   | `[0+]`   |                                                |
| `println`             | C    | fn   | `[0+]`   |                                                |
| `prn`                 | C    | fn   | `[0+]`   |                                                |
| `promise`             | clj  | fn   | `[0]`    |                                                |
| `pst->clj`            | C    | fn   | `[1 2]`  |                                                |
| `py->clj`             | C    | fn   | `[1 2]`  |                                                |
| `quot`                | C    | fn   | `[2]`    |                                                |
| `quote`               | C    | s    | `[s]`    |                                                |
| `range`               | clj  | fn   | `[0 1 2 3]` | Lazy                                        |
| `ratio?`              | C    | fn   | `[1]`    |                                                |
| `re-find`             | clj  | fn   | `[2]`    |                                                |
| `re-matches`          | clj  | fn   | `[2]`    |                                                |
| `re-pattern`          | clj  | fn   | `[1]`    |                                                |
| `re-seq`              | clj  | fn   | `[2]`    | Lazy                                           |
| `read-string`         | C    | fn   | `[1]`    |                                                |
| `realized?`           | clj  | fn   | `[1]`    | On delay/promise/lazy-seq                      |
| `recur`               | C    | s    | `[s]`    |                                                |
| `reduce`              | clj  | fn   | `[2 3]`  | `(f coll)` and `(f init coll)`                 |
| `rem`                 | C    | fn   | `[2]`    |                                                |
| `remove`              | clj  | fn   | `[2]`    | Convenience: `(filter (complement f) coll)`    |
| `remove-method`       | clj  | fn   | `[2]`    |                                                |
| `remove-watch`        | clj  | fn   | `[2]`    |                                                |
| `repeat`              | clj  | fn   | `[1 2]`  | Lazy; `(repeat x)` is infinite                 |
| `repeatedly`          | clj  | fn   | `[1 2]`  | Lazy                                           |
| `reset!`              | C    | fn   | `[2]`    |                                                |
| `rest`                | C    | fn   | `[1]`    |                                                |
| `reverse`             | clj  | fn   | `[1]`    | Eager                                          |
| `satisfies?`          | clj  | fn   | `[2]`    |                                                |
| `select-keys`         | clj  | fn   | `[2]`    |                                                |
| `seq`                 | C    | fn   | `[1]`    | Returns nil on empty                           |
| `seq?`                | clj  | fn   | `[1]`    |                                                |
| `sequential?`         | clj  | fn   | `[1]`    |                                                |
| `set`                 | clj  | fn   | `[1]`    | Convert any coll to a set                      |
| `set?`                | clj  | fn   | `[1]`    |                                                |
| `slurp`               | clj  | fn   | `[1]`    | Reads a file path (no opt args in v0.1)        |
| `some`                | clj  | fn   | `[2]`    |                                                |
| `some?`               | C    | fn   | `[1]`    |                                                |
| `some->`              | clj  | m    | `[m]`    |                                                |
| `some->>`             | clj  | m    | `[m]`    |                                                |
| `some-fn`             | clj  | fn   | `[1+]`   |                                                |
| `sort`                | clj  | fn   | `[1 2]`  | 2-arg form takes comparator                    |
| `sort-by`             | clj  | fn   | `[2 3]`  |                                                |
| `spit`                | clj  | fn   | `[2]`    | Writes a file path (no opt args in v0.1)       |
| `str`                 | C    | fn   | `[0+]`   |                                                |
| `string?`             | C    | fn   | `[1]`    |                                                |
| `subs`                | C    | fn   | `[2 3]`  |                                                |
| `swap!`               | C    | fn   | `[2 3 4 5+]` | Up to 4 args after the fn                  |
| `symbol`              | clj  | fn   | `[1 2]`  |                                                |
| `symbol?`             | C    | fn   | `[1]`    |                                                |
| `take`                | clj  | fn   | `[2]`    | Lazy                                           |
| `take-while`          | clj  | fn   | `[2]`    | Lazy                                           |
| `throw`               | C    | s    | `[s]`    |                                                |
| `true?`               | C    | fn   | `[1]`    |                                                |
| `try`                 | C    | s    | `[s]`    | With `catch` / `finally`                       |
| `update`              | clj  | fn   | `[3+]`   |                                                |
| `update-in`           | clj  | fn   | `[3+]`   |                                                |
| `vals`                | clj  | fn   | `[1]`    |                                                |
| `var`                 | C    | s    | `[s]`    |                                                |
| `var-get`             | C    | fn   | `[1]`    |                                                |
| `var-set`             | C    | fn   | `[2]`    | Dynamic vars only                              |
| `vec`                 | clj  | fn   | `[1]`    |                                                |
| `vector`              | C    | fn   | `[0+]`   |                                                |
| `vector?`             | clj  | fn   | `[1]`    |                                                |
| `volatile!`           | clj  | fn   | `[1]`    |                                                |
| `vreset!`             | clj  | fn   | `[2]`    |                                                |
| `vswap!`              | clj  | fn   | `[2+]`   |                                                |
| `when`                | C    | m    | `[m]`    | C++ at bootstrap, Clojure after `core.clj`     |
| `when-let`            | clj  | m    | `[m]`    |                                                |
| `when-not`            | clj  | m    | `[m]`    |                                                |
| `with-open`           | clj  | m    | `[m]`    |                                                |
| `zero?`               | clj  | fn   | `[1]`    |                                                |
| `zipmap`              | clj  | fn   | `[2]`    |                                                |
| `->`                  | clj  | m    | `[m]`    |                                                |
| `->>`                 | clj  | m    | `[m]`    |                                                |

**Totals.**

- C++ specials: 11 listed
- C++ primitive functions: 60 listed
- C++ bootstrap macros: 7 listed
- Clojure-defined: 99 listed
- **Total v0.1 surface: ~177 names**

That is higher than the "~80" target estimate I committed to in
casual conversation, and lower than the "~200" reading of the full
Clojure cheatsheet. The truth is in the middle and it landed where
the practical floor for "feels like Clojure" actually is. A user who
hits a missing name in a real workload immediately notices; a user
who never reaches for `merge-with` does not. Erring on the side of
"this name is present and works" is the right call for v0.1.

---

## 6. Arity discipline

A few functions have multiple arities in JVM Clojure. v0.1 ships
the common ones and explicitly raises on the rest, with a clear
"v0.2" message in the error.

| Function     | v0.1 arities ship                  | v0.2 adds                       |
|--------------|------------------------------------|---------------------------------|
| `map`        | `(map f coll)`, `(map f c1 c2)`    | `(map f)` transducer, 3+ colls  |
| `filter`     | `(filter pred coll)`               | `(filter pred)` transducer      |
| `remove`     | `(remove pred coll)`               | `(remove pred)` transducer      |
| `reduce`     | `(reduce f coll)`, `(reduce f init coll)` | (none — already complete) |
| `apply`      | up to 4 prepended args + final seq | unlimited prepended args        |
| `into`       | `(into coll src)`, `(into)`        | 3-arg transducer form           |
| `partition`  | `(partition n coll)`, `(partition n step coll)`, `(partition n step pad coll)` | (none) |
| `swap!`      | up to 4 args after `f`             | unlimited                       |
| `sort`       | `(sort coll)`, `(sort cmp coll)`   | (none)                          |
| `range`      | `(range)`, `(range end)`, `(range start end)`, `(range start end step)` | (none) |

The implementation pattern in `core.clj` for arities that raise:

```clojure
(defn map
  "Returns a lazy sequence ..."
  ([f coll]
   (lazy-seq
     (when-let [s (seq coll)]
       (cons (f (first s)) (map f (rest s))))))
  ([f c1 c2]
   (lazy-seq
     (let [s1 (seq c1) s2 (seq c2)]
       (when (and s1 s2)
         (cons (f (first s1) (first s2))
               (map f (rest s1) (rest s2)))))))
  ([f]
   (throw (ex-info
            "v0.1 does not implement the transducer arity of map (v0.2)"
            {:fn 'map :missing :transducer-arity})))
  ([f c1 c2 c3 & colls]
   (throw (ex-info
            "v0.1 does not implement the 3+-coll arity of map (v0.2)"
            {:fn 'map :missing :many-colls}))))
```

The deferred arity is a *real* clause that raises, not a missing
clause. The user gets a precise error pointing at the version that
will add support.

---

## 7. Foreign-object dispatch

A subtle but important point. Several `clojure.core` functions
(`count`, `seq`, `first`, `rest`, `get`, `contains?`, `conj`) are
called on Clojure-native collections AND on foreign UMD objects
(Python lists, JS arrays, ProtoST collections).

The protocols `Counted`, `Seqable`, `Indexed`, `Associative`,
`ICollection` are *declared in `core.clj`* and *extended to each
foreign collection type at UMD import time*.

The bootstrap of these protocols runs in two phases:

1. **At `core.clj` load**: `(defprotocol Seqable (-seq [coll]))`,
   `(defprotocol Counted (-count [coll]))`, etc., and an
   `extend-type` block per Clojure-native collection type (vector,
   map, set, list, string).
2. **At UMD provider registration**: each provider (`py/`, `js/`,
   `pst/`) registers methods on the foreign-language collection
   prototypes. Loading the Python provider extends `Seqable` and
   `Counted` to Python `list`, Python `dict`, Python `tuple`,
   Python `str`. Loading the JS provider extends to JS `Array`,
   JS `Map`, etc.

After both phases, `(count py-list)` works because protoCore looks
up the `Counted` protocol method on the Python list's prototype,
finds it, and calls. The dispatch is one attribute walk, just like
any other Clojure protocol call.

The user code does not see this — `count`, `seq`, `first`, etc.
just work on foreign objects. The plumbing is in the UMD providers,
loaded once.

---

## 8. Explicit omissions

The following names are reserved in `clojure.core` (calling them
raises a clear `v0.2` or `v0.3` error) but do not ship a working
implementation in v0.1.

**Reserved for v0.2:**

```
ref     dosync       ref-set     alter      commute     ensure
defrecord    deftype      definterface
proxy        reify
trampoline   eduction
clojure.set/union, intersection, difference
clojure.string/upper-case, lower-case, split, join, replace, trim, etc.
clojure.walk/postwalk, prewalk, walk
clojure.edn/read, read-string
clojure.pprint/pprint, pp
```

**Reserved for v0.3:**

```
agent   send    send-off    await   await-for
core.async/chan, >!, <!, go, go-loop, etc.
```

**Permanently out of scope** (not implementing on this substrate):

```
clojure.java.*      ; entire JVM interop family
.method   Class.   Class/static    proxy-with-super
gen-class  gen-interface  bean
monitor-enter  monitor-exit
clojure.lang.RT/* ; JVM-internal vars exposed by accident
```

Every name in any of these three categories that a user calls
raises `Symbol X is not implemented in this dialect (see
docs/STATUS.md Dnn)` with the right deviation ID.

---

## 9. File layout when implementation starts

Anticipating the source-tree layout so contributors have a target:

```
protoClojure/
├── src/
│   ├── reader/           # the .clj → AST reader
│   ├── compiler/         # AST → bytecode + macro expansion driver
│   ├── runtime/          # the bytecode VM, modeled on protoST
│   ├── primitives/       # the ~67 C++ primitive functions, one file per category
│   │   ├── arith_prims.cpp
│   │   ├── compare_prims.cpp
│   │   ├── collection_prims.cpp
│   │   ├── atom_prims.cpp
│   │   ├── string_prims.cpp
│   │   ├── print_prims.cpp
│   │   ├── error_prims.cpp
│   │   ├── interop_prims.cpp     # clj->py, py->clj, etc.
│   │   └── ...
│   ├── bootstrap_macros/ # the seven C++ bootstrap macros
│   ├── nrepl/            # bencode + TCP server
│   └── main.cpp
├── resources/
│   └── clojure/
│       └── core.clj      # the ~85 Clojure-defined names
├── test/                 # GoogleTest for the C++ parts
└── examples/             # .clj scripts
```

`resources/clojure/core.clj` is shipped with the binary and loaded
at startup from a known path or an embedded resource (the build
embeds it as a string constant — same trick `lein` plays for its
bundled scripts).

---

## 10. Definition of done for `clojure.core` v0.1

A name in §5 is **done** when:

1. It appears in `clojure.core` and resolves at the REPL.
2. Its v0.1 arities work as the JVM Clojure docs describe (or, where
   we deliberately diverge, as `docs/LANGUAGE.md` describes).
3. Its deferred arities raise the right error with the right
   deviation ID.
4. It has a docstring (copied from JVM Clojure with attribution; we
   are not rewriting them).
5. It is exercised by at least one test in `test/core/` (for C++
   primitives) or by a fixture in `examples/conformance/` (for
   Clojure-defined names).

`clojure.core` is **done** as a whole when:

1. Every name in §5 meets the above.
2. The bootstrap completes in under 100ms cold (50ms target, 100ms
   ceiling).
3. A side-by-side test runs the same set of ~30 idiomatic snippets
   against `protoclj` and `clj` (JVM Clojure) and they produce
   equivalent results modulo documented deviations.

The side-by-side comparison is the v0.1 release gate.

---

## 11. Revisions

When this spec is changed after implementation begins, the
revisions are appended below with date, reason, and SHA of the
commit that updated both the spec and the code.

(empty for now)
