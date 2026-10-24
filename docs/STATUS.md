# protoClojure — Status

> Living tracker. This file changes every time something is implemented,
> finished, or dropped. If a feature you expect is not listed as
> implemented here, it is not implemented.

**Branch `feature/macros` (2026-10-24).** `ctest` registers 862 test
cases: 680 conformance fixtures, 137 GoogleTest unit tests and module
checks, 44 CLI checks and the documentation snippet check; all pass locally on Linux against protoCore 3.1.0
(branch `feature/lazy-object`, not yet released). CI has not run this branch.
The paragraph below describes the 0.0.3 tag.

**Current state.** Version 0.0.3 (2026-10-22), a preview tag. The interpreter runs
scripts and an interactive REPL. `ctest` registers 528 test cases: 408
conformance fixtures under `tests/conformance/`, 99 GoogleTest unit
tests for the lexer, the reader, the bytecode module, the runtime map,
its key semantics, the lifetime of values that used to be interned, the
vector representation, value equality and hashing, the
native stack guard, the double printer and exception values
(`tests/unit/`), and 21 CLI checks (`tests/cli/`: `--help`, three checks
of the adaptive heap controller, a
generated program with 70,000 distinct literals of each kind, the native
bulk builders under a heap ceiling, the garbage a `loop` makes under a heap
ceiling, joins that must park for the collector, actor message payloads
under a heap ceiling, a stack overflow in the REPL, globals bound to nil in
the REPL, source nested too deeply to read or compile, an uncaught
exception in a script and in the REPL, exceptions thrown under a heap
ceiling, `read-line` over standard input in pipelines,
`*command-line-args*`, exit statuses, `sh` with `:dir` and `:env` and no
shell on `PATH`, `localhost` requests to a server started without a host,
an HTTP server under a heap ceiling, and scripts run as commands through a
`#!` line); all 528 pass locally on Linux (2026-10-17). The last CI record
predates the six newest cases: 522 of 522 on Linux, macOS and Windows, none
skipped, against protoCore 2.14.1 (runs 37169479198, 37169480523), and on Windows also against the 2.7.0 minimum). Benchmark numbers against Babashka 1.4.192
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
| Exceptions: `try` / `catch` / `finally`, `throw`, `ex-info`, catchable primitive errors | `26-exceptions` | 50 |
| A module's error types caught by name (protoCore's example geometry module) | `tests/modules` | 1 |
| Input and output: files, `with-open`, `sh`, the environment, HTTP client and Ring server, TCP / UDP / TLS sockets | `27-io` | 70 |
| Quote, the quoting reader macros, file loading | `28-quote`, `29-loading` | 25 |
| Macros, syntax-quote, the `core.clj` macros | `30-macros`, `31-syntax-quote`, `32-core-macros` | 62 |
| Destructuring, sets, regular expressions | `33-destructuring`, `34-sets`, `35-regex` | 57 |
| `format` / `printf` / `for`, lazy sequences, the Part C vocabulary | `36-format-for`, `37-lazy-seqs`, `38-vocabulary` | 73 |
| `read-string`, EDN, JSON (cheshire.core, clojure.data.json), string escapes, `#_`, `parse-long` / `parse-double` | `39-data-formats` | 42 |
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
- [x] The reader yields runtime values (DESIGN.md §3, decision M1): a
      symbol or keyword is its interned named value, a string a protoCore
      string, a vector or map the runtime's own, `nil` / `true` / `false`
      the values themselves
- [x] Symbols (interned named values, `src/runtime/Named.h`)
- [x] Keywords — `:foo`, self-evaluating constants
- [x] Non-ASCII letters in symbols and keywords — UTF-8 encoded Unicode
      letters, combining marks and decimal digits (`año`, `:ñandú`,
      `:日本語`; table generated from Unicode 16.0 in
      `src/reader/UnicodeLetters.h`); other non-ASCII characters and
      malformed UTF-8 are read errors naming the character (D21); lexer
      columns count code points
