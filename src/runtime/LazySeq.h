/*
 * LazySeq — lazy sequences, cons cells, chunks, and the seq protocol every
 * sequence function reads through (docs/DESIGN.md §10.2, M28-M31).
 *
 * Values (SeqTypes.h):
 *   - a lazy seq is a protoCore lazy object: an immutable object whose lazy
 *     slot holds a thunk (its producer) and, once realised, the seq the thunk
 *     produced. The thunk is a Clojure fn of no arguments (`lazy-seq`'s body)
 *     or a native step (an immutable object naming a kind and its operands:
 *     the lazy map, filter, range, ... below);
 *   - a cons holds a first element and `more`, any seq or nil;
 *   - a chunk is the elements [off, end) of a ProtoList followed by `more`;
 *   - a list is a protoCore ProtoLinkedList (SeqTypes.h, "Clojure lists").
 *
 * The seq protocol, as clojure.lang.RT and ISeq:
 *   seqOf(v)      nil, or a non-empty seq: a list, a cons or a chunk.
 *                 A lazy seq is realised (and the seq it holds answered); the
 *                 empty list answers nil; a vector answers a chunked chunk
 *                 over its elements; a map, a set and a string answer an
 *                 unchunked chunk over their entries, elements and
 *                 characters.
 *   seqFirst(s)   the first element of a non-empty seq.
 *   seqMore(s)    what follows it, never nil: `()` at the end (Clojure's
 *                 `rest`).
 *   seqNext(s)    seqOf(seqMore(s)) (Clojure's `next`).
 *
 * Realisation is thread-safe without a lock: the realising thread claims the
 * slot by compare-and-set, runs the thunk, and publishes the seq with a
 * second compare-and-set; a thread that finds a seq claimed waits,
 * parked for the collector, until it is published. So a thunk runs once, as
 * in Clojure, whatever the number of threads reading the seq. A thunk that
 * raises leaves the lazy seq unrealised (the next read runs it again, as in
 * Clojure). Native steps that run no Clojure code skip the claim: two
 * threads may compute the same value, and the first compare-and-set wins.
 * Once realised, the state no longer references the thunk, so what the
 * thunk captured (the source of a map, the head of a stream) is released.
 *
 * GC: inputs are rooted by the caller; results are unrooted, except that the
 * seq a lazy seq realised to is reachable from the lazy seq.
 */
#pragma once

#include "SeqTypes.h"

#include "protoCore.h"

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoList;
class ProtoArguments;
}

