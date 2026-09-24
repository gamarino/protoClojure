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
| Vectors `[...]` | Implemented (a `ProtoList` in a one-entry sparse-list box, distinct from list) |
| Maps `{...}` | Implemented (`hash-map`, `assoc`, `dissoc`, `get`, `contains?`, `keys`, `vals`; `count` and `empty?` accept maps) |
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
  In 0.0.1 an integer literal of any size reads exactly: a literal beyond
  the 64-bit range, such as `12345678901234567890`, is a LargeInteger, and
  the `N` suffix of Clojure's BigInt literals (`42N`) is accepted and does
  not change the value. `+`, `-`, `*`, `/`, `inc` and `dec` never wrap
  around, whether compiled to an opcode or called through `apply` or
  `reduce`: `(inc 9223372036854775807)` is `9223372036854775808` (JVM
  Clojure throws an `ArithmeticException`; deviation D14). `<`, `<=`, `>`
  and `>=` compare integers exactly at every magnitude. An integer divided
  by zero, at any magnitude, raises `ArithmeticException: Divide by zero`.
  Hexadecimal, binary and radix literals are planned.
- **Ratio**: `3/4`, `-22/7`. Stored as a normalised pair of integers.
  Arithmetic preserves exact rationality unless mixed with a float.
- **Float**: `3.14`, `1e10`, `-0.5e-3`. IEEE 754 double. A float prints
  as in JVM Clojure (Java's `Double.toString`): with the shortest digits
  that read back as the same double, so `(/ 1.0 3)` prints
  `0.3333333333333333` and `(+ 0.1 0.2)` prints `0.30000000000000004`; in
  plain notation with at least one fractional digit from 0.001 up to
  10,000,000 (`100.0`, `0.001`, `1234567.5`) and in scientific notation
  outside that range (`1.0E7`, `1.0E21`, `1.5E-7`); `-0.0` keeps its sign;
  infinities and NaN print as `##Inf`, `##-Inf` and `##NaN`, and `str` of a
  bare one gives `Infinity`, `-Infinity` or `NaN`. The reader accepts
  `##Inf`, `##-Inf` and `##NaN`, so every printed float reads back as the
  same value; as in JVM Clojure, whitespace may separate `##` from the
  symbol, any other symbol after `##` is the read error
  `Unknown symbolic value: <symbol>`, and anything but a symbol is
  `Invalid token: ##<text>`. When any operand of `/` is
  a float, the division follows IEEE 754: `(/ 1 0.0)` is `##Inf`,
  `(/ -1 0.0)` is `##-Inf` and `(/ 0 0.0)` is `##NaN`.
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

`str`, `println`, `join` and the REPL share one printer, which renders a
value in one of two modes, as Clojure's `print` and `pr` do. In the
*readable* mode a string, at any depth, is quoted and its `"`, `\`,
newline, tab, return, form-feed and backspace characters are escaped; in
the plain mode it is written as its characters. Everything else prints the
same in both modes.

`(str x ...)` concatenates its arguments: `nil` contributes nothing, a
string argument is inserted as is, and any other value is rendered
readably, so the strings nested in a collection keep their quotes.
`(str nil)` is `""`, `(str "a" nil "b")` is `"ab"`, `(str 1 :a "b")` is
`"1:ab"`, `(str ["a"])` is `"[\"a\"]"` and `(str {:a 1 :b [2]})` is
`"{:a 1, :b [2]}"`. `join` renders each element the same way:
`(join "," ["a" nil "b"])` is `"a,,b"`. `println` uses the plain mode at
every depth (`(println "a" ["b"])` prints `a [b]`) and prints `nil` as
`nil`; the REPL uses the readable mode.

A reference type renders as a tag: `(str (atom 1))` is `"#<atom 1>"`,
likewise `#<future ...>`, `#<promise ...>`, `#<actor ...>` and `#<fn>`;
a built-in function renders with its name, `(str println)` being
`"#<fn println>"` (JVM Clojure renders `#object[clojure.lang.Atom 0x... {:status :ready, :val 1}]`;
deviation D20 in `STATUS.md`). The value inside the tag is printed in the
same mode as the tag.

### 2.4 Characters

`\a`, `\space`, `\tab`, `\newline`, `é`. A character is a one-codepoint
`ProtoString`; we do *not* introduce a separate character type in v0.1
(see *Departures from Clojure-JVM*, §13).

In 0.0.1 character literals are not read yet. `nth` on a string returns the
character at an index as a one-character string, indexing by code point as
`count` and `subs` do: `(nth "año" 1)` is `"ñ"` (JVM Clojure returns the
character `\ñ`). An index past the end raises
`StringIndexOutOfBoundsException: nth index 3 is out of bounds (count 3)`, or
returns the not-found value when one is given.

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
- **Map**: `{:a 1 :b 2}` — pairs (key value); the order of iteration and
  printing is unspecified and can change between runs, and equality ignores
  order (§4.3).
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
lexical scope and then a single global table. A global defined with the
value `nil` or `false` resolves to that value (`(def x nil)` then `x` is
`nil`); only a name that was never defined raises
`unable to resolve symbol`.

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
listed in `STATUS.md`. Sets, `conj`, `update`, `merge`,
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

Indexed sequential collection. `O(log n)` random access with `nth`, `O(1)`
`count`, and `O(1)` coercion to a sequence, so `map`, `filter`, `reduce`,
`first` and `rest` on a vector cost what they cost on a list.

```clojure
(nth [1 2 3] 0)        ;; => 1
(count [1 2 3])        ;; => 3
(vec (list 1 2))       ;; => [1 2]
(vector? [1 2])        ;; => true, and (list? [1 2]) is false
(= [1 2] (list 1 2))   ;; => true (sequential equality, §4.5)
```

`conj`, `assoc` on a vector, `subvec` and calling a vector as a function of
its index are **not implemented yet** (`STATUS.md`).

**Representation.** A vector is a protoCore `ProtoList` of its elements inside
a one-entry `ProtoSparseList` box, which is what gives it a pointer tag of its
own and so tells `vector?` from `list?` in O(1). The box costs one cell per
vector. Building a vector from a call's arguments, and `vec` of a list, are
O(1): the elements already are the `ProtoList` that the box stores.

Up to protoClojure 0.0.1 a vector was a protoCore `ProtoTuple`. protoCore
interns every tuple node and frees none, so **every vector a program ever
built stayed in memory until it exited**. Vectors are now ordinary garbage.
The cost of the change is indexed access: a `ProtoList` node holds one
element where a `ProtoTuple` node holds four, so `nth` takes about twice the
node hops, and a large vector about three times the cells — while sequential
access, construction and `vec` all got cheaper, and a large vector now costs
less memory overall than the interned tuple plus its interner entry did. Two
equal vectors are no longer one pointer, so `=` walks their elements like
every other sequential comparison. (Decision R2, `CHANGELOG.md`.)

### 4.3 Maps

**Representation, in one sentence.** A map is an immutable protoCore
`ProtoMap`, driven through protoCore's shared hashed-collection helper: a key
matched by identity is stored under itself, every other key under its hash,
and both hold the original key object.

```clojure
(assoc {:a 1} :b 2)            ;; => {:a 1, :b 2}  (or {:b 2, :a 1}: order is unspecified)
(dissoc {:a 1 :b 2} :a)        ;; => {:b 2}
(get {:a 1} :a)                ;; => 1
(get {:a 1} :z :none)          ;; => :none
(contains? {:a nil} :a)        ;; => true
(count {:a 1 :b 2})            ;; => 2
({:a 1 :b 2} :a)               ;; => 1 (maps are functions of keys)
(:a {:a 1 :b 2})               ;; => 1 (keywords are functions of maps)
```

**Operations.**
- `count` is O(n) (deviation D25); `empty?` is O(1).
- `get`, `contains?`, calling a map or a keyword, `assoc` and `dissoc` are
  O(log n), plus the cost of hashing the key (below).
- Every operation returns a new map and leaves its argument unchanged.
- `assoc` of a key that is already present replaces the value and keeps the
  key object first stored: `(assoc {[1 2] :a} (list 1 2) :b)` is `{[1 2] :b}`.
  When the new value is the identical object, `assoc` returns the same map.
- `dissoc` of an absent key returns the same map.
- `update`, `merge`, `select-keys` and the `get-in` family are planned
  (`STATUS.md`).

**Order is unspecified and can change between runs.**
- `keys`, `vals`, printing and every other walk over a map visit the entries
  in an unspecified order. That order is the ascending order of the words the
  entries are stored under — a memory address for a key matched by identity,
  a hash for every other key — so the same program can print the same map
  differently on two runs.
- Within one run, `keys` and `vals` of one map value walk its entries in the
  same order, so `(= (map m (keys m)) (vals m))` holds.
- Never rely on the order of a map; sort the keys when output must be stable.
- Watches (`add-watch`) live in a map and fire in the same unspecified
  order, as on the JVM.

**Key equality.** Two keys name the same entry exactly when they match under
the rules below:

| Key type | Matched by | Same key as | A different key from |
|---|---|---|---|
| Integer in the SmallInteger range (−2^53 to 2^53 − 1), `true`, `false`, `nil` | the value itself | the same value | `1.0` for `1`; an equal double |
| Big integer (beyond the SmallInteger range) | its exact value | the same value however computed: `(* 10000000000 10000000000)` and `100000000000000000000` | an equal double: `1.0e20` |
| Double | its exact 64-bit pattern | a double with the same bit pattern: `1.5` and `(/ 3.0 2)`; `##NaN` and `##NaN` | `-0.0` for `0.0` (as in JVM Clojure); `1` for `1.0`; a NaN with another bit pattern |
| String | its characters | a string with the same characters, however built: `"user-1"` and `(str "user-" 1)` | a keyword or a symbol with the same spelling: `":a"` and `:a` |
| Keyword, symbol | identity (they are interned) | the same spelling | the string of its spelling |
| Vector | its elements, element by element | a vector or a list with equal elements: `[1 2]` and `(list 1 2)` | a vector whose elements differ in numeric type: `[1]` and `[1.0]` |
| List | as for a vector | as for a vector | as for a vector |
| Map | its entries, each value matched as a key would be | a map with the same entries, built in any order | a map whose keys or values differ in numeric type: `{:a 1}` and `{:a 1.0}` |
| Atom, function, future, promise, actor | identity | only itself | every other object |

These rules hold at every depth of a collection key. A big integer never
holds a value in the SmallInteger range, so every integer is matched one way
only. Each key type carries its own hash salt, so keys of different types are
never confused whatever they are built from:
`(get {1.0 :d} [:double 1072693248 0])` is `nil`.

**Map equality.** Two maps are `=` when they have the same number of
entries and every key of one names an entry of the other, with `=` values. Values are compared with `=` (§4.5), so
`(= {:a 1} {:a 1.0})` is true, while `(= {1 :a} {1.0 :a})` is false because
`1` and `1.0` are different keys. A map is never `=` to a vector or a list.

**Differences from JVM Clojure.**
- **Numeric keys.** `(= 1 1.0)` is true in protoClojure (deviation D15), but
  map keys do not follow `=` across numeric types. `1`, `1.0` and an equal
  big integer are three different keys, and so are `0` and `-0.0`, as in
  JVM Clojure: `(hash-map 1 :a 1.0 :b)` has two entries, and
  `(get {0.0 :z} -0.0)` is `nil`.
- **NaN keys.** A NaN key is found by a NaN with exactly the same bit
  pattern, so `(get {##NaN :n} ##NaN)` is `:n`; JVM Clojure never finds a
  NaN key. A NaN produced by arithmetic can have a different bit pattern and
  then does not find a `##NaN` key: on x86-64, `(/ 0 0.0)` has its sign bit
  set, while `##NaN` does not. Both print as `##NaN`.
- **Order.** Unspecified for every map, and it can differ between runs.
  (JVM Clojure keeps insertion order for array maps of up to 8 entries.)

**Keys are ordinary garbage.** A key is stored as the object the caller
passed and dies with the last map that holds it, exactly as in JVM Clojure.
Nothing on the key path is interned. (Up to protoClojure 0.0.1 every key was
reduced to an interned canonical key that protoCore never freed, so any
distinct string, double, big integer or collection ever used as a key —
keys only looked up included — stayed in memory until the program exited.
That was deviation D23; it is withdrawn.)

**Collection keys cost time.** The key is hashed on every operation that
takes it, and compared element by element inside its slot:
- O(1) for integers in the SmallInteger range, booleans, `nil`, keywords and
  symbols;
- proportional to the length for strings, doubles and big integers;
- proportional to the total size, at every depth, for vectors, lists and
  maps.

A large collection used as a key is walked again on each lookup.

**Limit.** A map holds at most 16,777,215 (2^24 − 1) slots, the size limit of
protoCore's `ProtoMap`.

**Keywords and maps as functions.** A keyword is a function of a map and a
map is a function of its keys, both with the semantics of `get`:
`(:a m)` is `(get m :a)`, `(:a m not-found)` is `(get m :a not-found)`,
`(m k)` is `(get m k)` and `(m k not-found)` is `(get m k not-found)`. The
result is `nil`, or `not-found`, when the key is absent; a keyword called on
`nil` or on a value that is not a map returns the same. A key mapped to `nil`
returns `nil`, not `not-found`. Calling either with any other number of
arguments is an error. Keywords, quoted symbols and maps are ordinary
function values: `(map :a [{:a 1} {:a 2}])` is `(1 2)`.

**Representation.** A map is a protoCore `ProtoMap` with no wrapper object,
read and written through protoCore's shared hashed-collection helper
(`hashedPut` / `hashedGet` / `hashedRemove` / `hashedForEach`) with
protoClojure's key semantics. A key matched by identity — a keyword, a
symbol, `nil`, a boolean, an atom, a function, an actor — is its own slot
key; every other key goes under a slot keyed by the low 54 bits of its hash,
whose value is a flat list `[k v]`, or `[k0 v0 k1 v1 …]` on the hash
collision that gives `count` its O(n). Printing and `keys` show the original
key object, so a list key prints as `(1 2)` and a string key as a string. A
map is immutable: the garbage collector scans it like any other value, and it
never enters protoCore's mutables tree. The implementation is
`src/runtime/MapOps.{h,cpp}`.

### 4.4 Set (planned)

Persistent hash set, backed by a `ProtoMap` keyed on the element (the same
hashed-collection helper the map uses).

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
  in `STATUS.md`). Map keys do not follow it: `1` and `1.0` are different
  keys, as in JVM Clojure, so `(= {1 :a} {1.0 :a})` is false (§4.3).
