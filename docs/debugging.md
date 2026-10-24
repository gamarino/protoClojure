# Debugging protoClojure scripts

`protoclj --dap` is a [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
server: it speaks the protocol over its standard input and output and runs the
script the client launches. Any DAP client can drive it; the repository ships a
small VS Code extension, `editor-integration/vscode/`, that registers the
`protoclj` debug type and starts the adapter.

## Installing

1. **protoclj.** Build and install it (`README.md`, "Getting started";
   `docs/INSTALLATION.md`). The extension looks for the executable in this order:
   1. the `protoclj.path` setting (Settings → Extensions → protoClojure), if set;
   2. the default install location, if it exists:
      `%LOCALAPPDATA%\Programs\proto\bin\protoclj.exe` on Windows,
      `~/.local/bin/protoclj` elsewhere;
   3. `protoclj` on the `PATH`.
2. **The extension.** Package it as a `.vsix` and install it (Node.js needed for
   `npx`; there is no build step):

   ```bash
   cd editor-integration/vscode
   npx @vscode/vsce@3.2.1 package --skip-license
   code --install-extension protoclj-debug-0.3.0.vsix
   ```

   On Linux and macOS a symlink also works, and picks up edits after
   **Developer: Reload Window**:
   `ln -s "$(pwd)/editor-integration/vscode" ~/.vscode/extensions/protoclj-debug-0.3.0`.

Syntax highlighting of `.clj`, `.cljs`, `.cljc` and `.edn` files is VS Code's own
(language id `clojure`); the extension adds no grammar and no language, only the
debugger and breakpoints for that language, so it coexists with Calva.

## Starting a session

Open a `.clj` file, click in the gutter to set breakpoints, press **F5**. Without
a `launch.json` the extension debugs the active file. `examples/.vscode/launch.json`
has ready-made configurations, and **Run → Add Configuration… → protoClojure**
writes one:

```json
{
  "type": "protoclj",
  "request": "launch",
  "name": "Debug current Clojure script (protoclj)",
  "program": "${file}",
  "args": [],
  "stopOnEntry": false
}
```

| Attribute     | Type     | Required | Meaning |
|---------------|----------|----------|---------|
| `program`     | string   | yes      | Path of the script. |
| `args`        | string[] | no       | The script's arguments, `*command-line-args*`. |
| `stopOnEntry` | boolean  | no       | Stop before the script's first form runs. |

The script runs exactly as `protoclj program args...` would, with its standard
output and standard error shown in the Debug Console. Its standard input is
empty: `(read-line)` answers `nil`, because the adapter's standard input carries
the protocol.

## What works

- **Line breakpoints**, set and cleared at any time, also while the script runs,
  with a **condition**, a **hit count** or a **log message** (see below).
- **Continue, Step Over, Step Into, Step Out, Pause.**
- **Call stack**: one frame per function call — named fns by their name
  (`defn`, `(fn name [..] ..)`, `(def name (fn ..))`), anonymous ones as
  `fn@file.clj:line` — down to `<top level>`, the top-level form being run.
  Each frame shows the line it is on.
- **Locals** of the selected frame, by their names: the fn's parameters
  (including `& rest`, `:keys` and `:as`), the closure's captured locals, and
  the `let`, `loop` and `catch` bindings in scope at the line the frame is on;
  destructuring shows the names it binds and the generated ones (`vec__12`,
  `map__3`) it binds them through. Vectors, lists, maps and sets expand.
- **Watch and Debug Console**: `evaluate` of any expression in the selected
  frame. The frame's locals the expression names are bound to their values;
  globals and every function are available as in the script. Hovering a local
  shows its value.
- **stopOnEntry**, and the program's exit status in the Debug Console.
- **Exception breakpoints** (the Breakpoints view's checkboxes; see below):
  *All Exceptions* and *Uncaught Exceptions*, with the exception's class,
  message and stack in VS Code's exception widget (`exceptionInfo`).
- **Threads**: every thread that runs the script's code — `main`, each
  `future`, each `pmap` element, the actor workers, an HTTP server's
  connection threads — is its own thread in the Call Stack view, and stops
  on its own (see below).

### Exception breakpoints

Two filters, both off by default:

- **All Exceptions** (`raised`): stop at every exception the script raises,
  caught or not — a `throw`, an error the runtime raises on the script's
  behalf (an arity error, `(nth v 10)`, a missing file...) and a protoCore
  error (`(/ 1 0)`, an integer overflow). The thread stops **before** the
  exception propagates, in the innermost frame of the script, so the stack,
  Locals and evaluate show the state of the throw. When the exception comes
  from a clojure.core function or the runtime rather than a `throw`, the
  frame shown is the script's frame that made the call, and the description
  says so ("raised by the runtime in a call on this line").
- **Uncaught Exceptions** (`userUnhandled`): stop at a raise that no
  `try`/`catch` of the script catches. At the moment of the throw the
  adapter walks the thread's frames from the innermost out and, for each
  active `try`, tests the exception against its `catch` clauses as the
  clauses themselves would (built-in classes, `ex-info`'s
  `clojure.lang.ExceptionInfo`, a type named by a global or a local); a
  `finally` alone catches nothing. The walk goes through clojure.core
  functions implemented in C++ that call back into the script (`map` and its
  lazy seq, `reduce`, `swap!`...), which pass an exception on unchanged. It
  stops at a native site that swallows what it calls throws — an atom's
  watch — so an exception that the script can never catch stops even when a
  `try` encloses the `swap!`. A `future` or `pmap` body, an actor's turn and
  an HTTP handler start a thread's frames: an exception leaving them is
  uncaught there (the future keeps it for `deref`, which raises an
  `ExecutionException` on the derefing thread, a new exception of its own).

**Conditions.** Each filter takes a condition (the pencil next to the filter
in the Breakpoints view): a Clojure form evaluated in the throwing frame — its
locals and the script's globals, as in Watch — with the exception bound to
**`ex`** (a local named `ex` is shadowed). The thread stops only when the form
is truthy (anything but `nil` and `false`). Examples:

```clojure
(= "bad" (ex-message ex))
(contains? (ex-data ex) :id)
(and (= 2 (:y (ex-data ex))) (> x 1))     ; x: a local of the throwing frame
```

protoClojure has no `instance?`: select by `ex-message`, `ex-data` or a local
instead. The condition runs on the throwing thread with the debugger's hooks
off (see "Evaluations the debugger makes" below). A condition that cannot be
read is reported in the Debug Console when it is set; a condition that fails
when it runs (it throws) **stops**, and the stop's description and the Debug
Console say `exception breakpoint condition `...` failed: <message>` — a
condition is never skipped silently.

What is never an exception stop:

- a rethrow: the `RETHROW` of a `finally` or of a `try` none of whose
  clauses matched raises the exception already reported, so one throw
  stops once;
- the runtime's own probes, raised and caught inside C++ (an integer
  constant or a `format` argument that may not fit a long);
- `loop`/`recur`, `reduce`, lazy seqs, `doseq`, iteration in general: none
  is implemented with exceptions in protoClojure, so there is nothing to
  filter;
- code run by the debugger itself (`evaluate`), and code with no frame of
  the script on the thread (clojure.core while it loads);
- a C++ exception that is not a language exception: `StackOverflowError`
  (the guard of the native stack) and a module's defect, which become
  exceptions only where a `try` catches them.

### Conditional, hit-count and log breakpoints

A line breakpoint can carry (right-click the breakpoint → Edit Breakpoint):

- **Condition**: a Clojure form evaluated in the frame at the line, with its
  locals and the globals; the thread stops when it is truthy. A condition that
  fails stops, with `breakpoint condition `...` failed: <message>` as the
  stop's description and in the Debug Console; one that cannot be read is also
  reported when it is set.
- **Hit count**: counts the hits of the breakpoint whose condition passed (all
  threads together; the count starts again when the file's breakpoints are
  set again) and stops when it matches:

  | Hit count | Stops on |
  |-----------|----------|
  | `N`, `== N` | the Nth hit only |
  | `>= N`, `> N` | the Nth hit and later / after the Nth |
  | `<= N`, `< N` | the first N hits / the first N-1 |
  | `% N` | every Nth hit (N, 2N, ...) |

  Anything else marks the breakpoint unverified, with a message.
- **Log message**: a log point — it never stops; it prints the message to the
  Debug Console, each `{form}` replaced by the form's value in the frame (a
  string without quotes, anything else as `pr-str` would print it); `{{` and
  `}}` are literal braces, and a form that fails prints `<error: message>`. A
  log point with a condition or a hit count logs only when they pass.

  ```
  i={i} total={(reduce + xs)} {{literal braces}}
  ```

