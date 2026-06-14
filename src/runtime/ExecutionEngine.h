/*
 * ExecutionEngine — runs a BytecodeModule.
 *
 * P1/P2: the engine pushes one proto::ProtoContext per top-level run.
 * The context's automaticLocals IS the operand stack — every value the
 * VM holds during execution lives in a slot the GC walks. A separate
 * sp_ tracks the stack pointer; the slot region is sized generously at
 * frame entry.
 *
 * P3: no std container holds ProtoObject*. The frame's stack is
 * automaticLocals; intermediate working ProtoLists (e.g. the args list
 * for CALL) live in slots of a child context that destructs at the end
 * of the instruction.
 *
 * Session 3 minimum: one flat run, no nested frames yet (no user
 * functions yet). When user-defined fns land (session 4+), each
 * invocation pushes its own context, chained via `previous` to the
 * caller's frame — the protoCore method-invocation model.
 */
#pragma once

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoString;
}

namespace proto {
class ProtoList;
}

namespace protoClojure {

class BytecodeModule;

class ExecutionEngine {
public:
    // Session 7 — external dispatch entry. Used by primitives that need
    // to invoke a Clojure callable mid-primitive (map / reduce / filter
    // calling the f they were handed). Handles user fns and C++
    // primitives uniformly. Variadic packing matches the in-VM CALL
    // path. Returns the callable's return value; never reads the
    // operand stack.
    const proto::ProtoObject* invoke(proto::ProtoContext* ctx,
                                     const proto::ProtoObject* callable,
                                     const proto::ProtoObject* const* args,
                                     unsigned int argc);

    // Run `mod` from PC=0 until RETURN or end-of-bytecode. The `globals`
    // namespace resolves PUSH_VAR. `fnMarkerProto` / `bytecodeKey` /
    // `arityKey` are the markers MAKE_FN uses to construct user-fn
    // wrappers and CALL uses to recognise them.
    //
    // Returns the value on top of the stack at RETURN, or PROTO_NONE if
    // the stack is empty. The returned pointer is UNROOTED — caller
    // responsible for rooting it in its own slot before doing anything
    // that allocates.
    const proto::ProtoObject* run(proto::ProtoContext* parent,
                                  const BytecodeModule& mod,
                                  const proto::ProtoObject* globals,
                                  const proto::ProtoObject* fnSingleProto,
                                  const proto::ProtoObject* fnMultiProto,
                                  const proto::ProtoString* bytecodeKey,
                                  const proto::ProtoString* arityKey,
                                  const proto::ProtoString* capturesKey,
                                  const proto::ProtoString* aritiesKey,
                                  const proto::ProtoObject* const* args = nullptr,
                                  unsigned int argCount = 0,
                                  const proto::ProtoObject* captures = nullptr);
};

} // namespace protoClojure
