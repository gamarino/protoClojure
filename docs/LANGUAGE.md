# The protoClojure Language Reference

> Authoritative. If a tutorial chapter and this document disagree, this
> document is correct and the tutorial has a bug.

This is the **design reference** for protoClojure — the surface the
implementation is converging on. The table in §1.2 below summarises
what protoClojure 0.0.1 implements and what is planned.
`docs/STATUS.md` is the living tracker; if a feature you expect is not
listed there as implemented, it is not implemented.

Sections marked **(planned)** describe v0.1 behaviour the current build
does not implement; sections marked **[v0.2]** or **[v0.3]** describe
later versions. Sections without a tag describe the v0.1 design and may
be partly implemented — check STATUS for the exact line.

---

## 1. Overview

protoClojure is a **dialect of Clojure** that runs on the protoCore
object kernel. It accepts the same surface syntax as Clojure (Lisp with
vectors, maps, and sets as primary literals), the same evaluation rules
(reader → analyser → interpreter, with macros), and an intentionally
compatible subset of `clojure.core`. It is not bytecode-compatible with
JVM Clojure and does not pretend to host or be hosted by the JVM.

### 1.1 Design stance

Three priorities, in order:

1. **Idiom over performance.** Clojure programmers must read protoClojure
   code and recognise it.
2. **REPL-driven development is the workflow.** The REPL is first-class;
   nREPL compatibility is planned for v0.1.
3. **Honesty about gaps.** Every departure from Clojure-JVM is
   documented in `STATUS.md`. Unimplemented forms raise; they do not
   silently return `nil`.

### 1.2 Implementation status at a glance (protoClojure 0.0.1)

This snapshot exists so a reader picking up the language reference can
tell at a glance which parts of the surface are runnable today. The
**living** tracker is `docs/STATUS.md`.

| Surface area | Status |
|---|---|
| Numbers — integers (SmallInteger + promoted LargeInteger) | Implemented |
| Numbers — IEEE-754 floats (`3.14`, `1e6`) | Implemented |
| Numbers — ratios `1/3` | Planned |
| Strings | Implemented (literals, `str`, `println`) |
| Strings — `clojure.string`-shaped ops (`split`, `join`, `upper-case`, …) | Implemented, in the global namespace |
| Symbols, keywords, booleans, nil literals | Implemented |
| Reader macros `'`, `` ` ``, `~`, `~@`, `#(...)`, `#'`, `#_`, `^` | Planned (only `@` today; `(quote atom)` works as a special form) |
| Lists `(...)` | Implemented |
| Vectors `[...]` | Implemented (ProtoTuple, distinct from list) |
| Maps `{...}` | Implemented (`hash-map`, `assoc`, `get`, `contains?`, `keys`, `vals`) |
| Sets `#{...}` | Planned |
| `def`, `defn`, `fn`, `let`, `loop`, `recur` | Implemented (`let` and `loop` inside function bodies only) |
| Multi-arity `defn` | Implemented |
| Variadic `& rest` | Implemented |
| `if`, `do`, `quote`, `apply` | Implemented (`quote` of atoms only; `apply` over a list) |
| `when`, `when-not`, `cond`, `and`, `or` | Implemented |
| `throw`, `try`, `catch`, `finally`, `ex-info` | Planned |
| Closures with N-level lexical capture | Implemented |
| Named arguments `& {:keys [...] :or {...} :as m}` | Implemented |
| Namespaces (`ns`, `:require`, `:as`, `:refer`) | Planned |
| State (`atom`, `swap!`, `reset!`, `compare-and-set!`, `@`, watches) | Implemented |
| `future`, `promise`, `deliver`, `pmap` | Implemented |
| Actors (`actor`, `send`, `send-h`, `send-l`) | Implemented (see tutorial chapter 13) |
| Lazy seqs (`lazy-seq`, `range`, `iterate`) | Planned |
| User-defined macros (`defmacro`) | Planned |
| Local interactive REPL | Implemented |
| nREPL server | Planned for v0.1 |
| UMD providers (`py/`, `js/`, `pst/`, `clj/`) | Planned |

Example A in §1.3 runs today; examples B and C show the v0.1 target
surface and do not run yet.

### 1.3 A first example

