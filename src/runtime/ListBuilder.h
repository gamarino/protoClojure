/*
 * ListBuilder — grow a protoCore ProtoList of unbounded length in the
 * caller's context, without a context of its own (CallerSlots.h).
 *
 * `ProtoList` is immutable: `appendLast` returns a NEW list that shares
 * structure with the old one and leaves one root-to-leaf path behind as
 * garbage, about log2(N) cells per append. That garbage is a young cell of
 * the caller's context, and reaches the collector when that context's chain
 * is submitted: at the frame's next safepoint, or, inside a primitive that
 * holds a SafepointPermit, every time the builder folds a chunk (below).
 *
 * The accumulator lives in a CallerSlots slot, and so do up to kMaxPending
 * elements waiting to be folded into it when the slots are traced (a
 * PrimitiveWindow). After each fold the builder ticks (CallerSlots::tick), so a long
 * build inside a primitive that holds a SafepointPermit hands its garbage
 * over chunk by chunk. Without traced slots (host code: the reader, an actor
 * turn) each element is appended at once, so it is referenced by a young
 * cell of the caller's context as soon as it is pushed.
 *
 * Until 2026-10-25 the builder opened a context of its own per build, and a
 * grandchild per chunk; the rule now is that only a method activation (a
 * Clojure fn frame) has a context (docs/DESIGN.md §10.2, M45).
 *
 * `context()` is the caller's context; it is kept so call sites read the same.
 *
 * Calling convention: `push` takes a value rooted by the caller or a young
 * cell of the caller's context (a fresh result qualifies). `finish()`
 * returns an UNROOTED pointer, anchored in the caller's chain when a
 * submission happened during the build; the caller stores it in a slot or
 * returns it before anything else allocates.
 */
#pragma once

#include "protoCore.h"  // proto::proto_long / proto_ulong
#include "CallerSlots.h"

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoList;
}

namespace protoClojure {

class ListBuilder {
public:
    /** Opens a build in `ctx`. Allocates no cell until the first fold. */
    explicit ListBuilder(proto::ProtoContext* ctx);
    ~ListBuilder();

    ListBuilder(const ListBuilder&) = delete;
    ListBuilder& operator=(const ListBuilder&) = delete;

    /** Appends one element (see the calling convention above). */
    void push(const proto::ProtoObject* value);

    /** Number of elements pushed so far. */
    proto::proto_ulong size() const { return size_; }

    /** The caller's context, in which the build runs. */
    proto::ProtoContext* context() { return ctx_; }

    /**
     * The finished list as a `ProtoObject*`. UNROOTED — see the calling
     * convention above. Calling `push()` afterwards is valid and resumes the
     * build from the returned value.
     */
    const proto::ProtoObject* finish();

    /** `finish()` typed as a `ProtoList*`. Same rooting contract. */
    const proto::ProtoList* finishList();

    /**
     * The elements pushed as a Clojure list (SeqTypes.h): what a primitive
     * answers as a list. Same rooting contract; `push()` may not follow.
     */
    const proto::ProtoObject* finishSeq();

private:
    // A build of at most kMaxPending elements is built straight from the
    // parked elements when it finishes as a Clojure list, with no ProtoList
    // in between.
    static constexpr unsigned int kMaxPending = 5;
    static constexpr unsigned int kSlotAcc = 0;
    static constexpr unsigned int kSlotPending0 = 1;

    /** Folds the parked elements into the accumulator. */
    void flush();
    const proto::ProtoList* acc();

    proto::ProtoContext*       ctx_;
    CallerSlots                slots_;    // the accumulator, then the parked elements
    unsigned int               npending_ = 0;
    proto::proto_ulong         size_     = 0;
};

} // namespace protoClojure
