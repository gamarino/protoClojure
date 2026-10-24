#!/usr/bin/env python3
"""CLI check: `protoclj --dap`, driven the way VS Code drives it.

Registered as the `cli/dap-session` ctest case by tests/CMakeLists.txt.
Usage: dap-session.py <path-to-protoclj>

Each session speaks framed DAP to `protoclj --dap` and reacts to its events.
Paths are absolute, with a lowercase drive letter on Windows, as VS Code sends
them. Covered:
  1. breakpoint in a fn -> stackTrace (fn name, line, caller) -> Locals
     (parameters) -> evaluate (a local, an expression over locals) -> next
     (a let local appears) -> stepOut (back in the caller, on its next line)
     -> continue -> terminated / exited 0; the program's stdout (printed
     before, between and after the stops, more than 64 KB of non-ASCII text)
     arrives as output events, and its (read-line) sees end of file;
  2. stopOnEntry, then stepIn;
  3. a breakpoint inside a loop stops on every iteration;
  4. a breakpoint on a line where no form starts moves to the next one;
  5. a program that throws: the error reaches stderr, exit status 1.
A watchdog fails a session that hangs instead of hanging the test.
"""
import json
import os
import subprocess
import sys
import tempfile
import threading
import time

PROTOCLJ = sys.argv[1]
FAILURES = []

PROGRAM = """\
(println "before")
(defn add [a b]
  (let [s (+ a b)]
    (* s 2)))
(def big (apply str (repeat 9000 "ñandú€")))
(println big)
(println "stdin:" (read-line))
(let [r (add 1 2)]
  (println "after" r))
(loop [i 0]
  (when (< i 3)
    (recur (inc i))))

(println "done")
"""

THROWS = """\
(println "start")
(defn boom [x] (throw (ex-info "kaboom" {:x x})))
(boom 7)
"""


def vscode_path(p):
    p = os.path.abspath(p)
    if os.name == "nt" and len(p) > 1 and p[1] == ":":
        p = p[0].lower() + p[1:]
    return p


def line_of(text, needle):
    for i, l in enumerate(text.splitlines(), 1):
        if needle in l:
            return i
    raise AssertionError(needle)


