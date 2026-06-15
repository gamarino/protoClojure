# 10. The REPL

The REPL — the read-eval-print loop — is the Clojure workflow. Every
serious Clojure programmer drives development through it: write a
form in the editor, send it to a running REPL, see the result, fix
the bug, re-send, repeat. The application does not restart. The
program runs continuously while you change its parts.

This chapter is about how to do that with protoClojure. It is
divided into three sections: the local REPL you launch from a
terminal, the nREPL server you connect editors to, and the workflow
you actually use day to day.

## 10.1 The local REPL

Launch `protoclj` with no arguments:

```
$ protoclj
protoClojure 0.0.1
REPL — :help for commands, :quit or Ctrl-D to exit
user=>
```

Type a form, press enter, see the result:

```
user=> (+ 1 2)
3
user=> (defn square [n] (* n n))
#<fn>
user=> (square 7)
49
user=> (map square [1 2 3 4 5])
(1 4 9 16 25)
user=> (def acc (atom 0))
#<atom 0>
user=> (swap! acc inc)
1
user=> @(future (* 100 100))
10000
user=>
```

A few things to know up front:

- **Multi-line forms work.** Open a `(`, `[`, or `{` and keep typing —
  the prompt changes to `  #_=>` and the REPL keeps reading until the
  delimiters balance. A blank line at the continuation prompt forces
  evaluation (useful escape hatch if you got the count wrong).
- **History.** Arrow keys recall earlier lines. The history is
  persisted across sessions in `~/.protoclj_history`.
- **Error recovery.** A parse / compile / runtime error is printed and
  the loop continues at the next prompt. The REPL does not crash on
  bad input.
- **Definitions persist.** Every `def` / `defn` lands in the same
  globals namespace, so the next form sees everything you have defined
  so far.

Two things that distinguish this from Python's or Node's REPL:

- **Every definition you make stays.** The next form sees the
  function you just defined. The next form *after that* sees both,
  and so on. The REPL is the live program.
- **You can redefine anything at any time.** `(defn square [x] (* x x
  x))` quietly replaces the cube definition; the next caller picks
  it up. No restart.

### 10.1.1 REPL helpers

The three special vars `*1`, `*2`, `*3` hold the last three results.
Useful for reaching back without retyping the form that produced
them:

```
user=> (* 7 6)
42
user=> *1
42
user=> (+ *1 100)
142
user=> *2
42
```

`(doc fn)` / `(source fn)` / `(find-doc "regex")` are tutorial-only
today — they are planned for v0.2 alongside docstring storage on
`defn`. `*e` (last exception) is also v0.2.

### 10.1.2 REPL meta-commands

Commands beginning with `:` are REPL-only — they are not legal in
`.clj` files. What ships today:

| Command          | What it does                                            |
|------------------|---------------------------------------------------------|
| `:help`, `:h`    | Show the in-REPL command list                           |
| `:quit`, `:q`    | Leave the REPL (Ctrl-D works too)                       |
| `:load <path>`   | Read and evaluate a `.clj` file in this session         |
| `:time <expr>`   | Evaluate `<expr>`, report wall-clock time alongside     |

`:reload`, `:cd`, `:pwd`, `:cp`, `:in-ns`, `:set` are v0.2 territory:
they depend on the namespace machinery (`:in-ns`, `:reload`) and a
classpath search abstraction (`:cp`) that are not in v0.1.

The prompt is plain `user=>` — once protoClojure gets real
namespaces, it will switch by namespace the way JVM Clojure does
(`my.app=>`).

### 10.1.3 What gets printed

The REPL prints values with `pr-str`, which is the *readable* form
— strings have their quotes, keywords have their colon, nil prints
as `nil`. This is intentional: the printed form is round-trippable
through the reader. `(read-string (pr-str x))` returns the original
`x` for any printable value.

For human-readable output, `(println x)` uses `print-str` (no quotes,
no escapes). You will use `println` in code and let the REPL use
`pr-str` for the result.

For pretty-printing of nested data — long maps, deep vectors — wrap
in `(pprint x)`:

```
user=> (pprint people)
[{:name "alice", :age 30} {:name "bob", :age 25}]
```

(In v0.1, `pprint` is a minimal one-pass printer. The full
`clojure.pprint` lands in v0.2.)

## 10.2 nREPL — your editor connects to a running program

The nREPL server is the protocol Clojurists rely on to make their
editor talk to a running Clojure process. It is the foundation of
CIDER (Emacs), Calva (VS Code), Conjure (Vim/Neovim), and a dozen
smaller plugins.

Start the server:

```bash
$ protoclj --nrepl 7888
nREPL server started on port 7888
Connect with: cider-connect localhost 7888
```

The process keeps running. From your editor:

- **Emacs / CIDER**: `M-x cider-connect-clj`, host `localhost`, port
  `7888`.
- **VS Code / Calva**: command palette → "Calva: Connect to a
  Running REPL Server, not in your project" → `localhost:7888`.
