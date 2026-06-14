# 1. Introduction

protoClojure is a Clojure dialect that runs on a different substrate
than the JVM. It is built on the **protoCore object kernel** — a small
C++20 runtime that is also the foundation for a Python interpreter
(protoPython), a JavaScript interpreter (protoJS), and a Smalltalk
interpreter (protoST). All four share the same in-memory object
model. That sharing is the point.

## In 60 seconds: why this exists

Clojure was designed around four principles: **immutability as the
default**, **data as the substrate**, **functions over methods**, and
**REPL-driven development**. The JVM is an excellent host for those
ideas — but you pay JVM start-up time, JVM RAM, and the JVM ecosystem
gravity. The Clojure community has been chipping at this for years:
Babashka cuts startup by AOT-compiling with GraalVM; ClojureScript
runs on V8; Clojerl runs on BEAM. Each is a different compromise.

protoClojure is another compromise. It gives up Java interop entirely.
In return:

- **Startup is in milliseconds.** No JVM warm-up.
- **OS threads with no GIL.** Real parallelism, with the protoCore
  concurrent garbage collector and per-thread allocation arenas.
- **Persistent collections are not a layer — they are the kernel.**
  The protoCore object model is structurally shared and immutable by
  default. Clojure does not have to convince it.
- **You can call Python and JavaScript modules directly.** A
  `(:require [py/numpy :as np])` brings NumPy in through the same
  module resolver the Python runtime uses. The result is a protoCore
  object you can call from Clojure. No FFI marshalling, no separate
  process.

The unique trade is **the JVM ecosystem for the Python and JavaScript
ecosystems, on a runtime that natively understands Clojure idioms**.
If you want Maven and Java libraries, this is not for you. If you
want Clojure's data discipline with NumPy or D3 reachable from
inside it, this might be.

## What is in the box (eventually)

The v0.1 target:

- The Clojure reader (S-expressions, vectors, maps, sets, reader
  macros).
- The core evaluator: `def`, `if`, `let`, `fn`, `loop`, `recur`,
  `quote`, `try`/`catch`.
- The four persistent collections: list, vector, map, set.
- Lazy sequences.
- The minimum `clojure.core`: ~80 functions covering arithmetic,
  collections, predicates, higher-order, threading, string basics.
- Atoms (one-line on top of protoCore's CAS primitive).
- Protocols and multimethods.
- The hybrid module system: `(:require [...])` with `py/`, `js/`,
  `pst/` prefixes for cross-runtime imports.
- A working REPL.
- An nREPL server you can connect CIDER / Calva / Conjure to.

What is **not** in v0.1: STM (refs), agents, transducers, chunked
seqs, `defrecord`, `core.async`, `clojure.spec`, BigDecimal `M`
literals, anything in `clojure.java.*`. The
[ROADMAP](../ROADMAP.md) lays out which are postponed and which are
permanent omissions.

## How to read this tutorial

The next two chapters split the audience:

- **Coming from Python or JavaScript?** Read [Chapter 2](02-for-the-python-or-javascript-developer.md)
  to learn Clojure from scratch, with every concept anchored to a
  Python or JS analogue.
- **Coming from Clojure-JVM?** Read [Chapter 3](03-for-the-clojure-developer.md)
  to find out exactly what protoClojure agrees with and where it
  departs.

Both audiences converge from Chapter 4 onward.

## A running example

This is the script the tutorial builds toward — a 30-line
demonstration that uses Python's pandas to load a CSV, processes the
data functionally in Clojure, and renders a chart with JavaScript's
D3:

```clojure
(ns demo.tri-runtime
  (:require [py/pandas :as pd]
            [js/d3 :as d3]
            [clojure.string :as str]))

(defn analyse [csv-path]
  (let [df    (pd/read_csv csv-path)
        cols  (py->clj (pd/.columns df))]
    (->> cols
         (filter #(str/starts-with? % "metric_"))
         (map (fn [col]
                (let [values (py->clj (pd/.loc df :all col))]
                  {:col col
                   :sum (reduce + values)
                   :avg (/ (reduce + values) (count values))}))))))

(defn -main [csv-path out-path]
  (let [data (clj->js (analyse csv-path) :deep true)]
    (d3/renderBarChart data out-path)
    (println "Wrote chart to" out-path)))
```

Every chapter in this tutorial unpacks one piece of that example.
By Chapter 12 you should be able to read this and write a variant.
