# The protoClojure virtual machine

This document describes the bytecode virtual machine that runs protoClojure
programs: how source becomes bytecode, how a frame is laid out, every opcode
the compiler emits, and worked examples. Every bytecode listing in it is real
output of `protoclj --disasm`, checked by the ctest `docs/vm-examples`; none
is written by hand.

Contents:

- [Start here](#start-here)
- [1. Overview](#1-overview)
- [2. Opcode reference](#2-opcode-reference)
- [3. Worked examples](#3-worked-examples)
- [4. How to add an opcode](#4-how-to-add-an-opcode)
- [5. How to add a primitive](#5-how-to-add-a-primitive)
- [6. Known oddities](#6-known-oddities)

The call path described here is `main` after the merge of
`perf/primitives-caller-context` (2026-10-25): a primitive runs in its
caller's context (docs/DESIGN.md §10.2, M45).

## Start here

### Reading order

Read the source in the order a form travels through it:

| Step | Files | Key functions | What happens |
|---|---|---|---|
| 1. Driver | `src/main.cpp`, `src/compiler/Eval.cpp` | `runFile`, `evalTopLevel` | The script is read, compiled and run **one top-level form at a time**: a form is compiled only after every form before it has run. |
| 2. Reader | `src/reader/Lexer.cpp`, `src/reader/Reader.cpp` | `Reader::readOne` | Text becomes data: lists, vectors, maps, sets, interned symbols and keywords (`src/runtime/Named.h`). |
| 3. Macros | `src/compiler/Compiler.cpp`, `src/runtime/Macros.cpp`, `src/core/core.clj` | `Compiler::macroexpandTopLevel`, `Compiler::macroForHead`, `Compiler::compileMacroCall`, `expandMacroCall` | A list whose head names a macro is expanded at compile time by calling the macro (an ordinary fn with a flag) on the unevaluated form. Most of `clojure.core`'s macros (`when`, `cond`, `for`, `lazy-seq`, ...) are Clojure code in `core.clj`, embedded in the binary. |
| 4. Compiler | `src/compiler/Compiler.cpp`, `Compiler.h` | `Compiler::compileForm`, `compileFnForm`, `compileArity`, `compileLetForm`, `compileLoopForm`, `compileTry`, `resolveLocal`, `clearLastReads` | Special forms (`def`, `if`, `do`, `let`, `loop`, `recur`, `fn`, `defn`, `try`, `throw`, `quote`, `and`, `or`, `apply`, `defmacro`) and calls become instructions. |
| 5. Bytecode | `src/runtime/Opcodes.h`, `src/runtime/BytecodeModule.h`, `BytecodeModule.cpp` | `BytecodeModule::emit`, `patchOperand`, `addBlock`, `computeMaxStack`, `opName` | The instruction set and the module: code words, constant pool, nested fn bodies (blocks), frame metadata. |
| 6. VM | `src/runtime/ExecutionEngine.h`, `ExecutionEngine.cpp` | `ExecutionEngine::run`, `execute`, `executeFrame`, `dispatchCall` (a lambda inside `executeFrame`), `invoke` | The dispatch loop. One `ProtoContext` per frame. |
| 7. Primitives | `src/runtime/Primitives.cpp` (and `CorePrims.cpp`, `LazySeq.cpp`, `IO.cpp`, ...) | `kPrimitives`, `installPrimitives`, `fastPrimitiveCall` | Built-in functions written in C++, installed as globals. |

`src/runtime/Disassembler.cpp` prints a module; reading its `listModule`
alongside `Opcodes.h` is a quick way to learn the operand of every opcode.

### Trying things with `--disasm`

```
protoclj --disasm script.clj [args...]
```

runs the script normally and, before each of its top-level forms runs,
prints the form as it reaches the compiler (after top-level macro expansion)
and the bytecode of the form's module, then every fn body it holds, nested
ones included. `clojure.core` itself is not listed. Example 01 in section 3
shows the listing of `(+ 1 2)`; every example there is such a listing.

How to read a listing:

- `;; form N:` is the N-th top-level form of the script, printed as data.
- `module form1:` names the module. A fn body is listed after the module
  that holds it and is named by its path: `form4.0` is block 0 of form 4,
  `form4.0.1` is block 1 of that body. `MAKE_FN 0` in `form4` builds a fn
  from `form4.0`.
- The header gives the frame layout: `arity` (fixed parameters, `+ rest` for
  a variadic fn, `+ keyword args` for `& {:keys ...}`), `locals` (every other
  slot: rest, `:keys`, `:as`, captures, `let`, `loop` and `catch` locals),
  `max stack` (operand-stack capacity) and `has handlers` when the body
  contains a `try`.
- `captures:` maps each captured variable from the enclosing frame's slot to
  this frame's slot. `keyword args:` maps each `:keys` name to its slot.
- `constants:` is the constant pool: index, kind, value. Kinds are `long`,
  `double`, `bigint`, `string`, `named` (a keyword or quoted-symbol value),
  `name` (a global's name, for `PUSH_VAR` / `STORE_GLOBAL` and member names)
  and `datum` (a quoted list, vector, map, set or regex).
- `code:` has one instruction per line: position, opcode, operand, and a
  comment that decodes the operand: the constant, the role of a slot
  (`param 0`, `local`, `capture (parent slot 1)`), a jump's absolute target
  (`-> 7`), a call's argument count.

The program's own output, if any, is interleaved with the listings on
standard output.

## 1. Overview

### 1.1 Pipeline

```
source text --Reader::readOne--> form --macroexpandTopLevel--> expanded form
  --Compiler::compileForm--> BytecodeModule (+ RETURN) --ExecutionEngine::run--> value
```

`evalTopLevel` (`src/compiler/Eval.cpp`) is the single entry for the script
driver, the REPL and `eval`:

1. it expands the form while its head names a macro;
2. a resulting `(do f1 f2 ...)` is evaluated form by form, recursively, so a
   macro defined by `f1` is in effect when `f2` is compiled;
3. otherwise the form is compiled into a fresh `BytecodeModule`, an
   instruction `RETURN` is appended, the module is retained for the rest of
   the run (the fns it defines point into it), and it is run.

Macros nested inside a form are expanded while that form is compiled
(`compileMacroCall`), not before: the `;; form` line of a listing shows only
the top-level expansion.

The top level has no local slots. A top-level `let`, `loop` or `try` is
therefore compiled as the body of a zero-argument fn that is called on the
spot (`compileAsThunk`: `MAKE_FN` then `CALL 0`); see example 02.

### 1.2 Instruction encoding and the module

An instruction is one 32-bit word (`Instr`): the opcode in the low 8 bits,
an unsigned 24-bit operand in the high 24 bits (`kOperandShift = 8`,
`kMaxOperand = 16,777,215`). `BytecodeModule::emit` throws
`std::length_error` for a larger operand (a compile error). The one fixed
width lets forward jumps be back-patched in place (`patchOperand`).

- **Positions and jumps** count instruction words. A jump's operand is
  relative to the instruction **after** the jump: `JUMP`, `JUMP_IF_FALSE`,
  `JUMP_IF_TRUE` and `TRY_BEGIN` go to `pc + 1 + operand`; `JUMP_BACK` goes
  to `pc + 1 - operand`.
- **One two-word instruction:** `CALL_MEMBER` is followed by a data word,
  a `NOP` whose operand is the member name's constant index. The VM consumes
  it; it is never executed as a `NOP`.
- **Constant pool** (`BytecodeModule::Const`): kinds `Long`, `Double`,
  `String`, `Symbol` (a global or member name, interned at compile time),
  `Named` (an interned keyword or quoted symbol), `BigInteger` (decimal
  digits; the `LargeInteger` is rebuilt by each `PUSH_CONST`) and `Datum`
  (a quoted collection, pinned in a `ProtoRootSet` owned by the module).
  A constant equal to an existing one of the same kind reuses its index;
  kinds never merge, and doubles compare by bit pattern.
- **Blocks:** every `fn` body is a module of its own, owned by the module
  that contains the `fn` form (`addBlock`). `MAKE_FN`'s operand is a block
  index. A multi-arity fn is an **arity group** (`addArityGroup`): a list of
  block indices, one per arity; `MAKE_FN_MULTI`'s operand is the group index.
- **Frame metadata** of a body: `arity` (fixed parameters), `localCount`,
  `isVariadic`, `isKwBased`, `kwKeys` (name, slot, interned keyword),
  `asSlot`, `captureSpecs` (parent slot, local slot), `hasHandlers` (set when
  a `TRY_BEGIN` is emitted) and `maxStack`.
- **`maxStack`** is computed once, on first use, by a forward flow analysis
  of the finished code (`computeMaxStack`), with the stack effect of every
  opcode listed in section 2. `CALL_APPLY` spreads a list whose length is
  known only at run time; the VM grows the stack for it (`reserveStack`).
- **Debug information** (`DebugInfo`: source, fn name, line, local variable
  ranges) exists only in code compiled for the debugger (`protoclj --dap`),
  which also contains `LINE` instructions.
- **Locals clearing:** after a body is compiled, `clearLastReads` rewrites
  the single read of a local that cannot run twice on the same binding from
  `PUSH_LOCAL` into `PUSH_LOCAL_CLEAR`, so a local bound to a lazy seq does
  not hold its head while the seq is walked (Clojure's "clear on last use";
  docs/DESIGN.md §10.2, M30). Not done under the debugger.

### 1.3 Frames

`ExecutionEngine::executeFrame` runs one activation of one body. It creates
exactly **one `ProtoContext` per activation**, chained to the caller's
context through `previous` (protoCore's method-invocation model). The
context's `automaticLocals` hold the whole frame, so every value the frame
holds is in a slot the garbage collector traces:

```
slot 0 .. arity-1                     fixed parameters (bound at the call)
slot arity .. arity+localCount-1      rest parameter, :keys and :as slots,
                                      closure captures, let/loop/catch locals
slot arity+localCount ..              operand stack, capacity maxStack
```

`sp` counts the values on the operand stack. On entry the frame binds the
arguments, the `:keys` values and the `:as` map (keyword-argument fns) and
the captures (from the fn value's `__captures__` list, in `captureSpecs`
order), then polls the GC safepoint (1.6). `RETURN` answers the top of the
stack, or nil when the stack is empty; falling off the end of the code does
the same.

The frame's result must stay rooted while its context is destroyed (the
destructor submits the frame's young generation, and a collection can run
there): a caller that will store the result in one of its own slots passes
that slot (`resultSlot`), otherwise the frame anchors it through
`ProtoContext::returnValue`.

Recursion nests on the native stack: `checkNativeStack` raises
`StackOverflowError` before the thread's stack runs out. `recur` does not
nest: it is `STORE_LOCAL`s and a `JUMP_BACK` in the same frame.

Only a body that contains a `try` runs in `executeFrame<true>`, the
instantiation with a C++ `try`/`catch` around the dispatch loop; every other
body runs in `executeFrame<false>`, which has none.

### 1.4 Function values

A fn value is an immutable protoCore object built by `MAKE_FN` or
`MAKE_FN_MULTI`; it is never mutated after construction.

| Kind | Prototype | Attributes |
|---|---|---|
| single arity | `fnSingleProto` | `__bytecode__`: the address of the body's `BytecodeModule`, stored as an integer; `__captures__`: a list of the captured values, present only when the body has captures |
| multi arity | `fnMultiProto` | `__arities__`: a list of 4-element lists `(arity, module address, variadic?, captures-list)`, one per arity, in source order |

- **Captures** are copied at `MAKE_FN` time: the compiler pushes each captured
  local of the enclosing frame (`PUSH_LOCAL parentSlot`) right before
  `MAKE_FN`, which pops them into the list. A capture is a value copy; the
  language has no mutable locals, so copying is exact. A variable captured
  through several nested fns is captured by each intermediate fn
  (`Compiler::resolveLocal`).
- **Keyword-argument fns** (`& {:keys [a b] :or {...} :as m}`) are ordinary
  single-arity fns whose module has `isKwBased` set; `:or` defaults compile
  to a prologue at the start of the body (example 09).
- **Variadic fns** (`& rest`) receive their rest arguments as one list (nil
  when there are none).
- A call binds at most 17 parameters (a variadic fn's rest counts as one),
  and a fn declares at most 16 `:keys`.

Keywords, quoted symbols, maps, sets and vectors are also callable (1.5).

### 1.5 Calling conventions

`CALL n` expects `[callable, arg1 .. argn]` on the stack (callable deepest)
and replaces them with the result. The dispatch (`dispatchCall` in
`executeFrame`) tries, in order:

1. **Fast primitive path.** A protoCore method value (every primitive) for
   which `fastPrimitiveCall` has a direct answer (`inc`, `dec`, two-argument
   `+`, `first`, `rest`, `next`, `seq`, `empty?`, `count`, `nth`, `cons`,
   `conj` on the values they read without running Clojure code): answered
   in the caller's frame, **no context created**. What it allocates is the
   caller frame's garbage.
2. **User fn** (prototype `fnSingleProto` or `fnMultiProto`). First the
   caller frame's young chain is handed to the collector
   (`submitBeforeCall`; 1.6). Then it picks the arity (multi: exact fixed
   arity first, then a variadic arity whose fixed part `n` covers, then a
   keyword-argument arity whose fixed part is followed by key/value pairs or
   one map), folds key/value pairs into the kwArgs map for a
   keyword-argument arity, checks the count, builds the rest list, moves the
   arguments out of their stack slots (clearing the slots, so a lazy seq
   passed down is held by the callee alone) and runs the body in a new frame
   (1.3).
3. **Lookup.** A keyword or quoted symbol called on a map or set, or a map or
   set called on a key, with an optional not-found: the semantics of `get`
   (`callLookup`). One or two arguments.
4. **General primitive path.** `asMethod` gives the C function; it is called
   **in the frame itself, with no context of its own**, a `ProtoArguments`
   view over the argument slots of the frame's operand stack (protoCore's
   array call form; nothing is copied), and a `PrimitiveWindow` over the
   frame's free slots above the arguments, from which the primitive takes
   `CallerSlots` (1.6). The argument slots are cleared afterwards.
5. A **vector** called on an index: `nth` without default.
6. Anything else raises `VM: value is not callable in CALL`.

Other call opcodes:

- **`CALL_KW n`** is emitted for a call whose source ends in
  `:keyword value` pairs. If the callee is a single-arity keyword-argument
  fn, the VM folds the trailing pairs into a map (last duplicate wins) and
  passes it as one extra trailing argument; any other callee receives the
  `n` arguments unchanged, exactly as with `CALL` (a multi-arity fn's
  keyword-argument arity folds them in step 2).
- **`CALL_APPLY`** (`(apply f args)`): spreads a list, vector, nil, map, set
  or string onto the stack, then dispatches as `CALL`. `(apply f x y coll)`
  first builds the list with `clojure.core/list*`.
- **`CALL_MEMBER n`** (`(alias/member args...)`, a module export or a member
  of a global object such as `System`): reads the member from the receiver
  (prototype chain included). A protoCore method is called with the receiver
  as `self`, in the frame, with a `PrimitiveWindow` like step 4; any other
  value is called through `ExecutionEngine::invoke`.
- **`ExecutionEngine::invoke`** is the same dispatch for C++ code that calls
  a Clojure callable (`reduce`, `map`, `swap!`, ...). A primitive callee
  runs in the context `invoke` was given, with no context of its own; its
  caller roots `callable` and the arguments. It counts as a nested
  activation on the current `PrimitiveWindow`, so it never submits a young
  chain its caller relies on (1.6).

### 1.6 Safepoints and rooting

protoCore reclaims nothing a context has not handed over: a context's
allocations are chained on its young generation, which is submitted to the
collector only when the context is destroyed or from
`ProtoContext::safepoint()`. The VM polls `safepoint()` at exactly two
points, where every live value is in a traced frame slot:

- **frame entry** (after the arguments and captures are bound), so that an
  allocation-free recursion still parks for a stop-the-world;
- **`JUMP_BACK`** (every `recur`), because a `loop` is one frame and its
  garbage would otherwise never reach the collector.

Two more submissions, not strided:

- **Before a call of a Clojure fn** (`submitBeforeCall`), when the frame
  allocated since its last submission. The frame is suspended for the call
  with every value in its slots; primitives allocate in their caller's
  frame, and a young chain is a root, so without it a lazy seq built here
  and walked by the callee would stay alive, every element realised, until
  the call returns (locals clearing, M30).
- **Inside a primitive**, through `CallerSlots::tick()` / `safepoint()`:
  only for the frame's outermost primitive activation (depth 1 of the
  `PrimitiveWindow`), only while its body holds a `SafepointPermit` (its
  promise that everything it holds across a call is in `CallerSlots` or its
  arguments), and only while no `SafepointBlock` is held (values parked in
  C++ storage, such as the JSON reader's batch of object entries). `reduce`,
  `count`, `last`, `dorun`, `doall`, `run!`, the JSON reader and
  `CALL_APPLY`'s realisation of a lazy seq hold a permit.

Both are strided: one real poll per 64 events per thread
(`kGcSafepointStride`). `PROTOCLJ_NO_GC_SAFEPOINT=1` disables the hook (for
measurements only). See the comment on `ExecutionEngine::gcSafepoint`.

Rooting inside the VM follows protoCore's EMBEDDER-CONFORMANCE rule 3 (no
`ProtoObject*` held only in a C++ local across an allocation or a
safepoint): operands stay in their stack slots until the result replaces
them, and every opcode that allocates (`PUSH_CONST` of a string, the
arithmetic slow path, `MAKE_FN`, calls) reads its inputs from slots.

### 1.7 Exceptions

A frame of a body with a `try` keeps a vector of active handlers, each a
code position and the operand-stack depth at `TRY_BEGIN`. When a C++
exception escapes an instruction (raised by `THROW`, a primitive, or a
nested call that has already unwound), the innermost handler is popped, the
stack is cut back to its depth, the exception value is pushed and execution
resumes at the handler. A `proto::ProtoException` (every language's error,
protoCore rule 14) pushes its value; any other `std::exception` (a module
defect, `StackOverflowError`) is converted to a classed exception value. With
no active handler the exception leaves the frame, rethrown after the C++
catch clause completes (required for MSVC's unwinding).

The handler's first instruction is `STORE_CAUGHT slot`, which stores the
value and keeps the caught C++ exception object. Catch clauses test the
value with `EXC_MATCH` (a built-in class) or `EXC_MATCH_TYPE` (a type
resolved at run time). `RETHROW slot` raises the original exception object
again when no clause matches and at the end of a `finally` handler. The
layout compiled by `Compiler::compileTry`:

```
      TRY_BEGIN Lfinally          ; only with a finally clause
      TRY_BEGIN Lcatch            ; only with catch clauses
      <body>
      TRY_END
      JUMP Ldone
Lcatch:                           ; stack: the exception
      STORE_CAUGHT ex
      PUSH_LOCAL ex, EXC_MATCH C1, JUMP_IF_FALSE L2, <catch body 1>, JUMP Ldone
L2:   ...
      RETHROW ex                  ; no clause matched
Ldone:
      TRY_END                     ; the finally handler
      <finally forms>, POP        ; normal path
      JUMP Lend
Lfinally:                         ; stack: the exception
      STORE_CAUGHT fx
      <finally forms>, POP
      RETHROW fx
Lend:
```

The finally forms are compiled twice, once per path. `recur` across a `try`
is a compile error.

## 2. Opcode reference

There are **41 opcodes**, values 0 to 40 (`src/runtime/Opcodes.h`). The enum,
the name table (`opName`), the stack analysis (`computeMaxStack`) and the
VM's switch (`executeFrame`) agree: each lists the same 41. The compiler
emits all 41, with three qualifications: `NOP` only as `CALL_MEMBER`'s data
word, `PUSH_LOCAL_CLEAR` only by rewriting a `PUSH_LOCAL`, and `LINE` only
in code compiled for the debugger.

Notation: the stack effect is `before -> after`, top of stack on the right;
`v` is any value. "Errors" lists what the instruction itself raises; a
`VM: ...` message is an internal error, impossible in compiler output.

### 2.1 Constants

| Op | # | Operand | Stack | Semantics | Emitted by |
|---|---|---|---|---|---|
| `PUSH_CONST` | 1 | constant index | `-> v` | Pushes the constant: a long or double as a protoCore number; a string as a new protoCore string (allocates each time); a bigint rebuilt from its digits (allocates); a keyword, quoted symbol or quoted datum as the very interned/pinned object. A `Symbol` constant raises `VM: PUSH_CONST of a global name`. | number, string, keyword literals; `quote`; `()`; constant sets `#{:a 1}`; regex literals |
| `PUSH_NIL` | 9 | reserved (0) | `-> nil` | Pushes nil (`PROTO_NONE`). | `nil`; implicit nil of `if` without else, empty `do`/`let`/`fn` bodies, `(or)` |
| `PUSH_TRUE` | 10 | reserved (0) | `-> true` | | `true`; `(and)` |
| `PUSH_FALSE` | 11 | reserved (0) | `-> false` | | `false` |

### 2.2 Locals and captures

| Op | # | Operand | Stack | Semantics | Emitted by |
|---|---|---|---|---|---|
| `PUSH_LOCAL` | 12 | slot | `-> v` | Pushes the frame slot (a parameter, capture or local). | a local symbol; before `MAKE_FN` for each capture |
| `PUSH_LOCAL_CLEAR` | 39 | slot | `-> v` | As `PUSH_LOCAL`, then clears the slot (locals clearing, 1.2). | `clearLastReads` rewriting the last read of a local |
| `STORE_LOCAL` | 13 | slot | `v ->` | Pops into the slot. | each `let`/`loop` binding; `recur` (stores in reverse order); `:or` defaults |

### 2.3 Globals and vars

protoClojure has no Var objects (deviation D28): a global is an own attribute
of the mutable globals object, keyed by an interned symbol.

| Op | # | Operand | Stack | Semantics | Emitted by |
|---|---|---|---|---|---|
| `PUSH_VAR` | 2 | `name` constant | `-> v` | Pushes the global. Errors: `VM: unable to resolve symbol: x` when it is not defined. A global bound to nil pushes nil. | a non-local symbol; the head of a call; `hash-map` / `vector` / `hash-set` / `list*` / `__macro__` helper calls; the receiver `a` of `a/b` when `a` is not a local |
| `STORE_GLOBAL` | 6 | `name` constant | `v -> v` | Sets the global to the top of the stack and **leaves it there** as the value of `def`. While `core.clj` loads, each store is emitted twice, the second under `clojure.core/name`. | `def`, `defn`, `defmacro` |

### 2.4 Control flow

| Op | # | Operand | Stack | Semantics | Emitted by |
|---|---|---|---|---|---|
| `JUMP` | 7 | forward offset | `->` | `pc += operand` (from the next instruction). | `if` (over the else branch), `try` (to the end) |
| `JUMP_IF_FALSE` | 8 | forward offset | `v ->` | Pops; jumps when the value is nil or false. | `if`, `and`, `cond` (via `if`), catch clause tests, `:or` prologue |
| `JUMP_IF_TRUE` | 18 | forward offset | `v ->` | Pops; jumps when the value is neither nil nor false. | `or` |
| `JUMP_BACK` | 15 | backward offset | `->` | `pc -= operand` (from the next instruction), then polls the GC safepoint (1.6). | `recur` |
| `DUP` | 17 | reserved (0) | `v -> v v` | Duplicates the top. | `and`, `or` (keep the tested value as the result) |
| `POP` | 4 | reserved (0) | `v ->` | Drops the top. | between the forms of a `do` / body; after `finally` forms |
| `NOP` | 0 | data | `->` | Does nothing. Emitted only as `CALL_MEMBER`'s data word, which the VM consumes and never executes. | `CALL_MEMBER` |

### 2.5 Calls and returns

| Op | # | Operand | Stack | Semantics | Emitted by |
|---|---|---|---|---|---|
| `CALL` | 3 | argc | `f a1..an -> r` | Calls `f` (1.5). Errors: arity mismatch (`VM: fn arity mismatch (expected 1, got 2)`), not callable, a lookup with other than 1 or 2 arguments, more than 17 bound parameters. | every call form without a trailing `:k v` suffix; literals of maps, vectors and non-constant sets; top-level thunks (`CALL 0`); `defmacro` (`__macro__`) |
| `CALL_KW` | 28 | argc | `f a1..an -> r` | As `CALL`, but a single-arity keyword-argument callee gets its trailing key/value pairs folded into a map (1.5). Errors: `VM: keyword-argument fn expects N positional arguments followed by key/value pairs`. | a call whose arguments end in one or more `:keyword value` pairs |
| `CALL_APPLY` | 16 | reserved (0) | `f coll -> r` | Spreads `coll` onto the stack and dispatches as `CALL`. | `apply` |
| `CALL_MEMBER` | 37 | argc; **next word**: `NOP` with the member's `name` constant | `recv a1..an -> r` | Calls member `b` of the receiver (1.5). Errors: `RuntimeException: No such member: b`. | `(a/b args...)` where `a` is not `clojure.core`, `user` or an aliased library namespace |
| `GET_MEMBER` | 36 | `name` constant | `obj -> v` | Reads attribute `b` of the object, prototype chain included. Errors: `RuntimeException: No such member: b (<type>)`. | a value reference `a/b`; a catch type `a/T` with `a` a local |
| `RETURN` | 5 | reserved (0) | `v ->` | Ends the frame with the top of the stack, or nil when the stack is empty. | the end of every fn body and of every top-level module (`evalTopLevel`) |

### 2.6 Closures

| Op | # | Operand | Stack | Semantics | Emitted by |
|---|---|---|---|---|---|
| `MAKE_FN` | 14 | block index | `c1..ck -> fn` | Pops the block's `k` captures (in `captureSpecs` order) and pushes a single-arity fn value (1.4). Allocates. | `fn`, `defn` with one arity; top-level thunks |
| `MAKE_FN_MULTI` | 19 | arity-group index | `c1..ck -> fn` | Pops the captures of every arity (arity 0's first) and pushes a multi-arity fn value. Allocates. | `fn`, `defn` with several `([params] body)` arities |

### 2.7 Arithmetic and comparison fast paths

| Op | # | Operand | Stack | Semantics |
|---|---|---|---|---|
| `ADD` | 20 | 0, or 1 for `clojure.core/` | `a b -> a+b` | |
| `SUB` | 21 | 0, or 1 for `clojure.core/` | `a b -> a-b` | |
| `MUL` | 22 | 0, or 1 for `clojure.core/` | `a b -> a*b` | |
| `LT` | 23 | 0, or 1 for `clojure.core/` | `a b -> bool` | |
| `LE` | 24 | 0, or 1 for `clojure.core/` | `a b -> bool` | |
| `GT` | 25 | 0, or 1 for `clojure.core/` | `a b -> bool` | |
| `GE` | 26 | 0, or 1 for `clojure.core/` | `a b -> bool` | |
| `EQ` | 27 | 0, or 1 for `clojure.core/` | `a b -> bool` | |

Semantics, common to all eight:

- **Fast path:** both operands are tagged SmallIntegers. The result is
  computed inline, with no call and no allocation. `ADD`, `SUB` and `MUL`
  fall back to protoCore's promoting `add` / `subtract` / `multiply` when the
  result leaves the 53-bit SmallInteger range (`MUL` also when an operand is
  outside +-2^26), producing a `LargeInteger`.
- **Slow path:** any other operands. `ADD` .. `GE` require numbers and raise
  `ClassCastException: + expects a number, got <type>` otherwise; numbers go
  through protoCore's `add` / `subtract` / `multiply` / `partialCompare`
  (exact across SmallInteger, LargeInteger and double; IEEE for NaN). `EQ`
  accepts any values and uses `valuesEqual`, the `=` primitive's equality.
- **Redefinition:** while the global of the name holds something other than
  the built-in (`(def + -)` after the form was compiled), the opcode calls
  the global with the two operands, as `PUSH_VAR` + `CALL 2` would. Operand
  1 marks a form that named `clojure.core/+` (and the like), which always
  computes the built-in. A `(+ a b c)` compiled before the redefinition
  calls the new value pairwise (STATUS D61).
- **Emitted by:** a call whose head is `+`, `-`, `*`, `<`, `<=`, `>`, `>=`
  or `=` (or `clojure.core/+`, ...) when the name is not a local binding and
  its global holds the built-in at compile time:
  `+`, `-`, `*` with two or more arguments fold left (`(+ a b c)` is
  `a b ADD c ADD`); comparisons only with exactly two arguments. One
  argument, chained comparisons and a locally shadowed operator compile to
  an ordinary `CALL` of the global. `EQ` is also emitted by the `:or`
  prologue (`slot = nil`).

### 2.8 Exceptions

| Op | # | Operand | Stack | Semantics | Emitted by |
|---|---|---|---|---|---|
| `TRY_BEGIN` | 29 | forward offset to the handler | `->` | Pushes a handler (target, current stack depth); 1.7. Raises `VM: TRY_BEGIN in a frame without handlers` if the module was not marked (impossible: `emit` marks it). | `try` with catch clauses, and again with a `finally` |
| `TRY_END` | 30 | reserved (0) | `->` | Pops the innermost handler (normal exit). | end of a `try` body; start of the normal-path `finally` |
| `THROW` | 31 | reserved (0) | `v ->` (never continues) | Raises the value. A value that is neither a Clojure exception nor a foreign object raises `ClassCastException: throw expects an exception, got <type>`. The value stays in its slot until the `ProtoException` has pinned it. | `throw` |
| `STORE_CAUGHT` | 38 | slot | `ex ->` | First instruction of a handler: pops the exception into the slot and keeps the caught C++ exception object for `RETHROW`. | `try` (catch and finally handlers) |
| `EXC_MATCH` | 32 | built-in class id (`Exceptions.h`) | `ex -> bool` | True when the exception's class is that class or a subclass (`exceptionMatches`). `Object` and `Throwable` match everything. | a catch clause naming a built-in class (`Exception`, `ArithmeticException`, `clojure.lang.ExceptionInfo`, ...) |
| `RESOLVE_CLASS` | 35 | `name` constant | `-> type` | Pushes the catch type the name denotes at run time: the global `X`, or member `b` of the global `a` for `a/b`. Errors: `RuntimeException: Unable to resolve classname: X`. | a catch clause naming a non-built-in type that is not a local |
| `EXC_MATCH_TYPE` | 34 | `name` constant (the clause's type name, for messages) | `ex type -> bool` | True when `type` is on the exception value's prototype chain. A `type` that is not a prototype object raises `IllegalArgumentException: catch: X is <type>, not an exception type`. | a catch clause naming a non-built-in type (after `RESOLVE_CLASS`, `PUSH_LOCAL` or `PUSH_LOCAL` + `GET_MEMBER`) |
| `RETHROW` | 33 | slot | `->` (never continues) | Rethrows the original C++ exception kept by `STORE_CAUGHT` for that slot (`std::rethrow_exception`, so a runtime subclass of `ProtoException` keeps its type); for a translated C++ error, raises the slot's value. Reads no stack. | end of the catch clauses (no match); end of a `finally` handler |

### 2.9 Collections

There are **no collection opcodes**. Literals compile to calls of the
built-ins, so their elements are evaluated left to right like any arguments:

- `[a b c]` -> `PUSH_VAR vector`, the elements, `CALL 3`;
- `{:k v}` -> `PUSH_VAR hash-map`, keys and values in source order, `CALL 2`;
- `#{a b}` -> `PUSH_VAR clojure.core/hash-set`, the elements, `CALL n`; a set
  of constants is one `PUSH_CONST` of the set the reader built;
- quoted collections, `'(a b)`, `'[1 2]`, are `PUSH_CONST` of a datum.

Lookups are calls too: `(:k m)`, `(m :k)`, `(get m :k)` (example 10).

### 2.10 Debugging

| Op | # | Operand | Stack | Semantics | Emitted by |
|---|---|---|---|---|---|
| `LINE` | 40 | source line, or 0 | `->` | The frame enters that line; the debugger may stop here (`DebugHook::onLine`). Operand 0, emitted before a loop's `JUMP_BACK`, forgets the current line so the next iteration enters its first line again. | only code compiled for the debugger (`protoclj --dap`, `SourceInfo::recordPositions`): one per form with a recorded position and one at each fn body's entry |

`--disasm` compiles without debug information, so no listing below contains
`LINE`.

## 3. Worked examples

Each example is a file in `docs/vm-examples/`; the listing after it is that
file's golden output. Regenerate them with
`python3 docs/vm-examples/disasm_examples.py build/protoclj --update` after a
compiler change, and review the notes.

### Example 01: a constant and arithmetic, `(+ 1 2)`

<!-- vm-example: 01-arithmetic -->
```clojure
(+ 1 2)
```

```text
;; form 1: (+ 1 2)
module form1: arity 0, locals 0, max stack 2
  constants:
    #0   long    1
    #1   long    2
  code:
    0  PUSH_CONST       0         ; 1
    1  PUSH_CONST       1         ; 2
    2  ADD
    3  RETURN
```

- `0`-`1` push the two constants. Stack: `1`, then `1 2`.
- `2 ADD` pops both and pushes `3`. Both are SmallIntegers, so the fast path
  adds inline: no call, no allocation.
- `3 RETURN` ends the top-level module; `evalTopLevel` appended it.
- `max stack 2`: the deepest point is after instruction 1.

### Example 02: `let`

<!-- vm-example: 02-let -->
```clojure
(let [x 10
      y (* x 2)]
  (+ x y))
```

```text
;; form 1: (let [x 10 y (* x 2)] (+ x y))
module form1: arity 0, locals 0, max stack 1
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  CALL             0         ; 0 args
    2  RETURN

module form1.0: arity 0, locals 2, max stack 2
  constants:
    #0   long    10
    #1   long    2
  code:
     0  PUSH_CONST       0        ; 10
     1  STORE_LOCAL      0        ; local
     2  PUSH_LOCAL       0        ; local
     3  PUSH_CONST       1        ; 2
     4  MUL
     5  STORE_LOCAL      1        ; local
     6  PUSH_LOCAL       0        ; local
     7  PUSH_LOCAL_CLEAR 1        ; local
     8  ADD
     9  RETURN
```

- The top level has no local slots, so the `let` becomes a zero-argument fn
  `form1.0`, built by `MAKE_FN 0` and called at once by `CALL 0` (1.1).
- In `form1.0`, each binding is "compute, `STORE_LOCAL`": `x` is slot 0,
  `y` is slot 1 (`locals 2`). `y`'s init reads `x` (`2`), as `let` is
  sequential.
- `7 PUSH_LOCAL_CLEAR 1`: the only read of `y`, so locals clearing empties
  the slot as it reads it. `x` is read twice and stays a `PUSH_LOCAL`.

### Example 03: `if`, `when`, `cond`

<!-- vm-example: 03-conditionals -->
```clojure
(defn sign-word [n]
  (if (< n 0) :negative :non-negative))

(defn inc-when [x]
  (when x (inc x)))

(defn classify [n]
  (cond (< n 0) :negative
        (= n 0) :zero
        :else   :positive))
```

```text
;; form 1: (defn sign-word [n] (if (< n 0) :negative :non-negative))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    sign-word
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  STORE_GLOBAL     0         ; sign-word
    2  RETURN

module form1.0: arity 1, locals 0, max stack 2
  constants:
    #0   long    0
    #1   named   :negative
    #2   named   :non-negative
  code:
    0  PUSH_LOCAL_CLEAR 0         ; param 0
    1  PUSH_CONST       0         ; 0
    2  LT
    3  JUMP_IF_FALSE    2         ; -> 6
    4  PUSH_CONST       1         ; :negative
    5  JUMP             1         ; -> 7
    6  PUSH_CONST       2         ; :non-negative
    7  RETURN

;; form 2: (defn inc-when [x] (when x (inc x)))
module form2: arity 0, locals 0, max stack 1
  constants:
    #0   name    inc-when
  code:
    0  MAKE_FN          0         ; fn form2.0
    1  STORE_GLOBAL     0         ; inc-when
    2  RETURN

module form2.0: arity 1, locals 0, max stack 2
  constants:
    #0   name    inc
  code:
    0  PUSH_LOCAL       0         ; param 0
    1  JUMP_IF_FALSE    4         ; -> 6
    2  PUSH_VAR         0         ; inc
    3  PUSH_LOCAL       0         ; param 0
    4  CALL             1         ; 1 arg
    5  JUMP             1         ; -> 7
    6  PUSH_NIL
    7  RETURN

;; form 3: (defn classify [n] (cond (< n 0) :negative (= n 0) :zero :else :positive))
module form3: arity 0, locals 0, max stack 1
  constants:
    #0   name    classify
  code:
    0  MAKE_FN          0         ; fn form3.0
    1  STORE_GLOBAL     0         ; classify
    2  RETURN

module form3.0: arity 1, locals 0, max stack 2
  constants:
    #0   long    0
    #1   named   :negative
    #2   named   :zero
    #3   named   :positive
  code:
     0  PUSH_LOCAL       0        ; param 0
     1  PUSH_CONST       0        ; 0
     2  LT
     3  JUMP_IF_FALSE    2        ; -> 6
     4  PUSH_CONST       1        ; :negative
     5  JUMP             7        ; -> 13
     6  PUSH_LOCAL       0        ; param 0
     7  PUSH_CONST       0        ; 0
     8  EQ
     9  JUMP_IF_FALSE    2        ; -> 12
    10  PUSH_CONST       2        ; :zero
    11  JUMP             1        ; -> 13
    12  PUSH_CONST       3        ; :positive
    13  RETURN
```

- `sign-word`: test, `JUMP_IF_FALSE` to the else branch (`3 -> 6`), then
  branch, `JUMP` over the else branch (`5 -> 7`). Both branches leave one
  value, so the stack depth agrees at `7`.
- `inc-when`: `when` is a macro (`core.clj`) that expands to
  `(if x (do (inc x)))`; the missing else branch is `6 PUSH_NIL`. The
  macro is expanded while `defn` is compiled, so `;; form 2` still shows
  `when`. `(inc x)` is an ordinary `CALL` of the global `inc`; at run time
  the VM answers it on the fast primitive path (1.5).
- `classify`: `cond` expands to nested `if`s. The final `:else` test is a
  literal keyword, so the compiler folds `(if :else :positive ...)` to
  `:positive` (`12`) with no test at all. `(= n 0)` is `EQ`.

### Example 04: `defn`, and a call of it

<!-- vm-example: 04-defn-call -->
```clojure
(defn square [x]
  (* x x))

(square 7)
```

```text
;; form 1: (defn square [x] (* x x))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    square
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  STORE_GLOBAL     0         ; square
    2  RETURN

module form1.0: arity 1, locals 0, max stack 2
  code:
    0  PUSH_LOCAL       0         ; param 0
    1  PUSH_LOCAL       0         ; param 0
    2  MUL
    3  RETURN

;; form 2: (square 7)
module form2: arity 0, locals 0, max stack 2
  constants:
    #0   name    square
    #1   long    7
  code:
    0  PUSH_VAR         0         ; square
    1  PUSH_CONST       1         ; 7
    2  CALL             1         ; 1 arg
    3  RETURN
```

- `form1`: `defn` is `MAKE_FN` (a fn value for body `form1.0`) and
  `STORE_GLOBAL square`. `STORE_GLOBAL` leaves the value on the stack, so
  `RETURN` answers the fn, the value of the `def`.
- `form1.0`: `arity 1`; the parameter is slot 0. `(* x x)` is `MUL`.
- `form2`: callee first (`PUSH_VAR square`), then the arguments, then
  `CALL 1`. Stack before the call: `square 7`; after: `49`. The call runs
  `form1.0` in a new frame with its own `ProtoContext`.

### Example 05: a recursive fn, `fib`

<!-- vm-example: 05-fib -->
```clojure
(defn fib [n]
  (if (< n 2)
    n
    (+ (fib (- n 1)) (fib (- n 2)))))

(fib 10)
```

```text
;; form 1: (defn fib [n] (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    fib
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  STORE_GLOBAL     0         ; fib
    2  RETURN

module form1.0: arity 1, locals 0, max stack 4
  constants:
    #0   long    2
    #1   name    fib
    #2   long    1
  code:
     0  PUSH_LOCAL       0        ; param 0
     1  PUSH_CONST       0        ; 2
     2  LT
     3  JUMP_IF_FALSE    2        ; -> 6
     4  PUSH_LOCAL       0        ; param 0
     5  JUMP             11       ; -> 17
     6  PUSH_VAR         1        ; fib
     7  PUSH_LOCAL       0        ; param 0
     8  PUSH_CONST       2        ; 1
     9  SUB
    10  CALL             1        ; 1 arg
    11  PUSH_VAR         1        ; fib
    12  PUSH_LOCAL       0        ; param 0
    13  PUSH_CONST       0        ; 2
    14  SUB
    15  CALL             1        ; 1 arg
    16  ADD
    17  RETURN

;; form 2: (fib 10)
module form2: arity 0, locals 0, max stack 2
  constants:
    #0   name    fib
    #1   long    10
  code:
    0  PUSH_VAR         0         ; fib
    1  PUSH_CONST       1         ; 10
    2  CALL             1         ; 1 arg
    3  RETURN
```

- The recursive call looks `fib` up as a global each time (`6`, `11
  PUSH_VAR fib`): a `defn` body refers to itself through the global, as
  Clojure's does through its Var.
- Stack at `16 ADD`: `fib(n-1) fib(n-2)`. `max stack 4` is reached inside
  the second call's argument computation: `fib(n-1) fib n 2`.
- `n` is read three times, so none of its reads is a clearing read.
- Each `CALL 1` nests a native frame: deep non-tail recursion ends in
  `StackOverflowError` (1.3), unlike `recur`.

### Example 06: `loop` / `recur`

<!-- vm-example: 06-loop-recur -->
```clojure
(defn sum-to [n]
  (loop [i 0 acc 0]
    (if (> i n)
      acc
      (recur (inc i) (+ acc i)))))

(sum-to 10)
```

```text
;; form 1: (defn sum-to [n] (loop [i 0 acc 0] (if (> i n) acc (recur (inc i) (+ acc i)))))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    sum-to
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  STORE_GLOBAL     0         ; sum-to
    2  RETURN

module form1.0: arity 1, locals 2, max stack 3
  constants:
    #0   long    0
    #1   name    inc
  code:
     0  PUSH_CONST       0        ; 0
     1  STORE_LOCAL      1        ; local
     2  PUSH_CONST       0        ; 0
     3  STORE_LOCAL      2        ; local
     4  PUSH_LOCAL       1        ; local
     5  PUSH_LOCAL       0        ; param 0
     6  GT
     7  JUMP_IF_FALSE    2        ; -> 10
     8  PUSH_LOCAL       2        ; local
     9  JUMP             9        ; -> 19
    10  PUSH_VAR         1        ; inc
    11  PUSH_LOCAL       1        ; local
    12  CALL             1        ; 1 arg
    13  PUSH_LOCAL       2        ; local
    14  PUSH_LOCAL       1        ; local
    15  ADD
    16  STORE_LOCAL      2        ; local
    17  STORE_LOCAL      1        ; local
    18  JUMP_BACK        15       ; -> 4
    19  RETURN

;; form 2: (sum-to 10)
module form2: arity 0, locals 0, max stack 2
  constants:
    #0   name    sum-to
    #1   long    10
  code:
    0  PUSH_VAR         0         ; sum-to
    1  PUSH_CONST       1         ; 10
    2  CALL             1         ; 1 arg
    3  RETURN
```

- `0`-`3` bind the loop locals `i` (slot 1) and `acc` (slot 2), after the
  parameter `n` (slot 0).
- `4` is the loop head, the `recur` target.
- `10`-`15` compute the new values left to right: stack `(inc i) (+ acc
  i)`; `16`-`17` store them in **reverse** order, so the top of the stack
  goes to the last local.
- `18 JUMP_BACK 15` goes to `19 - 15 = 4` and polls the GC safepoint
  (strided, 1.6). The loop never grows the native stack.

### Example 07: a closure capturing a local

<!-- vm-example: 07-closure -->
```clojure
(defn make-adder [n]
  (fn [x] (+ x n)))

((make-adder 5) 10)
```

```text
;; form 1: (defn make-adder [n] (fn [x] (+ x n)))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    make-adder
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  STORE_GLOBAL     0         ; make-adder
    2  RETURN

module form1.0: arity 1, locals 0, max stack 1
  code:
    0  PUSH_LOCAL_CLEAR 0         ; param 0
    1  MAKE_FN          0         ; fn form1.0.0
    2  RETURN

module form1.0.0: arity 1, locals 1, max stack 2
  captures: parent slot 0 -> slot 1
  code:
    0  PUSH_LOCAL_CLEAR 0         ; param 0
    1  PUSH_LOCAL       1         ; capture (parent slot 0)
    2  ADD
    3  RETURN

;; form 2: ((make-adder 5) 10)
module form2: arity 0, locals 0, max stack 2
  constants:
    #0   name    make-adder
    #1   long    5
    #2   long    10
  code:
    0  PUSH_VAR         0         ; make-adder
    1  PUSH_CONST       1         ; 5
    2  CALL             1         ; 1 arg
    3  PUSH_CONST       2         ; 10
    4  CALL             1         ; 1 arg
    5  RETURN
```

- `form1.0`: `0 PUSH_LOCAL_CLEAR 0` pushes `n`, the capture, and `1 MAKE_FN
  0` pops it into the new fn's `__captures__` list. It is `n`'s last read
  in `make-adder`, hence the clearing read.
- `form1.0.0`: `captures: parent slot 0 -> slot 1`: on entry the frame copies
  the captured value into its slot 1, after the parameter `x` (slot 0). The
  body then reads it like any local; capture slots are never cleared.
- `form2`: the callee is itself a call's result: `CALL 1` builds the adder,
  and the second `CALL 1` calls it.

### Example 08: multi-arity

<!-- vm-example: 08-multi-arity -->
```clojure
(defn greet
  ([] (greet "world"))
  ([who] (str "hello, " who)))

(greet)
```

```text
;; form 1: (defn greet ([] (greet "world")) ([who] (str "hello, " who)))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    greet
  arity group 0: blocks 0 1
  code:
    0  MAKE_FN_MULTI    0         ; multi-arity fn: form1.0 form1.1
    1  STORE_GLOBAL     0         ; greet
    2  RETURN

module form1.0: arity 0, locals 0, max stack 2
  constants:
    #0   name    greet
    #1   string  "world"
  code:
    0  PUSH_VAR         0         ; greet
    1  PUSH_CONST       1         ; "world"
    2  CALL             1         ; 1 arg
    3  RETURN

module form1.1: arity 1, locals 0, max stack 3
  constants:
    #0   name    str
    #1   string  "hello, "
  code:
    0  PUSH_VAR         0         ; str
    1  PUSH_CONST       1         ; "hello, "
    2  PUSH_LOCAL_CLEAR 0         ; param 0
    3  CALL             2         ; 2 args
    4  RETURN

;; form 2: (greet)
module form2: arity 0, locals 0, max stack 1
  constants:
    #0   name    greet
  code:
    0  PUSH_VAR         0         ; greet
    1  CALL             0         ; 0 args
    2  RETURN
```

- Each arity is its own block (`form1.0` for `[]`, `form1.1` for `[who]`),
  grouped by `arity group 0: blocks 0 1`. `MAKE_FN_MULTI 0` builds one fn
  value whose `__arities__` holds both.
- At `CALL`, the VM picks the arity by argument count: an exact fixed arity
  first, then a variadic one. `(greet)` runs `form1.0`, which calls `greet`
  again with one argument and so runs `form1.1`.

### Example 09: destructuring and keyword arguments

<!-- vm-example: 09-destructuring -->
```clojure
(defn sum-pair [[a b]]
  (+ a b))

(defn area [{:keys [w h]}]
  (* w h))

(defn connect [host & {:keys [port] :or {port 80}}]
  [host port])

(sum-pair [1 2])
(area {:w 3 :h 4})
(connect "example.org" :port 8080)
```

```text
;; form 1: (defn sum-pair [[a b]] (+ a b))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    sum-pair
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  STORE_GLOBAL     0         ; sum-pair
    2  RETURN

module form1.0: arity 1, locals 3, max stack 4
  constants:
    #0   name    clojure.core/nth
    #1   long    0
    #2   long    1
  code:
     0  PUSH_LOCAL_CLEAR 0        ; param 0
     1  STORE_LOCAL      1        ; local
     2  PUSH_VAR         0        ; clojure.core/nth
     3  PUSH_LOCAL       1        ; local
     4  PUSH_CONST       1        ; 0
     5  PUSH_NIL
     6  CALL             3        ; 3 args
     7  STORE_LOCAL      2        ; local
     8  PUSH_VAR         0        ; clojure.core/nth
     9  PUSH_LOCAL       1        ; local
    10  PUSH_CONST       2        ; 1
    11  PUSH_NIL
    12  CALL             3        ; 3 args
    13  STORE_LOCAL      3        ; local
    14  PUSH_LOCAL_CLEAR 2        ; local
    15  PUSH_LOCAL_CLEAR 3        ; local
    16  ADD
    17  RETURN

;; form 2: (defn area [{:keys [w h]}] (* w h))
module form2: arity 0, locals 0, max stack 1
  constants:
    #0   name    area
  code:
    0  MAKE_FN          0         ; fn form2.0
    1  STORE_GLOBAL     0         ; area
    2  RETURN

module form2.0: arity 1, locals 4, max stack 3
  constants:
    #0   name    clojure.core/__destructure-map
    #1   name    clojure.core/get
    #2   named   :w
    #3   named   :h
  code:
     0  PUSH_LOCAL_CLEAR 0        ; param 0
     1  STORE_LOCAL      1        ; local
     2  PUSH_VAR         0        ; clojure.core/__destructure-map
     3  PUSH_LOCAL_CLEAR 1        ; local
     4  CALL             1        ; 1 arg
     5  STORE_LOCAL      2        ; local
     6  PUSH_VAR         1        ; clojure.core/get
     7  PUSH_LOCAL       2        ; local
     8  PUSH_CONST       2        ; :w
     9  CALL             2        ; 2 args
    10  STORE_LOCAL      3        ; local
    11  PUSH_VAR         1        ; clojure.core/get
    12  PUSH_LOCAL       2        ; local
    13  PUSH_CONST       3        ; :h
    14  CALL             2        ; 2 args
    15  STORE_LOCAL      4        ; local
    16  PUSH_LOCAL_CLEAR 3        ; local
    17  PUSH_LOCAL_CLEAR 4        ; local
    18  MUL
    19  RETURN

;; form 3: (defn connect [host & {:keys [port], :or {port 80}}] [host port])
module form3: arity 0, locals 0, max stack 1
  constants:
    #0   name    connect
  code:
    0  MAKE_FN          0         ; fn form3.0
    1  STORE_GLOBAL     0         ; connect
    2  RETURN

module form3.0: arity 1 + keyword args, locals 1, max stack 3
  keyword args: :port -> slot 1
  constants:
    #0   long    80
    #1   name    vector
  code:
     0  PUSH_LOCAL       1        ; :keys port
     1  PUSH_NIL
     2  EQ
     3  JUMP_IF_FALSE    2        ; -> 6
     4  PUSH_CONST       0        ; 80
     5  STORE_LOCAL      1        ; :keys port
     6  PUSH_VAR         1        ; vector
     7  PUSH_LOCAL_CLEAR 0        ; param 0
     8  PUSH_LOCAL       1        ; :keys port
     9  CALL             2        ; 2 args
    10  RETURN

;; form 4: (sum-pair [1 2])
module form4: arity 0, locals 0, max stack 4
  constants:
    #0   name    sum-pair
    #1   name    vector
    #2   long    1
    #3   long    2
  code:
    0  PUSH_VAR         0         ; sum-pair
    1  PUSH_VAR         1         ; vector
    2  PUSH_CONST       2         ; 1
    3  PUSH_CONST       3         ; 2
    4  CALL             2         ; 2 args
    5  CALL             1         ; 1 arg
    6  RETURN

;; form 5: (area {:w 3, :h 4})
module form5: arity 0, locals 0, max stack 6
  constants:
    #0   name    area
    #1   name    hash-map
    #2   named   :w
    #3   long    3
    #4   named   :h
    #5   long    4
  code:
    0  PUSH_VAR         0         ; area
    1  PUSH_VAR         1         ; hash-map
    2  PUSH_CONST       2         ; :w
    3  PUSH_CONST       3         ; 3
    4  PUSH_CONST       4         ; :h
    5  PUSH_CONST       5         ; 4
    6  CALL             4         ; 4 args
    7  CALL             1         ; 1 arg
    8  RETURN

;; form 6: (connect "example.org" :port 8080)
module form6: arity 0, locals 0, max stack 4
  constants:
    #0   name    connect
    #1   string  "example.org"
    #2   named   :port
    #3   long    8080
  code:
    0  PUSH_VAR         0         ; connect
    1  PUSH_CONST       1         ; "example.org"
    2  PUSH_CONST       2         ; :port
    3  PUSH_CONST       3         ; 8080
    4  CALL_KW          3         ; 3 args
    5  RETURN
```

- **No destructuring opcodes.** A destructured parameter becomes a
  generated parameter `p__N`, and the body is compiled as a `let` over it
  (`Compiler::compileArity`, `destructure`). `sum-pair`: slot 0 is `p__N`,
  copied to slot 1, then `a` and `b` are `(nth p 0 nil)` and `(nth p 1 nil)`
  calls of `clojure.core/nth` (`2`-`13`).
- `area`: a map pattern calls `clojure.core/__destructure-map` once, then
  `clojure.core/get` per key.
- `connect`: `& {:keys [port] :or {port 80}}` makes a **keyword-argument
  fn** (`+ keyword args`, `keyword args: :port -> slot 1`). The VM fills slot
  1 from the keyword map at call time; `0`-`5` are the `:or` prologue: if the
  slot is nil, store the default `80`. (A key passed explicitly as nil also
  gets the default: deviation D16 in docs/STATUS.md.)
- `form4`, `form5`: vector and map literals are calls of `vector` and
  `hash-map` (2.9).
- `form6`: the call ends in a `:port 8080` pair, so it is `CALL_KW 3`. The
  callee is keyword-based with one positional parameter, so the VM folds
  `:port 8080` into `{:port 8080}` and passes two arguments.

### Example 10: keyword and map lookups, `(:k m)`

<!-- vm-example: 10-keyword-lookup -->
```clojure
(defn port-of [config]
  (:port config))

(port-of {:host "example.org" :port 8080})
({:a 1} :a)
```

```text
;; form 1: (defn port-of [config] (:port config))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    port-of
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  STORE_GLOBAL     0         ; port-of
    2  RETURN

module form1.0: arity 1, locals 0, max stack 2
  constants:
    #0   named   :port
  code:
    0  PUSH_CONST       0         ; :port
    1  PUSH_LOCAL_CLEAR 0         ; param 0
    2  CALL             1         ; 1 arg
    3  RETURN

;; form 2: (port-of {:port 8080, :host "example.org"})
module form2: arity 0, locals 0, max stack 6
  constants:
    #0   name    port-of
    #1   name    hash-map
    #2   named   :host
    #3   string  "example.org"
    #4   named   :port
    #5   long    8080
  code:
    0  PUSH_VAR         0         ; port-of
    1  PUSH_VAR         1         ; hash-map
    2  PUSH_CONST       2         ; :host
    3  PUSH_CONST       3         ; "example.org"
    4  PUSH_CONST       4         ; :port
    5  PUSH_CONST       5         ; 8080
    6  CALL             4         ; 4 args
    7  CALL             1         ; 1 arg
    8  RETURN

;; form 3: ({:a 1} :a)
module form3: arity 0, locals 0, max stack 3
  constants:
    #0   name    hash-map
    #1   named   :a
    #2   long    1
  code:
    0  PUSH_VAR         0         ; hash-map
    1  PUSH_CONST       1         ; :a
    2  PUSH_CONST       2         ; 1
    3  CALL             2         ; 2 args
    4  PUSH_CONST       1         ; :a
    5  CALL             1         ; 1 arg
    6  RETURN
```

- `form1.0`: a keyword in call position is a value, not a global name:
  `PUSH_CONST :port`, then the map, then `CALL 1`. The VM sees a keyword
  callable and performs a map lookup with `get` semantics (1.5, step 3); no
  fn is called.
- `form2`: the map literal's keys and values are pushed in **source order**
  (`:host` first), so they are evaluated left to right, even though the
  `;; form` line prints the map the reader built in its hash order.
- `form3`: a map in call position, `({:a 1} :a)`, is the same lookup with
  the roles swapped. Note the constant `:a` is stored once and pushed twice.

### Example 11: a call of a primitive, `(first xs)`

<!-- vm-example: 11-primitive-call -->
```clojure
(defn head [xs]
  (first xs))

(head [1 2 3])
```

```text
;; form 1: (defn head [xs] (first xs))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    head
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  STORE_GLOBAL     0         ; head
    2  RETURN

module form1.0: arity 1, locals 0, max stack 2
  constants:
    #0   name    first
  code:
    0  PUSH_VAR         0         ; first
    1  PUSH_LOCAL_CLEAR 0         ; param 0
    2  CALL             1         ; 1 arg
    3  RETURN

;; form 2: (head [1 2 3])
module form2: arity 0, locals 0, max stack 5
  constants:
    #0   name    head
    #1   name    vector
    #2   long    1
    #3   long    2
    #4   long    3
  code:
    0  PUSH_VAR         0         ; head
    1  PUSH_VAR         1         ; vector
    2  PUSH_CONST       2         ; 1
    3  PUSH_CONST       3         ; 2
    4  PUSH_CONST       4         ; 3
    5  CALL             3         ; 3 args
    6  CALL             1         ; 1 arg
    7  RETURN
```

- A primitive is called exactly like a user fn: `PUSH_VAR first`, the
  argument, `CALL 1`. The difference is at run time: the global holds a
  protoCore method value, and `first` on a vector is answered by
  `fastPrimitiveCall` (1.5, step 1). A primitive without a direct answer is
  called on the general path (1.5, step 4), also in the caller's frame: no
  primitive call creates a context.
- `form2`: the vector literal `[1 2 3]` is a `CALL 3` of `vector` on every
  evaluation, even though its elements are constants (a set of constants,
  by contrast, is a single `PUSH_CONST`).

### Example 12: `reduce` with a fn

<!-- vm-example: 12-reduce -->
```clojure
(reduce (fn [acc x] (+ acc x)) 0 [1 2 3 4])
(reduce + [1 2 3 4])
```

```text
;; form 1: (reduce (fn [acc x] (+ acc x)) 0 [1 2 3 4])
module form1: arity 0, locals 0, max stack 8
  constants:
    #0   name    reduce
    #1   long    0
    #2   name    vector
    #3   long    1
    #4   long    2
    #5   long    3
    #6   long    4
  code:
     0  PUSH_VAR         0        ; reduce
     1  MAKE_FN          0        ; fn form1.0
     2  PUSH_CONST       1        ; 0
     3  PUSH_VAR         2        ; vector
     4  PUSH_CONST       3        ; 1
     5  PUSH_CONST       4        ; 2
     6  PUSH_CONST       5        ; 3
     7  PUSH_CONST       6        ; 4
     8  CALL             4        ; 4 args
     9  CALL             3        ; 3 args
    10  RETURN

module form1.0: arity 2, locals 0, max stack 2
  code:
    0  PUSH_LOCAL_CLEAR 0         ; param 0
    1  PUSH_LOCAL_CLEAR 1         ; param 1
    2  ADD
    3  RETURN

;; form 2: (reduce + [1 2 3 4])
module form2: arity 0, locals 0, max stack 7
  constants:
    #0   name    reduce
    #1   name    +
    #2   name    vector
    #3   long    1
    #4   long    2
    #5   long    3
    #6   long    4
  code:
     0  PUSH_VAR         0        ; reduce
     1  PUSH_VAR         1        ; +
     2  PUSH_VAR         2        ; vector
     3  PUSH_CONST       3        ; 1
     4  PUSH_CONST       4        ; 2
     5  PUSH_CONST       5        ; 3
     6  PUSH_CONST       6        ; 4
     7  CALL             4        ; 4 args
     8  CALL             2        ; 2 args
     9  RETURN
```

- `reduce` is a C++ primitive (`prim_reduce`). The fn literal becomes a
  value on the stack (`1 MAKE_FN 0`), passed as an argument like any other.
- Inside `reduce`, each step calls the fn through
  `ExecutionEngine::invoke`, which runs `form1.0` in a frame of its own.
- `form1.0`: both parameters are read once, so both reads clear.
- `form2`: `+` as a **value** is `PUSH_VAR +`, the `+` primitive. Only a
  call with `+` at its head becomes `ADD`.

### Example 13: `try` / `catch` / `finally` / `throw`

<!-- vm-example: 13-try-catch-throw -->
```clojure
(defn checked-div [a b]
  (if (= b 0)
    (throw (ex-info "division by zero" {:a a}))
    (quot a b)))

(defn safe-div [a b]
  (try
    (checked-div a b)
    (catch clojure.lang.ExceptionInfo e
      (ex-data e))
    (finally
      (str "done"))))

(safe-div 10 0)
```

```text
;; form 1: (defn checked-div [a b] (if (= b 0) (throw (ex-info "division by zero" {:a a})) (quot a b)))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    checked-div
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  STORE_GLOBAL     0         ; checked-div
    2  RETURN

module form1.0: arity 2, locals 0, max stack 5
  constants:
    #0   long    0
    #1   name    ex-info
    #2   string  "division by zero"
    #3   name    hash-map
    #4   named   :a
    #5   name    quot
  code:
     0  PUSH_LOCAL       1        ; param 1
     1  PUSH_CONST       0        ; 0
     2  EQ
     3  JUMP_IF_FALSE    9        ; -> 13
     4  PUSH_VAR         1        ; ex-info
     5  PUSH_CONST       2        ; "division by zero"
     6  PUSH_VAR         3        ; hash-map
     7  PUSH_CONST       4        ; :a
     8  PUSH_LOCAL       0        ; param 0
     9  CALL             2        ; 2 args
    10  CALL             2        ; 2 args
    11  THROW
    12  JUMP             4        ; -> 17
    13  PUSH_VAR         5        ; quot
    14  PUSH_LOCAL       0        ; param 0
    15  PUSH_LOCAL       1        ; param 1
    16  CALL             2        ; 2 args
    17  RETURN

;; form 2: (defn safe-div [a b] (try (checked-div a b) (catch clojure.lang.ExceptionInfo e (ex-data e)) (finally (str "done"))))
module form2: arity 0, locals 0, max stack 1
  constants:
    #0   name    safe-div
  code:
    0  MAKE_FN          0         ; fn form2.0
    1  STORE_GLOBAL     0         ; safe-div
    2  RETURN

module form2.0: arity 2, locals 2, max stack 3, has handlers
  constants:
    #0   name    checked-div
    #1   name    ex-data
    #2   name    str
    #3   string  "done"
  code:
     0  TRY_BEGIN        22       ; -> 23
     1  TRY_BEGIN        6        ; -> 8
     2  PUSH_VAR         0        ; checked-div
     3  PUSH_LOCAL_CLEAR 0        ; param 0
     4  PUSH_LOCAL_CLEAR 1        ; param 1
     5  CALL             2        ; 2 args
     6  TRY_END
     7  JUMP             9        ; -> 17
     8  STORE_CAUGHT     2        ; local
     9  PUSH_LOCAL       2        ; local
    10  EXC_MATCH        4        ; ExceptionInfo
    11  JUMP_IF_FALSE    4        ; -> 16
    12  PUSH_VAR         1        ; ex-data
    13  PUSH_LOCAL       2        ; local
    14  CALL             1        ; 1 arg
    15  JUMP             1        ; -> 17
    16  RETHROW          2        ; local
    17  TRY_END
    18  PUSH_VAR         2        ; str
    19  PUSH_CONST       3        ; "done"
    20  CALL             1        ; 1 arg
    21  POP
    22  JUMP             6        ; -> 29
    23  STORE_CAUGHT     3        ; local
    24  PUSH_VAR         2        ; str
    25  PUSH_CONST       3        ; "done"
    26  CALL             1        ; 1 arg
    27  POP
    28  RETHROW          3        ; local
    29  RETURN

;; form 3: (safe-div 10 0)
module form3: arity 0, locals 0, max stack 3
  constants:
    #0   name    safe-div
    #1   long    10
    #2   long    0
  code:
    0  PUSH_VAR         0         ; safe-div
    1  PUSH_CONST       1         ; 10
    2  PUSH_CONST       2         ; 0
    3  CALL             2         ; 2 args
    4  RETURN
```

- `checked-div`: `throw` is `11 THROW`; its operand is the `ex-info` built at
  `4`-`10`. `12 JUMP` after it is unreachable (the compiler emits the `if`'s
  jump regardless).
- `safe-div` has `has handlers`, so it runs in the frame variant with a C++
  `catch` (1.3, 1.7). The layout is exactly the one in 1.7:
  - `0 TRY_BEGIN -> 23` is the `finally` handler, `1 TRY_BEGIN -> 8` the
    catch handler; `2`-`5` the body; `6 TRY_END`, `7 JUMP -> 17`.
  - `8 STORE_CAUGHT 2`: the exception into slot 2 (`e`). `9`-`11`: does it
    match `clojure.lang.ExceptionInfo` (built-in class id 4)? If not, jump to
    `16 RETHROW 2`, which rethrows the original C++ exception.
  - `17`-`22`: normal path: `TRY_END` (leaving the finally handler), the
    finally forms with their value dropped (`21 POP`), `JUMP` to the end.
  - `23`-`28`: exceptional path: the finally forms again, then `RETHROW 3`.
- The try's value is whatever is left on the stack at `29`: the body's
  result or the catch clause's.

### Example 14: a macro use

<!-- vm-example: 14-macro -->
```clojure
(defmacro unless [test then]
  `(if ~test nil ~then))

(unless false :ran)

(when (pos? 1) :yes)
```

```text
;; form 1: (defmacro unless [test then] (clojure.core/seq (clojure.core/concat (clojure.core/list (quote if)) (clojure.core/list test) (clojure.core/list nil) (clojure.core/list then))))
module form1: arity 0, locals 0, max stack 2
  constants:
    #0   name    __macro__
    #1   name    unless
  code:
    0  PUSH_VAR         0         ; __macro__
    1  MAKE_FN          0         ; fn form1.0
    2  CALL             1         ; 1 arg
    3  STORE_GLOBAL     1         ; unless
    4  RETURN

module form1.0: arity 4, locals 0, max stack 7
  constants:
    #0   name    clojure.core/seq
    #1   name    clojure.core/concat
    #2   name    clojure.core/list
    #3   named   if
  code:
     0  PUSH_VAR         0        ; clojure.core/seq
     1  PUSH_VAR         1        ; clojure.core/concat
     2  PUSH_VAR         2        ; clojure.core/list
     3  PUSH_CONST       3        ; if
     4  CALL             1        ; 1 arg
     5  PUSH_VAR         2        ; clojure.core/list
     6  PUSH_LOCAL_CLEAR 2        ; param 2
     7  CALL             1        ; 1 arg
     8  PUSH_VAR         2        ; clojure.core/list
     9  PUSH_NIL
    10  CALL             1        ; 1 arg
    11  PUSH_VAR         2        ; clojure.core/list
    12  PUSH_LOCAL_CLEAR 3        ; param 3
    13  CALL             1        ; 1 arg
    14  CALL             4        ; 4 args
    15  CALL             1        ; 1 arg
    16  RETURN

;; form 2: (if false nil :ran)
module form2: arity 0, locals 0, max stack 1
  constants:
    #0   named   :ran
  code:
    0  PUSH_CONST       0         ; :ran
    1  RETURN

;; form 3: (if (pos? 1) (do :yes))
module form3: arity 0, locals 0, max stack 2
  constants:
    #0   name    pos?
    #1   long    1
    #2   named   :yes
  code:
    0  PUSH_VAR         0         ; pos?
    1  PUSH_CONST       1         ; 1
    2  CALL             1         ; 1 arg
    3  JUMP_IF_FALSE    2         ; -> 6
    4  PUSH_CONST       2         ; :yes
    5  JUMP             1         ; -> 7
    6  PUSH_NIL
    7  RETURN
```

- `form1`: `defmacro` compiles the macro as a fn with the two hidden
  parameters `&form` and `&env` in front (`form1.0: arity 4`), wraps it with
  `__macro__` (which flags it as a macro) and stores it as a global. The
  `;; form` line shows the syntax-quote as the reader expanded it: calls of
  `seq`, `concat` and `list`.
- `form2`: what reaches the compiler is the **expansion**,
  `(if false nil :ran)`: the macro ran at compile time and is not in the
  bytecode. The literal `false` test is then folded, leaving one
  `PUSH_CONST`.
- `form3`: `when` (a `core.clj` macro) expanded to `(if (pos? 1) (do :yes))`.

### Example 15: a lazy seq and `for`

<!-- vm-example: 15-lazy-for -->
```clojure
(defn nats [n]
  (lazy-seq (cons n (nats (inc n)))))

(take 3 (nats 0))

(defn squares [n]
  (for [x (range n)] (* x x)))

(doall (squares 3))
```

```text
;; form 1: (defn nats [n] (lazy-seq (cons n (nats (inc n)))))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    nats
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  STORE_GLOBAL     0         ; nats
    2  RETURN

module form1.0: arity 1, locals 0, max stack 2
  constants:
    #0   name    clojure.core/__lazy-seq
  code:
    0  PUSH_VAR         0         ; clojure.core/__lazy-seq
    1  PUSH_LOCAL_CLEAR 0         ; param 0
    2  MAKE_FN          0         ; fn form1.0.0
    3  CALL             1         ; 1 arg
    4  RETURN

module form1.0.0: arity 0, locals 1, max stack 5
  captures: parent slot 0 -> slot 0
  constants:
    #0   name    cons
    #1   name    nats
    #2   name    inc
  code:
    0  PUSH_VAR         0         ; cons
    1  PUSH_LOCAL       0         ; capture (parent slot 0)
    2  PUSH_VAR         1         ; nats
    3  PUSH_VAR         2         ; inc
    4  PUSH_LOCAL       0         ; capture (parent slot 0)
    5  CALL             1         ; 1 arg
    6  CALL             1         ; 1 arg
    7  CALL             2         ; 2 args
    8  RETURN

;; form 2: (take 3 (nats 0))
module form2: arity 0, locals 0, max stack 4
  constants:
    #0   name    take
    #1   long    3
    #2   name    nats
    #3   long    0
  code:
    0  PUSH_VAR         0         ; take
    1  PUSH_CONST       1         ; 3
    2  PUSH_VAR         2         ; nats
    3  PUSH_CONST       3         ; 0
    4  CALL             1         ; 1 arg
    5  CALL             2         ; 2 args
    6  RETURN

;; form 3: (defn squares [n] (for [x (range n)] (* x x)))
module form3: arity 0, locals 0, max stack 1
  constants:
    #0   name    squares
  code:
    0  MAKE_FN          0         ; fn form3.0
    1  STORE_GLOBAL     0         ; squares
    2  RETURN

module form3.0: arity 1, locals 0, max stack 4
  constants:
    #0   name    clojure.core/__for-step
    #1   name    range
  code:
    0  PUSH_VAR         0         ; clojure.core/__for-step
    1  PUSH_VAR         1         ; range
    2  PUSH_LOCAL_CLEAR 0         ; param 0
    3  CALL             1         ; 1 arg
    4  MAKE_FN          0         ; fn form3.0.0
    5  PUSH_TRUE
    6  CALL             3         ; 3 args
    7  RETURN

module form3.0.0: arity 1, locals 0, max stack 3
  constants:
    #0   name    clojure.core/list
  code:
    0  PUSH_VAR         0         ; clojure.core/list
    1  PUSH_LOCAL       0         ; param 0
    2  PUSH_LOCAL       0         ; param 0
    3  MUL
    4  CALL             1         ; 1 arg
    5  RETURN

;; form 4: (doall (squares 3))
module form4: arity 0, locals 0, max stack 3
  constants:
    #0   name    doall
    #1   name    squares
    #2   long    3
  code:
    0  PUSH_VAR         0         ; doall
    1  PUSH_VAR         1         ; squares
    2  PUSH_CONST       2         ; 3
    3  CALL             1         ; 1 arg
    4  CALL             1         ; 1 arg
    5  RETURN
```

- `lazy-seq` is a macro: `(lazy-seq body)` is `(__lazy-seq (fn [] body))`.
  The body is a closure, `form1.0.0`, capturing `n`; the C++ primitive
  `__lazy-seq` calls it at most once, when the seq is first walked. The
  recursive `(nats (inc n))` is therefore not a nested call at definition
  time: it happens when `take` asks for the next element.
- `for` expands to `__for-step` (a C++ primitive in `LazySeq.cpp`) with the
  collection and a fn per binding (`form3.0.0`, the body, which returns a
  one-element list). The trailing `PUSH_TRUE` allows chunked walking of the
  innermost binding.

### Example 16: `swap!` on an atom

<!-- vm-example: 16-atom-swap -->
```clojure
(def counter (atom 0))

(swap! counter inc)
(swap! counter + 10)
@counter
```

```text
;; form 1: (def counter (atom 0))
module form1: arity 0, locals 0, max stack 2
  constants:
    #0   name    atom
    #1   long    0
    #2   name    counter
  code:
    0  PUSH_VAR         0         ; atom
    1  PUSH_CONST       1         ; 0
    2  CALL             1         ; 1 arg
    3  STORE_GLOBAL     2         ; counter
    4  RETURN

;; form 2: (swap! counter inc)
module form2: arity 0, locals 0, max stack 3
  constants:
    #0   name    swap!
    #1   name    counter
    #2   name    inc
  code:
    0  PUSH_VAR         0         ; swap!
    1  PUSH_VAR         1         ; counter
    2  PUSH_VAR         2         ; inc
    3  CALL             2         ; 2 args
    4  RETURN

;; form 3: (swap! counter + 10)
module form3: arity 0, locals 0, max stack 4
  constants:
    #0   name    swap!
    #1   name    counter
    #2   name    +
    #3   long    10
  code:
    0  PUSH_VAR         0         ; swap!
    1  PUSH_VAR         1         ; counter
    2  PUSH_VAR         2         ; +
    3  PUSH_CONST       3         ; 10
    4  CALL             3         ; 3 args
    5  RETURN

;; form 4: (clojure.core/deref counter)
module form4: arity 0, locals 0, max stack 2
  constants:
    #0   name    clojure.core/deref
    #1   name    counter
  code:
    0  PUSH_VAR         0         ; clojure.core/deref
    1  PUSH_VAR         1         ; counter
    2  CALL             1         ; 1 arg
    3  RETURN
```

- An atom is a value made by the `atom` primitive; `def` stores it
  (`STORE_GLOBAL counter`).
- `(swap! counter inc)` is a plain call of the `swap!` primitive with the
  atom and the fn. `swap!` calls the fn through `ExecutionEngine::invoke` and
  installs the result with a compare-and-set on the atom
  (`setAttributeIfEqual`), retrying if another thread changed it in between.
- `@counter` is read as `(clojure.core/deref counter)` by the reader.

### Example 17: `and`, `or`, `apply`, quoted data, members

<!-- vm-example: 17-misc -->
```clojure
(defn both [a b] (and a b))
(defn either [a b] (or a b))
(defn spread [f xs] (apply f 1 xs))
(def quoted '(a b c))
(defn now [] (System/currentTimeMillis))
(defn exit-fn [] System/exit)
```

```text
;; form 1: (defn both [a b] (and a b))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    both
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  STORE_GLOBAL     0         ; both
    2  RETURN

module form1.0: arity 2, locals 0, max stack 2
  code:
    0  PUSH_LOCAL_CLEAR 0         ; param 0
    1  DUP
    2  JUMP_IF_FALSE    2         ; -> 5
    3  POP
    4  PUSH_LOCAL_CLEAR 1         ; param 1
    5  RETURN

;; form 2: (defn either [a b] (or a b))
module form2: arity 0, locals 0, max stack 1
  constants:
    #0   name    either
  code:
    0  MAKE_FN          0         ; fn form2.0
    1  STORE_GLOBAL     0         ; either
    2  RETURN

module form2.0: arity 2, locals 0, max stack 2
  code:
    0  PUSH_LOCAL_CLEAR 0         ; param 0
    1  DUP
    2  JUMP_IF_TRUE     2         ; -> 5
    3  POP
    4  PUSH_LOCAL_CLEAR 1         ; param 1
    5  RETURN

;; form 3: (defn spread [f xs] (apply f 1 xs))
module form3: arity 0, locals 0, max stack 1
  constants:
    #0   name    spread
  code:
    0  MAKE_FN          0         ; fn form3.0
    1  STORE_GLOBAL     0         ; spread
    2  RETURN

module form3.0: arity 2, locals 0, max stack 4
  constants:
    #0   name    clojure.core/list*
    #1   long    1
  code:
    0  PUSH_LOCAL_CLEAR 0         ; param 0
    1  PUSH_VAR         0         ; clojure.core/list*
    2  PUSH_CONST       1         ; 1
    3  PUSH_LOCAL_CLEAR 1         ; param 1
    4  CALL             2         ; 2 args
    5  CALL_APPLY
    6  RETURN

;; form 4: (def quoted (quote (a b c)))
module form4: arity 0, locals 0, max stack 1
  constants:
    #0   datum   (a b c)
    #1   name    quoted
  code:
    0  PUSH_CONST       0         ; (a b c)
    1  STORE_GLOBAL     1         ; quoted
    2  RETURN

;; form 5: (defn now [] (System/currentTimeMillis))
module form5: arity 0, locals 0, max stack 1
  constants:
    #0   name    now
  code:
    0  MAKE_FN          0         ; fn form5.0
    1  STORE_GLOBAL     0         ; now
    2  RETURN

module form5.0: arity 0, locals 0, max stack 1
  constants:
    #0   name    System
    #1   name    currentTimeMillis
  code:
    0  PUSH_VAR         0         ; System
    1  CALL_MEMBER      0         ; 0 args
    2  NOP              1         ; data word of CALL_MEMBER: member currentTimeMillis
    3  RETURN

;; form 6: (defn exit-fn [] System/exit)
module form6: arity 0, locals 0, max stack 1
  constants:
    #0   name    exit-fn
  code:
    0  MAKE_FN          0         ; fn form6.0
    1  STORE_GLOBAL     0         ; exit-fn
    2  RETURN

module form6.0: arity 0, locals 0, max stack 1
  constants:
    #0   name    System
    #1   name    exit
  code:
    0  PUSH_VAR         0         ; System
    1  GET_MEMBER       1         ; exit
    2  RETURN
```

- `and` / `or`: the tested value is kept as the result, so it is duplicated
  (`DUP`) before the conditional jump consumes the copy; when the test does
  not short-circuit, `POP` drops it and the next operand is evaluated.
- `apply` with extra arguments: `(list* 1 xs)` builds the spread list,
  then `CALL_APPLY` spreads it and dispatches `f`.
- A quoted list is one `PUSH_CONST` of a `datum`: every evaluation answers
  the same immutable object.
- `(System/currentTimeMillis)`: a member call on the global object `System`:
  `CALL_MEMBER 0` followed by its data word (`NOP 1`, the member's name
  constant).
- `System/exit` without parentheses is a value: `GET_MEMBER`.

### Example 18: catch clauses with types resolved at run time

<!-- vm-example: 18-catch-types -->
```clojure
(defn guarded [f T mod]
  (try
    (f)
    (catch ArithmeticException e :arithmetic)
    (catch T e :local-type)
    (catch mod/Failure e :member-type)
    (catch app.errors/Timeout e :global-type)))

(guarded (fn [] 42) nil nil)
```

```text
;; form 1: (defn guarded [f T mod] (try (f) (catch ArithmeticException e :arithmetic) (catch T e :local-type) (catch mod/Failure e :member-type) (catch app.errors/Timeout e :global-type)))
module form1: arity 0, locals 0, max stack 1
  constants:
    #0   name    guarded
  code:
    0  MAKE_FN          0         ; fn form1.0
    1  STORE_GLOBAL     0         ; guarded
    2  RETURN

module form1.0: arity 3, locals 1, max stack 2, has handlers
  constants:
    #0   named   :arithmetic
    #1   name    T
    #2   named   :local-type
    #3   name    Failure
    #4   name    mod/Failure
    #5   named   :member-type
    #6   name    app.errors/Timeout
    #7   named   :global-type
  code:
     0  TRY_BEGIN        4        ; -> 5
     1  PUSH_LOCAL_CLEAR 0        ; param 0
     2  CALL             0        ; 0 args
     3  TRY_END
     4  JUMP             26       ; -> 31
     5  STORE_CAUGHT     3        ; local
     6  PUSH_LOCAL       3        ; local
     7  EXC_MATCH        5        ; ArithmeticException
     8  JUMP_IF_FALSE    2        ; -> 11
     9  PUSH_CONST       0        ; :arithmetic
    10  JUMP             20       ; -> 31
    11  PUSH_LOCAL       3        ; local
    12  PUSH_LOCAL_CLEAR 1        ; param 1
    13  EXC_MATCH_TYPE   1        ; T
    14  JUMP_IF_FALSE    2        ; -> 17
    15  PUSH_CONST       2        ; :local-type
    16  JUMP             14       ; -> 31
    17  PUSH_LOCAL       3        ; local
    18  PUSH_LOCAL_CLEAR 2        ; param 2
    19  GET_MEMBER       3        ; Failure
    20  EXC_MATCH_TYPE   4        ; mod/Failure
    21  JUMP_IF_FALSE    2        ; -> 24
    22  PUSH_CONST       5        ; :member-type
    23  JUMP             7        ; -> 31
    24  PUSH_LOCAL       3        ; local
    25  RESOLVE_CLASS    6        ; app.errors/Timeout
    26  EXC_MATCH_TYPE   6        ; app.errors/Timeout
    27  JUMP_IF_FALSE    2        ; -> 30
    28  PUSH_CONST       7        ; :global-type
    29  JUMP             1        ; -> 31
    30  RETHROW          3        ; local
    31  RETURN

;; form 2: (guarded (fn [] 42) nil nil)
module form2: arity 0, locals 0, max stack 4
  constants:
    #0   name    guarded
  code:
    0  PUSH_VAR         0         ; guarded
    1  MAKE_FN          0         ; fn form2.0
    2  PUSH_NIL
    3  PUSH_NIL
    4  CALL             3         ; 3 args
    5  RETURN

module form2.0: arity 0, locals 0, max stack 1
  constants:
    #0   long    42
  code:
    0  PUSH_CONST       0         ; 42
    1  RETURN
```

The body never throws here, so none of these clauses runs; the listing shows
how each kind of catch type compiles:

- a built-in class: `EXC_MATCH` with its class id (`7`);
- a local holding a type: `PUSH_LOCAL` then `EXC_MATCH_TYPE` (`11`-`13`);
- `mod/Failure` with `mod` a local: `PUSH_LOCAL`, `GET_MEMBER Failure`,
  `EXC_MATCH_TYPE` (`17`-`20`);
- a name that is neither (`app.errors/Timeout`): `RESOLVE_CLASS`, looked up
  in the globals only when an exception reaches the clause (`24`-`26`).

Clauses are tested in order; the first match wins; no match reaches `30
RETHROW 3`.

## 4. How to add an opcode

The model is `JUMP_IF_TRUE` (value 18), added together with `DUP` to compile
`or`. Every place to touch, in order:

1. **The enum,** `src/runtime/Opcodes.h`: append the next free value (41
   today) with a comment giving its operand and stack effect. Never renumber
   existing opcodes: the value is the encoding.
   ```cpp
   JUMP_IF_TRUE    = 18,  // pop; jump forward if value is NOT (nil|false)
   ```
2. **The name,** `opName` in `src/runtime/BytecodeModule.cpp`. GCC and
   Clang warn about an unhandled enum value (`-Wswitch`), which CI turns
   into an error (`-DPROTOCLJ_WARNINGS_AS_ERRORS=ON`).
3. **The stack analysis,** `BytecodeModule::computeMaxStack`: give its stack
   effect and, for a jump, its successors. `JUMP_IF_TRUE` is grouped with
   `JUMP_IF_FALSE`: `reach(next + operand, d - 1); after = d - 1;`. An
   opcode missing here makes the analysis fall back to a stack of 64, which
   can overflow silently.
4. **The compiler,** `src/compiler/Compiler.cpp`: emit it with
   `out.emit(Op::X, operand)`. For a forward jump, emit with operand 0, keep
   the position `emit` returns, and back-patch when the target is known
   (the `or` branch of `compileForm`):
   ```cpp
   out.emit(Op::DUP, 0);
   shortAts.push_back(out.emit(Op::JUMP_IF_TRUE, 0));
   out.emit(Op::POP, 0);
   ...
   out.patchOperand(at, end - (at + 1));
   ```
   If it reads or writes a local slot, teach `clearLastReads` (locals
   clearing) about it.
5. **The VM,** the `switch` in `ExecutionEngine::executeFrame`
   (`src/runtime/ExecutionEngine.cpp`). Rooting rules: read operands from
   their stack slots with `peekAt` and pop them only once the result is
   computed, if anything in between allocates; never hold a `ProtoObject*`
   only in a C++ local across an allocation or a safepoint (protoCore
   EMBEDDER-CONFORMANCE rule 3). Keep cold paths out of line (`[[gnu::cold,
   gnu::noinline]]` helpers): code inlined into the dispatch loop changes the
   register allocation of the whole loop and has cost several percent on
   benchmarks that never run it.
   ```cpp
   case Op::JUMP_IF_TRUE: {
       const proto::ProtoObject* v = popVal();
       if (v != PROTO_NONE && v != PROTO_FALSE) ip += operand;
       break;
   }
   ```
   If the debugger's "does this frame catch it" scan
   (`handlerCatches`) or `clearLastReads` walk the code, check whether the
   new opcode changes the shapes they expect.
6. **The disassembler,** `listModule` in `src/runtime/Disassembler.cpp`:
   put the opcode in the group that decodes its operand (`JUMP_IF_TRUE` is
   with the forward jumps, annotated `-> target`). An opcode it does not know
   is listed as `unknown opcode`.
7. **Tests:** a conformance fixture under `tests/conformance/` for the
   behaviour (format in section 5), and a `docs/vm-examples/NN-name.clj`
   whose listing shows the new opcode, with its marker in this document.
   Run `disasm_examples.py ... --update`, then `ctest -R docs/` to confirm
   the golden files and this document agree.
8. **This document:** the opcode's row in section 2 (and the count at the
   top of that section), and the notes of any example whose listing changed.

## 5. How to add a primitive

A primitive is a C++ function installed as a global.

**The context rule (maintainer's rule).** A `ProtoContext` is created only
when a method, that is interpreted code with its own locals, is activated;
a primitive runs in its caller's context, and so does every helper it
calls. **Never construct a `ProtoContext` in a primitive.** Allocate in the
`ctx` you are given, and root what you hold with `CallerSlots` (below).

1. **Write the function** with protoCore's `ProtoMethod` signature. A
   built-in reads its arguments from `args` and raises errors with
   `throwClassed` (a classed Clojure exception) or `throwError`. Model:
   `prim_first` in `src/runtime/Primitives.cpp`:
   ```cpp
   const proto::ProtoObject* prim_first(proto::ProtoContext* ctx,
                                        const proto::ProtoObject*,          // self (unused)
                                        const proto::ParentLink*,
                                        const proto::ProtoArguments* args,  // positional
                                        const proto::ProtoKeywordArguments*) {
       if (!args || args->getSize(ctx) != 1)
           throwError(ctx, "first: expects 1 arg");
       const proto::ProtoObject* v = args->getAt(ctx, 0);
       ...
   }
   ```
   Files for new built-ins: `Primitives.cpp` for the core ones,
   `CorePrims.cpp` for the scripting vocabulary (it has `COREPRIM`, `argc`,
   `argAt` and `arity` helpers), `LazySeq.cpp` for sequence functions,
   `IO.cpp` for I/O. A function that needs no C++ speed belongs in
   `src/core/core.clj` instead.
2. **Register it.** Add `{"name", &prim_name}` to the file's table
   (`kPrimitives` in `Primitives.cpp`; `CorePrims.cpp`, `LazySeq.cpp` and
   `IO.cpp` have their own install functions). `installPrimitives` wraps
   each entry with `ctx->fromMethod(nullptr, fn)` and installs it with
   `defineBuiltin`, which also binds `clojure.core/name`, so the built-in
   stays reachable after a program `def`s the same name.
3. **Calling convention.** Arguments arrive positionally in `args` (the
   VM's operand-stack slots, or storage the caller of `invoke` roots);
   `kwArgs` is always null (keyword arguments are a property of Clojure fns,
   not of primitives). A primitive that calls back into Clojure (`reduce`,
   `swap!`) uses `activeCallContext()->engine->invoke(ctx, f, argv, n)`,
   with `argv` rooted.
4. **Rooting** (protoCore EMBEDDER-CONFORMANCE rule 3): no `ProtoObject*` may
   be held only in a C++ local across an allocation, or across a
   `safepoint()`. The arguments are rooted by the caller for the whole call.
   What you allocate is a young cell of `ctx`, live until `ctx`'s chain is
   submitted, and nothing submits it while your primitive runs unless you
   ask (below), so values you build are safe across your own allocations
   and callbacks. Put in `CallerSlots slots(ctx, n)` (`src/runtime/
   CallerSlots.h`, at most 8; `set`, `get`, `data`) what must survive a
   submission, and `pin` a value read from shared mutable state (an atom)
   that another thread may drop. A long loop hands the frame's garbage to the
   collector with `slots.tick()`, which works only while your body holds a
   `SafepointPermit permit(ctx)`: declare it only where every value you hold
   across a call is in `CallerSlots` or your arguments, and return a result
   computed before a submission through `slots.ret(v)`. Return the result
   unrooted; the VM roots it in the caller's slot.
5. **Fast path (optional).** If the primitive is hot and can answer some
   inputs without running Clojure code, add a case to `fastPrimitiveCall`
   (end of `Primitives.cpp`); return `nullptr` for anything it does not
   answer, and allocate only when `mayAllocate` is true.
6. **Test fixture.** Add `tests/conformance/<area>/<name>.clj`; it is
   discovered by the glob in `tests/CMakeLists.txt`. The first line is the
   directive: `;; EXPECT: <text>` (exit 0, last line of standard output
   equals the text), `;; EXPECT-ERROR[: <text>]`, or `;; XFAIL: ...` for a
   known deviation. Cover the error cases too.
   ```clojure
   ;; EXPECT: 1
   (println (first [1 2 3]))
   ```
7. **Docs:** `docs/LANGUAGE.md` / `docs/STATUS.md` list the built-ins.

## 6. Known oddities

Found while writing this document, from the code on `main` at `721e5c3`.
None is a dead opcode: all 41 are emitted.

- **Stale comments in `Opcodes.h`.** The header comment still describes the
  "Session 3 minimum" of five opcodes, and `JUMP`'s comment says
  "offset in instructions (x2 bytes)": instructions are 4-byte words.
  `CALL_APPLY`'s "must be 0 in v0.7.x" is still true (the compiler always
  emits 0 and the VM ignores it).
- **`EXC_MATCH_TYPE`'s operand** is documented as a "const-pool string idx";
  the compiler adds it as a `Symbol` (a global-name constant, interned). The
  VM reads only its spelling, for the error message, so the effect is the
  same, but the pool holds an interned symbol that is never used as a key.
- **Fixed 2026-10-25: the arithmetic fast paths ignored a global
  redefinition** (after `(def + -)`, `(+ 5 3)` was `8`). The compiler now
  emits `ADD` .. `EQ` only while the global holds the built-in, and the
  opcode calls the global when it was redefined later (2.7; DESIGN.md M46).
  The comment "falls back to the primitive on globals" in `compileForm` is
  inaccurate: the slow path computes with protoCore directly, with the same
  results as the primitive.
- **Fixed 2026-10-25: `CALL_KW` and multi-arity fns.** A keyword-argument
  arity of a multi-arity fn could not be called with keyword arguments
  (`VM: no matching arity for argc=3`); `pickArity` now has a third pass for
  it and the VM folds the pairs (1.5, step 2).
- **`__arity__`.** The key `arityKey` (`__arity__`) is created and passed to
  `ExecutionEngine::run` and through `ActiveCallContext`, but no code reads
  or writes it: fn values carry their arity in their module.
- **A fn value stores a C++ address as an integer.** `__bytecode__` (and the
  second element of each `__arities__` entry) is the `BytecodeModule`'s
  address passed through `fromLong`. It relies on user-space addresses
  fitting a SmallInteger, and on the module outliving every fn built from it
  (true: modules are retained until the end of the run).
- **`computeMaxStack` falls back to 64** for an opcode it does not know,
  instead of failing. With all 41 handled this cannot happen today; a new
  opcode missing there would get a fixed capacity, and `pushVal` does not
  check capacity per push.
- **`(fn name [x] ...)`**: the name is accepted and dropped, so the fn cannot
  call itself by that name (deviation D18 in docs/STATUS.md).
- **Vector literals of constants are rebuilt on every evaluation**
  (`PUSH_VAR vector` + `CALL n`, example 11), while a set of constants is a
  single `PUSH_CONST`. Not a correctness issue; noted because it costs a call
  per evaluation.
- **Unreachable code after `throw` and `recur`.** The compiler emits the
  enclosing `if`'s `JUMP` after a then-branch that ends in `THROW` (example
  13, instruction 12) or in a `recur`'s `JUMP_BACK`. Harmless.