### Evaluations the debugger makes

Conditions, log messages, Watch, hover and the Debug Console run on the thread
they inspect (the values are its own), with that thread's breakpoints, steps
and exception stops off: a condition that throws, or that calls a function
with a breakpoint, never enters the debugger again. Other threads are not
affected.

### Threads

When a thread stops — a breakpoint, a step, a pause, an exception — **only
that thread stops**; the others keep running. Each stopped thread is
inspected on its own (call stack, Locals, evaluate) and resumed on its own:
Continue, Step Over / Into / Out and Pause act on the thread the request
names. Several threads can be stopped at once. Breakpoints apply to every
thread; stepping in one thread never stops another.

Thread names: `main` for the script's own thread; `future@file.clj:12` for
a future whose body starts on line 12 of file.clj (also when the body calls
a fn defined elsewhere: the line is the one the future's thread entered
first); `pmap@file.clj:7` for an element of a `pmap`; `actor worker N` for
the threads that run actors' turns (a worker runs the turns of every actor
in turn); `http@file.clj:30` for a connection thread of an HTTP server.
VS Code shows a thread when it first runs a line of the script, and drops it
when it ends.

Pause names a thread too. A thread that is running the script's code stops
at the next line it enters; a thread that is waiting (a `deref`, a socket
read, a join) or is inside clojure.core's C++ code stops when it comes back
to a line of the script.

