# protoClojure adoption of the protoCore call convention

**Date.** 2026-06-14.
**Status.** Adopted by construction (sessions 5–6). This note records
*why* the convention is the right shape for protoClojure, *what*
already conforms, and *what* still has to be filled in for the
convention's named-argument half.

**Companion specs.**
- protoST's surface-syntax version: `protoST/docs/superpowers/specs/2026-06-13-protocore-call-syntax.md`.
- protoClojure foreign-dispatch (collection protocols layer):
  `protoClojure/docs/superpowers/specs/2026-06-14-foreign-dispatch.md`.

## 1. What "the protoCore call convention" actually says

The convention is a kernel-level shape every runtime in the
protoCore family agrees on:

> A method is reached by **bare attribute name** on the receiver, and
> the runtime hands it a **positional argument vector** plus a
> **named-argument dictionary**.

The convention is a kernel rule because the kernel — and only the
kernel — is what makes a *foreign* method reachable in the same
move as a local one. protoPython, protoJS, protoST, and protoClojure
all dispatch through the same `getAttribute` chain walk; the
convention is the agreement that the *thing on the other side of
that walk* knows how to receive `(positional, named)`.

## 2. What protoClojure already conforms to

The bare-name half lands automatically:

- `defn foo` registers `foo` in the current namespace as a plain
  attribute on the namespace object (a protoCore object). Foreign
  consumers reach it with `ns.foo`, exactly like any other
  protoCore method.
- `fn` produces a callable wrapper (`fnMarkerProto` child); the
  wrapper is itself a protoCore object and dispatches through the
  ordinary `CALL` opcode.

The positional half also lands in session 5:

- `(defn foo [a b c] body)` accepts three positional arguments,
  bound to slots 0..2 of the call frame.

So `(foo 1 2 3)` from protoClojure and `recv foo(1, 2, 3)` from
protoST and `obj.foo(1, 2, 3)` from protoPython all hit the same
attribute on the same protoCore object and run the same bytecode.

## 3. What is *not* yet there (and how it lands)

Named arguments. Clojure's idiom for "named arguments" is map
destructuring:

```clojure
(defn area [w h & {:keys [unit] :or {unit :meters}}]
  ...)
(area 3 4 :unit :feet)
```

The surface syntax is Clojure-native; the **bytecode layer** is the
same shape protoST uses (session 5 conformance fixtures already
cover positional; the named half lands when map literals + keyword
literals reach v0.x). When that lands:

- The compiler sorts named keys alphabetically.
- The call site pushes positional values in source order, then
  named values in alphabetical-key order.
- A new opcode (or an overload of `CALL`) carries `(nPos, nNamed,
  sortedNamedKeys)` — same descriptor protoST uses, sharing the
  mangled-symbol encoding.
- The dispatcher does the same chain walk → bytecode method →
  build-frame work. A missing named key triggers the
  `:or`-defaulted expression in the method's prologue, again
  mirroring protoST's unset-sentinel pattern.

Deferred to v0.2+, not v0.1.

## 4. Why this matters — the cross-runtime promise

The convention is what lets:

```clojure
(:require [py/numpy :as np])

(let [arr (np/zeros [3 3] :dtype :float64)]
  ...)
```

route the named arg `:dtype` to NumPy's `dtype=` keyword without
any per-call adapter. The Clojure compiler emits the standard
"positional + named" descriptor; protoPython's UMD bridge receives
positional + named and dispatches into NumPy's call protocol; the
user pays nothing for the cross-runtime hop beyond the chain walk
documented in
`docs/superpowers/specs/2026-06-14-foreign-dispatch.md` §9.

The same code path runs in the other direction: a protoST caller
invoking a protoClojure `defn` reaches the bytecode through the
exact opcode flow protoST uses for its own call-form methods.

## 5. The two halves of "foreign dispatch"

This is the second-of-two specs that together define how
protoClojure participates in the protoCore ecosystem:

| Spec | Layer | What it covers |
|------|-------|----------------|
| `foreign-dispatch.md` | Collection protocols | `(count py-list)` works — protocols extended to foreign types |
| `protocore-call-convention.md` (this) | Call shape | `(np/zeros [3 3] :dtype :float64)` works — bare name + positional + named |

The first is the user-visible polymorphism over collection types;
the second is the kernel-visible shape of a single method call.
Both are required for the "zero burden" promise to hold.

## 6. Out of scope for v0.1

- Implementation of the named-argument bytecode (deferred to v0.2,
  when map / keyword literals land).
- Coexistence with multi-arity `defn` (`(defn foo ([x] ...) ([x y] ...))`)
  — multi-arity is its own dispatch axis; named arguments land on
  *one* arity at a time.
- Variadic capture (`& rest`) — that's the third axis (positional +
  named + variadic). Variadic was already specified in the v0.1
  roadmap; named is the addition.

## 7. Revisions

(empty)
