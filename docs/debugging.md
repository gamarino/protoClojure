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
   code --install-extension protoclj-debug-0.1.0.vsix
   ```

   On Linux and macOS a symlink also works, and picks up edits after
   **Developer: Reload Window**:
   `ln -s "$(pwd)/editor-integration/vscode" ~/.vscode/extensions/protoclj-debug-0.1.0`.

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

- **Line breakpoints**, set and cleared at any time, also while the script runs.
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
  stopped frames. The script's file descriptors 1 and 2 are pipes the adapter
  drains into `output` events; descriptor 0 is the null device.

## Limitations

- One thread in the client's view, "main". A breakpoint hit on a `future`,
  `pmap` or actor thread stops that thread and shows its stack under the same
  id; the other threads keep running. Stops are taken one at a time.
- No exception breakpoints: an uncaught exception ends the script, and its
  message appears in the Debug Console, as on the command line.
- No conditional breakpoints, hit counts, logpoints, data breakpoints,
  `setVariable`, restart or attach.
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
form and a script that throws. A watchdog fails a session that hangs.
