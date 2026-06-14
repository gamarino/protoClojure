# 3. For the Clojure Developer

If you have been writing Clojure-JVM, the syntax and the data model
will be exactly what you expect. The reader is standard, the
collections are persistent, `assoc` returns a new map. This chapter
covers the parts where protoClojure is *not* exactly Clojure-JVM, in
the order a working Clojure programmer is likely to hit them.

The most important framing: **protoClojure is a dialect, not a port.**
We try to keep the surface idiomatic; we do not try to be
bytecode-, tooling-, or library-compatible with Clojure-JVM. Where
JVM-specific design would not earn its keep on this substrate, we
diverge. Every divergence is recorded in [`STATUS.md`](../STATUS.md)
under a `Dnn` id.

## 3.1 What is identical

Read these and feel at home:

- The reader. `'`, `` ` ``, `~`, `~@`, `#()`, `#{}`, `#'`, `#_`, `^`
  metadata, `:keyword`, `::ns-keyword`, namespaced symbols. All as you
  know them.
- The four core collections. Vectors, maps, sets, lists. Persistent,
  with `O(log n)` operations. `assoc`, `dissoc`, `update`, `merge`,
  `select-keys`, `get-in`, `assoc-in`, `update-in`.
- Truthiness. `nil` and `false` are falsy; *everything else* is truthy.
  `(if 0 :y :n)` is `:y`. `(if "" :y :n)` is `:y`.
- The `=` semantics. Structural value equality on collections, numeric
  equality across number types via `==`, identity via `identical?`.
- Lazy sequences. `map`, `filter`, `take`, `range`, `iterate`,
  `repeat`, `cycle`, `partition`, `concat`, `mapcat` are all lazy.
  (See §3.4 for one performance caveat.)
- Higher-order vocabulary. `comp`, `partial`, `juxt`, `complement`,
  `every-pred`, `some-fn`, `apply`, `reduce`.
- Threading macros. `->`, `->>`, `as->`, `some->`, `some->>`.
- `defn` with multi-arity, variadic, docstring, attribute map.
- `let`, `loop`, `recur`. `recur` is enforced tail-position. No
  automatic TCO outside `recur` (same as JVM Clojure).
- Closures. Captured by value at creation; mutate through `atom`.
- Namespaces. `ns`, `:require`, `:as`, `:refer`, `:refer :all` (still
  discouraged). `in-ns`, `create-ns`, `find-ns`, `all-ns`,
  `ns-publics`, `ns-refers`, `ns-aliases`, `ns-unmap`.
- Vars. `def`, `defonce`, `^:dynamic`, `binding`. `alter-var-root`
  (with a warning).
- Atoms. `atom`, `swap!`, `reset!`, `compare-and-set!`, `deref` / `@`,
  `add-watch`, `remove-watch`.
- Volatiles. `volatile!`, `vreset!`, `vswap!`.
- Delays and promises. `delay`, `force`, `realized?`, `promise`,
  `deliver`.
- Exceptions. `try`/`catch`/`finally`, `throw`, `ex-info`,
  `ex-message`, `ex-data`, `ex-cause`.
- `pr-str`, `print-str`, `println`, `prn`, `pr`, edn-style printing.

If your code uses only the above, the migration is mostly a recompile.

## 3.2 What is gone, and the substitute

### 3.2.1 Java interop — D1, D2

The entire `(.method obj args)`, `(Class/staticMethod ...)`, `Class.`,
`proxy`, `reify` (as a JVM interface implementer), `gen-class`,
`gen-interface` family is gone. There is no JVM here.

The substitute, when applicable:

- For "this Java library does X" → check whether Python or JavaScript
  has an equivalent and use it via the UMD interop (Chapter 9):
  `[py/X :as ...]` or `[js/X :as ...]`.
- For `reify` to implement a Clojure protocol on the fly → still
  works; protocols are not JVM interfaces in this dialect, they are
  attribute-set extensions on protoCore prototypes. `defprotocol`
  + `extend-type` and `extend-protocol` are supported. `reify` is
  syntactic sugar over an anonymous protocol implementation and
  works.
- For `proxy` to wrap a Java class → no equivalent; the use case is
  Java-specific.
- For `java.time` / `java.util.Date` → either the Python `datetime`
  module or a future protoClojure-native time library.
- For `java.util.regex` → protoClojure regex is backed by the host
  C++ `std::regex` for v0.1. `#"..."` reader literal works.

### 3.2.2 STM and refs — D6 (v0.2)

`ref`, `dosync`, `ref-set`, `alter`, `commute`, `ensure` all raise a
"not yet implemented (v0.2)" error in v0.1. The implementation plan
is in [`DESIGN.md`](../DESIGN.md) §6 — STM is genuinely scheduled,
not abandoned, but it is *not* in v0.1.

If your code uses STM right now, you have three workable options for
the v0.1 window:

1. Use atoms with care. Most ref use cases can be re-expressed as a
   single atom holding a map; `swap!` over the whole map is atomic.
2. Use the protoCore actor model directly (see protoST for the API)
   for genuinely actor-shaped workloads.
3. Wait for v0.2.

### 3.2.3 Agents — D7 (v0.3)

`agent`, `send`, `send-off`, `await`, `await-for` raise in v0.1.
Implementation is on the v0.3 milestone, on top of the protoCore
actor primitive. We deliberately do not lift the JVM design 1:1 —
some of agent semantics is shaped by the JVM thread pool, and the
protoCore actor model has a different cost structure.

### 3.2.4 `core.async` — D8 (v0.3+ or never)

