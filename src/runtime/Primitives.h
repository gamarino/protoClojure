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

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoString;
class ProtoList;
}

namespace protoClojure {

class ExecutionEngine;

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
    const proto::ProtoString*  bytecodeKey;
    const proto::ProtoString*  arityKey;
    const proto::ProtoString*  capturesKey;
    const proto::ProtoString*  aritiesKey;       // session 8
};
void setActiveCallContext(const ActiveCallContext& cc);
void clearActiveCallContext();
const ActiveCallContext* activeCallContext();

} // namespace protoClojure