### Which line a breakpoint binds to

Clojure forms span lines, and several can start on one line. The rules:

- A breakpoint binds to the first line, from its own on, where a list form
  starts — `(` or `#(`. VS Code shows a breakpoint set on a blank line, a
  comment or the middle of a vector moved to that line.
- Execution stops there when a frame **enters** the line: when it begins
  evaluating the first form that starts on that line — before the form's
  arguments are evaluated. Further forms on the same line, and nested calls
  that return to it, do not stop again. Each iteration of a `loop` (or a
  fn-level `recur`) enters its lines again, so a breakpoint inside a loop stops
  on every iteration.
- The line of `(defn f [x]` or `(fn [x]` binds to the **function's entry**: it
  stops at every call of `f`, with its arguments in Locals, and not when the
  function is defined. A one-line `(defn f [x] (g x))` stops on entry only.
- Code that runs while the script is compiled — a `defmacro`'s body — never
  stops.

Stepping uses the same notion of entering a line. **Step Into** stops at the
next line entered anywhere, which goes into the functions the line calls (the
script's own; clojure.core's functions have no lines and are stepped over).
**Step Over** stops at the next line entered by the same frame or a caller.
**Step Out** stops at the next line entered by a caller: when the call was the
last thing on its line, that is the caller's next line.

## How it works

- The reader records, for each list form, the line and column it starts at
  (`src/reader/SourceInfo.h`), only when the driver asks for it — only
  `protoclj --dap` does.
- With those positions the compiler emits a `LINE` instruction before each list
  form, records each local's name, slot and range of instructions, and names
  each function body (`DebugInfo` in `src/runtime/BytecodeModule.h`). Under the
  debugger locals are not cleared after their last use (as Clojure's
  `disable-locals-clearing`), so Locals shows every binding in scope.
