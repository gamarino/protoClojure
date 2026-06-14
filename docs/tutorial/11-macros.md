# 11. Macros

A macro is a function that takes code as input and returns code as
output, at compile time. The compiler calls it, takes whatever it
returns, and compiles *that* instead of the original form.

That sentence sounds dry. The implication is that you can extend the
language itself, *with the language itself*, without modifying the
compiler. Every control-flow form Clojure has — `when`, `if-let`,
`cond`, `case`, `->`, `->>`, `for`, `doseq`, `loop` — is a macro on
top of the small set of special forms. The language as you use it is
*mostly your code*.

This is the seed Lisp planted in 1958 and most other languages have
not picked up since. It is also the part of Clojure that takes the
most time to internalise, because the levels of evaluation are
genuinely tricky. We will take it slow.

## 11.1 The intuition: the reader gives you a list

Recall from Chapter 2:

```clojure
'(+ 1 2)              ;; => (+ 1 2)   — a list of three elements
```

The reader took the text `(+ 1 2)` and produced a *data structure*: a
list whose elements are the symbol `+`, the number `1`, the number
`2`. The evaluator then walked that data structure and treated it as a
call.

The quote `'` says "stop, give me the data structure, do not
evaluate". A macro is a function the compiler calls *before
evaluating*, on the unevaluated data structure, and uses the return
value as the new data structure to compile. From the compiler's view,
the macro rewrites the code in place.

## 11.2 The smallest possible macro

A trivial example. JVM Clojure has `(when test body...)`, which is
sugar for `(if test (do body...) nil)`. Watch:

```clojure
(defmacro my-when [test & body]
  (list 'if test (cons 'do body)))

;; Use:
(my-when (= 1 1)
  (println "yes")
  (println "yes again"))

;; What the compiler sees after expansion:
(if (= 1 1)
  (do (println "yes")
      (println "yes again")))
```

What happened:

- `my-when` was called *at compile time* with two arguments: the form
  `(= 1 1)` (unevaluated, as data) and a seq `((println "yes")
  (println "yes again"))` (the rest of the args, also as data).
- It built and returned `(if (= 1 1) (do (println "yes") (println
  "yes again")))` as data.
- The compiler discarded the original `my-when` form and compiled
  the returned form instead.

The user wrote `my-when`; the compiler executed `if`. The
*surface* of the language gained a new control form without
changing the *core*.

You can inspect what a macro expands to with `macroexpand-1`:

```clojure
(macroexpand-1 '(my-when (= 1 1) (println "hi")))
;; => (if (= 1 1) (do (println "hi")))
```

`macroexpand-1` expands one level. `macroexpand` keeps expanding
until the form is no longer a macro call.

## 11.3 Quasiquote — the macro syntax

Building lists with `list`, `cons`, `concat` is verbose. Quasiquote
gives you a template syntax that looks like the output, with holes
where the input goes.

```clojure
(defmacro my-when [test & body]
  `(if ~test
     (do ~@body)))
```

The pieces:

- `` ` `` — quasiquote. Reads the form, mostly literally, but
  resolves symbols against the current namespace (so `if` becomes
  `clojure.core/if`).
- `~` — unquote. Splices a single value into the template.
- `~@` — unquote-splicing. Splices a sequence's elements into the
  template (so the items of `body` become siblings, not a nested
  list).

The same `my-when` reads as "an `if` of `~test`, followed by a `do`
of the body items". That is the shape the output has.

Quasiquote is the cleanest macro-writing tool. Reach for `list`,
`cons`, `concat` only when the template gets too dynamic to
express.

### 11.3.1 The auto-resolved symbols

`` `if `` reads as `clojure.core/if`. That namespace resolution is
intentional: it means the expanded code refers to the `if` the
*macro author* intended, not whatever the *caller's* namespace
happens to bind `if` to.

The same protection applies to user names:

```clojure
(defmacro doublet [body]
  `(do ~body ~body))

(macroexpand-1 '(doublet (println "hi")))
;; => (do (println "hi") (println "hi"))
```

`do` came out as `clojure.core/do`, even if the caller has shadowed
`do` somehow. Macros are hygienic *for known names*.

### 11.3.2 The gensym for fresh symbols

What if you need a fresh name inside the expansion that does *not*
collide with anything the caller defined?

```clojure
;; A bad once: introduces a name `x` that might shadow the caller's x
(defmacro bad-once [expr]
  `(let [x ~expr] (* x x)))

(let [x 10]
  (bad-once (+ 1 2)))
;; expands to (let [x (+ 1 2)] (* x x))
;; the outer x=10 is shadowed; the user might not have wanted that
```

Solution: gensym. The `name#` syntax inside a quasiquote *generates a
fresh symbol* on every expansion:

