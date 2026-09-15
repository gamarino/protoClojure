# Phase 1 — bootstrap interpreter implementation spec

> **Purpose.** Decide, before any code is written, the precise shape
> of the four-week phase-1 deliverable: the reader, the compiler,
> the bytecode VM, and the smallest possible `core.clj` such that
> `protoclj script.clj` runs hello-world, factorial, fizzbuzz, and
> word-count. The clojure.core spec
> ([`2026-06-14-clojure-core-v0.1.md`](2026-06-14-clojure-core-v0.1.md))
> already decided the *full* v0.1 surface; this spec decides what
> ships at the end of the **first** four weeks, and what is deferred
> to the rest of the v0.1 schedule.

**Date.** 2026-06-14.
**Status.** Approved internally; subject to revision when phase 1
begins implementation.

---

## 1. Scope of phase 1

In, out, deferred.

### 1.1 In phase 1

By the end of week 4, `protoclj` runs the following kinds of files:

- A hello-world script that prints a constant.
- Arithmetic scripts using `+`, `-`, `*`, `/`, `quot`, `rem`,
  comparison, `min`/`max`.
- Function definitions with `defn`, including multi-arity and
  variadic.
- Tail recursion via `loop` / `recur`.
- The four persistent collection literals (`'(...)`, `[...]`,
  `{...}`, `#{...}`).
- The collection core operations needed for fizzbuzz and word-count:
  `count`, `first`, `rest`, `conj`, `assoc`, `get`, `nth`, `into`,
  `seq`, `empty?`.
- `map`, `filter`, `reduce` (the 2- and 3-arg arities — see arity
  discipline in [`2026-06-14-clojure-core-v0.1.md`](2026-06-14-clojure-core-v0.1.md)
  §6).
- The threading macros `->`, `->>`.
- The control macros `when`, `when-not`, `if-let`, `cond`, `case`,
  `and`, `or`.
- `let`, `loop`, `fn`, `defn`, `defmacro`, `ns`, `def` user-facing
  forms.
- Exception handling: `try`/`catch`/`finally`, `throw`, `ex-info`,
  `ex-data`, `ex-message`.
- Atoms with `swap!` / `reset!` / `deref` / `@`.
- The `ns` form with the Clojure path resolver — `(:require [foo.bar
  :as fb])` works against a local `.clj` file.
- An interactive REPL with the bare minimum meta-commands.
- A file runner: `protoclj script.clj` reads, compiles, runs.

### 1.2 Not in phase 1

The following are explicitly **out** of the phase-1 deliverable. They
land in subsequent phases of the v0.1 roadmap.

- Macros defined *by the user* (only the bootstrap macros work in
  phase 1; user `defmacro` arrives at the end of week 4 as the
  capstone). Phase 3 of the roadmap moves all `core.clj` macros
  from C++ into Clojure-side `defmacro` and is the proper macros
  phase.
- Lazy sequences (`lazy-seq`, `delay`, infinite ranges). They arrive
  in phase 2 alongside the full sequence vocabulary.
- The UMD interop providers (`py/`, `js/`, `pst/`). Phase 4.
- nREPL. Phase 5.
- Protocols and multimethods. Phase 3.
- Most of `clojure.core` (the rest of the §5 catalog of the
  clojure.core spec). Phase 2.

### 1.3 The four reference scripts

These four scripts are the **executable spec** of phase 1. By the
end of week 4, each runs unchanged through `protoclj`. Any change
needed to make them work is in scope.

**`examples/01-hello.clj`**
```clojure
(println "hello, world")
```

**`examples/02-factorial.clj`**
```clojure
(defn factorial [n]
  (loop [n n acc 1]
    (if (<= n 1)
      acc
      (recur (dec n) (* acc n)))))

(println (factorial 20))
;; expect: 2432902008176640000
```

**`examples/03-fizzbuzz.clj`**
```clojure
(defn fizzbuzz [n]
  (cond
    (zero? (mod n 15)) "FizzBuzz"
    (zero? (mod n 3))  "Fizz"
    (zero? (mod n 5))  "Buzz"
    :else              (str n)))

(doseq [n (range 1 16)]
  (println (fizzbuzz n)))
;; expect: 1 2 Fizz 4 Buzz Fizz 7 8 Fizz Buzz 11 Fizz 13 14 FizzBuzz
```

**`examples/04-word-count.clj`**
```clojure
(ns demo.word-count)

(defn word-freq [text]
  (->> text
       (re-seq #"\w+")
       frequencies
       (sort-by (comp - val))
       (take 5)
       vec))

(defn -main [& args]
  (let [text (or (first args)
                 "the cat sat on the mat the cat sat")]
    (doseq [[w c] (word-freq text)]
      (println (str c " " w)))))

(-main)
;; expect:
;; 3 the
;; 2 cat
;; 2 sat
;; 1 on
;; 1 mat
```

The four together exercise: literals, arithmetic, function
definitions, closures, `loop`/`recur`, `cond`, `doseq`, `range`,
`re-seq`, `frequencies`, `sort-by`, `take`, `vec`, threading, `ns`,
multi-arity `defn`, regex, command-line args, the file runner.

Anything not used by these four scripts can land later in phase 2
without blocking phase 1's completion.

---

## 2. Architecture

### 2.1 Source-tree layout