- A body with debug information pushes a `DebugFrame` while it runs
  (`src/runtime/DebugHook.h`); `LINE` calls the debugger's hook when the frame
  enters a line. Code compiled without debug information — clojure.core, and
  every program not run under `--dap` — has no `LINE` instructions and costs one
  test per call.
- The adapter (`src/dap/`) reads the protocol on a plain thread. A thread that
  stops stays inside the hook, parked in a protoCore unmanaged region, and
  answers the requests that read the program (stack, scopes, variables,
  evaluate) itself, on its own context, so every value it reads is rooted in the
  stopped frames. Each thread has its own stop state and its own queue of
  requests; frame ids and variable references carry the id of the thread
  that owns them, so a request reaches the thread whose stack holds the
  value. A stopped thread does not hold up the collector: the other threads
  allocate and collections run while it is stopped, and its frames' values
  stay rooted. The script's file descriptors 1 and 2 are pipes the adapter
  drains into `output` events; descriptor 0 is the null device.
- The runtime's raise funnels — `throw` (`throwException`), the runtime's
  helpers (`throwClassed`, `throwError`) and the error factory protoCore's
  kernel errors go through — call the hook before the C++ exception is
  thrown (`DebugHook::onThrow`, `src/runtime/Exceptions.cpp`). Each frame of
  the debugger's chain points at its active `try` handlers, whose handler
  code (`STORE_CAUGHT`, the clauses' type tests, `RETHROW`) the Uncaught
  filter reads to tell whether a clause would select the exception
  (`debugFrameCatches`, `src/runtime/ExecutionEngine.cpp`).

## Limitations

- A thread blocked in native code (a `deref`, I/O, a join), or running
  clojure.core's C++ code, is listed but cannot show a call stack until it
  stops; a pause takes effect when it returns to the script's code. A thread
  that has not run a line of the script yet is not listed.
- All stops are one-thread stops; there is no "stop every thread" mode.
- Uncaught Exceptions: a `catch` whose type is an arbitrary expression the
  adapter cannot work out without running code counts as catching; an
  exception raised as a C++ exception (StackOverflowError, a module's
  defect) never stops.
- No data breakpoints, function breakpoints, `setVariable`, restart or attach.
- A condition or log form is compiled each time it runs (there is no cache):
  a breakpoint with a condition costs a compilation per hit.
- Forms a macro builds have no lines: a breakpoint binds to the forms of the
  script itself, including those a macro call passes through (the body of a
  `when`, a `doseq`, ...). Lines inside quoted data or a syntax-quote template
  can be set but are never reached.
- The Variables view never runs the script's code: a lazy seq shows as far as
  it has been realised, `(1 2 ...)`, and long or deep collections are cut
  (100 elements, 8 levels). An `evaluate` result is realised up to the
  elements shown, and is not expandable.
- `evaluate` can bind at most 16 of the frame's locals at once.
- Ending the session (Stop, or the client going away) ends the script.

## Testing

`tests/cli/dap-session.py` (ctest `cli/dap-session`) drives `protoclj --dap` as
VS Code does — absolute paths with a lowercase drive letter on Windows — through
a breakpoint in a function, the call stack, Locals, evaluate, Step Over, Step
Out, a script printing more than 64 KB of non-ASCII text and reading standard
input, `stopOnEntry`, a breakpoint in a loop, a breakpoint moved to the next
form and a script that throws; both exception filters (the throw's line and
locals, `exceptionInfo`, one stop through a `finally`, a division by zero,
no stop on the runtime's own control flow, no stop with the filters off, no
Uncaught stop on caught throws — also through `map`'s lazy seq — and a stop
on a `catch` of another class); two futures stopped at the same breakpoint
while `main` runs on, stepping one while the other stays stopped, pausing one
busy future, and a thread stopped while another allocates through several
collections; exception filter conditions (true, false, one that throws, one
that does not read, one calling a fn with a breakpoint), line conditions, hit
counts (`3`, `>= 2`, `% 2`, with a condition, an invalid one), log points
(`{i}`, `{{literal}}`, an expression error), and a condition evaluated per
thread. A watchdog fails a session that hangs.
