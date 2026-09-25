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
 * functions yet). When user-defined fns land, each
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
struct ActiveCallContext;
struct NamedLayout;

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

    // Top-level entry: run `mod` from PC=0 until RETURN or end-of-bytecode,
    // with the ActiveCallContext built from the arguments installed on this
    // thread (the previous one is restored on every exit path). The
    // `globals` namespace resolves PUSH_VAR. `fnSingleProto` /
    // `fnMultiProto` / `bytecodeKey` are the markers MAKE_FN uses to
    // construct user-fn wrappers and CALL uses to recognise them.
    //
    // Returns the value on top of the stack at RETURN, or PROTO_NONE if
    // the stack is empty. The returned pointer is UNROOTED — caller
    // responsible for rooting it in its own slot before doing anything
    // that allocates. Throws StackOverflowError (StackGuard.h) when calls
    // nest deeper than the thread's stack allows.
    const proto::ProtoObject* run(proto::ProtoContext* parent,
                                  const BytecodeModule& mod,
                                  const proto::ProtoObject* globals,
                                  const proto::ProtoObject* fnSingleProto,
                                  const proto::ProtoObject* fnMultiProto,
                                  const proto::ProtoObject* atomMarkerProto,
                                  const proto::ProtoObject* futureMarkerProto,
                                  const proto::ProtoObject* promiseMarkerProto,
                                  const proto::ProtoObject* actorMarkerProto,
                                  const proto::ProtoString* bytecodeKey,
                                  const proto::ProtoString* arityKey,
                                  const proto::ProtoString* capturesKey,
                                  const proto::ProtoString* aritiesKey,
                                  const proto::ProtoString* valueKey,
                                  const proto::ProtoString* watchesKey,
                                  const proto::ProtoString* thunkKey,
                                  const proto::ProtoString* ccBlobKey,
                                  const proto::ProtoString* threadKey,
                                  const proto::ProtoString* resultKey,
                                  const proto::ProtoString* doneKey,
                                  const proto::ProtoString* actorStateKey,
                                  const proto::ProtoString* mailboxKey,
                                  // Keyword and symbol values (Named.h),
                                  // installed in the ActiveCallContext for
                                  // printing and for primitives that build
                                  // keywords.
                                  const NamedLayout& named,
                                  const proto::ProtoObject* const* args = nullptr,
                                  unsigned int argCount = 0,
                                  const proto::ProtoObject* captures = nullptr,
                                  // Session 13 — pre-extracted kwArg values
                                  // in the order of mod.kwKeys(). nullptr
                                  // means the caller passed no kwArgs;
                                  // missing keys leave their slot as nil.
                                  const proto::ProtoObject* const* kwVals = nullptr,
                                  unsigned int kwCount = 0,
                                  // Session 14 — raw kwArgs map for the
                                  // `:as` binding (nullptr/PROTO_NONE when
                                  // none was supplied).
                                  const proto::ProtoObject* kwMap = nullptr);

