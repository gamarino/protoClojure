# Changelog

All notable changes to protoClojure are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The project has no tagged releases yet; the version declared in
`CMakeLists.txt` (and printed by `protoclj --version`) is 0.0.1.

## [Unreleased]

### Added

- **Reader.** Integers, floats (`3.14`, `1e6`), strings with escapes,
  symbols, keywords, `true` / `false` / `nil`, lists, vectors, map literals
  `{...}`, `;` line comments, commas as whitespace, and `@form` as
  `(deref form)`. The remaining reader macros (`'`, `` ` ``, `~`, `^`, `#...`)
  are rejected with a "reserved-for-later token" error.
- **Compiler and bytecode VM.** A single-pass compiler to a 29-opcode stack VM.
  Special forms: `def`, `if`, `do`, `quote` (of symbols, keywords and other
  atoms), `fn`, `defn`, `let`, `loop`, `recur`, `when`, `when-not`, `cond`,
  `and`, `or`, `apply` and `future`.
- **Functions.** Closures with lexical capture across nested `fn` bodies;
  variadic parameters (`& rest`); `apply`; multi-arity `fn` / `defn`; the
  `(fn name [args] ...)` form (the name is not bound inside the body).
- **Numbers.** IEEE-754 floats; SmallInteger fast-path opcodes for
  `+ - * < <= > >= =`; automatic promotion to LargeInteger on overflow and to
  double when an operand is a float.
- **Collections.** Vectors as a type distinct from lists; `list`, `vector`,
  `vec`, `nth`, `first`, `rest`, `cons`, `count`, `empty?`, `reverse`, `map`,
  `filter`, `reduce`; maps with `hash-map`, `assoc`, `get`, `contains?`,
  `keys`, `vals` and `map?`.
- **Named arguments.** Destructuring with `& {:keys [...] :or {...} :as name}`,
  and trailing keyword/value pairs at call sites (`CALL_KW` opcode).
- **Strings.** `string?`, `subs`, `upper-case`, `lower-case`, `starts-with?`,
  `ends-with?`, `includes?`, `index-of`, `replace`, `join`, `split`, `trim`,
  `triml`, `trimr` and `blank?`; `count`, `empty?` and `reverse` accept strings.
- **Atoms.** `atom`, `atom?`, `deref` / `@`, `reset!`, `swap!` and
  `compare-and-set!` on protoCore compare-and-set, with `add-watch` and
  `remove-watch`.
- **Futures and promises.** `future` on OS threads, `future?`, `realized?`,
  `promise`, `deliver`, `promise?`, and `pmap`, which runs one thread per
  element.
- **Actors.** `actor`, `actor?`, `send`, `send-h`, `send-m`, `send-l` and
  `actor-stats` on a worker pool sized by `PROTOCLJ_ACTOR_WORKERS`, with three
  priority bands, a single-method invariant and a lock-free per-actor mailbox.
- **REPL.** Running `protoclj` without arguments starts an interactive REPL on
  libreadline: `user=>` prompt, multi-line input, history in
  `~/.protoclj_history`, `*1` / `*2` / `*3`, and the `:help`, `:quit`,
  `:load` and `:time` commands.
- **Packaging.** CPack configuration: DEB, RPM and TGZ on Linux, DragNDrop on
  macOS, NSIS and ZIP on Windows.
- **Tests.** A glob-discovered conformance suite (274 fixtures under
  `tests/conformance/`), GoogleTest unit tests for the lexer, the reader,
  the bytecode module, the runtime map, value equality and hashing, the
  native stack guard and the double printer (82 tests), and five CLI checks (`--help`, a
  generated program with 70,000 distinct literals of each kind, a
  stack overflow in the REPL, globals bound to nil in the REPL, and
  deeply nested source).
- **Benchmarks and examples.** `benchmarks/bench.sh` (comparison with
  Babashka), `benchmarks/actor-bench.sh` (actor throughput) and twelve
  example scripts under `examples/`.
- **Documentation.** Language reference, architectural design, interop design,
  status tracker, roadmap and a thirteen-chapter tutorial.

### Changed

- Function wrappers use separate single-arity and multi-arity prototypes, and
  the captures attribute is read only when a body has captures, reducing the
  attribute lookups per call.

### Fixed

- Future worker threads are joined before the runtime shuts down, so a script
  that exits without dereferencing its futures no longer crashes at exit.
- A race in the actor drain path that could let two workers process the same
  actor concurrently.
- The actor benchmark harness now checks each script's `:messages-processed`
  output. Earlier actor throughput figures measured a script that failed to
  compile and have been withdrawn.
- Trailing keyword/value arguments to an ordinary callee are no longer packed
  into a map and unpacked again. `(list :b 1 :a 2)` returned `(:a 2 :b 1)`
  (hash order) and `(vector :a 1 :a 2)` returned `[:a 2]` (repeated keyword
  dropped); ordinary callees now receive the arguments unchanged. A
  keyword-argument fn builds its map from the pairs that follow its declared
  positionals, in order, with the last duplicate winning, so keywords passed
  to its positional parameters (`(f :p 1 :x 10)` for `[a b & {:keys [x]}]`)
  no longer raise an arity error.
- Maps keep insertion order at every size. Iteration order used to be the
  ascending order of key hashes, so after protoCore switched string hashing
  to FNV-1a `{:a 1 :b 2 :c 3}` printed as `{:a 1, :c 3, :b 2}` and three
  conformance fixtures failed. `assoc` of an existing key keeps its position,
  `remove-watch` keeps the order of the remaining watches, and `keys`, `vals`,
  printing, `:as` maps, watches and `actor-stats` all follow insertion order.
  All map operations now live in `src/runtime/MapOps.{h,cpp}`.
- `=` compares maps by value. `(= {:a 1 :b 2} {:b 2 :a 1})` returned `false`:
  the `EQ` opcode compared maps by identity, and the `=` primitive (used with
  more than two arguments or as a function value) accepted only numbers. Both
  now share one value equality: maps are equal when they hold the same keys
  mapped to equal values, whatever the insertion order; vectors compare
  element by element; nested maps and vectors compare recursively; a map is
  never equal to a vector or a list. `not=` is added.
- `=` compares lists by value. `(= (list 1 2) (list 1 2))` returned `false`
  and a vector was never equal to a list, because lists compared by
  identity. Lists and vectors are now compared as sequential collections:
  two of them are equal when they hold equal elements in the same order,
  whatever their concrete types, so `(= [1 2] (list 1 2))` and
  `(= [] (list))` are `true`. Nested collections compare recursively, and a
  sequential collection is never equal to a map, a string or `nil`.
- Map keys are hashed by value. A map or a list used as a map key was hashed
  and matched by identity, so `(get {{:a 1} :x} {:a 1})` returned `nil`, and
  an integer key was not found with an equal float although `(= 1 1.0)` is
  `true`. Keys are now hashed with a value hash consistent with `=` and
  matched with `=`: maps hash independently of insertion order, lists and
  vectors share one order-dependent hash, and numbers hash by value modulo
  2^61 − 1 across SmallInteger, LargeInteger and double. Keys that are `=`
  are one key (`(hash-map 1 :a 1.0 :b)` is `{1 :b}`), and maps whose keys
  are collections compare by value.
- A keyword is no longer `=` to the string of its spelling. `(= :a ":a")`
  returned `true`, `(get {:a 1} ":a")` returned `1`, `(hash-map :a 1 ":a" 2)`
  held one entry, `(string? :a)` returned `true`, and a quoted symbol was
  likewise `=` to the string of its name. Keywords and quoted symbols were
  materialised as protoCore symbols, which are strings: protoCore stores a
  short ASCII string inline in the pointer, so `:a` and `":a"` were the same
  pointer, and longer spellings compared equal by content. Keywords and
  symbols are now interned values of their own kind
  (`src/runtime/Named.{h,cpp}`), published with a lock-free compare-and-set
  so every thread obtains the same value; they compare and hash by identity
  and print as their spelling. The compiler interns keyword literals, quoted
  symbols and `:keys` keywords once, so executing them reads a pointer, and
  `=` and map-key hashing decide non-map objects by their pointer tag.
- Keywords and maps are functions. `(:a {:a 1})` failed with "unable to
  resolve symbol: :a" and `({:a 1} :a)` with "value is not callable": the
  compiler looked a keyword in call position up as a global name, and the VM
  called only fns and primitives. `(:k m)`, `(:k m not-found)`, `(m k)` and
  `(m k not-found)` now look the key up as `get` does, returning `nil` (or
  `not-found`) for a missing key and, for a keyword, for a non-map argument.
  Keywords, quoted symbols and maps are function values for locals, globals,
  `apply`, `map`, `filter`, `reduce` and `pmap`.
- `str` renders maps, atoms and the other runtime objects. `(str {:a 1})` and
  `(str (atom 1))` returned `#<unprintable>`, as did a map nested in a vector
  and a map or atom passed to `join`: `str` and `join` used a second printer
  that knew only literals, strings, keywords, lists and vectors, while
  `println` and the REPL used their own. There is now one printer, so
  `(str {:a 1})` is `"{:a 1}"` and `(str (atom 1))` is `"#<atom 1>"`, exactly
  as `println` prints them, and `println` writes each line with one call.
- `str`, `join` and the REPL follow Clojure's printing modes. `(str nil)`
  returned `"nil"` and `(str "a" nil "b")` `"anilb"`; `join` rendered a nil
  element as `nil`; `(str ["a"])` returned `"[a]"`; and the REPL echoed the
  string `"a"` as `a`, indistinguishable from the symbol. The printer had a
  single mode, Clojure's `print`. It now takes a readable flag: `str` and
  `join` insert nil as the empty string and a string argument as is, and
  render every other value readably, so `(str "a" nil ["b" "c\n"])` is
  `a["b" "c\n"]` with the nested strings quoted and escaped; the REPL echoes
  readably; `println` still prints strings bare at every depth. Deviation
  D20 is narrowed to the `#<atom 1>`-style tags of reference types.
- Built-in functions print with their name. `(str println)`, `(println map)`
  and a REPL echo of `+` gave `#<unprintable>`, because the printer
  recognised only user fns. A built-in function now prints as
  `#<fn NAME>` (`#<fn println>`), next to the `#<fn>` of a user fn; the name
  comes from the table that also installs the primitives.
- Integers no longer lose their value silently. A literal beyond 64 bits
  such as `12345678901234567890` read as `9223372036854775807`, because the
  lexer parsed it with a clamping `strtoll`; the `+ - * / inc dec`
  primitives, reached through `apply`, `reduce` or a first-class `+`,
  computed in `long long` and wrapped around (`(apply + (list
  9223372036854775807 1))` was `-9223372036854775808`), failed on
  LargeInteger arguments, and `(apply / (list -9223372036854775808 -1))`
  crashed the process with SIGFPE; `< <= > >=` compared through doubles
  and misordered integers beyond 2^53. Literals beyond 64 bits now read as
  exact LargeIntegers (also under `quote`), the `N` suffix is accepted on
  integer literals, the arithmetic primitives use protoCore's promoting
  integer operations like the arithmetic opcodes, and integer comparisons
  are exact.
- Programs are no longer limited by one-byte bytecode operands. A script
  or function body with more than 256 constants failed with the
  undocumented "const-pool overflow", because every instruction carried a
  one-byte operand and equal number and string literals each took a new
  pool entry; the same one-byte limit stopped a script defining more than
  255 functions, a function with more than 255 locals, a call with more
  than 255 arguments, and an `if`, `cond`, `and`, `or` or `loop` body
  longer than 255 instructions, while a vector literal or a call with more
  than 63 arguments failed at run time with "operand-stack overflow".
  Instructions are now 32-bit words with a 24-bit operand, equal constants
  of the same kind share one pool entry (`1` and `1.0`, `"a"` and `:a` stay
  distinct), and the operand stack grows on demand; a script with 70,000
  distinct literals of each kind compiles and runs. The remaining hard
  limits are documented in `LANGUAGE.md` §17.
- Deep recursion no longer crashes the process. Every non-tail call nests
  the VM on the native stack; a call took 4,512 bytes of it and the depth
  was not checked, so `(defn f [n] (if (= n 0) 0 (+ 1 (f (- n 1)))))`
  crashed with SIGSEGV before `(f 2000)`. A call now takes 1,344 bytes, the
  script driver and the REPL run on a 32 MiB stack, every thread the
  runtime creates (futures, `pmap`, actor workers) gets the same size, and
  the VM checks the remaining stack on every call: a simple self-recursive
  function reaches about 24,700 nested calls, and a deeper recursion
  raises the runtime error `StackOverflowError` on any of those threads.
  Printing, comparing or hashing a collection nested deeper than the stack
  allows raises the same error instead of crashing.
- Arithmetic and comparisons check that their operands are numbers. When
  an operand was not a SmallInteger, the `+ - * < <= > >=` opcodes handed
  it to protoCore unchecked: `(* "ab" 3)` returned `"ababab"`,
  `(+ 1.5 "a")` returned `1.5`, `(< 1 "a")` and `(>= [1] 1)` returned
  `true`, and `(+ 1 nil)` failed with protoCore's "Objects are not integer
  types for addition.". The opcodes and the `+ - * / < <= > >= inc dec`
  primitives now raise one error naming the operation and the offending
  type: `ClassCastException: + expects a number, got nil`.
- Floats print as JVM Clojure prints them. The printer used `%g`, which
  keeps six significant digits: `(/ 1.0 3)` printed `0.333333` and
  `(+ 0.1 0.2)` printed `0.3`, and `1e21` printed `1e+21`. It also tested
  whether a double equalled its `long long` cast, which is undefined
  behaviour for a double beyond the `long long` range and true for `-0.0`,
  so `(- 0.0)` printed `0.0`. Infinities and NaN printed as `inf`, `-inf`
  and `-nan`. A float now prints with the shortest digits that read back as
  the same double (`0.3333333333333333`, `0.30000000000000004`), in plain
  notation from 0.001 up to 10,000,000 and as `1.0E21` or `1.5E-7` outside
  that range, `-0.0` keeps its sign, and infinities and NaN print as
  `##Inf`, `##-Inf` and `##NaN` (`str` of a bare one gives `Infinity`,
  `-Infinity` or `NaN`, as in JVM Clojure). `println`, `str`, `join` and the
  REPL share the one printer.
- Division by a float zero follows IEEE 754. `(/ 1 0.0)`, `(/ 1.0 0)` and
  `(/ 0 0.0)` raised "/: divide by zero"; as in JVM Clojure they now return
  `##Inf`, `##-Inf` (`(/ -1 0.0)`, `(/ 1 -0.0)`) and `##NaN`. Integer
  division by zero, big integers included, still raises an error, now the
  ArithmeticException analogue with JVM Clojure's message:
  `ArithmeticException: Divide by zero`.
- `nth`, `subs` and `index-of` accept index arguments of any size. A big
  integer index failed with protoCore's "LargeInteger value exceeds long
  long range." without naming the function, and an index out of range gave
  "nth: index out of bounds" or "subs: bounds out of range" without the
  index. An index out of range now raises
  `IndexOutOfBoundsException: nth index 12345678901234567890 is out of
  bounds (count 3)` or `StringIndexOutOfBoundsException: subs begin 2,
  end 1, length 3`, `nth` with a not-found value returns it for any
  out-of-range index, and a non-integer index raises
  `ClassCastException: nth expects an integer, got a float`. `index-of`
  with a negative start position returned `nil`; it now searches from the
  beginning, as Java's `String.indexOf` does.
- Symbols and keywords may contain non-ASCII letters. `:ñandú` or
  `(defn año [x] ...)` failed with "unexpected character: �": the lexer
  classified source bytes with `isalnum`, which rejects every byte of a
  multi-byte UTF-8 character, and reported a single broken byte. The lexer
  now decodes UTF-8 and accepts Unicode letters, combining marks and decimal
  digits (Unicode 16.0) in symbols and keywords, which read, print, intern
  and compare like ASCII ones. Any other non-ASCII character is a read error
  naming the whole character and its code point (`→ (U+2192)`), malformed
  UTF-8 is reported as such, `42ñ` is a malformed number like `42x`, and
  error columns count characters instead of bytes.
- `--help` and error messages no longer mention internal development labels
  ("Phase 5", "next milestone", "v0.0.x", "v0.7.x", "v0.13"); each message
  now states the actual restriction, for example "let: not supported at top
  level yet; use it inside a fn". The argument-limit message now states the
  limit the VM enforces (17 bound parameters).
- A global bound to `nil` resolves. `(def x nil)` followed by `x` failed with
  "unable to resolve symbol: x", as did redefining a global to `nil` and, in
  the REPL, reading `*1` before any evaluation: protoCore's `getAttribute`
  returns nil both for an attribute holding nil and for a missing one, and
  the VM treated both as missing. A global lookup that returns nil now
  checks whether the name is defined, so only a name never defined is an
  error; the lookup of a non-nil global is unchanged.
- Errors raised on future and `pmap` threads are no longer lost. A future
  whose body raised an error, a `StackOverflowError` included, realised to
  `nil`, and a failing `pmap` element mapped to `nil`: the worker threads
  caught every error and kept nothing. The error's message is now recorded
  with the future or the element, every `deref` of such a future raises it
  wrapped as JVM Clojure's `ExecutionException`
  (`ExecutionException: ArithmeticException: Divide by zero`), and `pmap`
  raises the error of the first failing element in input order, after
  waiting for every element. Errors in actor message handlers are still not
  reported.
- Deeply nested source no longer crashes the reader or the compiler. Both
  recursed once per level of nesting without checking the stack, so 100,000
  nested lists, or 40,000 nested `fn` forms (which read but did not compile),
  crashed the process with SIGSEGV, in a script and in the REPL. They now
  check the native stack at every level: too deep a nesting is a read error,
  reported where reading stopped, or a compile error, both
  `StackOverflowError: forms nested too deeply for the 32 MiB thread stack`,
  and the REPL keeps evaluating. The REPL also keeps the form it compiles,
  and the forms `:load` reads, in a slot the garbage collector sees; they
  were held only in C++ locals, so a collection could reclaim a form while it
  was being compiled (after a read error on a deeply nested form, the next
  deeply nested form failed with "compile: unsupported form" in 7 of 15
  runs).
- `nth` accepts strings and `nil`. `(nth "abc" 1)` raised "seq op: argument
  is not a list or vector", `(nth nil 0)` raised "nth: nil collection", and
  `nth` on a map or a number raised the same "seq op" error. As in JVM
  Clojure, `(nth nil i)` is now `nil`, or the not-found value, for any integer
  index; `(nth "abc" 1)` is the one-character string `"b"` (deviation D3),
  indexed by code point like `count` and `subs`, with
  `StringIndexOutOfBoundsException: nth index 3 is out of bounds (count 3)`
  past the end; and any other type raises
  `UnsupportedOperationException: nth not supported on a map`.
- The reader accepts `##Inf`, `##-Inf` and `##NaN`. The printer has spelled
  infinities and NaN that way since floats print as in JVM Clojure, but the
  lexer rejected any `#` it did not reserve with
  "reserved-for-later token: #", so printed floats did not read back. They
  now read as the positive and negative infinity and a quiet NaN; as in JVM
  Clojure, whitespace may separate `##` from the symbol, another symbol is
  "Unknown symbolic value: <symbol>" and a number or keyword is
  "Invalid token: ##<text>".
- Comparisons follow IEEE 754 for NaN. protoCore's numeric compare reports
  NaN equal to every number, so `(<= ##NaN 1)`, `(>= ##NaN 1)`, `(= ##NaN 1)`
  and `(= ##NaN ##NaN)` were `true`, and a NaN map key was found with `0`,
  `0.0` or any other NaN. The ordering opcodes and `=` (which map keys share)
  now use protoCore's `partialCompare`: every ordering and `=` with a NaN is
  `false`, `not=` is `true`, and a NaN is `=` only to the identical object,
  as in JVM Clojure. Numbers still compare exactly across integer and float
  representations, and `-0.0` is still `=` to `0.0`.
