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

function protocljCommand() {
  const configured = vscode.workspace.getConfiguration("protoclj").get("path");
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
      return vscode.window
        .showInformationMessage("protoClojure: open a .clj file or set 'program' in launch.json.")
        .then(() => undefined);
    }
    if (config.remote !== undefined) {
      const cmd = config.remote && config.remote.command;
      if (!Array.isArray(cmd) || cmd.length === 0 || !cmd.every((a) => typeof a === "string")) {
        return vscode.window
          .showErrorMessage('protoClojure: "remote.command" must be a non-empty array of strings, e.g. ["ssh", "-T", "me@host"].')
          .then(() => undefined);
      }
      if (!Array.isArray(config.pathMappings) || config.pathMappings.length === 0) {
        return vscode.window
          .showErrorMessage(
            'protoClojure: a "remote" launch needs "pathMappings": [{"localRoot": "${workspaceFolder}", ' +
              '"remoteRoot": "<the same folder on the remote machine>"}], so that breakpoints and the ' +
              "call stack name the same files on both sides."
          )
          .then(() => undefined);
      }
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