```
protoClojure/
├── CMakeLists.txt
├── headers/
│   └── protoClojure.h           # public API for embedders
├── src/
│   ├── reader/
│   │   ├── Lexer.h / .cpp       # tokeniser
│   │   ├── Reader.h / .cpp      # token → form (protoCore objects)
│   │   ├── ReaderMacros.h / .cpp# `, ~, ~@, #(...), #{}, #'
│   │   └── SourcePos.h          # line/col tracking metadata
│   ├── compiler/
│   │   ├── Compiler.h / .cpp    # form → bytecode
│   │   ├── Analyser.h / .cpp    # scope + tail-position analysis
│   │   ├── MacroExpand.h / .cpp # macro driver: call macro, take result
│   │   └── BytecodeModule.h / .cpp  # const pool, instructions, debug info
│   ├── runtime/
│   │   ├── Opcodes.h            # the bytecode opcode enum
│   │   ├── ExecutionEngine.h / .cpp # the VM
│   │   ├── Frame.h              # call frame layout
│   │   ├── Namespace.h / .cpp   # the var table
│   │   ├── Var.h                # the var cell
│   │   └── ProtoClojureRuntime.h / .cpp  # the runtime singleton
│   ├── primitives/
│   │   ├── arith_prims.cpp
│   │   ├── compare_prims.cpp
│   │   ├── collection_prims.cpp
│   │   ├── atom_prims.cpp
│   │   ├── string_prims.cpp
│   │   ├── print_prims.cpp
│   │   ├── error_prims.cpp
│   │   └── PrimitiveRegistry.h / .cpp
│   ├── bootstrap_macros/
│   │   ├── defn_macro.cpp       # one .cpp per C++ bootstrap macro
│   │   ├── defmacro_macro.cpp
│   │   ├── let_macro.cpp
│   │   ├── fn_macro.cpp
│   │   ├── loop_macro.cpp
│   │   ├── when_macro.cpp
│   │   └── ns_macro.cpp
│   ├── repl/
│   │   ├── Repl.h / .cpp        # local interactive prompt
│   │   └── ReplCommands.cpp     # the `:help`, `:exit`, etc.
│   ├── path/
│   │   └── ClojurePathResolver.h / .cpp  # the .clj source resolver
│   └── main.cpp                  # the protoclj entry point
├── resources/
│   └── clojure/
│       └── core.clj             # the phase-1 core.clj (see §9)
├── test/
│   ├── unit/                    # GoogleTest for C++ pieces
│   ├── conformance/             # .clj fixtures run black-box
│   └── benchmarks/              # phase-1 smoke benchmarks
└── examples/
    ├── 01-hello.clj
    ├── 02-factorial.clj
    ├── 03-fizzbuzz.clj
    └── 04-word-count.clj
```

Total estimated phase-1 source size: ~8000 lines of C++ + ~600
lines of Clojure (`core.clj`) + ~1500 lines of test code.

### 2.2 Dependencies

- **protoCore.** Linked as a shared library. We use the public
  headers (`headers/protoCore.h`) plus the same patterns
  protoST/protoJS/protoPython use for GC discipline (TransientPin,
  ProtoRootSet).
- **GoogleTest.** For C++ unit tests. CMake fetches it via
  `FetchContent` — same pattern as protoCore / protoST.
- **No third-party Clojure libs.** The phase-1 build is
  self-contained.

### 2.3 The binary

A single executable `protoclj`, built by CMake. Invocations:

```bash
protoclj                          # interactive REPL
protoclj script.clj               # run a file
protoclj -e '(+ 1 2)'             # eval one form
protoclj --version
protoclj --help
```

Phase 5 adds `--nrepl PORT`. Phase 1 does not need it.

---

## 3. The reader

The reader turns text into protoCore objects. It is two passes:
a tokeniser (Lexer) and a parser (Reader). The reader macros (`'`,
`` ` ``, `~`, `~@`, `#(...)`, `#{}`, `#_`, `#'`) are handled at the
parser level.

### 3.1 The token grammar

```
Token := Number | String | Char | Symbol | Keyword | Boolean | Nil
       | Open | Close                       // ( ) [ ] { }
       | HashOpen                            // #{
       | HashFn                              // #(
       | HashUnderscore                      // #_
       | HashApostrophe                      // #'
       | HashDoubleQuote                     // #"  (regex literal)
       | Apostrophe                          // '
       | Backtick                            // `
       | Tilde                               // ~
       | TildeAt                             // ~@
       | Caret                               // ^  (metadata)
       | Comment                             // ; to end of line
       | EOF
       | Error
```

The lexer is hand-written recursive single-character lookahead, the
same pattern protoST's lexer uses. Whitespace (including commas, per
Clojure convention) separates tokens. Comments run from `;` to end
of line.

**Numbers** — the most subtle case:

- `42`, `-7`, `0xff`, `0b1010`, `2r1010` — integers (Long, then
  LargeInteger on overflow).
- `3.14`, `1e10`, `-0.5e-3` — floats (IEEE 754 double).
- `3/4`, `-22/7` — ratios.
- `3.14M` — BigDecimal **reserved**, raises with v0.2 message.

**Strings** — double-quoted, backslash escapes `\n \t \r \\ \"`,
unicode `\uXXXX`, octal `\oNNN`. Multi-line by literal newline.

**Characters** — `\a`, `\space`, `\tab`, `\newline`, `é`. Reads as
a 1-codepoint protoCore String (deviation D3).

**Symbols** — letters, digits, `* + ! - _ ' ? < > = . / :`. First
char cannot be a digit. Optional namespace prefix: `foo/bar`.
Special symbols `true`, `false`, `nil` produce the corresponding
tokens, NOT a symbol.

**Keywords** — symbols prefixed with `:`. `::foo` is
"namespace-qualified to the current namespace" — resolution happens
at reader time, using the namespace then in scope.

**Regex literals** — `#"..."` is a regex pattern. Reads as a
ProtoCore `Regex` object (wrapping a `std::regex`). Phase 1 needs
this for word-count.

### 3.2 The parser

After tokens come *forms*. The parser produces protoCore objects:

- A list `(a b c)` reads as a ProtoList (linked).
- A vector `[a b c]` reads as a ProtoList (AVL-indexed) tagged as
  vector.
- A map `{:a 1 :b 2}` reads as a ProtoSparseList tagged as map.
- A set `#{a b c}` reads as a ProtoSparseList tagged as set.

The "tagged" part is a property of the prototype: `clojure.lang/Vector`
prototype is a child of `clojure.lang/PersistentCollection`, etc.
Dispatch on collection type is one chain-walk.

### 3.3 Reader macros — translation table

| Reader form           | Translates to                              |
|-----------------------|--------------------------------------------|
| `'form`               | `(quote form)`                             |
| `` `form ``           | `(syntax-quote form)` — see §3.5           |
| `~form`               | `(clojure.core/unquote form)`              |
| `~@form`              | `(clojure.core/unquote-splicing form)`     |
| `#(...)`              | `(fn [...] (...))` — see §3.4              |
| `#{...}`              | `#{...}` — set literal (handled in lexer)  |
| `#_form`              | nothing — the next form is discarded       |
| `#'sym`               | `(var sym)`                                |
| `#"..."`              | `(re-pattern "...")`                       |
| `^meta form`          | `(with-meta form meta)`                    |

