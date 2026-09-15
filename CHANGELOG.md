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
- **Tests.** A glob-discovered conformance suite (165 fixtures under
  `tests/conformance/`) and GoogleTest unit tests for the lexer, the reader,
  the runtime map and value equality (46 tests).
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
- Programs no longer hang when a garbage-collection cycle starts while a
  thread is blocked. protoCore now starts cycles from allocation, and a cycle
  waits until every running thread parks; a thread blocked in a wait never
  parks. Idle actor workers waiting for work, `deref` of a pending future,
  `pmap` joining its workers, the shutdown joins of futures and actor workers,
  and the REPL waiting for input now run in protoCore unmanaged regions.
- A `loop` or a recursion that allocates nothing no longer holds up a
  garbage-collection cycle requested by another thread. The VM polls the
  protoCore safepoint on every `recur` back-edge and on function entry; a
  loop that waited for the thread requesting the cycle used to deadlock.
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
  never equal to a vector or a list. `not=` is added. A map used as a map key
  is still matched by identity (map hashing is not implemented).
- `=` compares lists by value. `(= (list 1 2) (list 1 2))` returned `false`
  and a vector was never equal to a list, because lists compared by
  identity. Lists and vectors are now compared as sequential collections:
  two of them are equal when they hold equal elements in the same order,
  whatever their concrete types, so `(= [1 2] (list 1 2))` and
  `(= [] (list))` are `true`. Nested collections compare recursively, and a
  sequential collection is never equal to a map, a string or `nil`.
- `--help` and error messages no longer mention internal development labels
  ("Phase 5", "next milestone", "v0.0.x", "v0.7.x", "v0.13"); each message
  now states the actual restriction, for example "let: not supported at top
  level yet; use it inside a fn". The argument-limit message now states the
  limit the VM enforces (17 bound parameters).
