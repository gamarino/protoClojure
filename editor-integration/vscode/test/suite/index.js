// Mocha inside the Extension Development Host: @vscode/test-electron loads
// this module and awaits run().
const path = require("path");
const fs = require("fs");
const Mocha = require("mocha");

function run() {
  const mocha = new Mocha({
    ui: "tdd",
    color: false,
    timeout: 120000,
    reporter: "spec",
  });
  if (process.env.PROTO_E2E_GREP) mocha.grep(process.env.PROTO_E2E_GREP);
  for (const f of fs.readdirSync(__dirname).sort()) {
    if (f.endsWith(".test.js")) mocha.addFile(path.join(__dirname, f));
  }
  return new Promise((resolve, reject) => {
    mocha.run((failures) => {
      if (failures > 0) reject(new Error(failures + " test(s) failed"));
      else resolve();
    });
  });
}

module.exports = { run };
