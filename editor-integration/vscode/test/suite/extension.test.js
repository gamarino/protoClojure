// Integration tests of the debug extension, run inside a real VS Code
// Extension Development Host (see ../runTest.js). They drive VS Code's own
// debug API -- startDebugging, breakpoints, customRequest -- so the
// extension's code (configuration provider, adapter descriptor factory) runs
// exactly as on F5, against the installed runtime's `--dap` adapter.
//
// Shared verbatim by the five proto* extensions; ../lang.js holds the
// per-language bits (debug type, fixture lines, the local to inspect ...).
const vscode = require("vscode");
const assert = require("assert");
const path = require("path");
const fs = require("fs");
const os = require("os");
const cp = require("child_process");
const lang = require("../lang");

const FIXTURE_DIR = path.resolve(__dirname, "..", "fixture");
const PROGRAM = path.join(FIXTURE_DIR, lang.fixture);
const WSL_DISTRO = process.env.PROTO_E2E_WSL_DISTRO || "Ubuntu-24.04";
const REMOTE_ROOT = "/tmp/proto-vscode-e2e/" + lang.debugType;

// ---------------------------------------------------------------- recording
// Every message between VS Code and the adapter, per session, through a
// debug adapter tracker (it sees the real traffic, whatever sent it).
const records = new Map();
const terminated = new Set();

function recordFor(session) {
  let rec = records.get(session.id);
  if (!rec) {
    rec = { session, messages: [], errors: [], exit: undefined };
    records.set(session.id, rec);
  }
  return rec;
}

function events(rec, name) {
  return rec.messages.filter((m) => m.type === "event" && m.event === name);
}

function stdout(rec) {
  return events(rec, "output")
    .filter((m) => !m.body.category || m.body.category === "stdout")
    .map((m) => m.body.output)
    .join("");
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function until(what, predicate, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const v = predicate();
    if (v) return v;
    if (Date.now() > deadline) throw new Error("timed out after " + timeoutMs + " ms waiting for " + what);
    await sleep(50);
  }
}

function withTimeout(promise, ms, what) {
  let timer;
  return Promise.race([
    promise,
    new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(what + " did not settle within " + ms + " ms")), ms);
    }),
  ]).finally(() => clearTimeout(timer));
}

// The n-th (0-based) event `name` of a session, waiting for it.
function waitEvent(rec, name, n = 0, timeoutMs = 60000) {
  return until(`'${name}' event #${n + 1} of ${lang.debugType}`, () => events(rec, name)[n], timeoutMs);
}

// ------------------------------------------------------------------ helpers
function workspaceFolder() {
  const f = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
  assert.ok(f, "the test host must open test/fixture as its workspace");
  return f;
}

function samePath(a, b) {
  const norm = (p) => {
    const n = path.normalize(p);
    return process.platform === "win32" ? n.toLowerCase() : n;
  };
  return typeof a === "string" && typeof b === "string" && norm(a) === norm(b);
}

function setBreakpoint(line, file = PROGRAM) {
  const loc = new vscode.Location(vscode.Uri.file(file), new vscode.Position(line - 1, 0));
  vscode.debug.addBreakpoints([new vscode.SourceBreakpoint(loc)]);
}

function launchConfig(extra = {}) {
  return Object.assign({
    type: lang.debugType,
    request: "launch",
    name: "e2e " + lang.debugType,
    program: PROGRAM,
    cwd: FIXTURE_DIR,
  }, extra);
}

// Starts a session and returns {ok, session, rec}. `ok` is what
// vscode.debug.startDebugging resolved to.
async function start(config, timeoutMs = 60000) {
  let startedSession;
  const sub = vscode.debug.onDidStartDebugSession((s) => {
    if (s.type === lang.debugType && !startedSession) startedSession = s;
  });
  try {
    const ok = await withTimeout(
      Promise.resolve(vscode.debug.startDebugging(workspaceFolder(), config)), timeoutMs, "startDebugging");
    if (!ok) return { ok, session: startedSession };
    const session = await until("the debug session to start", () => startedSession, timeoutMs);
    return { ok, session, rec: recordFor(session) };
  } finally {
    sub.dispose();
  }
}