### 3.4 `#(...)` — anonymous function shorthand

The reader rewrites `#(body)` into a `fn` form with positional args:

- `%`, `%1`, …, `%9` map to positional args.
- `%&` maps to a rest arg.

Algorithm:

1. Read the body normally, collecting all `%`/`%n`/`%&` references.
2. Determine the highest N referenced.
3. Construct `(fn [%1 %2 ... %N & %&] body)`, eliding the rest arg
   if `%&` was not used.
4. If `%` (no number) was used, treat as `%1` for arity-1
   compatibility.

Edge case: `#(...#(...)...)` is a reader error. Nested anonymous
shorthands collide on the `%` namespace.

### 3.5 Syntax-quote — the macro author's tool

`` `form `` is the most subtle reader-macro behaviour. The
algorithm:

1. Walk `form` recursively.
2. For each symbol, resolve against the current namespace and
   replace with the fully-qualified symbol. (So `` `if `` becomes
   `` clojure.core/if ``.)
3. For each `~form`, leave the inner form unevaluated for the
   compiler to splice in.
4. For each `~@form`, mark for sequence-splicing.
5. For each symbol ending in `#`, generate a fresh gensym **scoped
   to the enclosing syntax-quote**, so repeated references inside
   the same backtick produce the same symbol.

This is the trickiest piece of the reader. It must be correct on
day one because macros built in `core.clj` rely on it; getting it
wrong cascades into every macro.

### 3.6 Reader output — the AST shape

The reader produces protoCore objects, full stop. There is no
intermediate AST node type. A list is a `ProtoList`; a symbol is
a `ProtoString` with the `:symbol` type metadata; a number is a
`ProtoInteger`. Code is data, literally.

Source positions are attached as **metadata** on lists, vectors, and
maps:

```
{:line 12 :column 7 :file "src/demo.clj"}
```

The compiler reads this metadata to produce useful error messages.
Atoms (numbers, strings, etc.) do not carry source-position
metadata in v0.1; we accept the loss.

---

## 4. The bytecode module

### 4.1 The instruction set

Lifted from protoST's design with Clojure-specific opcodes added.
Each instruction is 2 bytes (opcode + 1 byte operand), with EXTEND
prefixes for operands > 255.

| Opcode               | Operand               | Stack effect                          |
|----------------------|-----------------------|---------------------------------------|
| `NOP`                | —                     | none                                  |
| `PUSH_CONST`         | const pool index      | push 1                                |
| `PUSH_LOCAL`         | local slot            | push 1                                |
| `STORE_LOCAL`        | local slot            | pop 1                                 |
| `PUSH_NIL`           | —                     | push 1                                |
| `PUSH_TRUE`          | —                     | push 1                                |
| `PUSH_FALSE`         | —                     | push 1                                |
| `DUP`                | —                     | push 1 (copy of top)                  |
| `POP`                | —                     | pop 1                                 |
| `PUSH_VAR`           | const pool sym index  | push 1 (var deref)                    |
| `STORE_VAR`          | const pool sym index  | pop 1                                 |
| `PUSH_DYNAMIC`       | const pool sym index  | push 1 (binding-stack lookup)         |
| `INVOKE`             | argc                  | pop (argc + 1), push 1                |
| `INVOKE_RECUR`       | argc                  | jump to recur target with bindings    |
| `JUMP`               | offset                | none                                  |
| `JUMP_IF_TRUE`       | offset                | pop 1                                 |
| `JUMP_IF_FALSE`      | offset                | pop 1                                 |
| `RETURN`             | —                     | pop frame                             |
| `MAKE_FN`            | block index           | push 1 (closure)                      |
| `MAKE_VECTOR`        | size                  | pop size, push 1                      |
| `MAKE_MAP`           | size×2                | pop 2×size, push 1                    |
| `MAKE_SET`           | size                  | pop size, push 1                      |
| `THROW`              | —                     | pop 1, raises                         |
| `TRY_ENTER`          | handler offset        | push handler frame                    |
| `TRY_EXIT`           | —                     | pop handler frame                     |
| `BIN_INT_ADD`        | —                     | pop 2, push 1 (fast-path int +)       |
| `BIN_INT_SUB`        | —                     | pop 2, push 1                         |
| `BIN_INT_LT`         | —                     | pop 2, push 1                         |
| `BIN_INT_LE`         | —                     | pop 2, push 1                         |
| `BIN_INT_EQ`         | —                     | pop 2, push 1                         |
| `EXTEND`             | high byte             | next instruction's operand widened    |
| `HALT`               | —                     | (debugger)                            |

About 30 opcodes for phase 1. Specific notes:

- **`PUSH_VAR` vs `PUSH_LOCAL`.** Locals are slot-indexed within a
  frame. Vars are namespace-scoped indirections — when a user
  redefines a var at the REPL, the next `PUSH_VAR` sees the new
  binding. This is the mechanism that makes REPL-driven development
  work.
- **`INVOKE_RECUR`.** Distinct from `INVOKE` because it does not
  push a new frame — it rebinds the current frame's locals and
  jumps to PC 0 (for `fn` recur) or the `loop` target. Required
  for unbounded `recur` without stack growth.
- **The `BIN_INT_*` fast paths.** Same trick protoST uses: if both
  operands are tagged SmallInts, compute in C++; otherwise fall
  through to the general `INVOKE` of `clojure.core/+`. Saves a
  function call on the common case.
- **`TRY_ENTER` / `TRY_EXIT`.** Push a handler frame onto a parallel
  stack inside the frame. On `THROW`, the engine unwinds until it
  hits a matching handler.

### 4.2 The const pool

A `BytecodeModule` carries:

- A vector of `Const` entries (one per literal constant).
- A vector of sub-modules (one per closure body).
- A bytecode byte vector.
- Source-position info for error reporting.

`Const` kinds for phase 1:

```
enum class ConstKind {
    Long, Double, Ratio,
    String,
    Symbol, Keyword,
    Vector, List, Map, Set,         // for literal collection consts
    Regex,
    Nil, True, False,
    BlockRef                         // index into the sub-modules vector
};
```

Const-pool entries are de-duplicated within a module: the same
symbol or string is interned once and referenced by index.