**Example A — runs today.**

```clojure
(defn greet [who]
  (println "hello," who))

(defn -main [] (greet "world"))

(-main)   ;; => hello, world
```

Run:

```bash
$ protoclj greeting.clj
hello, world
```

**Example B — target shape for v0.1 (planned).**

```clojure
(ns demo.greeting
  (:require [clojure.string :as str]))

(defn greet [who]
  (str/join " " ["hello" who]))

(defn -main [& args]
  (println (greet (or (first args) "world"))))
```

This will run once `ns` and `:require` land and `clojure.string` is
available as a namespace (see `ROADMAP.md`).

**Example C — cross-runtime interop, target shape (planned).**

```clojure
(ns demo.numpy-bridge
  (:require [py/numpy :as np]
            [clojure.string :as str]))

(defn row-sums [matrix-of-vecs]
  (let [arr (np/array (clj->py matrix-of-vecs))
        sums (np/sum arr :axis 1)]
    (py->clj sums)))

(println (row-sums [[1 2 3] [4 5 6] [7 8 9]]))
;; => [6 15 24]
```

`py/numpy` resolves through the UMD registry just like a Clojure
namespace; `np/array` is a method call on the resulting module object.
`clj->py` / `py->clj` are explicit conversion functions for collections
that need a representation change (see §11 *Interop*).

---

## 2. Lexical structure

The protoClojure 0.0.1 reader implements integers, floats, strings,
symbols, keywords, booleans, `nil`, lists, vectors, maps, `@`, line
comments and commas as whitespace. The rest of this section is planned.

### 2.1 Whitespace and comments

Whitespace separates tokens. Commas count as whitespace (an unusual
Clojure inheritance — `[1, 2, 3]` is identical to `[1 2 3]`). A line
comment begins with `;` and runs to end of line. A form-discarding
comment is `#_<form>` — the next form is read and dropped.

### 2.2 Numbers

- **Integer**: `42`, `-7`, `0xff` (hex), `0b1010` (binary), `2r1010`
  (radix-N). Range: protoCore `SmallInteger` (signed 54-bit, stored
  inline in the tagged pointer) and `LargeInteger` (heap-allocated
  arbitrary precision) — overflow promotes transparently.
- **Ratio**: `3/4`, `-22/7`. Stored as a normalised pair of integers.
  Arithmetic preserves exact rationality unless mixed with a float.
- **Float**: `3.14`, `1e10`, `-0.5e-3`. IEEE 754 double.
- **BigDecimal**: `3.14M` — **[v0.2]**.

### 2.3 Strings

Double-quoted, backslash-escaped: `"hello\n"`. The escape set is the
Clojure set: `\n \t \r \\ \"` plus `\uXXXX` unicode and `\oNNN` octal.
A string is a protoCore `ProtoString` (rope-backed UTF-8). Multi-line
strings are written across newlines:

```clojure
"line one
line two"
```

`(str x ...)` concatenates its arguments rendered exactly as `println`
prints them; `str`, `println`, `join` and the REPL share one printer.
`(str 1 :a "b")` is `"1:ab"`, `(str {:a 1 :b [2]})` is `"{:a 1, :b [2]}"`,
and a reference type renders as a tag: `(str (atom 1))` is `"#<atom 1>"`,
likewise `#<future ...>`, `#<promise ...>`, `#<actor ...>` and `#<fn>`
(JVM Clojure renders `#object[clojure.lang.Atom 0x... {:status :ready, :val 1}]`).
Strings nested in a collection render without quotes: `(str ["a"])` is
`"[a]"` (deviation D20 in `STATUS.md`).

### 2.4 Characters

`\a`, `\space`, `\tab`, `\newline`, `é`. A character is a one-codepoint
`ProtoString`; we do *not* introduce a separate character type in v0.1
(see *Departures from Clojure-JVM*, §13).

### 2.5 Symbols and keywords