- Comparisons follow IEEE 754 for NaN, as in JVM Clojure: `<`, `<=`, `>`,
  `>=` and `=` with a NaN are false and `not=` is true, so
  `(= ##NaN ##NaN)` is false; one NaN object is `=` to itself, because `=`
  tests identity first (`(let [x ##NaN] (= x x))` is true). `-0.0` is `=` to
  `0.0` and to `0`.
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
deep self-recursion exhausts the stack — there is no automatic TCO for
general calls, only at `recur` points (matches JVM Clojure exactly). In
0.0.1 every non-tail call nests the interpreter on the native stack. A
recursion deeper than the thread's stack allows raises the runtime error
`StackOverflowError`, as JVM Clojure does; every protoClojure thread runs
on a 32 MiB stack, where a simple self-recursive function reaches about
24,700 nested calls (§17).

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
chapter 13. The evaluator, future and `pmap` threads and actor workers
all run on 32 MiB stacks, so a recursion reaches the same depth on each
of them (§17).

```clojure
(def result (future (slow-computation)))
@result                        ;; blocks until done
```

An error raised by a future's body is kept with the future, and every
`deref` of that future raises it wrapped as the analogue of JVM Clojure's
`java.util.concurrent.ExecutionException`: `@(future (/ 1 0))` raises
`ExecutionException: ArithmeticException: Divide by zero`. A future whose
error is never dereferenced fails silently, as in JVM Clojure. `pmap` raises
the error of the first element, in input order, whose call failed, wrapped
the same way, after waiting for every element. An error in an actor's
message handler is not reported yet: the actor's state becomes `nil`
(`STATUS.md`, Known issues).

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
and continues. A recursion, or a collection printed, compared or hashed,
nested deeper than the thread's stack allows raises the runtime error
`StackOverflowError` (§17). Source forms nested deeper than the reader or the
compiler can follow are a read or compile error with the message
`StackOverflowError: forms nested too deeply for the 32 MiB thread stack`
(§17). An arithmetic or ordering operation
(`+ - * / < <= > >= inc dec`) on a value that is not a number raises the
runtime error `ClassCastException: <operation> expects a number, got
<type>`, for example `(+ 1 nil)`: `ClassCastException: + expects a number,
got nil`. An index out of range raises
`IndexOutOfBoundsException: nth index 3 is out of bounds (count 3)` or
`StringIndexOutOfBoundsException: subs begin 2, end 1, length 3`, with the
index as written at any magnitude (`StringIndexOutOfBoundsException` for
`nth` on a string), `nth` on a value that is neither sequential, a string nor
`nil` raises `UnsupportedOperationException: nth not supported on a map`
(naming the type), and an integer divided by zero raises
`ArithmeticException: Divide by zero`. An error raised on a future or `pmap`
thread reaches the thread that dereferences the future or calls `pmap`,
prefixed with `ExecutionException: ` (§12).