### 4.3 Bytecode loading

Each top-level form compiles to its own `BytecodeModule`. The
runtime keeps a list of loaded modules (for debugging and macro
expansion). The compiler-to-runtime hand-off is just calling
`ExecutionEngine.run(module)`.

---

## 5. The compiler

A single-pass lowering from reader output (protoCore objects) to
bytecode.

### 5.1 The entry point

```cpp
// One Clojure form → one BytecodeModule.
std::unique_ptr<BytecodeModule> Compiler::compileForm(
    const proto::ProtoObject* form, Namespace* ns);
```

The compiler maintains a *compile-time state*:

- The current namespace (where vars get interned).
- A scope stack (lexical bindings).
- A loop-target stack (for `recur` validation).
- A tail-position flag.

### 5.2 The dispatch table

For each form, the compiler checks:

1. Is the form an atom (number, string, etc.)? Push as constant.
2. Is it a symbol? Resolve via the scope chain → emit `PUSH_LOCAL`
   or `PUSH_VAR`.
3. Is it a vector/map/set literal? Compile each element and emit a
   `MAKE_*` opcode.
4. Is it a list? The first element decides:
   - One of the 11 special forms (§3.1 of the
     clojure.core spec) → special compilation.
   - A symbol resolving to a *macro* → call the macro at compile
     time, compile its result instead.
   - Anything else → ordinary invocation. Compile the callee,
     compile each arg, emit `INVOKE argc`.

### 5.3 Special-form compilation

A short rundown of each. Details are mechanical.

**`(if test then else)`** —
```
[compile test]
JUMP_IF_FALSE label-else
[compile then]
JUMP label-end
label-else:
[compile else]
label-end:
```

**`(do form1 form2 ... formN)`** —
```
[compile form1] POP
[compile form2] POP
...
[compile formN]   ; last value left on stack
```

**`(let* [a expr-a b expr-b] body)`** —
```
[compile expr-a] STORE_LOCAL slot-a
[compile expr-b] STORE_LOCAL slot-b
[compile body]
```

(The user-facing `let` macro destructures; the special `let*` only
binds bare symbols.)

**`(loop* [bindings] body)`** — like `let*` but also installs a
recur target at the PC of the body's first instruction.

**`(recur arg1 arg2 ...)`** —
```
[compile arg1] STORE_LOCAL recur-slot-1
[compile arg2] STORE_LOCAL recur-slot-2
...
JUMP back-to-recur-target
```

The compiler checks that `recur` appears in tail position (last
statement of `if`, `when`, `do`, `let`, function body) and that
the arity matches the recur target. Mismatch = compile error
with position.

**`(fn* [params] body)`** — compiles a new sub-module for the
function body, captures the lexical scope of free variables, emits
`MAKE_FN <sub-module-index>` in the outer module. The closure
object carries:

- A pointer to the sub-module.
- A `captured` dict of name → value for closed-over locals
  (mutable protoCore object).

`MAKE_FN` is the same pattern protoST uses for blocks.

**`(quote form)`** — emit a `PUSH_CONST` referencing `form` in the
const pool. The form has already been read as a protoCore object,
so it can be stored directly.

**`(def name expr)`** or `(def name docstring expr)` — interns
`name` as a var in the current namespace, compiles `expr`, emits
the store.

**`(var sym)`** — emit `PUSH_VAR sym` but resolving to the var
itself rather than its current value. (A separate opcode
`PUSH_VAR_RAW` is cleaner; phase 1 uses `PUSH_VAR` and a flag bit.)

**`(throw expr)`** — compile `expr`, emit `THROW`. The expression
must evaluate to an exception object.

**`(try body (catch Class e handler) (finally cleanup))`** —
emits `TRY_ENTER handler-pc`, compiles body, emits `TRY_EXIT`,
arranges for `finally` to run on both normal and abnormal exit. The
catch clause stores the exception in a fresh local and dispatches
on class.

### 5.4 Macro expansion

When the compiler sees `(macro-name args)` where `macro-name`
resolves to a macro var:

1. Read the macro var's value (a callable: either a C++ bootstrap
   macro or a Clojure-defined `defmacro`).
2. Call the macro with the **unevaluated** args (as protoCore
   objects).
3. Take the return value (a new form).
4. Compile that form instead, recursively.

The compiler's `expand-1` recursion stops when the result is no
longer a macro call.

**GC discipline during macro expansion.** The unevaluated args are
protoCore objects that the C++ stack holds while calling the macro;
they must be pinned via TransientPin (`include/protoClojure/TransientPin.h`
— same pattern as protoST). Without it, the GC may reclaim them
mid-expansion.

### 5.5 Closure capture

For each `fn*` body, the compiler runs a scope analysis pass to
determine which symbols in the body refer to **outer-scope
locals**. Those become the closure's captured set.

Implementation matches protoST exactly: free names get a
`captured-dict` attribute on the closure object, and accesses go
through `PUSH_CAPTURED` / `STORE_CAPTURED` opcodes (added to §4.1 if
not already there). For phase 1 simplicity we can fold this into
`PUSH_VAR`/`STORE_VAR` with an extra bit, but cleaner to add the
dedicated opcodes.

**Decision for phase 1:** add `PUSH_CAPTURED` and `STORE_CAPTURED`
opcodes. Costs nothing and the implementation is clearer.

### 5.6 Error reporting

Every compile error carries:

