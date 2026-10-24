# protoClojure — Architectural Design

> **Purpose of this document.** Explain the design decisions that the
> language reference (`LANGUAGE.md`) takes for granted. Useful before
> writing code, useful for understanding *why* a feature is the way it is
> when the reference only documents *what* it does.

> **Implementation status (protoClojure 0.0.1).** §1 (substrate) is
> realised — the runtime is what is described there. The reader and the
> compiler / VM (§3-§4), atoms (§6) and the local REPL (§9) are
> implemented in part; the UMD module system (§2), namespaces and vars
> (§5), refs and agents (§6), lazy sequences (§7), protocols and
> multimethods (§8), the nREPL server (§9) and macros (§10) are planned.
> `docs/STATUS.md` lists exactly what ships; where this document and the
> implementation differ, the design describes the target shape, not the
> current behaviour.

---

## 1. The substrate

protoClojure compiles to bytecode for a stack-based interpreter that
runs on top of the **protoCore object kernel** — the same C++20 kernel
that hosts protoJS, protoPython, and protoST. The kernel gives us, for
free, every property that Clojure usually has to fight the host runtime
for:

- **Immutability by default**: every protoCore object is immutable unless
  explicitly created mutable. A protoClojure `(assoc m :k v)` returns a new
  map through the kernel's structural-sharing collections.
- **Persistent collections**: protoCore ships AVL trees (`ProtoList`),
  tuples (`ProtoTuple`), hash-keyed sparse lists (`ProtoSparseList`), ropes
  (`ProtoString`), and multisets (`ProtoMultiset`). These map onto
  Clojure's persistent lists and vectors, `PersistentHashMap`, persistent
  strings, and `PersistentHashSet` with almost no adapter layer.
- **Compare-and-swap on attributes**: protoCore's `setAttributeIfEqual`
  *is* the primitive that `atom` and `swap!` need. No JVM atomics:
  `swap!` is a short retry loop around one kernel CAS call.
- **GIL-free concurrency**: per-thread allocation arenas, concurrent
  garbage collector, no global lock. Clojure's promise of "use all your
  cores" actually lands.
- **Tagged immediates**: `SmallInteger` (signed 54-bit) is stored in the
  pointer. Arithmetic on small ints does not allocate.

The implementation cost we therefore pay is *interpretation*, *the reader*,
*the standard library*, *the REPL*, and *the macro system* — but **not**
the data model, the memory model, or concurrency.

### 1.1 Building large collections in native code

Immutability is not free on the way *in*. `appendLast` returns a new list
that shares structure with the old one and leaves one root-to-leaf path —
about log2(N) cells — behind as garbage, so growing an N-element list one
element at a time allocates on the order of N·log2(N) cells while only O(N)
of them survive.

That garbage becomes *collectable* only once protoCore has been shown it. A
context's allocations — its young generation — reach the collector when the
context is **destroyed**, or when the embedder calls
`ProtoContext::safepoint()` at a point where every live value is reachable
from a real GC root (an automatic local of a live context, a root set, or
another rooted structure — never a C++ local alone). A native primitive that
appends N times inside one long-lived context therefore offers the collector
nothing until it returns: the heap fills with garbage that is unreachable but
has never been submitted. Under a heap ceiling
(`PROTOCORE_HEAP_LIMIT_CELLS`) such a build runs out of memory with a live
set of a few hundred thousand cells.

**The rule.** A native primitive that builds a structure whose size the
program controls must:

1. **Prefer a bulk construction that never creates the intermediates.**
   `newTupleFromList` builds a whole tuple in one bottom-up pass, and the
   positional arguments a primitive is handed already *are* a `ProtoList` —
   so `(vector …)` and `(list …)` need no rebuild at all.
2. **Where the size is not known ahead, keep the accumulator in an automatic
   local (P1) and let each chunk's young generation be reclaimed as the build
   proceeds**, by folding the elements in inside a short-lived child context.

`src/runtime/ListBuilder.{h,cpp}` implements (2) and is the type to reach for.
`map`, `filter`, `reverse`, `keys` / `vals`, `split`, `pmap`, `send`, the
reader and the VM's argument packing all build through it. Its header
documents the single ordering rule it imposes: a thread's contexts are a LIFO
stack, so while a builder is open every nested call must be passed
`builder.context()`, and the builder must be closed before the caller hands
its own context back to code that pushes contexts.

Two habits follow, and neither is optional: never let a C++ local be the only
reference to a value across an allocation (P1), and never assume that an
object nothing points at has been reclaimed.

## 2. The hybrid module system — UMD as the invisible motor (planned)

This is the most consequential architectural decision in the language.

**The surface form is Clojure.** The Clojure programmer writes:

```clojure
(ns my.app
  (:require [clojure.string :as str]
            [py/numpy :as np]
            [js/d3 :as d3]
            [my.util :refer [fmt]]))
```

**The plumbing is UMD.** Every `(:require ...)` resolves through the same
universal module-discovery chain that protoPython, protoJS, and protoST
share. The `py/` prefix says "resolve this module against the protoPython
side of the UMD registry"; `js/` says "resolve against protoJS"; an
unprefixed namespace resolves through the standard Clojure path (the local
filesystem and any registered Clojure module sources).

The Clojure programmer sees no extra system — `:require` is still
`:require`. The protoCore programmer sees no extra system either — the UMD
registry has one more language consumer.

### 2.1 Why a hybrid is the right call

The other two designs were considered and rejected:

- **"Pure Clojure namespaces, UMD only for cross-lang."** Would force the
  programmer to learn a second system for cross-runtime calls. The whole
  reason to choose protoClojure over JVM Clojure is the interop; making
  the interop feel like a second-class citizen defeats the value.
- **"UMD for everything, no Clojure namespaces."** Would offend the
  Clojure programmer's idiom expectation immediately — `:require`,
  `:as`, `:refer` are deep muscle memory.

The hybrid resolves it: Clojure syntax on top, one resolver underneath.

### 2.2 What this requires from us

- A reader extension recognising the `lang/module` prefix syntax. The
  Clojure reader accepts namespaced symbols already; we use that
  pre-existing rule.
- The UMD registry must return modules in a form that protoClojure can
  dispatch against — that already works (a foreign UMD module is a
  protoCore object with attributes; method invocation is the standard
  attribute walk that protoST and protoJS already use).
- A clear naming convention: `py/X`, `js/X`, `pst/X` (for protoST),
  `clj/X` (when we want to be explicit), unprefixed defaults to Clojure.

### 2.3 What this gives back to the rest of the ecosystem

A protoClojure module is itself a UMD module. A protoPython script can
`import` a `.clj` file and call its functions exactly as it imports a
`.py`. The substrate is symmetric — every language is a producer and a
consumer.

## 3. The reader

A standard Clojure reader, in C++. The lexer produces tokens for:

- Symbols, keywords, numbers, strings, chars, booleans, nil.
- Open/close paren, bracket, brace.
- Quote `'`, quasiquote `` ` ``, unquote `~`, splice `~@`.
- Reader macros: `#{}` (set), `#()` (anonymous fn), `#""` (regex — TBD),
  `#'` (var quote), `#_` (discard).
- Metadata `^{...}` and the shorthand `^kw` / `^Type`.

The lexer and reader implement symbols, keywords, numbers, strings,
booleans, nil, lists, vectors, maps, `@` (read as
`(clojure.core/deref form)`), quote `'`, syntax-quote `` ` `` with `~` and
`~@` (expanded at read time, decision M6); the `#` reader macros, characters
and metadata are rejected with a "reserved-for-later token" error.

Every reader output is a runtime value: the reader yields exactly what a
program manipulates at run time, so code is data with no conversion at any
boundary (decision M1, §10.1). A list is a `ProtoList`; a vector is a
`ProtoList` inside a one-entry `ProtoSparseList` box
(`src/runtime/VectorOps.h`, decision R2); a map literal is the runtime map;
a symbol or a keyword is its interned named value (`src/runtime/Named.h`); a
string is a protoCore string; `nil`, `true` and `false` are the values
themselves. A map literal that repeats a key is a read error, as in Clojure.
The compiler turns a vector literal into a `vector` call and a map literal
into a `hash-map` call, so their elements are evaluated; it evaluates a map
literal's forms in source order, which the reader records on the side
(`src/reader/SourceInfo.h`, decision M3) because the map itself has none.
The runtime map
is an immutable `ProtoMap`, with no wrapper object and no mutable state,
read and written through protoCore's shared hashed-collection helper
(`hashedPut` / `hashedGet` / `hashedRemove` / `hashedForEach`) with one
`KeySemantics` (`src/runtime/MapOps.h`):

- **Slots.** A key whose Clojure map-key equality is pointer identity —
  keywords, symbols, `nil`, booleans, atoms, functions, futures, promises,
  actors — is its own slot key. Every other key goes under a slot keyed by
  the low 54 bits of `keyHash`, whose value is a flat `ProtoList` `[k v]`,
  or `[k0 v0 k1 v1 …]` when two keys collide in those 54 bits. Both kinds
  hold the original key object, which is what walks return.
- **Key semantics.** `keyIsIdentity` / `keyHash` / `keyEquals` implement the
  table of `LANGUAGE.md` §4.3: strings by content whatever form protoCore
  stores them in, doubles by their exact bit pattern, big integers by value,
  lists and vectors element by element (so a list and a vector with equal
  elements are one key), maps by their entries with each value matched as a
  key would be. Each class carries its own hash salt, so keys of different
  classes are never confused. The two slot kinds cannot collide either: a
  hashed slot key is always a SmallInteger word and an identity slot key
  never is.
- **Costs.** `get` is one `keyHash` plus a `getAt`; `assoc` and `dissoc` are
  a probe plus a `setAt` / `removeAt`. `count` is O(n) (D25): a collision
  slot holds more than one entry, so the slot count would undercount.
  `empty?` is the O(1) slot count, because a slot always holds at least one
  entry.
- **Order and equality.** Walks follow the ascending slot-word order — an
  address for an identity key, a hash for every other — so iteration order is
  unspecified and varies between runs, which is what Clojure guarantees. Map
  `=` walks one map and probes the other.
- **Memory.** Nothing is interned: a key is stored as the object the caller
  passed and dies with the last map that holds it. The interned canonical-key
  layer this replaced kept every key alive for the life of the process
  (deviation D23, withdrawn).

The user-facing rules are in `LANGUAGE.md` §4.3. Symbols and keywords are
interned named values, never strings (`src/runtime/Named.h`).

The reader is the place where "code is data" becomes literal: the result
of `(read-string "(+ 1 2)")` is a real `ProtoList` of three elements that
the same code path can evaluate.

## 4. The compiler / evaluator

A single-pass compiler that lowers reader output to bytecode for a small
stack VM. The VM is conceptually the same shape as protoST's
ExecutionEngine — frames with operand stack and locals, opcodes for
push / pop / lookup / call / branch / closure-make / try-catch — with
Clojure-specific opcodes for:

- **`recur`**: a tail-only opcode that jumps to the enclosing `loop` /
  `fn` recur point without growing the C++ call stack. Necessary because
  protoCore (rightly) does not assume TCO at the host C++ level.
- **`var-get` / `var-set`**: a Clojure `def` produces a *var* — a mutable
  protoCore object whose `__value__` attribute holds the bound value.
  Var lookup is a fast attribute read on the namespace object.
- **Anonymous function creation** as a closure-over-locals — same
  mechanism as protoST blocks and protoPython lambdas.