class Session:
    def __init__(self, timeout=60):
        self.p = subprocess.Popen([PROTOCLJ, "--dap"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.seq = 1
        self.buf = b""
        self.outputs = []        # (category, text)
        self.pending = []        # messages read while waiting for something else
        self.timer = threading.Timer(timeout, self.p.kill)
        self.timer.start()

    def send(self, command, arguments=None):
        seq = self.seq
        self.seq += 1
        body = json.dumps({"seq": seq, "type": "request", "command": command,
                           "arguments": arguments or {}}).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()
        return seq

    def _read(self):
        while b"\r\n\r\n" not in self.buf:
            c = self.p.stdout.read1(65536)
            if not c:
                raise EOFError("adapter closed its output")
            self.buf += c
        head, rest = self.buf.split(b"\r\n\r\n", 1)
        n = 0
        for h in head.decode().split("\r\n"):
            if h.lower().startswith("content-length:"):
                n = int(h.split(":")[1])
        while len(rest) < n:
            c = self.p.stdout.read1(65536)
            if not c:
                raise EOFError("adapter closed its output mid-message")
            rest += c
        self.buf = rest[n:]
        m = json.loads(rest[:n].decode("utf-8"))
        if m.get("type") == "event" and m.get("event") == "output":
            self.outputs.append((m["body"]["category"], m["body"]["output"]))
        return m

    def next_message(self):
        if self.pending:
            return self.pending.pop(0)
        return self._read()

    def wait(self, pred):
        kept = []
        try:
            while True:
                m = self.next_message()
                if pred(m):
                    return m
                kept.append(m)
        finally:
            self.pending = kept + self.pending

    def request(self, command, arguments=None):
        seq = self.send(command, arguments)
        return self.wait(lambda m: m.get("type") == "response" and m.get("request_seq") == seq)

    def event(self, name):
        return self.wait(lambda m: m.get("type") == "event" and m.get("event") == name)

    def start(self, program, breakpoints=(), stop_on_entry=False, bp_path=None):
        self.request("initialize", {"adapterID": "protoclj", "linesStartAt1": True})
        self.request("launch", {"program": program, "stopOnEntry": stop_on_entry})
        r = self.request("setBreakpoints", {"source": {"path": bp_path or program},
                                            "breakpoints": [{"line": l} for l in breakpoints]})
        self.request("configurationDone")
        return r

    def top(self):
        st = self.request("stackTrace", {"threadId": 1})
        return st["body"]["stackFrames"]

    def locals(self, frame_id):
        sc = self.request("scopes", {"frameId": frame_id})
        ref = sc["body"]["scopes"][0]["variablesReference"]
        va = self.request("variables", {"variablesReference": ref})
        return {v["name"]: v["value"] for v in va["body"]["variables"]}

    def evaluate(self, expr, frame_id):
        return self.request("evaluate", {"expression": expr, "frameId": frame_id, "context": "watch"})

    def finish(self):
        self.event("terminated")
        ex = self.event("exited")
        self.request("disconnect")
        try:
            rc = self.p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.p.kill()
            rc = None
        self.timer.cancel()
        return ex["body"]["exitCode"], rc

    def stdout_text(self):
        return "".join(t for c, t in self.outputs if c == "stdout")

    def stderr_text(self):
        return "".join(t for c, t in self.outputs if c == "stderr")


def check(cond, what):
    if not cond:
        FAILURES.append(what)
        print("FAIL:", what)
    return cond


def session_breakpoint_and_steps(prog):
    s = Session()
    bp = line_of(PROGRAM, "(let [s (+ a b)]")
    r = s.start(prog, [bp])
    check(r["body"]["breakpoints"][0]["verified"] and r["body"]["breakpoints"][0]["line"] == bp,
          "breakpoint verified at line %d: %r" % (bp, r["body"]))
    st = s.event("stopped")
    check(st["body"]["reason"] == "breakpoint", "stopped reason: %r" % st["body"])
    big = "ñandú€" * 9000
    printed = "before\n" + big + "\nstdin: nil\n"
    check(s.stdout_text() == printed, "output before the stop (%d chars, expected %d): %a"
          % (len(s.stdout_text()), len(printed), s.stdout_text()[:60]))
    frames = s.top()
    check(frames[0]["name"] == "add" and frames[0]["line"] == bp, "top frame: %r" % frames[0])
    check(os.path.normcase(frames[0]["source"]["path"]) == os.path.normcase(prog),
          "frame source: %r" % frames[0]["source"])
    check(len(frames) == 2 and frames[1]["line"] == line_of(PROGRAM, "(let [r (add 1 2)]"),
          "caller frame: %r" % frames)
    loc = s.locals(frames[0]["id"])
    check(loc.get("a") == "1" and loc.get("b") == "2" and "s" not in loc, "locals at entry: %r" % loc)
    e = s.evaluate("a", frames[0]["id"])
    check(e["success"] and e["body"]["result"] == "1", "evaluate a: %r" % e)
    e = s.evaluate("(+ a b 100)", frames[0]["id"])
    check(e["success"] and e["body"]["result"] == "103", "evaluate (+ a b 100): %r" % e)
    e = s.evaluate("(str \"x\" :k [a {:b b}])", frames[0]["id"])
    check(e["success"] and e["body"]["result"] == '"x:k[1 {:b 2}]"', "evaluate str: %r" % e)
    e = s.evaluate("(take 3 (iterate inc a))", frames[0]["id"])
    check(e["success"] and e["body"]["result"] == "(1 2 3)", "evaluate a lazy seq: %r" % e)
    e = s.evaluate("(undefined-fn 1)", frames[0]["id"])
    check(not e["success"], "evaluate of an unknown fn fails: %r" % e)

    s.request("next", {"threadId": 1})
    st = s.event("stopped")
    frames = s.top()
    check(st["body"]["reason"] == "step" and frames[0]["line"] == bp + 1,
          "next -> line %d: %r %r" % (bp + 1, st["body"], frames[0]))
    loc = s.locals(frames[0]["id"])
    check(loc.get("s") == "3", "let local after next: %r" % loc)

    s.request("stepOut", {"threadId": 1})
    st = s.event("stopped")
    frames = s.top()
    after = line_of(PROGRAM, '(println "after" r)')
    check(frames[0]["line"] == after and frames[0]["name"] == "<top level>",
          "stepOut -> caller line %d: %r" % (after, frames[0]))
    loc = s.locals(frames[0]["id"])
    check(loc.get("r") == "6", "caller local r: %r" % loc)

    s.request("continue", {"threadId": 1})
    code, rc = s.finish()
    check(code == 0 and rc == 0, "exit status: exited=%r process=%r" % (code, rc))
    expected = "before\n" + big + "\nstdin: nil\nafter 6\ndone\n"
    out = s.stdout_text()
    check(out == expected, "program output (%d chars, expected %d): %a" % (len(out), len(expected), out[:60]))
    check(len(big.encode("utf-8")) > 65536, "the big line is more than 64 KB")


def session_stop_on_entry(prog):
    s = Session()
    s.start(prog, [], stop_on_entry=True)
    st = s.event("stopped")
    frames = s.top()
    check(st["body"]["reason"] == "entry" and frames[0]["line"] == 1, "entry stop: %r %r" % (st["body"], frames))
    check(s.stdout_text() == "", "nothing printed before the entry stop: %r" % s.stdout_text())
    s.request("stepIn", {"threadId": 1})
    st = s.event("stopped")
    frames = s.top()
    check(st["body"]["reason"] == "step" and frames[0]["line"] == line_of(PROGRAM, "(def big"),
          "stepIn from entry: %r" % frames[0])
    s.request("continue", {"threadId": 1})
    code, rc = s.finish()
    check(code == 0 and rc == 0, "entry session exit: %r %r" % (code, rc))


def session_loop(prog):
    s = Session()
    bp = line_of(PROGRAM, "(when (< i 3)")
    s.start(prog, [bp])
    seen = []
    while True:
        m = s.wait(lambda m: m.get("type") == "event" and m.get("event") in ("stopped", "terminated"))
        if m["event"] == "terminated":
            s.pending.insert(0, m)
            break
        frames = s.top()
        e = s.evaluate("i", frames[0]["id"])
        seen.append(e["body"]["result"] if e["success"] else e)
        s.request("continue", {"threadId": 1})
    code, rc = s.finish()
    check(seen == ["0", "1", "2", "3"], "loop iterations seen: %r" % seen)
    check(code == 0, "loop session exit: %r" % code)


def session_moved_breakpoint(prog):
    s = Session()
    blank = line_of(PROGRAM, "(println \"done\")") - 1   # the empty line before it
    r = s.start(prog, [blank])
    b = r["body"]["breakpoints"][0]
    check(b["verified"] and b["line"] == blank + 1, "moved breakpoint: %r" % b)
    st = s.event("stopped")
    check(s.top()[0]["line"] == blank + 1, "stopped on the moved line")
    s.request("continue", {"threadId": 1})
    s.finish()


def session_throws(prog):
    s = Session()
    s.start(prog, [])
    code, rc = s.finish()
    check(code == 1, "a program that throws exits 1: %r" % code)
    check("kaboom" in s.stderr_text(), "the error reaches stderr: %r" % s.stderr_text())
    check(s.stdout_text() == "start\n", "its output: %r" % s.stdout_text())


def main():
    with tempfile.TemporaryDirectory() as work:
        prog = os.path.join(work, "prog.clj")
        with open(prog, "w", encoding="utf-8", newline="\n") as f:
            f.write(PROGRAM)
        throws = os.path.join(work, "throws.clj")
        with open(throws, "w", encoding="utf-8", newline="\n") as f:
            f.write(THROWS)
        prog = vscode_path(prog)
        throws = vscode_path(throws)
        for name, fn, arg in [("breakpoint and steps", session_breakpoint_and_steps, prog),
                              ("stop on entry", session_stop_on_entry, prog),
                              ("breakpoint in a loop", session_loop, prog),
                              ("moved breakpoint", session_moved_breakpoint, prog),
                              ("uncaught exception", session_throws, throws)]:
            started = time.time()
            try:
                fn(arg)
            except Exception as e:  # a hang (watchdog), a protocol error
                check(False, "%s: %s: %s" % (name, type(e).__name__, e))
            print("session %-22s %.1fs" % (name, time.time() - started))
    if FAILURES:
        print("%d check(s) failed" % len(FAILURES))
        sys.exit(1)
    print("OK")


if __name__ == "__main__":
    main()