- The form (a protoCore object).
- The source position (from the form's metadata).
- A precise message — `"Unable to resolve symbol: foo"`,
  `"recur can only be used in tail position"`, etc.

The user sees:
```
$ protoclj src/demo.clj
src/demo.clj:14:3: Unable to resolve symbol: foo
```

---

## 6. The VM

### 6.1 The frame

Each frame holds:

- A pointer to its bytecode module.
- A program counter (PC).
- A pointer into a shared operand stack (the stack base for this
  frame).
- An array of locals (sized by the bytecode module's
  `localCount`).
- A pointer to its `captured` dict (or null for non-closure
  frames).
- A pointer to the var being defined (for `(def name (fn ...))`
  patterns).
- A recur target PC.
- A handler stack (for nested `try` blocks).

Layout matches protoST's `Frame` struct exactly. We can lift the
header.

### 6.2 The dispatch loop

Threaded-goto in C++:

```cpp
const Op* labels[256];
labels[Op::PUSH_CONST] = &&L_PUSH_CONST;
labels[Op::PUSH_LOCAL] = &&L_PUSH_LOCAL;
// ...

#define DISPATCH() goto *labels[*pc++]

DISPATCH();

L_PUSH_CONST:
  push(const_pool[*pc++]);
  DISPATCH();

L_PUSH_LOCAL:
  push(locals[*pc++]);
  DISPATCH();

// ... etc.
```

Same threading-goto trick protoST uses (commit `b883fa9`). Standard
performance idiom for dispatch loops.

### 6.3 GC discipline inside the VM

Every C++ local that holds a `ProtoObject*` across an operation
that may allocate is pinned via `TransientPin`. Operations that
allocate:

- `INVOKE` calls user functions which may allocate freely.
- `MAKE_VECTOR` / `MAKE_MAP` / `MAKE_SET` allocate the result.
- Most `clojure.core` primitive functions allocate.
- The macro expansion driver may allocate during expansion.

The pattern lives in `headers/protoClojure/TransientPin.h`, lifted
from protoST's TransientPin (which lifted it from protoCore's
ProtoRootSet pattern). The discipline is documented in a
project-level GC-roots audit, similar to protoPython's
`tasks/audit/03-gc-roots.md` — we will start that audit in phase 1
and grow it as primitives land.

### 6.4 Exception handling

`THROW`:
1. Pop the exception value.
2. Walk the handler stack (in the current frame, then the next
   frame down, etc.) looking for a matching handler.
3. If found: unwind to that frame, set PC to the handler, push the
   exception, dispatch.
4. If no handler in any frame: propagate up to the run-loop boundary;
   the top-level runner prints a stack trace and returns non-zero.

`TRY_ENTER handler-pc`:
- Push a `HandlerFrame { pc, fn }` onto the frame's handler stack.

`TRY_EXIT`:
- Pop the top handler.

`finally` clauses fire on both normal and abnormal exit. The
compiler arranges this by emitting the `finally` body inline at the
end of the `try` body AND as a fragment inside any handler that
matches.

---

## 7. Namespaces and vars

### 7.1 Namespace structure

A `Namespace` is a C++ struct (held as a protoCore mutable object):

- `name` — the namespace's symbol (`my.app`).
- `vars` — a map from local-name → var.
- `aliases` — a map from alias → target namespace.
- `refers` — a map from external-name → var (from another
  namespace).
- `meta` — namespace metadata.

A `Var` is a protoCore mutable object with attributes:

- `value` — the current root binding.
- `meta` — the var's metadata (docstring, arglists, etc.).
- `dynamic?` — boolean.
- `binding-stack` — for dynamic vars, the thread-local stack of
  pushed bindings.

`def name expr` creates or updates the var named `name` in the
current namespace. Subsequent references to `name` resolve through
the var, so REPL redefinition propagates automatically.

### 7.2 Symbol resolution at the call site

When the compiler sees a bare symbol `foo`:

1. Look up `foo` in the lexical scope. Hit → emit `PUSH_LOCAL` or
   `PUSH_CAPTURED`.
2. Miss → look up `foo` in the current namespace's `vars`. Hit →
   emit `PUSH_VAR my-ns/foo`.
3. Miss → look up `foo` in the current namespace's `refers`. Hit
   → emit `PUSH_VAR external-ns/foo`.
4. Miss → look up `foo` in `clojure.core`'s `vars`. Hit → emit
   `PUSH_VAR clojure.core/foo`.
5. Miss → compile error: `Unable to resolve symbol: foo`.

For a namespace-qualified symbol `ns/name` (e.g., `str/upper-case`):

1. Look up `ns` in the current namespace's `aliases` →
   target-namespace.
2. Look up `name` in target-namespace's `vars`. Hit → emit
   `PUSH_VAR target-ns/name`.
3. Miss → compile error.

### 7.3 The Clojure path resolver

The phase-1 `(:require [foo.bar :as fb])` walks `CLOJURE_PATH` (a
colon-separated list of directories) plus the current source
file's directory. For each candidate directory, the resolver looks
for `foo/bar.clj`. The first hit wins.

The found file is read, compiled, and its namespace is bound to
`foo.bar` in the global namespace registry. Subsequent
`(:require [foo.bar])` finds it cached.

Phase 4 adds the `py/`, `js/`, `pst/` providers. Phase 1 only
implements the Clojure path resolver. If you write `(:require
[py/numpy :as np])` in a phase-1 build, the resolver raises a
clear "UMD providers not yet wired (phase 4)" error.

---

## 8. The REPL

### 8.1 The basic loop

`Repl::run()`:

```
print "protoClojure 0.1.0\nType :help for help. Ctrl-D to exit.\n"
loop forever:
  print "<current-ns>=> "
  read a line; if EOF, break
  if line starts with ":", dispatch as a meta-command, continue
  feed the line to the reader; if incomplete (mismatched parens),
    keep reading more lines and accumulating
  compile and evaluate
  print (pr-str result)
  bind result to *1; shift *2, *3
  on exception: bind to *e, print message + brief location, continue
```

Phase 1 uses raw line-by-line stdin. Phase 1 stretch: bring in
`linenoise` (single-file BSD-licensed readline replacement, ~1000
lines of C) for editing and history. Decision made at week 4.

### 8.2 Meta-commands

Phase 1 ships the minimum useful set:

| Command       | Behaviour                                            |
|---------------|------------------------------------------------------|
| `:help`       | Print this list                                      |
| `:exit`       | Exit (Ctrl-D works too)                              |
| `:reload <ns>`| Re-load a namespace's file                           |
| `:cd <path>`  | Change working directory                             |
| `:pwd`        | Print working directory                              |
| `:cp <path>`  | Add to CLOJURE_PATH                                  |
| `:in-ns <ns>` | Switch the current namespace                         |

`(doc symbol)` and `(source symbol)` are *functions*, not
meta-commands, so they work in scripts too. Phase 1 implements both
as regular Clojure functions backed by var metadata.

### 8.3 The repl-loaded user namespace

On startup, the REPL switches to `user`, which is created on demand
and starts with `clojure.core` referred. Subsequent `def`s land in
`user` unless the user `(:in-ns 'my.app)` first.

