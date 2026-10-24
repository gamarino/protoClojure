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
  5. a program that throws: the error reaches stderr, exit status 1;
  6. exception breakpoints, "raised": a throw in a fn stops with its line,
     locals and exceptionInfo; a throw through a finally stops once; a
     protoCore error (division by zero) stops at the call; the catches run
     and the program ends normally;
  7. "raised" does not stop on the runtime's own control flow (reduce,
     loop/recur, lazy seqs, an integer probe in format, swap!...);
  8. no filters: no exception stop;
  9. "userUnhandled": no stop on throws a try/catch catches (also through
     map's lazy seq, a native boundary), a stop with the stack intact on
     one whose catch names another class;
 10. threads: two futures hit the same breakpoint -> two stopped events with
     their own threadIds and names, the main thread keeps running; stepping
     one leaves the other stopped; each continues on its own;
 11. pause of one busy future thread;
 12. a thread stopped while another allocates through several collections:
     the other finishes, the stopped one's locals are intact.
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


EXC = """\
(defn risky [x]
  (let [y (* x 2)]
    (throw (ex-info "bad" {:y y}))))
(defn safe-div [a b]
  (/ a b))
(println "start")
(try
  (risky 5)
  (catch Exception e
    (println "caught" (ex-message e))))
(try
  (try (risky 1) (finally (println "fin")))
  (catch clojure.lang.ExceptionInfo e (println "outer" (ex-message e))))
(try
  (safe-div 1 0)
  (catch ArithmeticException e (println "div" (ex-message e))))
(println "end")
"""

INTERNAL = """\
(defn total [xs]
  (reduce (fn [acc x] (+ acc x)) 0 (take-while #(< % 5) xs)))
(println (total (range 100)))
(println (loop [i 0 acc 0] (if (< i 50) (recur (inc i) (+ acc i)) acc)))
(println (count (filter even? (map inc (range 1000)))))
(println (format "%d" 123456789012345678901234567890))
(println (first (for [x (range 10) :when (> x 5)] x)))
(doseq [x (take 3 (iterate inc 0))] (print x))
(println)
(def a (atom 0))
(swap! a inc)
(println "ok" @a (into {} [[1 2]]) (apply str (seq "ab")) (lazy-seq nil))
"""

UNCAUGHT = """\
(defn risky [x]
  (let [y (* x 2)]
    (throw (ex-info "bad" {:y y}))))
(defn wrong-class []
  (try (risky 2) (catch ArithmeticException e :no)))
(try (risky 1) (catch Exception e (println "caught")))
(println (try (doall (map (fn [v] (if (= v 2) (risky v) v)) [1 2 3]))
              (catch Exception e "caught in map")))
(println (try (/ 1 0) (catch ArithmeticException e "div caught")))
(wrong-class)
"""

THREADS = """\
(defn work [n]
  (let [sq (* n n)]
    (+ sq 1)))
(def ticks (atom 0))
(def f1 (future (work 3)))
(def f2 (future (work 4)))
(dotimes [i 5] (swap! ticks inc))
(println "main ticks" @ticks)
(println "results" @f1 @f2)
"""

SPIN = """\
(def stop (atom false))
(def spinner (future (loop [n 0] (if @stop n (recur (inc n))))))
(println "spun" (pos? @spinner))
"""

GC = """\
(def go (atom false))
(defn churn []
  (loop [i 0 acc nil]
    (if (< i 400000) (recur (inc i) (vec (range 20))) (count acc))))
(def worker (future (loop [] (when-not @go (recur))) (println "churned" (churn))))
(defn hold [m]
  (let [v (vec (range 5)) k (keyword "kk")]
    (println "holding" (count m) (count v) k)))
(hold {:a 1 :b [1 2 3]})
@worker
(println "gc done")
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
    def __init__(self, timeout=60, env=None):
        full_env = None
        if env:
            full_env = dict(os.environ)
            full_env.update(env)
        self.p = subprocess.Popen([PROTOCLJ, "--dap"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=full_env)
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

    def start(self, program, breakpoints=(), stop_on_entry=False, bp_path=None, filters=None):
        self.capabilities = self.request("initialize", {"adapterID": "protoclj", "linesStartAt1": True})["body"]
        self.request("launch", {"program": program, "stopOnEntry": stop_on_entry})
        r = self.request("setBreakpoints", {"source": {"path": bp_path or program},
                                            "breakpoints": [{"line": l} for l in breakpoints]})
        if filters is not None:
            self.request("setExceptionBreakpoints", {"filters": list(filters)})
        self.request("configurationDone")
        return r

    def top(self, thread=1):
        st = self.request("stackTrace", {"threadId": thread})
        return st["body"]["stackFrames"]

    def stopped_or_end(self):
        """The next stopped event, or None when the program terminates first."""
        m = self.wait(lambda m: m.get("type") == "event" and m.get("event") in ("stopped", "terminated"))
        if m["event"] == "terminated":
            self.pending.insert(0, m)
            return None
        return m

    def wait_output(self, text, timeout=60):
        """Reads messages until the program's stdout contains `text`."""
        deadline = time.time() + timeout
        while text not in self.stdout_text():
            if time.time() > deadline:
                raise AssertionError("no %r in the output: %r" % (text, self.stdout_text()[-200:]))
            self.pending.append(self._read())

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


def session_exception_raised(prog):
    s = Session()
    s.start(prog, [], filters=["raised"])
    filters = {f["filter"] for f in s.capabilities.get("exceptionBreakpointFilters", [])}
    check(filters == {"raised", "userUnhandled"} and s.capabilities.get("supportsExceptionInfoRequest"),
          "initialize advertises the filters: %r" % s.capabilities)
    throw_line = line_of(EXC, "(throw (ex-info")
    st = s.event("stopped")
    check(st["body"]["reason"] == "exception" and "bad" in st["body"].get("description", ""),
          "raised: stopped on the throw: %r" % st["body"])
    frames = s.top()
    check(frames[0]["name"] == "risky" and frames[0]["line"] == throw_line,
          "raised: top frame at the throw: %r" % frames[0])
    loc = s.locals(frames[0]["id"])
    check(loc.get("x") == "5" and loc.get("y") == "10", "raised: locals at the throw: %r" % loc)
    e = s.evaluate("(+ x y)", frames[0]["id"])
    check(e["success"] and e["body"]["result"] == "15", "raised: evaluate at the throw: %r" % e)
    info = s.request("exceptionInfo", {"threadId": 1})
    b = info.get("body", {})
    check(info["success"] and b.get("exceptionId") == "ExceptionInfo" and b.get("breakMode") == "always"
          and b.get("details", {}).get("message") == "bad" and "risky" in b.get("details", {}).get("stackTrace", ""),
          "raised: exceptionInfo: %r" % info)
    check(s.stdout_text() == "start\n", "raised: output before the stop: %r" % s.stdout_text())
    s.request("continue", {"threadId": 1})
    # The throw inside try/finally: one stop, not one more for the finally's rethrow.
    st = s.event("stopped")
    frames = s.top()
    loc = s.locals(frames[0]["id"])
    check(st["body"]["reason"] == "exception" and loc.get("x") == "1",
          "raised: the throw inside try/finally: %r %r" % (st["body"], loc))
    s.request("continue", {"threadId": 1})
    st = s.event("stopped")
    frames = s.top()
    check(st["body"]["reason"] == "exception" and "Divide by zero" in st["body"].get("description", "")
          and frames[0]["name"] == "safe-div" and frames[0]["line"] == line_of(EXC, "(/ a b)"),
          "raised: a protoCore error stops at the call: %r %r" % (st["body"], frames[0]))
    info = s.request("exceptionInfo", {"threadId": 1})
    check(info["body"]["exceptionId"] == "ArithmeticException", "raised: kernel exceptionInfo: %r" % info)
    s.request("continue", {"threadId": 1})
    nxt = s.stopped_or_end()
    check(nxt is None, "raised: no further stop: %r" % (nxt and nxt["body"]))
    code, rc = s.finish()
    check(code == 0, "raised: exit status: %r" % code)
    check(s.stdout_text() == "start\ncaught bad\nfin\nouter bad\ndiv Divide by zero\nend\n",
          "raised: the catches ran: %r" % s.stdout_text())


def session_exception_internal(prog):
    s = Session()
    s.start(prog, [], filters=["raised"])
    st = s.stopped_or_end()
    if not check(st is None, "internal control flow stops nowhere: %r" % (st and st["body"])):
        frames = s.top()
        print("  stopped at", frames[:1], s.request("exceptionInfo", {"threadId": 1}).get("body"))
        s.request("disconnect")
        return
    code, rc = s.finish()
    check(code == 0, "internal: exit %r, stderr %r" % (code, s.stderr_text()))
    check(s.stdout_text() == "10\n1225\n500\n123456789012345678901234567890\n6\n012\nok 1 {1 2} ab ()\n",
          "internal: output: %r" % s.stdout_text())


def session_exception_off(prog):
    s = Session()
    s.start(prog, [], filters=[])
    st = s.stopped_or_end()
    check(st is None, "no filters: no stop: %r" % (st and st["body"]))
    code, rc = s.finish()
    check(code == 0 and "end\n" in s.stdout_text(), "no filters: ends normally: %r" % code)


def session_exception_uncaught(prog):
    s = Session()
    s.start(prog, [], filters=["userUnhandled"])
    st = s.event("stopped")
    frames = s.top()
    check(st["body"]["reason"] == "exception", "uncaught: stopped: %r" % st["body"])
    check([f["name"] for f in frames] == ["risky", "wrong-class", "<top level>"]
          and frames[0]["line"] == line_of(UNCAUGHT, "(throw (ex-info"),
          "uncaught: the stack is intact: %r" % frames)
    loc = s.locals(frames[0]["id"])
    check(loc.get("x") == "2" and loc.get("y") == "4", "uncaught: locals: %r" % loc)
    info = s.request("exceptionInfo", {"threadId": 1})
    check(info["body"].get("breakMode") == "userUnhandled", "uncaught: breakMode: %r" % info)
    check(s.stdout_text() == "caught\ncaught in map\ndiv caught\n",
          "uncaught: the caught throws did not stop: %r" % s.stdout_text())
    s.request("continue", {"threadId": 1})
    nxt = s.stopped_or_end()
    check(nxt is None, "uncaught: no second stop: %r" % (nxt and nxt["body"]))
    code, rc = s.finish()
    check(code == 1 and "bad" in s.stderr_text(), "uncaught: the program fails: %r %r" % (code, s.stderr_text()))


def thread_names(s):
    r = s.request("threads")
    return {t["id"]: t["name"] for t in r["body"]["threads"]}


def is_event(m, name):
    return m.get("type") == "event" and m.get("event") == name


def session_threads(prog):
    s = Session()
    bp = line_of(THREADS, "(let [sq (* n n)]")
    s.start(prog, [bp])
    a = s.event("stopped")
    b = s.event("stopped")
    ta, tb = a["body"]["threadId"], b["body"]["threadId"]
    check(ta != tb and 1 not in (ta, tb) and a["body"]["allThreadsStopped"] is False,
          "threads: two stops on two future threads: %r %r" % (a["body"], b["body"]))
    # The main thread runs on while both futures are stopped.
    s.wait_output("main ticks 5\n")
    names = thread_names(s)
    expected = {"future@prog-threads.clj:%d" % line_of(THREADS, "(def f1"),
                "future@prog-threads.clj:%d" % line_of(THREADS, "(def f2")}
    check(names.get(1) == "main" and {names.get(ta), names.get(tb)} == expected,
          "threads: names: %r" % names)
    fa, fb = s.top(ta), s.top(tb)
    la, lb = s.locals(fa[0]["id"]), s.locals(fb[0]["id"])
    check(fa[0]["name"] == "work" and fb[0]["name"] == "work" and {la.get("n"), lb.get("n")} == {"3", "4"},
          "threads: each stack is its own: %r %r" % (la, lb))
    check(fa[0]["id"] != fb[0]["id"], "threads: frame ids are unique across threads")
    # Step one thread; the other stays where it is.
    s.request("next", {"threadId": ta})
    st = s.event("stopped")
    check(st["body"]["threadId"] == ta and st["body"]["reason"] == "step", "threads: step in A: %r" % st["body"])
    fa = s.top(ta)
    la = s.locals(fa[0]["id"])
    check(fa[0]["line"] == bp + 1 and la.get("sq") in ("9", "16"), "threads: A stepped: %r %r" % (fa[0], la))
    fb2 = s.top(tb)
    lb2 = s.locals(fb2[0]["id"])
    check(fb2[0]["line"] == bp and lb2 == lb, "threads: B still stopped where it was: %r %r" % (fb2[0], lb2))
    e = s.evaluate("(* n 10)", fb2[0]["id"])
    check(e["success"] and e["body"]["result"] == str(int(lb.get("n", "0")) * 10), "threads: evaluate in B: %r" % e)
    r = s.request("continue", {"threadId": ta})
    check(r["body"].get("allThreadsContinued") is False, "threads: continue is per thread: %r" % r)
    nxt = s.wait(lambda m: is_event(m, "terminated") or is_event(m, "stopped") or
                 (is_event(m, "thread") and m["body"]["threadId"] == ta and m["body"]["reason"] == "exited"))
    check(nxt["event"] == "thread", "threads: A ran to its end alone: %r" % nxt)
    s.request("continue", {"threadId": tb})
    code, rc = s.finish()
    check(code == 0 and s.stdout_text() == "main ticks 5\nresults 10 17\n",
          "threads: program end: %r %r" % (code, s.stdout_text()))


def session_pause_thread(prog):
    s = Session()
    s.start(prog, [])
    started = s.wait(lambda m: is_event(m, "thread") and m["body"]["reason"] == "started"
                     and m["body"]["threadId"] != 1)
    tid = started["body"]["threadId"]
    time.sleep(0.3)
    s.request("pause", {"threadId": tid})
    st = s.event("stopped")
    check(st["body"]["reason"] == "pause" and st["body"]["threadId"] == tid, "pause: the future stops: %r" % st["body"])
    frames = s.top(tid)
    loc = s.locals(frames[0]["id"])
    check("n" in loc and frames[0]["line"] == line_of(SPIN, "(def spinner"), "pause: in the loop: %r %r" % (frames[0], loc))
    e = s.evaluate("(reset! stop true)", frames[0]["id"])
    check(e["success"], "pause: evaluate in the paused thread: %r" % e)
    s.request("continue", {"threadId": tid})
    code, rc = s.finish()
    check(code == 0 and s.stdout_text() == "spun true\n", "pause: end: %r %r" % (code, s.stdout_text()))


def session_gc_while_stopped(prog):
    s = Session(timeout=240, env={"PROTOCLJ_GC_STATS": "1"})
    bp = line_of(GC, '(println "holding"')
    s.start(prog, [bp])
    s.event("stopped")
    frames = s.top()
    before = s.locals(frames[0]["id"])
    e = s.evaluate("(reset! go true)", frames[0]["id"])
    check(e["success"], "gc: release the worker: %r" % e)
    # The worker allocates through several collections while main is stopped.
    s.wait_output("churned 20\n", timeout=180)
    frames2 = s.top()
    after = s.locals(frames2[0]["id"])
    check(before == after and after.get("m") == "{:a 1, :b [1 2 3]}" and after.get("v") == "[0 1 2 3 4]"
          and after.get("k") == ":kk", "gc: the stopped frame's locals: %r %r" % (before, after))
    e = s.evaluate("(conj v (count m))", frames2[0]["id"])
    check(e["success"] and e["body"]["result"] == "[0 1 2 3 4 2]", "gc: evaluate after: %r" % e)
    s.request("continue", {"threadId": 1})
    code, rc = s.finish()
    check(code == 0 and s.stdout_text().endswith("holding 2 5 :kk\ngc done\n"),
          "gc: end: %r %r" % (code, s.stdout_text()))
    cycles = 0
    for line in s.stderr_text().splitlines():
        if line.startswith("protoclj gc: cycles="):
            cycles = int(line.split("cycles=")[1].split()[0])
    check(cycles >= 2, "gc: collections ran: %r" % s.stderr_text()[-300:])


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
        files = {}
        for name, text in [("exc", EXC), ("internal", INTERNAL), ("uncaught", UNCAUGHT),
                           ("threads", THREADS), ("spin", SPIN), ("gc", GC)]:
            path = os.path.join(work, "prog-%s.clj" % name)
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            files[name] = vscode_path(path)
        only = os.environ.get("DAP_SESSION_ONLY")
        for name, fn, arg in [("breakpoint and steps", session_breakpoint_and_steps, prog),
                              ("stop on entry", session_stop_on_entry, prog),
                              ("breakpoint in a loop", session_loop, prog),
                              ("moved breakpoint", session_moved_breakpoint, prog),
                              ("uncaught exception", session_throws, throws),
                              ("exceptions raised", session_exception_raised, files["exc"]),
                              ("exceptions internal", session_exception_internal, files["internal"]),
                              ("exceptions off", session_exception_off, files["exc"]),
                              ("exceptions uncaught", session_exception_uncaught, files["uncaught"]),
                              ("threads", session_threads, files["threads"]),
                              ("pause a thread", session_pause_thread, files["spin"]),
                              ("gc while stopped", session_gc_while_stopped, files["gc"])]:
            if only and only not in name:
                continue
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
