# protoClojure — Status

> Living tracker. This file changes every time something is implemented,
> finished, or dropped. If a feature you expect is not listed as
> implemented here, it is not implemented.

**Current state.** Version 0.0.1, no tagged release. The interpreter runs
scripts and an interactive REPL. `ctest` registers 383 test cases: 291
conformance fixtures under `tests/conformance/`, 86 GoogleTest unit
tests for the lexer, the reader, the bytecode module, the runtime map,
value equality and hashing, the native stack guard and the double printer
(`tests/unit/`), and
six CLI checks (`tests/cli/`: `--help`, a generated program with 70,000
distinct literals of each kind, the native bulk builders under a heap
ceiling, a stack overflow in the REPL, globals bound to nil in the REPL,
and source nested too deeply to read or compile);
all pass. Benchmark numbers against Babashka 1.4.192
are in [`benchmarks/RESULTS.md`](../benchmarks/RESULTS.md). Shipped changes
are listed in [`CHANGELOG.md`](../CHANGELOG.md).

---

## Feature coverage by conformance directory

Features in the order they were implemented, with the conformance
directories that cover them.

| Feature | Conformance directories | Fixtures |
|---|---|---:|
| Binary, lexer, reader, bytecode VM, `println` | `00-binary`, `01-literals` | 2 |
| `def`, `if`, `do`, integer arithmetic, comparisons, `str` | `02-special-forms`, `03-arithmetic` | 35 |
| `fn`, `defn`, `let`, `loop`, `recur`, `StackOverflowError` | `04-functions`, `05-recursion` | 14 |
| Closures with N-level lexical capture | `06-closures` | 6 |
| Variadic `& rest`, `apply`, list operations, `map` / `filter` / `reduce` | `07-variadic`, `08-collections`, `09-higher-order` | 35 |
| Multi-arity `defn`, `cond` / `when` / `and` / `or`, booleans, keywords | `10-multi-arity`, `11-sugar-forms`, `12-literals` | 21 |
| IEEE-754 floats, vectors distinct from lists | `13-floats`, `14-vectors` | 26 |
| LargeInteger promotion, big integer literals | `15-bigint` | 9 |
| Maps, `& {:keys [...]}` named-argument destructuring | `16-maps`, `17-kw-destructuring` | 55 |
| Trailing keyword/value pairs, `:or`, `:as` | `18-kw-callsite`, `19-or-and-as` | 16 |
| `clojure.string`-shaped string functions | `20-strings` | 20 |
| Atoms | `21-atoms` | 13 |
| Futures and `pmap` on OS threads | `22-futures` | 18 |
| Watches, promises | `23-watches`, `24-promises` | 12 |
| Actors | `25-actors` | 9 |
| Interactive REPL | — (no conformance fixtures) | — |

The design specifications written during development are archived under
[`docs/archive/design-specs/`](archive/design-specs/).

---

## Implemented and stable

### Reader

- [x] Integers (SmallInteger + auto-promoted LargeInteger); a literal
      beyond the 64-bit range reads as an exact LargeInteger
      (`12345678901234567890`), and Clojure's `N` suffix is accepted on
      integer literals (`42N`, same value; `1.5N` is a read error)
- [x] Floats — `3.14`, `1e6`, `1.5e-3`
- [x] Symbolic values — `##Inf`, `##-Inf`, `##NaN` read as the infinities
      and NaN the printer spells that way; another symbol after `##` is
      `Unknown symbolic value: <symbol>`
- [x] Strings — `"hello"`, with `\n \t \r \\ \" \0` escapes
- [x] Symbols (interned via the protoCore symbol table)
- [x] Keywords — `:foo`, self-evaluating constants
- [x] Non-ASCII letters in symbols and keywords — UTF-8 encoded Unicode
      letters, combining marks and decimal digits (`año`, `:ñandú`,
      `:日本語`; table generated from Unicode 16.0 in
      `src/reader/UnicodeLetters.h`); other non-ASCII characters and
      malformed UTF-8 are read errors naming the character (D21); lexer
      columns count code points
