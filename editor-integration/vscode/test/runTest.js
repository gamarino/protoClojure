// Runs the extension's integration tests in a VS Code Extension Development
// Host: `npm test` (from editor-integration/vscode, after `npm install`).
//
// Shared verbatim by the five proto* extensions; test/lang.js holds the
// per-language bits.
//
// Which VS Code: $VSCODE_EXECUTABLE if set; else, on Windows, the per-user
// install (%LOCALAPPDATA%\Programs\Microsoft VS Code\Code.exe) when present
// and not halfway through an update; else @vscode/test-electron downloads
// $VSCODE_VERSION (default pinned below) into $VSCODE_TEST_CACHE (default
// <e2e dir>/vscode-test, shared by the five repos). A throwaway
// --user-data-dir and --extensions-dir under the e2e dir
// ($PROTO_VSCODE_E2E_DIR, default C:\tmp\protowin\vscode-e2e on Windows,
// $TMPDIR/proto-vscode-e2e elsewhere) keep it apart from your own VS Code,
// which may stay open. Every extension but the one under test is disabled.
//
// $PROTO_E2E_EXE: the runtime the tests launch (the `<lang>.path` setting);
// empty = the extension's own lookup (default install, then PATH).
// $PROTO_E2E_GREP runs only the tests whose title matches (a mocha grep).
const path = require("path");
const fs = require("fs");
const os = require("os");
const { runTests } = require("@vscode/test-electron");
const lang = require("./lang");

const PINNED_VSCODE = "1.105.0";

function installedVSCode() {
  if (process.platform !== "win32") return undefined;
  const base = process.env.LOCALAPPDATA || path.join(os.homedir(), "AppData", "Local");
  const dir = path.join(base, "Programs", "Microsoft VS Code");
  const exe = path.join(dir, "Code.exe");
  if (!fs.existsSync(exe)) return undefined;
  // A background update waiting for VS Code to close holds a mutex that makes
  // every new Code.exe refuse to start ("Code is currently being updated").
  if (fs.existsSync(path.join(dir, "updating_version"))) {
    console.log("The installed VS Code has an update pending; using a downloaded one.");
    return undefined;
  }
  return exe;
}

async function main() {
  const extensionDevelopmentPath = path.resolve(__dirname, "..");
  const extensionTestsPath = path.resolve(__dirname, "suite", "index.js");
  const workspace = path.resolve(__dirname, "fixture");
  const root = process.env.PROTO_VSCODE_E2E_DIR ||
    (process.platform === "win32" ? "C:\\tmp\\protowin\\vscode-e2e"
                                  : path.join(os.tmpdir(), "proto-vscode-e2e"));
  const userDataDir = path.join(root, lang.debugType, "user-data");
  const extensionsDir = path.join(root, lang.debugType, "extensions");
  // A fresh profile every run: settings the suite writes never leak into the
  // next run.
  fs.rmSync(userDataDir, { recursive: true, force: true });
  fs.mkdirSync(userDataDir, { recursive: true });
  fs.mkdirSync(extensionsDir, { recursive: true });

  const options = {
    extensionDevelopmentPath,
    extensionTestsPath,
    launchArgs: [
      workspace,
      "--user-data-dir", userDataDir,
      "--extensions-dir", extensionsDir,
      // VS Code ignores every --disable-extension <id> once --disable-extensions
      // is given (and the latter leaves built-ins on). So: --disable-extensions
      // when no built-in is in the way, else the built-ins one by one -- the
      // throwaway extensions dir holds nothing else to disable.
      ...(lang.disableBuiltins.length === 0
        ? ["--disable-extensions"]
        : lang.disableBuiltins.flatMap((id) => ["--disable-extension", id])),
    ],
    extensionTestsEnv: {
      PROTO_E2E_GREP: process.env.PROTO_E2E_GREP || "",
      PROTO_E2E_EXE: process.env.PROTO_E2E_EXE || "",
      PROTO_E2E_DIR: root,
    },
  };
  // Run from a VS Code terminal (or a tool VS Code started), the environment
  // may carry ELECTRON_RUN_AS_NODE, which would start Code.exe as plain node.
  delete process.env.ELECTRON_RUN_AS_NODE;
  const exe = process.env.VSCODE_EXECUTABLE || installedVSCode();
  if (exe) {
    options.vscodeExecutablePath = exe;
  } else {
    options.version = process.env.VSCODE_VERSION || PINNED_VSCODE;
    // Shared by the five proto* extensions' runs (and cacheable in CI).
    options.cachePath = process.env.VSCODE_TEST_CACHE || path.join(root, "vscode-test");
  }
  try {
    await runTests(options);
  } catch (err) {
    console.error("The extension tests failed:", err && err.message ? err.message : err);
    process.exit(1);
  }
}

main();