A **symbol** is an identifier: `foo`, `+`, `*ear-muffs*`,
`my.namespace/foo`. The reader produces a symbol; the evaluator resolves
it. Allowed characters: letters, digits, and `* + ! - _ ' ? < > = . / :`.
Letters and digits are not limited to ASCII: a source file is UTF-8, and
any Unicode letter, combining mark or decimal digit may appear in a symbol
or keyword (`año`, `café`, `:ñandú`, `:日本語`). Other non-ASCII characters,
such as `→` or a no-break space, and malformed UTF-8 are read errors
(deviation D21 in `STATUS.md`). The first character cannot be a digit. A symbol with `/` is *namespace-
qualified*: `my.app/handler` means symbol `handler` in namespace `my.app`.

A **keyword** is a symbol with a leading `:`. Keywords are interned and
self-evaluating — they evaluate to themselves. Idiomatic for map keys.
`::foo` is a namespace-qualified keyword in the current namespace.

At run time a keyword, and a symbol produced by `quote`, is an interned
value of its own kind, never a string: `(= :a ":a")` and
`(= (quote a) "a")` are false, `(string? :a)` is false, and `:a` and `":a"`
are two different map keys. `(str :a)` is the string `":a"`. A keyword is
also a function of a map: `(:a {:a 1})` is `1` (§4.3).

### 2.6 Booleans and nil

`true`, `false`, `nil`. As in Clojure, `false` and `nil` are the only
falsy values; everything else is truthy (`0`, `""`, `'()` are all
truthy).

### 2.7 Collection literals

- **List**: `(a b c)` — when quoted: `'(a b c)`. An unquoted list at
  reader position is a *call form*: the first element is the function /
  macro / special form, the rest are arguments.
- **Vector**: `[a b c]` — always a literal value, never a call form.
- **Map**: `{:a 1 :b 2}` — pairs (key value); iteration and printing
  follow insertion order, equality ignores order.
- **Set**: `#{:a :b :c}` — unordered, deduplicated.

### 2.8 Reader macros

- `'form` → `(quote form)`
- `` `form `` → quasiquote (syntax-quote) — recursive auto-resolves
  symbols against the current namespace; `~x` unquotes, `~@x` splices.
- `#(...)` → anonymous function: `#(+ 1 %)` is `(fn [%] (+ 1 %))`.
  Args are `%`, `%1`..`%9`, `%&` (rest).
- `#{...}` → set literal (already covered).
- `#'sym` → var-quote: produces the var, not the value (`@#'my-fn`
  returns the current value).
- `#_form` → discard the next form.
- `^{:a 1} form` → attach metadata to `form`. `^kw` is `{:tag kw}`;
  `^Type` is `{:tag Type}`.
- `@form` → `(deref form)` (implemented).

### 2.9 Reader literals

`#inst "..."` and `#uuid "..."` are reserved. v0.1 reads them as the
tagged forms but does not yet ship `inst?` / `uuid?` predicates.

---

## 3. Evaluation

### 3.1 The reader

`(read-string s)` parses one form. `(read rdr)` reads from a reader
object. Errors carry source position. (Planned: `read-string` and
`read` are not available in 0.0.1; the reader is used internally.)

### 3.2 The evaluator

Each top-level form is read, compiled to bytecode, and evaluated. Forms
are evaluated by case:

- Self-evaluating literals (numbers, strings, keywords, booleans, nil,
  vectors of constants, maps of constants, sets of constants) return
  themselves.
- Symbols resolve through the lexical scope first, then the current
  namespace's vars, then `clojure.core` referred vars, then namespace
  aliases. An unresolved symbol throws `Unable to resolve symbol`.
- A list `(f arg1 arg2 …)` evaluates `f`. If `f` is a special form, its
  special rule applies. If `f` is a macro, the macro is expanded and the
  result is evaluated. Otherwise `f` is evaluated to a value (must be a
  function), the args are evaluated left-to-right, and the function is
  applied.

In 0.0.1 there are no namespaces or macros: symbols resolve through the
lexical scope and then a single global table.

### 3.3 Special forms

`def`, `if`, `do`, `let*`, `loop*`, `recur`, `fn*`, `quote`, `var`,
`throw`, `try`, `monitor-enter` / `monitor-exit` are reserved (the last
two raise an explicit "JVM-specific, not supported" error in v0.1). The
user-facing forms `let`, `loop`, `fn`, `defn`, `if-let`, `when`, `cond`,
etc., are macros built on these specials.

