# Changelog

All notable changes to protoClojure are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The version declared in `CMakeLists.txt` (and printed by `protoclj --version`)
is 0.0.2, the first tag: a preview with a Linux `.deb`. v0.1.0 remains the
first planned release (`docs/ROADMAP.md`).

## [Unreleased]

### Changed — lists as cells, direct seq primitives (2026-10-24)

- A Clojure list is a chain of immutable list cells (first, rest, count)
  ending in the empty list, one value (DESIGN.md M44): `first`, `rest`,
  `count` and `conj` onto a list are O(1) and allocate one cell; `nth` on a
  list is O(n), as in Clojure. The reader yields these lists and the
  compiler, macroexpansion, syntax-quote, destructuring and `eval` read them
  as they are; a `ProtoList` is never a Clojure value any more (the printer
  shows a leaked one as `#<internal list>`).
- `(cons x coll)` answers a cons for any coll but nil, as in Clojure, so
  `(list? (cons 1 '(2)))` is false; the rest parameter of a variadic fn is
  a list; `count` stops at a counted rest. `coll?` and `sequential?` added.
- The VM answers `inc`, `dec`, two-argument `+`, `first`, `rest`, `next`,
  `seq`, `empty?`, `count`, `nth`, `cons` and `conj` on nil, lists, vectors,
  cons cells and chunks without a call context (M43); anything else takes
  the general call.
- Measured against `main` (n = 10^6, ns per element): a list walk with
  `rest` 1304 -> 288 (Babashka 175), `reduce` over a list 161 -> 85,
  cons-build then count 993 -> 432, `empty?` + `first` 331 -> 195, sequential
  `nth` on a vector 389 -> 292. Slower: start-up +3.9 % cycles, `(vec list)`
  O(n log n) where it was O(1), four protoCore cells per list element (D60).
  Report: `benchmarks/reports/2026-10-24-seq-cost/`.
- Tests: `cli/list-walk-under-heap-limit` (a list reclaimed cell by cell as
  it is walked, two-directional), conformance `list-cells`,
  `long-list-iterative`, `direct-primitive-calls`.
### Added — VS Code extension tests (2026-10-25)

