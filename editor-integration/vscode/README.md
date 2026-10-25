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
code --install-extension protoclj-debug-0.4.1.vsix
```

On Linux and macOS you can symlink the folder instead (picks up edits after
**Developer: Reload Window**):

```bash
ln -s "$(pwd)/editor-integration/vscode" ~/.vscode/extensions/protoclj-debug-0.4.1
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
catches; the thread stops before the exception propagates. Each filter takes a
condition, a Clojure form in which `ex` is the exception, for example
`(= "bad" (ex-message ex))`. Line breakpoints take a condition, a hit count
(`N`, `>= N`, `% N`...) and a log message (`i={i}`, `{{` for a literal brace).
Every thread that
runs the script — `main`, `future@file.clj:12`, `pmap@...`, the actor workers —
appears in the Call Stack view; a stop stops only its thread, the others keep
running, and Continue / Step / Pause act on the selected thread.

clojure.core's Clojure source is embedded in `protoclj`; the call stack and
**Loaded Scripts** show it read-only (`"justMyCode": false` steps into it).

**Remote debugging.** With VS Code's Remote - SSH / WSL / Dev Containers,
install `protoclj` and this extension on the remote side (the extension is a
workspace extension). Or keep VS Code local and add to the configuration
(snippets "Launch over SSH" and "Launch in WSL"):

```json
"remote": { "command": ["ssh", "-T", "-o", "BatchMode=yes", "me@host"], "path": "protoclj" },
"pathMappings": [ { "localRoot": "${workspaceFolder}", "remoteRoot": "/home/me/project" } ]
```

The extension runs `<command...> <path> --dap`; the adapter maps the paths. The
remote shell must print nothing when started non-interactively (no motd or
`echo` in `.bashrc`).

See `docs/debugging.md` for the rules, stepping, exceptions, threads, remote
debugging, JetBrains IDEs and the known limitations.

## Testing the extension

`test/` holds integration tests that run this extension inside a real VS Code
(an Extension Development Host, through `@vscode/test-electron` and mocha)
and debug `test/fixture/fixture.clj` with `protoclj`, driving VS Code's own
debug API the way F5 does:

```bash
cd editor-integration/vscode
npm install
npm test
```

They check: a local launch (breakpoint hit on the right line, `stackTrace`,
a local's value, the program's stdout as `output` events, `terminated`); F5
without a `launch.json` on the open fixture; the executable lookup (a
`protoclj.path` that does not exist is refused with a message naming it; with
the setting empty the default install is the process that runs); a `remote`
launch through WSL with `pathMappings` (the breakpoint is set by the Windows
path and the frames come back with Windows paths); a `remote` launch without
`pathMappings` refused with its explanation; the `raised` exception filter;
and that the launch schema, snippets and initial configurations agree.

- **Which VS Code.** `$VSCODE_EXECUTABLE` if set; on Windows the per-user
  install when present (unless it is halfway through an update, which makes
  every new instance refuse to start); otherwise a pinned VS Code is
  downloaded (`$VSCODE_VERSION` overrides the version) into
  `$VSCODE_TEST_CACHE`.
- **Isolation.** A throwaway `--user-data-dir` and `--extensions-dir` under
  `$PROTO_VSCODE_E2E_DIR` (default `C:\tmp\protowin\vscode-e2e` on Windows,
  `$TMPDIR/proto-vscode-e2e` elsewhere), recreated on every run, and every
  other extension disabled: your own VS Code may stay open.
- **Which runtime.** `$PROTO_E2E_EXE` (put into `protoclj.path`), else the
  extension's own lookup: default install, then `PATH`. The default-install
  test is skipped when there is no default install.
- **Remote.** Runs only on Windows with WSL (`$PROTO_E2E_WSL_DISTRO`, default
  `Ubuntu-24.04`) and the Linux build named in `test/lang.js`; it is skipped
  elsewhere.
- `$PROTO_E2E_GREP` runs only the tests whose title matches.

`test/runTest.js`, `test/suite/` are the same in the five proto* runtimes'
extensions; `test/lang.js` and the fixture hold the per-language part.
`test/` and `node_modules/` are left out of the `.vsix` (the `files` list in
`package.json`).
CI runs the local tests on Linux under `xvfb-run` (`.github/workflows/ci.yml`).