- [x] Booleans + nil — `true`, `false`, `nil`, read as the values
- [x] Lists `(...)`
- [x] Vectors `[...]`
- [x] `format` and `printf` (Java's `String.format` conversions, D47);
      `for` with `:let` / `:when` / `:while` and `doseq` with the same
      modifiers (DESIGN.md §10.2, M27; `for` is lazy since stage 11);
      `apply` with leading arguments, `(apply f x y coll)`
- [x] Lazy sequences (DESIGN.md §7, §10.2, M28-M31): `lazy-seq`,
      `lazy-cat`, and lazy `map` (several collections too) `filter`
      `remove` `take` `take-while` `drop` `drop-while` `iterate` `repeat`
      `repeatedly` `cycle` `range` (with no end too) `concat` `mapcat`
      `partition` `partition-all` `interleave` `distinct` `for` `line-seq`
      `re-seq`; `doall` `dorun` `run!` `realized?` `chunked-seq?`; chunked
      as in Clojure; `seq?` true for every seq; walks hold only their
      position (locals clearing, M30)
- [x] Regular expressions `#"..."` (compiled when read; DESIGN.md §10.2,
      M25), `re-pattern`, `re-find`, `re-matches`, `re-seq`, `re-matcher`,
      `re-groups`, and `replace`, `replace-first`, `split` with a pattern,
      on `std::regex` ECMAScript (D45); `clojure.string/x` and
      `clojure.set/x` name the built-ins (M26)
- [x] Sets `#{...}` — a repeated element is a read error, `Duplicate key: :a`;
      a set of constants is built once, at compile time (DESIGN.md §10.2, M24)
- [x] Maps `{...}` — a repeated key is a read error, `Duplicate key: :a`
      (decision M2); the key and value forms are evaluated in source order
      (decision M3)
- [x] `@form` — read as `(clojure.core/deref form)`
- [x] Quote `'form` — read as `(quote form)`
- [x] Syntax-quote `` `form ``, unquote `~`, unquote-splicing `~@`,
      expanded at read time as Clojure's reader expands them
      (`(clojure.core/seq (clojure.core/concat ...))`), with symbols
      qualified (`clojure.core/x`, `user/x`, D42) and `foo#` auto-gensyms
      (`foo__N__auto__`). `~@` outside a list is the read error
      `splice (~@) not in list`; `~` or `~@` outside a syntax-quote is a
      compile error (D36)
- [x] `clojure.core/x` names the built-in `x` and `user/x` the program's
      global `x`; neither is shadowed by a local, and a `def` of `x` does
      not change `clojure.core/x`
- [x] Line comments `;`; commas as whitespace
- [x] `#!` line comments, as in Clojure (anywhere, to end of line), so a
      script starting with `#!/usr/bin/env protoclj` runs as a command
      (`tests/cli/shebang-scripts.sh`, `examples/13-filter-grep.clj`)
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
- [x] Destructuring (DESIGN.md §10.2, M23) in `let`, `loop`, `fn`, `defn`,
      `defmacro` parameters and the `core.clj` binding macros: sequential
      `[a b & r :as all]`, associative `{x :x :keys [...] :strs [...]
      :syms [...] :or {...} :as m}` with `:ns/keys`, nested forms; `:or`
      applies to missing keys only, as in Clojure (the `& {:keys ...}`
      keyword-argument path keeps D16)
- [x] `if`
- [x] `do`
- [x] `quote` — of symbols, keywords and other atoms (list quoting is not implemented)
- [x] `apply` — two-argument shape `(apply f list)`
- [x] `when`, `when-not`
- [x] `cond` (with `:else` / `else`)
- [x] `and`, `or` (short-circuit via DUP + JUMP_IF_TRUE/FALSE)
- [x] `future` — `(future body...)` compiles to `(make-future (fn [] body...))`
- [x] `try` / `catch` / `finally` and `throw` — catch clauses tried in
      order, a clause matching subclasses of its class, the same object
      rethrown when none matches; `finally` runs on the normal path, after a
      handled exception and when a catch clause throws; a throw in `finally`
      replaces the exception in flight; `recur` across a `try` is the compile
      error `Cannot recur across try`; `try` works at the top level, in
      `fn`, `let`, `loop` bodies, futures and actor handlers. The built-in
      classes are a fixed hierarchy of protoCore prototypes (`LANGUAGE.md`
      §14, `DESIGN.md` §4.1), named simply or qualified
      (`java.io.IOException`, `clojure.lang.ExceptionInfo`); `Object` and
      `Throwable` catch every value
- [x] protoCore's unified exception model: every throw is a
      `proto::ProtoException`, every handler selects by the value's
      prototype chain, so a module's error type is caught by name,
      `(catch geometry/NegativeRadius e ...)`, resolved when an exception
      reaches the clause (D34); protoCore's own errors arrive as Clojure
      classes through the error factory (`DESIGN.md` §4.1 has the table)
- [x] `(import-module "logical/path")` loads a module through Unified
      Module Discovery; `alias/member` reads a member and
      `(alias/member args...)` calls it with the module as the receiver
      (`INTEROP.md` §0)
- [x] Errors raised by the runtime are exceptions of the class their message
      names: `(try (/ 1 0) (catch ArithmeticException e (ex-message e)))` is
      `"Divide by zero"`; `StackOverflowError` is an `Error` a `try` can
      catch; an error naming no class is a `RuntimeException` (D26)
- [x] Variadic `& rest` in fn params
- [x] Named-argument destructuring `& {:keys [...] :or {...} :as name}`

### Data structures

- [x] Lists — protoCore `ProtoList`
- [x] Vectors — a protoCore `ProtoList` of the elements in a one-entry
      `ProtoSparseList` box (`src/runtime/VectorOps.h`): O(log N) `nth`,
      O(1) `count`, O(1) construction and O(1) coercion to a sequence, and
      collected like any other value. They were interned `ProtoTuple`s, which
      protoCore never frees (decision R2, CHANGELOG)
- [x] Maps — an immutable protoCore `ProtoMap`, read and written through
      protoCore's shared hashed-collection helper with protoClojure's key
      semantics (`src/runtime/MapOps.h`), with `hash-map` / `assoc` /
      `dissoc` / `get` / `contains?` / `keys` / `vals` / `map?`; `count`
      (O(n), D25) and `empty?` (O(1)) accept maps. Iteration and print order
      are unspecified and can change between runs. `assoc` of an equal key
      keeps the stored key object and replaces the value
- [x] Map equality — `=` / `not=` compare maps by value, whatever the order
      (`mapEquals` in `src/runtime/MapOps.h`): the same keys with
      `=` values; values compare recursively, so nested collections compare
      by value; a map is never `=` to a vector or a list
- [x] Sequential equality — `=` / `not=` compare lists and vectors element
      by element, whatever their concrete types: `(= [1 2] (list 1 2))` and
      `(= [] (list))` are true, nested collections compare by value, and a
      list or vector is never `=` to a map, a string or `nil`
      (`valuesEqual` in `src/runtime/Primitives.h`)
- [x] Map key semantics — `keyEquals` / `keyHash` in
      `src/runtime/MapOps.h` (table in LANGUAGE.md §4.3): strings by
      content, a list and a vector with equal elements as one key, maps by
      their entries in any order, doubles by bit pattern, big integers by
      exact value, keywords, symbols and other objects by identity; so
      `(get {{:a 1} :x} {:a 1})`, `(get {[1 2] :x} (list 1 2))` and
      `(get {"ab-cd-ef" 1} (str "ab-" "cd-ef"))` find their entries.
      Numbers of different types are different keys (D15) and `##NaN` finds
      itself (D22). Keys are ordinary garbage: nothing on the key path is
      interned (D23, withdrawn)
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

### Input and output (`LANGUAGE.md` §18, `DESIGN.md` §4.2)

On the protoIO library (linked statically; the package depends on OpenSSL
only). Every blocking call runs outside the collector's quorum
(`ProtoContext::UnmanagedScope`), with its arguments copied to C++ values
first and its result built after.

- [x] Files: `slurp` (files, readers, sockets, `http(s)` URLs), `spit`
      with `:append`, `reader`, `writer` (`:append`), `with-open` (closes
      in reverse order on return and on exception), `read-line` (standard
      input, readers, sockets; nil at the end), `write`, `close`,
      `line-seq`, `file`, `file-seq`, `exists?`, `directory?`,
      `delete-file`, `make-parents`, `copy`; UTF-8 only
- [x] Programs: `sh` with `:in`, `:dir`, `:env`, answering
      `{:exit :out :err}`; 2 MiB fed to a program that never reads it
      neither kills nor hangs the runtime
- [x] The running program: `getenv` (one variable or all), `exit`,
      `*command-line-args*` bound to the arguments after the script's path
- [x] HTTP client in babashka http-client's shape: `http-get`,
      `http-post`, `http-put`, `http-delete`, `http-head`, `http-request`,
      answering `{:status :headers :body}`; an exceptional status throws
      ex-info carrying the response unless `:throw false`; `:headers`,
      `:body`, `:query-params`, `:timeout`, `:follow-redirects`; a
      cross-origin redirect drops the caller's headers, https to http is
      refused
- [x] HTTP server on Ring's contract: `run-server`, `stop-server`,
      `server-port`; request map with `:request-method`, `:uri`,
      `:query-string`, `:headers`, `:body`, `:remote-addr`, ...; 400 / 414
      / 431 / 413 before the handler runs; a handler exception, a non-map
      answer or a response header with a line break is a plain 500; each
      connection on its own ProtoThread, never on the actor pool
- [x] Sockets: `tcp-connect` (`:tls`, `:timeout`, `:verify`),
      `tcp-listen`, `tcp-accept` (timeout -> nil), `socket-read-line`,
      `socket-write`, `socket-close`, `udp-socket`, `udp-send`,
      `udp-receive` (timeout -> nil)
- [x] Errors: `FileNotFoundException`, `IOException`, `ConnectException`,
      `SocketTimeoutException`, `UnknownHostException`, `SocketException`
      and `IllegalArgumentException`, with ex-data
      `{:type :file-not-found :errno 2}` and the like

### Primitives installed at startup (106, plus 36 I/O, plus core.clj)

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
- [x] Sequences: `seq` (nil for an empty collection; a vector's chunked
      seq; a lazy seq's realised seq; a map's entries as `[k v]` vectors; a
      string's characters) and `concat` (lazy when an argument is a lazy
      seq, M29)
- [x] `second next last conj seq?` (`next` is nil past the last element;
      `conj` adds at the front of a list, at the end of a vector, an entry
      to a map) and the printers `pr-str prn pr print` (readable or not, as
      Clojure's)
- [x] Symbols: `gensym symbol keyword name list*`
- [x] `even? odd?` (integers of any size)
- [x] Macros: `macroexpand-1 macroexpand eval`, and `__macro__`, the
      internal built-in `defmacro` compiles to (DESIGN.md §10.1, M10)
- [x] Higher-order: `map filter reduce pmap`
- [x] Predicates: `nil? not vector? list? map? string? symbol? keyword?`
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
- [x] Exceptions: `ex-info ex-data ex-message ex-cause`. An exception
      prints as `#error {:type ExceptionInfo, :message "boom", :data {:a 1}}`
      and `str` renders its toString, `ExceptionInfo: boom {:a 1}` (D27)

### Bytecode VM

- [x] 39 opcodes — see `src/runtime/Opcodes.h`
- [x] Exception opcodes `TRY_BEGIN` / `TRY_END` / `THROW` / `STORE_CAUGHT` / `RETHROW` /
      `EXC_MATCH` / `EXC_MATCH_TYPE` / `RESOLVE_CLASS`, and the module-member
      opcodes `GET_MEMBER` / `CALL_MEMBER`,
      over a per-frame handler stack; only a body containing a `try` runs in
      a frame with the C++ catch region, so code without one runs at the
      speed it had before exceptions (`DESIGN.md` §4.1)
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
      future whose body raised an exception raises an `ExecutionException`
      whose cause (`ex-cause`) is that exception and whose message is its
      toString (`@(future (/ 1 0))` raises
      `ExecutionException: ArithmeticException: Divide by zero`), as JVM
      Clojure does, and `pmap` raises the exception of the first failing
      element in input order the same way, after waiting for every element
- [x] **Actors** (`actor`, `send`, `send-h` / `send-m` / `send-l`, `actor?`,
      `actor-stats`) on a configurable worker pool (`PROTOCLJ_ACTOR_WORKERS`,
      default `max(2, cores-2)`, cap 16). Three priority bands, single-method
      invariant, and three protoCore `ProtoMPSCQueue` mailboxes per actor —
      lock-free on the send path and traced by the collector. Peak throughput
      with a trivial
      body, 1,000,000 messages per mode (2026-09-24, shared machine at load
      average 5.8): 222,074 msg/s (single), 552,486 (fan-out), 201,329
      (MPSC), 329,817 (MPMC). Against the pre-migration mailbox the
      single-consumer modes are 6-33% slower and fan-out is 34-80% faster —
      the full before/after tables and why are in
      [`benchmarks/RESULTS.md`](../benchmarks/RESULTS.md).

### REPL

- [x] Interactive `protoclj` REPL on libreadline (history persisted to
      `~/.protoclj_history`, multi-line continuation prompt `#_=>`,
      `:help` / `:quit` / `:load` / `:time` meta-commands)
- [x] `*1` `*2` `*3` bindings to the last three evaluation results

### Tooling

- [x] `protoclj <script.clj> [args...]` — single-file evaluation; the
      arguments after the script are `*command-line-args*`
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

- [ ] Anonymous-fn shorthand `#(...)`
- [ ] Var-quote `#'`
- [ ] Discard `#_`
- [ ] Metadata `^{...}` plus shorthands `^kw` `^Type`
- [ ] Reader literals `#inst` `#uuid`
- [ ] Ratio literal `1/3`
- [ ] Character literal `\a`
- [ ] Namespace-qualified symbols / keywords `foo/bar`, `:ns/kw`

### Special forms (not yet)

(`let` and `loop` at the top level and docstrings in `defn` / `defmacro`
work since macros stage 4.)

- [ ] `let*` / `fn*` raw forms
- [ ] `var` form
- [x] `letfn` (a `core.clj` macro over atoms; DESIGN.md §10.2, M34; D53)
- [x] `#(...)` with `%`, `%N`, `%&` (M32)

### Data structures (not yet)

- [x] Lazy seqs — protoCore lazy objects (M28)
- [ ] `hash` as a function
- [ ] Vectors as functions (`(v 0)`)

### Namespaces & vars (not yet)

- [x] `ns` form, for the `:require` of the library namespaces (M33, D52)
- [x] `:require :as :refer` and a top-level `require`, for clojure.core,
      clojure.string, clojure.set, clojure.java.io, clojure.java.shell
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

Since Part B, stage 7, a `& {...}` map with `:strs`, `:syms`, `{x :x}`
entries or nested forms destructures the rest arguments as a map instead
(DESIGN.md §10.2, M23); `:keys`, `:or` and `:as` alone keep the
keyword-argument path.

See the archived
[call-convention design](archive/design-specs/2026-06-14-protocore-call-convention.md)
for the rationale.

### Core library extensions (not yet)

- [x] `quot rem mod` (M34)
- [ ] More collection: `interpose` (`into mapcat interleave next conj
      second last seq concat` exist)
- [x] Map operations: `update merge merge-with select-keys get-in update-in
      assoc-in zipmap key val`
- [x] More higher-order: `comp partial juxt complement constantly fnil`
- [x] More predicates: `every? some not-any? not-every?`
- [x] `compare max min max-key min-key`, `clojure.string/split-lines`,
      `(System/exit n)` (D54)
- [x] Range / sequence: `iterate repeat cycle take drop take-while drop-while partition partition-all`, and `(range)` with no end (stage 11)
- [x] `frequencies group-by sort sort-by`

### REPL and command line (not yet)

- [ ] `*e` (last exception) — exception values exist; the REPL does not bind them yet
- [ ] `(doc symbol)`, `(source symbol)` — needs docstring storage on `defn`
- [ ] `protoclj -e <expr>`
- [ ] nREPL server (CIDER / Calva / Conjure compatible) — planned for v0.1

### Macros

Work in progress on branch `feature/macros` (DESIGN.md §10, ROADMAP.md).

- [x] Stage 1: the reader yields runtime values and the compiler consumes
      them (DESIGN.md §10.1, M1-M4)
- [x] Stage 2: `quote` of any datum; the reader macros `'`, `` ` ``, `~`,
      `~@` (M5-M8)
- [x] Stage 3: scripts are read, compiled and run one top-level form at a
      time, as the REPL is; a compile error names its form's line and column
      (M9)
- [x] Stage 4: `defmacro` (docstring, attribute map, multi-arity, `&form`,
      `&env`), compile-time expansion through the engine, recursive
      expansion, the timing rule, `macroexpand-1`, `macroexpand`, `eval`;
      a top-level `do` evaluated form by form (M10-M16)
- [x] Stage 5: syntax-quote qualifies symbols as Clojure's reader does
      (`clojure.core/x` for a built-in, `user/x` otherwise; special forms,
      `&` and class names unqualified), `foo#` auto-gensyms, `gensym`,
      `symbol`, `keyword`, `name`, `list*`; `user/x` and `clojure.core/x`
      resolve to globals, never to locals (M17-M19)
- [x] Stage 6: `core.clj`, embedded in the binary and loaded at start-up
      (about 3 ms of CPU), defines `when when-not if-not cond comment while
      if-let when-let if-some when-some -> ->> as-> some-> some->> cond->
      cond->> doto dotimes doseq case condp with-open future` and `identity
      some? true? false? zero? pos? neg? nnext ffirst nthnext butlast`;
      `when`, `when-not`, `cond`, `with-open` and `future` moved there from
      the C++ compiler, `and` and `or` stayed (M20-M22)

### Scripting vocabulary (macros brief, Part B)

On branch `feature/macros` (DESIGN.md §10.2, ROADMAP.md).

- [x] Stage 7: destructuring in `let`, `loop`, `fn`, `defn`, `defmacro`
      and the binding macros (M23)
- [x] Stage 8: sets, `clojure.set`'s `union` / `intersection` /
      `difference` / `subset?` / `superset?`, `into` (M24)
- [x] Stage 9: regular expressions on `std::regex` ECMAScript (M25, D45);
      `clojure.string/x` and `clojure.set/x` name the built-ins (M26)
- [x] Stage 10: `format`, `printf`, `for` and `doseq` with `:let` /
      `:when` / `:while`, a finite `range`, `apply` with leading arguments
      (M27, D46, D47)
- [x] Stage 11: lazy sequences (M28-M31; closes D29 and D46; D48-D51);
      protoCore 3.1.0's lazy objects
- [x] Stage 12: `read-string` and `clojure.edn/read-string` on the reader
      (M35, D55, D56); JSON as `cheshire.core` and `clojure.data.json` over
      one C++ parser and writer (M36-M38, D57); `pr-str` / `read-string`
      round-trip every printable value; the string escapes `\uXXXX`,
      octal, `\b`, `\f` and `#_`; `parse-long`, `parse-double`,
      `parse-boolean` (M40, D58); `pmap` joins every worker (M39)
- [x] Stage 13: showcase against Babashka (`examples/showcase/`, README
      "Showcase", `benchmarks/reports/2026-10-23-showcase/`); the
      vocabulary its scripts and the tutorial needed: `+'` `-'` `*'` `inc'`
      `dec'`, `bigint`, `biginteger`, `mapv`, `filterv`, `number?`,
      `integer?`, `int?`, `float?`, `double?`, `==`, `hash`, `peek`, `pop`,
      a vector as a function of its index (D59); the scripting tutorial
      chapter; every runnable documentation snippet checked by
      `tests/docs/check_snippets.py` (ctest `docs/snippets`)

### Bootstrap

- [x] A `core.clj` evaluated at startup (`src/core/core.clj`, embedded in
      the binary); the primitives stay in C++, new library code can be
      written in Clojure there

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
| D13 | `read-string` strict on unregistered reader literals: there are no tagged-literal readers at all (`#inst`, `#uuid`, data readers), so `#tag x` is the read error `No reader function for tag tag` | core   |
| D14 | LargeInteger promotion is automatic in `+ - * / inc dec`, so `(* 9223372036854775807 2)` is `18446744073709551614` where JVM Clojure throws `ArithmeticException: integer overflow` — no `+'` / `*'` needed; big integers are not a separate type, so the `N` suffix is accepted but does not change the value or how it prints (CONTRA Clojure-JVM, by design) | (perm) |
| D15 | `(= 1 1.0)` returns `true` in v0.x (CONTRA Clojure-JVM where `=` is type-strict). Map keys do **not** follow `=` across numeric types: `1`, `1.0`, an equal big integer, and `0` / `-0.0` are different keys, as in JVM Clojure, so `(hash-map 1 :a 1.0 :b)` has two entries and `(= {1 :a} {1.0 :a})` is `false` | (perm) |
| D16 | In the keyword-argument form `& {:keys [...] :or {...}}` of a parameter vector, `:or` defaults fire on **explicit nil** as well as missing keys (CONTRA JVM-Clojure where only missing keys take the default). Every other map binding form (`let`, positional parameters, `& {:strs ...}`, ...) follows Clojure (DESIGN.md §10.2, M23) | v0.2 |
| D17 | String ops (`upper-case`, `lower-case`, `split`, `reverse`, `trim`, `index-of`) are byte-level / ASCII-correct only; multi-byte UTF-8 codepoints traverse as bytes (CONTRA JVM-Clojure which is codepoint-aware) | v0.2 |
| D18 | `(fn name [args] body)` — the name is accepted by the compiler but dropped; self-reference via `name` inside the body is not supported (use `defn` for self-recursion). Planned for v0.2 via wrapper-into-slot capture. | v0.2 |
| D20 | Atoms, futures, promises, actors and fns print as tags such as `#<atom 1>`, `#<fn>` and `#<fn println>` in `println`, `str` and the REPL (JVM Clojure: `#object[clojure.lang.Atom 0x... {:status :ready, :val 1}]` when printed, and `clojure.lang.Atom@...` or the class name under `str`) | v0.x |
| D21 | Beyond ASCII, symbols and keywords accept only Unicode letters, combining marks and decimal digits: `a→b`, or a symbol containing a no-break space, is a read error (CONTRA JVM-Clojure, whose reader accepts any character that is neither whitespace nor a macro character) | v0.x |
| D22 | A NaN map key is found by a NaN with exactly the same 64-bit pattern: `(get {##NaN :n} ##NaN)` is `:n` (CONTRA JVM-Clojure, which never finds a NaN key). A NaN produced by arithmetic can have a different bit pattern and then does not find a `##NaN` key: on x86-64, `(/ 0 0.0)` has its sign bit set | (perm) |
| ~~D23~~ | **Withdrawn.** Map keys used to be interned and never freed. Since maps moved onto protoCore's `ProtoMap` and its hashed-collection helper, a key is stored as the object the caller passed and dies with the last map that holds it, as in JVM Clojure | — |
| D24 | Map and set iteration and print order is unspecified for every size and can change between runs of the same program (JVM Clojure keeps insertion order for array maps of up to 8 entries; its hash sets have a fixed hash order) | (perm) |
| D25 | `count` of a map or a set is O(n), not O(1) (JVM Clojure: O(1)). Two keys whose hashes collide in their low 54 bits share one slot of the underlying `ProtoMap`, so the slot count is a lower bound on the number of entries, not the number itself; `count` therefore walks the entries. `empty?` stays O(1), because a slot always holds at least one entry | (perm) |
| D26 | A runtime error whose message names no class is a `RuntimeException` carrying the whole message: most arity and argument-type errors raised by primitives (`(inc 1 2)`: `inc: expects 1 arg`, where JVM Clojure raises `ArityException`; `(upper-case 1)`: `upper-case: arg must be a string`, where it raises `ClassCastException`), and an unresolved global, which is caught at run time as a `RuntimeException` where JVM Clojure reports it at compile time. The class-naming errors (`ArithmeticException`, `ClassCastException` for arithmetic on non-numbers, `IndexOutOfBoundsException`, `StringIndexOutOfBoundsException`, `UnsupportedOperationException`, `StackOverflowError`) have JVM Clojure's classes | v0.x |
| D27 | Exceptions are not Java objects: there are no stack traces, no user-defined exception classes, no `class` / `instance?` and no `.getMessage`-style interop; `catch` names one of 24 built-in classes, `Object`, or a type held by a var or a module member (D34, `DESIGN.md` §4.1). An exception prints on one line as `#error {:type C, :message "m", :data {...}, :cause #error {...}}` (JVM Clojure: a multi-line `#error` map with `:via`, `:trace` and a root-cause `:cause` string), and its toString and the uncaught-error report use the simple class name, `ArithmeticException: Divide by zero` (JVM Clojure: `java.lang.ArithmeticException: Divide by zero`). `throw` of a value that is neither an exception nor a foreign (module) object raises `ClassCastException: throw expects an exception, got an integer` | (perm) |
| D28 | The I/O functions are globals, not in `clojure.java.io`, `clojure.java.shell` or babashka's `babashka.http-client` (there are no namespaces yet), and so are `clojure.set`'s `union`, `intersection`, `difference`, `subset?` and `superset?`, and small functions stand in for Java interop: `close` / `write` / `(read-line r)` for `.close` / `.write` / `.readLine`, `exists?` / `directory?` for `.exists` / `.isDirectory`, `getenv` / `exit` for `System/getenv` / `System/exit`. `file` answers a protoClojure file handle, not a `java.io.File` (`str` gives its path; `file-seq` answers such handles); readers and writers are UTF-8 only, with no InputStream / Reader stack | (perm while there are no namespaces) |
| ~~D29~~ | *Closed in Part B stage 11*: `line-seq` is a lazy seq read one line at a time as it is consumed, as in JVM Clojure (and, as there, reading it after its reader closed raises `IOException: Stream closed`) | closed |
| D30 | Sockets are protoClojure's own functions (`tcp-connect`, `tcp-listen`, `tcp-accept`, `socket-read-line`, `socket-write`, `socket-close`, `udp-socket`, `udp-send`, `udp-receive`, `server-port`): Clojure has only Java interop (`java.net.Socket`) | (perm) |
| D31 | The Ring request `:body` is a string (nil when empty), not an InputStream, and a response `:body` may be a string, nil, a list or vector of strings or a file, not an InputStream; `run-server` answers a server handle (http-kit answers a stop fn), serves one request per connection (`connection: close`, no keep-alive), each on its own thread, and a server still running when the script ends is stopped (the JVM keeps running while a server thread lives) | v0.x |
| D32 | `sh` does not support `:in-enc` / `:out-enc` (output is always decoded as UTF-8). (`:dir` and `:env` no longer deviate: since protoIO 0.2.1 the spawn call sets them itself, with no `/bin/sh` or `env -i` in between, on every platform; the program is searched in protoclj's own `PATH`, as the JVM searches it) | v0.x |
| D33 | I/O exceptions carry an ex-data map (`{:type :file-not-found :errno 2}`), so their toString and the uncaught-error report end with it, and their messages are protoIO's (`cannot read x: No such file or directory`, JVM: `x (No such file or directory)`) | (perm) |
| D35 | *Closed in macros stage 5*: syntax-quote qualifies symbols (`user/f`, `clojure.core/map`) and `foo#` is an auto-gensym, as in Clojure. What remains is D42 | closed |
| D36 | `~x` or `~@x` outside a syntax-quote is a compile error, `unquote (~) used outside a syntax-quote` (JVM Clojure compiles it as a call of the unbound var `clojure.core/unquote` and fails at run time with `Attempting to call unbound fn`) | (perm) |
| D37 | *Closed in macros stage 5*: every built-in is also installed under `clojure.core/x`, which a `def` of `x` does not replace, so a syntax-quote expansion keeps calling the built-in after the program redefines `seq` or `concat` | closed |
| D38 | A macro's `&env` maps each local's symbol to the symbol itself (JVM Clojure: to the compiler's `LocalBinding` object); it is nil at the top level, as in Clojure | (perm) |
| D39 | A macro called with the wrong number of arguments reports the count as written, `Wrong number of args (2) passed to macro: one` (JVM Clojure counts the hidden `&form` and `&env` too, `Wrong number of args (4) passed to: user/one`) | (perm) |
| D40 | A form `eval` cannot compile raises `RuntimeException` `Syntax error compiling: ...` (JVM Clojure: `clojure.lang.Compiler$CompilerException`, a `RuntimeException` subclass) | (perm) |
| D41 | `and`, `or`, `fn`, `let`, `loop`, `defn`, `defmacro` and `apply` are compiled by the C++ compiler, so `macroexpand` returns them unchanged (JVM Clojure expands `and`, `or`, `fn`, `let`, `loop` and `defn` into `let*`, `fn*`, ...). `and` and `or` stay in C++ for speed (DESIGN.md §10.1, M21) | (perm) |
| D42 | There are two namespaces only, `user` (the program: every file, the REPL, `eval`) and `clojure.core` (the built-ins and, from stage 6, `core.clj`), and no `ns` form (D28). Syntax-quote qualifies a name the program has not defined and no built-in has as `user/x`; `user/x` names the global `x` and `clojure.core/x` the built-in, and neither is ever resolved to a local. A built-in exception class name in a template stays a simple name (`Exception`; JVM Clojure writes `java.lang.Exception`). `user/x` cannot name a module alias called `user` | (perm until namespaces) |
| D43 | Symbols are interned and never collected, generated ones included: a program that calls `gensym` or `symbol` on fresh names without end grows by one symbol per name (JVM Clojure collects unreferenced symbols) | (perm) |
| D44 | `case` tests its constants in order with `=` (O(n) in the number of clauses; JVM Clojure dispatches on a hash in constant time), so numbers of different types match (D15); the result is otherwise Clojure's, including `IllegalArgumentException: No matching clause: x` | (perm) |
| D45 | Regular expressions run on the C++ standard library's `std::regex`, ECMAScript grammar, over the UTF-8 bytes of the string (JVM Clojure: `java.util.regex` over UTF-16). Translated from Java: the leading inline flags `(?i)` `(?m)` `(?s)` (`(?u)` `(?U)` accepted, no effect), named groups `(?<n>X)` (numbered; `\k<n>` is not supported), `\Q...\E`, `\A` / `\z` / `\Z` outside multiline mode. Not supported, a `PatternSyntaxException`: lookbehind `(?<=` `(?<!`, possessive quantifiers `*+`, atomic groups `(?>`, `\p{...}` / `\P{...}` classes, class intersection `[a&&b]`, flags in mid-pattern, `(?x)` comments, `(?d)`. Different: `.`, `\w`, `\s` and character classes see bytes, so a multi-byte UTF-8 character is several (D17); `$` in non-multiline mode matches only at the very end (Java also before a final line terminator); case folding is ASCII; the captures of a group repeated by `*` that matched empty may differ (`(a*)*b`). Every distinct pattern source is compiled once and kept for the process (like interned symbols, D43). A pattern with back-references backtracks on the native stack, so a match that could exhaust it raises `StackOverflowError` beforehand (about 32 KB of subject); others are not limited. No `Matcher` interop beyond `re-matcher` / `re-find` / `re-groups` | (perm: engine) |
| ~~D46~~ | *Closed in Part B stage 11*: `for`, `range` and `re-seq` are lazy, and `(range)` counts without end | closed |
| D48 | `(str lazy-seq)` is `clojure.lang.LazySeq@` followed by a hash of the elements in hexadecimal, as JVM Clojure renders it, but the digits differ from the JVM's (protoClojure's hash, D25). `pr-str`, `println` and the REPL print the elements, as in Clojure | (perm) |
| D49 | Lazy sequences, as built (DESIGN.md M29): no transducers (`(map f)`, `(filter p)`, `(mapcat f)`, `(partition n)` with no collection raise `UnsupportedOperationException`), no chunk API beyond `chunked-seq?` (`chunk-first`, `chunk-rest`, `chunk-cons`, `chunk-buffer`); a vector's chunks start where its seq starts (JVM Clojure: at its 32-element tree leaves, so `(rest v)` begins with a 31-element chunk there and a 32-element one here); `realized?` answers true for a cons or a chunk (JVM Clojure: ClassCastException for a Cons) and false for any other non-pending value; `apply` and `pmap` realise their sequence argument to its end (JVM Clojure's `apply` passes an endless seq to a variadic fn lazily); a seq read while its own body computes it raises `StackOverflowError` at once (JVM Clojure recurses until the stack overflows) | (perm) |
| D50 | Locals clearing is conservative (DESIGN.md M30): a local is cleared at its read only when it is read exactly once in its fn and outside any loop it is not rebound in. A local read in both branches of an `if`, or twice, keeps what it holds until its scope ends (JVM Clojure clears it at the last use on every path), so `(let [s (line-seq r)] (if (seq s) (count s) 0))` holds the head while `count` walks | v0.2 |
| D51 | At the REPL a definition echoes as its var, `#'user/x` (M31), but `*1` holds the defined value: there are no vars (D28) | (perm until vars) |
| D52 | `require` and `ns` know only the library namespaces protoClojure provides as built-ins (clojure.core, clojure.string, clojure.set, clojure.java.io, clojure.java.shell; any other raises `FileNotFoundException`), and `:as` aliases live in one table for the process: an alias recorded by any file or at the REPL holds for every form compiled afterwards (JVM Clojure: per namespace). `:refer` changes nothing, every function being a global; `ns` ignores its name and has no `*ns*` (D28) | (perm until namespaces) |
| D53 | `letfn` binds each name to a fn that calls its definition through an atom, so every call through a `letfn` name costs an `apply` and a `deref` more than a direct call (JVM Clojure: `letfn*` binds the fns themselves); `(fn name ...)` still cannot call itself by name (D18) | v0.2 |
| D54 | `System` is an object standing in for `java.lang.System` with the members `exit`, `getenv`, `currentTimeMillis` and `nanoTime` only; a map entry is a `[k v]` vector, which `key` and `val` read (JVM Clojure: a `MapEntry`, also a vector) | (perm) |
| D47 | `format` / `printf` implement Java's `String.format` conversions `%s %S %d %x %X %o %f %e %E %g %G %b %B %c %C %n %%` with argument indexes, flags, width and precision, but not `%h`, `%a`, the date/time `%t` conversions, nor locale-specific output. Errors are `IllegalArgumentException`s whose message starts with Java's exception name and names a protoClojure type (`IllegalFormatConversionException: d != a float`; Java: `d != java.lang.Double`). `%g` rounds an exact tie to even (Java: half up; `%f` and `%e` round half up, as Java). Widths count bytes, so a multi-byte UTF-8 character counts as several (D17) | (perm) |
| D55 | Strings are UTF-8, so a `\uXXXX` escape naming an unpaired surrogate (`"\uD83D"` alone) is the read error `Invalid unicode escape: unpaired surrogate` (JVM Clojure keeps the lone UTF-16 unit in its string); a pair (`"\uD83D\uDE00"`) is the one character it encodes, so `count` gives 1 where JVM Clojure gives 2 (D17) | (perm) |
| D56 | `read-string` and `clojure.edn/read-string` (DESIGN.md M35): a read error is a `RuntimeException` carrying the reader's own message and its line and column in the string (`Map literal must contain an even number of forms (line 2, column 1)`; an unterminated form says `unterminated vector — missing ']'`, JVM Clojure `EOF while reading`); the EDN reader rejects `'`, `` ` ``, `~`, `~@` and `@` with `EDN does not support the reader macro '` (JVM Clojure's EdnReader reads them as part of a symbol); `read-string` takes only the `:eof` option (no `:read-cond` / `:features`), and `clojure.edn/read-string` ignores `:readers` and `:default` (there are no tagged literals, D13); there is no `read` on a reader, nor `clojure.edn/read` | v0.x |
| D57 | JSON (DESIGN.md M36-M38). Both APIs: error messages are protoClojure's own, `JSON error: <problem> at line L, column C` (Jackson and clojure.data.json word them differently), raised as `IOException` by cheshire.core and `Exception` by clojure.data.json; an unpaired surrogate escape is an error (Jackson accepts it); map keys are written in the map's unspecified order (D24); a stream (`parse-stream`, `clojure.data.json/read`) is read a line at a time and the rest of the line after the value is consumed with it. cheshire.core: a top-level array parses to a vector (Cheshire: a lazy seq; nested arrays are vectors in both); `parse-string`'s third argument (array-coerce-fn), `parsed-seq`, the SMILE / CBOR functions, custom encoders and `generate-string` options other than `:pretty`, `:escape-non-ascii` and `:key-fn` are not supported. clojure.data.json: its behaviour here follows the library's documentation, not a run against it (Babashka does not bundle it); `read-str` raises on data after the value; `:bigdec true` raises `UnsupportedOperationException` (no BigDecimal, D10); `write-str`'s `:value-fn` and `pprint` are not supported | v0.x |
| D59 | `hash` answers protoClojure's map-key hash folded to 32 bits, consistent with `=` for map keys (a list and a vector with equal elements hash alike) but not the numbers JVM Clojure answers; `int?` is true for an integer of any size, integers being one type (D14; JVM Clojure: fixed-width only); a vector called as a function takes exactly one index, as in Clojure | (perm) |
| D58 | `parse-double` reads what Java's `Double.valueOf` reads except hexadecimal floats (`0x1p3`) and the `d` / `f` suffixes (`1.5d`), which answer nil | (perm) |
| D34 | A `catch` naming no built-in class names a type looked up at run time, when an exception reaches the clause (a var, a local, or a module member `alias/Type`), and is matched by the thrown value's prototype chain; JVM Clojure resolves the class at compile time. So `(try 1 (catch NoSuchException e 2))` compiles and answers 1, and the error `Unable to resolve classname: NoSuchException` is a `RuntimeException` raised when an exception reaches the clause (JVM Clojure: a compile error). A type that resolves to a value that is not a prototype object raises `IllegalArgumentException`. A module's error (a foreign value) is caught by `Exception`, `Throwable` and `Object`, by its own types, and by no other built-in class | (perm: the platform is late-binding) |

## Known issues

- **Regular expressions differ by standard library (std::regex), found by
  the first macOS and Windows CI runs of the macros branch, 2026-10-24.
  Severity: low.** The engine is each platform's `std::regex` (D45).
  - libc++ (macOS) ignores `match_prev_avail` for `^`, so a continued search
    (re-seq, a matcher's re-find, replace, split) is given `match_not_bol`
    and `match_not_bow` from the byte before it instead (Regex.h,
    `continuationFlags`). One case cannot be said with those flags: `\b`
    right after a word byte and before a non-word byte, at the start of a
    continued search, does not match there. libc++ backtracks on the heap,
    so the native-stack guard never refuses a pattern; a back-reference
    pattern on a huge subject matches (and may use much memory) where
    libstdc++ raises StackOverflowError.
  - MSVC's standard library has no `std::regex::multiline`, and its
    ECMAScript `^` (and `$`) match at line terminators whatever the flags:
    on Windows every pattern behaves as if `(?m)` were given, so `^\w` finds
    a match at the start of each line where Java finds one only at the start
    of the input. Its engine bounds its own recursion and work; a match that
    goes past those bounds raises StackOverflowError (as Java's overflows),
    so long subjects with long matches may fail there: `(a|b)*` over
    300 KB does (`.*` passes), and `conformance/35-regex/long-subject.clj`
    is registered WILL_FAIL with MSVC (tests/CMakeLists.txt) until the
    engine decision below.
  A single engine everywhere (a bundled one) would remove all three
  differences; that is a dependency decision left to the maintainer.

- **Start-up reads and compiles core.clj every time: about +17 M cycles
  (about 4 ms of CPU) over the baseline before macros. Severity: low. Needs a
  design decision.** Measured 2026-10-24
  (`benchmarks/reports/2026-10-24-regression/`): an empty file costs 64.1 M
  cycles against the baseline's 47.3 M; `loadCore` (81 top-level forms,
  reading a third, compiling and running two thirds) is nearly all of the
  difference, after the intern caches took 4 M cycles off it. The only large
  saving left is not to compile `core.clj` at start-up: a build-time
  snapshot of its bytecode modules and constant pools (quoted data, named
  values, macro functions), or compiling each definition on first use (a
  hook at every global miss, macro lookup and syntax-quote resolution, plus
  a once-per-definition load that parks for the collector). Both change how
  the runtime boots; neither is done. Registering built-ins lazily would
  save under 1 M cycles.

- **Resolved on 2026-10-10: protoclj now runs protoCore's adaptive heap
  controller** (`src/main.cpp`, README "Memory"), which starts cycles under
  heap pressure within a budget of 75 % of memory; the `churn` program below
  now collects with nothing in the environment. The entry is kept as it was
  written. Built against a protoCore older than 2.10.0 there was no
  controller and the issue remained (the floor is now 3.0.0).

  **Nothing is ever collected unless a heap ceiling is set. Severity: high.
  The fix belongs in protoCore, so it is not made here.** protoCore starts a
  collection cycle from exactly one place, the heap-limit enforcement path
  (`ProtoSpace::reclaimWaitLocked`, reached from `waitForHeapHeadroom`).
  `triggerGC()` is advisory — it raises the request only when the free-cell
  ratio is under 0.2 or one is already pending — and, since the
  allocation-budget trigger was reverted (protoCore `d8ceb7e7`, `c9320180`),
  *nothing inside protoCore calls it at all*. So with no `softHeapLimit` and
  no `maxHeapSize`, protoClojure grows the heap for ever and never collects,
  however faithfully the VM submits its young generations.

  Reproduction, on a build with the VM's safepoint working:

      $ churn() { cat <<'CLJ'
      (defn churn [n]
        (loop [i 0 acc 0]
          (if (>= i n) acc
            (recur (+ i 1) (+ acc (count (str "garbage-" i)))))))
      (println (churn 1600000))
      CLJ
      }
      $ churn | PROTOCLJ_GC_STATS=1 protoclj /dev/stdin
      22888890
      protoclj gc: cycles=0 ... heap=9699328 heap-start=262144     # 710 MB resident
      $ churn | PROTOCLJ_GC_STATS=1 PROTOCORE_HEAP_LIMIT_CELLS=1000000 protoclj /dev/stdin
      22888890
      protoclj gc: cycles=9 reclaimed-total=7772740 ... live-last=2753 heap=1000000

  The live set of that program is two SmallIntegers. Whether a runtime should
  install a default soft heap limit of its own, or whether protoCore should
  start cycles from allocation again, is protoCore's call and the maintainer's:
  it affects five runtimes, so nothing was changed here in either project.

- **Long global names leak memory on every access.** Resolving a global
  whose name is longer than 6 bytes, or non-ASCII, interns it with
  protoCore's `createSymbol`. When the symbol already exists, `createSymbol`
  still builds two permanent copies of the string and drops them: about
  500 bytes per call for a 25-byte name. The fix belongs in protoCore (look
  the symbol up before building the permanent copy). Map keys no longer go
  through `createSymbol`, so this no longer affects string keys.

- **Errors in atom watches are discarded.** A watch function that throws
  (any thrown value: a Clojure exception, a protoCore error, a module's
  error) is skipped and the next watch runs; JVM Clojure propagates the
  exception to the `swap!` / `reset!` caller. Unchanged by the move to
  protoCore's unified exceptions.

- **Errors on actor threads are silent.** An actor message whose handler
  throws (a `StackOverflowError` included) sets the actor's value to `nil`
  and delivers `nil` to the promise `send` returned; the exception is not
  reported. Neither exceptions nor protoCore's unified exceptions changed
  this (a `ProtoException` escaping a handler is caught the same way): a handler can `try` / `catch`
  its own exceptions and keep its state (`26-exceptions/try-inside-actor`),
  but an escaping one is still swallowed. How a failed message should
  surface (JVM Clojure's agents keep the failed state and rethrow on the
  next `send`) is an open design decision.

- **Promise `deref` polls.** A pending promise is checked every millisecond
  (`ProtoThread::sleepFor`, which takes the thread out of the stop-the-world
  quorum so garbage collection can proceed; `cli/waiters-park-for-gc`);
  adequate for hand-off latency, not for sub-millisecond waits. A reader of
  a lazy seq another thread is realising polls the same way.
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
  under `PROTOCORE_HEAP_LIMIT_CELLS=2000000`. This is accepted: a call with
  tens of thousands of arguments is an unusual program shape, and ordinary
  calls pass few arguments, which `ProtoContext::newList` already builds in a
  single cell.
- **`pmap` spawns one OS thread per element**, which is wasteful for large
  collections. (It no longer leaks them: every worker is joined, M39.)
- **A finished future keeps its native thread until the program exits.** A
  future's thread is joined by `deref` while the future is pending and by
  `shutdownFutures` at exit, never once it has finished, so a long-running
  program that creates futures without end keeps one thread stack mapped per
  finished future (the `pmap` leak, fixed in M39, in its future form). Two
  threads that `deref` the same pending future both join its thread, which
  `std::thread` does not allow (found by reading the code, not reproduced).
  Not fixed: the future's thread would need an
  owner that joins it exactly once.
- **Actor `MPMC` throughput** is limited by the global ready-queue mutex.
- **The HTTP server starts one thread per connection, without a cap.** A
  flood of concurrent connections starts as many ProtoThreads; each waits
  at most `:read-timeout` (30 s by default) for its request. Adequate for
  the tools and services protoClojure targets today, not for exposure to
  hostile traffic; a connection limit (answering 503) is the follow-up.
- **I/O handles are not closed by the collector.** A reader, writer or
  socket the program drops without closing keeps its descriptor until the
  process ends (`with-open` scopes one); protoCore has no finalisers.

## History

Shipped changes are listed in [`CHANGELOG.md`](../CHANGELOG.md); the commit
log on `main` records the rationale and measurements for each change.