- [x] Booleans + nil — `true`, `false`, `nil` as compile-time literals
- [x] Lists `(...)`
- [x] Vectors `[...]`
- [x] Maps `{...}`
- [x] `@form` — read as `(deref form)`
- [x] Line comments `;`; commas as whitespace
- [x] Nesting depth — the reader and the compiler recurse once per level of
      nesting and check the native stack at each level: forms nested deeper
      than the 32 MiB stack allows are a read error, reported where reading
      stopped, or a compile error, `StackOverflowError: forms nested too
      deeply for the 32 MiB thread stack`, in a script and in the REPL.
      80,000 nested lists read, compile and run and 100,000 do not; 20,000
      nested `fn` forms compile and 40,000 do not

### Special forms

- [x] `def` — a global defined as `nil` or `false` resolves to that value;
      only a name never defined is `unable to resolve symbol`
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
- [x] Maps — an immutable protoCore `ProtoSparseList` indexed by the
      interned canonical key of each key, holding the original key and the
      value (`src/runtime/MapOps.h`), with `hash-map` / `assoc` / `dissoc` /
      `get` / `contains?` / `keys` / `vals` / `map?`; `count` (O(1)) and
      `empty?` accept maps. Iteration and print order are unspecified and can
      change between runs. `assoc` of an equal key keeps the stored key
      object and replaces the value
- [x] Map equality — `=` / `not=` compare maps by value, whatever the order
      (`mapEquals` in `src/runtime/MapOps.h`): the same canonical keys with
      `=` values; values compare recursively, so nested collections compare
      by value; a map is never `=` to a vector or a list
- [x] Sequential equality — `=` / `not=` compare lists and vectors element
      by element, whatever their concrete types: `(= [1 2] (list 1 2))` and
      `(= [] (list))` are true, nested collections compare by value, and a
      list or vector is never `=` to a map, a string or `nil`
      (`valuesEqual` in `src/runtime/Primitives.h`)
- [x] Canonical map keys — every key is reduced to an interned canonical
      key (`canonicalKey` in `src/runtime/MapOps.h`; table in LANGUAGE.md
      §4.3): strings by content, a list and a vector with equal elements as
      one key, maps by their entries in any order, doubles by bit pattern,
      big integers by exact value, keywords, symbols and other objects by
      identity; so `(get {{:a 1} :x} {:a 1})`, `(get {[1 2] :x} (list 1 2))`
      and `(get {"ab-cd-ef" 1} (str "ab-" "cd-ef"))` find their entries.
      Numbers of different types are different keys (D15), `##NaN` finds
      itself (D22), and canonical keys are never freed (D23)
- [x] Strings — protoCore `ProtoString`
- [x] Keywords and symbols — interned runtime values distinct from strings
      (`src/runtime/Named.h`): `(= :a ":a")` and `(= (quote a) "a")` are
      false, `(string? :a)` is false, and `:a` and `":a"` are different map
      keys; keywords and symbols compare and hash by identity, and interning
      makes identity equivalent to equality of spelling, also across threads
- [x] Keywords and maps as functions — `(:a m)` and `(:a m not-found)` look
      `:a` up as `get` does, returning `nil` (or `not-found`) when the key is
      absent or `m` is not a map; `(m k)` and `(m k not-found)` likewise.
      Keywords, quoted symbols and maps are function values wherever a
      function is accepted: locals, globals, `apply`, `map`, `filter`,
      `reduce`, `pmap` (`callLookup` in `src/runtime/ExecutionEngine.cpp`)
- [x] Integers — `SmallInteger` tagged + auto-promoted `LargeInteger`
- [x] Floats — `ProtoObject::fromDouble`

### Numeric semantics

- [x] SmallInteger fast-path arithmetic via opcodes (`ADD SUB MUL LT LE GT GE EQ`)
- [x] Automatic promotion to LargeInteger on overflow, in the opcodes and in
      the `+ - * / inc dec` primitives alike (`(apply + ...)`, `reduce`,
      `(inc 9223372036854775807)`): no integer operation wraps around (D14)
- [x] Automatic promotion to double when any operand is a float
- [x] Arithmetic and ordering take numbers only: `+ - * / < <= > >= inc dec`,
      compiled to an opcode or called as a primitive, raise the
      ClassCastException analogue naming the operation and the offending
      type, `ClassCastException: * expects a number, got a string`
      (`throwNotANumber` in `src/runtime/Primitives.h`); the one-argument
      comparison `(< x)` is `true` for any value, as in Clojure
