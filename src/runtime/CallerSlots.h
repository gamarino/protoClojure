/*
 * CallerSlots — how a primitive roots what it holds without a context.
 *
 * A primitive (a C++ ProtoMethod) and every helper it calls run in the
 * caller's ProtoContext: the frame of the Clojure fn that called it, or the
 * context `invoke` was given. They build no context of their own
 * (docs/DESIGN.md §10.2, M45): a context belongs to a method activation, a
 * Clojure fn frame, because only a frame has local variables of its own.
 *
 * What protects a primitive's values, then:
 *
 *  - Its arguments are in traced slots: the caller frame's operand stack
 *    (CALL, CALL_MEMBER), or storage the caller of `invoke` roots.
 *  - What it allocates is a young cell of the caller's context, and a young
 *    cell stays live until that context's chain is submitted, by its
 *    `safepoint()` or its destruction (EMBEDDER-CONFORMANCE rules 1 and 3).
 *  - A callback's result is anchored in the caller's context: the callee
 *    frame's destructor places a ReturnReference in its `previous`.
 *  - While a primitive runs, nothing submits the caller's chain except
 *    CallerSlots::safepoint, and that does so only under a SafepointPermit
 *    (below). The frame's dispatch loop is suspended, and every callback runs
 *    in a frame of its own, whose safepoints submit only that frame's chain.
 *
 * So without a permit a primitive needs no slots at all for correctness,
 * except for a value read from shared mutable state (an atom) that another
 * thread may drop while the primitive still uses it: `pin` it.
 *
 * Bounded memory needs more. A primitive that loops for long (reduce, count,
 * a walk over a lazy seq, a lazy filter skipping many elements) produces
 * garbage, and the caller's chain keeps all of it until it is submitted. A
 * loop hands it to the collector by calling `tick()`, which submits the
 * caller frame's chain every kTickStride calls, and that is safe only when
 * every value alive on the frame is in a traced slot. The frame's own values
 * are (the VM holds nothing in C++ locals at a call); the primitive's must be
 * in CallerSlots. Two conditions make that checkable:
 *
 *  - The primitive is the frame's outermost primitive activation (depth 1 of
 *    the PrimitiveWindow): a primitive called through `invoke` by another
 *    one never submits, because its caller may hold C++ locals.
 *  - The primitive's body holds a SafepointPermit: its promise that, while
 *    the permit lives, it keeps every value it needs across a call in
 *    CallerSlots or its arguments. A helper it calls (a lazy-seq step, a
 *    walk) may then tick; each helper keeps its own values in CallerSlots
 *    across the calls it makes, and returns its result through `ret()`,
 *    which anchors the result in the caller's chain when a submission
 *    happened while the helper ran.
 *
 * The slots: every VM frame keeps kPrimitiveSlots spare slots above its
 * operand stack, and the VM installs a PrimitiveWindow over the free part of
 * that stack around each primitive call. Taking slots is a pointer bump;
 * releasing clears them. With no window (a primitive called from host code:
 * an actor turn, a future's thread, the compiler) or with the window used up,
 * CallerSlots holds the values in its own C++ array, which is correct because
 * nothing submits the caller's chain then (tick does nothing; `pin` anchors
 * the value in the caller's young chain with one list cell).
 */
#pragma once

namespace proto {
class ProtoContext;
class ProtoObject;
}