---

## 15. The REPL

`protoclj` with no arguments launches the local interactive REPL
(implemented). With `--nrepl PORT`, an nREPL server will listen on that
port for CIDER / Calva / Conjure connections (planned for v0.1).

nREPL operations planned for v0.1:
- `eval`, `interrupt`, `clone`, `close`, `describe`, `load-file`
- `info`, `complete` — **[v0.1 stretch]**

The REPL prints values in the readable, `pr-str`-style form (quotes and
escapes on strings at every depth, `:keyword` for keywords), not
`print-str`: `"a"` echoes as `"a"` and `["a"]` as `["a"]` (§2.3). `*1` `*2`
`*3` hold the last three results (implemented); `*e` will hold the last
exception (planned).

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

---

## 17. Implementation limits

The hard limits of protoClojure 0.0.1. Each instruction of the bytecode
VM is a 32-bit word whose operand has 24 bits (`src/runtime/Opcodes.h`), so
the limits that come from the bytecode are 16,777,215; exceeding one is a
compile error naming the instruction. A function body has its own
constant pool and local slots; the top level of a script, or of one REPL
input, is one more body. Equal constants of the same kind share one pool
entry (`1` and `1.0`, the string `"a"`, the keyword `:a` and the symbol `a`
are four different kinds of constant).