- [x] Integer division truncates: `(/ 10 4)` is `2`, exact at any magnitude;
      an integer divided by zero, big integers included, raises
      `ArithmeticException: Divide by zero`
- [x] Division with a float operand follows IEEE 754, as in JVM Clojure:
      `(/ 1 0.0)` is `##Inf`, `(/ -1 0.0)` is `##-Inf`, `(/ 0 0.0)` is `##NaN`
- [x] `< <= > >=` compare integers exactly beyond 2^53; a comparison with a
      float compares doubles
- [x] Comparisons follow IEEE 754 for NaN (protoCore `partialCompare`):
      `< <= > >=` and `=` with a NaN are false and `not=` is true, so
      `(= ##NaN ##NaN)` is false, while one NaN object is `=` to itself
      (identity first, as in JVM Clojure); `-0.0` is `=` to `0.0` and `0`
- [x] Print path handles SmallInteger, LargeInteger and floats; a float
      prints as JVM Clojure prints it (`formatDouble` in
      `src/runtime/Primitives.h`): the shortest digits that read back as the
      same double (`0.3333333333333333`, `0.30000000000000004`), plain
      notation from 0.001 up to 10,000,000 and `1.0E21` / `1.5E-7` outside,
      `-0.0` with its sign, `##Inf`, `##-Inf` and `##NaN`, in `println`,
      `str`, `join` and the REPL alike; `str` of a bare infinity or NaN is
      `Infinity`, `-Infinity` or `NaN`, as in JVM Clojure

### Closures

- [x] N-level lexical capture across nested `fn` bodies
- [x] Captures cascade — intermediate scopes create capture slots automatically
- [x] First-class fns — `((make-mul k) x)` callable head, passed around freely

### Primitives installed at startup (75)

- [x] Arithmetic and comparison: `+ - * / inc dec < <= > >= = not=`
- [x] Output: `println str` — one printer (`printTo` in
      `src/runtime/Primitives.cpp`) renders values for `println`, `str`,
      `join` and the REPL, with a readable flag: `println` prints strings
      bare at every depth (`(println ["a"])` prints `[a]`); the REPL prints
      readably (`"a"`, `["a"]`); `str` and `join` insert nil as `""` and a
      string argument as is, and render any other value readably, so
      `(str "a" nil ["b"])` is `"a[\"b\"]"`, `(str {:a 1})` is `"{:a 1}"`
      and `(str (atom 1))` is `"#<atom 1>"` (D20); a user fn prints as
      `#<fn>` and a built-in function as `#<fn NAME>` (`(str println)` is
      `"#<fn println>"`)
- [x] Lists and vectors: `list vector vec nth first rest cons count empty? reverse`
- [x] Higher-order: `map filter reduce pmap`
- [x] Predicates: `nil? not vector? list? map? string?`
- [x] Maps: `hash-map assoc dissoc get contains? keys vals`; `count` and
      `empty?` also accept maps
- [x] Strings: `subs upper-case lower-case starts-with? ends-with?
      includes? index-of replace join split trim triml trimr blank?`;
      `count` / `empty?` / `reverse` also accept strings, and `nth` returns
      the character at a code-point index as a one-character string
      (`(nth "año" 1)` is `"ñ"`, D3)
- [x] `nth` on `nil` returns `nil`, or the not-found value, for any integer
      index; on a value that is not a list, a vector, a string or `nil` it
      raises `UnsupportedOperationException: nth not supported on a map`
- [x] Index and position arguments (`nth`, `subs`, `index-of`) accept
      integers of any size: an index out of range raises
      `IndexOutOfBoundsException: nth index 12345678901234567890 is out of
      bounds (count 3)` or `StringIndexOutOfBoundsException: subs begin 2,
      end 1, length 3`, naming the index as written; `nth` with a not-found
      value returns it instead; `index-of` treats its start position as
      Java's `String.indexOf` does (a negative start searches from 0); a
      non-integer index raises `ClassCastException: nth expects an integer,
      got a float`
- [x] Atoms and watches: `atom atom? deref reset! swap! compare-and-set!
      add-watch remove-watch`
