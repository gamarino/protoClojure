# 6. State and Atoms

State in Clojure is a deliberate, named tool — not the default. The
default is values: maps, vectors, sets, numbers, strings, all
immutable and persistent. When you genuinely need a value that can
*change over time* — a counter, a cache, a current-user reference, a
configuration that can be reloaded — you reach for an `atom`.

This chapter is the smallest in the tutorial because the concept is
small. An atom is one named cell holding one value, swappable
atomically. That is it.

## 6.1 The basic moves

```clojure
(def counter (atom 0))    ;; create an atom holding 0

@counter                  ;; => 0    — deref reads the current value
(deref counter)           ;; => 0    — long form

(swap! counter inc)       ;; => 1    — apply function, return new value
(swap! counter inc)       ;; => 2
(swap! counter + 10)      ;; => 12   — extra args go after the function
@counter                  ;; => 12

(reset! counter 100)      ;; => 100  — set unconditionally
@counter                  ;; => 100
```

The `!` at the end of `swap!` / `reset!` is the Clojure convention for
"this function mutates something". It does not mean anything
special to the runtime; it is a *naming convention* to flag mutation
sites at a glance.

## 6.2 `swap!` is atomic, and that means it can retry

`swap!` is built on a compare-and-swap (CAS) primitive — protoCore's
`setAttributeIfEqual`. The semantics:

1. Read the current value.
2. Compute `(f current-value & args)`.
3. CAS: install the new value only if the current value still matches
   what we read.
4. If the CAS fails (another thread got there first), go back to step 1.

That retry has a consequence the language hides but you should know:
**the function passed to `swap!` must be pure**. It may be called more
than once with the same input if there is contention. If it has side
effects, those side effects fire on every retry, not just the winning
one.

```clojure
;; Wrong — side effect retried on contention
(swap! counter (fn [n]
                 (println "computing...")     ;; printed multiple times!
                 (inc n)))

;; Right — pure inside, side effect outside
(let [new-value (swap! counter inc)]
  (println "now at" new-value))
```

If you genuinely need a side effect under contention, use a lock or
serialise through a single agent (v0.3) or actor.

## 6.3 `compare-and-set!` — the explicit CAS

When you need to *attempt* an update and know whether it succeeded:

```clojure
(def x (atom 10))

(compare-and-set! x 10 20)   ;; => true,  x is now 20
(compare-and-set! x 10 30)   ;; => false, x is still 20
```

You almost never reach for this directly; `swap!` covers the common
case. It is there for protocol-shaped retry loops you build yourself.

## 6.4 Watches — react to changes

`add-watch` registers a callback that fires after every successful
update:

```clojure
(def counter (atom 0))

(add-watch counter :logger
  (fn [key ref old-state new-state]
    (println key ":" old-state "->" new-state)))

(swap! counter inc)   ;; prints ":logger : 0 -> 1"   then returns 1
(swap! counter inc)   ;; prints ":logger : 1 -> 2"

(remove-watch counter :logger)
```

Watches are convenient for tracing, logging, and notification. They
fire *synchronously* on the thread that did the update, so a slow
watch slows the update — keep watches fast or push the work to a
queue.

## 6.5 An atom holding a map — the common pattern

You will see this constantly:

```clojure
(def state
  (atom {:users    {}
         :sessions {}
         :version  1}))

(defn login! [username]
  (swap! state update-in [:users username] (fnil inc 0)))

(defn current-version []
  (:version @state))
```

The whole application state is one atom holding one map. Mutations
go through `swap!`, which preserves atomicity over the whole map
update. Reads dereference and walk the snapshot.

This is the "one big atom" pattern that Reagent, Re-frame, Datomic, and
most idiomatic Clojure programs use. It works because:

- `update-in` is `O(log n)` on persistent maps — sharing structure
  with the previous version, not copying.
- `swap!` is atomic over the entire transformation, so a `swap!` that
  reads `:version` and modifies `:users` is observably one step.
