# The protoClojure Tutorial

> **A dual-audience tutorial.** This document teaches protoClojure to
> two audiences at once. If you come from **Python or JavaScript**, the
> tutorial introduces Lisp and Clojure from first principles, with a
> constant bridge back to concepts you already know. If you come from
> **Clojure-JVM**, the tutorial is a precise, honest catalogue of
> where protoClojure agrees with and departs from the dialect you know.

The authoritative language reference is
[`docs/LANGUAGE.md`](LANGUAGE.md). The cross-runtime story is in
[`docs/INTEROP.md`](INTEROP.md). The live tracker of what works and what
deviates is [`docs/STATUS.md`](STATUS.md). The tutorial describes the
language protoClojure is converging on; not every snippet runs against
the current `protoclj` binary.

> **What runs today (protoClojure 0.0.1).** Functions, closures,
> multi-arity and variadic functions, lists, vectors, maps and strings
> (chapters 2, 4 and 5, in part); atoms, watches, futures and promises
> (chapter 6, in part); the local REPL (chapter 10, in part); and actors
> (chapter 13). Chapters 7, 8, 9, 11 and 12 describe planned features
> that are not implemented yet. Each chapter that uses unimplemented
> features opens with a note listing them; [`STATUS.md`](STATUS.md) is
> the authoritative list.

## How to use this tutorial

**If you are a Python or JavaScript developer.** Read every chapter in
order. The Lisp-to-Python/JS bridges are written for you; reading them
helps. Chapter 3 (for Clojure programmers) you can skim — it documents
deviations from a dialect you do not know yet.

**If you are a Clojure programmer.** Skim Chapter 2 (it teaches Clojure
from scratch and you know it). Read Chapter 3 carefully — that is the
honest list of differences from Clojure-JVM, which is what you actually
need from this tutorial. Then read Chapters 4 onward where they cover
features you may not have used: the cross-runtime interop, the persistence
of the underlying object model, the protoCore concurrency primitives.

## Chapters

| #  | Chapter                                                                       | What it covers                                                                                            |
| -- | ----------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------- |
| 1  | [Introduction](tutorial/01-introduction.md)                                   | What protoClojure is. How to run it. The cross-runtime angle in 60 seconds.                               |
| 2  | [For the Python or JS developer](tutorial/02-for-the-python-or-javascript-developer.md) | Lisp syntax. S-expressions. Immutability by default. Functions as values. The REPL.                       |
| 3  | [For the Clojure developer](tutorial/03-for-the-clojure-developer.md)         | What is identical to Clojure-JVM. What is renamed. What is missing. What is new.                          |
| 4  | [Data structures](tutorial/04-data-structures.md)                             | Lists, vectors, maps, sets. Persistence. Why `assoc` returns a new map.                                   |
| 5  | [Functions and closures](tutorial/05-functions-and-closures.md)               | `fn`, `defn`, variadic, multi-arity. Closures. `recur` for tail recursion.                                |
| 6  | [State and atoms](tutorial/06-state-and-atoms.md)                             | The atom. CAS. When to reach for one. The protoCore primitive underneath.                                 |
| 7  | [Protocols and multimethods](tutorial/07-protocols-and-multimethods.md)       | *Planned.* Polymorphism without classes. Why this is interesting on a prototype runtime.                  |
| 8  | [Modules](tutorial/08-modules.md)                                             | *Planned.* `(ns ... :require [...])`. The Clojure path. Aliases.                                          |
| 9  | [Cross-runtime interop](tutorial/09-interop.md)                               | *Planned.* `py/`, `js/`, `pst/` prefixes. When to convert. The conversion functions.                      |
| 10 | [The REPL](tutorial/10-repl.md)                                               | The interactive `protoclj` REPL. The planned nREPL server for CIDER / Calva / Conjure.                    |
| 11 | [Macros](tutorial/11-macros.md)                                               | *Planned.* Code as data. `defmacro`. Why this is the language's leverage point.                           |
| 12 | [A worked example](tutorial/12-worked-example.md)                             | *Planned.* The tri-runtime demo from `INTEROP.md`, walked through one line at a time.                     |
| 13 | [Actors](tutorial/13-actors.md)                                               | `actor` / `send` / `send-h` / `send-l` / `actor?` / `actor-stats`. Single-method invariant, priority bands, worker pool. |

## A note on running the examples

The `protoclj` binary runs `.clj` files and, when started without
arguments, an interactive REPL. The `-e` one-expression flag and the
nREPL server are planned (see [`ROADMAP.md`](ROADMAP.md)).

```bash
$ protoclj script.clj           # run a file
$ protoclj                      # interactive REPL
$ protoclj --version            # print the version
$ protoclj -e '(+ 1 2)'         # planned
$ protoclj --nrepl 7888         # planned (nREPL server)
```

The runnable examples shipped with the repository live under
[`examples/`](../examples/) and exercise the implemented surface end to
end.