- [x] Futures and promises: `make-future future? realized? promise
      promise? deliver`
- [x] Actors: `actor actor? send send-h send-m send-l actor-stats`

### Bytecode VM

- [x] 29 opcodes — see `src/runtime/Opcodes.h`
- [x] 32-bit instruction words with a 24-bit operand: constant-pool
      indices, local slots, function bodies, argument counts and jump
      offsets range up to 16,777,215 per function body or script top level
      (a larger operand is a compile error); a script with 70,000 distinct
      literals of each kind compiles and runs
- [x] Constant pool de-duplicated by kind and value (`1` and `1.0`, `"a"`,
      `:a` and a symbol `a` stay distinct; doubles by bit pattern)
- [x] Operand stack grows on demand, so calls, vector and map literals and
      `apply` take any number of arguments; the remaining hard limits are
      listed in `LANGUAGE.md` §17
- [x] `MAKE_FN` / `MAKE_FN_MULTI` — single + multi-arity wrappers
- [x] `CALL_APPLY` — spread-arguments dispatch
- [x] `CALL_KW` — trailing keyword/value pairs; ordinary callees receive the
      arguments unchanged, keyword-based callees get the pairs after their
      positionals folded into a kwArgs map
- [x] Split fn prototypes (`fnSingleProto` / `fnMultiProto`) for one attribute lookup per single-arity call
- [x] SmallInteger fast-path binary opcodes
- [x] DUP + JUMP_IF_TRUE for short-circuit `and` / `or`
- [x] Native stack check on every call (`src/runtime/StackGuard.h`): a
      recursion deeper than the thread's stack raises the runtime error
      `StackOverflowError` instead of crashing, on the evaluator, future,
      `pmap` and actor threads alike, and so does printing, comparing or
      hashing a collection nested that deeply. The script driver and the
      REPL run on a 32 MiB stack and every thread the runtime creates gets
      the same size; one call takes 1,344 bytes of native stack, so a simple
      self-recursive fn reaches about 24,700 nested calls

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
- [x] Errors on future and `pmap` threads propagate: every `deref` of a
      future whose body raised an error raises it as
      `ExecutionException: <error>` (`@(future (/ 1 0))` raises
      `ExecutionException: ArithmeticException: Divide by zero`), and `pmap`
      raises the error of the first failing element in input order the same
      way, after waiting for every element
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
- [ ] `hash` as a function
- [ ] Vectors as functions (`(v 0)`)

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
      emits the `CALL_KW` opcode with every argument as a positional. An
      ordinary callee (a primitive such as `assoc` or `get`, or a fn
      without `& {:keys ...}`) receives the arguments unchanged, in source
      order and with repeated keywords kept. A kw-based callee takes its
      declared positionals from the front and gets the remaining pairs
      folded into a kwArgs map in order; a repeated key keeps its last
      value.
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
- [ ] Map operations: `update merge select-keys get-in update-in assoc-in`
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
| D14 | LargeInteger promotion is automatic in `+ - * / inc dec`, so `(* 9223372036854775807 2)` is `18446744073709551614` where JVM Clojure throws `ArithmeticException: integer overflow` — no `+'` / `*'` needed; big integers are not a separate type, so the `N` suffix is accepted but does not change the value or how it prints (CONTRA Clojure-JVM, by design) | (perm) |
| D15 | `(= 1 1.0)` returns `true` in v0.x (CONTRA Clojure-JVM where `=` is type-strict). Map keys do **not** follow `=` across numeric types: `1`, `1.0`, an equal big integer, and `0` / `-0.0` are different keys, as in JVM Clojure, so `(hash-map 1 :a 1.0 :b)` has two entries and `(= {1 :a} {1.0 :a})` is `false` | (perm) |
| D16 | `:or` defaults fire on **explicit nil** as well as missing keys (CONTRA JVM-Clojure where only missing keys take the default) | v0.2 |
| D17 | String ops (`upper-case`, `lower-case`, `split`, `reverse`, `trim`, `index-of`) are byte-level / ASCII-correct only; multi-byte UTF-8 codepoints traverse as bytes (CONTRA JVM-Clojure which is codepoint-aware) | v0.2 |
| D18 | `(fn name [args] body)` — the name is accepted by the compiler but dropped; self-reference via `name` inside the body is not supported (use `defn` for self-recursion). Planned for v0.2 via wrapper-into-slot capture. | v0.2 |
| D20 | Atoms, futures, promises, actors and fns print as tags such as `#<atom 1>`, `#<fn>` and `#<fn println>` in `println`, `str` and the REPL (JVM Clojure: `#object[clojure.lang.Atom 0x... {:status :ready, :val 1}]` when printed, and `clojure.lang.Atom@...` or the class name under `str`) | v0.x |
| D21 | Beyond ASCII, symbols and keywords accept only Unicode letters, combining marks and decimal digits: `a→b`, or a symbol containing a no-break space, is a read error (CONTRA JVM-Clojure, whose reader accepts any character that is neither whitespace nor a macro character) | v0.x |
| D22 | A NaN map key is found by a NaN with exactly the same 64-bit pattern: `(get {##NaN :n} ##NaN)` is `:n` (CONTRA JVM-Clojure, which never finds a NaN key). A NaN produced by arithmetic can have a different bit pattern and then does not find a `##NaN` key: on x86-64, `(/ 0 0.0)` has its sign bit set | (perm) |
| D23 | Map keys are interned and never freed: every distinct string, double, big integer or collection used as a map key, including keys only looked up and keys of removed entries, stays in memory until the program ends (keywords, symbols, SmallIntegers, booleans, `nil` and ASCII strings of up to 6 bytes cost nothing). Long-running programs should not use unbounded run-time-generated values as keys (CONTRA JVM-Clojure, where keys are ordinary garbage-collected objects) | (perm) |
| D24 | Map iteration and print order is unspecified for maps of every size and can change between runs of the same program (JVM Clojure keeps insertion order for array maps of up to 8 entries) | (perm) |

