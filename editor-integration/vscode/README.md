# protoClojure Debugger — VS Code extension

Debugs Clojure scripts with [protoClojure](https://github.com/gamarino/protoClojure):
breakpoints, exception breakpoints, stepping, threads, call stack, locals, Watch
and the Debug Console. The debug adapter is the `protoclj` binary itself, run as
`protoclj --dap`, which speaks the [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
over stdin/stdout. What the script prints shows up in the Debug Console.

Syntax highlighting is VS Code's own Clojure support (language id `clojure`:
`.clj`, `.cljs`, `.cljc`, `.edn`). This extension adds no grammar and no
language — only the `protoclj` debug type and breakpoints for `clojure` — so it
coexists with Calva. There is no build step and no bundler.

## Requirements

`protoclj` must be installed or built (see `docs/debugging.md` in the
repository). The extension looks for it in this order:

1. the `protoclj.path` setting (Settings → Extensions → protoClojure), if set;
2. the default install location — `%LOCALAPPDATA%\Programs\proto\bin\protoclj.exe`
   on Windows, `~/.local/bin/protoclj` elsewhere — if it exists;
3. `protoclj` on your `PATH`.

## Install

```bash
cd editor-integration/vscode
npx @vscode/vsce@3.2.1 package --skip-license
code --install-extension protoclj-debug-0.2.0.vsix
```

On Linux and macOS you can symlink the folder instead (picks up edits after
**Developer: Reload Window**):

```bash
ln -s "$(pwd)/editor-integration/vscode" ~/.vscode/extensions/protoclj-debug-0.2.0
```

## Use

1. Open a folder with `.clj` scripts (for example `examples/`).
2. Open a script and click in the gutter to set breakpoints.
3. Press **F5**. Without a `launch.json` the extension debugs the active
   Clojure file; `examples/.vscode/launch.json` has ready-made configurations,
   and **Add Configuration…** → **protoClojure** writes one.

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

| Attribute     | Type     | Required | Meaning                                          |
|---------------|----------|----------|--------------------------------------------------|
| `program`     | string   | yes      | Path of the `.clj` script to debug.              |
| `args`        | string[] | no       | The script's arguments (`*command-line-args*`).  |
| `stopOnEntry` | boolean  | no       | Stop before the first form runs (default off).   |

A breakpoint binds to the first line, from its own on, where a form starts, and
stops when execution enters that line; on the line of `(defn f [x]` it stops at
each call of `f`.

The Breakpoints view offers two exception filters: **All Exceptions** stops at
every throw (and every error the runtime or protoCore raises, such as a division
by zero), **Uncaught Exceptions** only at one no `try`/`catch` of the script
catches; the thread stops before the exception propagates. Every thread that
runs the script — `main`, `future@file.clj:12`, `pmap@...`, the actor workers —
appears in the Call Stack view; a stop stops only its thread, the others keep
running, and Continue / Step / Pause act on the selected thread.

See `docs/debugging.md` for the rules, stepping, exceptions, threads and the
known limitations.
