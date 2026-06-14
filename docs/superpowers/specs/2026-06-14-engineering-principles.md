# Engineering principles — protoCore-side discipline

> **Purpose.** Codify the architectural rules that the user has set
> for the project before any code that could violate them is
> written. The previous protoCore-hosted runtimes (protoST,
> protoJS, protoPython) acquired some of these rules incrementally
> and paid a real cost in latent bugs that ASan or stress tests
> would surface months later. protoClojure starts under the rules.

**Date.** 2026-06-14. Each principle has an ID; future code review
references the ID.

---

## P1 — `ProtoContext` + `automaticLocals` from day one

**Rule.** Every function that holds a `ProtoObject*` across an
operation that may allocate MUST hold it in a
`ProtoContext::automaticLocals` slot, NOT in a C++ stack local or a
member field.

**Why.** The protoCore tracing GC walks `automaticLocals` to find
roots. A pointer held only in a C++ register or stack slot is
*invisible* to the GC and may be reclaimed mid-computation. The
symptom is a use-after-free that ASan surfaces only under heap
pressure — which is exactly the bug class the previous runtimes
spent quarters debugging.

**Concretely.** A function that does:

```cpp
const proto::ProtoObject* x = something_that_allocates(ctx);
// ... another allocation here ...
do_something_with(x);   // x may already be dead
```

…is wrong. The fix:

```cpp
proto::ProtoContext frame(ctx->space, ctx);
frame.resizeAutomaticLocals(1);
frame.setAutomaticLocal(0, something_that_allocates(&frame));
// ... another allocation here ...
do_something_with(frame.getAutomaticLocal(0));
```

The intermediate is rooted in a slot the GC walks. Window between
"return from allocator" and "store into slot" is one C++ statement
— with no allocation between them — and is safe.

**Calling convention.** Functions return UNROOTED `ProtoObject*`.
The caller MUST store the result in its own slot before doing
anything that allocates. This is the protoCore norm; do not
deviate.

**Reference.** `src/reader/Reader.cpp::readList()` is the canonical
implementation pattern.

---

## P2 — One `ProtoContext` per method invocation

**Rule.** `ProtoSpace` is per-process. `ProtoContext` is per method
invocation. Each recursive entry into a slot-using function pushes
a new child context chained to its parent via `previous`.

**Why.** Each context is bound to its calling thread. The
`previous` chain represents the call stack; the GC walks it to find
all roots on every thread. Pushing a new context per invocation
keeps the slot regions scoped — the previous invocation's slots are
not visible to the current one, so no slot index can collide
across recursion depths. Multithreading comes for free: two
threads each have their own context chain rooted at their thread's
slot of the `ProtoSpace`. No locks. No shared state.

**Concretely.**

```cpp
const ProtoObject* readList(ProtoContext* parent, ...) {
    proto::ProtoContext frame(parent->space, parent);
    frame.resizeAutomaticLocals(N);
    // ... work in frame's slots ...
    return frame.getAutomaticLocal(0);   // caller roots immediately
}
```

The child context destructs on return. Its slots are freed. Any
`ProtoObject*` value returned from the function is no longer
rooted by us — the caller takes responsibility (see P1).

**Reference.** `src/reader/Reader.cpp::readList()` and `readAll()`.

---

## P3 — No `std::` objects inside protoCore

**Rule.** `std::string`, `std::vector`, `std::map`, `std::pair`,
and any other `std::` container or RAII type MUST NOT be stored
inside a protoCore container (as an attribute, in a SparseList
slot, in a ProtoList element, etc.) OR be reachable from a
`ProtoObject*`.

**Why.** The protoCore tracing GC is a machine for protoCore
objects. A `std::string` stored as an attribute is invisible to the
GC's destroy/reclaim cycle; lifetimes get tangled; ASan eventually
reports a leak or a use-after-free at process exit.

**Concretely — where `std::` is OK:**

- Function-local C++ variables that hold POD or `std::` types and
  never get stored into protoCore. Example: a `Token` struct with a
  `std::string text` member, lex-stage transient, never crossing
  into the runtime.
- Buffer ownership (the Lexer's `std::string source_` — the input
  we own).
- Exception messages (`ReaderError` carries `std::string` via
  `std::runtime_error`).

**Concretely — where `std::` is NOT OK:**

- As an attribute value via `setAttribute`.
- As a list element via `appendLast`.
- As a closure capture in a callable.
- Anything reachable by tracing from a `ProtoObject*`.

**The boundary.** When data crosses from std-side to protoCore-side,
copy out via raw bytes:

```cpp
// OK — protoCore copies the bytes; the std::string can die afterwards
ctx->fromUTF8String(tok.text.c_str());

// OK — same; protoCore interns the symbol from the raw bytes
proto::ProtoString::createSymbol(ctx, tok.text.c_str());

// WRONG — would mix allocators
list->setAttribute(ctx, key, &my_std_string);
```

After the call returns, the protoCore object is independent of the
std-side source. The std-side source can be discarded.

**Reference.** `src/reader/Reader.cpp::readFromToken()` shows
every crossing. Each one passes either POD (`long long`), a raw
`const char*` (which protoCore copies from), a `ProtoObject*`
(protoCore-native), or a `ProtoContext*` (protoCore-native).

---

## P4 — Document deviations explicitly

When a future session genuinely needs to violate one of P1–P3 (for
example, to embed a foreign-language object that is *not* a
ProtoObject), the deviation lands as a new section in this
document with the rationale, the bounded scope, and the cleanup
plan. The principle is honesty: deviations are recorded, not
silently introduced.

---

## How to use this document

- **Before a new module is added** (compiler, VM, primitives,
  REPL, …), the author reads P1–P3 and confirms the module's API
  is consistent with them.
- **In code review**, citations like "P1 violation in readList:
  the intermediate elem is unrooted across appendLast" are short,
  unambiguous, and actionable.
- **When ASan reports a UAF or LSan reports a leak**, P1 and P3
  are the first hypotheses to check; the bug is almost always one
  of them.

This document is short on purpose. Three rules, one anti-rule.
Anything that does not fit here probably belongs in DESIGN.md or
the per-phase specs.
