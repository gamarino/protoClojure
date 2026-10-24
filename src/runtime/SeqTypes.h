/*
 * SeqTypes — the representation of protoClojure's sequence objects: lazy
 * seqs, cons cells and chunks (docs/DESIGN.md §10.2, M28).
 *
 * Lists and vectors stay protoCore's own structures (a ProtoList, a vector's
 * boxed list, VectorOps.h). Three object types are added beside them, each
 * an object cell whose prototype tells it apart, as a set is (SetOps.h):
 *
 *   - a lazy seq: a protoCore lazy object (ProtoObject::newLazyChild,
 *     protoCore 3.1), an immutable object whose lazy slot holds the thunk
 *     that computes it and, once realised by compare-and-set, the seq it
 *     computed (LazySeq.h). Not a mutable object: a mutable's state is a
 *     collector root until its handle is swept, so a realised chain of
 *     mutable lazy seqs, each pointing at the next, would be freed one link
 *     per collection cycle;
 *   - a cons: an immutable object holding `first` and `more` (any seq or
 *     nil), what `(cons x lazy)` builds and what most realised lazy seqs are;
 *   - a chunk: an immutable view of the elements [off, end) of a ProtoList,
 *     followed by `more`. A chunk marked "chunked" is Clojure's chunked seq
 *     (the seq of a vector or of a finite range): map, filter and for process
 *     its elements together, 32 at a time, as Clojure does. An unmarked
 *     chunk is a plain stretch of a list a step already holds (concat of a
 *     list), walked one element at a time.
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
};

// The prototypes of the current ProtoSpace. Written by initSeqTypes before
// any Clojure code runs; read-only afterwards.
extern SeqPrototypes g_seqProtos;

// Creates the prototypes and answers a list holding them, which the caller
// roots. Calling it again (a new ProtoSpace) replaces the previous ones.
const proto::ProtoObject* initSeqTypes(proto::ProtoContext* ctx);

enum class SeqObject { None, Lazy, Cons, Chunk };

// Which sequence object `v` is, if any: one pointer-tag test and, for an
// object cell, one prototype read.
SeqObject seqObjectKind(proto::ProtoContext* ctx, const proto::ProtoObject* v);

// True when `v` is a lazy seq, a cons or a chunk.
inline bool isSeqObject(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    return seqObjectKind(ctx, v) != SeqObject::None;
}

// The elements of the seq object `v` as a list, realising it to its end;
// nullptr when it is empty. Installed by the runtime library
// (LazySeq.cpp); the result is unrooted.
using SeqToListFn = const proto::ProtoList* (*)(proto::ProtoContext* ctx,
                                                const proto::ProtoObject* v);
void setSeqToList(SeqToListFn fn);
const proto::ProtoList* seqObjectToList(proto::ProtoContext* ctx, const proto::ProtoObject* v);

} // namespace protoClojure