- Reads on `@state` get a *consistent snapshot* — no torn reads, no
  partial mutations visible.

The `(fnil f default)` combinator above turns `inc` into "increment,
treating nil as the default" — handy because the first time a user
logs in, `(get-in m [:users "alice"])` is `nil`.

## 6.6 When `atom` is not enough

Atoms are the right tool when:

- One ref, one value.
- Updates are independent — you can compute the new value from the old
  one without coordinating with another atom.
- Retries are cheap (the function is pure and small).

Atoms are *not* enough when:

- You need to update **two refs together atomically** — neither sees a
  partial state. That is what STM and refs are for. Not in v0.1; see
  [`STATUS.md`](../STATUS.md) D6 (v0.2 deliverable).
- You need **serialised, asynchronous, side-effecting updates** — the
  agent pattern. Not in v0.1; D7 (v0.3).
- You are coordinating between threads with a *message-passing* shape
  rather than a *shared-state* shape — actors. The protoCore actor
  primitive is available; protoClojure exposes it from v0.3.

In v0.1, your toolkit for state is: `atom`, `volatile!` (single-thread,
no CAS, used inside transducer-shaped code), `promise` /  `deliver`
(one-shot rendezvous), `delay` (one-shot deferred computation), and
foreign-language refs via UMD interop.

## 6.7 `volatile!`, `promise`, `delay` — the minor tools

**`volatile!`** is a single-threaded mutable cell with no CAS.
Cheaper than `atom` because there is no retry. Use inside an inner
loop where you have externally guaranteed single-thread access:

```clojure
(let [counter (volatile! 0)]
  (dotimes [_ 10]
    (vswap! counter inc))
  @counter)                   ;; => 10
```

`vswap!`, `vreset!` are the volatile equivalents of `swap!` / `reset!`.
Used heavily in transducer internals (v0.2).

**`promise`** is a one-shot rendezvous. Created empty. The first
`deliver!` sets its value. Subsequent `@promise` reads the value
(blocking until delivered):

```clojure
(def result (promise))

(future                            ;; future runs on another thread
  (Thread/sleep 100)
  (deliver result 42))

@result                            ;; blocks until deliver, then => 42
```

(`future` is the protoCore async primitive, surfaced under the same
name JVM Clojure uses; see Chapter 10 for the concurrency model.)

**`delay`** wraps a computation that runs at most once, the first time
its value is needed:

```clojure
(def expensive (delay (do (println "computing...") 42)))

@expensive    ;; prints "computing..." then => 42
@expensive    ;; => 42  — no recomputation
```

Useful for lazy initialisation: the work happens only if and when
the value is needed.

## 6.8 The substrate underneath

A protoClojure `atom` is a mutable protoCore object with two
attributes:

- `__value__`  — the current held value.
- `__watches__` — a map from key to callback, optional.

`swap!` is a CAS loop using `setAttributeIfEqual(__value__, expected,
new-value)`. The protoST `Atom` class exposes the same primitive at
the Smalltalk surface; we reuse the same kernel call from Clojure.

This matters in one observable way: a Clojure atom and a Smalltalk
`Atom` instance are **interchangeable across the UMD interop
boundary**. A protoST program can deref a Clojure atom and read its
value; a protoClojure program can `deref` a protoST `Atom`. Both go
through the same `getAttribute(__value__)` path.

That symmetry — same primitive, four languages, one wire format — is
the substrate paying off again.

## 6.9 Idiomatic atom checklist

A short checklist for code review on your own atoms:

- [ ] Is the `swap!` function pure?
- [ ] Is the atom genuinely shared, or am I making a local variable
  that should have been a `let`?
- [ ] Is the value an immutable data structure (map, vector, set,
  primitive)?
- [ ] If two atoms must agree on a value, can they be merged into one
  atom holding a map?
- [ ] If the answer above is "no, they really are independent and
  must coordinate", I want refs/STM — postpone or use the actor
  primitive.

Next: [Chapter 7 — Protocols and multimethods](07-protocols-and-multimethods.md).
