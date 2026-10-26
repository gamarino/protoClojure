/*
 * SeqTypes — the representation of protoClojure's sequence objects: lists,
 * lazy seqs, cons cells and chunks (docs/DESIGN.md §10.2, M28, M47).
 *
 * Vectors stay protoCore's own structure (a boxed ProtoList, VectorOps.h),
 * and so do lists: a Clojure list is a protoCore ProtoLinkedList (see
 * "Clojure lists" below), the empty list `()` included. Three object types
 * are added beside them, each an object cell whose prototype tells it apart,
 * as a set is (SetOps.h):
 *
 *   - a lazy seq: a protoCore lazy object (ProtoObject::newLazyChild,
 *     protoCore 3.1), an immutable object whose lazy slot holds the thunk
 *     that computes it and, once realised by compare-and-set, the seq it
 *     computed (LazySeq.h). Not a mutable object: a mutable's state is a
 *     collector root until its handle is swept, so a realised chain of
 *     mutable lazy seqs, each pointing at the next, would be freed one link
 *     per collection cycle;
 *   - a cons: an immutable object holding `first` and `more` (any seq or
 *     nil), what `(cons x coll)` builds and what most realised lazy seqs are;
 *   - a chunk: an immutable view of the elements [off, end) of a ProtoList,
 *     followed by `more`. A chunk marked "chunked" is Clojure's chunked seq
 *     (the seq of a vector or of a finite range): map, filter and for process
 *     its elements together, 32 at a time, as Clojure does. An unmarked
 *     chunk is a plain stretch of a ProtoList a step already holds (the seq
 *     of a map, a set or a string), walked one element at a time.
 *
 * This header lives in the support library, beside the map and set helpers,
 * because map keys and set elements are matched by value and a seq is a
 * sequential value (MapOps.cpp): those helpers only need to recognise a seq
 * object and to read it as a list, which the runtime library provides
 * through a hook (setSeqToList), since realising a lazy seq may run Clojure
 * code.
 *
 * GC: the prototypes are created once per ProtoSpace by initSeqTypes and
 * must be rooted by its caller (installPrimitives stores them on the
 * globals). The empty list is protoCore's, perennial.
 */
#pragma once

#include "protoCore.h"  // proto::proto_ulong

#include <cstddef>

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoList;
class ProtoLinkedList;
}