In 0.0.1 the VM has 33 opcodes (`src/runtime/Opcodes.h`). `recur`
compiles to a backward jump (`JUMP_BACK`); `def` stores directly into a
single globals object (`STORE_GLOBAL` / `PUSH_VAR`); closures use
`MAKE_FN` / `MAKE_FN_MULTI`; SmallInteger arithmetic and comparison have
fast-path opcodes; `try` / `throw` use the four exception opcodes of §4.1.
Vars as objects are planned.

The compiler does NOT do whole-program analysis. Each top-level form
compiles independently — the REPL relies on this, and the JVM Clojure
does it the same way.

**The evaluator thread.** `main` creates the `ProtoSpace` and runs the
script driver or the REPL on a protoCore thread of it
(`runOnEvaluatorThread`, `ProtoSpace::newThread`), the same kind of thread
as futures, `pmap` and actor workers, and joins it before the space is
destroyed. Every non-tail call nests the VM on the native stack, so all of
these threads need the same large stack: `configureThreadStacks` asks
protoCore for 32 MiB (`ProtoSpace::setThreadStackBytes`, honoured on macOS
since protoCore 2.8.0 and everywhere since 2.9.0) and, for an older
protoCore, raises glibc's default thread stack size; on Windows
`protoclj.exe` is also linked with a 32 MiB `/STACK` reservation, the
default of any thread created without a size. The runtime creates no
thread with a native API: the evaluator, futures, actor workers and the
HTTP server's threads are all ProtoThreads. What remains native, and why:
the stack guard reads the thread's stack bounds from the platform
(`pthread_getattr_np`, `pthread_get_stackaddr_np`,
`GetCurrentThreadStackLimits`), since protoCore reports a stack's size
but not its address; the main thread blocks POSIX signals while it joins
the evaluator, so they reach the evaluator; and the actor scheduler sizes
its pool with `std::thread::hardware_concurrency`. Waits poll through
protoCore's `ProtoThread::yield` / `sleepFor` (3.1.0), which take the thread
out of the stop-the-world quorum for the wait: `deref` of a promise sleeps
1 ms between polls, a reader of a lazy seq another thread is realising
yields and then sleeps (`waitABit`, `src/runtime/LazySeq.cpp`), and the
`PROTOCLJ_GC_STATS=1` diagnostic's sampler is a ProtoThread that sleeps
200 µs between reads.

### 4.1 Exceptions

The user-facing rules are in `LANGUAGE.md` §14; the code is
`src/runtime/Exceptions.{h,cpp}`, the `try` / `throw` branches of
`src/compiler/Compiler.cpp` and the handler loop of
`ExecutionEngine::executeFrame`.

The runtime follows protoCore's unified exception model (protoCore
`docs/EXCEPTIONS.md`): every language-level throw is a C++ `throw` of a
`proto::ProtoException` carrying the thrown value, every handler catches
that base type and selects by the value's prototype chain, and protoCore's
own errors arrive through an error factory as the language's classes.

**Representation.** Each built-in exception class is a protoCore
prototype object, created at start-up and kept alive as a hidden
attribute of the globals namespace. Its parent is its superclass
(`ArithmeticException` → `RuntimeException` → `Exception` → `Throwable`),
and it carries `__ex_class__`, its id in the built-in table as a
SmallInteger, and `name`, its simple name (the attribute other languages
and modules read). An exception is an immutable child of its class with
three attributes: `__ex_message__`, a string or nil; `__ex_data__`, the
data map of an `ex-info`, or nil; `__ex_cause__`, the cause or nil. The
class id is inherited from the class, so the test of a built-in class
stays an integer walk of at most five parent links, and any other type is
tested with protoCore's `isInstanceOf` over the same chain.

**Classes.** protoClojure has no Java classes (D2), so the built-in
classes a `catch` names mirror the Java ones a Clojure programmer
catches. The simple name and the qualified Java name are both accepted
(`ArithmeticException`, `java.lang.ArithmeticException`,
`clojure.lang.ExceptionInfo`). `Object` (`java.lang.Object`) is accepted
too and, like `Throwable`, catches every thrown value. A name that is no
built-in class is a type resolved at run time (below).

```
Throwable
├── Exception
│   ├── RuntimeException
│   │   ├── ExceptionInfo                      (clojure.lang)
│   │   ├── ArithmeticException
│   │   ├── ClassCastException
│   │   ├── IllegalArgumentException
│   │   │   ├── ArityException                 (clojure.lang)
│   │   │   └── NumberFormatException
│   │   ├── IllegalStateException
│   │   ├── IndexOutOfBoundsException
│   │   │   └── StringIndexOutOfBoundsException
│   │   ├── NullPointerException
│   │   └── UnsupportedOperationException
│   ├── IOException                            (java.io)
│   │   ├── FileNotFoundException              (java.io)
│   │   ├── SocketException                    (java.net)
│   │   │   └── ConnectException               (java.net)
│   │   ├── SocketTimeoutException             (java.net)
│   │   └── UnknownHostException               (java.net)
│   └── ExecutionException                     (java.util.concurrent)
└── Error
    └── StackOverflowError
```

Unmarked classes are in `java.lang`. Two links are shortened against
Java: `SocketTimeoutException` hangs directly under `IOException` (Java:
via `InterruptedIOException`) and `StackOverflowError` directly under
`Error` (Java: via `VirtualMachineError`). The I/O classes exist before
any I/O primitive does, so that the I/O layer can raise them.

**Foreign and module error types.** A module written in C++ or in
another language raises a `ProtoException` whose value is an instance of
its own error prototype (protoCore's example geometry module raises a
child of its `NegativeRadius`, itself a child of `GeometryError`). A
Clojure program reaches the module with `import-module` and names the
module's types as members of it:

```clojure
(def geometry (import-module "example/geometry"))
(try (geometry/circleArea -1)
     (catch geometry/NegativeRadius e (ex-message e)))
```

A `catch` that names no built-in class compiles to a run-time lookup of
the type: a local, a var (`(catch NR e ...)` after `(def NR
geometry/NegativeRadius)`), or member `b` of the value of `a` for `a/b`.
**Timing:** the type is looked up when an exception reaches the clause,
after the clauses before it did not match, and on every such arrival;
JVM Clojure resolves the class at compile time. A `try` whose body
completes, or whose exception an earlier clause handles, never looks the
type up. The lookup never fails silently: a name that resolves to nothing
raises `RuntimeException: Unable to resolve classname: X`, and a value
that is not a prototype object (a number, a string, a collection, an
atom, nil) raises `IllegalArgumentException: catch: X is an integer, not
an exception type`; either error replaces the exception being matched.
The clause matches when the type is on the thrown value's prototype
chain.

Which built-in classes catch a foreign value: `Throwable` and `Object`
catch every value; `Exception` catches every value that is not a Clojure
`Error` (a module's error is treated as a library's exception, which JVM
Clojure would also catch with `Exception`); the other built-in classes
catch only their own instances. `throw` accepts a Clojure exception or a
foreign object (any protoCore object that is no Clojure value), so a
caught module error can be rethrown; any other value raises
`ClassCastException` as before. `ex-message` answers a foreign object's
`message` attribute when it is a string; `ex-data` and `ex-cause` answer
nil for it.

**Bytecode.** A frame keeps a stack of active handlers, each a code
position and an operand-stack depth. `TRY_BEGIN` pushes one, `TRY_END`
pops it on the normal path, `THROW` raises the value on top of the stack
(`throw`), `STORE_CAUGHT <slot>` starts a handler by storing the caught
value in a slot and keeping the caught C++ exception for that slot,
`RETHROW <slot>` rethrows that original exception (a value no clause
matched, the end of a finally handler), `EXC_MATCH <class>` tests a value
against a built-in class, and `EXC_MATCH_TYPE` tests it against a type
pushed at run time (`RESOLVE_CLASS <name>`, or a local, or `GET_MEMBER`).
`(try body (catch C1 e h1) (catch C2 e h2) (finally f))` compiles to

```
    TRY_BEGIN Lfinally          ; only with a finally clause
    TRY_BEGIN Lcatch            ; only with catch clauses
    <body>
    TRY_END
    JUMP Ldone
Lcatch:                         ; stack: the exception
    STORE_CAUGHT ex
    PUSH_LOCAL ex; EXC_MATCH C1; JUMP_IF_FALSE L2; <h1>; JUMP Ldone
L2: PUSH_LOCAL ex; RESOLVE_CLASS m/T; EXC_MATCH_TYPE; JUMP_IF_FALSE L3; <h2>; JUMP Ldone
L3: RETHROW ex                  ; no clause matched: the original exception
Ldone:
    TRY_END                     ; the finally handler
    <f>; POP                    ; normal path
    JUMP Lend
Lfinally:                       ; stack: the exception
    STORE_CAUGHT fx
    <f>; POP
    RETHROW fx
Lend:
```

(here C1 is a built-in class and C2 the module member `m/T`). The rethrow
is protoCore's rule E3, "rethrown unchanged": the handler loop's
`catch (const proto::ProtoException&)` keeps `std::current_exception()`,
`STORE_CAUGHT` files it under the handler's slot in a per-frame vector of
`std::exception_ptr` (frame state, never global or thread-local), and
`RETHROW` calls `std::rethrow_exception` on it. The very exception object
continues, so a runtime subclass of `ProtoException` raised by another
language keeps its dynamic type and its extra state, and the value its
identity, across a Clojure `try` that does not handle it
(`tests/unit/test_rethrow.cpp`). A handler entered for a translated C++
error (`exceptionFromError`) has no original to rethrow; `RETHROW` raises
its value as a new `ProtoException`.

The finally handler encloses the catch clauses too, so `finally` runs
after a handled exception and when a catch clause throws; its forms are
compiled once per path, as javac does. The catch binding names the
exception's slot for the clause body only. A top-level `try` compiles as
`((fn [] (try ...)))`, because the top level has no local slots. Clojure
has no early return, so normal completion and an exception are the only
two ways out of a `try`; `recur` across a `try` is the compile error
`Cannot recur across try` (the handler it would leave behind is frame
state a backward jump does not undo), tracked by a per-scope try depth
recorded on every recur target.

**Unwinding.** Calls nest natively (every call is a nested
`executeFrame`), so an exception travels up the native stack as a C++
exception. Every body that contains a `try` runs in `executeFrame<true>`,
whose dispatch loop sits inside a C++ `try`: an exception escaping any
instruction — raised by the VM, by a primitive or by a nested call —
lands there. With no active handler it leaves the frame; otherwise the
innermost handler is popped, the operand stack is cut back to its depth,
the exception value is pushed and the loop resumes at the handler code.
A body without a `try` runs in `executeFrame<false>`, which has no catch
region at all: with the region in every frame `fib.clj` ran +3.5 % cycles,
and with the split the benchmarks are at parity with the VM before
exceptions (`perf stat -r 5`, fib / tak / sum-loop).

**GC safety.** While an exception unwinds, the frames it leaves are
destroyed and hand their young generations to the collector, and those
may be the only thing keeping the exception, its message and its data
alive. protoCore pins a `ProtoException`'s value in a root set of its
space from its construction until its last copy is destroyed; the frame
that catches it pushes the value into a slot first. protoClojure no
longer keeps a pin of its own. The unit test
`ExceptionsFixture.InFlightExceptionSurvivesCollections` checks it.

**protoCore's errors.** protoCore raises its own errors as
`ProtoException`s whose value the space's error factory builds.
`initExceptions` installs a factory once, at start-up, that builds the
Clojure exception the error would be had Clojure code raised it, reaching
the class prototypes through the globals namespace:

