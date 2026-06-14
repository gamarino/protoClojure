# protoClojure

A Clojure dialect that runs on the protoCore object kernel, with **transparent
interop to Python and JavaScript modules** through the shared UMD module
system. No JVM. Real OS threads, no GIL. Persistent collections all the way
down — because protoCore is built that way already.

> **Status: pre-alpha, design phase.** This repository currently holds the
> language specification, design documents, and tutorial drafts. The
> interpreter is not yet implemented. The first runnable milestone (v0.1)
> targets the core forms (`def`, `fn`, `let`, `if`, `do`, `recur`), the four
> core data structures (vector, map, set, list), `atom`, and a working REPL
> on top of nREPL.

## Why this exists

The Clojure community is small but technically demanding, with a strong shared
respect for *immutability as the default*, *data as the substrate*, and
*REPL-driven development*. protoCore was built on the same three principles
without setting out to host Clojure — they're the kernel's design stance.
Putting them together is the natural step.

What protoClojure offers that the JVM Clojure does not:

- **Native interop with Python and JavaScript modules**, through the same
  UMD plumbing protoPython and protoJS already use. A `(:require [py/numpy
  :as np])` form pulls a NumPy module in and the result is a real protoCore
  object — no FFI marshalling, no copy at the boundary, no separate process.
- **Fast startup, small footprint.** No JVM warm-up: process start is
  milliseconds. Scripts and CLI tools become a viable form factor without
  reaching for GraalVM AOT.
- **Real parallelism without the GIL.** Every protoCore-hosted runtime
  shares the same GIL-free concurrency model. The `atom` primitive is a
  protoCore CAS, not a Clojure abstraction over a JVM primitive.

What protoClojure does **not** offer:

- **JVM interop.** No Java classes, no `clojure.java.io`, no `java.time`,
  no Maven, no Leiningen. The substitute, where applicable, is calling the
  Python or JavaScript ecosystem through UMD.
- **100% Clojure-JVM compatibility.** This is a *dialect*. The reader, the
  core forms, and the standard library try to feel like Clojure; details
  intentionally do not match where the JVM-specific design would not earn
  its keep on this substrate.
- **`core.async` channels (v0.1).** The protoCore actor / future primitives
  give a different concurrency model that is closer in spirit to the
  Hickey designs. A CSP layer is on the roadmap but not in v0.1.

## Reading the docs

The documentation is split for two audiences in parallel. Read the chapter
that matches your background; both end at the same place.

- **From Python or JavaScript.** Start with
  [`docs/tutorial/02-for-the-python-or-javascript-developer.md`](docs/tutorial/02-for-the-python-or-javascript-developer.md).
  You'll meet S-expressions, immutability-by-default, persistent collections,
  the REPL, functions as values, and macros, with each idea grounded in a
  Python or JS analogue you already know.
- **From Clojure.** Start with
  [`docs/tutorial/03-for-the-clojure-developer.md`](docs/tutorial/03-for-the-clojure-developer.md).
  It is an honest catalogue of every place protoClojure agrees with and
  departs from Clojure-JVM — what is identical, what is renamed, what is
  gone, and the rationale for each call.

The authoritative reference is [`docs/LANGUAGE.md`](docs/LANGUAGE.md). The
module system and cross-runtime story is in [`docs/INTEROP.md`](docs/INTEROP.md).
The roadmap and what is implemented today is in
[`docs/STATUS.md`](docs/STATUS.md) and [`docs/ROADMAP.md`](docs/ROADMAP.md).

## Project stance

Three explicit design priorities, in this order:

1. **Idiom over performance.** A Clojure programmer must read protoClojure
   code and recognise it as Clojure. `reduce`, `map`, `into`, `update`,
   `assoc`, `get-in`, threading macros — they have to feel right. We
   accept measurable performance cost to keep idiom intact, and document
   any deviation as a deliberate departure rather than an oversight.
2. **A respected REPL.** Clojure is a REPL-driven language. A
   protoClojure that lacks a REPL conversable with CIDER, Calva, or
   Conjure is, to a Clojure programmer, broken. nREPL compatibility is a
   v0.1 requirement, not a v0.x stretch goal. (The implementation cost is
   real, the strategic cost of skipping it is larger.)
3. **Honesty about what is missing.** Every gap from Clojure-JVM is
   documented up-front in [`docs/STATUS.md`](docs/STATUS.md). No silent
   stubs that return `nil`. Calling something not yet implemented raises a
   clear error pointing at the tracking issue.

## Repository layout

```
protoClojure/
├── README.md                       # this file
├── LICENSE                         # MIT
├── docs/
│   ├── LANGUAGE.md                 # authoritative language reference
│   ├── INTEROP.md                  # UMD module system, cross-runtime
│   ├── STATUS.md                   # living tracker of deviations + gaps
│   ├── ROADMAP.md                  # milestone plan
│   ├── DESIGN.md                   # architectural overview
│   ├── TUTORIAL.md                 # index to the tutorial chapters
│   ├── tutorial/                   # numbered chapters, dual-audience
│   └── superpowers/specs/          # dated design docs
├── examples/                       # runnable .clj samples (once interp lands)
├── src/                            # C++ interpreter (not yet present)
└── test/                           # GoogleTest (not yet present)
```

## Building

Not yet — no source code in the repository. The first runnable milestone
will produce a `protoclj` binary using the same CMake pattern as protoST and
protoPython.

## Contributing

This is a single-person project in its design phase. Issues and PRs are
welcome once v0.1 lands and the implementation is open. Until then, the way
to contribute is to read the documents and tell us where the design feels
wrong.