async function topFrames(session, threadId) {
  const r = await session.customRequest("stackTrace", { threadId, startFrame: 0, levels: 20 });
  assert.ok(r.stackFrames && r.stackFrames.length > 0, "stackTrace returned no frames");
  return r.stackFrames;
}

// The printed value of the variable `name` in some scope of `frameId`.
async function localValue(session, frameId, name) {
  const { scopes } = await session.customRequest("scopes", { frameId });
  assert.ok(scopes && scopes.length, "no scopes for the top frame");
  const ordered = [...scopes].sort((a, b) => (/local/i.test(b.name) ? 1 : 0) - (/local/i.test(a.name) ? 1 : 0));
  const seen = [];
  for (const scope of ordered) {
    const { variables } = await session.customRequest("variables", { variablesReference: scope.variablesReference });
    for (const v of variables || []) {
      seen.push(`${scope.name}:${v.name}=${v.value}`);
      if (v.name === name) return v.value;
    }
  }
  assert.fail(`no variable '${name}' in the top frame; saw ${seen.join(", ")}`);
}

async function continueAndFinish(session, rec, threadId) {
  await session.customRequest("continue", { threadId });
  await waitEvent(rec, "terminated");
  await until("the session to end", () => terminated.has(session.id), 30000);
}

// Replaces window.show{Error,Information,Warning}Message with recorders whose
// promise never settles -- like a notification nobody dismisses -- so a launch
// that waits for the user to close a message is caught hanging.
function stubMessages() {
  const shown = [];
  const names = ["showErrorMessage", "showInformationMessage", "showWarningMessage"];
  const original = {};
  for (const n of names) {
    original[n] = vscode.window[n];
    const stub = (message) => {
      shown.push({ kind: n, message: String(message) });
      return new Promise(() => {});
    };
    vscode.window[n] = stub;
    assert.strictEqual(vscode.window[n], stub, "cannot stub vscode.window." + n);
  }
  return {
    shown,
    restore() {
      for (const n of names) vscode.window[n] = original[n];
    },
  };
}