| protoCore `ProtoErrorKind` | Clojure class | Message |
| -------------------------- | ------------- | ------- |
| `DivisionByZero` | `ArithmeticException` | `Divide by zero` (JVM Clojure's) |
| `Overflow` | `ArithmeticException` | protoCore's |
| `TypeMismatch` | `ClassCastException` | protoCore's |
| `ArityMismatch` | `ArityException` | protoCore's |
| `InvalidArgument`, from `Integer::fromString` (a malformed literal) | `NumberFormatException` | protoCore's |
| `InvalidArgument`, any other (a base outside 2..36, a negative shift) | `IllegalArgumentException` | protoCore's |

The parse case is told apart by protoCore's message (it names
`fromString`); both classes are `IllegalArgumentException`s. Clojure's `/`
on integers leaves the zero divisor to protoCore, so `(/ 1 0)` is the
factory's `ArithmeticException`.

**Errors raised by C++.** Primitives and the VM raise classed exceptions
as `ProtoException`s: directly with `throwClassed`, which takes an
optional data map and cause, or through `throwError`, which reads the
class from a message that starts with a class name (`ArithmeticException:
Divide by zero`) and otherwise raises a `RuntimeException` carrying the
whole text (D26).

```cpp
[[noreturn]] void throwClassed(proto::ProtoContext* ctx, const char* className,
                               const std::string& message,
                               const proto::ProtoObject* data = nullptr,
                               const proto::ProtoObject* cause = nullptr);
```

A C++ exception that is not a `ProtoException` (`std::exception`) is a
module defect, not a language value. A module member called as
`(m/f ...)` that throws one raises a `RuntimeException` carrying its
`what()` at that call; elsewhere (a `StackOverflowError` from the stack
guard, a few internal errors) the handler loop makes the same translation
(`exceptionFromError`, which also reads a class-name prefix).