- **Vim / Conjure**: `:ConjureConnect localhost 7888` (or whatever
  your config maps to).

Once connected, your editor sends every form to the running process.
Evaluate a `defn` and the var is updated in the live process. Run a
`(do-the-thing)` and you see the result inline. Trigger a function
that has a bug, fix it, re-evaluate the `defn`, trigger again — no
restart, no losing in-memory state.

### 10.2.1 The operations protoClojure v0.1 supports

The nREPL protocol is open-ended; servers implement a subset of
operations. v0.1 implements the minimum that CIDER / Calva / Conjure
need for the core workflow:

| Op            | What it does                                                       |
|---------------|--------------------------------------------------------------------|
| `eval`        | Evaluate a form, return the result and any output                  |
| `load-file`   | Evaluate a whole file (with line numbers preserved for errors)     |
| `interrupt`   | Stop a long-running evaluation                                     |
| `clone`       | Create a new session (own bindings, own history)                   |
| `close`       | Close a session                                                    |
| `describe`    | Tell the client which ops are supported                            |

The two stretch operations:

| Op            | What it does                                                       |
|---------------|--------------------------------------------------------------------|
| `info`        | Look up the docstring and source of a symbol                       |
| `complete`    | Return completion candidates for a partial symbol                  |

`info` and `complete` are what make autocomplete and "jump to
definition" work in CIDER. We are targeting them for v0.1; if the
v0.1 budget is tight, they slip to v0.2.

### 10.2.2 What does *not* work in v0.1

CIDER and friends expect more than the minimum on a mature server —
inline test results, debug stepping, code formatting, etc. These all
slip past v0.1. Connecting and evaluating works; the fancier
integrations come incrementally.

### 10.2.3 nREPL middleware

JVM Clojure nREPL has a rich middleware ecosystem (cider-nrepl,
refactor-nrepl, etc.). protoClojure does not run JVM middleware (they
are JVM-specific bytecode). v0.x will grow its own equivalents
written in protoClojure as needs arise; the protocol itself is the
language-independent piece, so the editor sees a working nREPL from
day one.

## 10.3 The workflow

What a real Clojure programmer's day looks like:

1. `protoclj --nrepl 7888` in one terminal. Leaves it running.
2. Open the project in the editor. Connect to `localhost:7888`.
3. Open `src/app/core.clj`. Evaluate the whole file (CIDER:
   `C-c C-k`; Calva: `Ctrl+Alt+C Enter`).
4. Now every var defined in `app.core` is in the live REPL session.
5. Write a new function in the file. Send it to the REPL with one
   key (CIDER: `C-c C-c` on the defn form; Calva: `Ctrl+Enter`).
6. In the REPL window, call it: `(my-new-function arg)`. See the
   result.
7. Notice a bug. Fix the code in the file. `C-c C-c` again. The var
   is updated.
8. Re-call. New result. Done.
9. The application running in the REPL — including any state it
   built up — is still there, with the fixed function now in place.

This is what people mean by "REPL-driven development". You are not
running a script that loads, executes, exits. You are tending to a
running program.

### 10.3.1 Tap and inspect

A small but useful pair:

```clojure
(tap> {:user some-user :step :pre-validate})
```

`tap>` sends a value to every registered tap. The REPL has a default
tap that prints it. Editors install their own that route the value
to an inspector panel. Useful as a print-like debug call that does
not require importing print at the call site.

`add-tap` registers a function; `remove-tap` unregisters it. Used
mostly to wire a debug panel into a running process.

## 10.4 The REPL across runtimes

A subtle benefit of the protoCore substrate: a protoClojure REPL can
hold references to objects from any other runtime, and the protoCore
GC keeps them alive across REPL turns.

You can, in one session:

```
user=> (require '[py/numpy :as np])
nil
user=> (def arr (np/array [[1 2] [3 4]]))
#'user/arr
user=> arr
;; prints a NumPy array
user=> (np/.shape arr)
#py (2 2)
user=> (np/sum arr)
10
user=> (py->clj arr)
[[1 2] [3 4]]
```

`arr` survives between REPL turns, holds the original NumPy array,
and is reachable as a Clojure var. The GC traces it through the var
binding. The whole interactive cross-language session works
because the data lives once, not once per language.

## 10.5 Why a respected REPL is a v0.1 requirement, not a stretch goal

Saying "we are missing nREPL but the local REPL works" sounds
reasonable until you try to convince a Clojurist. To a Clojure
programmer, "REPL" implicitly means "the editor talks to it" —
because for the past fifteen years that has been the way. A
language with a basic command-line REPL and no editor integration
is, in their experience, a toy.

That is why nREPL is in v0.1. We pay the implementation cost
(bencode parser, TCP server, session management, ~1000 lines of C++)
because the alternative is being dismissed as a toy by the audience
we are most trying to reach.

Next: [Chapter 11 — Macros](11-macros.md). The other thing that
makes Clojure Clojure, and the part where the substrate's
"code is data" mantra earns its keep.
