# Design specifications (archive)

Historical design documents written during development. They are not maintained and may not match the current code; see the [README](../../../README.md) and [docs](../../) for current documentation.

- [2026-06-14-clojure-core-v0.1.md](2026-06-14-clojure-core-v0.1.md) — Planned `clojure.core` surface for v0.1: which names ship, whether each is implemented in C++ or in Clojure, bootstrap order and supported arities.
- [2026-06-14-engineering-principles.md](2026-06-14-engineering-principles.md) — Rules for code at the protoCore boundary: `ProtoContext` and `automaticLocals` for held objects, one `ProtoContext` per method invocation, no `std::` containers holding protoCore objects, and explicit documentation of deviations.
- [2026-06-14-foreign-dispatch.md](2026-06-14-foreign-dispatch.md) — Design for applying the core collection functions (`count`, `first`, `get`, `nth`, ...) to Python, JavaScript and protoST objects loaded through UMD, with no conversion by the caller.
- [2026-06-14-phase-1-bootstrap-interpreter.md](2026-06-14-phase-1-bootstrap-interpreter.md) — Implementation plan for the first interpreter milestone: reader, compiler, bytecode VM and a minimal `core.clj`.
- [2026-06-14-protocore-call-convention.md](2026-06-14-protocore-call-convention.md) — How protoClojure adopts the protoCore call convention (positional and named arguments) for calls in both directions.