**Top level.** The script driver reports an uncaught value on standard
error as `<file>: runtime error: <text>` and exits with status 1, where
the text is a Clojure exception's toString (`ExceptionInfo: boom {:a
1}`), or a foreign error's `name` and `message` attributes
(`NegativeRadius: circleArea: the radius -1.000000 is negative`), or the
value's printed form; a `std::exception` reports its `what()`, and any
other C++ exception `unknown C++ exception`. The REPL reports the same
text after `error:` and reads the next form. An HTTP handler's uncaught
exception is reported the same way and answered with a 500.

**Threads.** A future's or a `pmap` element's worker records the thrown
value on the future (still inside its `catch`, while protoCore's pin
holds); `deref` then raises an `ExecutionException` whose cause is that
value and whose message is the cause's text, as JVM Clojure does. A
foreign error is recorded and raised the same way. An exception escaping
an actor's message handler is caught by the worker, and the actor's state
becomes `nil` (STATUS.md, Known issues); a handler that catches its own
exceptions keeps its state. An exception raised by an atom's watch
function is discarded (STATUS.md, Known issues). Both apply to every
thrown value, a `ProtoException` included.

### 4.2 Input and output

The user-facing reference is `LANGUAGE.md` §18; the code is
`src/runtime/IO.{h,cpp}`, the `with-open` branch of
`src/compiler/Compiler.cpp` and the argument handling of `src/main.cpp`.

**protoIO does the I/O.** The POSIX layer — buffered descriptors that may
be shared between threads, SIGPIPE handling, TLS, child processes, sockets
and the HTTP/1.1 message layer and client — is the protoIO library, shared
with protoST and protoScala and linked statically (`CMakeLists.txt` finds
an installed package or builds the sibling `../protoIO`). `IO.cpp` only
binds it, in three steps per primitive:

1. arguments are read and copied into C++ values (`std::string`, `int`,
   `protoio::http::Request`, ...);
2. the protoIO call runs inside `ProtoContext::UnmanagedScope`, touching no
   protoCore object, so a thread blocked on a file, a pipe, a child or a
   peer never holds up a collection (`blocking()` in `IO.cpp`);
3. the result is built into protoClojure values after the scope has been
   left; a `protoio::Error` caught there becomes a classed exception
   (`throwClassed`, §4.1) with the ex-data map `{:type <kind> :errno n}`.

| protoio::Error kind | Class | `:type` |
|---|---|---|
| FileNotFound | `FileNotFoundException` | `:file-not-found` |
| FileExists, FileSystem, Process, LineTooLong, BodyTooLarge | `IOException` | `:file-exists`, `:file-system`, `:process`, `:line-too-long`, `:body-too-large` |
| ConnectionRefused | `ConnectException` | `:connection-refused` |
| ConnectionTimedOut | `SocketTimeoutException` | `:timeout` |
| NameLookup | `UnknownHostException` | `:unknown-host` |
| Network | `SocketException` | `:network` |
| InvalidArgument | `IllegalArgumentException` | `:invalid-argument` |

**Handles.** Files named with `file`, readers, writers, sockets,
listening sockets, UDP sockets and HTTP servers are protoCore objects whose
prototype is one IO marker (kept alive as a hidden attribute of the
globals, like the exception marker). Their attributes are the kind, the
path or address, and the descriptor. A file handle is immutable; the
others are mutable, and `close` swaps the descriptor for -1 with
`setAttributeIfEqual`, so exactly one caller closes a descriptor however
many threads race to close the handle. There is no finaliser: a handle the
program drops without closing keeps its descriptor until the process ends,
which is why `with-open` exists.

**`with-open`** is a macro in Clojure; protoClojure has no macros yet, so
the compiler performs the same rewrite,
`(with-open [a ia b ib] body)` → `(let [a ia] (try (with-open [b ib] body)
(finally (__with_open_close__ a))))`, closing in reverse order.
`__with_open_close__` is the `close` primitive under a name a program
cannot shadow by defining its own `close`. A top-level `with-open` is
compiled as `((fn [] ...))`, like a top-level `try`.

**The HTTP server and the blocking decision.** The spec proposed serving
requests "on actors". The actor pool is a fixed set of workers
(`PROTOCLJ_ACTOR_WORKERS`, default `max(2, cores - 2)`), and a request
spends most of its life blocked: waiting for the request head, for the
body, for a slow client to take the response. On actors, a handful of slow
or idle connections would occupy every worker and stop every actor in the
program — the starvation protoST avoids by growing its pool while a worker
is blocked (`BlockingIO`). protoClojure's scheduler has no such accounting,
and adding it would change the actor model for a problem actors do not
have. Decision (2026-09-30): the server does not use the actor pool.

- `run-server` binds the listening socket and starts one **accept thread**,
  a ProtoThread (`ProtoSpace::newThread`), which blocks in
  `protoio::net::tcpAccept` inside an `UnmanagedScope`.
- Each accepted connection gets a **connection thread** of its own, also a
  ProtoThread, which reads the request with protoIO's limits (400, 414,
  431 and 413 are answered before the handler runs), builds the Ring
  request map in a per-request child context, calls the handler through the
  ExecutionEngine with the ActiveCallContext of the thread that started the
  server, converts and validates the response (a header value with a line
  break, or an answer that is not a map, becomes a plain 500; a handler
  exception becomes a plain 500 and a report on standard error), writes
  it and closes. `connection: close` — one request per connection.
- No native thread API is used: both kinds of thread are ProtoThreads,
  joined with `ProtoThread::join`, and `stop-server` waits for the accept
  thread to leave its accept call (Windows, macOS) on a condition variable
  the accept thread notifies, not by sleeping. The accept thread joins the connection
  threads that have finished each time it wakes, and all of them when it
  stops; `stop-server` joins the accept thread.
- **Stopping.** Only the thread that owns a descriptor ever closes it, so a
  descriptor number is never released under a thread still using it and
  then reused by an unrelated `open`. `stop-server` therefore does not close
  anything: it shuts the listening socket down (which wakes the blocked
  accept) and shuts down the connections still waiting for their request,
  under the per-connection mutex that the owner holds when it closes;
  requests already in their handler finish and are answered. A handler may
  stop its own server: the call returns at once (the join would wait for
  the calling thread itself), and `shutdownIO` joins the threads at exit.
- **GC.** The handler is pinned in a `ProtoRootSet` for as long as the
  server runs. A connection thread's request map, the handler's garbage
  and the response live in a child context destroyed when the request
  ends, so a long-running server's garbage is collected
  (`tests/cli/http-server-under-heap-limit.sh`: 400 requests under a
  200,000-cell ceiling, about 20 collection cycles).
- **Exit.** `shutdownIO` stops every server still running before
  `shutdownFutures` and the actor scheduler's shutdown, in the script
  driver and the REPL, since a handler may be using futures and actors. A
  script that wants to keep serving blocks its main thread, for example on
  `@(promise)`.

Blocking I/O called from user code (an `http-get` inside an actor's
handler, a `slurp` inside a future) runs on the calling thread and is
bracketed like every other call: it never holds up a collection, but an
actor worker blocked in I/O is unavailable to other actors until it
returns, as a JVM agent's `send` pool thread is. Long blocking work
belongs in a `future`, which gets its own thread.

**Processes.** `sh` is `protoio::process::run`, which feeds `:in` and
drains standard output and error together without SIGPIPE. `:dir` and
`:env` are `RunOptions::directory` and `RunOptions::environment` (protoIO
0.2.1): the spawn call itself sets the child's working directory and
replaces its environment (posix_spawn's file actions and `envp`;
`CreateProcessW`'s directory and environment block), so no shell or `env`
program is involved and both work on a stock Windows. The program is
searched in protoclj's own `PATH`, as the JVM's `ProcessBuilder` searches
it, so `:env` need not carry `PATH`; on Windows protoIO adds `SystemRoot`
when `:env` lacks it, as the JVM does. A `:dir` that does not exist is
checked up front, for the JVM's message.

## 5. Namespaces and vars (planned)

A namespace is a protoCore object. Its attributes are vars (or aliases to
vars). A var is a small mutable protoCore object with `__value__` and
optionally `__meta__`. `def` interns a symbol into the current namespace
and points the var at the value.

This matches the JVM Clojure semantics exactly:

- A var is a mutable indirection. Code references the var, not the value
  directly, so redefining a function in the REPL updates every caller
  without re-compilation.
- A namespace is just a map from symbol to var, with some metadata
  (aliases, refer imports). It IS the map; an `intern` is a `setAttribute`.

We get this for free from protoCore's mutable-object model with CAS.

## 6. Atoms, refs, and STM

**Atoms** are a one-liner, and they are implemented. A
`clojure.core/atom` is a mutable protoCore object with `__value__`.
`swap!` is a CAS loop using `setAttributeIfEqual`. `deref` / `@` is an
attribute read. Watches live in a `__watches__` map on the atom. The
exact same primitive protoST exposes in its `Atom` class.

**Refs and STM** are in scope for v0.2, not v0.1. The implementation path
is: each ref carries a (value, version) pair; a transaction collects a
read-set and a write-set; commit is a multi-CAS over `setAttributeIfEqual`
on every (ref, expected-version) pair. The protoCore per-cycle
mutable-shard snapshot in the GC (`335ef608`) is suggestive of how the
read-set can be made consistent across allocation cycles. This is non-trivial
but mechanical. v0.2.

**Agents** are deferred to v0.3. protoClojure 0.0.1 already ships a
protoCore-native `actor` — a worker pool, three priority bands, a
single-method invariant and three protoCore `ProtoMPSCQueue` mailboxes per
actor, one per band — whose `send` returns a promise. A message is a
three-element `ProtoList` of the function, its arguments and the promise, and
the three queues hang off the actor's (mutable, and therefore root) wrapper
object, so the collector traces every queued message. The Clojure `send` / `send-off` agent API has
scheduling semantics we want to align with that actor model
thoughtfully.

## 7. Lazy sequences

Built in the macros branch, Part B stage 11; the decisions are M28-M31
(§10.2). In short:

- A lazy seq is a protoCore **lazy object** (`ProtoObject::newLazyChild`,
  protoCore 3.1.0): an immutable object whose lazy slot holds the thunk and,
  once realised by compare-and-set, the seq it produced. A cons and a chunk
  are lazy objects published at birth (`src/runtime/SeqTypes.h`,
  `src/runtime/LazySeq.h`).
- `map`, `filter`, `remove`, `take`, `take-while`, `drop`, `drop-while`,
  `iterate`, `repeat`, `repeatedly`, `cycle`, `range`, `concat`, `mapcat`,
  `partition`, `partition-all`, `interleave`, `distinct`, `for`, `line-seq`
  and `re-seq` are lazy, as in Clojure, and chunked where Clojure chunks
  (the seq of a vector, a finite range, and what map, filter and for make of
  those).
- Every sequence function reads through one protocol (`seqOf`, `seqFirst`,
  `seqMore`) and walks with a cursor that holds only its position
  (`SeqWalk`), so a walk over a seq nobody else holds keeps nothing it has
  passed; locals clearing and argument-slot clearing make "not holding the
  head" work as Clojure programmers expect (M30).

## 8. Protocols and multimethods (planned)

**Protocols** map onto prototype attributes. A `defprotocol` declares a
set of method names. `extend-type` adds attributes (the methods) onto the
class object. Dispatch is a normal attribute read on the receiver — the
same `getAttribute` chain walk every other language on protoCore uses.
This is *one of the places where protoClojure is closer to its substrate
than JVM Clojure* — JVM Clojure synthesises an interface; here we just
set attributes.

**Multimethods** are a *separate* dispatch system, by design. A
`defmulti` creates a multimethod object holding (dispatch-fn,
method-table). `defmethod` adds an entry to the table. Calling the
multimethod evaluates the dispatch function on the args, looks up the
table (with hierarchy fallback via `isa?` and `derive`), and invokes.
The method table is a `ProtoSparseList`; the hierarchy is a small graph
of derive relationships, also stored as protoCore attributes.

Multimethods are the place where protoClojure is *farther* from its
substrate than the rest. That's fine — Clojure's whole multimethod design
is a deliberate departure from class-based dispatch. The implementation
is mechanical.

## 9. The REPL

A respected REPL is a v0.1 hard requirement. Two layers:

- **The read-eval-print loop itself**: read one form, eval, print,
  loop. Implemented on libreadline (`src/repl/Repl.cpp`), with
  multi-line input, history, `*1` / `*2` / `*3` and the `:help`,
  `:quit`, `:load` and `:time` commands. Every form of an input is read,
  compiled and run in turn (several on one line print one result each), in
  a context of its own, as a script's forms are (decision M9).
- **nREPL server** (planned for v0.1): a TCP server speaking the
  [nREPL protocol](https://nrepl.org/) so CIDER / Calva / Conjure can
  connect. The protocol is bencode over a socket; the operations a v0.1
  needs to support are `eval`, `load-file`, `interrupt`, `describe`,
  `clone`, `close`. Pretty-printing, completion (`complete`), and lookup
  (`info`) are stretch but high-value.

The implementation cost of nREPL is the highest in the v0.1 plan — bencode
parser, an op dispatcher, session management. We accept it because
without it the language fails the *respect* test.

## 10. Macros (in progress)

Status: stage 1 (the reader yields runtime values), stage 2 (quote and
syntax-quote), stage 3 (scripts compiled and run form by form), stage 4
(`defmacro`, expansion, `macroexpand-1`, `macroexpand`, `eval`), stage 5
(qualified symbols and auto-gensyms in syntax-quote) and stage 6 (the
`clojure.core` macros in a bootstrap `core.clj`) are built (ROADMAP.md,
"Macros"). The plan below is the original one; §10.1 records the decisions
taken while building it.

Macros are functions that take and return reader output (data). The
compiler expands them at compile time: when it sees a symbol that resolves
to a macro var, it calls the macro with the unevaluated args, takes the
result, and compiles that.

The implementation requirement: macros must be *defined* in protoClojure
itself (most of `clojure.core` is macros — `when`, `if-let`, `cond`,
threading macros). That means the compiler must be able to evaluate
protoClojure code at compile time, which means the compiler must be
*itself* a protoClojure program from a certain point onward. The natural
sequencing is:

1. v0.1-a: compiler is C++ only. Bootstraps `clojure.core` without using
   macros it defines. The C++ side includes a small set of special forms
   and built-in macros (`fn`, `let`, `do`, `if`, `quote`, `def`).
2. v0.1-b: `clojure.core` macros are defined in protoClojure. The C++
   side can call into the just-compiled evaluator.
3. v0.2+: user macros work because user code runs through the same path.

This is the standard Lisp bootstrap. The complication is the protoCore
GC interaction during macro expansion — we have to be careful that the
unevaluated forms (which are data, but are protoCore objects) stay rooted
across calls into user code. The existing GC discipline patterns
(TransientPin, ProtoRootSet) carry over.

### 10.1 Decisions

The maintainer is not a Clojure programmer: the rule is to follow Clojure
by default. Each decision below is marked with who took it. Those marked
*agent, pending review* were taken by the implementing agent and wait for
the maintainer's review; each lists the alternatives considered.

**M1. The reader yields runtime values** (*maintainer*). The reader
produces the values the runtime manipulates: a symbol or a keyword is its
interned named value (`runtime/Named.h`), a string a protoCore string, a
vector and a map the runtime's own, `nil` / `true` / `false` the values
themselves, a list a `ProtoList`. The compiler consumes those values. There
is no conversion at the reader/compiler boundary, and none at the
compiler/macro boundary later: a macro receives and returns the same values
a program builds with `list`, `vector`, `hash-map` and quote.
As built (*agent, pending review*): the marker prototypes that used to wrap
strings, vectors and maps between the reader and the compiler
(`stringMarkerProto`, `vectorMarkerProto`, `mapMarkerProto` and their
`__bytes__` / `__items__` / `__entries__` keys) are gone. The value
representation (`Named`, `VectorOps`, `MapOps`) moved from the runtime
library into `protoclj_support`, so the reader still does not depend on the
runtime; the alternative, linking the reader against the whole runtime, was
rejected because it would pull the VM and the I/O layer into every reader
user. Symbols and keywords are now interned when they are read rather than
when they are compiled; since the print order of a map keyed by keywords
follows their addresses (D24), such a map may print its entries in another
order than before for the same program. That order was already unspecified.

**M2. A repeated key in a map literal is a read error** (*agent, pending
review*), `Duplicate key: :a`, as in Clojure. It follows from M1: the reader
builds the map, and a map cannot hold the key twice. Alternative: keep the
previous behaviour, the last value wins, by building the map with the last
value; rejected because it differs from Clojure and silently drops a form
(with its side effects). This is a behaviour change against 0.0.3, where
`{:a 1 :a 2}` was `{:a 2}`.

**M3. A map literal's forms are evaluated in source order** (*agent,
pending review*). A runtime map has no order (D24), so the reader records
each map literal's keys in source order in a side table, `SourceInfo`
(`src/reader/SourceInfo.h`), keyed by the map it yielded, and the compiler
compiles the literal in that order. This keeps 0.0.3's behaviour and
Clojure's: `{:a (f) :b (g)}` calls `f` before `g`. A map the reader did not
yield (one a macro builds) is compiled in its walk order, as Clojure
compiles a hash map. The table holds raw pointers and is valid only while
the forms it describes are alive, so each driver keeps one per unit it reads
and compiles. Alternatives: (a) evaluate in the map's walk order, which is
Clojure's rule for maps over 8 entries but here applies to every literal and
changes with the keywords' addresses, so side effects would run in an order
that depends on what was interned earlier; rejected as a silent behaviour
change; (b) an ordered map type for literals; rejected because it would be a
second map representation, against M1.

**M4. `nil`, `true` and `false` are read as values** (*agent, pending
review*), as Clojure's reader does. Before, they were symbols the compiler
recognised by name. Consequence: `(quote nil)` is now `nil`, as in Clojure,
where it was the symbol `nil`.

**M5. A quoted collection is a pinned constant** (*agent, pending
review*). `(quote form)` of a list, vector or map compiles to a `Datum`
constant holding the very value the reader yielded (or a macro built);
`PUSH_CONST` pushes it, so every evaluation answers the same immutable
object, at the cost of one pointer read, as in Clojure. The module pins its
data in a `ProtoRootSet` of its own, created on the first datum and
destroyed with the module (`src/runtime/BytecodeModule.h`), so a module that
holds data must die before its `ProtoSpace`; every driver already orders it
so. Alternative: compile a quoted collection into the calls that build it
(`(list (quote a) ...)`), which needs no rooting and matches how string
constants are rebuilt on every execution; rejected because it allocates on
every evaluation of a quoted constant (a quoted table inside a loop) and
because the calls would resolve `list` / `vector` / `hash-map` at run time,
where a program can redefine them. Quoted symbols and keywords stay `Named`
constants (interned, never collected); quoted strings and numbers are
literals.

**M6. Syntax-quote is expanded by the reader** (*agent, pending review*),
as Clojure's `LispReader` does: `` `(f ~x ~@ys) `` reads as
`(clojure.core/seq (clojure.core/concat (clojure.core/list (quote f))
(clojure.core/list x) ys))`, a vector template is applied to
`clojure.core/vector` and a map template to `clojure.core/hash-map`; a
nested syntax-quote is expanded first and its expansion quoted as data.
This gives Clojure's `macroexpand` output and needs nothing from the
compiler. Alternative: a `syntax-quote` special form the compiler expands
into bytecode; rejected because `'`\`form` and `macroexpand` would then show
a form no Clojure programmer recognises. Stage 5 added the qualification of
symbols and the auto-gensyms (M17, M18). `~`, `~@` and `@` read as
`clojure.core/unquote`, `clojure.core/unquote-splicing` and
`clojure.core/deref`, as in Clojure. `seq` and `concat`, which the expansion
calls, were planned for stage 5 and were added here; both answered lists,
eagerly. Since stage 11 (M29) `seq` answers Clojure's seq of its argument and
`concat` is lazy when an argument is a lazy seq, and builds the list at once
when every argument is an already realised collection, so the expansions
above cost what they did. `apply` now spreads a vector or `nil`,
which a vector template needs (`` `[] `` is `(apply vector (seq (concat)))`).

**M7. `clojure.core/x` names the built-in `x`** (*agent, pending review*).
Without namespaces (D28) the built-ins are the runtime's globals, so the
compiler resolves `clojure.core/x` as the global `x`, never as a local, and
dispatches `clojure.core/if` and the other special forms as the unqualified
form. That keeps a syntax-quote expansion correct inside
`(let [list 1] ...)`. Since stage 5 `clojure.core/x` reads its own globals
key, which a `def` of `x` does not replace (M19). Behaviour change against 0.0.3: `@x` now calls the built-in
`deref` even where a local is named `deref`, as in Clojure.

**M8. `~` and `~@` outside a syntax-quote are compile errors** (*agent,
pending review*): `unquote (~) used outside a syntax-quote (`)`. JVM
Clojure compiles them as calls of the unbound var `clojure.core/unquote`
and fails only at run time, with `Attempting to call unbound fn`; the brief
asks for a clear message, and the earlier report costs nothing (D36). `~@`
directly under a syntax-quote, `` `~@x ``, is the read error
`splice (~@) not in list`, Clojure's. At the REPL, a line that ends in a
reader macro (`'`, `` ` ``, `~`, `~@`, `@`) is incomplete: the REPL waits
for the form on the next line, as Clojure's REPL does.

**M9. A script is read, compiled and run one top-level form at a time**
(*agent, pending review*), as the REPL does and as Clojure's `load` does:
the next form is read only after the previous one has run, so a definition
made at run time (a `defmacro`, from stage 4) is in effect for the forms
after it. Each form is read into a context of its own, rooted in a slot of
that context while it is compiled and run, and is garbage once it has run
unless something reachable from the globals holds it; each form gets its own
`BytecodeModule`, kept until the run ends (the functions it defined point
into it) and its own `SourceInfo`. One `ExecutionEngine` serves the whole
run. The REPL and `:load` go through the same per-form loop
(`Session::evalSource`). Consequences, all as in Clojure: the forms before a
failing one have run and printed; a read error late in a file no longer
prevents the earlier forms from running; the exit status is still 1 on any
error. A compile error now names the line and column where its top-level
form starts (`script.clj:3:1: compile error: ...`; the read error already
named its position, the runtime error format is unchanged). The REPL used
to evaluate only the first form of an input line and drop the rest; it now
evaluates them all and prints each result. Alternative: keep reading the
whole file first and compile each form after the previous one ran; rejected
because it holds every form alive for the whole run, reports a read error
before running anything, and differs from Clojure's `load`.

**M10. A macro is a fn carrying a flag, kept in the globals** (*agent,
pending review*). `(defmacro m [x] body)` compiles to
`(def m (__macro__ (fn [&form &env x] body)))`: an ordinary user fn whose
wrapper also carries the own attribute `__macro__`, stored in the globals
under its name (`src/runtime/Macros.h`). The globals table is therefore the
macro table: a later `def` of the name replaces the macro with a plain value
and a later `defmacro` with the new macro, which is what Clojure does to the
var. Like Clojure's macro fns, it takes the hidden `&form` and `&env` first;
docstrings and an attribute map are accepted and dropped (D28: no vars to
hold them), and multi-arity macros work as multi-arity fns do.
Alternatives: (a) a separate name-to-macro table owned by the driver,
rejected because a `def` of the name has to unmake the macro, so two tables
would have to agree; (b) a marker object wrapping the fn, rejected because
the flag on the fn costs nothing at call time and keeps the macro an
ordinary fn.

**M11. Expansion happens when a form is compiled; the timing rule**
(*agent, pending review*). For a list form whose head is a symbol that is
not a special form (`isSpecialFormName`: Clojure's special forms plus
protoClojure's `let`, `loop` and `fn`) and not a local (a local shadows a
macro, as in Clojure), the compiler looks the name up in the globals as they
stand at that moment; when it holds a macro, the compiler calls it through
the engine (`ExecutionEngine::invoke`, on the call context the driver keeps
installed) with the call form, `&env` and the unevaluated arguments, and
compiles the result in place of the call, expanding again while the result
is a macro call. **The timing rule**: a macro is in effect for every form
compiled after the form that defined it has run; since a file and the REPL
compile each top-level form after the previous one has run (M9), that is
every later top-level form. Redefining a macro changes the forms compiled
afterwards; a function compiled earlier keeps the expansion it got. A macro
defined and used inside the same top-level form is not yet defined when the
use is compiled (except across the forms of a top-level `do`, M12), as in
Clojure. A user macro may take the name of one of the forms the compiler
still implements in C++ (`when`, `cond`, `with-open`, ...), and then it
wins; it cannot take a special form's. Naming a macro outside call position
is the compile error `Can't take value of a macro: m`, as in Clojure. The
expansion is rooted in a slot of a scope of its own while it is compiled,
as are the macro and the `&env` map while the macro runs
(`Compiler::compileMacroCall`); `cli/macro-expansion-survives-gc` checks it
under a 1,000,000-cell heap ceiling and fails when the expansion is left
unrooted. Alternative: expand at run time, on the first call; rejected as
not Clojure (a macro would see run-time state and expand per call site
execution).

**M12. A top-level form is expanded, then a top-level `do` is evaluated one
form at a time** (*agent, pending review*), as Clojure's `Compiler.eval`
does: `evalTopLevel` (`src/compiler/Eval.h`) expands a top-level form while
its head names a macro and, when the result is `(do f*)`, evaluates each
`f` the same way before compiling the next, so a macro may expand into
`(do (defmacro m ...) (m))`. Every driver (file, REPL, `:load`) and `eval`
go through it. Each module it compiles is kept until the driver ends (a
process-wide registry, released after the worker threads are joined,
because a module pins its quoted data in the ProtoSpace).

**M13. `&form` and `&env`** (*agent, pending review*). `&form` is the call
form as written. `&env` is nil at the top level and, inside a function, a
map from the symbol of every local in scope to itself; Clojure maps each to
the compiler's `LocalBinding` object, which has no counterpart here (D38).
`(contains? &env 'x)`, `(keys &env)` and `(&env 'x)` work as in Clojure.

**M14. `macroexpand-1`, `macroexpand` and `eval`** (*agent, pending
review*). `macroexpand-1` expands a form one step when its head names a
macro in the globals, with nil `&env`; `macroexpand` repeats it until the
form no longer changes; both return any other form unchanged, special forms
included. Until stage 6 the forms the compiler implements in C++ (`when`,
`cond`, `and`, ...) are not macros to them (D41). `eval` is `evalTopLevel`
on its argument; a form it cannot compile raises `RuntimeException`
`Syntax error compiling: ...` (JVM Clojure: `Compiler$CompilerException`,
D40). The runtime library reaches the compiler through a hook the compiler
installs at start-up (`setEvalHook`), so the runtime does not link against
the compiler.

**M15. Errors raised while expanding** (*agent, pending review*). A macro
that raises makes the calling form a compile error that names the macro
and carries the exception, `script.clj:4:1: compile error: Syntax error
macroexpanding bad: ArithmeticException: Divide by zero`, as Clojure's
`Syntax error macroexpanding bad at (script.clj:4:1)`. A call with the
wrong number of arguments is checked before the macro runs and reported as
`Wrong number of args (2) passed to macro: one`, counting the arguments as
written (D39). A macro that keeps expanding into a call of itself ends in
`StackOverflowError`, a compile error. At the REPL a compile or runtime
error no longer drops the rest of the input line: the next form runs, as in
Clojure's REPL; a read error still drops it, the reader's position being
unreliable after one. `:load` and scripts stop at the first error.

**M16. Changes the macros needed elsewhere in the compiler** (*agent,
pending review*). (a) `let` and `loop` bindings are now scoped to the
form's body (they used to stay bound for the rest of the enclosing
function, so `(let [x 1] (let [x 2] x) x)` answered 2); (b) `let` and
`loop` work at the top level, compiled as the body of a fn called on the
spot as a top-level `try` already was, because most macro expansions are
`let` forms; (c) a variadic fn called with no rest arguments binds the rest
parameter to nil instead of `()`, as in Clojure, so `(if body ...)` in a
`[& body]` macro behaves as Clojure programmers expect; (d) `defn` accepts
a docstring and an attribute map. (a) and (c) change the behaviour of
existing programs (CHANGELOG).

**M17. Syntax-quote qualifies symbols when it is read, as Clojure's reader
does** (*agent, pending review*). The reader asks a resolver the drivers
install (`qualifySyntaxQuoteSymbol`, `src/compiler/Eval.h`) how to spell
each symbol of a template: Clojure's special forms and `&` stay as they
are, and so do symbols that have a namespace or a `.` and the built-in
exception class names (Clojure would write `java.lang.Exception`; our
`catch` takes both, D42); a built-in -- a primitive, or a form the compiler
implements that is a `clojure.core` macro in Clojure (`let`, `fn`, `defn`,
`when`, `cond`, `and`, `or`, ...) -- becomes `clojure.core/x`, unless the
program has defined `x` itself, which makes it `user/x` as Clojure's
namespace map would; anything else becomes `user/x`. Resolution happens at
read time, against the globals as they stand, since each top-level form is
read after the previous one ran (M9). There are no other namespaces (D28),
so `user` and `clojure.core` are the only qualifiers syntax-quote writes.
Alternative: leave symbols unqualified and rename locals at expansion time
(a hygienic expander in the Scheme style); rejected because the
`macroexpand` output and the rules (`x#`, `~'x`, "Can't let qualified
name") would not be Clojure's.

**M18. `foo#` and `gensym`** (*agent, pending review*). Inside one
syntax-quote, every `foo#` is the same generated symbol `foo__N__auto__`;
each syntax-quote, nested ones included, has its own table, as in Clojure.
`(gensym)` is `G__N` and `(gensym "p")` `pN`. N comes from one process-wide
counter (`nextGensymId`, `src/runtime/Named.h`), shared with the reader. The
lexer now reads `#` inside a symbol (`foo#`, as Clojure does); a `#` that
starts a token is still the dispatch character. Generated symbols are
interned like every symbol and never collected (D43). `symbol`, `keyword`,
`name` and `list*` were added with them.

**M19. `user/x` and `clojure.core/x` name globals, never locals**
(*agent, pending review*). `user/x` compiles to the global `x` and
`clojure.core/x` to the globals key `clojure.core/x`; every built-in is
installed under both its name and that key at start-up (`defineBuiltin`,
`src/runtime/Macros.h`), so a program's `(def seq ...)` changes `seq` but
not `clojure.core/seq`, which closes D37. A `def` or `defn` of `user/x`
defines `x`. A qualified name as a parameter or a `let` / `loop` binding is
rejected with Clojure's messages (`Can't use qualified name as parameter:
user/x`, `Can't let qualified name: user/v`): it is what a template that
forgot `#` produces. Cost: the globals table holds each built-in twice
(about 140 extra entries); a lookup is hashed, so no call path changed.

**M20. `core.clj`: embedded, loaded at start-up, in `clojure.core`**
(*agent, pending review*). `src/core/core.clj` holds the part of
`clojure.core` written in Clojure. The build embeds its bytes in the binary
(`cmake/EmbedFile.cmake` generates `core_clj.inc`, included by
`src/compiler/Eval.cpp`), so the deb, the tarball and the build tree run the
same core and nothing is looked up at run time (`cli/core-embedded` runs a
copy of the binary from an empty directory). Every driver calls `loadCore`
after installing the primitives and its call context, before the program's
first form; a failure prints `core.clj:line:col: ...` and exits with status
1. While it loads, the current namespace is `clojure.core`
(`coreNamespaceActive`): a `def`, `defn` or `defmacro` also stores under
`clojure.core/x` (M19), and syntax-quote qualifies every unqualified name
as `clojure.core/x`. Internal helpers are named `__x`, there being no
private vars. Alternatives: (a) install it beside the binary
(`/usr/share/protoclj/core.clj`), rejected because the binary would then
depend on a path and a build tree would need the same layout; (b)
precompile it to bytecode at build time, not needed: loading it costs
about 3.4 ms of CPU and 12 M cycles per process (benchmarks/RESULTS.md,
2026-10-23), under the 10 ms budget.

**M21. What moved to `core.clj`, what stayed in C++, measured** (*agent,
pending review*). Moved: `when`, `when-not` and `cond` (their expansions
compile to the same instructions the C++ forms emitted; the compiler now
folds an `if` whose test is a literal, so `cond`'s `:else` clause costs
nothing at run time, as it did not before either), and `with-open` and
`future` (pure rewrites, now visible to `macroexpand`). Their C++ code is
deleted. Their error messages now come from the macros:
`Syntax error macroexpanding with-open: IllegalArgumentException:
with-open requires an even number of forms in binding vector`, and `cond`
no longer accepts the bare symbol `else` as a default (Clojure does not;
`:else` works). Stayed in C++: `and` and `or`, because as Clojure's macros
they bind a local per operand (`(let [and# x] (if and# (and ...) and#))`),
which costs a store and two loads where the C++ forms use `DUP`: measured
on a 20,000,000-iteration loop whose body is a three-operand `and`, the
macro version took 2.24-2.33 s against 2.13-2.15 s (+5-8 %), two
interleaved runs each; and `fn`, `let`, `loop`, `defn`, `defmacro`,
`apply`, the forms Clojure builds on `fn*`, `let*` and `loop*`. Those
remain unchanged by `macroexpand` (D41).

**M22. The `clojure.core` macros as built** (*agent, pending review*).
`core.clj` defines `when`, `when-not`, `if-not`, `cond`, `comment`, `while`,
`if-let`, `when-let`, `if-some`, `when-some`, `->`, `->>`, `as->`,
`some->`, `some->>`, `cond->`, `cond->>`, `doto`, `dotimes`, `doseq`,
`case`, `condp`, `with-open` and `future`, following clojure.core's
definitions, and the functions `identity`, `some?`, `true?`, `false?`,
`zero?`, `pos?`, `neg?`, `nnext`, `ffirst`, `nthnext`, `butlast`
(`even?` and `odd?` are new C++ built-ins). Binding macros check their
binding vector at expansion time and raise `IllegalArgumentException` with
Clojure's messages; since Part B stage 7 their binding forms destructure
(M23). Differences: `doseq` has no `:let`, `:when` or
`:while` yet (stage 10); `case` tests its constants one after the other
with `=` instead of a constant-time hash dispatch, so it costs O(n) in the
number of clauses (D44), and its constants are compared with protoClojure's
`=` (numbers across types, D15); `letfn` is not provided, because mutually
recursive local functions need a `letfn*` that binds names before the
functions exist, and closures here capture locals by value when they are
created (with D18, no fn can name itself). Since Part C `letfn` is a
`core.clj` macro that goes through atoms (M34, D53).

### 10.2 Decisions of the scripting vocabulary (macros brief, Part B)

**M23. Destructuring is a compile-time rewrite in the C++ compiler, into
Clojure's own expansion** (*agent, pending review*). `let`, `loop` and the
parameter vectors of `fn`, `defn` and `defmacro` accept binding forms:
`[a b & r :as all]` and `{x :x :keys [...] :strs [...] :syms [...] :or {...}
:as m}` (and `:ns/keys`, `:ns/syms`), nested. `Compiler::destructure`
(`src/compiler/Compiler.cpp`) turns a binding form into the symbol bindings
`clojure.core/destructure` produces -- `vec__N`, `seq__N`, `first__N` and
`map__N` locals read with `clojure.core/nth` (with a `nil` default),
`first`, `next`, `seq` and `get`, and `__destructure-map` for a map form
(a seq of pairs becomes their map, as Clojure's `createAsIfByAssoc`) -- and
the compiler compiles that `let`. `loop` is rewritten as Clojure's macro
does, `(let [g v form g] (loop [g g] (let [form g] body)))`, so an init
sees the names earlier forms destructured and `recur` re-destructures; a
destructured parameter becomes `p__N` and the body `(let [form p__N] ...)`,
so the implicit `recur` of a fn works too. The binding macros of
`core.clj` (`if-let`, `when-let`, `if-some`, `when-some`, `doseq`) needed
no change: they expand into `let`. `& {:keys [...] :or {...} :as m}` with
symbol keys keeps the keyword-argument call convention (D16, its `:or`
fires on an explicit nil); any other map after `&`, and every map form
elsewhere, destructures with Clojure's semantics (`:or` only for a missing
key). Errors use Clojure's messages (`Unsupported binding form: 1`, `Unsupported
binding form, only :as can follow & parameter`). Alternatives: (a)
`destructure` as a Clojure function in `core.clj`, as JVM Clojure has it;
rejected because `let`, `loop` and `fn` are C++ forms that `core.clj` itself
uses before it could define it, and it would add its definition to every
start-up; (b) dedicated opcodes that read positions and keys; rejected:
each read is one call either way, and the rewrite keeps the expansion
identical to Clojure's; (c) taking `& {:keys}` off the keyword-argument
path; rejected because the call convention is how protoClojure functions
take named arguments from other protoCore runtimes. `macroexpand` does not
show the rewrite, since `let` and `fn` are compiled, not expanded (D41).

**M24. A set is an object holding a ProtoMap, matched as map keys are**
(*agent, pending review*). `src/runtime/SetOps.h`: a set is an immutable
child of a set prototype (created per ProtoSpace, rooted on the globals)
whose own attribute `__set__` is a `ProtoMap` from each element to itself,
through the map's hashed-collection helper and `clojureKeySemantics`. So
STATUS.md's plan (a `ProtoMap` through the map's helper) holds, and element
equality, hashing and the cost model are the map's: `#{1 1.0}` has two
elements, as in JVM Clojure, and a set is a map key by value
(`keyHash` / `keyEquals` know sets). The reader yields a set for `#{...}`
(M1), a repeated element being a read error; the compiler pins a set
literal whose elements are all constants as a datum (M5) and compiles any
other as a call of `clojure.core/hash-set`; syntax-quote applies
`hash-set` to a set template. Alternatives: (a) protoCore's `ProtoSet`,
rejected because it keys elements by a 64-bit hash with no collision
bucket, so two different elements with one hash would be one; (b) a
distinct pointer tag in the way a vector boxes its list (VectorOps.h),
rejected because no tag the collector traces and protoCore does not intern
is left (a `ProtoTuple` is interned and never freed); (c) a marker entry
inside the map, rejected because every `map?` would then pay a lookup. The
cost of the object is one cell per set and an own-attribute read per
operation; protoScala's sets have the same shape. `clojure.set`'s
functions are globals (D28). Also in this stage, as a prerequisite of
`into`: `()` evaluates to the empty list (it was a compile error), and
`first`, `rest`, `map`, `filter`, `reduce`, `count` and the other sequence
functions accept maps, sets and strings, as `seq` already did.

**M25. Regular expressions on `std::regex` ECMAScript, Java's syntax
translated where it has an equivalent** (*agent, pending review*).
`src/runtime/Regex.h`. The engines considered, as the brief allows: (a)
`std::regex` ECMAScript, chosen: no dependency, on every platform the CI
builds; (b) PCRE2, the closest dialect to Java's (lookbehind, possessive
quantifiers, `\p{...}`, UTF-8), excluded because it is not a dependency of
protoClojure or of any runtime of the ecosystem (it is installed on the
development machine only); (c) QuickJS's `libregexp`, which protoJS
vendors, rejected because it does not build with MSVC, which the Windows CI
uses, and it would mean vendoring three C files. A pattern is translated
before compilation (leading `(?ims)` flags, `(?s)` by rewriting `.` as
`[\s\S]`, named groups numbered, `\Q...\E`, `\A` / `\z`), and what the
engine rejects is a `PatternSyntaxException`, a new built-in class under
`IllegalArgumentException`; the literal `#"..."` is compiled by the reader
(M1), so a bad literal is a read error, as in Clojure, and the compiler pins
it as a datum (M5). Compiled patterns are cached by source for the process
and never freed: the value holds an external pointer to its cache entry, so
a match needs no lock and no lookup. libstdc++'s default (backtracking)
executor recurses once per subject character and crashed protoclj with
SIGSEGV past about 40,000 bytes for `(a|b)*` (measured: 840 bytes of stack
per byte); every pattern without back-references is therefore compiled with
libstdc++'s `__polynomial` option, its breadth-first executor, measured to
match 5,000,000 bytes on an 8 MiB stack with the same results as the
backtracking one except the captures of empty iterations (`(a*)*b`), and a
pattern with back-references (or any pattern on another standard library)
is refused with `StackOverflowError` before a match that could exhaust the
stack. Since stage 11 `re-seq` is lazy (M29). The differences against Java
are D45.

**M26. `clojure.string/x` and `clojure.set/x` name the built-ins** (*agent,
pending review*). Without namespaces (D28) the string and set functions are
globals; the compiler reads `clojure.string/split`, `clojure.set/union` and
the other names those namespaces have and protoClojure provides as
`clojure.core/x`, the built-in's own key (M19), so Clojure code that spells
them out runs with no `require`, and a local named `split` does not capture
them. Alternative: wait for namespaces; rejected because the full names are
common in scripts and cost nothing to accept. The `str/` alias of
`(:require [clojure.string :as str])` still needs `ns` (stage 13 notes).

**M27. `for` and `doseq` in `core.clj`; `for` is eager behind one hook;
`format` in C++** (*agent, pending review*). `doseq` with `:let`, `:when`
and `:while` expands, per binding, into a `loop` over `(seq coll)` whose
body wraps the inner code in the modifiers in order (`:when` false steps
to the next element, `:while` false ends the loop without the `recur`), as
clojure.core's `doseq` does without its chunked-seq path. `for` expands
into one `(clojure.core/__for-step coll (fn [binding] ...))` per binding;
the innermost body is `(list expr)`, `:when` false is `()` and `:while`
false is the marker `__for-stop`, which makes `__for-step` stop iterating
that binding. `__for-step` was an eager walk that concatenated the inner
results into a list (D46); stage 11 made it a lazy walk (a C++ step,
`LazySeq.cpp`), and the expansion changed in one detail: the innermost
binding passes a third argument, `true`, which lets `__for-step` walk a
chunked collection a chunk at a time, as Clojure's `for` does (M29). Alternatives: (a)
Clojure's own `for`, built on `lazy-seq` and chunked seqs, not possible
before stage 11; (b) `for` as a `doseq` that conjoins onto an accumulator,
rejected because it would have to be rewritten for laziness. A finite,
eager `range` (in `core.clj`, replaced in stage 11 by the lazy primitive)
came with it, since `for` without `range` is
not how Clojure programmers write it; `(range)` with no end is an error
until stage 11. `format` and `printf` are C++ (`formatString` in
`Primitives.cpp`), implementing the conversions of Java's `Formatter` a
script uses, with Java's half-up rounding for `%f` and `%e` (C's printf
rounds an exact tie to even), `%s` rendering a value as `str` does and nil
as `null`, as Clojure's format does; the differences are D47. `(apply f x y
coll)` now compiles as `(apply f (list* x y coll))`.


**M28. A lazy seq is a protoCore lazy object, a new protoCore type**
(*agent, pending review*; the protoCore type itself needs the maintainer's
review: protoCore branch `feature/lazy-object`, 3.1.0). A lazy seq must
cache its realisation and be shared safely by threads (futures can read one
seq), with compare-and-set and no mutex. The first build made it a protoCore
**mutable** object whose one attribute moved from the thunk to the realised
seq by `setAttributeIfEqual`, as atoms do. It was correct and leaked: every
state in protoCore's global mutable table is a collector root until the
object's handle has been swept, so in a realised chain -- each lazy seq's
state is a cons pointing at the next lazy seq -- the next handle is marked
through the dead entry and survives, and the chain is freed one link per
cycle. Measured: `(count (range 3000000))` under a 1,000,000-cell ceiling
did not finish in 60 s (429 cycles, the live set climbing to 975,000
cells), and 300,000 plain atoms each pointing at the next took 278 cycles
and 55 s on the stage-6 binary too: the property predates the lazy seqs and
affects every runtime's linked mutable structures (protoCore
`docs/GLOBAL_MUTABLE_TABLE.md`, Limitations, now records it). Following the
platform rule (extend protoCore by a new type, do not change an existing
model), protoCore 3.1.0 adds **lazy objects**: `newLazyChild(ctx, producer)`
makes an immutable object cell with a write-once slot (`claimLazy`,
`unclaimLazy`, `publishLazy`, `getLazyValue`, `getLazyProducer`), traced
only through the cell like any field, so an unreachable chain is freed in
the cycle that finds it unreachable; the producer stops being traced from
the cycle after publication, so what the thunk captured is released. It
needs no pointer tag (the cell derives from the object cell) and keeps the
ABI (SOVERSION 4). Measured after the change: the same `count` takes 0.8 s
with a last live set of 6,884 cells.
Layouts (`src/runtime/LazySeq.cpp`): a lazy seq is a lazy child of the
lazy-seq prototype whose producer is the thunk; a cons and a chunk are lazy
objects of their own prototypes published at birth with a small list of
their fields (`[first more]`, `[items off end more]`); a native step (the
thunk of a lazy `map`, `filter`, `range`, ...) is a list `[kind operands]`.
A cons costs four cells and leaves no garbage. Realisation: a thunk that runs
Clojure code is claimed first, so it runs once whatever the number of
readers (a reader that finds the seq claimed waits, parked for the
collector); a native step that runs none is computed without claiming and
the first publication wins. A body that raises leaves the seq unrealised, so
the next read runs it again, as in Clojure. A seq read while its own body
computes it raises `StackOverflowError` at once (JVM Clojure recurses until
the stack overflows). Alternatives: (a) the mutable object, rejected for the
leak above; (b) changing protoCore's collector to treat the mutable table as
ephemerons (an entry's state traced only when its handle is reachable),
which would also fix other runtimes' linked mutable structures but changes
an existing model and its proof; recorded in protoCore's docs for the
maintainer; (c) one mutable cache per sequence (a buffer of realised
elements), rejected because it keeps every element of a stream; (d)
recomputing pure steps instead of caching them, rejected because user code
(`map`'s f) must run once.

**M29. What is lazy, and how it is chunked** (*agent, pending review*).
The lazy functions are C++ steps (`LazySeq.cpp`), not `core.clj` code: a
step realises in one native call what Clojure's `lazy-seq` body does, without
a Clojure frame per element. As in Clojure: `map` (one or several
collections), `filter`, `remove`, `take`, `take-while`, `drop`,
`drop-while`, `iterate`, `repeat`, `repeatedly`, `cycle`, `range`, `concat`,
`mapcat`, `partition`, `partition-all`, `interleave`, `distinct` and `for`
answer lazy seqs; `lazy-seq` and `lazy-cat` are macros in `core.clj`;
`line-seq` and `re-seq` read their first line or match at once and the rest
as the seq is read, as Clojure's do; `doall`, `dorun` (both with an optional
count) and `run!` walk; `realized?` tells a lazy seq's state. Chunking
follows Clojure: the seq of a vector and a finite `range` are chunked seqs
(`chunked-seq?`), and `map`, `filter` and the innermost binding of `for`
realise 32 elements of a chunked seq at a time, so `(first (map f (range
100)))` calls f 32 times, as in Clojure, and once over a list. `(range)` with
no end counts one at a time, as Clojure's `(iterate inc' 0)`. `concat` keeps
the chunks of what it concatenates; when every argument is an already
realised collection (lists, vectors, maps, strings), `concat` builds the
list at once, since nothing could observe the difference and syntax-quote
calls it on lists. `seq` of a vector answers the vector's chunked seq (it
answered the vector's list before), and so do `rest` and `next` of a vector:
`(list? (rest [1 2]))` is false, as in Clojure. A form that is a seq object
(what a macro builds with `map`, `cons` or a lazy `concat`) is compiled as
the list of its elements, as Clojure compiles any ISeq. Differences: D49.

**M30. Not holding the head: locals clearing and released arguments**
(*agent, pending review*). JVM Clojure lets a walk drop the head of a seq
because its compiler clears a local at its last use and the JVM drops
arguments it no longer needs. protoClojure does three things, measured by
`cli/lazy-head-retention` and `cli/lazy-grep-under-heap-limit`:
(a) **locals clearing** (`clearLastReads`, `src/compiler/Compiler.cpp`):
after a fn body is compiled, a local read by exactly one `PUSH_LOCAL`, where
that read cannot run twice on the same binding (it is outside every loop
the binding is not rebound in), has the read turned into the new opcode
`PUSH_LOCAL_CLEAR`, which empties the slot; fixed parameters and `let` /
`loop` locals qualify, rest, `:keys` and `:as` parameters, closure
captures and catch slots never do; (b) the VM clears an argument's stack
slot once a call has taken it: a fn's arguments as they are copied into the
callee's frame, a primitive's when it returns; (c) the walking primitives
(`dorun`, `count`, `reduce`, `last`, `run!`) clear their sequence argument
once their cursor holds it (`releaseArg`, `LazySeq.h`). Together:
`(let [s (line-seq rdr)] (doseq [l s] ...))`, `(defn walk [s] (dorun s))`
and `(count (filter p (line-seq rdr)))` run in constant memory, and a local
read twice after the walk holds the head, as in Clojure. The cost on calls:
`fib` executes 0.5 % fewer instructions than before (benchmarks/RESULTS.md).
Alternatives: (i) Clojure's full analysis (clear on the last use along each
path, both branches of an `if` included), rejected for now as a larger
compiler change; the conservative rule leaves a local read in both branches
of an `if` uncleared (D50); (ii) no clearing, rejected: the brief's
streaming test needs it.