// The executables of every live descendant of this process (the extension
// host, which spawns debug adapters).
function descendantExecutables() {
  let procs;
  if (process.platform === "win32") {
    const out = cp.execFileSync("powershell.exe", [
      "-NoProfile", "-NonInteractive", "-Command",
      "Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,ExecutablePath | ConvertTo-Json -Compress",
    ], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
    procs = JSON.parse(out).map((p) => ({ pid: p.ProcessId, ppid: p.ParentProcessId, exe: p.ExecutablePath }));
  } else {
    const out = cp.execFileSync("ps", ["-eo", "pid=,ppid="], { encoding: "utf8" });
    procs = out.trim().split("\n").map((l) => {
      const [pid, ppid] = l.trim().split(/\s+/).map(Number);
      let exe;
      try { exe = fs.readlinkSync(`/proc/${pid}/exe`); } catch (e) { exe = undefined; }
      return { pid, ppid, exe };
    });
  }
  const mine = new Set([process.pid]);
  let grew = true;
  while (grew) {
    grew = false;
    for (const p of procs) {
      if (!mine.has(p.pid) && mine.has(p.ppid)) {
        mine.add(p.pid);
        grew = true;
      }
    }
  }
  return procs.filter((p) => p.pid !== process.pid && mine.has(p.pid) && p.exe).map((p) => p.exe);
}

function defaultInstallPath() {
  if (process.platform === "win32") {
    const base = process.env.LOCALAPPDATA || path.join(os.homedir(), "AppData", "Local");
    return path.join(base, "Programs", "proto", "bin", lang.exe + ".exe");
  }
  return path.join(os.homedir(), ".local", "bin", lang.exe);
}

function toWslPath(p) {
  const m = /^([a-zA-Z]):[\\/](.*)$/.exec(p);
  assert.ok(m, "not a drive path: " + p);
  return `/mnt/${m[1].toLowerCase()}/${m[2].replace(/\\/g, "/")}`;
}

function wsl(script) {
  return cp.execFileSync("wsl.exe", ["-d", WSL_DISTRO, "--", "bash", "-lc", script],
                         { encoding: "utf8", timeout: 60000, windowsHide: true });
}

// Why the remote test cannot run here, or null.
function remoteUnavailable() {
  if (process.platform !== "win32") return "not Windows (the remote test goes through WSL)";
  try {
    wsl(`test -x '${lang.linuxExe}'`);
    return null;
  } catch (e) {
    return `WSL ${WSL_DISTRO} or ${lang.linuxExe} is not available`;
  }
}

function setting() {
  return vscode.workspace.getConfiguration(lang.settingSection);
}

// -------------------------------------------------------------------- suite
suite(`${lang.debugType} debug extension`, function () {
  let messages;

  suiteSetup(async function () {
    this.timeout(60000);
    vscode.debug.registerDebugAdapterTrackerFactory(lang.debugType, {
      createDebugAdapterTracker(session) {
        const rec = recordFor(session);
        return {
          onDidSendMessage: (m) => rec.messages.push(m),
          onError: (e) => rec.errors.push(e),
          onExit: (code, signal) => { rec.exit = { code, signal }; },
        };
      },
    });
    vscode.debug.onDidTerminateDebugSession((s) => terminated.add(s.id));
    const ext = vscode.extensions.getExtension(lang.extensionId);
    assert.ok(ext, "extension " + lang.extensionId + " is not loaded");
    await ext.activate();
  });

  setup(async function () {
    vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
    await setting().update("path", process.env.PROTO_E2E_EXE || undefined, vscode.ConfigurationTarget.Global);
  });

  teardown(async function () {
    this.timeout(60000);
    if (messages) {
      messages.restore();
      messages = undefined;
    }
    if (vscode.debug.activeDebugSession) {
      await vscode.debug.stopDebugging();
      await until("every session to end", () => !vscode.debug.activeDebugSession, 30000).catch(() => {});
    }
    vscode.debug.removeBreakpoints(vscode.debug.breakpoints);
    await setting().update("path", undefined, vscode.ConfigurationTarget.Global);
    await vscode.commands.executeCommand("workbench.action.closeAllEditors");
  });

  test("local launch: breakpoint, stack, a local's value, stdout, terminated", async function () {
    setBreakpoint(lang.breakpointLine);
    const { ok, session, rec } = await start(launchConfig());
    assert.strictEqual(ok, true, "startDebugging resolved false");
    const stop = await waitEvent(rec, "stopped");
    assert.strictEqual(stop.body.reason, "breakpoint", "stop reason " + JSON.stringify(stop.body));
    const threadId = stop.body.threadId;
    const frames = await topFrames(session, threadId);
    assert.strictEqual(frames[0].line, lang.breakpointLine, "top frame " + JSON.stringify(frames[0]));
    assert.ok(frames[0].source && samePath(frames[0].source.path, PROGRAM),
              "top frame source " + JSON.stringify(frames[0].source));
    assert.strictEqual(await localValue(session, frames[0].id, lang.local.name), lang.local.value);
    await until("the program's first output", () => stdout(rec).includes(lang.stdoutBefore), 10000);
    await continueAndFinish(session, rec, threadId);
    assert.ok(stdout(rec).includes(lang.stdoutAfter), "stdout was " + JSON.stringify(stdout(rec)));
  });

  test("F5 without launch.json: the provider debugs the active file", async function () {
    const doc = await vscode.workspace.openTextDocument(PROGRAM);
    await vscode.window.showTextDocument(doc);
    assert.strictEqual(doc.languageId, lang.languageId);
    setBreakpoint(lang.breakpointLine);
    // What VS Code passes to the provider on F5 with no launch.json: no type,
    // no request, no name.
    messages = stubMessages();
    const { ok, session, rec } = await start({});
    assert.strictEqual(ok, true, "startDebugging resolved false; shown: " + JSON.stringify(messages.shown));
    assert.strictEqual(session.configuration.type, lang.debugType);
    assert.strictEqual(session.configuration.request, "launch");
    assert.ok(samePath(session.configuration.program, PROGRAM),
              "program " + JSON.stringify(session.configuration.program));
    const stop = await waitEvent(rec, "stopped");
    assert.strictEqual(stop.body.reason, "breakpoint");
    const frames = await topFrames(session, stop.body.threadId);
    assert.strictEqual(frames[0].line, lang.breakpointLine);
    await continueAndFinish(session, rec, stop.body.threadId);
  });

  test(`exe lookup: a ${lang.settingSection}.path that does not exist fails clearly`, async function () {
    const missing = path.join(FIXTURE_DIR, "no-such-dir", lang.exe + (process.platform === "win32" ? ".exe" : ""));
    await setting().update("path", missing, vscode.ConfigurationTarget.Global);
    messages = stubMessages();
    const { ok } = await start(launchConfig(), 30000);
    assert.strictEqual(ok, false, "startDebugging resolved true with a missing executable");
    const err = messages.shown.find((m) => m.kind === "showErrorMessage");
    assert.ok(err, "no error message; shown: " + JSON.stringify(messages.shown));
    assert.ok(err.message.includes(missing) && err.message.includes(lang.settingSection + ".path"),
              "the message should name the setting and the path: " + err.message);
  });

  test("exe lookup: with the setting empty the default install is used", async function () {
    const installed = defaultInstallPath();
    if (!fs.existsSync(installed)) this.skip();
    await setting().update("path", undefined, vscode.ConfigurationTarget.Global);
    setBreakpoint(lang.breakpointLine);
    const { ok, session, rec } = await start(launchConfig());
    assert.strictEqual(ok, true);
    const stop = await waitEvent(rec, "stopped");
    const exes = descendantExecutables();
    const real = (p) => { try { return fs.realpathSync(p); } catch (e) { return p; } };
    assert.ok(exes.some((e) => samePath(real(e), real(installed))),
              `no adapter process runs ${installed}; children: ${JSON.stringify(exes)}`);
    await continueAndFinish(session, rec, stop.body.threadId);
  });

  test("remote launch through WSL with pathMappings", async function () {
    this.timeout(180000);
    const why = remoteUnavailable();
    if (why) {
      console.log("      (skipped: " + why + ")");
      this.skip();
    }
    // A Linux copy of the fixture (LF line ends, as a Linux checkout has).
    wsl(`rm -rf '${REMOTE_ROOT}' && mkdir -p '${REMOTE_ROOT}' && ` +
        `sed 's/\\r$//' '${toWslPath(PROGRAM)}' > '${REMOTE_ROOT}/${lang.fixture}'`);
    setBreakpoint(lang.breakpointLine);
    const libPath = lang.linuxLibPath;
    const { ok, session, rec } = await start(launchConfig({
      remote: {
        command: ["wsl", "-d", WSL_DISTRO, "--", "env", "LD_LIBRARY_PATH=" + libPath],
        path: lang.linuxExe,
      },
      pathMappings: [{ localRoot: FIXTURE_DIR, remoteRoot: REMOTE_ROOT }],
    }), 120000);
    assert.strictEqual(ok, true, "startDebugging resolved false");
    const stop = await waitEvent(rec, "stopped", 0, 120000);
    assert.strictEqual(stop.body.reason, "breakpoint", "stop " + JSON.stringify(stop.body));
    const frames = await topFrames(session, stop.body.threadId);
    assert.strictEqual(frames[0].line, lang.breakpointLine);
    assert.ok(frames[0].source && samePath(frames[0].source.path, PROGRAM),
              "the top frame should carry the Windows path: " + JSON.stringify(frames[0].source));
    assert.strictEqual(await localValue(session, frames[0].id, lang.local.name), lang.local.value);
    await continueAndFinish(session, rec, stop.body.threadId);
    assert.ok(stdout(rec).includes(lang.stdoutAfter), "stdout was " + JSON.stringify(stdout(rec)));
  });

  test("remote launch without pathMappings is refused with an explanation", async function () {
    messages = stubMessages();
    let started = false;
    const sub = vscode.debug.onDidStartDebugSession((s) => { if (s.type === lang.debugType) started = true; });
    try {
      const { ok } = await start(launchConfig({
        remote: { command: ["wsl", "-d", WSL_DISTRO, "--"], path: lang.linuxExe },
      }), 15000);
      assert.strictEqual(ok, false, "startDebugging resolved true");
    } finally {
      sub.dispose();
    }
    assert.strictEqual(started, false, "a session was started");
    const err = messages.shown.find((m) => m.kind === "showErrorMessage");
    assert.ok(err && err.message.includes("pathMappings"),
              "expected an error naming pathMappings; shown: " + JSON.stringify(messages.shown));
  });

  test("exception breakpoints: the 'raised' filter stops at the throw", async function () {
    setBreakpoint(lang.breakpointLine);
    const { ok, session, rec } = await start(launchConfig());
    assert.strictEqual(ok, true);
    const first = await waitEvent(rec, "stopped");
    assert.strictEqual(first.body.reason, "breakpoint");
    await session.customRequest("setExceptionBreakpoints", { filters: ["raised"] });
    await session.customRequest("continue", { threadId: first.body.threadId });
    const stop = await waitEvent(rec, "stopped", 1);
    assert.strictEqual(stop.body.reason, "exception", "second stop " + JSON.stringify(stop.body));
    const frames = await topFrames(session, stop.body.threadId);
    assert.strictEqual(frames[0].line, lang.exceptionLine, "top frame " + JSON.stringify(frames[0]));
    await session.customRequest("setExceptionBreakpoints", { filters: [] });
    await continueAndFinish(session, rec, stop.body.threadId);
    assert.ok(stdout(rec).includes(lang.stdoutAfter), "stdout was " + JSON.stringify(stdout(rec)));
  });

  test("contributions: launch schema, snippets and initial configurations agree", function () {
    const pkg = vscode.extensions.getExtension(lang.extensionId).packageJSON;
    // A "\b" meant as a regex word boundary but written unescaped in JSON is a
    // backspace character: no contributed string should hold control chars.
    const walk = (v, at) => {
      if (typeof v === "string") {
        assert.ok(!/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/.test(v), `control character in ${at}: ${JSON.stringify(v)}`);
      } else if (v && typeof v === "object") {
        for (const [k, x] of Object.entries(v)) walk(x, at + "." + k);
      }
    };
    walk(pkg.contributes, "contributes");
    const dbg =pkg.contributes.debuggers.find((d) => d.type === lang.debugType);
    assert.ok(dbg, "no debugger of type " + lang.debugType);
    assert.ok((dbg.languages || []).includes(lang.languageId), "debugger languages " + JSON.stringify(dbg.languages));
    assert.ok((pkg.contributes.breakpoints || []).some((b) => b.language === lang.languageId),
              "no breakpoints contribution for " + lang.languageId);
    assert.ok(pkg.contributes.configuration.properties[lang.settingSection + ".path"], "no path setting");
    const launch = dbg.configurationAttributes.launch;
    const known = new Set(["type", "request", "name", ...Object.keys(launch.properties)]);
    const bodies = [...(dbg.initialConfigurations || []), ...(dbg.configurationSnippets || []).map((s) => s.body)];
    assert.ok(bodies.length > 0, "no initial configurations or snippets");
    for (const body of bodies) {
      const where = JSON.stringify(body);
      assert.strictEqual(body.type, lang.debugType, where);
      assert.strictEqual(body.request, "launch", where);
      for (const k of Object.keys(body)) assert.ok(known.has(k), `unknown attribute '${k}' in ${where}`);
      for (const k of launch.required || []) assert.ok(k in body, `missing required '${k}' in ${where}`);
      if (body.remote) {
        assert.ok(Array.isArray(body.remote.command) && body.remote.command.length > 0, where);
        assert.ok(Array.isArray(body.pathMappings) && body.pathMappings.length > 0,
                  "a remote snippet without pathMappings would be refused: " + where);
      }
    }
  });
});