In 0.0.1 the C++ compiler handles `def`, `if`, `do`, `quote`, `fn`,
`defn`, `let`, `loop`, `recur`, `when`, `when-not`, `cond`, `and`, `or`,
`apply` and `future` directly; the `*` forms, `var`, `throw`, `try` and
`if-let` are not implemented.

### 3.4 Truthiness

`if`, `when`, `and`, `or`, `cond`, `if-let`, `when-let`: only `false`
and `nil` are falsy. `(if 0 :y :n)` is `:y`. `(if "" :y :n)` is `:y`.
This is JVM Clojure semantics; it differs from Python (`0` falsy) and
JavaScript (`""` falsy). The protoClojure runtime stays strict on this
to keep the idiom intact.

---

## 4. Data structures

All four core structures are persistent (every "modification" returns a
new value; the old value is unchanged) and immutable by default.

In 0.0.1, lists, vectors and maps are implemented with the primitives
listed in `STATUS.md`. Sets, `conj`, `dissoc`, `update`, `merge`,
vectors as functions, and the parts of the equality
rules of §4.5 listed there as not implemented are planned.

### 4.1 List

A singly-linked persistent list backed by `ProtoList`. Cheap `conj` at
head, `O(1)` `first` / `rest`, `O(n)` random access.

```clojure
(conj '(2 3) 1)   ;; => (1 2 3)
(first '(a b c))  ;; => a
(rest '(a b c))   ;; => (b c)
```

### 4.2 Vector

Indexed sequential collection, backed by protoCore `ProtoTuple`.
`O(log n)` random access, `O(log n)` `conj` at tail, `O(log n)` `assoc`.

```clojure
(conj [1 2] 3)         ;; => [1 2 3]
(assoc [10 20 30] 1 0) ;; => [10 0 30]
(nth [1 2 3] 0)        ;; => 1
([1 2 3] 0)            ;; => 1 (vectors are functions of their indices)
```

### 4.3 Map

Map of key→value pairs. Keys can be any value supporting `=` / `hash`.

**Order.** A map keeps insertion order at every size: `keys`, `vals`,
printing and every other walk visit entries in the order their keys were
first added. `assoc` of a new key appends it; `assoc` of an existing key
replaces the value and keeps the key's position; removing a key keeps the
order of the others. Equality ignores order. (JVM Clojure
guarantees insertion order only for array maps of at most 8 entries;
deviation D19 in `docs/STATUS.md`.)

**Equality and hashing.** Two maps are `=` when they have the same number
of entries and every key of one is mapped to an `=` value in the other;
insertion order is irrelevant, and values are compared recursively, so
nested maps, vectors and lists compare by value (§4.5). A map is never `=`
to a vector or a list. Keys are hashed with a value hash consistent with `=`
and matched with `=`, so a key matches by value: `(get {{:a 1} :x} {:a 1})`
is `:x`, a vector key is found with an equal list, and an integer key is
found with an equal float (`(get {1 :x} 1.0)` is `:x`, deviation D15). Keys
that are `=` are one key: `(hash-map 1 :a 1.0 :b)` is `{1 :b}`, keeping the
first key and the last value. A keyword is never the same key as the string
of its spelling: `(get {:a 1} ":a")` is `nil`, and `(hash-map :a 1 ":a" 2)`
has two entries. The hash ignores map insertion order, is the
same for a list and a vector with equal elements, and hashes numbers by
value modulo 2^61 − 1, so equal numbers hash equally whatever their
representation (SmallInteger, LargeInteger, double). Hashes are not cached:
a collection key is hashed in full on every lookup. NaN is the exception:
protoCore's numeric comparison reports NaN `=` to every number, which no
hash can agree with; NaN hashes like `0`, so a NaN key is matched only by
`0`, `0.0`, `-0.0`, NaN and integers that are multiples of 2^61 − 1.

**Keywords and maps as functions.** A keyword is a function of a map and a
map is a function of its keys, both with the semantics of `get`:
`(:a m)` is `(get m :a)`, `(:a m not-found)` is `(get m :a not-found)`,
`(m k)` is `(get m k)` and `(m k not-found)` is `(get m k not-found)`. The
result is `nil`, or `not-found`, when the key is absent; a keyword called on
`nil` or on a value that is not a map returns the same. A key mapped to `nil`
returns `nil`, not `not-found`. Calling either with any other number of
arguments is an error. Keywords, quoted symbols and maps are ordinary
function values: `(map :a [{:a 1} {:a 2}])` is `(1 2)`.