- **VS Code extension 0.4.1: integration tests, and two launch fixes they
  found.** `editor-integration/vscode/test/` runs the extension in a real VS
  Code Extension Development Host (`npm install && npm test`; the extension
  README's "Testing the extension"): a local launch with a breakpoint, stack,
  a local and stdout; F5 without a launch.json; the executable lookup; a
  remote launch through WSL with pathMappings; the refusal without them; the
  `raised` exception filter; the launch schema and snippets. CI runs the
  local ones on Linux under xvfb. Fixed: a refused launch (no `program`, a
  bad `remote`, `remote` without `pathMappings`) waited for the user to close
  the error notification, so the launch stayed pending as long as it was not
  dismissed; it now ends at once. A `protoclj.path` naming a file that does
  not exist is refused with a message naming the setting, instead of a bare
  spawn error.

### Fixed — GC checks on protoCore 3.2.0; CI on protoCore 3.2.0 (2026-10-24)

- `cli/loop-garbage-is-reclaimed`, `cli/blocking-joins-park-for-gc` and
  `cli/waiters-park-for-gc` failed their premise (too few collection cycles)
  on protoCore 3.2.0 although the collector works: the same loop allocates
  1.5-1.9 cells per iteration there against 3.7-3.9 on 3.1.0 (a heap
  string is its own rope root), so the fixed workloads made less than half
  the garbage. Per cycle the collector reclaims the same (360-440 K cells
  under the same ceilings), and the old kernel passes all three. The checks
  now measure cells per iteration at run time (tests/cli/gc-workload.sh) and
  scale their iteration counts to the garbage they were designed for; their
  thresholds and assertions are unchanged.
- `docs/snippets` on Windows: Git for Windows' POSIX tools (`tr`, `ls`) are
  on PATH for the test (ENVIRONMENT_MODIFICATION, as protoST does), and a
  snippet that fetches a URL whose server answers 5xx (example.com's
  occasional 503) is skipped as a transient network failure, with the
  server's answer.
- CI builds against protoCore 3.2.0 (master bb2d581a, untagged) on Linux,
  macOS and Windows; the Windows floor job stays on 3.1.0, the declared
  minimum.

### Added — debugger: sources, path mappings, remote launch (2026-10-24)

- clojure.core's Clojure part, embedded in the binary, is compiled with debug
  information under `--dap` as library code: its fns can be stepped into
  (`justMyCode: false`; the default `true` keeps steps, pause and exception
  stops in the program) and take breakpoints. Frames, breakpoints and
  Loaded Scripts name it by `sourceReference`; the `source` request serves
  its text. `loadedSources` / `loadedSource` events.
- `launch` takes `pathMappings` (`[{localRoot, remoteRoot}]`), translated by
  the adapter both ways (longest prefix, either separator, Windows roots
  without case); an unmapped file comes back as a `sourceReference` served
  by the adapter. Also `cwd`.
- VS Code extension 0.4.0: `remote: {command, path}` runs the adapter through
  ssh / wsl / docker (`<command...> <path> --dap`), snippets "Launch over
  SSH" and "Launch in WSL", `extensionKind: ["workspace"]`.
- docs/debugging.md: remote debugging, sources without a file, JetBrains IDEs
  through LSP4IJ.

### Added — debugger: conditions, hit counts and log points (2026-10-24)

- Exception filters take a condition (`supportsExceptionFilterOptions`): a
  Clojure form evaluated in the throwing frame with the exception bound to
  `ex`; a condition that throws stops and reports its error.
- Line breakpoints: `condition`, `hitCondition` (`N`, `== N`, `>= N`, `> N`,
  `<= N`, `< N`, `% N`; anything else is unverified) and `logMessage`
  (`{form}` interpolation, `{{`/`}}` literal braces, `<error: ...>` for a
  form that fails). Conditions and log forms run on the hitting thread with
  its breakpoints and exception stops off. The VS Code extension is 0.3.0.

### Added — debugger: exception breakpoints and threads (2026-10-24)

- Exception breakpoints: *All Exceptions* (`raised`) and *Uncaught
  Exceptions* (`userUnhandled`), with `exceptionInfo`. The thread stops
  before the exception propagates — at a `throw`, an error the runtime
  raises for the script, or a protoCore error such as a division by zero —
  in the script's innermost frame. The Uncaught filter tests the exception
  against the `catch` clauses of every active `try` of the thread, through
  clojure.core's native calls back into the script. Rethrows and the
  runtime's own probes never stop. docs/debugging.md.
- Threads: each thread that runs the script (`main`, `future@file:line`,
  `pmap@file:line`, `actor worker N`, `http@file:line`) is a thread of its
  own in the client, with `thread` started/exited events. A stop stops only
  its thread; continue, step and pause act on the thread they name; several
  threads can be stopped and inspected at once, and the collector runs
  while they are.
- `cli/dap-session` covers both.

### Added — debugging in VS Code (2026-10-24)

- `protoclj --dap`: a Debug Adapter Protocol server over stdin/stdout. Line
  breakpoints, continue / step in / step over / step out / pause, the call
  stack (fn names, `fn@file:line`, `<top level>`), Locals of the selected
  frame by name (parameters, captures, `let` / `loop` / `catch` bindings;
  collections expand), evaluate in the selected frame (Watch, Debug Console,
  hover), stopOnEntry; the script's stdout and stderr go to the Debug
  Console. docs/debugging.md.
- The reader records where list forms start, and the compiler emits line
  marks (`Op::LINE`), locals' scopes and function names, only for code run
  under `--dap`; other code is compiled as before.
- `editor-integration/vscode/`: the `protoclj` debug type for VS Code's own
  `clojure` language (no grammar of its own; coexists with Calva), F5
  without a launch.json; `examples/.vscode/launch.json`.
- ctest `cli/dap-session` drives the adapter as VS Code does.

### Changed — CI

- ci.yml and cross-platform.yml pin protoCore 3.1.0 (master 34eca79f; not
  tagged yet), the minimum CMakeLists.txt declares since lazy sequences.

### Fixed — the macros-branch regression (2026-10-24)

- Start-up: the reader and the compiler interned and spelled the same
  symbols over and over while loading `core.clj`. Per-thread caches keyed
  by the run (`NamedLayout::epoch`) now serve `internNamed`, `internSymbol`
  and `namedSpellingText`: an empty file 67.9 -> 64.1 M cycles (baseline
  before macros 47.3 M; the rest is `core.clj` itself, STATUS.md "Known
  issues").
- Lazy steps over a list (`map`, `filter`, `take`, `for`, ...) read it
  through a view instead of `removeFirst`, which allocated O(log n) per
  element, and open one context fewer per element: sum-squares run phase
  +36.5 % -> +12.3 % against the baseline.
- `get`, `contains?` test for a map before a set; `isSet` is inline:
  map-loop +8.7 % -> +4.5 %.
- Measured causes, the bisect and what remains (fib +4.4 %, tak +1.7 %,
  mostly dispatch-loop layout, not isolated):
  `benchmarks/reports/2026-10-24-regression/`.

### Added — the showcase against Babashka (macros brief, Part B stage 13)

- `examples/showcase/`: a grep over standard input, a JSON transform and
  word frequencies that run unchanged under protoclj and Babashka, each
  printing a result that checks its work; `(factorial 1000)` with `*'`
  (both runtimes) and with `*` (bb overflows, protoclj promotes); a
  deterministic input generator. `benchmarks/reports/2026-10-23-showcase/`
  holds the runner and the raw results; README "Showcase" the numbers.
- The vocabulary the scripts and the tutorial needed (DESIGN.md M41, D59):
  `+'` `-'` `*'` `inc'` `dec'`, `bigint`, `biginteger`, `mapv`, `filterv`,
  `number?`, `integer?`, `int?`, `float?`, `double?`, `==`, `hash`, `peek`,
  `pop`, and a vector called as a function of its index.
- `docs/tutorial/15-scripting.md`: standard input, regular expressions,
  `ns` / `require`, JSON, EDN, lazy sequences, and what a Babashka user
  meets.
- `tests/docs/check_snippets.py` (ctest `docs/snippets`, M42) runs every
  Clojure snippet of the README, LANGUAGE.md and the tutorial; 157 run, and
  each of the 65 that cannot says why. Snippets that claimed to run and did
  not were fixed.

### Added — JSON and EDN (macros brief, Part B stage 12)

- **`read-string`** (the first form of a string, read as code is read,
  syntax-quote included) and **`clojure.edn/read-string`** (data only: the
  code reader macros are errors, no evaluation, no tagged literals), on the
  reader itself (DESIGN.md §10.2, M35; D56). `pr-str` and `read-string`
  round-trip every printable value.
- **JSON**: `cheshire.core` (`parse-string` with `true` or a key fn,
  `parse-stream`, `generate-string` with `:pretty`, `:escape-non-ascii` and
  `:key-fn`, `generate-stream`, `decode`, `encode`) and `clojure.data.json`
  (`read-str` / `read` with `:key-fn`, `:value-fn`, `:eof-error?`,
  `:eof-value`; `write-str` / `write` with `:escape-unicode`, `:escape-slash`,
  `:escape-js-separators`, `:key-fn`), thin names over one C++ parser and
  writer (M36): exact big integers, `\u` escapes and surrogate pairs, errors
  with the line and column. Streams are read a line at a time, so
  newline-delimited JSON is read value by value (M38). The namespaces work
  with `require` / `ns` aliases (M37). Deviations: D57.
- The reader: the string escapes `\uXXXX` (a surrogate pair is one
  character; an unpaired one is a read error, D55), octal `\0`..`\377`,
  `\b` and `\f`; `#_` discards the next form; `#tag` is Clojure's `No
  reader function for tag` error (D13).
- **`parse-long`**, **`parse-double`**, **`parse-boolean`** (Clojure 1.11;
  M40, D58).
- New tests: 40 fixtures in `tests/conformance/39-data-formats/` (the
  cheshire ones also pass under Babashka 1.4.192), the CLI checks
  `cli/repl-data-formats`, `cli/json-under-heap-limit` (a 20,000-record
  document under a 600,000-cell ceiling, 200,000 streamed records under a
  300,000-cell one, and a control that must run out of memory) and
  `cli/pmap-joins-workers`, and three lexer unit tests.

### Fixed — pmap threads, strings holding NUL

- **`pmap` leaked a native thread per element.** It joined only the workers
  still running when it reached them; a finished worker kept its stack
  mapped until the process exited (21 calls over 64 elements: about 40 GB of
  virtual size; ThreadSanitizer reported the threads as leaked). Every worker
  is joined now, and each element holds its thread as an object until then
  (M39). The same leak remains for finished futures (STATUS.md, known
  issues).
- **Strings holding a NUL byte were cut at it** by the reader, by string
  constants and by most string primitives (`str`, `pr-str`, `subs`,
  `upper-case`, ...): `(count (str "a\u0000b"))` was 1. They now build their
  result from its whole length.

### Fixed — waits that no longer touch native thread APIs; a future race

- **No native thread API in the runtime's waits.** A reader of a lazy seq
  another thread is realising, `deref` of a promise and the
  `PROTOCLJ_GC_STATS=1` sampler waited with `std::this_thread::yield` /
  `sleep_for` (the first two inside a `goUnmanaged` bracket; the sampler was
  a `std::thread`). They now use protoCore 3.1.0's `ProtoThread::yield` /
  `sleepFor`, which take the thread out of the stop-the-world quorum for the
  wait, and the sampler is a ProtoThread. New CLI check
  `cli/waiters-park-for-gc`: a lazy seq claimed by a future, and a promise
  delivered by one, each read by the main thread while the future needs
  collections under a 500,000-cell ceiling; with a bare native yield in
  `waitABit` the lazy-seq case is killed by its timeout.
- **A future copied its spawner's call context while the spawner changed
  it.** The worker read the spawning thread's thread-local
  `ActiveCallContext` through a pointer, while the spawner went on running
  and rewrote it (reported by ThreadSanitizer on
  `37-lazy-seqs/futures-share-a-seq`). `buildFuture` now hands the worker a
  copy it owns. `pmap` keeps the pointer: its spawner joins every worker
  before its context can change.
- A thread that claimed a lazy seq and finds it published by someone else
  when it publishes returns the published value, so every reader sees one.

### Added — the everyday vocabulary of a script (macros brief, Part C)

- **`#(...)`** with `%`, `%1`..`%20` and `%&`, read as Clojure reads it,
  `(fn* [p1__N#] ...)` (DESIGN.md §10.2, M32); a nested `#()` is a read error.
- **`ns`** with `(:require [clojure.string :as str] [clojure.set :as set])`
  and a top-level **`require`**: the aliases of clojure.core, clojure.string,
  clojure.set, clojure.java.io and clojure.java.shell resolve (`str/join`,
  `io/reader`, `sh`), `:refer` is accepted, and `No such var: str/foo` /
  `FileNotFoundException` report what does not exist (M33, D52).
- **`letfn`**, mutually recursive local functions (M34, D53).
- C++ built-ins: `quot`, `rem`, `mod` (exact for integers of any size),
  `compare`, `max`, `min`, `sort` and `sort-by` (stable, with a comparator or
  a boolean predicate), `frequencies`, `clojure.string/split-lines`, and a
  `System` object for `(System/exit n)`, `System/getenv`,
  `System/currentTimeMillis` and `System/nanoTime` (D54).
- In `core.clj`: `key`, `val`, `constantly`, `complement`, `partial`, `comp`,
  `juxt`, `fnil`, `some`, `every?`, `not-any?`, `not-every?`, `max-key`,
  `min-key`, `get-in`, `assoc-in`, `update`, `update-in`, `merge`,
  `merge-with`, `select-keys`, `zipmap`, `group-by`. They add about 1.2 ms
  to start-up (`core.clj` costs about 5 ms in all).
- `assoc` takes `nil` (the empty map) and a vector (by index), as in Clojure.
- `examples/15-top-words.clj`, the tutorial's twenty-line script.
- New tests: 22 fixtures in `tests/conformance/38-vocabulary/` and
  `37-lazy-seqs/tutorial-ch05-lazy.clj`, and the CLI check
  `cli/script-top-words`.

### Added — scripting vocabulary, stage 11: lazy sequences

- **Lazy sequences** (DESIGN.md §7 and §10.2, M28-M31; LANGUAGE.md §10):
  `lazy-seq` and `lazy-cat`; lazy `map` (over several collections too),
  `filter`, `remove`, `take`, `take-while`, `drop`, `drop-while`,
  `iterate`, `repeat`, `repeatedly`, `cycle`, `range` (with no end too),
  `concat`, `mapcat`, `partition`, `partition-all`, `interleave`,
  `distinct`, `for`, `line-seq` and `re-seq`; `doall`, `dorun` (with an
  optional count), `run!`, `realized?` of a lazy seq and `chunked-seq?`.
  Chunked as in Clojure: a vector's seq and a finite `range` are realised
  32 elements at a time by `map`, `filter` and `for`. Each element is
  computed once, also when futures share the seq.
- **`*in*`**: a reader on the standard input; `(line-seq *in*)` reads it a
  line at a time, in constant memory (closes D29).
- **Requires protoCore 3.1.0** (protoCore branch `feature/lazy-object`): a
  lazy seq is a protoCore lazy object, `ProtoObject::newLazyChild`. As a
  mutable object, a realised chain of lazy seqs was freed one link per
  collection cycle (M28).
- **Locals clearing**: the last read of a local that is read once becomes
  the new opcode `PUSH_LOCAL_CLEAR`, and the VM clears an argument's slot
  once a call has taken it, so a walk does not hold the head of a lazy seq
  it was handed (M30; D50 for the conservative rule).
- New tests: 31 fixtures in `tests/conformance/37-lazy-seqs/` and
  `27-io/tutorial-ch14-line-seq-lazy.clj`, and the CLI checks
  `cli/lazy-seqs-survive-gc`, `cli/lazy-head-retention`,
  `cli/lazy-grep-under-heap-limit` (500,000 lines under a 300,000-cell
  ceiling) and `cli/repl-lazy-seqs`.

### Changed — stage 11 (behaviour changes against stage 10)

- **`map` and `filter` are lazy**: a `(map f coll)` whose result is never
  read no longer calls `f`, as in Clojure (`25-actors/many-actors.clj` and
  `concurrent-sends-no-race.clj` now `doall` it).
- **`line-seq` is lazy**: read after its reader closed, it raises
  `IOException: Stream closed`, as in Clojure; `doall` it inside
  `with-open` (the `27-io` fixtures that returned it do so now).
- `seq`, `rest` and `next` of a vector answer its chunked seq, not a list:
  `(list? (rest [1 2]))` is false, as in Clojure. `seq?` is true for every
  seq, not only for lists.
- `(range)` with no end works (`36-format-for/range-infinite.clj` became
  `range-endless.clj`); multi-collection `map` works
  (`09-higher-order/map-multi-coll-error.clj` became `map-multi-coll.clj`).
- `for`'s expansion passes `true` as a third argument of the innermost
  `__for-step` (`for-macroexpand.clj`).
- The REPL echoes a definition as its var, `#'user/x`, as Clojure's REPL
  does (M31, D51), instead of printing the value (which realised a lazy seq).
- `(str lazy-seq)` is `clojure.lang.LazySeq@...`, as in JVM Clojure (D48).
- `cli/for-format-survive-gc` runs 4,000 rounds instead of 1,500: lazy
  `for` allocates less, and the REPL run saw fewer than three cycles.

### Fixed — keyword `get` / `assoc` speed

- The 7 % slowdown stage 10 brought to keyword-keyed map reads and `assoc`
  is gone (benchmarks/RESULTS.md, 2026-10-23): the map key semantics find
  a keyword or a symbol in a cache of interned named values before asking
  whether the key is a set.

### Added — scripting vocabulary, stage 10: `format`, `printf`, `for`, `doseq` modifiers

- **`format`** and **`printf`** (DESIGN.md §10.2, M27; D47): Java's
  `String.format` conversions `%s %d %x %o %f %e %g %b %c %n %%` with
  argument indexes, flags, width and precision; `%f` / `%e` round half up
  as Java; errors carry Java's exception names.
- **`for`** with `:let`, `:when` and `:while`, and the same modifiers for
  **`doseq`** (in `core.clj`). `for` answers a list built eagerly through one
  hook, `__for-step`, which stage 11 makes lazy (D46).
- A finite, eager **`range`** (`core.clj`); `(range)` with no end is an
  error until lazy sequences exist.
- **`apply` with leading arguments**, `(apply f x y coll)`.

### Changed — stage 10

- `09-higher-order/apply-variadic-error` became `apply-leading-args` (the
  form now works) and `apply-arity-error`.
- New tests: 17 fixtures in `tests/conformance/36-format-for/`, the CLI check
  `cli/for-format-survive-gc`, and `for` / `format` / `printf` / `doseq`
  lines in `cli/repl-scripting`.

### Added — scripting vocabulary, stage 9: regular expressions

- **Regular expressions** (DESIGN.md §10.2, M25; D45): the `#"..."` literal,
  compiled when read; `re-pattern`, `re-find` (also on a matcher),
  `re-matches`, `re-seq`, `re-matcher`, `re-groups`; `replace` and the new
  `replace-first` with a pattern and a string (`$1`) or fn replacement;
  `split` with a pattern and an optional limit, answering a vector. On
  `std::regex` ECMAScript, with Java's leading `(?i)` `(?m)` `(?s)`, named
  groups, `\Q...\E` and `\A` / `\z` translated. Patterns without
  back-references run on libstdc++'s breadth-first executor, so a long
  subject cannot overflow the native stack; one with back-references raises
  `StackOverflowError` before a match that could.
- New exception class `PatternSyntaxException` (under
  `IllegalArgumentException`).
- `clojure.string/x` and `clojure.set/x` name the built-ins, with no
  `require` (M26).
- `identical?`.

### Changed — stage 9

- `#"x"` used to be the read error `reserved-for-later token: #"`.
- New tests: 16 fixtures in `tests/conformance/35-regex/`, unit tests
  `Regex.*` (5) and `Lexer.HashQuoteIsARegexLiteral`, the CLI check
  `cli/regex-survives-gc`, regex lines in `cli/repl-scripting`;
  `cli/shebang-scripts` now provokes its read error with `#"("`.

### Added — scripting vocabulary, stage 8: sets

- **Sets** (DESIGN.md §10.2, M24): the `#{...}` literal (a repeated element
  is the read error `Duplicate key: x`), `hash-set`, `set`, `set?`, `disj`,
  and `conj`, `contains?`, `get`, `count`, `empty?`, `seq` and every
  sequence function on sets; a set is a function of its elements and a
  keyword looks itself up in a set. A set is an immutable object holding a
  `ProtoMap` from each element to itself, matched as map keys are; sets are
  map keys and set elements by value. `clojure.set`'s `union`,
  `intersection`, `difference`, `subset?` and `superset?` are globals (D28).
- `into` (in `core.clj`).
- `()` evaluates to the empty list (it was the compile error `empty list ()
  is not supported yet`).

### Changed — stage 8

- `first`, `rest`, `cons`, `map`, `filter`, `reduce`, `vec`, `reverse` and
  `apply` accept maps (entries as `[k v]`), sets and strings, as `seq`
  already did; another value raises `IllegalArgumentException: Don't know
  how to create ISeq from: ...` (it was `seq op: argument is not a list or
  vector`).
- D24 and D25 cover sets; D28 covers `clojure.set`.
- New tests: 13 fixtures in `tests/conformance/34-sets/` and
  `08-collections/empty-list-literal`, unit tests `Lexer.HashBraceOpensASet`,
  `ReaderFixture.SetLiteral` and `MapOpsFixture.SetsAreValueKeys`, the CLI
  check `cli/sets-survive-gc`, and set lines in `cli/repl-scripting`.

### Added — scripting vocabulary, stage 7: destructuring

- **Destructuring** (DESIGN.md §10.2, M23) in `let`, `loop`, the
  parameters of `fn`, `defn` and `defmacro`, and the `core.clj` binding
  macros (`if-let`, `when-let`, `if-some`, `when-some`, `doseq`):
  `[a b & r :as all]`, `{x :x :keys [...] :strs [...] :syms [...] :or {...}
  :as m}`, `:ns/keys`, nested forms. The compiler rewrites a binding form
  into the bindings `clojure.core/destructure` produces. `:or` applies to
  missing keys only, except in the keyword-argument form `& {:keys ...}`,
  which keeps D16. A seq of key/value pairs destructures as their map
  (`[& {:as opts}]`). New internal built-in `__destructure-map`.
- Errors with Clojure's messages: `Unsupported binding form: :a`,
  `Unsupported binding form, only :as can follow & parameter`, `No value
  supplied for key: :b`. A parameter that is a keyword or a number used to
  be `fn/defn: parameter name must be a symbol`.
- New tests: 23 fixtures in `tests/conformance/33-destructuring/` (the
  README, LANGUAGE §5.4 and tutorial chapter 5 snippets among them), the
  CLI checks `cli/destructuring-survives-gc` (compile-time rewrites and
  run-time destructuring under a 1,000,000-cell ceiling, as a file and at
  the REPL) and `cli/repl-scripting` (destructuring typed at the REPL).

### Added — macros, stage 6: the `clojure.core` macros in `core.clj`

- **`core.clj`** (`src/core/core.clj`, DESIGN.md §10.1, M20): the part of
  `clojure.core` written in Clojure, embedded in the binary at build time
  (`cmake/EmbedFile.cmake`) and loaded by every driver before the program,
  in the namespace `clojure.core`. It costs about 3.4 ms of CPU (12 M cycles) at start-up
  (benchmarks/RESULTS.md); run-time benchmarks did not move.
- **Macros** (M22): `when`, `when-not`, `if-not`, `cond`, `comment`,
  `while`, `if-let`, `when-let`, `if-some`, `when-some`, `->`, `->>`,
  `as->`, `some->`, `some->>`, `cond->`, `cond->>`, `doto`, `dotimes`,
  `doseq`, `case`, `condp`, `with-open`, `future`, with Clojure's
  expansions and binding-vector checks. **Functions**: `identity`, `some?`,
  `true?`, `false?`, `zero?`, `pos?`, `neg?`, `nnext`, `ffirst`, `nthnext`,
  `butlast`; and the C++ built-ins `even?` and `odd?`.
- New deviation D44 (`case` tests its constants in order).

### Changed — macros, stage 6

- **`when`, `when-not`, `cond`, `with-open` and `future` moved from the C++
  compiler to `core.clj`** (M21): `macroexpand` now shows their expansions.
  They compile to the same instructions as before; the compiler now folds
  an `if` whose test is a literal (`cond`'s `:else`). Their errors come from
  the macros (`Syntax error macroexpanding with-open:
  IllegalArgumentException: with-open requires an even number of forms in
  binding vector`), and `cond` no longer accepts the bare symbol `else` as
  its default (Clojure does not; `:else` works). `and` and `or` stay in C++:
  as macros they measured 5-8 % slower on a loop.
- New tests: 21 fixtures in `tests/conformance/32-core-macros/` (every
  macro, `case` without a match, expansion errors, a program's own `when`),
  and the CLI check `cli/core-embedded` (a copy of the binary in an empty
  directory has the core macros, in a file and at the REPL).

### Added — macros, stage 5: hygienic syntax-quote

- **Syntax-quote qualifies symbols** as Clojure's reader does (DESIGN.md
  §10.1, M17): a built-in becomes `clojure.core/x`, any other name `user/x`
  (the program's namespace; there are only these two, D42); special forms,
  `&`, symbols with a namespace or a `.` and exception class names stay as
  they are. Closes D35.
- **`foo#` auto-gensyms** (`foo__N__auto__`, the same symbol throughout one
  syntax-quote) and **`gensym`** (M18); the lexer reads `#` inside a symbol.
- **`user/x` and `clojure.core/x` name globals, never locals** (M19). Every
  built-in is also installed under `clojure.core/x`, which a `def` of `x`
  does not replace, so an expansion keeps calling the built-in after the
  program redefines it. Closes D37.
- A qualified name as a parameter or a `let` / `loop` binding is a compile
  error with Clojure's message (`Can't use qualified name as parameter:
  user/x`, `Can't let qualified name: user/v`).
- New built-ins: `gensym`, `symbol`, `keyword`, `name`, `list*`.
- New deviations D42 (two namespaces) and D43 (symbols are never
  collected).

### Changed — macros, stage 5

- The printed expansion of a template now shows qualified symbols:
  `` `(f ~x) `` is `(user/f 1)` where it was `(f 1)`.
- New tests: 17 fixtures in `tests/conformance/31-syntax-quote/` (auto-gensym,
  `gensym`, `my-or` hygiene, built-ins not captured, `~'it` capture, error
  cases), a lexer unit test, two new REPL checks in `cli/repl-macros`; six
  fixtures in `28-quote/` and `cli/repl-quote` now expect qualified symbols.

### Added — macros, stage 4: `defmacro`, expansion, `macroexpand`, `eval`

- **`defmacro`** (DESIGN.md §10.1, M10-M11): `(defmacro name doc? attr-map?
  [params] body)` and the multi-arity form. The compiler calls the macro on
  the unevaluated arguments, plus `&form` and `&env`, when it compiles a
  call, and compiles the expansion in its place, recursively. A macro is in
  effect for every top-level form compiled after the one that defined it,
  in files and at the REPL; a redefinition changes only the forms compiled
  afterwards. A macro is a fn carrying a flag, stored in the globals, so a
  `def` of the name replaces it. A local shadows a macro; special forms
  cannot be macros.
- **`macroexpand-1`, `macroexpand` and `eval`** (M14). `eval` compiles and
  runs a form as a top-level form; a form it cannot compile raises
  `RuntimeException` (D40).
- **A top-level `do` is evaluated one form at a time**, after the
  top-level form has been expanded (M12), as in Clojure, so a macro may
  expand into `(do (defmacro m ...) (m))`.
- Errors (M15): a macro that raises is the compile error `Syntax error
  macroexpanding m: <exception>`; a wrong argument count is `Wrong number
  of args (n) passed to macro: m` (D39); `Can't take value of a macro: m`;
  a macro that always expands into itself ends in `StackOverflowError`.
- New built-ins: `second`, `next`, `last`, `conj`, `seq?`, `pr-str`, `prn`,
  `pr`, `print`.
- `defn` accepts a docstring and an attribute map (both dropped).
- `let` and `loop` work at the top level of a file and at the REPL.
- New deviations D38-D41 (STATUS.md).

### Changed — macros, stage 4

- **`let` and `loop` bindings are scoped to their body** (a bug fix that
  changes results): a binding used to stay visible for the rest of the
  enclosing function, so `(let [x 1] (let [x 2] x) x)` answered 2 instead
  of 1, and a later reference to a global of the same name read the stale
  local.
- **A variadic fn called with no rest arguments binds the rest parameter
  to `nil`**, as in Clojure; it used to be `()`. `(defn f [& xs] xs) (f)`
  is now `nil`.
- **The REPL keeps going after a compile or runtime error** in a line with
  several forms: the next form runs, as in Clojure's REPL (stage 3 dropped
  the rest of the line). A read error still drops it. Results printed
  before an error now appear before the error message.
- The drivers (script, REPL, `:load`) share one evaluator
  (`src/compiler/Eval.h`); a script keeps its call context installed for
  the whole run, which macros need at compile time.
- New tests: 24 fixtures in `tests/conformance/30-macros/` (the classic
  `unless`, a `with-` macro, a threading macro and a recursive macro among
  them, and error cases), `02-special-forms/let-top-level.clj` (replacing
  `let-top-level-error.clj`), the CLI checks `cli/repl-macros` and
  `cli/macro-expansion-survives-gc` (fails when an expansion is left
  unrooted, checked by removing the root).

### Changed — macros, stage 3: scripts run form by form

- **A script is read, compiled and run one top-level form at a time**, as
  the REPL does and as Clojure's `load` does (DESIGN.md §10.1, M9): a form
  is compiled only after the previous one has run, which is what lets a
  `defmacro` apply to the forms after it (stage 4). The forms before a
  failing one have run and printed (before, a compile or read error
  anywhere in the file stopped the script before anything ran); the exit
  status is still 1.
- **A compile error names where its top-level form starts**:
  `script.clj:3:1: compile error: ...` (it used to name only the file).
  Read and runtime errors keep their formats.
- Each form is read into a context of its own and is garbage once it has
  run, in scripts, at the REPL and under `:load`; it used to stay alive
  until the end of the run (the REPL's for the whole session).
- **The REPL evaluates every form of an input line**, in order, printing
  each result; it used to evaluate the first and silently drop the rest.
  `:load` runs its file form by form through the same loop and reports
  errors with the file's positions.
- New tests: CLI checks `cli/file-form-by-form` (output before a compile,
  read or runtime error, error positions, exit statuses) and
  `cli/repl-forms-in-order` (several forms per line, an error mid-line,
  `:load`), and the fixtures `29-loading/compile-error-names-its-form.clj`
  and `29-loading/later-form-sees-earlier-definition.clj`.
  `cli/quoted-data-survive-gc` now fails, in both modes, when a module does
  not pin its quoted data (checked by disabling the pin).

### Added — macros, stage 2: quote and syntax-quote

- **`quote` takes any datum** (DESIGN.md §10.1, M5): `'(+ 1 2)` is a list
  value that `first`, `rest`, `count` and `nth` take apart, `'[a {:k b}]` a
  vector holding a map, and a quoted symbol a symbol value (`symbol?`). A
  quoted collection is the very value the reader yielded, pinned by the
  compiled module (a `Datum` constant in a `ProtoRootSet` of the module's
  own), so every evaluation answers the same object.
- **The reader macros `'`, `` ` ``, `~` and `~@`** (M6). Syntax-quote is
  expanded at read time as Clojure's reader expands it:
  `` `(f ~x ~@ys) `` reads as `(clojure.core/seq (clojure.core/concat
  (clojure.core/list (quote f)) (clojure.core/list x) ys))`; vector and map
  templates apply `clojure.core/vector` and `clojure.core/hash-map`.
  Symbols are not namespace-qualified and `foo#` is not an auto-gensym yet
  (D35, macros stage 5).
- **`clojure.core/x` names the built-in `x`**, never a local (M7, D37), so
  an expansion is not captured by `(let [list ...] ...)`. `@x` now reads as
  `(clojure.core/deref x)`, as in Clojure, so it calls the built-in even
  where a local is named `deref`.
- `~` or `~@` outside a syntax-quote is the compile error `unquote (~) used
  outside a syntax-quote` (M8, D36); `` `~@x `` is the read error
  `splice (~@) not in list`; a reader macro at the end of input is
  `EOF while reading: ' must be followed by a form`.
- New built-ins: `seq` and `concat` (eager, answering lists; the expansion
  needs them), `symbol?`, `keyword?`. `apply` spreads a vector or `nil` as
  well as a list.
- The REPL waits for more input when a line ends in a reader macro (`'`,
  `` ` ``, `~`, `~@`, `@`), as Clojure's REPL does.
- New tests (29): 23 conformance fixtures in `tests/conformance/28-quote/`
  (among them the tutorial and README snippets that now run), 4 reader and
  lexer unit tests (quote / unquote / splice lists, the syntax-quote
  expansions, nested syntax-quote, read errors, the new tokens), and the CLI
  checks `cli/repl-quote` and `cli/quoted-data-survive-gc` (file and REPL
  mode under a 1,000,000-cell heap).

### Changed — macros, stage 1: the reader yields runtime values

- **The reader produces the values the runtime manipulates** (DESIGN.md
  §10.1, decision M1): a symbol or a keyword is its interned named value, a
  string a protoCore string, `[...]` the runtime vector, `{...}` the runtime
  map, `nil` / `true` / `false` the values themselves. The compiler consumes
  those values; the reader-only marker objects (`stringMarkerProto`,
  `vectorMarkerProto`, `mapMarkerProto`) are gone. The value representation
  (`Named`, `VectorOps`, `MapOps`) moved into the `protoclj_support` library
  so the reader still does not depend on the runtime. This is the base for
  quote and macros: code is data of the ordinary kinds.
- **A map literal that repeats a key is a read error**, `Duplicate key: :a`,
  as in Clojure (decision M2). It used to keep the last value. The odd-forms
  message is Clojure's: `Map literal must contain an even number of forms`.
- A map literal's forms are still evaluated in source order (decision M3):
  the reader records each literal's key order in a side table
  (`src/reader/SourceInfo.h`) because the map itself has none.
- Keywords and symbols are interned when read rather than when compiled, so
  a map keyed by keywords may print its entries in another (still
  unspecified, D24) order than 0.0.3 did for the same program.
- New tests: 8 reader unit tests (string, keyword, `nil`/`true`/`false`,
  vector and map values, a repeated key, an odd map, `@`, the recorded key
  order), the conformance fixtures
  `16-maps/literal-evaluates-in-source-order.clj` and
  `16-maps/literal-duplicate-key-is-read-error.clj`, and the CLI checks
  `cli/repl-literals` and `cli/reader-values-under-heap-limit` (a
  20000-entry map literal and 300 literal-heavy forms read and compiled
  under a 1,000,000-cell heap).

## [0.0.3] - 2026-10-22

**Requires protoCore 3.0.0** (SOVERSION 4), the declared minimum for a build from source; the `.deb` depends on `protocore (>= 3.0.0)`. CI pins protoCore tag `v3.0.0` on every job.

### Changed — protoCore 3.0.0: the unified exception model, 2026-10-21

- **Every throw is a `proto::ProtoException`.** `ClojureThrow` now derives
  from protoCore's `ProtoException` (no longer from `std::runtime_error`),
  protoCore pins the thrown value while it is in flight, and the runtime's
  own root set `protoclj-exceptions-in-flight` is gone. The VM's handler
  catches `const proto::ProtoException&` first and selects by the value's
  prototype chain; a `std::exception` (a module defect, a
  `StackOverflowError`) is still translated into a classed exception there.
- **Exception classes are protoCore prototypes.** Each of the 23 built-in
  classes is a prototype whose parent is its superclass, and an exception
  is a child of its class; the class id is inherited, so the test of a
  built-in class stays an integer walk.
- **A module's error types are caught by name.** A `catch` naming no
  built-in class names a type resolved at run time, when an exception
  reaches the clause — a local, a var or a module member
  (`(catch geometry/NegativeRadius e ...)`) — and matched by protoCore's
  `isInstanceOf` (D34). New opcodes `RETHROW`, `EXC_MATCH_TYPE`,
  `RESOLVE_CLASS`, `GET_MEMBER`, `CALL_MEMBER`, `STORE_CAUGHT`. `Object` is accepted as a
  catch class and, like `Throwable`, catches every value; `Exception`
  catches a module's error too. `throw` accepts a caught module error.
  `ex-message` reads a module error's `message` attribute.
- **A rethrow continues the original exception** (protoCore rule E3): a
  `try` whose clauses do not match, and the end of a `finally`, rethrow the
  very C++ exception the frame caught (`STORE_CAUGHT` keeps it per handler
  slot as a `std::exception_ptr`; `RETHROW` calls `std::rethrow_exception`),
  so another runtime's `ProtoException` subclass keeps its type and extras
  across Clojure code (`tests/unit/test_rethrow.cpp`).
- **`(import-module "logical/path")`** loads a module through Unified Module
  Discovery; `alias/member` reads a member and `(alias/member args...)`
  calls it with the module as its receiver (`INTEROP.md` §0). A C++
  exception escaping such a call becomes a `RuntimeException` carrying its
  `what()`.
- **protoCore's errors arrive as Clojure classes** through the error factory
  installed at start-up: division by zero and overflow as
  `ArithmeticException` (`"Divide by zero"`), a type mismatch as
  `ClassCastException`, a malformed integer literal as
  `NumberFormatException`, another malformed argument as
  `IllegalArgumentException`, an arity mismatch as `ArityException`
  (`DESIGN.md` §4.1). Integer `/` leaves the zero divisor to protoCore.
  This fixes the 16 tests that protoCore 3.0.0 broke (big-integer reads and
  arithmetic terminated the process: protoCore's overflow is no longer a
  `std::overflow_error`).
- **Primitives raise `ProtoException`s at the raise site** (`throwClassed`,
  or `throwError` for the older `"Class: message"` texts) instead of
  `std::runtime_error`s classified when they reach a `try`; the VM's errors
  likewise.
- **Top level.** The script driver, the REPL and the HTTP server report an
  uncaught `ProtoException` by its value (a module error by its `name` and
  `message`), and stop any other C++ exception with a message.
- **Behaviour change:** `(try 1 (catch NoSuchException e 2))` now compiles
  and answers 1; `Unable to resolve classname: NoSuchException` is raised,
  as a `RuntimeException`, when an exception reaches the clause (D34).
- **Test build `protoclj_geometry`** (developer protoCore tree only) links
  protoCore's example geometry module and runs
  `tests/modules/geometry-exceptions.clj`.

### Changed — protoCore 3.0.0: primitives read their arguments in place, 2026-10-20

- **Built against protoCore 3.0.0** (SOVERSION 4), now the declared minimum:
  every C++ primitive receives its arguments as protoCore's
  `ProtoArguments` / `ProtoKeywordArguments` descriptors (protoCore
  `docs/CALLING_CONVENTION.md`) instead of a `ProtoList`.
- **A call to a primitive no longer packs its arguments.**  The `CALL`
  opcode hands the primitive a descriptor over the arguments where they
  already are, in the frame's operand stack; `invoke` (a primitive calling a
  function back, as `map` and `reduce` do) copies them into the call scope's
  slots.  Before, each call built a fresh `ProtoList` of its arguments.  A
  loop calling `str` and `count` once per iteration allocates 4.0 cells per
  iteration instead of 6.0; `cli/loop-garbage-is-reclaimed` scales its bar
  to the garbage the workload now makes.
- `(list ...)` and `(vector ...)` build their list once from the arguments
  (`asList`); `mapAssocPairs` lost its list overload.
- **A frame's context is built at its initial size** (arguments, locals and
  the 64-slot initial operand stack) instead of empty and then grown with
  `resizeAutomaticLocals`; likewise `invoke`'s call scope.  `fib` -5 %
  cycles from this, -12.7 % with protoCore 3.0.0's integer paths.
- **A frame's operand stack has the capacity its body needs**
  (`BytecodeModule::maxStack`, a flow analysis of the finished code: the
  deepest point along every path, joins keeping the larger depth) instead of
  a fixed 64 slots; a push no longer tests the capacity, and `apply` reserves
  room for the list it spreads.  Measured: instructions -1 % to -6 %, cycles
  within -2.7 % to +4.6 % of before (`fib` +1.4 %, `sum-loop` +4.6 %, `tak`
  -2.2 %, `churn` -2.7 %) -- no net gain; filling 64 slots was cheap.

## [0.0.2] - 2026-10-17

A preview tag of everything below. The `.deb` is built in a clean Ubuntu 24.04
container with GCC 13 against protoCore 2.14.6 and declares
`protocore (>= 2.14.6)`.

### Added — `#!` comments: scripts run as commands, 2026-10-17

- **`#!` is a line comment**, as in Clojure (LispReader's `CommentReader`):
  the lexer skips from `#!` to the end of the line wherever it appears, and
  the REPL's completeness check does too. A script whose first line is
  `#!/usr/bin/env protoclj` and that is executable runs as a command and
  chains in pipelines. Before, such a script failed with `read error:
  reserved-for-later token: #`. Line numbers in read errors are unchanged.
- **Examples:** `examples/13-filter-grep.clj` (lines that contain a
  pattern, grep's exit statuses) and `examples/14-filter-count.clj` (a line
  count), executable; the install keeps their permissions
  (`USE_SOURCE_PERMISSIONS`).
- **Tests:** a lexer unit test, the conformance fixture
  `01-literals/hash-bang-comment.clj`, and the CLI check
  `cli/shebang-scripts`, which runs the two examples as commands through
  `env` in real pipelines (results, exit statuses 1 and 2, an early
  `head`) and checks that a read error after the line names its real line.
- **Docs:** LANGUAGE.md §2.1, §2.8 and §18.2 ("Scripts as commands"),
  tutorial §14.3, TUTORIAL.md, README, STATUS.md.

### Changed — protoCore 2.14.5 in CI, 2026-10-11

- **CI builds protoCore 2.14.5** (tag `v2.14.5`, `063c6d5`) on Linux, macOS
  and Windows instead of 2.14.2: the controller's out-of-memory rule uses the
  live set and the collector's registries outlive static destruction
  (2.14.4); an embedder exits LeakSanitizer-clean (2.14.5). The Windows floor
  job stays on 2.7.0. No source change was needed; the suite passes 525/525
  locally against 2.14.5 (ldd-verified).

### Changed — protoCore's adaptive heap controller by default; protoCore 2.14.2 in CI, 2026-10-10

- **protoclj runs protoCore's adaptive heap controller.** `main` calls
  `ProtoSpace::enableAdaptiveHeap` on the space of the REPL and of a script
  run (`applyHeapPolicy`, guarded by `PROTOCORE_HAS_ADAPTIVE_HEAP`, which
  protoCore defines from 2.10.0). Before, protoClojure set no heap limit:
  protoCore never started a collection by itself and a program grew its heap
  with every allocation (STATUS.md, Known issues, now marked resolved;
  protoCore's re-measurement saw `coll_alloc` with six tasks killed at
  16 GB). The heap budget is 75 % of the smaller of physical memory and the
  process memory limit (cgroup on Linux, job object on Windows); a soft limit
  inside it grows only while the program waits for the collector; **memory
  is not returned to the operating system**. `PROTOCORE_HEAP_LIMIT_CELLS`
  sets the budget, `PROTOCORE_ADAPTIVE_HEAP=0` turns the controller off (the
  budget becomes a fixed hard limit). README, "Memory". Approved by the
  maintainer (protoCore's final re-measurement, section 7: "adopt").
  - **The floor stays at 2.7.0.** Built against a protoCore older than 2.10.0
    protoclj has no controller and keeps the old behaviour; CMake reports
    `adaptive heap controller: OFF` and does not register the three tests
    below (the Windows floor job builds 2.7.0).
  - Tests: `cli/adaptive-heap-on-by-default` (the controller's per-cycle
    trace appears with nothing in the environment),
    `cli/adaptive-heap-off-by-env` (only fixed-limit cycles with
    `PROTOCORE_ADAPTIVE_HEAP=0`) and `cli/adaptive-heap-bounded-memory` (a
    1 M-cell budget, 60000 rounds -- about 40 times the budget in garbage --,
    at least 10 cycles, peak RSS at most 160 MB; measured 72 MB), driven by
    `tests/cli/adaptive_heap_check.py`, which measures the child's peak RSS on
    Linux, macOS and Windows and needs Python 3 (not registered without it).
    The first and third fail against the previous build (no controller line;
    with the variable set, fixed-limit cycles); the second passes there too,
    because `PROTOCORE_HEAP_LIMIT_CELLS` alone already applied a fixed limit,
    so it guards the switch, not the adoption.
  - Measured (DEV12 notebook, synthetic; before and after builds of the same
    tree against protoCore 2.14.2; medians of 3, interleaved; wall s / peak
    RSS MB): `coll_alloc` one task (1500 rounds) 13.73 / 8404 -> 11.81 / 142
    (-14 %, -98 %); `adaptive-heap-garbage.clj` 60000 rounds 6.32 / 2643 ->
    4.15 / 144 (-34 %, -95 %); `fib`, `tak`, `sum-loop`, `reduce-list`,
    `sum-squares`, `factorial-100` and `actor-saturation-32` run no cycle
    and are within -8 to +5 %. No benchmark regressed by more than 10 %.
- **CI builds protoCore 2.14.2** (tag `v2.14.2`, `17bc118`) on Linux, macOS
  and Windows instead of 2.14.1 (the collector adjustments, including the
  controller's live-set floor). The Windows floor job stays on 2.7.0 and
  protoIO on 0.2.2. The full suite passes locally against 2.14.2 (525/525,
  ldd-verified), and CI is green on the branch: Linux, macOS and Windows 525
  of 525, the 2.7.0 floor job 522 of 522 (runs 38104010751, 38104012172).

### Changed — protoCore 2.14.1 in CI, 2026-10-04

- **CI builds protoCore 2.14.1** (tag `v2.14.1`, `9495155`) on Linux, macOS
  and Windows instead of 2.10.2: the collector's pacing and multi-cursor
  sweep (2.12.0), its control law (2.13.0), the sweep's helper threads
  (2.14.0) and the thread-exit quorum fix (2.14.1). The Windows floor job
  stays on 2.7.0 (protoClojure does not use `setAttributes`: its state
  changes go through atoms, vars and persistent collections) and protoIO on
  0.2.2. No source change was needed: the full suite passes locally against
  2.14.1 (522/522, ldd-verified), and CI is green on Linux, macOS and Windows
  (522 of 522 on each; runs 37169479198, 37169480523). protoClojure does not
  enable protoCore's adaptive heap.

### Changed — protoCore 2.10.2 in CI, 2026-10-03

- **CI builds protoCore 2.10.2** (tag `v2.10.2`, `b7f6d82`) on Linux, macOS
  and Windows instead of 2.9.4; the Windows floor job stays on 2.7.0 and
  protoIO on 0.2.2. No source change was needed: the full suite passes locally
  against 2.10.2 (522/522). protoClojure does not enable protoCore's adaptive
  heap.

### Changed — protoCore 2.9.4 in CI, 2026-10-02

- **CI builds protoCore 2.9.4** (tag `v2.9.4`, `9cb0ef5`) on Linux, macOS and
  Windows for every push and pull request; it built 2.7.0 on Linux and 2.8.0
  on macOS and Windows. The declared minimum, 2.7.0, is tested by one job
  only, the Windows floor job of `cross-platform.yml`; Linux no longer
  builds it. protoIO stays pinned to 0.2.2 (`1611ff9`).

### Changed — Windows-port review, 2026-10-02

- **`sh` sets `:dir` and `:env` itself** (protoIO 0.2.2 `RunOptions`), with no
  `/bin/sh -c 'cd ...'` or `env -i` on Linux and macOS and no Git for Windows
  `sh`/`env` on Windows, where a stock system has neither. The program is
  searched in protoclj's own `PATH`, as the JVM does; deviation D32 now covers
  only `:in-enc` / `:out-enc`. Test: `cli/io-sh-dir-env-native`.
- **Servers started without a host listen on IPv4 and IPv6** (one dual-stack
  socket, protoIO 0.2.2). They bound 0.0.0.0 only, so on Windows each
  `localhost` client (which tries `::1` first) was refused there before it
  reached 127.0.0.1. Test: `cli/http-localhost-latency`.
- **The evaluator runs on a protoCore thread** (`ProtoSpace::newThread`)
  instead of a `pthread_create` / `_beginthreadex` thread; `main` owns the
  `ProtoSpace`. `configureThreadStacks` asks protoCore for 32 MiB thread
  stacks (`ProtoSpace::setThreadStackBytes`, honoured on every platform from
  protoCore 2.9.0). `stop-server` waits for the accept thread on a condition
  variable instead of sleeping. The unit test that a protoCore thread gets
  32 MiB now runs on Windows too (it was skipped).
- **Windows packages are self-contained**: protoCore's DLL (from the
  imported target, whatever its file name), the OpenSSL DLLs of the version
  found (configure fails if they are missing), OpenSSL's licence and the
  MSVC runtime are installed next to `protoclj.exe`; the NSIS generator is
  enabled only when `makensis` is found. CI runs the unpacked ZIP and the
  silently installed installer with only the system directories on `PATH`.
- **MSVC builds protoClojure's sources at `/W4`** (warning-free; `/WX` in CI,
  `-Werror` on Linux and macOS, through `PROTOCLJ_WARNINGS_AS_ERRORS`).
- **Requires protoIO 0.2.2.** On Windows that also brings: `.bat` / `.cmd`
  targets refused by `sh`, crash exit codes as 128 + signal, open files
  deletable and renamable, and TLS verification against the Windows
  certificate stores.
- CI tests protoCore 2.7.0, the declared minimum, on Windows as well as on
  Linux (since the entry above, on Windows only). All 522 tests pass on Linux, macOS and Windows, none skipped.

### Added — Windows (MSVC), 2026-10-01

- **Native Windows build.** protoClojure builds with Visual Studio 2022 against
  an installed protoCore and the sibling protoIO; `protoclj` runs scripts and
  the REPL natively and installs with `cmake --install` (or `cpack -G ZIP`).
  In CI (`windows-2022`) all 520 tests of that day passed but one, skipped
  (the default-thread-stack test; it runs on Windows since 2026-10-02). A
  first run by hand on Windows 11 had counted 16 failures in the Git Bash
  harness (Git's `mktemp` answers `/tmp` paths, `python3` was the Store
  stub), none in the runtime; the harness and the fixtures were fixed before
  CI ran. Every Windows difference is behind `_WIN32` / `_MSC_VER` / `WIN32`;
  on Linux the suite passes 520/520 as before, with the same compile and link
  lines. See docs/INSTALLATION.md, "Windows (MSVC)".
- The runtime spells protoCore's 64-bit integers `proto::proto_long` /
  `proto::proto_ulong` (`long` / `unsigned long` outside Windows, where `long`
  is 32 bits).
- `.gitattributes` keeps `*.clj` and `*.sh` LF on every platform.

### Fixed

- **`recur` read its target from freed memory when an argument held a fn.**
  The compiler kept a reference into its scope stack while compiling the
  arguments; `(recur (+ i 1) (cons (future ...) acc))` pushes a nested scope,
  which can reallocate that stack. glibc left the old block intact, so Linux
  never showed it; on Windows it crashed the compiler. The target is copied.
- **A StackOverflowError could crash the process under MSVC.** A frame with
  `try` handlers re-threw an exception it does not handle with `throw;` inside
  its catch clause; it now re-throws with `std::rethrow_exception` after it.
  The two are equivalent under the Itanium ABI (Linux, macOS); under MSVC a
  catch clause runs before the stack below it is released.

- **protoCore floor raised to 2.6.1.** The HTTP server creates a thread per connection from its accept thread; before 2.6.1, protoCore's `newThread` could detach a live context's roots when called from another thread. The DEB now depends on `protocore (>= 2.6.1)`.

### Added

- **Reader.** Integers, floats (`3.14`, `1e6`), strings with escapes,
  symbols, keywords, `true` / `false` / `nil`, lists, vectors, map literals
  `{...}`, `;` line comments, commas as whitespace, and `@form` as
  `(deref form)`. The remaining reader macros (`'`, `` ` ``, `~`, `^`, `#...`)
  are rejected with a "reserved-for-later token" error.
- **Compiler and bytecode VM.** A single-pass compiler to a 33-opcode stack VM.
  Special forms: `def`, `if`, `do`, `quote` (of symbols, keywords and other
  atoms), `fn`, `defn`, `let`, `loop`, `recur`, `when`, `when-not`, `cond`,
  `and`, `or`, `apply`, `future`, `try` and `throw`.
- **Functions.** Closures with lexical capture across nested `fn` bodies;
  variadic parameters (`& rest`); `apply`; multi-arity `fn` / `defn`; the
  `(fn name [args] ...)` form (the name is not bound inside the body).
- **Numbers.** IEEE-754 floats; SmallInteger fast-path opcodes for
  `+ - * < <= > >= =`; automatic promotion to LargeInteger on overflow and to
  double when an operand is a float.
- **Collections.** Vectors as a type distinct from lists; `list`, `vector`,
  `vec`, `nth`, `first`, `rest`, `cons`, `count`, `empty?`, `reverse`, `map`,
  `filter`, `reduce`; maps with `hash-map`, `assoc`, `dissoc`, `get`,
  `contains?`, `keys`, `vals` and `map?`; `count` and `empty?` accept maps.
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
- **Exceptions.** `try` / `catch` / `finally` and `throw` as special forms,
  and `ex-info`, `ex-data`, `ex-message` and `ex-cause`. Catch clauses are
  tried in order and match subclasses; `finally` runs on the normal path,
  after a handled exception and when a catch clause throws; `recur` across a
  `try` is the compile error `Cannot recur across try`. A `catch` names one
  of 23 built-in classes mirroring the Java hierarchy (`Exception`,
  `RuntimeException`, `clojure.lang.ExceptionInfo`, `ArithmeticException`,
  `IOException` with `FileNotFoundException` and the `java.net` classes,
  `ExecutionException`, `Error`, `StackOverflowError`, …), by simple or
  qualified name. Errors raised by the runtime are exceptions of the class
  their message names (`(/ 1 0)` is an `ArithmeticException` with message
  `"Divide by zero"`), and any other runtime error a `RuntimeException`
  (D26); a stack overflow is a catchable `StackOverflowError`. `deref` of a
  failed future (and `pmap`) raises an `ExecutionException` whose `ex-cause`
  is the original exception. Exceptions print as a one-line `#error` map
  (D27). An uncaught exception keeps the `runtime error: <class>: <message>`
  report and exit status 1. Four opcodes (`TRY_BEGIN`, `TRY_END`, `THROW`,
  `EXC_MATCH`) over a per-frame handler stack; only a body that contains a
  `try` runs in a frame with a C++ catch region, which keeps `fib`, `tak`
  and `sum-loop` at cycle parity with the VM before exceptions (the region
  in every frame cost `fib` +3.5 % cycles). An exception in flight is pinned
  in a protoCore `ProtoRootSet` while the frames it unwinds hand their young
  generations to the collector; the unit test
  `InFlightExceptionSurvivesCollections` fails without the pin. C++ code
  raises classed exceptions with
  `throwClassed(ctx, className, message, data, cause)` (`DESIGN.md` §4.1).
- **Input and output**, on the shared protoIO library (linked statically;
  the DEB gains only `libssl3t64`), as globals named as in Clojure
  (`LANGUAGE.md` §18, `DESIGN.md` §4.2): `slurp` (files, readers, sockets and
  `http(s)` URLs), `spit` with `:append`, `reader`, `writer`, `with-open`
  (compiled as Clojure's macro expands it, closing in reverse order on
  return and on exception), `read-line` (standard input, readers, sockets),
  `write`, `close`, `line-seq` (eager, D29), `file`, `file-seq`, `exists?`,
  `directory?`, `delete-file`, `make-parents` and `copy`; `sh` with `:in`,
  `:dir` and `:env`, answering `{:exit :out :err}`; `getenv`, `exit`, and
  `*command-line-args*` bound to the arguments after the script's path; an
  HTTP client in babashka http-client's shape (`http-get`, `http-post`,
  `http-put`, `http-delete`, `http-head`, `http-request`; an exceptional
  status throws ex-info carrying the response unless `:throw false`); a
  Ring server (`run-server`, `stop-server`, `server-port`) that refuses
  malformed and oversized requests with 400 / 414 / 431 / 413 and answers
  a plain 500 for a failing handler or a response header with a line
  break, each connection on a ProtoThread of its own rather than on the
  actor pool; and sockets (`tcp-connect` with `:tls` and `:timeout`,
  `tcp-listen`, `tcp-accept`, `socket-read-line`, `socket-write`,
  `socket-close`, `udp-socket`, `udp-send`, `udp-receive`, D30). Every
  blocking call runs outside the collector's quorum. Failures are
  `FileNotFoundException`, `IOException`, `ConnectException`,
  `SocketTimeoutException`, `UnknownHostException`, `SocketException` or
  `IllegalArgumentException` with ex-data `{:type :file-not-found :errno 2}`
  and the like. Deviations D28-D33 (`STATUS.md`). 69 conformance fixtures in
  `tests/conformance/27-io` and four CLI checks.
- **REPL.** Running `protoclj` without arguments starts an interactive REPL on
  libreadline: `user=>` prompt, multi-line input, history in
  `~/.protoclj_history`, `*1` / `*2` / `*3`, and the `:help`, `:quit`,
  `:load` and `:time` commands.
- **Packaging.** CPack configuration: DEB, RPM and TGZ on Linux, DragNDrop on
  macOS, NSIS and ZIP on Windows.
- **Tests.** A glob-discovered conformance suite (407 fixtures under
  `tests/conformance/`), GoogleTest unit tests for the lexer, the reader,
  the bytecode module, the runtime map, value equality and hashing, the
  native stack guard, the double printer and exception values (98 tests),
  and fifteen CLI checks (`--help`, a generated program with 70,000 distinct
  literals of each kind, the native bulk builders under a heap ceiling, the
  garbage a `loop` makes under a heap ceiling, joins that park for the
  collector, actor payloads under a heap ceiling, a stack overflow in the
  REPL, globals bound to nil in the REPL, deeply nested source, an uncaught
  exception, exceptions under a heap ceiling, `read-line` in pipelines,
  `*command-line-args*`, exit statuses, and an HTTP server under a heap
  ceiling).
- **Benchmarks and examples.** `benchmarks/bench.sh` (comparison with
  Babashka), `benchmarks/actor-bench.sh` (actor throughput) and twelve
  example scripts under `examples/`.
- **Documentation.** Language reference, architectural design, interop design,
  status tracker, roadmap and a fourteen-chapter tutorial.
- **Garbage-collection census (`PROTOCLJ_GC_STATS=1`).** Prints one line to
  standard error when a script ends: cycles completed, cells reclaimed
  (total, per-cycle maximum and mean), the live set of the last cycle, and
  the heap's high-water mark against its starting size. protoCore publishes
  `reclaimedLastCycle` per cycle and overwrites it, so a run that reclaimed
  millions of cells and one that reclaimed none can end with the same value;
  the census samples the counters as they are published, which is what makes
  "the collector is working" a measurement rather than an assumption. Inert
  without the variable, and it starts no thread.

### Fixed

- **A `loop`'s garbage is reclaimed while the loop runs.** protoCore chains
  every cell a context allocates onto that context's young generation, and its
  root scan records the chain head as a root and marks the whole chain — so
  until a context submits its chain, every cell it ever allocated is live *by
  definition*. A context submits in exactly two places: when it is destroyed,
  and from `ProtoContext::safepoint()` past
  `ProtoSpace::maxAllocatedCellsPerContext`. The VM builds one context per
  frame, so a call that returns shows the collector its garbage; a `loop` is
  ONE frame, and the VM called `safepoint()` nowhere at all. Measured on a
  1,600,000-iteration loop that allocates one string per iteration and keeps
  two SmallIntegers: zero collection cycles, the heap grown from 262,144 to
  9,699,328 cells, 710 MB resident; and under
  `PROTOCORE_HEAP_LIMIT_CELLS=1000000` the same program aborted with
  protoCore's "live set 996519 cells, last cycle reclaimed 0 — out of memory",
  so cycles did run and every one of them reclaimed exactly nothing. The VM
  now polls a garbage-collection safepoint at the loop back-edge
  (`Op::JUMP_BACK`) and at frame entry, the two points where it holds no
  half-built value in a C++ local — every live value is in a frame slot, which
  *is* the frame context's traced `automaticLocals`. The same program now
  completes inside the ceiling: 9 cycles, 7,772,740 cells reclaimed, a live
  set of 2,753 cells, 77 MB resident. `PROTOCLJ_NO_GC_SAFEPOINT=1` disables
  the hook, and `tests/cli/loop-garbage-is-reclaimed.sh` fails without it —
  two-directionally, since it also requires the disabled run to abort.

  The safepoint is polled on a **stride of 64 events per thread**, not on
  every event: `ProtoContext::safepoint()` is a call across protoCore's
  shared-library boundary that reads five fields and takes
  `std::this_thread::get_id()` before it can decide to do nothing, and a
  protoClojure loop body can be five opcodes long. One call per back-edge cost
  +9.4 % instructions and +17.0 % cycles on a 20,000,000-iteration loop that
  does nothing but add (`perf stat -r 3`, against the same source with both
  call sites removed); the stride brings that to +0.75 % instructions with
  cycles at or below the unhooked build, and +0.67 % instructions on an
  allocation-free `fib 30`. Neither of the safepoint's jobs needs a poll per
  event — submission is threshold-gated at 10,000 cells inside protoCore, and
  parking needs bounded latency rather than none — and 64 is the stride
  protoCore's own `allocCell` uses for its stop-the-world poll.

  An earlier attempt at this hook (`1bcc433`) was reverted in `dde94c8`
  because it "only served the allocation-budget GC trigger being reverted in
  protoCore". That reading covered one of the safepoint's three effects. The
  young-generation submission has happened *only* inside
  `ProtoContext::safepoint()` since protoCore `b5fa8d31` moved it out of
  `allocCell`, and the stop-the-world park is independent of any trigger;
  neither depends on the reverted allocation budget.

  `dde94c8` also removed the fixture `22-futures/spin-loop-allows-gc.clj`, and
  it is **not** restored, because its premise no longer holds: its program was
  re-run with the hook disabled and it completed, twice, rather than
  deadlocking. A protoClojure loop that looks allocation-free is not one — the
  `@stop` in `(loop [i 0] (if (= @stop 0) (recur (+ i 1)) :released))` is a
  primitive call, every primitive call builds a child context, and that
  context allocates, so the loop already parks at protoCore's own every-64
  allocation poll. The stop-the-world park is a real effect of this hook and
  the reason frame entry is one of its two sites, but it is asserted by no
  test here, for the same reason protoST could not close its S3. Writing a
  fixture for it would need a loop shape whose body provably allocates
  nothing, and no such shape exists in the language today.

### Changed

- **Vectors are protoCore `ProtoList`s, not interned `ProtoTuple`s**
  (decision R2 below). A vector is now the `ProtoList` of its elements inside
  a one-entry `ProtoSparseList` box — the box is what carries a pointer tag of
  its own, so `vector?`, `list?`, `conj` and printing still tell a vector from
  a list in O(1), and it costs one cell per vector. protoCore interns every
  `ProtoTuple` node and frees none, so **every vector a program ever built
  used to stay in memory until the process exited**; vectors are now ordinary
  garbage, which `tests/unit/test_value_lifetime.cpp` fails to observe if the
  interning comes back. Building a vector from a call's arguments and `vec` of
  a list became O(1), and coercing a vector to a sequence — which `map`,
  `filter`, `reduce`, `first` and `rest` all do — became O(1) instead of an
  O(N) tuple-to-list conversion. Indexed access pays for it: a `ProtoList`
  node holds one element where a `ProtoTuple` node holds four, so `nth` takes
  about twice the node hops. Two equal vectors are no longer one pointer, so
  `=` walks their elements like every other sequential comparison. Measured on
  a settled machine (`benchmarks/RESULTS.md` records the conditions): a build
  plus one full `nth` scan of a 200,000-element vector takes 0.47-0.49 s and
  296 MB where the interned tuple took 0.76-0.81 s and 545 MB, and an
  access-only probe (one build, 50 full `nth` scans of a 20,000-element
  vector) is indistinguishable at 0.61-0.69 s against 0.59-0.73 s. So the
  extra node hops are real but below the interpreter's own per-iteration cost
  at these sizes, while the interner's cost is not: the new representation is
  faster and half the memory on everything measured.

- **Actor mailboxes are protoCore `ProtoMPSCQueue`s** (PMQ-SPEC §6 step 4):
  three per actor, one per priority band, in place of the three
  `std::atomic<ActorMessage*>` stacks of C++ heap nodes. A message is now a
  three-element `ProtoList` — the function, its arguments, the promise — and
  the three queues hang off the actor's wrapper object, which is mutable and
  therefore a GC root. **This closes a GC-safety defect**: the old heap nodes
  held protoCore pointers that nothing rooted, so a collection between a send
  and its handler could free a payload nothing else referenced.
  `tests/cli/actor-payloads-survive-gc.sh` is the fixture with that premise —
  it segfaults when the one line that roots the queues is removed. Sends
  allocate and free their message in a scope of their own, and a worker runs
  each actor turn in a context of its own, so neither side pins a message in
  a long-lived young generation. Cost, measured on the 1,000,000-message
  single-actor benchmark: about 29% throughput and about 50% more resident
  memory, against a mailbox whose payloads the collector could not see.

- **Maps are protoCore `ProtoMap`s** (protoCore 2.1.0), read and written
  through protoCore's shared hashed-collection helper (`hashedPut` /
  `hashedGet` / `hashedRemove` / `hashedForEach`) with one Clojure
  `KeySemantics`. The interned canonical-key layer is gone: a key is now
  stored as the object the caller passed, so **map keys are ordinary garbage**
  and deviation D23 (every distinct key ever used stays in memory until the
  program exits) is withdrawn. Key semantics are unchanged — strings by
  content, a list and a vector with equal elements as one key, maps by their
  entries, doubles by bit pattern, big integers by value, keywords, symbols
  and other objects by identity, numbers of different types apart (D15) —
  and `keyHash` agrees with `keyEquals` at every depth. Iteration order stays
  unspecified, as Clojure requires; it is now the ascending order of a slot
  word (an address for a key matched by identity, a hash for every other),
  where it used to be the address of the canonical key. `count` of a map
  becomes **O(n)** (new deviation D25): two keys whose hashes collide in
  their low 54 bits share a slot, so the slot count is a lower bound, not the
  count; `empty?` stays O(1). Which slot kind a key lands in is invisible
  through the language, so the new tests for it are white box
  (`tests/unit/test_mapops.cpp`), and `tests/unit/test_mapops_gc.cpp` fails
  if collection keys are retained.

- **Built against protoCore 2.0.0** (soname `libprotoCore.so.2`). No source
  change was needed: protoClojure calls none of the APIs whose semantics
  changed — a repo-wide search finds no `setParents`, `addParent`,
  `hasParent`, `isInstanceOf` or `getParents` call site at all — and every
  `newChild` receiver is `ProtoSpace::objectPrototype` or an immutable marker
  prototype that is itself a child of it, so no parent chain in protoClojure
  is ever mutated after construction and 2.0.0's chain flattening and
  current-chain capture are both no-ops here. The ABI bump means an existing
  build directory must be recreated from clean; a stale object now fails to
  load with a missing-soname error rather than linking against an
  incompatible `ProtoSpace` layout. Verified: clean `build_release`, `ldd`
  confirming `libprotoCore.so.2`, `ctest -j1` 383/383.
- Function wrappers use separate single-arity and multi-arity prototypes, and
  the captures attribute is read only when a body has captures, reducing the
  attribute lookups per call.
- Objects that are never mutated after construction are immutable protoCore
  objects: function wrappers (`fn`, `defn`, multi-arity), the reader's string,
  vector and map literal wrappers, and the prototype markers. A mutable object
  registers permanently in protoCore's mutables tree and pays a
  compare-and-set on a shared shard for every attribute it receives, so every
  closure created used to leave a permanent entry behind. Only values
  mutated in place stay mutable: the globals namespace, the keyword and
  symbol intern table, atoms, futures, promises and actors.

### Fixed

- **An idle actor worker could deadlock the whole process at a collection.**
  Workers are protoCore threads, so a stop-the-world phase waits for every
  one of them to park at a safepoint — and a `std::condition_variable` wait
  is not a safepoint. A worker idling in `popReady_` therefore never parked,
  the collector waited for it for ever, and every other thread waited for the
  collector. It needed only one idle worker and one collection; the
  conformance fixtures never hit it because they allocate too little to
  collect. The wait now runs inside a `ProtoContext::UnmanagedScope`, as the
  promise poll in `deref` already did, and releases `queueMtx_` before
  rejoining the quorum — rejoining blocks while a stop-the-world is in
  progress, and holding that mutex across the block would stall every sender.
  Found by `tests/cli/actor-payloads-survive-gc.sh`, which hangs the
  pre-migration binary.

- Native primitives that build a large collection no longer exhaust the heap.
  protoCore reclaims a context's allocations only once that context is
  destroyed, or once the embedder reaches a safepoint with everything live
  rooted; `ProtoList` is immutable, so each `appendLast` leaves a root-to-leaf
  path (~log2 N cells) behind. A primitive that appended N times inside one
  long-lived context therefore pinned ~N·log2(N) cells of unreachable garbage
  until it returned, and under a heap ceiling
  (`PROTOCORE_HEAP_LIMIT_CELLS`) ran out of memory with a live set of a few
  hundred thousand cells.

  `vector` and `list` no longer rebuild anything: the positional arguments
  already are a `ProtoList`, so `vector` hands it straight to
  `newTupleFromList`, which builds the tuple bottom-up in one pass, and
  `list` returns it as it stands. Every builder whose size the program
  controls — `map`, `filter`, `reverse`, `keys` / `vals`, `split`, `pmap`,
  `send`, the reader's list and top-level form accumulators, and the VM's
  call-argument packing — now grows through `ListBuilder`
  (`src/runtime/ListBuilder.{h,cpp}`), which keeps the accumulator in an
  automatic local and folds elements in inside a short-lived child context so
  each chunk's garbage becomes collectable while the build is still running.
  Calls of five arguments or fewer — that is, nearly all of them — now build
  their argument list in a single cell instead of one per argument.

  Measured as the `PROTOCORE_HEAP_LIMIT_CELLS` needed to run a
  70,000-element vector literal: read and compiled but never evaluated,
  1,500,000 → 200,000 cells; evaluated, 3,000,000 → 1,500,000;
  `tests/cli/large-program`, 4,000,000 → 3,000,000. `fib` and `tak` are
  unchanged. The rule this follows is written down in `docs/DESIGN.md` § 1.1.
- The reader gets about twice as deep before the native stack guard fires,
  because `readList` no longer keeps a whole `ProtoContext` in its native
  frame. The guard measures remaining stack rather than nesting level, so the
  protection is unchanged; `tests/cli/deeply-nested-source` now uses 200,000
  nested forms where it used 100,000.
- `reset!` replaces the value with one compare-and-set. It read the old value
  and then wrote the new one in two steps, so under concurrent writers two
  `reset!` calls could hand their watches the same old value, and a replaced
  value was never reported. The old value the watches receive is now exactly
  the value that call replaced.
- Watches no longer run inside a protoCore critical section. With more than
  three watches on an atom, protoCore walked the watches map inside a GC
  critical section, and the watch functions ran inside it. That is user
  code, which may allocate heavily, wait for a future or start threads, and
  a thread in a critical section neither reaches heap-limit checkpoints nor
  parks for garbage collection. The watches are now collected into a list
  first and called after the walk.
- Promise delivery is atomic. `deliver`, and the actor worker delivering the
  promise `send` returned, first marked the promise done and then stored the
  value in a second write, so a concurrent `deref` or `realized?` could see a
  delivered promise whose value was still `nil`. A promise now holds its
  value in one attribute, installed with a single compare-and-set, and
  `deref`, `realized?` and the printer read only that attribute.
- `swap!` and `reset!` keep the old and new atom values rooted while the
  update function and the watches run. Another thread could replace the
  atom's value and drop the last reference to the old value, which was held
  only in a C++ local across allocations, and the new value returned by the
  update function was unrooted across the compare-and-set.
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
- Maps are immutable protoCore `ProtoSparseList`s indexed by canonical keys.
  A map used to be a mutable wrapper object around two sparse lists (an
  insertion-order store and a hash index with collision buckets). Every map
  version registered permanently in protoCore's mutables tree, iteration
  looked each key up again, and the insertion order was a guarantee
  Clojure does not make.
  - A map is now the sparse list itself, indexed by the address of each
    key's interned canonical key and holding the original key and the
    value. `count` is O(1).
  - Iteration and print order are unspecified and can change between runs.
  - `assoc` of an equal key keeps the stored key object and replaces the
    value, returning the same map for an identical value.
  - Watches fire in unspecified order, as on the JVM.
  - Canonical keys:
    - strings are interned symbols, so a string built at run time finds a
      literal key;
    - a list and a vector with equal elements are one key;
    - a map key is matched by its entries;
    - doubles match by bit pattern: `-0.0` and `0.0` are different keys, and
      `##NaN` finds itself (deviation D22);
    - big integers match by exact value;
    - numbers of different types are different keys, as in JVM Clojure,
      although `(= 1 1.0)` stays `true` (deviation D15), so
      `(= {1 :a} {1.0 :a})` is now `false`;
    - doubles, big integers and maps are keyed by tuples headed by private
      marker objects, so no user vector collides with them.
  - Canonical keys are interned and stay in memory until the program ends
    (deviation D23).
  - The map value hash (`valueHash`) and the `__map__` state attribute are
    removed.
  - All map operations live in `src/runtime/MapOps.{h,cpp}`.
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
- Map keys match by value. A map or a list used as a map key was matched by
  identity, so `(get {{:a 1} :x} {:a 1})` returned `nil`. Keys now match
  through their canonical keys (see the map representation entry above), and
  maps whose keys are collections compare by value.
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
  `0.0` or any other NaN. The ordering opcodes and `=` now use protoCore's
  `partialCompare`: every ordering and `=` with a NaN is
  `false`, `not=` is `true`, and a NaN is `=` only to the identical object,
  as in JVM Clojure. Numbers still compare exactly across integer and float
  representations, and `-0.0` is still `=` to `0.0`.

## Decisions

protoClojure has no separate decisions log; design decisions that were not
taken by the maintainer are recorded here.

### R2 — vectors off `ProtoTuple` *(agent, pending review)*

**Taken by the implementing agent on 2026-09-24**, under the maintainer's
authorisation for the overnight run of platform Track C
(`protoScala/docs/ROADMAP.md`). It had not been decided; protoScala's DESIGN
risk R2 lists it as "maintainer + platform track".

**The decision.** A vector is the `ProtoList` of its elements inside a
one-entry `ProtoSparseList` box (`src/runtime/VectorOps.h`).

**Why.** `ProtoList` is what a vector wants: an immutable AVL list with
structural sharing, an O(N) bulk constructor, O(log N) indexed access, and
ordinary garbage collection. `ProtoTuple` gave compactness (four elements per
node) and interning — and interning is the defect: protoCore never frees an
interned tuple node, so a long-running program retained every vector it ever
built. Verified against how protoClojure actually uses vectors before
committing to it: `nth` is one indexed read, `count` is O(1) either way,
`vector` / `vec` receive their elements *as a `ProtoList` already* (so they
became O(1) boxes instead of O(N) tuple builds), every seq operation went
through an O(N) `ProtoTuple::asList` conversion that is now an O(1) unbox, and
`conj`, `assoc` on a vector and `subvec` are not implemented at all, so no
tail-append or update path was affected.

**Why the box.** A bare `ProtoList` was not possible: a Clojure *list* is
already a `ProtoList`, and `vector?`, `list?`, `conj` and printing (`[1 2]`
against `(1 2)`) must keep telling the two apart in O(1). protoClojure cannot
mint a protoCore pointer tag, and the alternatives — a wrapper object with an
`__items__` attribute, or a marker element inside the list — cost an attribute
lookup or an index shift on every access, and a wrapper would have to be
mutable and so would enter protoCore's mutables tree, which is walked at every
stop-the-world. The one-entry sparse list is immutable, costs one Small-form
cell and an inline scan of up to three slots, and reuses the very pointer tag
that protoClojure's maps vacated when they moved to `ProtoMap` in this same
release. Keying the sparse list by element index instead — a vector as a dense
`ProtoSparseList` — was rejected: protoCore has no bulk sparse-list builder,
so building an N-element vector would be N `setAt` calls, O(N log N) with
log N cells of garbage each, against the one-pass `ProtoList` the arguments
already are.

**The access-cost change.** Indexed access costs about twice the node hops
(log2 N against log4 N) and a large vector about three times the cells of the
tuple's payload, offset by the box's single cell and by no interner entry.
Construction, `vec`, and every sequential operation are cheaper — O(1) where
they were O(N). Equal vectors stopped being pointer-equal. Measured on a
settled machine: 50 full `nth` scans of a 20,000-element vector are
indistinguishable from the tuple (0.61-0.69 s against 0.59-0.73 s), so the
extra hops sit below the interpreter's per-iteration cost at that size, while
a build plus one full scan of a 200,000-element vector is 0.47-0.49 s and
296 MB against 0.76-0.81 s and 545 MB. Nothing measured is slower; a
random-access-dominated workload on a vector of millions of elements is where
the extra hops would show, and no such workload exists in the suite or the
benchmarks.

**What reversing it would cost.** The representation is confined to
`src/runtime/VectorOps.{h,cpp}` and its callers in `Primitives.cpp` and
`MapOps.cpp`; `isVector` / `newVector` / `vectorItems` are the whole seam, so
going back to `ProtoTuple` is re-implementing those three functions plus the
`vector?` tag test — a day's work at most, and the test suite pins the
behaviour either way. What would come back with it is the perennial retention
of every vector, and what would be lost is the O(1) construction and seq
coercion. A third option, if the maintainer wants the tuple's compactness
without its interning, is a non-interned tuple type in protoCore; that is a
protoCore change and needs a pointer tag, which this decision deliberately
did not take.