namespace protoClojure {

struct SeqPrototypes {
    const proto::ProtoObject* lazy  = nullptr;  // lazy seqs (lazy children)
    const proto::ProtoObject* cons  = nullptr;  // cons cells
    const proto::ProtoObject* chunk = nullptr;  // chunks
    const proto::ProtoObject* empty = nullptr;  // THE empty list `()` (not a prototype)
};

// The prototypes of the current ProtoSpace. Written by initSeqTypes before
// any Clojure code runs; read-only afterwards.
extern SeqPrototypes g_seqProtos;

// Creates the prototypes and answers a list holding them, which the caller
// roots. Calling it again (a new ProtoSpace) replaces the previous ones.
const proto::ProtoObject* initSeqTypes(proto::ProtoContext* ctx);

// List and Empty are Clojure lists (PersistentList and its EmptyList); the
// others are the seq objects above.
enum class SeqObject { None, Lazy, Cons, Chunk, List, Empty };

// Which sequence object `v` is, if any: one pointer-tag test and, for an
// object cell, one prototype read; a list is told by its tag alone.
SeqObject seqObjectKind(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// True when `v` is a lazy seq, a cons, a chunk or a list (empty or not).
inline bool isSeqObject(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return seqObjectKind(ctx, v) != SeqObject::None;
}

// ---- Clojure lists ------------------------------------------------------------
//
// A Clojure list (`list?`, PersistentList) is a protoCore ProtoLinkedList
// (protoCore 3.3.0): an immutable chain of 64-byte cells of up to four
// elements, every cell full except the head, each holding the count of its
// suffix. The handle is a tagged word of its own (pointer tag 29), so a list
// is recognised by its tag alone, with no prototype read. The empty list
// `()` is protoCore's one empty list of the process, told apart by identity.
//
// Costs: first and count read the head cell; conj onto a list allocates one
// cell; rest allocates nothing when the head holds one element and one cell
// otherwise (three steps in four); nth walks n / 4 cells. A list of n
// elements takes ceil(n / 4) cells. Walks that do not hand the rest list to
// Clojure code go through ListCursor or listNth and allocate nothing.
//
// The reader yields these lists, the compiler and the macros read them, and
// every primitive that answers a list answers one (docs/DESIGN.md, M44, M47).
// A protoCore ProtoList is never a Clojure value: it is the element storage
// of a vector (VectorOps.h) and an internal container.
//
// GC: operands are rooted by the caller; results are unrooted. A cell
// references the next, so a list is collected from its head as the head
// becomes unreachable.

// protoCore's pointer tag for a ProtoLinkedList (headers/proto_internal.h,
// POINTER_TAG_LINKED_LIST).
constexpr proto::proto_ulong kTagLinkedList = 29;

// True when `v` is a Clojure list, empty or not: its pointer tag.
inline bool isListValue(proto::ProtoContext*, const proto::ProtoObject* v) {
    return (reinterpret_cast<proto::proto_ulong>(v) & 0x3F) == kTagLinkedList;
}

// The list behind a value isListValue accepted: the handle is the word.
inline const proto::ProtoLinkedList* asLinkedList(const proto::ProtoObject* l) {
    return reinterpret_cast<const proto::ProtoLinkedList*>(l);
}

// The empty list `()`.
inline const proto::ProtoObject* emptyListValue() { return g_seqProtos.empty; }

// The list `x` followed by the elements of the list `rest`.
const proto::ProtoObject* listCons(proto::ProtoContext* ctx, const proto::ProtoObject* x,
                                   const proto::ProtoObject* rest);
// The first element, the rest and the count of a NON-EMPTY list. listRest
// allocates one cell unless the head holds a single element.
const proto::ProtoObject* listFirst(proto::ProtoContext* ctx, const proto::ProtoObject* cell);
const proto::ProtoObject* listRest(proto::ProtoContext* ctx, const proto::ProtoObject* cell);
// The first element and the rest of a NON-EMPTY list, read together.
void listSplit(proto::ProtoContext* ctx, const proto::ProtoObject* cell,
               const proto::ProtoObject** first, const proto::ProtoObject** rest);
// The number of elements of a list (0 for the empty list). O(1).
long long listCount(proto::ProtoContext* ctx, const proto::ProtoObject* l);
// The element at `index` of a list, 0 <= index < count; walks index / 4
// cells and allocates nothing.
const proto::ProtoObject* listNth(proto::ProtoContext* ctx, const proto::ProtoObject* l,
                                  long long index);

// The list `l` without its first `count` elements, 0 < count <= count of
// `l`. Allocates nothing when `count` is the number of elements of the head
// cell (listHeadCount), and one cell otherwise.
const proto::ProtoObject* listDrop(proto::ProtoContext* ctx, const proto::ProtoObject* l,
                                   long long count);
// The number of elements in the head cell of a non-empty list: every cell of
// a ProtoLinkedList but the head holds four elements, so the head holds
// ((count - 1) mod 4) + 1. Only a cost depends on it: dropping any other
// number of elements allocates one cell.
inline long long listHeadCount(proto::ProtoContext* ctx, const proto::ProtoObject* l) {
    return (listCount(ctx, l) - 1) % 4 + 1;
}

// The list of the `n` values at `items`, in that order; the values must be
// rooted by the caller (a frame's slots, a rooted ProtoList's elements).
const proto::ProtoObject* listFromArray(proto::ProtoContext* ctx, std::size_t n,
                                        const proto::ProtoObject* const* items);
// The list of the elements of `items` (nullptr: the empty list), which the
// caller roots.
const proto::ProtoObject* listFromProtoList(proto::ProtoContext* ctx, const proto::ProtoList* items);
// The elements of the list `l` as a ProtoList, for reading by index.
const proto::ProtoList* listToProtoList(proto::ProtoContext* ctx, const proto::ProtoObject* l);

// Walks the elements of a list in order without allocating. A C++ value in
// a local; the list must stay reachable (rooted by the caller) for the walk.
class ListCursor {
public:
    explicit ListCursor(const proto::ProtoObject* l) : c_(asLinkedList(l)) {}
    bool hasNext() const { return c_.hasNext(); }
    const proto::ProtoObject* next() {
        const proto::ProtoObject* x = c_.next();
        return x ? x : PROTO_NONE;
    }

private:
    proto::ProtoLinkedListCursor c_;
};

// The fields of a seq node (a cons or a chunk): the list it was published
// with. For the seq modules only.
const proto::ProtoList* seqNodeFields(proto::ProtoContext* ctx, const proto::ProtoObject* node);
// A node of prototype `proto` holding the `n` values at `fields`.
const proto::ProtoObject* newSeqNode(proto::ProtoContext* ctx, const proto::ProtoObject* proto,
                                     unsigned n, const proto::ProtoObject* const* fields);

// The elements of the seq object `v` as a list, realising it to its end;
// nullptr when it is empty. Installed by the runtime library
// (LazySeq.cpp); the result is unrooted.
using SeqToListFn = const proto::ProtoList* (*)(proto::ProtoContext* ctx,
                                                const proto::ProtoObject* v);
void setSeqToList(SeqToListFn fn);
const proto::ProtoList* seqObjectToList(proto::ProtoContext* ctx, const proto::ProtoObject* v);

} // namespace protoClojure
