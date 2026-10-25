// Minimal protoClojure debug adapter glue for VS Code.
//
// The protoclj binary itself IS the debug adapter: `protoclj --dap` speaks the
// Debug Adapter Protocol over stdin/stdout (docs/debugging.md). VS Code only
// needs an extension to register the debug `type` and tell it how to spawn
// that adapter. Clojure syntax highlighting is VS Code's own (language id
// `clojure`); this extension adds no grammar, so it coexists with Calva.
//
// `protoclj` is taken from the `protoclj.path` setting if set, else from the
// default install location (when present), else from PATH.
//
// Remote launch: a configuration with `remote.command` (an argv prefix such as
// ["ssh", "-T", "me@host"] or ["wsl", "-d", "Ubuntu-24.04", "--"]) runs
// `<command...> <remote.path> --dap` instead; everything about paths
// (pathMappings) is done by the adapter itself, so this file only chooses the
// command.
const vscode = require("vscode");
const fs = require("fs");
const path = require("path");
const os = require("os");

function defaultInstallPath() {
  if (process.platform === "win32") {
    const base = process.env.LOCALAPPDATA || path.join(os.homedir(), "AppData", "Local");
    return path.join(base, "Programs", "proto", "bin", "protoclj.exe");
  }
  return path.join(os.homedir(), ".local", "bin", "protoclj");
}

function configuredPath() {
  return vscode.workspace.getConfiguration("protoclj").get("path");
}

// What is wrong with the `protoclj.path` setting, or null: it names a file
// (not a bare command looked up in PATH) that does not exist. Checked before
// the launch, so the user reads which setting is wrong instead of a spawn
// error.
function configuredPathProblem() {
  const configured = configuredPath();
  if (!configured || !/[\\/]/.test(configured) || fs.existsSync(configured)) return null;
  return `protoClojure: the protoclj.path setting is "${configured}", which does not exist. ` +
    "Fix it, or clear it to use the default install location or protoclj from PATH.";
}

// Shows a message and aborts the launch, without waiting for the user to
// close the message: a launch that waited would stay pending for as long as
// the notification is not dismissed.
function refuse(kind, message) {
  vscode.window[kind](message);
  return undefined;
}

function protocljCommand() {
  const configured = configuredPath();
  if (configured) return configured;
  const installed = defaultInstallPath();
  return fs.existsSync(installed) ? installed : "protoclj";
}

// Lets F5 on an open Clojure file work without a launch.json, and fills in
// `program` when a configuration omits it.
const configurationProvider = {
  resolveDebugConfiguration(folder, config) {
    if (!config.type && !config.request && !config.name) {
      const editor = vscode.window.activeTextEditor;
      if (editor && editor.document.languageId === "clojure") {
        config.type = "protoclj";
        config.name = "Debug current Clojure script (protoclj)";
        config.request = "launch";
        config.program = "${file}";
      }
    }
    if (!config.program) {
      return refuse("showInformationMessage", "protoClojure: open a .clj file or set 'program' in launch.json.");
    }
    if (config.remote !== undefined) {
      const cmd = config.remote && config.remote.command;
      if (!Array.isArray(cmd) || cmd.length === 0 || !cmd.every((a) => typeof a === "string")) {
        return refuse(
          "showErrorMessage",
          'protoClojure: "remote.command" must be a non-empty array of strings, e.g. ["ssh", "-T", "me@host"].'
        );
      }
      if (!Array.isArray(config.pathMappings) || config.pathMappings.length === 0) {
        return refuse(
          "showErrorMessage",
          'protoClojure: a "remote" launch needs "pathMappings": [{"localRoot": "${workspaceFolder}", ' +
            '"remoteRoot": "<the same folder on the remote machine>"}], so that breakpoints and the ' +
            "call stack name the same files on both sides."
        );
      }
    } else {
      const problem = configuredPathProblem();
      if (problem) return refuse("showErrorMessage", problem);
    }
    return config;
  },
};

function activate(context) {
  const factory = {
    createDebugAdapterDescriptor(session) {
      const remote = session && session.configuration && session.configuration.remote;
      if (remote && Array.isArray(remote.command) && remote.command.length > 0) {
        const remotePath = remote.path || "protoclj";
        return new vscode.DebugAdapterExecutable(remote.command[0], [
          ...remote.command.slice(1),
          remotePath,
          "--dap",
        ]);
      }
      return new vscode.DebugAdapterExecutable(protocljCommand(), ["--dap"]);
    },
  };
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("protoclj", factory),
    vscode.debug.registerDebugConfigurationProvider("protoclj", configurationProvider)
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