**M31. The REPL echoes a definition as its var** (*agent, pending review*).
`(def x ...)`, `(defn f ...)` and `(defmacro m ...)` at the REPL print
`#'user/x`, as JVM Clojure's REPL does, instead of the value. With lazy seqs
the old echo printed the value, which realised it (and never finished for
`(def nat (iterate inc 0))`). `*1` still holds the value, there being no
vars (D28, D51).

**M32. `#(...)` is read as Clojure's reader reads it** (*agent, pending
review*). The lexer yields `#(` and the reader reads the body as a list,
replacing `%` and `%1`..`%20` with generated symbols `p1__N#` (`%` is `%1`)
and `%&` with `rest__N#`, then answers `(fn* [p1 ... pN & rest] (body))`
with N the largest index used (`src/reader/Reader.cpp`). The compiler treats
`fn*` as `fn` (it already counted as a special form). A `#()` inside another
is the read error `Nested #()s are not allowed`, Clojure's. Alternative:
expand `#()` to `fn` with the symbols `%`, `%1` as parameter names, rejected
because `'#(inc %)` would then not print what Clojure prints, and a `%`
inside a nested quoted form would differ.

**M33. Library aliases: `require` records, the compiler rewrites**
(*agent, pending review*). `require` is a C++ built-in
(`src/runtime/CorePrims.cpp`): for each spec it checks that the namespace is
one protoClojure provides as built-ins (`clojure.core`, `clojure.string`,
`clojure.set`, `clojure.java.io`, `clojure.java.shell`) and records each
`:as` alias in a process-wide table (`addNamespaceAlias`,
`src/runtime/Macros.h`); `:refer` is accepted and does nothing, every
function being a global. `ns` is a `core.clj` macro that expands its
`(:require ...)` clauses into `(do (require 'spec) ... nil)`, which a top
level `do` runs form by form (M12), so the aliases hold for the next forms.
The compiler's `aliasLibraryNamespace` replaces a recorded alias by its
namespace before M26's rewrite of `clojure.string/x` to the built-in `x`, at
the two places a qualified name is compiled (a call's head, a symbol); a
name the namespace does not have is the compile error `No such var: str/foo`
(Clojure's). Any other namespace raises `FileNotFoundException` with
Clojure's "Could not locate ..." message. Alternatives: (a) `ns` as a
compiler special form, rejected: a macro needs no C++ and `macroexpand`
shows what it does; (b) aliases per file, rejected while there are no
namespaces to hang them on (D52); (c) a module object per namespace read
through `alias/member` (`import-module`'s path), rejected because the
compile-time rewrite keeps a call to `str/join` as fast as a call to `join`.

**M34. The Part C vocabulary: where each function lives** (*agent, pending
review*). C++ where the work is per element or the semantics need
protoCore's exact integers: `quot`, `rem`, `mod`, `compare`, `max`, `min`,
`sort` and `sort-by` (a stable sort, `std::stable_sort`, over a comparator
that is `compare`, a number-returning fn or a boolean predicate, as
Clojure's AFunction.compare), `frequencies` (one pass with a C++ hash table
on the map key semantics, the distinct elements rooted in a list), and
`clojure.string/split-lines`; `(System/exit n)` and its siblings are members
of a `System` object read by `CALL_MEMBER`. Clojure in `core.clj`, from
clojure.core's own definitions, for the rest: `key`, `val`, `constantly`,
`complement`, `partial`, `comp`, `juxt`, `fnil`, `some`, `every?`,
`not-any?`, `not-every?`, `max-key`, `min-key`, `get-in`, `assoc-in`,
`update`, `update-in`, `merge`, `merge-with`, `select-keys`, `zipmap`,
`group-by`, `letfn` and `ns`. They add about 4.5 M cycles (1.2 ms) to
start-up (benchmarks/RESULTS.md); `core.clj` now costs about 5 ms in all,
under the brief's 10 ms. `assoc` now takes `nil` (the empty map) and a
vector (by index), which `assoc-in` and `update-in` need. `letfn` binds
each name to a small fn that calls its definition through an atom, reset
once every definition exists: a closure captures a local's value when it is
made (D18), so two definitions cannot otherwise see each other. Each call
through the name costs an `apply` and a `deref` (D53). Alternative: a
`letfn*` special form with a frame slot per name filled after the closures
are made, rejected for now as compiler work the brief did not require.
**M35. `read-string` and `clojure.edn/read-string` are the reader, run on a
string** (*agent, pending review*). Both are C++ built-ins
(`src/runtime/Edn.cpp`) that build a `Reader` (`src/reader/Reader.cpp`) on
the string and return its first form; the rest of the string is ignored, as
in Clojure. `read-string` reads as code is read: quote, `#(...)`, `#"..."`
and syntax-quote, which it qualifies through the resolver the compiler
installs beside the `eval` hook (`setSyntaxQuoteResolverHook`, Macros.h), so
`` (read-string "`(map ~f)") `` reads as the drivers read it. `clojure.edn`
sets the reader's EDN mode: the code-only reader macros (`'`, `` ` ``, `~`,
`~@`, `@`, `#(`, `#"`) are read errors, and there is no evaluation (`#=`
does not exist) and no tagged-literal reader (`#inst` raises Clojure's "No
reader function for tag inst", D13). `(read-string "")` raises "EOF while
reading"; `(clojure.edn/read-string "")` and `(clojure.edn/read-string nil)`
answer nil; both take Clojure's `{:eof v}` option. A read error is a
`RuntimeException` with the reader's message and its line and column in the
string (D56). To make `pr-str` and `read-string` round-trip every printable
value, the reader gained what the printer already wrote or Clojure's reader
already read: the string escapes `\b`, `\f`, `\uXXXX` (a surrogate pair
being the one character it encodes, an unpaired surrogate a read error,
D55) and octal `\0`..`\377`; `#_` (discard the next form, anywhere a form
may stand); strings holding a NUL byte, which the reader, the VM's string
constants and the string primitives used to cut at the NUL
(`src/runtime/Utf8String.h`, `makeUtf8String`). Ratios do not exist in
protoClojure (`(/ 1 3)` is integer division), so the round trip covers nil,
booleans, integers of any size, doubles (printed with the shortest digits
that read back), strings, keywords, symbols, lists, vectors, maps and sets
(`39-data-formats/pr-str-read-string-round-trip`). Alternatives: (a) a
separate EDN parser, rejected: it would duplicate the reader and drift from
it; (b) reading `'a` in EDN mode as the symbol `'a`, as JVM Clojure's
EdnReader does, rejected: nobody writes that on purpose, and an error says
what happened.

**M36. JSON: one C++ parser and one writer, two APIs as thin names**
(*agent, pending review*). `src/runtime/Json.cpp` holds a recursive-descent
parser over a `Source` (a whole string, or a reader pulled a line at a
time, M38) and a writer, both building or reading protoClojure values
directly: an object is a map, an array a vector, a number without a
fraction or exponent an integer (exact at any size: a `LargeInteger` beyond
64 bits), any other number a double (`strtod`, as the lexer reads
float literals; overflow is an infinity and underflow a zero or a subnormal,
as Java's `Double.parseDouble`).
Strings take the escapes of RFC 8259, a surrogate pair is one character and
an unpaired one an error (D57); an unescaped control character is accepted,
as Cheshire's parser accepts it. Errors name the problem and the line and
column (in characters) where the input stopped making sense: `JSON error:
expected ':' after an object key, found character '1' at line 1, column 6`.
cheshire.core (`parse-string`, `parse-stream`, `generate-string`,
`generate-stream`, `decode`, `encode`) and clojure.data.json (`read-str`,
`read`, `write-str`, `write`) are thin functions over it that differ only
where the libraries differ, as a `Flavor`: Cheshire keeps a keyword's
namespace (`:a/b` is `"a/b"`), escapes nothing beyond control characters
unless `:escape-non-ascii`, writes NaN and the infinities as quoted strings
(Jackson's default), ignores what follows the first value, applies
`:key-fn` to keyword keys only, writes `{nil 1}` as `{"":1}`, writes
`:pretty` in Jackson's default pretty-printer layout, and raises
`IOException` (Jackson's `JsonParseException` is one); clojure.data.json
writes a keyword's name, escapes non-ASCII (`\u00e9`, lower case), `/` and
U+2028/U+2029 by default (`:escape-unicode`, `:escape-slash`,
`:escape-js-separators`), raises on NaN, on a nil key and on data after the
value in `read-str`, applies `:key-fn` to every key and `:value-fn` to
every entry (an entry whose value-fn answers the value-fn itself is
dropped), and raises `Exception`. The behaviour of Cheshire was checked
against Babashka 1.4.192, which bundles it: the cheshire fixtures of
`tests/conformance/39-data-formats` pass under `bb` too. clojure.data.json
is not in Babashka, so its behaviour follows its documentation and source
as remembered, not a run against the JVM library (D57). GC: each container
is built in a context of its own, an array through a `ListBuilder` and an
object in a scope whose slots hold the map so far and up to 64 pending
entries, folded into the map inside a short-lived context; a key fn or a
value fn runs with the key and the value in slots. A 20,000-record document
parses under a 600,000-cell ceiling and 200,000 newline-delimited records
stream through a 300,000-cell one (`tests/cli/json-under-heap-limit.sh`).
Alternatives: (a) JSON in Clojure in `core.clj`, rejected: a character per
VM dispatch would be an order of magnitude slower on the large inputs
scripts read; (b) a C++ JSON library (nlohmann/json, simdjson), rejected: a
new dependency, and its document would be one more copy of the data to
convert into protoCore values, where this parser builds them directly.

**M37. The data-format namespaces are objects whose members are the
functions** (*agent, pending review*). `clojure.edn`, `cheshire.core` and
`clojure.data.json` are globals holding an object (as `System` is, M34)
whose members are the C++ functions. `aliasLibraryNamespace`
(`src/compiler/Compiler.cpp`) knows their member names: it leaves
`cheshire.core/parse-string` as it is (and turns `json/parse-string` into it
once `[cheshire.core :as json]` is required), so it compiles as the member
call `alias/member` already compiles to (`CALL_MEMBER`, or `GET_MEMBER` for
a value), and a name the namespace does not have is the compile error `No
such var: json/parse`, as for clojure.string. `isKnownNamespace` lists them,
so `require` and `ns` accept them. Alternatives: (a) installing the
functions as globals, as clojure.string's are (M26), rejected: the names
collide (`clojure.edn/read-string` is not `clojure.core/read-string`,
`clojure.data.json/read` would shadow a future `read`) and would fill the
program's namespace with `parse-string` and `write-str`; (b) rewriting to
hidden globals (`__cheshire_parse_string`), rejected: a member call costs
one attribute lookup more, nothing next to parsing a document, and needs no
second table.

**M38. JSON streams are read a line at a time** (*agent, pending review*).
`parse-stream` and `clojure.data.json/read` pull their input through
`readLineBlocking` (IO.h), which blocks outside protoCore's managed state
with every value parsed so far held in a slot or a live context. A
document is therefore never held whole beyond the line being parsed, and a
value that ends a line is answered as soon as the line arrives, so a
newline-delimited JSON stream over a pipe or a socket is read value by value
(`(loop [] (when-let [r (json/parse-stream *in* true)] ... (recur)))`). The
price: the rest of the line after a value is consumed with it (D57).
Alternatives: (a) reading the whole stream first, rejected: memory, and a
pipe that stays open would never answer; (b) fixed-size chunks, rejected:
protoIO's `readBytes` waits until the chunk is full, so a short message on
a socket would hang.

**M39. `pmap` joins every worker, and holds the thread object until it
does** (*agent, pending review*). `pmap` joined only the workers it found
still running; one that had finished kept its native thread (its stack
mapping and protoCore's `std::thread` object) until the process ended: 21
calls over 64 elements grew the virtual size by about 40 GB, and
ThreadSanitizer reported the threads as leaked. Every worker is joined now,
finished or not (joining a finished thread returns at once), and each
element holds its `ProtoThread` as an object, not as an address in an
integer: a finished thread leaves protoCore's thread list, so a thread known
only by its address could be collected before the join
(`tests/cli/pmap-joins-workers.sh`). Futures are unchanged: a future's
thread is still kept by address and joined by `deref` while pending and by
`shutdownFutures` at exit, so a program that creates futures without end
keeps every finished one's thread until it exits (an open issue, STATUS.md).
Alternative: a worker pool (ROADMAP), the real fix for one thread per
element, not needed for the leak.

**M40. `parse-long`, `parse-double` and `parse-boolean` are C++**
(*agent, pending review*). Clojure 1.11's string parsers, which a script
reads numbers with (`src/runtime/CorePrims.cpp`): nil for a string that
spells no number, `IllegalArgumentException` for an argument that is not a
string, as in Clojure. `parse-long` is Java's `Long.valueOf` (sign and
decimal digits, nil beyond 64 bits); `parse-double` reads surrounding
whitespace, a sign, digits with an optional fraction and exponent, `NaN`
and `Infinity`, but not Java's hexadecimal floats or the `d` / `f`
suffixes (D58).

## 11. The standard library

Two phases:

- **v0.1: minimum viable `clojure.core`.** ~80 functions: arithmetic,
  comparison, predicates, collection primitives (`first`, `rest`,
  `cons`, `conj`, `assoc`, `dissoc`, `update`, `get`, `count`,
  `seq`, `into`), higher-order (`map`, `filter`, `reduce`, `apply`),
  control flow macros (`when`, `if-let`, `cond`, `case`, `->`,
  `->>`), string operations (delegating to `ProtoString`), `atom`,
  `swap!`, `reset!`, `deref`, plus a handful of utility namespaces.
- **v0.2+: `clojure.string`, `clojure.set`, `clojure.walk`,
  `clojure.edn`.** Mostly straightforward — each is a few hundred lines
  of Clojure-level code once `clojure.core` is solid.

In 0.0.1 every library function is a C++ primitive: 74 are installed in
the global table at startup, including `clojure.string`-shaped string
functions (the list is in `STATUS.md`).

What is *out of scope* (likely permanently):

- `clojure.java.*` — JVM-specific.
- `clojure.data.csv` — replaced by the Python or JS UMD equivalents.
  (`clojure.data.json` and `cheshire.core` exist since the macros branch,
  §10.2, M36.)
- `clojure.tools.logging` — TBD; may bridge to a host logger.
- `core.async` — re-imagined on protoCore actors in a separate v0.3
  design pass, not lifted directly.

## 12. The interpreter as a Clojure program later

A long-term direction: once the C++ interpreter is solid, port chunks of
the compiler (macro-expansion, analysis passes) into protoClojure itself.
This is the natural Lisp endgame, and lines up with the protoCore
philosophy of moving runtime decisions into the language rather than the
kernel. Not a v0.1 concern — but the design should not preclude it.

## 13. Performance posture

Stated explicitly so future contributors know how to weigh tradeoffs:

> **Idiom over performance until users justify otherwise.**

A change that makes `(reduce + 0 [1 2 3 4 5])` 30% faster but turns
`reduce` into something a Clojure programmer would not recognise is
rejected. A change that makes the same expression 30% faster while the
external behaviour stays bit-identical is accepted. The yardstick for
"bit-identical" is the JVM Clojure reference behaviour, modulo the
documented departures.

Benchmarks live next to the source (`benchmarks/`); there is no CI yet.
The first benchmark target is **startup time + simple-script
throughput**, because that is the comparison Clojure programmers will
make first against Babashka.

## 14. Out of scope (v1 line)

- No Java interop.
- No reader literals beyond the standard set (`#inst`, `#uuid` reserved
  but not implemented).
- No transducers in v0.1 — composition over `seq` is fine for first
  pass; transducers in v0.2.
- No `core.async` in v0.1.
- No record types (`defrecord`) in v0.1 — protocols suffice.
- No deftype in v0.1 — same.
- No ClojureScript-style cljs-ifying. This is one substrate.
- No `clojure.spec`. Reserved for a v0.x explicit design pass.

## 15. The risk register

Honest accounting of where this design could fail:

| Risk                                                  | Mitigation                                                                                    |
| ----------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| nREPL implementation cost is larger than the v0.1 budget | Cut scope of v0.1 stdlib first; nREPL is non-negotiable                                       |
| Macro bootstrap (§10) is harder than estimated         | Time-box phase v0.1-b; if it slips, ship v0.1 with the C++ built-in macro set and call it     |
| Persistent collection performance is much worse than JVM | Benchmark early; protoCore's structural sharing should be competitive; if not, profile and fix |
| The community reads "not 100% compatible" as "broken"  | Lead with the dialect framing in README, STATUS, and tutorial chapter 3                       |
| Clojure community is too small to materially help      | Accept it — the project does not depend on a large contributor base                           |
