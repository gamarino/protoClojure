/*
 * Primitives — C++-implemented Clojure functions installed as vars in the
 * globals namespace at runtime startup.
 *
 * Each primitive matches protoCore's ProtoMethod signature:
 *   (ctx, self, parentLink, posArgs, kwArgs) -> result
 *
 * Wrapped into a callable ProtoObject via `ctx->fromMethod(self, fnPtr)` and
 * installed on the globals namespace under its symbol name. The VM's CALL
 * opcode dispatches via the standard `asMethod`/`asMethodSelf` path.
 *
 * Session 3 ships only `println`. Subsequent sessions add `+`, `-`, `*`,
 * `=`, `<`, `str`, `inc`, `dec`, `count`, `first`, `rest`, `list`,
 * `vector` as planned in the phase-1 spec.
 */
#pragma once

#include "Named.h"

#include <cstdio>

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoString;
class ProtoList;
}

namespace protoClojure {

class ExecutionEngine;
struct MapLayout;

// Value equality as `=` defines it, shared by the `=` / `not=` primitives
// and the VM's EQ opcode. Maps are equal when they hold the same keys mapped
// to equal values, whatever the insertion order (mapEquals). Sequential
// collections — lists and vectors, and so every sequence the runtime
// produces — are equal when they hold equal elements in the same order,
// whatever their concrete types: `[1 2]` equals `(1 2)` and `[]` equals
// `()`. Values are compared recursively with this same function, so nested
// collections compare structurally. A map is never equal to a non-map, and
// a sequential collection is never equal to a non-sequential value. Every
// other pair of values is compared with protoCore `compare(ctx, other) == 0`:
// numbers across types (deviation D15), strings by content, and everything
// else by identity. Keywords and symbols are interned named values
// (Named.h), so identity is equality of spelling, and a keyword or a symbol
// is never equal to a string. nullptr is nil. Both values must be rooted by
// the caller; the function allocates nothing itself.
//
// Cost: one pass over the elements of a sequential pair, stopping at the
// first mismatch; each element is read by index in O(log n).
bool valuesEqual(proto::ProtoContext* ctx, const MapLayout& layout,
                 const proto::ProtoObject* a, const proto::ProtoObject* b);

// Value hash consistent with valuesEqual, used by MapOps as the hash of
// every map key: valuesEqual(a, b) implies valueHash(a) == valueHash(b) for
// every pair of values except those involving NaN (below).
//
//   - Numbers hash to their value modulo 2^61 - 1 (the CPython scheme), so
//     equal numbers hash equally whatever their representation — SmallInteger,
//     LargeInteger or double (deviation D15): 1 and 1.0, or 2^70 as a
//     LargeInteger and as a double. The infinities hash to fixed values.
//   - NaN hashes to 0, like the integer 0. protoCore compare reports NaN
//     equal to every number, so no hash consistent with `=` exists for NaN;
//     as a map key NaN is matched only by numbers hashing to 0 (0, 0.0,
//     -0.0, multiples of 2^61 - 1, and NaN) and vice versa.
//   - Strings use protoCore's content hash.
//   - Maps combine a mix of each entry's key hash and value hash with a sum,
//     so the hash ignores insertion order.
//   - Lists and vectors share one order-dependent combination of their
//     element hashes, so `[1 2]` and `(1 2)` hash equally.
//   - Every other object (keywords, symbols, atoms, functions, ...) hashes
//     its address, and nil and booleans use protoCore's identity-based
//     hash, matching identity equality; for keywords and symbols, interning
//     makes identity equivalent to equality of spelling.
//
// Collections hash recursively. Nothing is cached: a collection is hashed
// in full on every call — O(1) for numbers in the long long range, strings
// and keywords; linear in the number of nested elements for collections
// (list and vector elements are read by index in O(log n)). An integer
// beyond the long long range renders its digits, which allocates; `v` must
// be rooted by the caller. nullptr is nil.
unsigned long valueHash(proto::ProtoContext* ctx, const MapLayout& layout,
                        const proto::ProtoObject* v);

// Install all v0.0.x primitives on the supplied globals object. The globals
// object must be a mutable protoCore object (typically a child of
// objectPrototype) so setAttribute is in-place. After this call, references
// to the corresponding symbols from compiled bytecode will resolve.
void installPrimitives(proto::ProtoContext* ctx,
                       proto::ProtoObject* globals);

// Session 7 — primitives that invoke user fns (map / reduce / filter)
// need access back into the bytecode VM. The ExecutionEngine installs
// itself here on each top-level run() entry and restores on exit, so the
// primitives can reach the current VM through this slot without changing
// the ProtoMethod signature. Thread-local; safe under concurrent VMs in
// later sessions.
struct ActiveCallContext {
    ExecutionEngine*           engine;
    const proto::ProtoObject*  globals;
    // Session 12 — split the single prototype `fnMarkerProto` into two:
    // single-arity wrappers carry `__bytecode__` (+ `__captures__` only
    // when the body has captures), multi-arity wrappers carry
    // `__arities__`. The CALL handler picks the path via `getPrototype`
    // alone — no more `getAttribute(aritiesKey)` probe to discover
    // arity-shape on every single-arity call.
    const proto::ProtoObject*  fnSingleProto;
    const proto::ProtoObject*  fnMultiProto;
    // Session 13 — map runtime values are children of mapMarkerProto
    // carrying their state under mapStateKey. Only src/runtime/MapOps
    // reads or writes that state (layout documented in MapOps.h).
    const proto::ProtoObject*  mapMarkerProto;
    // Session 16 — atomMarkerProto identifies atoms; valueKey stores
    // the current value as an attribute. swap! / reset! mutate via
    // setAttribute / setAttributeIfEqual on the receiver atom.
    const proto::ProtoObject*  atomMarkerProto;
    // Session 17 — futureMarkerProto identifies futures. Wire shape:
    //   __thunk__   the 0-arg fn that produces the result.
    //   __cc_blob__ pointer (as long) to the parent's ActiveCallContext.
    //   __thread__  pointer (as long) to the running ProtoThread.
    //   __result__  the value, once computed.
    //   __done__    PROTO_TRUE when realized, otherwise PROTO_FALSE.
    const proto::ProtoObject*  futureMarkerProto;
    // Session 18 — promises. Same valueKey/doneKey as the rest of the
    // family. deliver does a single-shot CAS on the value attribute;
    // deref busy-waits in goUnmanaged-protected sleeps.
    const proto::ProtoObject*  promiseMarkerProto;
    // Session 19 — actors. The wrapper carries `__actor_state__`
    // (long handle to ActorState) and mirrors the current value
    // under `__value__` so `@actor` reads work via the standard
    // deref path.
    const proto::ProtoObject*  actorMarkerProto;
    const proto::ProtoString*  bytecodeKey;
    const proto::ProtoString*  arityKey;
    const proto::ProtoString*  capturesKey;
    const proto::ProtoString*  aritiesKey;       // multi-arity dispatch list
    const proto::ProtoString*  mapStateKey;      // map state (MapOps.h)
    const proto::ProtoString*  valueKey;         // atom value
    const proto::ProtoString*  watchesKey;       // atom watches
    // Session 17 keys.
    const proto::ProtoString*  thunkKey;
    const proto::ProtoString*  ccBlobKey;
    const proto::ProtoString*  threadKey;
    const proto::ProtoString*  resultKey;
    const proto::ProtoString*  doneKey;
    // Session 19.
    const proto::ProtoString*  actorStateKey;
    // Keyword and symbol values (Named.h).
    NamedLayout                named;
};
void setActiveCallContext(const ActiveCallContext& cc);
void clearActiveCallContext();
const ActiveCallContext* activeCallContext();

// Join every worker thread spawned by `(future …)` (and therefore by
// `pmap`). Call this before ProtoSpace destructs — otherwise an in-
// flight worker dereferencing a now-freed Cell crashes the process at
// exit. Mirrors `ActorScheduler::shutdown`, idempotent.
void shutdownFutures(proto::ProtoContext* ctx);

// Same value-formatter `println` / `prn` use. Exposed so the REPL can
// echo evaluated results in the canonical Clojure shape without
// rebuilding the printer. Falls back to "nil" on a null pointer.
void replPrintValue(proto::ProtoContext* ctx, std::FILE* out,
                    const proto::ProtoObject* v);

} // namespace protoClojure
