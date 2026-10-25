/*
 * SeqTypes — the representation of protoClojure's sequence objects: lazy
 * seqs, cons cells and chunks (docs/DESIGN.md §10.2, M28).
 *
 * Vectors stay protoCore's own structure (a boxed ProtoList, VectorOps.h).
 * Four object types are added beside them, each an object cell whose
 * prototype tells it apart, as a set is (SetOps.h), and one value:
 *
 *   - a list cell: a non-empty Clojure list, holding its first element, its
 *     rest and its count (see "Clojure lists" below); the empty list `()` is
 *     a value of its own;
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
 * globals).
 */
#pragma once

#include "protoCore.h"  // proto::proto_ulong

namespace proto {
class ProtoContext;
class ProtoObject;
class ProtoList;
}

namespace protoClojure {

struct SeqPrototypes {
    const proto::ProtoObject* lazy  = nullptr;  // lazy seqs (lazy children)
    const proto::ProtoObject* cons  = nullptr;  // cons cells
    const proto::ProtoObject* chunk = nullptr;  // chunks
    const proto::ProtoObject* list  = nullptr;  // non-empty lists (list cells)
    const proto::ProtoObject* empty = nullptr;  // THE empty list `()` itself
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
// object cell, one prototype read.
SeqObject seqObjectKind(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// True when `v` is a lazy seq, a cons, a chunk or a list (empty or not).
inline bool isSeqObject(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return seqObjectKind(ctx, v) != SeqObject::None;
}

// ---- Clojure lists ------------------------------------------------------------
//
// A Clojure list (`list?`, PersistentList) is a chain of immutable list
// cells ending in the empty list. A cell holds its first element, the rest
// (a list: another cell or the empty list) and its count, so first, rest,
// count and cons onto a list are O(1) and allocate nothing but the new cell,
// as in Clojure; nth is O(n). A cell is a protoCore lazy object of prototype
// `g_seqProtos.list` published at birth with the list [first rest count],
// the layout of a cons (LazySeq.cpp). The empty list is one value per
// ProtoSpace, `g_seqProtos.empty`, told apart by identity.
//
// The reader yields these lists, the compiler and the macros read them, and
// every primitive that answers a list answers one (docs/DESIGN.md, M44). A
// protoCore ProtoList is never a Clojure value: it is the element storage of
// a vector (VectorOps.h) and an internal container.
//
// GC: operands are rooted by the caller; results are unrooted. A cell
// references its rest, so a chain is collected from its head as the head
// becomes unreachable.

// True when `v` is a Clojure list, empty or not.
bool isListValue(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// The empty list `()`.
inline const proto::ProtoObject* emptyListValue() { return g_seqProtos.empty; }

// The list `x` followed by the elements of the list `rest`.
const proto::ProtoObject* listCons(proto::ProtoContext* ctx, const proto::ProtoObject* x,
                                   const proto::ProtoObject* rest);
// The first element, the rest and the count of a NON-EMPTY list.
const proto::ProtoObject* listFirst(proto::ProtoContext* ctx, const proto::ProtoObject* cell);
const proto::ProtoObject* listRest(proto::ProtoContext* ctx, const proto::ProtoObject* cell);
// The first element and the rest of a NON-EMPTY list, read together (one
// read of the cell's fields).
void listSplit(proto::ProtoContext* ctx, const proto::ProtoObject* cell,
               const proto::ProtoObject** first, const proto::ProtoObject** rest);
// The number of elements of a list (0 for the empty list). O(1).
long long listCount(proto::ProtoContext* ctx, const proto::ProtoObject* l);

// The list of the `n` values at `items`, in that order; the values must be
// rooted by the caller (a frame's slots, a rooted ProtoList's elements).
const proto::ProtoObject* listFromArray(proto::ProtoContext* ctx, unsigned long n,
                                        const proto::ProtoObject* const* items);
// The list of the elements of `items` (nullptr: the empty list), which the
// caller roots.
const proto::ProtoObject* listFromProtoList(proto::ProtoContext* ctx, const proto::ProtoList* items);
// The elements of the list `l` as a ProtoList, for reading by index.
const proto::ProtoList* listToProtoList(proto::ProtoContext* ctx, const proto::ProtoObject* l);

// The fields of a seq node (a cons, a chunk or a list cell): the list it was
// published with. For the seq modules only.
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