| Limit | Value | When exceeded |
|---|---|---|
| Distinct constants in one body | 16,777,216 | compile error |
| Local slots in one function body (parameters, `let` and `loop` bindings, captured variables) | 16,777,216 | compile error |
| `fn` / `defn` bodies and multi-arity groups written directly in one body | 16,777,216 each | compile error |
| Arguments written in one call, vector literal or map literal (keys and values) | 16,777,215 | compile error |
| Instructions jumped over by `if`, `when`, `cond`, `and`, `or`, an `:or` default, or `recur` back to its loop | 16,777,215 | compile error |
| Parameters one call binds (a variadic fn binds its rest arguments as one) | 17 | runtime error |
| Names in one `& {:keys [...]}` | 16 | runtime error |
| Extra arguments to `swap!` | 15 | runtime error |
| Operand stack of a call (arguments pushed by a call, a literal or `apply`) | grows on demand | memory |
| Integer literals, strings, collections | none | memory |
| Nesting depth of non-tail calls | the 32 MiB native stack of every thread: about 24,700 calls of a simple self-recursive fn; a recursion through a primitive such as `map` uses more stack per level | runtime error `StackOverflowError` |
| Nesting depth of a collection that is printed, compared or hashed | the 32 MiB native stack of every thread | runtime error `StackOverflowError` |
| Nesting depth of source forms (lists, vectors, maps, `fn` bodies) | the 32 MiB native stack of the thread that reads and compiles them: 80,000 nested lists read, compile and run and 100,000 do not; 20,000 nested `fn` forms compile and 40,000 do not | read or compile error `StackOverflowError` |