```clojure
(defmacro square-of [expr]
  `(let [x# ~expr]
     (* x# x#)))

(macroexpand-1 '(square-of (+ 1 2)))
;; => (let [x__123__auto__ (+ 1 2)] (* x__123__auto__ x__123__auto__))
```

Every reference to `x#` inside the same backtick form resolves to
the *same* gensym. Different `name#` instances generate different
symbols. This is the bread-and-butter macro hygiene technique.

## 11.4 Why macros are useful — three concrete cases

### 11.4.1 Wrapping code with setup / teardown

The `with-open` macro ensures a resource is closed even if the body
throws:

```clojure
(defmacro with-open [bindings & body]
  `(let ~bindings
     (try
       ~@body
       (finally
         (.close ~(bindings 0))))))

(with-open [file (open-file "data.txt")]
  (process file))
;; Expands to (let [file (open-file "data.txt")]
;;             (try (process file)
;;                  (finally (.close file))))
```

Without macros, this would have to be a higher-order function
`(with-open* file-fn body-fn)` and the caller would have to wrap the
body in `(fn [file] ...)` — losing local bindings and adding
boilerplate. Macros let you express the *shape* you want directly.

### 11.4.2 Compile-time domain-specific languages

Imagine a routing DSL for a web framework:

```clojure
(defroutes app
  (GET    "/users/:id"    [id]       (show-user id))
  (POST   "/users"        []         (create-user))
  (DELETE "/users/:id"    [id]       (delete-user id)))
```

This is *not* function calls. `GET`, `POST`, `DELETE` are not
functions — they are macros that expand into route registrations.
The whole `defroutes` form is a macro that walks its children and
builds a routing table at compile time.

You will see this pattern in every Clojure web framework, every
testing library, every database wrapper. It is a major reason
Clojure libraries can have such concise, declarative-looking APIs.

### 11.4.3 Threading and pipelines

The threading macros (`->`, `->>`, `as->`) we have been using are
themselves macros:

```clojure
(macroexpand '(->> (range 10)
                   (filter even?)
                   (map #(* % %))
                   (reduce +)))
;; => (reduce + (map (fn* [%] (* % %)) (filter even? (range 10))))
```

`->>` rewrites the chained form into nested calls. The user-facing
pipeline syntax is a pure macro convenience over a plain composition
of function calls.

## 11.5 The macros vs. functions decision

Most logic should be functions. Use macros only when:

- You need to control *which* expressions get evaluated and when.
  (`when` and `or` short-circuit. A function cannot.)
- You need to introduce new binding scopes for the caller's
  expressions. (`let`, `for`, `doseq` bind names.)
- You are building a compile-time DSL whose shape needs to differ
  from function-call shape.
- Performance: you want to inline a computation that would otherwise
  be a function call. (Rarely worth it.)

If any of the above is *not* the case, write a function. Functions
are composable, debuggable, testable, REPL-friendly, and don't trip
you up on the levels of evaluation. Macros are powerful and
correspondingly costly.

## 11.6 The protoClojure macro bootstrap

A point worth mentioning for the curious. Most of `clojure.core` is
itself macros (`when`, `if-let`, `cond`, etc.). For protoClojure to
support user macros, the compiler must be able to *evaluate
protoClojure code at compile time* — to call the macro and use its
output. The compiler is itself a host program (C++); the macros run
on the protoClojure side; the bridge is the same evaluator the user
sees at runtime.

The bootstrap order is delicate:

1. **v0.1-a:** the compiler is C++ only. It supports the irreducible
   special forms (`if`, `let*`, `loop*`, `recur`, `fn*`, `quote`,
   `def`, `try`, `throw`) and a small set of C++ built-in macros
   (`let`, `loop`, `fn`, `defn`). The bare minimum.
2. **v0.1-b:** `clojure.core` macros (`when`, `if-let`, `cond`,
   `->`, `->>`, `case`, `for`, `doseq`, …) are defined in
   protoClojure itself and loaded at startup. The compiler calls
   back into the evaluator when it sees them.
3. **v0.2+:** user macros work — they go through the same path as
   the core macros.

This is invisible to the user; you write `(when test body)` from day
one without caring about which side it lives on. But it is one of
the trickier parts of the implementation, and we will iterate on
it. See [`DESIGN.md`](../DESIGN.md) §10 for the protoClojure-specific
GC discipline during macro expansion.

## 11.7 Where the rabbit hole goes

Topics not covered in this chapter but worth following up on once you
are comfortable with the basics:

- **`syntax-quote` vs `quote`** — subtleties of when a macro author
  wants the symbol resolution and when they want a literal.
- **Walking forms** — `clojure.walk` (v0.2) is the standard
  toolkit for traversing and rewriting code structures.
- **Reader macros** — `defreader` lets you extend the *reader*, not
  the compiler, at the cost of an extra hook. Niche, advanced. Not
  v0.1.
- **`gen-class`-style code generation** — JVM Clojure can generate
  classes at compile time. We do not have an analogue (no JVM
  bytecode); protocols + multimethods cover most of the use cases.

A small Clojure library with one beautiful macro will teach you
more than reading about macros for a week. Look at how
`clojure.test` defines `deftest` and `is`; look at how
`clojure.core.match` builds a pattern compiler. Read,
`macroexpand`, change, re-expand. The mechanics become
second-nature surprisingly fast.

Next: [Chapter 12 — A worked example](12-worked-example.md), where
we walk through the tri-runtime demo one line at a time and tie
every concept from the tutorial to a concrete use.