private:
    // Runs `mod` in a new frame under the call context `env`, which must be
    // installed on this thread for the whole call (run installs it; invoke
    // and nested calls find it installed). Every nested call runs here, so
    // its native frame is what bounds the recursion depth: the context
    // travels as one reference instead of 23 arguments.
    const proto::ProtoObject* execute(proto::ProtoContext* parent,
                                      const BytecodeModule& mod,
                                      const ActiveCallContext& env,
                                      const proto::ProtoObject* const* args,
                                      unsigned int argCount,
                                      const proto::ProtoObject* captures,
                                      const proto::ProtoObject* const* kwVals,
                                      unsigned int kwCount,
                                      const proto::ProtoObject* kwMap);

    // protoClojure's garbage-collection safepoint.
    //
    // protoCore reclaims nothing a context has not handed over. Every cell a
    // context allocates is chained onto that context's *young generation*,
    // and the collector's root scan records the chain head as a root and
    // marks the whole chain — so until the chain is submitted, every cell the
    // context ever allocated is live by definition. A context submits its
    // chain in exactly two places: when it is destroyed, and from
    // `ProtoContext::safepoint()` once it has crossed
    // `ProtoSpace::maxAllocatedCellsPerContext` (10,000 cells by default,
    // `PROTOCORE_GC_CONTEXT_THRESHOLD` to change it).
    //
    // The VM builds one `ProtoContext` per frame, so a call that returns
    // shows the collector its garbage. A `loop` / `recur` does not: it is one
    // frame, and its context lives as long as the loop. Before this hook the
    // VM called `safepoint()` nowhere, so a loop's garbage accumulated in an
    // un-submitted chain for the loop's whole duration. Measured on
    // `(loop [i 0 acc 0] ... (str "garbage-" i) ...)` over 1,600,000
    // iterations, whose live set is two SmallIntegers: zero collection cycles,
    // the heap grown from 262,144 to 9,699,328 cells, 710 MB resident. Under
    // `PROTOCORE_HEAP_LIMIT_CELLS` the same program aborted with protoCore's
    // "live set 996519 cells, last cycle reclaimed 0 — out of memory": cycles
    // ran, and every one of them reclaimed exactly nothing.
    //
    // The loop back-edge (`Op::JUMP_BACK`) and frame entry are the two points
    // where the engine holds no half-built value in a C++ local: every live
    // value is in a frame slot — locals, or the operand stack, which IS the
    // frame context's `automaticLocals` — and therefore traced. That is
    // exactly the precondition `ProtoContext::safepoint()` documents, and it
    // is why the submission may not be made from `allocCell` instead.
    //
    // Frame entry submits nothing (a fresh frame has allocated nothing); it
    // is there because `safepoint()` is also protoCore's stop-the-world park
    // point, and recursion over the SmallInt fast-path opcodes allocates
    // nothing at all, so without it an allocation-free call tree would hold
    // up every other thread's collection for its whole duration.
    //
    // Polled on a stride, for cost. `ProtoContext::safepoint()` is a call
    // across protoCore's shared-library boundary that reads five fields of
    // the space and the context and takes `std::this_thread::get_id()` before
    // it can decide to do nothing, and protoClojure's loop body can be as
    // short as five opcodes — so on a 20,000,000-iteration loop that does
    // nothing but add, one call per back-edge costs +9.4 % instructions and
    // +17.0 % cycles. Measured, `perf stat -r 3`, against the same source
    // with both call sites removed.
    //
    // Neither of the safepoint's two jobs needs polling on every event.
    // Submission is threshold-gated at 10,000 cells inside protoCore, so it
    // is inherently amortised; parking needs bounded latency, not zero. One
    // poll per `kGcSafepointStride` events gives both: at 64 — the same
    // stride protoCore's own `allocCell` uses for its stop-the-world poll — a
    // loop allocating the six cells per iteration this project's own garbage
    // workload allocates overshoots the submission threshold by under 4 %,
    // and a stop-the-world waits at most 64 iterations longer. What a
    // back-edge pays drops to a decrement and a branch: +0.7 % instructions
    // and +1.3 % cycles on that same worst case.
    //
    // The counter is per thread, not per frame, so it also bounds the poll
    // rate of deep allocation-free recursion, where the events are frame
    // entries rather than back-edges.
    //
    // `PROTOCLJ_NO_GC_SAFEPOINT=1` disables the hook for A/B measurement and
    // is read once per process.
    void gcSafepoint(proto::ProtoContext* ctx) const {
        if (--gcSafepointCountdown_ == 0) gcSafepointSlow(ctx);
    }

    // The strided path: resets the countdown and, unless disabled, calls
    // `ProtoContext::safepoint()` on `ctx`. Defined in ExecutionEngine.cpp,
    // because this header only forward-declares `proto::ProtoContext`.
    void gcSafepointSlow(proto::ProtoContext* ctx) const;

    // Events between polls; see gcSafepoint. A power of two, so the countdown
    // reload is an immediate.
    static constexpr unsigned kGcSafepointStride = 64;

    // Counts down to the next poll on this thread. Seeded at 1 so the first
    // event on a thread polls, which keeps a short program's behaviour
    // independent of the stride.
    static thread_local unsigned gcSafepointCountdown_;

    // Read once from PROTOCLJ_NO_GC_SAFEPOINT at static-init time; see
    // gcSafepoint.
    static const bool gcSafepointEnabled_;
};

} // namespace protoClojure
