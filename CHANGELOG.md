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
- **Tests.** A glob-discovered conformance suite (130 fixtures under
  `tests/conformance/`) and GoogleTest unit tests for the lexer and reader
  (31 tests).
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