**Representation.** A map holds a protoCore `ProtoSparseList` of keys
indexed by a per-map sequence number (the insertion-order store) and a
second `ProtoSparseList` indexed by key hash whose buckets hold each key's
value and sequence number. Lookup, `assoc` and removal are `O(log n)`;
iteration looks each value up and is `O(n log n)`. The implementation is
`src/runtime/MapOps.{h,cpp}`.

```clojure
(assoc {:a 1} :b 2)            ;; => {:a 1, :b 2}
(dissoc {:a 1 :b 2} :a)        ;; => {:b 2}
(get {:a 1} :a)                ;; => 1
({:a 1 :b 2} :a)               ;; => 1 (maps are functions of keys)
(:a {:a 1 :b 2})               ;; => 1 (keywords are functions of maps)
(update {:n 1} :n inc)         ;; => {:n 2}
(merge {:a 1} {:b 2})          ;; => {:a 1, :b 2}
```

### 4.4 Set (planned)

Persistent hash set, backed by a `ProtoSparseList` keyed on the element.

```clojure
(conj #{:a :b} :c)             ;; => #{:a :b :c}
(disj #{:a :b :c} :a)          ;; => #{:b :c}
(contains? #{:a :b} :a)        ;; => true
(#{:a :b} :a)                  ;; => :a (sets are membership predicates)
```

### 4.5 Equality

`=` is structural value equality. Two vectors are equal iff they have
the same elements in order. Two maps are equal iff they have the same
key→value pairs. `==` is numeric equality across types (`(== 1 1.0)`
is true; `(= 1 1.0)` is false). Identity is `identical?` (pointer
equality, useful for sentinels).

Lists and vectors are *sequential* collections: two sequential
collections are `=` when they hold `=` elements in the same order,
whatever their concrete types, so `(= [1 2] '(1 2))` and `(= [] '())` are
true. A sequential collection is never `=` to a map, a set, a string or
`nil`.

Keywords, symbols and strings are three different kinds of value, never
`=` to one another even when spelled alike: `(= :a ":a")`,
`(= (quote a) "a")` and `(= (quote a) :a)` are false. Keywords and symbols
are interned, so two of them are `=` exactly when they have the same
spelling.

In 0.0.1, `=` and `not=` follow these rules for numbers, strings,
keywords, booleans, `nil`, lists, vectors and maps, recursively through
nested collections, with any number of arguments; every sequence the
runtime produces (`rest`, `map`, `filter`, `keys`, `cons`, ...) is a list
or a vector and compares the same way. Lists are written with `list`,
since the `'` reader macro is not implemented. The departures are:

- `=` compares numbers across types: `(= 1 1.0)` is true (deviation D15
  in `STATUS.md`), and map keys follow it: `1` and `1.0` are the same key
  (§4.3).