namespace protoClojure {

class CallerSlots;

// Interns the attribute keys and installs the hook the map helpers use
// (SeqTypes.h). Called by installPrimitives after initSeqTypes.
void initLazySeqs(proto::ProtoContext* ctx);

// ---- Construction (unrooted results; operands rooted by the caller) -------

// A lazy seq whose thunk is `thunk`: a Clojure fn of no arguments or a step.
const proto::ProtoObject* newLazySeq(proto::ProtoContext* ctx, const proto::ProtoObject* thunk);
const proto::ProtoObject* newCons(proto::ProtoContext* ctx, const proto::ProtoObject* first,
                                  const proto::ProtoObject* more);
// The elements [off, end) of `items`, then `more` (nil for none). A chunk
// marked `chunked` is a Clojure chunked seq (map, filter and for process it
// 32 elements at a time).
const proto::ProtoObject* newChunk(proto::ProtoContext* ctx, const proto::ProtoList* items,
                                   long long off, long long end,
                                   const proto::ProtoObject* more, bool chunked);

// The native steps. A step realises to a seq (nil, a list, a cons or a
// chunk) and may build further lazy seqs of its own kind.
enum class StepKind : int {
    Map1, MapN, Filter, Take, TakeWhile, Drop, DropWhile, Iterate, IterateNext,
    Repeat, Cycle, Range, RangeInf, Concat, Partition, Interleave, For, Repeatedly,
    Distinct, Line, ReSeq,
    Count_
};
// A lazy seq over the step `kind` with operands a, b, c, d (nullptr for an
// unused one).
const proto::ProtoObject* lazyStep(proto::ProtoContext* ctx, StepKind kind,
                                   const proto::ProtoObject* a,
                                   const proto::ProtoObject* b = nullptr,
                                   const proto::ProtoObject* c = nullptr,
                                   const proto::ProtoObject* d = nullptr);

// lazyStep with a fifth operand.
const proto::ProtoObject* lazyStep5(proto::ProtoContext* ctx, StepKind kind,
                                    const proto::ProtoObject* a, const proto::ProtoObject* b,
                                    const proto::ProtoObject* c, const proto::ProtoObject* d,
                                    const proto::ProtoObject* e);

// How a step kind realises. `self` is the lazy seq being realised (a step
// may build a seq that points back to it, as an endless repeat does), and
// the operands are the step's. Registered by the module that owns the kind
// (IO.cpp: Line; Primitives.cpp: ReSeq); `pure` steps run no Clojure code.
struct StepOperands {
    const proto::ProtoObject* a;
    const proto::ProtoObject* b;
    const proto::ProtoObject* c;
    const proto::ProtoObject* d;
    const proto::ProtoObject* e;
};
using StepRealizer = const proto::ProtoObject* (*)(proto::ProtoContext* ctx,
                                                   const proto::ProtoObject* self,
                                                   const StepOperands& ops);
void registerStep(StepKind kind, StepRealizer fn, bool pure);

// ---- The seq protocol ---------------------------------------------------------

// RT.seq: nil, or a non-empty list, cons or chunk (see above). Raises
// IllegalArgumentException "Don't know how to create ISeq from: ..." for a
// value that is not seqable.
const proto::ProtoObject* seqOf(proto::ProtoContext* ctx, const proto::ProtoObject* v);
const proto::ProtoObject* seqFirst(proto::ProtoContext* ctx, const proto::ProtoObject* s);
const proto::ProtoObject* seqMore(proto::ProtoContext* ctx, const proto::ProtoObject* s);
inline const proto::ProtoObject* seqNext(proto::ProtoContext* ctx, const proto::ProtoObject* s) {
    return seqOf(ctx, seqMore(ctx, s));
}

// ---- Direct reads -------------------------------------------------------------
//
// first, rest, next, seq, empty? and cons of the values that are read without
// running Clojure code: nil, a list, a vector, a cons and a chunk. Each
// answers false, touching nothing, for any other value (a lazy seq, which
// may run code to realise; a map, a set, a string, or a value that is not
// seqable), which the caller then reads through the general path above.
// The answers are those of the general path. rest, next and seq may
// allocate (a chunk, a list one shorter, an empty list) in `ctx`; results
// are unrooted. The primitives and the VM's direct primitive calls share
// them (ExecutionEngine.cpp, fastPrimitiveCall).
bool tryFirst(proto::ProtoContext* ctx, const proto::ProtoObject* v, const proto::ProtoObject** out);
bool tryRest(proto::ProtoContext* ctx, const proto::ProtoObject* v, const proto::ProtoObject** out);
bool tryNext(proto::ProtoContext* ctx, const proto::ProtoObject* v, const proto::ProtoObject** out);
bool trySeq(proto::ProtoContext* ctx, const proto::ProtoObject* v, const proto::ProtoObject** out);
bool tryEmpty(proto::ProtoContext* ctx, const proto::ProtoObject* v, bool* out);
// (count v) of nil, a vector, a list, and a chain of cons cells and chunks
// that ends in nil, a vector or a list (the count of a list or a vector is
// O(1), the chain is walked, as Clojure's RT.count walks to a Counted); false,
// with nothing realised, when the chain reaches a lazy seq.
bool tryCount(proto::ProtoContext* ctx, const proto::ProtoObject* v, long long* out);
// (cons x coll) onto nil (a one-element list) or a seq object (a cons cell,
// which realises nothing), as Clojure's RT.cons; false for any other coll.
bool tryCons(proto::ProtoContext* ctx, const proto::ProtoObject* x,
             const proto::ProtoObject* coll, const proto::ProtoObject** out);

// `seq?`: a list (empty or not), a lazy seq, a cons or a chunk.
bool isSeqValue(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// The seq a lazy seq realises to, realising it on the first call. The
// answer is reachable from `ls`.
const proto::ProtoObject* realizeLazy(proto::ProtoContext* ctx, const proto::ProtoObject* ls);
// `realized?` of a lazy seq: its thunk has run.
bool lazyIsRealized(proto::ProtoContext* ctx, const proto::ProtoObject* ls);
// A chunked seq: a chunk marked chunked (`chunked-seq?`).
bool isChunkedSeq(proto::ProtoContext* ctx, const proto::ProtoObject* s);

// The elements of a seqable as a list, realising every lazy part: nullptr
// for nil or an empty collection. A list is itself, a vector its elements.
// What every function that needs the whole collection reads (vec, sort,
// apply, ...). Raises like seqOf for a value that is not seqable.
const proto::ProtoList* seqToList(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// Provided by Primitives.cpp: the entries of a map, the elements of a set
// and the characters of a string as a list (nullptr when there are none),
// for seqOf. Answers false for any other value.
bool collectionItems(proto::ProtoContext* ctx, const proto::ProtoObject* v,
                     const proto::ProtoList** out);

// ---- Walking ----------------------------------------------------------------

// A cursor over the elements of a seqable that holds nothing but its
// position: the current seq node, in slot `slot` of `roots` (CallerSlots.h),
// which the caller keeps alive for the walk. So a walk over a lazy seq that
// no one else holds lets every element it has passed be collected, which is
// what keeps `(count (line-seq rdr))` in constant memory. Lists and chunks
// are read by index; moving to the next node realises it in `ctx`, the
// caller's context; the walker hands that garbage to the collector by
// ticking its CallerSlots (under a SafepointPermit), never the walk itself.
class SeqWalk {
public:
    SeqWalk(CallerSlots& roots, unsigned slot, proto::ProtoContext* ctx,
            const proto::ProtoObject* coll);
    bool done() const { return node_ == nullptr; }
    // The current element; reachable from the current node until advance.
    const proto::ProtoObject* first(proto::ProtoContext* ctx) const;
    void advance(proto::ProtoContext* ctx);
    // True, with the count in `*n`, when the number of elements from the
    // current position to the end is known without walking: the current
    // node is a list (its count) or a chunk with nothing after it.
    bool counted(proto::ProtoContext* ctx, long long* n) const;
    // What remains, as a seq (nil at the end): the rest of the walked
    // collection from the current position. Allocates for a list or a chunk
    // read part-way; the result is unrooted.
    const proto::ProtoObject* remaining(proto::ProtoContext* ctx) const;

private:
    void load(proto::ProtoContext* ctx, const proto::ProtoObject* node);

    // What the current node is: a chunk (read by index in items_), a cons
    // (fields_) or a list (its head cell read by index).
    enum class Mode { Chunk, Cons, List };

    CallerSlots&              roots_;
    unsigned                  slot_;
    const proto::ProtoObject* node_ = nullptr;  // also in roots_'s slot
    const proto::ProtoList*   items_ = nullptr; // a chunk's elements
    long long                 i_ = 0, end_ = 0; // position in a chunk or a list's head
    Mode                      mode_ = Mode::Chunk;
    const proto::ProtoList*   fields_ = nullptr; // a cons's fields, [first more ...]
};

// Installs the lazy-sequence functions (map, filter, take, range, ...) on
// the globals, as installPrimitives does for the others.
void installLazySeqPrimitives(proto::ProtoContext* ctx, proto::ProtoObject* globals);
// The name of one of those primitives, for printing a fn; nullptr otherwise.
const char* lazySeqPrimitiveName(proto::ProtoMethod fn);

// Clears argument `i` of `args` after its value has been rooted elsewhere: a
// function that walks its sequence argument to the end does not keep the
// head of that sequence alive through the argument slot (docs/DESIGN.md,
// M30). The descriptor's array is the caller's call slots, which no caller
// reads after the call.
void releaseArg(const proto::ProtoArguments* args, unsigned i);

} // namespace protoClojure