## Known issues

- **String map keys leak memory in this version.** An operation that takes
  a string key of more than 6 bytes, or a non-ASCII one, interns it with
  protoCore's `createSymbol`. When the symbol already exists, `createSymbol`
  still builds two permanent copies of the string and drops them: about
  500 bytes per call for a 25-byte string (200,000 lookups of one existing
  key grow memory by 100 MB). String literals are re-created on every
  execution, so `(get m "username")` in a loop leaks too. The fix belongs in
  protoCore (look the symbol up before building the permanent copy). Until
  then, prefer keywords as map keys in loops. The same `createSymbol` call
  runs on every access to a global whose name is longer than 6 bytes.

- **Errors on actor threads are silent.** An actor message whose handler
  throws (a `StackOverflowError` included) sets the actor's value to `nil`
  and delivers `nil` to the promise `send` returned; the error is not
  reported. How a failed message should surface is an open design decision.

- **Promise `deref` polls.** A pending promise is checked every millisecond
  (with the thread marked unmanaged so garbage collection can proceed);
  adequate for hand-off latency, not for sub-millisecond waits.
- **A call with tens of thousands of arguments still needs a large heap.**
  Every C++ primitive receives its positional arguments as a `ProtoList`, and
  protoCore has no bulk bottom-up `ProtoList` constructor —
  `ProtoContext::newList(n, items)` is a single cell only up to five elements
  and an `appendLast` loop above that. So `[0 … 69999]`, which compiles to
  `(vector …)` with 70,000 arguments, must materialise a 70,000-element AVL
  list purely to hand it to `newTupleFromList`, and each append leaves a
  root-to-leaf path behind. Those intermediate versions are now reclaimed as
  the list grows, which halved what the program needs, but they are still
  allocated: `tests/cli/large-program` runs in about 3,000,000 cells where
  its live data is nearer 200,000, and it is the one check that does not pass
  under `PROTOCORE_HEAP_LIMIT_CELLS=2000000`. Removing the rest needs a bulk
  `ProtoList` constructor in protoCore, which would benefit every embedder.
- **`pmap` spawns one OS thread per element**, which is wasteful for large
  collections.
- **Actor `MPMC` throughput** is limited by the global ready-queue mutex.

## History

Shipped changes are listed in [`CHANGELOG.md`](../CHANGELOG.md); the commit
log on `main` records the rationale and measurements for each change.