namespace protoClojure {

// Spare slots every VM frame keeps above its operand stack for the
// primitives it calls (CallerSlots).
constexpr unsigned int kPrimitiveSlots = 16;

// The free traced slots of the frame whose primitive is running.
struct PrimitiveWindow {
    proto::ProtoContext*        frame;  // the frame that owns the slots
    const proto::ProtoObject**  next;   // first free slot
    const proto::ProtoObject**  end;    // one past the last slot
    unsigned int                depth;  // primitive activations sharing it
    unsigned int                local = 0;        // live CallerSlots outside the window
    unsigned int                permits = 0;      // live SafepointPermits
    unsigned int                submissions = 0;  // safepoint() calls made
};

// The window of the primitive call in progress on this thread, or nullptr.
PrimitiveWindow*& currentPrimitiveWindow();

// Installed by the VM around a primitive call (CALL, CALL_MEMBER): `w` is the
// window over the calling frame's free slots, depth 1. Restores the previous
// window, the one of an enclosing primitive call, on every exit path.
class PrimitiveCallScope {
public:
    explicit PrimitiveCallScope(PrimitiveWindow& w)
        : slot_(currentPrimitiveWindow()), saved_(slot_) { slot_ = &w; }
    ~PrimitiveCallScope() { slot_ = saved_; }
    PrimitiveCallScope(const PrimitiveCallScope&) = delete;
    PrimitiveCallScope& operator=(const PrimitiveCallScope&) = delete;
private:
    PrimitiveWindow*& slot_;
    PrimitiveWindow*  saved_;
};

// Installed by `invoke` around a primitive it calls: one more activation on
// the current window, so the nested primitive never submits a chain its
// caller relies on.
class NestedPrimitiveScope {
public:
    NestedPrimitiveScope() : w_(currentPrimitiveWindow()) { if (w_) ++w_->depth; }
    ~NestedPrimitiveScope() { if (w_) --w_->depth; }
    NestedPrimitiveScope(const NestedPrimitiveScope&) = delete;
    NestedPrimitiveScope& operator=(const NestedPrimitiveScope&) = delete;
private:
    PrimitiveWindow* w_;
};

// A primitive body's promise, for as long as it lives, that every value it
// holds across a call is in CallerSlots or in its arguments, so the helpers
// it calls may submit the caller frame's chain (CallerSlots::tick). Takes
// effect only for the frame's outermost primitive activation.
class SafepointPermit {
public:
    explicit SafepointPermit(proto::ProtoContext* ctx);
    ~SafepointPermit();
    SafepointPermit(const SafepointPermit&) = delete;
    SafepointPermit& operator=(const SafepointPermit&) = delete;
private:
    PrimitiveWindow* w_;
};

// Held while code keeps values a submission would lose in C++ storage (a
// batch of parked elements): no CallerSlots::safepoint submits on the current
// window while any block is held. hold() and release() may be called many
// times; the destructor releases.
class SafepointBlock {
public:
    SafepointBlock() : w_(currentPrimitiveWindow()) {}
    ~SafepointBlock() { release(); }
    SafepointBlock(const SafepointBlock&) = delete;
    SafepointBlock& operator=(const SafepointBlock&) = delete;
    void hold() { if (!held_) { held_ = true; if (w_) ++w_->local; } }
    void release() { if (held_) { held_ = false; if (w_) --w_->local; } }
private:
    PrimitiveWindow* w_;
    bool held_ = false;
};

// `n` traced slots (at most kMaxSlots) for code running in `ctx` on behalf of
// a primitive, set to PROTO_NONE. Released, cleared, at scope exit; they are
// a stack, released in reverse order of acquisition.
class CallerSlots {
public:
    static constexpr unsigned int kMaxSlots = 8;
    static constexpr unsigned int kTickStride = 64;

    CallerSlots(proto::ProtoContext* ctx, unsigned int n);
    ~CallerSlots();
    CallerSlots(const CallerSlots&) = delete;
    CallerSlots& operator=(const CallerSlots&) = delete;

    proto::ProtoContext* ctx() const { return ctx_; }
    const proto::ProtoObject* get(unsigned int i) const { return slots_[i]; }
    void set(unsigned int i, const proto::ProtoObject* v) { slots_[i] = v; }
    // set() for a value read from shared mutable state, which another thread
    // may drop: outside a window it is also anchored in the caller's chain.
    void pin(unsigned int i, const proto::ProtoObject* v);
    // The slots as a contiguous array (argument lists for `invoke`).
    const proto::ProtoObject* const* data() const { return slots_; }
    // True when the slots are traced (in a PrimitiveWindow); false in
    // fallback mode, where they are a C++ array the collector does not see.
    bool traced() const { return inWindow_; }

    // One iteration of a loop: every kTickStride calls, safepoint(). Call it
    // only where everything the code still needs is in these slots (or in
    // CallerSlots of its callers).
    void tick() { if (++ticks_ % kTickStride == 0) safepoint(); }
    // Hands the caller frame's young generation to the collector
    // (ProtoContext::safepoint) when that is safe: these slots are in the
    // window of the frame `ctx`, the primitive is the frame's outermost
    // activation, its body holds a SafepointPermit and no CallerSlots of it
    // is outside the window. Otherwise it does nothing.
    void safepoint();
    // `v`, a result about to leave this scope, anchored in the caller's
    // chain if a submission happened since this scope began (it may then
    // be held only by these slots).
    const proto::ProtoObject* ret(const proto::ProtoObject* v);

private:
    proto::ProtoContext*        ctx_;
    PrimitiveWindow*            window_;   // current at construction, or nullptr
    const proto::ProtoObject**  slots_;
    unsigned int                n_;
    unsigned int                ticks_ = 0;
    unsigned int                submissionsAtStart_ = 0;
    bool                        inWindow_ = false;
    const proto::ProtoObject*   local_[kMaxSlots];
};

} // namespace protoClojure
