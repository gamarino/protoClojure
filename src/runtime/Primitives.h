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
#include <string>

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

// True when `v` is a number: an integer (SmallInteger or LargeInteger) or a
// float. nullptr is nil, not a number. Decided by the pointer tag alone, with
// the tag values of protoCore's headers/proto_internal.h (a SmallInteger
// carries POINTER_TAG_EMBEDDED_VALUE with EMBEDDED_TYPE_SMALLINT in its low
// ten bits; POINTER_TAG_LARGE_INTEGER = 14, POINTER_TAG_DOUBLE = 15), so the
// check on the arithmetic opcodes' slow path costs three compares and no
// call.
inline bool isNumber([[maybe_unused]] proto::ProtoContext* ctx,
                     const proto::ProtoObject* v) {
    constexpr unsigned long kSmallIntMask     = 0x3FFUL;
    constexpr unsigned long kSmallIntValue    = 0x001UL;
    constexpr unsigned long kPointerTagMask   = 0x3FUL;
    constexpr unsigned long kTagLargeInteger  = 14;
    constexpr unsigned long kTagDouble        = 15;
    const auto bits = reinterpret_cast<unsigned long>(v);
    const unsigned long tag = bits & kPointerTagMask;
    return (bits & kSmallIntMask) == kSmallIntValue ||
           (v != nullptr && (tag == kTagDouble || tag == kTagLargeInteger));
}

// The type of `v` as error messages name it, with its article: "nil",
// "a boolean", "an integer", "a float", "a string", "a list", "a vector",
// "a fn", and, when an ActiveCallContext is installed, "a keyword",
// "a symbol", "a map", "an atom", "a future", "a promise", "an actor";
// "an object" otherwise. Allocates nothing.
const char* valueTypeName(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// Raises the analogue of JVM Clojure's ClassCastException for a
// non-numeric operand of an arithmetic or ordering operation:
// "ClassCastException: <operation> expects a number, got <type>", with the
// type from valueTypeName (`(+ 1 nil)`: "+ expects a number, got nil").
// A double as JVM Clojure prints it (Java's Double.toString, JDK 19 and
// later): the shortest digits that read back as the same double, in plain
// notation for magnitudes in [1e-3, 1e7) (`100.0`, `0.3333333333333333`)
// and as <digit>.<digits>E<exponent> otherwise (`1.0E21`, `4.9E-324`);
// `-0.0` keeps its sign; infinities and NaN are `##Inf`, `##-Inf` and
// `##NaN`. The one float printer of println, str, join and the REPL.
std::string formatDouble(double d);

// Cold: call sites move out of the VM's dispatch loop.
[[noreturn, gnu::cold]]
void throwNotANumber(proto::ProtoContext* ctx, const char* operation,
                     const proto::ProtoObject* v);

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

// The value printer `println`, `str` and `join` use (printTo in
// Primitives.cpp), in its readable mode, writing to `out`. Exposed so the
// REPL echoes evaluated results the way Clojure's REPL does: strings are
// quoted and escaped at every depth (`"a"`, `["a"]`), while `println`
// prints them bare. A null pointer prints "nil".
void replPrintValue(proto::ProtoContext* ctx, std::FILE* out,
                    const proto::ProtoObject* v);

} // namespace protoClojure