- NaN is `=` to every number (protoCore's numeric comparison), so NaN map
  keys cannot be hashed consistently with `=` (§4.3).
- `==`, `identical?` and `hash` are not implemented.

---

## 5. Functions

### 5.1 `defn` and `fn`

```clojure
(defn square [x] (* x x))

(fn [x y] (+ x y))            ;; anonymous
((fn [x] (* x 2)) 21)          ;; => 42
```

Functions are first-class values. They close over lexical bindings.
Variadic: `(defn f [x & more] ...)` collects all trailing args into
`more` as a sequence. Multi-arity:

```clojure
(defn greet
  ([] (greet "world"))
  ([who] (str "hello " who)))
```

### 5.2 Recursion and `recur`

A self-call in tail position can use `recur` for proper tail-call
optimisation:

```clojure
(defn factorial [n]
  (loop [n n acc 1]
    (if (<= n 1)
      acc
      (recur (dec n) (* acc n)))))
```

`recur` may only appear in tail position. The v0.1 design has the
compiler enforce this with a clear error; the 0.0.1 compiler does not
check it yet, and a non-tail `recur` fails at run time. Without `recur`,
deep self-recursion will overflow the operand stack — there is no
automatic TCO for general calls, only at `recur` points (matches JVM
Clojure exactly).

### 5.3 Closures

```clojure
(defn adder [n] (fn [x] (+ x n)))
(def add3 (adder 3))
(add3 10)                       ;; => 13
```

Captured locals are bound by value at closure creation; values can be
updated by referring to an `atom`.

---

## 6. Special forms — the irreducible set

`if`, `do`, `let*`, `loop*`, `recur`, `fn*`, `quote`, `def`, `var`,
`throw`, `try` / `catch` / `finally`. The macro `let` expands into
`let*`, `loop` into `loop*`, `fn` into `fn*`, etc. User code rarely
writes the `*` forms directly. (See §3.3 for what 0.0.1 implements.)

```clojure
(if test then else)
(do form1 form2 ...)
(let [x 1 y 2] body)
(loop [x 0] (if (< x 10) (recur (inc x)) x))
(fn [x] (+ x 1))
(quote (1 2 3))   ;; same as '(1 2 3)
(def x 42)
(throw (ex-info "msg" {:k v}))
(try body (catch ExceptionType e handler) (finally cleanup))
```

`ExceptionType` is one of the protoCore exception class objects:
`Error`, `ArithmeticError`, `IndexError`, etc. Catching `Exception`
matches all of them.

---

## 7. Namespaces (planned)

A file starts with a `ns` form declaring the namespace and its imports:

```clojure
(ns my.app
  (:require [clojure.string :as str]
            [clojure.set :refer [union intersection]]
            [py/numpy :as np]
            [js/d3 :as d3]))
```

`(:require [foo.bar :as fb])` makes `fb/x` resolve to `x` in `foo.bar`.
`(:require [foo.bar :refer [x y]])` makes `x` and `y` resolve directly.
`(:require [foo.bar :refer :all])` is supported but discouraged.

The module name resolves through the UMD registry. An unprefixed name
(`clojure.string`, `my.util`) resolves through the Clojure path: the
runtime walks `CLOJURE_PATH` plus the current source's directory looking
for a `.clj` file. A prefixed name (`py/numpy`, `js/d3`) resolves
through the namespaced UMD provider — see `INTEROP.md` for the full
chain.

`in-ns`, `create-ns`, `find-ns`, `the-ns`, `all-ns`, `ns-publics`,
`ns-refers`, `ns-aliases`, `ns-unmap` will be supported and behave as in
JVM Clojure. `ns-import` (JVM-specific) is not supported.

---

## 8. Vars and dynamic binding (planned)

`def` interns a *var* in the current namespace. The var is what is
captured when code references the symbol; redefining it propagates.

```clojure
(def ^:dynamic *width* 80)

(defn fmt [x] (str (subs (str x) 0 *width*)))

(binding [*width* 5]
  (fmt "hello world"))          ;; => "hello"

(fmt "hello world")              ;; => "hello world" (back to 80)
```

`^:dynamic` marks the var dynamically rebindable. `binding` pushes
thread-local bindings; without `^:dynamic` the var is constant.

`alter-var-root` will be supported with a warning — most uses are a
code smell.

In 0.0.1, `def` stores the value in a single global table; redefining
a name at the REPL replaces it for later calls.

---

## 9. State

### 9.1 Atoms

A reference to a value, swappable atomically. Backed by protoCore's
`setAttributeIfEqual` CAS. Implemented.

```clojure
(def counter (atom 0))
(swap! counter inc)       ;; => 1
@counter                  ;; => 1
(reset! counter 100)      ;; => 100
(compare-and-set! counter 100 0)  ;; => true
```

`swap!` may retry — the function must be pure. `add-watch` /
`remove-watch` are supported.

### 9.2 Refs and STM — **[v0.2]**

`ref`, `dosync`, `ref-set`, `alter`, `commute`, `ensure` are reserved
in the reader and the var namespace but raise a clear "not yet
implemented (v0.2)" at runtime in v0.1. See `DESIGN.md` §6 for the
implementation plan.

### 9.3 Agents — **[v0.3]**

`agent`, `send-off`, `await`, `await-for` are reserved. The
implementation will run on protoCore actors but with the JVM Clojure
surface API where it makes sense. protoClojure 0.0.1 already ships a
protoCore-native `actor` whose `send` returns a promise; see tutorial
chapter 13.

### 9.4 Volatiles (planned)

`volatile!`, `vreset!`, `vswap!` will be supported. They are *not* CAS
based — they're an unsynchronised mutable cell, used for inner-loop
state where the user has externally guaranteed single-thread access.
Useful for the implementation of transducers.

### 9.5 Promises and delays

`promise`, `deliver` and `realized?` are implemented. `delay` and
`force` are planned and will work as in JVM Clojure.

---

## 10. Sequences and lazy evaluation (planned)

In 0.0.1, `first`, `rest`, `cons`, `map`, `filter` and `reduce` work
over lists and vectors and return fully realised lists; `seq`, `next`,
laziness and the functions below are planned.

### 10.1 The seq abstraction

`seq` returns a sequence view of a collection. `first`, `rest`, `next`,
`cons` operate on seqs. Most collection-walking functions (`map`,
`filter`, `take`, etc.) consume any seqable and return a seq.

### 10.2 Laziness

`map`, `filter`, `take`, `drop`, `range`, `iterate`, `repeat`, `cycle`,
`partition`, `interleave`, `concat`, `mapcat` return **lazy
sequences**. Elements are produced on demand and memoised.

```clojure
(take 5 (iterate inc 0))         ;; => (0 1 2 3 4)

(def odd-squares
  (->> (range)
       (filter odd?)
       (map #(* % %))))

(take 5 odd-squares)             ;; => (1 9 25 49 81)
```

**v0.1: element-at-a-time.** No chunked-seq optimisation — every step
produces one element. v0.2 will add 32-element chunking transparently.

### 10.3 Forcing realisation

`doall` realises (and returns) a lazy seq fully. `dorun` realises but
discards. `doseq` is the side-effect-friendly iteration macro.

```clojure
(doseq [x [1 2 3]] (println x))
```

### 10.4 Transducers — **[v0.2]**

`(map f)`, `(filter pred)`, etc. with no collection arg currently
return a clear "transducers not yet supported (v0.2)" error in v0.1.
The arity that takes a collection works.

---

## 11. Interop with foreign UMD modules (planned)

Loaded modules from `py/X`, `js/X`, `pst/X` are protoCore objects.
Their *attributes* are their members. Function call uses standard
Clojure invocation.

```clojure
(:require [py/math :as m])

(m/sqrt 2.0)                   ;; => 1.4142135623730951
(m/pi)                         ;; => 3.141592653589793 (or, depending on the
                                ;;     export shape, just (m/pi) returns the
                                ;;     attribute)
```

Collection types do not auto-convert. A Python list and a Clojure
vector are different protoCore objects with different prototypes. To
hand a Clojure value to a foreign function that expects native
collection types, convert explicitly:

```clojure
(clj->py [1 2 3])              ;; protoPython list
(py->clj (np/array [1 2 3]))   ;; protoClojure vector
(clj->js {:a 1})               ;; protoJS object
(js->clj some-js-obj)          ;; protoClojure map
```

Primitive types (numbers, strings, booleans, nil) pass through with no
conversion. They're the same `ProtoObject` regardless of which
runtime created them.

The full chain and edge cases are documented in
[`INTEROP.md`](INTEROP.md).

---

## 12. Concurrency

Inherited entirely from protoCore: real OS threads, no GIL, per-thread
allocation arenas, concurrent garbage collector.

`future`, `promise`, `deliver`, `deref` and `realized?` are
implemented: `future` runs its body on a new OS thread, and `deref`
blocks on a pending future or promise. `pmap` runs one OS thread per
element. `pcalls` and `pvalues` are planned. Actors (`actor`, `send`,
`send-h`, `send-l`) run on a shared worker pool; see tutorial
chapter 13.

```clojure
(def result (future (slow-computation)))
@result                        ;; blocks until done
```

The Clojure-JVM thread-local Var binding semantics will be preserved
(planned, with `binding`): a `binding` form establishes a thread-local
rebinding that propagates to threads created inside the binding scope.

---

## 13. Departures from Clojure-JVM

Honest catalogue. Updated whenever a deviation is introduced or removed.

| ID  | Departure                                                                | Why                                                                                                                  |
| --- | ------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------- |
| D1  | No JVM interop (`Class.`, `(.method obj args)`, `proxy`, `reify` as JVM) | This runtime does not embed a JVM. The protoCore prototype model gives `reify` analogues via `defprotocol` + `extend-type`. |
| D2  | No Java classes (`java.lang.String`, `java.util.Date`, etc.)             | Same reason. Substitutes via Python / JS UMD modules or protoCore primitives.                                        |
| D3  | Characters are 1-codepoint strings, not a separate type                  | protoCore has no Character primitive; `(char 65)` returns `"A"`.                                                     |
| D4  | No chunked sequences in v0.1                                             | Simplicity. Performance follow-up in v0.2.                                                                           |
| D5  | No transducers in v0.1                                                   | v0.2 deliverable. The 2-arg seq form of `map`/`filter`/etc. works.                                                   |
| D6  | No STM in v0.1                                                           | v0.2. `ref` raises with a clear "v0.2" message.                                                                      |
| D7  | No agents in v0.1                                                        | v0.3. `agent` raises. The protoCore-native `actor` ships instead.                                                    |
| D8  | No `core.async` (likely permanent — re-imagined on actors)              | protoCore actors give CSP-like behaviour with a different surface; deliberate divergence.                            |
| D9  | No `defrecord` / `deftype` in v0.1                                       | Protocols + maps cover most needs.                                                                                   |
| D10 | No `BigDecimal` literal `M` suffix in v0.1                               | v0.2.                                                                                                                |
| D11 | No `clojure.spec`                                                        | Reserved; explicit v0.x design pass.                                                                                 |
| D12 | `clojure.java.*` namespaces do not exist                                 | JVM-specific.                                                                                                        |
| D13 | `read-string` rejects forms with reader literals not registered          | JVM Clojure is permissive here; we are strict to surface typos.                                                      |

Deviations introduced after these (D14 onward) are listed in
`STATUS.md`.

---

## 14. Errors (planned)

Errors are protoCore exception objects. The `clojure.core` exception
constructors (`ex-info`, `ex-data`, `ex-message`, `ex-cause`) work as
JVM Clojure. `try` / `catch` / `finally` operate on exception class
prototypes.

```clojure
(try
  (something-risky)
  (catch ArithmeticError e
    (println "math error:" (ex-message e)))
  (finally
    (cleanup)))
```

Class hierarchy: `Exception` ← `Error` ← `ArithmeticError`, `IndexError`,
`KeyError`, `TypeError`, `ArityError`. A catch on `Exception` matches any
of them.

In 0.0.1, a read, compile or runtime error stops a script with a
message on standard error and exit status 1; the REPL prints the error
and continues.

---

## 15. The REPL

`protoclj` with no arguments launches the local interactive REPL
(implemented). With `--nrepl PORT`, an nREPL server will listen on that
port for CIDER / Calva / Conjure connections (planned for v0.1).

nREPL operations planned for v0.1:
- `eval`, `interrupt`, `clone`, `close`, `describe`, `load-file`
- `info`, `complete` — **[v0.1 stretch]**

The v0.1 design prints REPL values using `pr-str`-style formatting
(quotes on strings, `:keyword` for keywords, etc.), not `print-str`. In
0.0.1 the REPL, `println` and `str` share one printer, so strings print
without quotes. `*1` `*2` `*3` hold the last three results (implemented); `*e`
will hold the last exception (planned).

---

## 16. Sample code reference (planned)

This example combines most v0.1 features and does not run on 0.0.1.

```clojure
;; A non-trivial example combining most v0.1 features.

(ns demo.word-count
  (:require [clojure.string :as str]
            [py/collections :as pyc]))

(defn word-freq
  "Count occurrences of each word in s, returning a sorted seq of
   [word count] pairs descending by count."
  [s]
  (->> s
       str/lower-case
       (re-seq #"\w+")
       frequencies
       (sort-by (comp - val))))

(defn top-n [s n]
  (take n (word-freq s)))

(defn -main [& args]
  (let [text (slurp (or (first args) "/dev/stdin"))]
    (doseq [[w c] (top-n text 10)]
      (println (format "%4d  %s" c w)))))
```