The CSP / channels layer is not on the v0.1 roadmap and may never
arrive in 1:1 form. protoCore provides actors and futures as
primary concurrency primitives, with a different cost model from
`core.async` channels. A "CSP-shaped layer on top of protoCore
actors" is plausibly a v0.3 deliverable; an exact port of
`core.async` is unlikely. If your application is heavily structured
around `core.async`, protoClojure v0.1 is not for you yet — say so,
we will prioritise.

### 3.2.5 `defrecord` and `deftype` — D9 (v0.2)

Not in v0.1. The argument for skipping in v0.1: a `defrecord` is
mostly a typed map with protocol method dispatch attached, and a
plain map plus a protocol get you most of the way there.
`defprotocol` + `extend-type` over `clojure.lang.IPersistentMap` is
the v0.1 idiom. We will revisit in v0.2.

### 3.2.6 `clojure.java.*` namespaces — D12

None of `clojure.java.io`, `clojure.java.jdbc`,
`clojure.java.javadoc`, etc. exist. For file I/O, use `slurp`,
`spit`, `with-open` against host-provided file handles; for JDBC,
use a Python-or-JS UMD substitute (`py/sqlite3`, `js/better-sqlite3`).

### 3.2.7 `clojure.spec`, BigDecimal `M`, transducers, chunked seqs

All scheduled (`STATUS.md`) but not in v0.1. The 2-arg seq forms of
`map`/`filter`/etc. work; the 1-arg transducer arity raises.

## 3.3 What is different but compatible

### 3.3.1 Characters — D3

There is no separate `Character` type. `\a` reads, but it returns a
1-codepoint `String` rather than a `java.lang.Character`. `char?`
returns `true` only for 1-codepoint strings; `(char 65)` returns
`"A"`. Code that uses chars structurally (in regex matches, in
string operations) keeps working. Code that does `(instance?
Character x)` does not — there is no `Character` class to be an
instance of.

### 3.3.2 Numbers and overflow

protoCore has a unified numeric tower: `SmallInteger` (56-bit inline,
tagged immediate), `LargeInteger` (arbitrary precision heap),
`Float` (double). Arithmetic auto-promotes between integer sizes —
identical to JVM Clojure since 1.3. There is no `int`/`long`
distinction at the user level. There is no `BigDecimal` in v0.1.
`Ratio` works as expected.

### 3.3.3 Strings

A `ProtoString` is rope-backed UTF-8 with `O(log n)` `subs` /
concatenation. Mostly transparent — your code works — with two
observations:

- `subs` is cheap; reach for it freely.
- `(count s)` is in codepoints, not UTF-16 code units. If you have
  surrogate-pair-aware Java code, this is actually a simplification.

### 3.3.4 Regex

Backed by C++ `std::regex` (ECMAScript syntax). Most patterns will
work; some Java-specific regex features (named groups by some
syntaxes, certain POSIX classes) may differ. If you find a
divergence, file a `STATUS.md` deviation.

### 3.3.5 The reader and `read-string` strictness — D13

Reader literals that are not registered (`#myorg/foo`) raise on read.
JVM Clojure is more permissive here. We are strict to surface typos
in the data layer. Register your literals.

## 3.4 What is new

### 3.4.1 Cross-runtime UMD interop

The whole reason this dialect exists. From a Clojure namespace:

```clojure
(:require [py/numpy   :as np]
          [js/lodash  :as _]
          [pst/Counter :as ctr])
```

The `py/`, `js/`, `pst/` prefixes resolve through the universal
module discovery the rest of the protoCore ecosystem already uses.
The result of `np/array` is a real callable protoCore object you can
pass around like any other value. `clj->py`, `py->clj`, `clj->js`,
`js->clj` are the explicit conversion helpers for collections.

The complete story is in [Chapter 9](09-interop.md) of this
tutorial and in [`INTEROP.md`](../INTEROP.md).

### 3.4.2 The substrate is *natively* persistent

This is a subtle but real win. In JVM Clojure, `assoc` is a method
call on a Java class that implements `IPersistentMap`. There is a
small boxing / unboxing tax at every JVM boundary. In protoClojure,
`assoc` is a `setAttribute` on a protoCore mutable-prototype object,
which is *also* what every other language on the kernel uses.
There are no boundaries to box across.

Practical effect: certain collection operations are measurably
cheaper than the JVM equivalent. Where Babashka pays a GraalVM AOT
cost, protoClojure pays nothing extra — the data model *is* the
kernel.

### 3.4.3 No GIL, real OS threads

A `future` is a real protoCore future running on a real thread, in
parallel, no global lock. The Clojure `pmap` works the way the API
promised on the JVM, without the JVM thread costs. This is the
killer feature for CPU-bound work on multi-core hardware.

### 3.4.4 Atoms are exposed at kernel level

The protoCore `setAttributeIfEqual` CAS primitive is the same
primitive `swap!` uses, with one less indirection than the JVM
implementation. `protoST` already exposes this primitive at the
language level; we inherit the surface.

## 3.5 Where to look next

- **The full deviation table**: [`LANGUAGE.md`](../LANGUAGE.md) §13
  and [`STATUS.md`](../STATUS.md).
- **The interop story**: [`INTEROP.md`](../INTEROP.md) and Chapter 9.
- **What is scheduled for v0.2 / v0.3**: [`ROADMAP.md`](../ROADMAP.md).
- **Why the design is the way it is**: [`DESIGN.md`](../DESIGN.md).

If you want to write Clojure that runs against `protoclj` and you do
not lean heavily on `defrecord`, STM, agents, transducers, JVM
interop, or `core.async`, v0.1 should feel like home. If you do lean
on any of those, the gap is documented and the path is on the
roadmap.