---

## 9. The phase-1 `clojure.core`

A subset of the full v0.1 catalog. Just enough that the four
reference scripts work. Approximately **45 names** versus the
~177-name full v0.1 catalog. The rest land in phase 2.

### 9.1 The phase-1 catalog

In C++:

```
+  -  *  /  quot  rem  mod  inc  dec
=  ==  not=  <  <=  >  >=
hash  not
nil?  true?  false?  some?
number?  integer?  string?  symbol?  keyword?  boolean?  fn?  ifn?
identity  constantly
count  first  rest  next  cons  conj  seq  empty
get  assoc  dissoc
list  vector  hash-map  hash-set
atom  swap!  reset!  compare-and-set!  deref
str  subs  format
print  println  pr  prn  newline
read-string
ex-info  ex-data  ex-message  ex-cause
var-get  var-set
```

C++ bootstrap macros:

```
defn  defmacro  let  fn  loop  when  ns
```

Defined in `core.clj`:

```
;; Boolean
and  or

;; Control
when-not  if-let  when-let  cond  case
declare  defonce  defn-

;; Threading
->  ->>

;; Iteration
dotimes  doseq

;; Collections
empty?  not-empty  nth  peek  pop  last
keys  vals  contains?  find  select-keys
update  get-in  assoc-in  update-in
merge  into  vec  set
reverse  concat  mapcat

;; Sequences (eager in phase 1 — no laziness yet)
range  take  drop  take-while  drop-while
distinct  partition  partition-all
interleave  interpose
sort  sort-by  frequencies  group-by  zipmap

;; Higher-order
map  filter  reduce  apply
comp  partial  juxt  complement
every?  some  not-every?  not-any?
doall  dorun

;; Math helpers
min  max  zero?  pos?  neg?  even?  odd?  abs

;; Printing
pr-str  print-str

;; Regex
re-pattern  re-find  re-seq  re-matches

;; Symbol/keyword
name  namespace  symbol  keyword

;; Atom watcher
add-watch  remove-watch
```

Total: ~85 Clojure-defined names + the C++ ones = approximately
130 vars in `clojure.core` at the end of phase 1.

### 9.2 What is deferred to phase 2

- `lazy-seq`, `delay`, infinite ranges (laziness wholesale).
- `iterate`, `repeat`, `cycle` (depend on laziness).
- Transducers.
- `chunked-seq` realisation behaviour.
- `clojure.string`, `clojure.set`, `clojure.walk` (separate
  namespaces).
- `defrecord`, `deftype`.
- `defprotocol`, `defmulti` (phase 3).
- All UMD interop (`clj->py` etc.) — phase 4.
- nREPL — phase 5.

### 9.3 Eager-only sequence semantics for phase 1

Without laziness, `map` returns a vector, not a lazy seq.
`filter` likewise. This is **observable** — `(realized? (map inc
[1 2 3]))` does not work — but does not cause script breakage
because the four reference scripts do not depend on laziness.

The behavioural shift between phase 1 (eager) and phase 2 (lazy)
must be communicated clearly. We mark the phase-1 release a
*preview* and warn users that pipelines that look infinite are not
yet supported.

---

## 10. The week-by-week plan

### 10.0 Testing strategy — conformance-first

Before the weekly deliverables, an architectural commitment about
how features get tested.

protoST and protoPython both grew large suites of **black-box
conformance fixtures** — small `.st` / `.py` files with an expected
output declared as a directive in the first line. A simple shell
runner invokes the binary, captures stdout, and compares.

protoClojure adopts the same pattern from **day one**. Each fixture
looks like this:

```clojure
;; EXPECT: hello, world
(println "hello, world")
```

```clojure
;; EXPECT: 30
(let [x 10 y 20] (println (+ x y)))
```

The runner is ~80 lines of `bash`, registered as a `ctest` test per
fixture. Authoring a new test is ~5 lines of Clojure plus one
directive comment.

**Why this matters for cost.** A GoogleTest case in C++ is ~40-80
lines of boilerplate; a conformance fixture is ~5 lines of
demonstrative Clojure. Over the 30-50 features that land in phase
1, the savings compound to days of calendar time. The fixtures
also *double as documentation* — each one is a runnable example of
the feature it covers.

**Where conformance fixtures do NOT replace unit tests.** The
lexer, the reader's structural correctness, the bytecode module's
constant pool integrity, and the VM's GC discipline still benefit
from GoogleTest unit tests because they exercise pieces below the
script-level interface. The split is roughly:

- **Conformance fixtures** for every user-facing feature: literals,
  special forms, primitives, macros, error behaviour.
- **GoogleTest unit tests** for sub-language internals: the lexer
  token stream, the reader's metadata attachment, the bytecode
  module's serialisation, the VM's frame layout, the GC
  discipline patterns.

Roughly: ~80% of test code is conformance fixtures, ~20% is
GoogleTest unit tests. JVM Clojure has a similar ratio (`test/`
directory plus generative tests in scripts).

The runner script and the first fixture (`hello-world.clj` →
`EXPECT: hello, world`) ship in **week 1**, so every subsequent
feature pays its test as a one-line addition. Concrete deliverable
in §10.1 below.

### Week 1 — Reader and minimum eval (Mon → Fri, ~30 hours)

**Deliverables:**

1. The CMake project skeleton (lifted from protoST).
2. The lexer (`src/reader/Lexer.h/.cpp`), covering every token in
   §3.1. ~600 lines.
3. The reader (`src/reader/Reader.h/.cpp`), producing protoCore
   objects from tokens. ~800 lines.
4. The reader macros (`src/reader/ReaderMacros.h/.cpp`): `'`, `` ` ``,
   `~`, `~@`, `#_`, `^`. Phase 1 defers `#(...)` and `#"..."` to
   week 2 — they need the compiler.
5. The minimum bytecode infrastructure
   (`src/runtime/BytecodeModule.h/.cpp` + `Opcodes.h`): ~30 opcodes
   declared, the const pool, the emit / patch / extend helpers.
   ~400 lines.
6. The minimum compiler (`src/compiler/Compiler.h/.cpp`):
   self-evaluating literals (numbers, strings, keywords, booleans,
   nil), `if`, `do`, `quote`, `def`, calls to C++ primitive
   functions. ~600 lines.
