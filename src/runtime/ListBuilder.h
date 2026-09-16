/*
 * ListBuilder — grow a protoCore ProtoList of unbounded length without
 * pinning the intermediate versions in an un-collectable young generation.
 *
 * Why this type exists
 * --------------------
 * `ProtoList` is immutable: `appendLast` returns a NEW list that shares
 * structure with the old one and leaves one root-to-leaf path behind as
 * garbage — about log2(N) cells per append. Building an N-element list by
 * repeated `appendLast` therefore allocates on the order of N*log2(N) cells
 * of which only O(N) are live at the end.
 *
 * protoCore reclaims a context's allocations — its "young generation" — only
 * when that context is DESTROYED (`~ProtoContext` submits the chain to the
 * collector) or when the embedder calls `ProtoContext::safepoint()` at a
 * point where every live value is reachable from a real GC root. A native
 * primitive that appends N times inside ONE long-lived context therefore
 * holds every intermediate version until it returns: the garbage is
 * unreachable, but the collector has never been shown it, so it cannot be
 * freed. Under a heap ceiling (`PROTOCORE_HEAP_LIMIT_CELLS`) the build
 * exhausts the heap even though its live data is tiny.
 *
 * What this type does
 * -------------------
 * The accumulator lives in an automatic local of `holder_`, a child context
 * of the caller's — so the GC always sees it as a root (principle P1). The
 * appends run in a SHORT-LIVED grandchild context created and destroyed once
 * per chunk; destroying it submits that chunk's intermediate versions to the
 * collector while the accumulator stays rooted in `holder_`. The heap high
 * water mark of a build is therefore one chunk, not the whole build.
 *
 * Values waiting to be appended are parked in `holder_`'s slots too, never in
 * a C++ array: `push()` stores into a GC-visible slot BEFORE anything
 * allocates, so an element cannot be reclaimed between the call that produced
 * it and the append that consumes it.
 *
 * Using it: `context()` is the innermost context
 * --------------------------------------------
 * A thread's contexts form a strict LIFO stack — `~ProtoContext` makes
 * `previous` current again — and the GC walks a thread's roots by following
 * that chain. While a builder is open its `holder_` IS the innermost context
 * of the calling thread, so ANY context a caller creates in the loop must
 * chain to it: pass `context()`, not the caller's own `ctx`, to every nested
 * call that may push a context (`ExecutionEngine::invoke`, `readFromToken`,
 * `ProtoThread::join`). Creating a context whose `previous` is an ancestor
 * instead would unlink the builder from the thread's chain when that context
 * destructs, and the accumulator would stop being a GC root.
 *
 * For the same reason a builder must be CLOSED (destroyed, typically by a
 * nested block) before the caller resumes handing its own context to code
 * that pushes contexts. Plain reads and allocations through any context are
 * always fine — only context construction is ordered.
 *
 * Between two calls to `push()` no context of the builder's own is alive
 * beyond `holder_`, so a caller may freely invoke Clojure code in the loop.
 * That is what makes this usable from `map`, `filter`, `reverse` and the
 * reader.
 *
 * Calling convention
 * ------------------
 * `finish()` returns an UNROOTED pointer, like every other protoCore-facing
 * function here (principle P1): the caller must store it into its own slot,
 * or return it, before doing anything that allocates. The builder anchors it
 * in the parent context's young generation on the way out, so it survives a
 * collection that starts between the return and the caller's store.
 *
 * Reference: docs/archive/design-specs/2026-06-14-engineering-principles.md
 * (P1, P2) and `docs/DESIGN.md` § "Building large collections in native code".
 */
#pragma once

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoList;
}

namespace protoClojure {

class ListBuilder {
public:
    /**
     * Opens a build under `parent`. Allocates no protoCore cell until the
     * first chunk is flushed.
     */
    explicit ListBuilder(proto::ProtoContext* parent);
    ~ListBuilder();

    ListBuilder(const ListBuilder&) = delete;
    ListBuilder& operator=(const ListBuilder&) = delete;

    /**
     * Appends one element. `value` is parked in a GC-visible slot before any
     * allocation happens, so it is safe to pass a freshly returned, still
     * unrooted pointer straight from the call that produced it.
     */
    void push(const proto::ProtoObject* value);

    /** Number of elements pushed so far. */
    unsigned long size() const { return size_; }

    /**
     * The builder's own context — the innermost one on this thread while the
     * builder is open. Pass it to every nested call made inside the build
     * loop; see "Using it" above. Stable for the builder's whole lifetime.
     */
    proto::ProtoContext* context() { return holder_; }

    /**
     * The finished list as a `ProtoObject*`. UNROOTED — see the calling
     * convention above. Calling `push()` afterwards is valid and resumes the
     * build from the returned value.
     */
    const proto::ProtoObject* finish();

    /** `finish()` typed as a `ProtoList*`. Same rooting contract. */
    const proto::ProtoList* finishList();

private:
    /**
     * The parking area starts small — the overwhelming majority of builds in
     * a Clojure program are a handful of elements and must not pay for a
     * wide slot array — and widens on demand up to kMaxPending.
     *
     * 256 elements per chunk keeps a chunk's garbage (256 * log2(N) cells,
     * ~4.4k cells at N = 70000) below protoCore's 10000-cell per-context
     * submission threshold, and matches the chunk size
     * `ExecutionEngine::foldKeywordPairs` already uses for keyword pairs.
     */
    static constexpr unsigned int kInitialPending = 8;
    static constexpr unsigned int kMaxPending     = 256;
    static constexpr unsigned int kSlotAcc        = 0;
    static constexpr unsigned int kSlotPending0   = 1;

    /** Appends every parked element inside a fresh, short-lived context. */
    void flush();

    // Held by pointer so this header does not need protoCore.h. Constructed
    // in the .cpp; destroyed by ~ListBuilder.
    proto::ProtoContext* holder_;
    unsigned int  capacity_ = kInitialPending;
    unsigned int  pending_  = 0;
    unsigned long size_     = 0;
};

} // namespace protoClojure