7. The minimum VM (`src/runtime/ExecutionEngine.h/.cpp`): dispatch
   loop, frame creation/destruction, the opcodes
   `PUSH_CONST`/`PUSH_NIL`/`PUSH_TRUE`/`PUSH_FALSE`/`POP`/`DUP`/`INVOKE`/`RETURN`/`JUMP_IF_*`/`THROW`.
   ~600 lines.
8. The bare minimum C++ primitives:
   `+ - * = < println str inc dec count first rest list vector`.
   ~400 lines.
9. The file runner: `main.cpp` parses `protoclj script.clj`, reads
   and compiles the file's forms one by one, runs them. ~200
   lines.
10. **The conformance suite scaffolding** (`tests/conformance/run.sh`,
    `tests/conformance/CMakeLists.txt`, the directive parser, the
    `EXPECT:` / `EXPECT-ERROR:` / `XFAIL:` directives, lifted from
    protoST's pattern). ~150 lines of bash + ~50 lines of CMake.
    Plus the first three fixtures:
    `tests/conformance/01-literals/integer.clj`,
    `tests/conformance/01-literals/string.clj`,
    `tests/conformance/01-literals/hello-world.clj`. Every feature
    landed from week 2 onward adds a fixture as it is implemented.
11. GoogleTest unit tests for the lexer and the reader's
    metadata-attachment correctness — the pieces that black-box
    fixtures cannot easily exercise. ~400 lines of GoogleTest
    (down from the ~800 originally estimated, because most of what
    the user sees moves to conformance fixtures).

**Week-1 end state:**

```
$ protoclj -e '(+ 1 2)'
3

$ protoclj -e '(println "hello, world")'
hello, world

$ cat /tmp/test.clj
(def x 42)
(println x)

$ protoclj /tmp/test.clj
42
```

Examples `01-hello.clj` runs. The other three do not yet
(`defn`, `loop`/`recur` are missing).

---

### Week 2 — Functions, let, recur, persistent collections (~30 hours)

**Deliverables:**

1. C++ bootstrap macros: `let`, `fn`, `loop`, `when`, `defn`,
   `defmacro`. ~600 lines across one .cpp per macro.
2. The special forms `let*`, `loop*`, `recur`, `fn*` in the
   compiler. ~500 lines.
3. The opcodes `PUSH_LOCAL`, `STORE_LOCAL`, `INVOKE_RECUR`,
   `MAKE_FN`, `PUSH_CAPTURED`, `STORE_CAPTURED` in the VM.
4. Closure capture: scope analysis pass + the `captured-dict`
   pattern. ~400 lines.
5. `recur` tail-position validation in the compiler.
6. Vector / map / set literal compilation: `MAKE_VECTOR`,
   `MAKE_MAP`, `MAKE_SET` opcodes + their handlers.
7. The reader macros `#(...)` and `#"..."`.
8. Persistent-collection constructor primitives:
   `list / vector / hash-map / hash-set`, plus the core access
   primitives: `get / assoc / dissoc / count / first / rest /
   conj / seq / nth / peek / pop`. ~600 lines.
9. Unit tests + the first conformance fixtures
   (`examples/01-hello.clj` and `examples/02-factorial.clj`).
10. The `defmacro` form working: a user can define a trivial
    macro and call it.

**Week-2 end state:**

```
$ protoclj examples/02-factorial.clj
2432902008176640000
```

`01-hello.clj` and `02-factorial.clj` both run. `03-fizzbuzz.clj`
does not yet (needs `cond`, `mod`, `doseq`, `range`, `case`).

---

### Week 3 — Namespaces, `core.clj`, the Clojure path (~30 hours)

**Deliverables:**

1. The C++ `ns` macro fully working: parses the `(:require [...])`
   clauses, calls into the path resolver.
2. The Clojure path resolver (`src/path/ClojurePathResolver.h/.cpp`).
   Walks `CLOJURE_PATH` + current-file dir. ~300 lines.
3. The namespace registry: a global map from name → Namespace
   object. ~150 lines.
4. The first version of `resources/clojure/core.clj`. Includes the
   re-defined bootstrap macros, the §9.1 Clojure-defined names
   (about 85). ~600 lines of Clojure.
5. Resource-embed `core.clj` into the binary (a CMake step turns
   the .clj into a C string baked into the executable).
6. Bootstrap sequencing: on startup, install C++ stuff, then
   parse-and-eval `core.clj` to install the rest.
7. The remaining v0.1 control macros in C++ that core.clj will
   then redefine. (Phase 1 stays with what C++ has; full re-define
   in Clojure happens in phase 3.)
8. `apply` (needs special support to splat a seq into argc).
9. The eager-only `map` / `filter` / `reduce` and the rest of §9.1.
10. Conformance fixtures `03-fizzbuzz.clj` (without lazy `range`,
    eager works for `(range 1 16)`) and a stub for word-count
    (without `frequencies`/`re-seq` — added in week 4).

**Week-3 end state:**

```
$ protoclj examples/03-fizzbuzz.clj
1
2
Fizz
4
Buzz
Fizz
7
8
Fizz
Buzz
11
Fizz
13
14
FizzBuzz
```

The full `core.clj` is loaded and operating. The fourth reference
script doesn't yet run because we are still missing regex
primitives and `frequencies` / `sort-by` / `take` / `vec`.

---

### Week 4 — Errors, the REPL, the demo, the benchmark (~30 hours)

**Deliverables:**

1. The `try` / `catch` / `finally` special form: compiler emission
   of `TRY_ENTER` / `TRY_EXIT`, runtime handler-stack walking on
   `THROW`. ~500 lines.
2. The exception class hierarchy: `Exception`, `Error`,
   `ArithmeticError`, `IndexError`, `KeyError`, `TypeError`,
   `ArityError` as protoCore prototypes (~150 lines).
3. The interactive REPL (`src/repl/Repl.h/.cpp`): line-by-line
   prompt, the meta-commands from §8.2, `*1` / `*2` / `*3` / `*e`,
   multi-line form continuation. ~400 lines.
4. `frequencies`, `sort-by`, `take`, `vec`, regex primitives
   (`re-pattern`, `re-find`, `re-seq`, `re-matches`).
5. `examples/04-word-count.clj` works end-to-end.
6. The phase-1 benchmark suite: a few scripts and a
   `bench/run.sh` that times them with `/usr/bin/time -v` against
   Babashka.
7. The phase-1 release artefact: `protoclj` binary + `core.clj`
   embedded, runnable on a fresh DEV12. README pointer added.

**Week-4 end state:**

The four reference scripts pass. The REPL works. The benchmark
suite produces numbers we can publish in the phase-2 plan.

```
$ protoclj
protoClojure 0.1.0
Type :help for help. Ctrl-D to exit.
user=> (+ 1 2)
3
user=> (defn square [x] (* x x))
#'user/square
user=> (map square [1 2 3 4])
[1 4 9 16]
user=> :exit

$ protoclj examples/04-word-count.clj
3 the
2 cat
2 sat
1 on
1 mat
```

---

## 11. The smoke benchmark

Run at the end of week 4 to measure where we are and decide
whether to continue with phase 2 directly or pause for profiling.

### 11.1 What to measure

For each of these:

| Script                  | What it measures                          |
|-------------------------|-------------------------------------------|
| `01-hello.clj`          | Cold-start time (read + compile + 1 print) |
| `02-factorial.clj`      | Pure tail-recursive arithmetic            |
| `03-fizzbuzz.clj`       | Control flow + string formatting          |
| `04-word-count.clj`     | Realistic mix: regex, sort, threading     |
| A 1000-iter loop summing 1..1000 | Tight-loop integer arithmetic    |
| A 1000-element vector building with `conj` | Allocation throughput   |

Measure:
- Wall-clock time (`/usr/bin/time -v`).
- Max RSS.
- For the tight-loop script: ops/sec.

### 11.2 The comparison baselines and the four-axis framework

We measure against two Clojures and four axes. The unit of comparison
is **never** "is X faster" alone; it is always the four-axis tuple
that captures the actual tradeoff a Clojure programmer is making
when they choose a runtime.

**Primary baseline: JVM Clojure** (the canonical implementation).
Comparing to JVM Clojure is what tells a Clojurist whether
protoClojure is a credible *runtime* or a toy. The single-thread
steady-state arm is where JVM Clojure is genuinely strong — twenty
five years of HotSpot JIT — and where we expect to lose.

**Secondary baseline: Babashka** (GraalVM AOT). The Clojure
programmer's "fast Clojure for CLI tools" today. Useful sanity
check: if we are catastrophically slower than Babashka, something
is structurally wrong.

**The four axes:**

| Axis                                | JVM Clojure              | protoClojure realistic           | Where the winner is   |
|-------------------------------------|--------------------------|----------------------------------|-----------------------|
| Startup (cold + first print)        | 600-1500ms               | <50ms target                     | protoClojure 10-30×   |
| Steady-state single-thread CPU      | fast (JIT inlines)       | 3-10× slower                     | JVM, **5× the target**|
| Steady-state multi-core parallel    | thread-pooled but slow   | GIL-free, real OS threads        | protoClojure 1-4×     |
| RSS footprint (long-running proc)   | 100-300MB                | 10-30MB                          | protoClojure 5-10×    |

The headline framing — for the README, conference talks, blog
posts — is **the tuple, never a single number**. The Clojure
community has the sensibility (inherited from Hickey) to value
honest multi-dimensional measurement over cherry-picked
benchmarks. Posting "fib(30) in N seconds" alone is asking to be
dismissed for missing the point.

### 11.3 The decision rule

The phase-1 → phase-2 go/no-go is read off the four axes:

- **Within target on startup AND within 5× of JVM Clojure on
  steady-state single-thread AND wining on the other two axes** →
  continue with phase 2 (the rest of `clojure.core` + laziness +
  protocols). This is the **expected** outcome and constitutes
  "success" for v0.1.
- **Beating 5× JVM on single-thread** → tag it as the public-facing
  number; continue phase 2.
- **Worse than 5× JVM on single-thread but within 10×** → continue
  phase 2 anyway; performance work is its own phase 6 milestone.
  Mark the public release as "performance preview" and explain.
- **Catastrophic on a clearly-fixable bottleneck (>10× JVM
  single-thread OR >150ms startup)** → pause phase 2, profile, fix,
  then continue. These thresholds indicate something structurally
  wrong, not just unoptimised — bad opcode dispatch, missing tagged
  immediates, GC thrashing.
- **Babashka comparison**: a separate sanity check. If we lose to
  Babashka on startup (Babashka is ~10-30ms; we should be in the
  same range or better), the AOT comparison is uncomfortable and we
  document. If we lose to Babashka by >5× on steady-state, something
  in our hot path is broken — Babashka is also a bytecode-on-JVM
  story, AOT'd; we should not be a generation behind it.

---

## 12. Definition of done for phase 1

Phase 1 is done when **all** of these hold:

1. The four reference scripts in §1.3 each run to completion with
   correct output via `protoclj <file>`.
2. The interactive REPL launches, accepts at least the meta-commands
   in §8.2, supports `*1`/`*2`/`*3`/`*e`, and survives a
   non-pathological hour of interactive use.
3. The GoogleTest unit tests in `tests/unit/` (lexer, reader,
   bytecode-module, VM internals) pass under both Release and
   ASan builds, single-threaded, with no UB or use-after-free.
4. The conformance fixtures in `tests/conformance/` — by end of
   phase 1 there should be **~80-120 fixtures** covering literals,
   special forms, control macros, collection ops, higher-order,
   arithmetic, errors, and the four reference scripts. Every one
   passes black-box through `protoclj` under Release and ASan.
5. The smoke benchmark runs to completion and produces numbers we
   are willing to publish in phase 2.
6. The `STATUS.md` is updated: every name in the phase-1 catalog
   moves from "not yet" to "implemented"; the phase-2 names are
   marked "next".
7. A `BUILD.md` documents the build procedure on DEV12 (and
   confirms the single-thread-build rule applies — protoClojure
   may be lighter than protoJS but we play safe).
8. A `phase-1-retro.md` lives in `docs/superpowers/specs/` with:
   what went as planned, what slipped, what the benchmark
   actually showed, and what the phase-2 plan needs to adjust as a
   result.

---

## 13. Revisions

(empty for now)
